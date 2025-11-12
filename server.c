// server.c — Unified V1+V2+V3 server
// - V1: handles SIGUSR1/SIGUSR2 to print a random pair (demo mode)
// - V2: writer thread scans dictionary files -> queues lines -> reader thread fills shared memory
// - V3: request handler handles client requests; on MISS it rescans immediately, then rechecks

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <limits.h>

#include "proto.h"

// ---------- Globals ----------
static SharedDictionary *g_dict = NULL;
static int g_word_q  = -1;   // queue for file->word pairs
static int g_req_q   = -1;   // queue for client requests
static int g_resp_q  = -1;   // queue for server replies

// Track scanned files & mtimes to avoid resending unchanged content
typedef struct { char filename[PATH_MAX]; time_t mtime; } FileInfo;
static FileInfo g_tracked[512];
static int g_tracked_count = 0;

static volatile sig_atomic_t g_want_random_enfr = 0;
static volatile sig_atomic_t g_want_random_fren = 0;

// ---------- PID file helpers ----------
static int write_pid_file_atomic(const char *path, pid_t pid) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", pid);
    fflush(f);
    int fd = fileno(f);
    if (fd != -1) fsync(fd);
    fclose(f);
    if (rename(tmp, path) == -1) { remove(tmp); return -1; }
    return 0;
}

static void remove_pid_file(void) {
    unlink(PID_FILE);
}

static void on_sigint_term(int sig) {
    (void)sig;
    remove_pid_file();
    // release IPC resources that we own
    // NOTE: If you want to fully remove shared memory and queues on exit, uncomment:
    // shmctl(shmget(SHM_KEY, 0, 0), IPC_RMID, NULL);
    // msgctl(g_word_q, IPC_RMID, NULL);
    // msgctl(g_req_q,  IPC_RMID, NULL);
    // msgctl(g_resp_q, IPC_RMID, NULL);
    _exit(0);
}

// ---------- Signal handlers (V1 demo) ----------
static void h_usr1(int s) { (void)s; g_want_random_enfr = 1; }
static void h_usr2(int s) { (void)s; g_want_random_fren = 1; }

static void maybe_print_random(void) {
    int dir = 0; // 1 EN->FR, 2 FR->EN
    if (g_want_random_enfr) { dir = 1; g_want_random_enfr = 0; }
    else if (g_want_random_fren) { dir = 2; g_want_random_fren = 0; }
    if (!dir) return;

    pthread_mutex_lock(&g_dict->mutex);
    if (g_dict->size > 0) {
        int idx = rand() % g_dict->size;
        if (dir == 1)
            printf("[RANDOM EN→FR] %s -> %s\n", g_dict->words[idx].english, g_dict->words[idx].french);
        else
            printf("[RANDOM FR→EN] %s -> %s\n", g_dict->words[idx].french, g_dict->words[idx].english);
        fflush(stdout);
    }
    pthread_mutex_unlock(&g_dict->mutex);
}

// ---------- Tracked files ----------
static int tracked_find(const char *name) {
    for (int i = 0; i < g_tracked_count; ++i)
        if (strcmp(g_tracked[i].filename, name) == 0) return i;
    return -1;
}

static int is_new_or_modified(const char *name, time_t mtime) {
    int i = tracked_find(name);
    if (i < 0) return 1;            // new file
    return mtime > g_tracked[i].mtime; // modified
}

static void tracked_update(const char *name, time_t mtime) {
    int i = tracked_find(name);
    if (i >= 0) { g_tracked[i].mtime = mtime; return; }
    if (g_tracked_count < (int)(sizeof(g_tracked)/sizeof(g_tracked[0]))) {
        strncpy(g_tracked[g_tracked_count].filename, name, PATH_MAX-1);
        g_tracked[g_tracked_count].filename[PATH_MAX-1] = '\0';
        g_tracked[g_tracked_count].mtime = mtime;
        g_tracked_count++;
    }
}

// ---------- Dictionary pipeline helpers ----------
static void send_line_to_queue(const char *eng, const char *fr, long dir_mtype) {
    MsgWord m; memset(&m, 0, sizeof(m));
    m.mtype = dir_mtype; // 1 EN->FR, 2 FR->EN
    strncpy(m.english, eng, MAX_WORD_LENGTH-1);
    strncpy(m.french,  fr, MAX_WORD_LENGTH-1);
    if (msgsnd(g_word_q, &m, sizeof(MsgWord) - sizeof(long), 0) == -1) {
        perror("msgsnd(word)");
    }
}

static void read_word_pairs_from_file(const char *filepath, long *out_dir_mtype) {
    // Detect direction from the very first line (optional): "# direction: EN_FR" or "FR_EN"
    FILE *f = fopen(filepath, "r");
    if (!f) { perror("fopen"); return; }

    long dir_mtype = 1; // default EN->FR
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        if (strstr(line, "FR_EN")) dir_mtype = 2;
        else if (strstr(line, "EN_FR")) dir_mtype = 1;
        else {
            // Not a direction header, rewind to treat as data line
            fseek(f, 0, SEEK_SET);
        }
    }

    while (fgets(line, sizeof(line), f)) {
        // Expected format per line: english;french\n
        // Trim trailing newline
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';

        char *semi = strchr(line, ';');
        if (!semi) continue; // skip bad line
        *semi = '\0';

        const char *eng = line;
        const char *fr  = semi + 1;
        if (*eng == '\0' || *fr == '\0') continue;
        send_line_to_queue(eng, fr, dir_mtype);
    }

    fclose(f);
    if (out_dir_mtype) *out_dir_mtype = dir_mtype;
}

static void rescan_dictionary_once(void) {
    DIR *dir = opendir(DICTIONARY_DIR);
    if (!dir) {
        perror("opendir(dictionary)");
        return;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", DICTIONARY_DIR, de->d_name);

        struct stat st;
        if (stat(path, &st) == -1) continue;
        if (!S_ISREG(st.st_mode)) continue;

        if (!is_new_or_modified(de->d_name, st.st_mtime)) continue;

        long dir_mtype = 1;
        read_word_pairs_from_file(path, &dir_mtype);
        tracked_update(de->d_name, st.st_mtime);
    }

    closedir(dir);
}

// ---------- Threads ----------
static void *writer_thread(void *arg) {
    (void)arg;
    for (;;) {
        rescan_dictionary_once();
        sleep(5); // periodic scan
    }
    return NULL;
}

static void *reader_thread(void *arg) {
    (void)arg;
    MsgWord m;
    for (;;) {
        ssize_t n = msgrcv(g_word_q, &m, sizeof(MsgWord) - sizeof(long), 0, 0);
        if (n == -1) { perror("msgrcv(word)"); continue; }

        pthread_mutex_lock(&g_dict->mutex);
        if (g_dict->size < MAX_WORDS) {
            if (m.mtype == 1) { // EN->FR as given
                strncpy(g_dict->words[g_dict->size].english, m.english, MAX_WORD_LENGTH-1);
                strncpy(g_dict->words[g_dict->size].french,  m.french,  MAX_WORD_LENGTH-1);
            } else {            // FR->EN, normalize to english/french field order
                strncpy(g_dict->words[g_dict->size].english, m.french,  MAX_WORD_LENGTH-1);
                strncpy(g_dict->words[g_dict->size].french,  m.english, MAX_WORD_LENGTH-1);
            }
            g_dict->size++;
        } else {
            // Dictionary is full; you can choose to overwrite or drop
            // Here we simply drop and warn once in a while
            if ((g_dict->size % 100) == 0)
                fprintf(stderr, "[WARN] dictionary full, skipping pairs...\n");
        }
        pthread_mutex_unlock(&g_dict->mutex);
    }
    return NULL;
}

static int lookup(int dir /*1 EN->FR, 2 FR->EN*/, const char *w, char out[MAX_WORD_LENGTH]) {
    int found = 0;
    pthread_mutex_lock(&g_dict->mutex);
    for (int i = 0; i < g_dict->size; ++i) {
        if (dir == 1) {
            if (strcmp(g_dict->words[i].english, w) == 0) { strncpy(out, g_dict->words[i].french, MAX_WORD_LENGTH-1); found = 1; break; }
        } else {
            if (strcmp(g_dict->words[i].french,  w) == 0) { strncpy(out, g_dict->words[i].english, MAX_WORD_LENGTH-1); found = 1; break; }
        }
    }
    pthread_mutex_unlock(&g_dict->mutex);
    return found;
}

static void *request_handler_thread(void *arg) {
    (void)arg;
    MsgReq r; MsgResp s;
    for (;;) {
        ssize_t n = msgrcv(g_req_q, &r, sizeof(MsgReq) - sizeof(long), 0, 0);
        if (n == -1) { perror("msgrcv(req)"); continue; }

        memset(&s, 0, sizeof(s));
        s.mtype = r.reply_to;   // reply specifically to that client PID
        s.req_id = r.req_id;
        strncpy(s.from, r.word, MAX_WORD_LENGTH-1);

        char tmp[MAX_WORD_LENGTH] = {0};
        int dir = (int)r.mtype; // 1 EN->FR, 2 FR->EN
        if (!lookup(dir, r.word, tmp)) {
            // MISS: perform immediate rescan (V3 requirement), then recheck
            rescan_dictionary_once();
            if (lookup(dir, r.word, tmp)) {
                s.found = 1;
                strncpy(s.to, tmp, MAX_WORD_LENGTH-1);
            } else {
                s.found = 0;
            }
        } else {
            s.found = 1;
            strncpy(s.to, tmp, MAX_WORD_LENGTH-1);
        }

        if (msgsnd(g_resp_q, &s, sizeof(MsgResp) - sizeof(long), 0) == -1) {
            perror("msgsnd(resp)");
        }
    }
    return NULL;
}

// ---------- Bootstrap shared memory ----------
static SharedDictionary *attach_or_init_shm(void) {
    int shmid = shmget(SHM_KEY, sizeof(SharedDictionary), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); return NULL; }

    SharedDictionary *p = (SharedDictionary *)shmat(shmid, NULL, 0);
    if (p == (void *)-1) { perror("shmat"); return NULL; }

    // One-time init of the process-shared mutex
    if (p->initialized != 1) {
        memset(p, 0, sizeof(*p));
        pthread_mutexattr_t attr; pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&p->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        p->size = 0;
        p->initialized = 1;
    }
    return p;
}

int main(void) {
    srand((unsigned)time(NULL));

    // 1) IPC setup
    g_dict = attach_or_init_shm();
    if (!g_dict) return 1;

    g_word_q = msgget(MSG_WORD_KEY, IPC_CREAT | 0666);
    g_req_q  = msgget(MSG_REQ_KEY,  IPC_CREAT | 0666);
    g_resp_q = msgget(MSG_RESP_KEY, IPC_CREAT | 0666);
    if (g_word_q == -1 || g_req_q == -1 || g_resp_q == -1) {
        perror("msgget");
        return 1;
    }

    // 2) PID file + termination handlers
    pid_t mypid = getpid();
    if (write_pid_file_atomic(PID_FILE, mypid) != 0) {
        perror("write_pid_file");
    }
    atexit(remove_pid_file);
    struct sigaction sa_term = {0}; sa_term.sa_handler = on_sigint_term; sigaction(SIGINT, &sa_term, NULL); sigaction(SIGTERM, &sa_term, NULL);

    // 3) Random demo signals (V1) — optional
    struct sigaction sa1 = {0}, sa2 = {0};
    sa1.sa_handler = h_usr1; sigaction(SIGUSR1, &sa1, NULL);
    sa2.sa_handler = h_usr2; sigaction(SIGUSR2, &sa2, NULL);

    printf("Server PID: %d (pid file: %s)\n", mypid, PID_FILE);
    fflush(stdout);

    // 4) Threads
    pthread_t th_writer, th_reader, th_req;
    pthread_create(&th_writer, NULL, writer_thread, NULL);
    pthread_create(&th_reader, NULL, reader_thread, NULL);
    pthread_create(&th_req,    NULL, request_handler_thread, NULL);

    // 5) Simple event loop: wake on signals and print random if requested
    for (;;) {
        pause();           // blocked until a signal arrives
        maybe_print_random();
        // NOTE: threads keep running independently; no need to wake them here
    }

    return 0;
}


