#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <linux/limits.h>

#define MAX_WORD_LENGTH 50
#define DICTIONARY_DIR "./dictionary_files"
#define SHM_KEY 1234
#define MSG_KEY 5678
#define MAX_WORDS 1024

typedef struct {
    char english[MAX_WORD_LENGTH];
    char french[MAX_WORD_LENGTH];
} WordPair;

typedef struct {
    WordPair words[MAX_WORDS];
    int size;
    pthread_mutex_t mutex;
} SharedDictionary;

SharedDictionary *shared_dict = NULL;

typedef struct {
    long mtype; // 1 = EN_FR, 2 = FR_EN
    char english[MAX_WORD_LENGTH];
    char french[MAX_WORD_LENGTH];
} Msg;

// For tracking file timestamps
typedef struct {
    char filename[PATH_MAX];
    time_t mtime;
} FileInfo;

FileInfo tracked_files[128];
int tracked_count = 0;
static const char *PID_FILE = "/tmp/dict_server.pid";

static int write_pid_file(const char *path, pid_t pid) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", pid);
    fflush(f);
    int fd = fileno(f);
    if (fd != -1) fsync(fd);
    fclose(f);

    if (rename(tmp, path) == -1) {
        remove(tmp);
        return -1;
    }
    return 0;
}

static void remove_pid_file(void) {
    unlink(PID_FILE);
}

static void handle_sigint(int sig) {
    (void)sig;
    remove_pid_file();
    _exit(0);
}
void handle_usr1(int sig) {
    // Handle SIGUSR1: Print English-to-French translation
    pthread_mutex_lock(&shared_dict->mutex);
    if (shared_dict->size > 0) {
        int idx = rand() % shared_dict->size;
        printf("English: %s -> French: %s\n",
               shared_dict->words[idx].english,
               shared_dict->words[idx].french);
    }
    pthread_mutex_unlock(&shared_dict->mutex);
}

void handle_usr2(int sig) {
    // Handle SIGUSR2: Print French-to-English translation
    pthread_mutex_lock(&shared_dict->mutex);
    if (shared_dict->size > 0) {
        int idx = rand() % shared_dict->size;
        printf("French: %s -> English: %s\n",
               shared_dict->words[idx].french,
               shared_dict->words[idx].english);
    }
    pthread_mutex_unlock(&shared_dict->mutex);
}

int is_new_file(const char *filename, time_t mtime) {
    // Check if file is new or modified
    for (int i = 0; i < tracked_count; i++) {
        if (strcmp(tracked_files[i].filename, filename) == 0) {
            return mtime > tracked_files[i].mtime;
        }
    }
    return 1; // not tracked yet
}

void update_tracked_file(const char *filename, time_t mtime) {
    // Update tracked file info
    for (int i = 0; i < tracked_count; i++) {
        if (strcmp(tracked_files[i].filename, filename) == 0) {
            tracked_files[i].mtime = mtime;
            return;
        }
    }
    // New file
    strncpy(tracked_files[tracked_count].filename, filename, PATH_MAX);
    tracked_files[tracked_count].mtime = mtime;
    tracked_count++;
}

void read_word_pairs_from_file(const char *filename, int direction, int msgid) {
    // Read word pairs from file and send to message queue
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening file");
        return;
    }

    char line[128];
    // Skip first line if it's direction marker
    if (fgets(line, sizeof(line), file) && strstr(line, "# direction:") != NULL) {
        // first line processed
    } else {
        rewind(file); // no direction line
    }

    while (fgets(line, sizeof(line), file)) {
        char *eng = strtok(line, ";\n");
        char *fr = strtok(NULL, ";\n");
        if (eng && fr) {
            Msg msg;
            msg.mtype = direction;
            strncpy(msg.english, eng, MAX_WORD_LENGTH);
            strncpy(msg.french, fr, MAX_WORD_LENGTH);
            if (msgsnd(msgid, &msg, sizeof(Msg) - sizeof(long), 0) == -1) {
                perror("msgsnd");
            }
        }
    }

    fclose(file);
}

void *writer_thread(void *arg) {
    int msgid = *((int *)arg);

    while (1) {
        DIR *dir = opendir(DICTIONARY_DIR);
        if (!dir) {
            perror("Error opening directory");
            sleep(5);
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "%s/%s", DICTIONARY_DIR, entry->d_name);

            struct stat st;
            if (stat(filepath, &st) == -1) continue;

            if (!is_new_file(entry->d_name, st.st_mtime)) continue;
            
            // Determine direction from first line (example: "# direction: EN_FR")
            FILE *f = fopen(filepath, "r");
            int direction = 1; // default EN_FR
            if (f) {
                char first_line[128];
                if (fgets(first_line, sizeof(first_line), f)) {
                    if (strstr(first_line, "FR_EN") != NULL) direction = 2;
                }
                fclose(f);
            }

            read_word_pairs_from_file(filepath, direction, msgid);
            update_tracked_file(entry->d_name, st.st_mtime);
        }

        closedir(dir);
        sleep(5); // periodic scan
    }

    return NULL;
}

void *reader_thread(void *arg) {
    int msgid = *((int *)arg);
    Msg msg;

    while (1) {
        if (msgrcv(msgid, &msg, sizeof(Msg) - sizeof(long), 0, 0) == -1) {
            perror("msgrcv failed");
            continue;
        }

        pthread_mutex_lock(&shared_dict->mutex);
        if (shared_dict->size < MAX_WORDS) {
            if (msg.mtype == 1) { // EN_FR
                strncpy(shared_dict->words[shared_dict->size].english, msg.english, MAX_WORD_LENGTH);
                strncpy(shared_dict->words[shared_dict->size].french, msg.french, MAX_WORD_LENGTH);
            } else { // FR_EN
                strncpy(shared_dict->words[shared_dict->size].english, msg.french, MAX_WORD_LENGTH);
                strncpy(shared_dict->words[shared_dict->size].french, msg.english, MAX_WORD_LENGTH);
            }

            shared_dict->size++;
        }
        pthread_mutex_unlock(&shared_dict->mutex);
    }

    return NULL;
}

// int check_directory_for_new_files() {
//     DIR *openedDir = opendir(DICTIONARY_DIR);
//     if (!openedDir) {
//         perror("Error opening directory");
//         return 0;
//     }

//     struct dirent *dir;
//     while ((dir = readdir(openedDir)) != NULL) {
//         // Skip "." and ".." entries
//         if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
//             continue;
//         }

//         char file_path[PATH_MAX];
//         snprintf(file_path, sizeof(file_path), "%s/%s", DICTIONARY_DIR, dir->d_name);

//         // Read word pairs from the current file
//         if (!read_word_pairs_from_file(file_path)) {
//             closedir(openedDir);
//             return 0;
//         }
//     }

//     closedir(openedDir);
//     return 1;
// }

int main() {
   srand(time(NULL));

    // Setup shared memory
    int shmid = shmget(SHM_KEY, sizeof(SharedDictionary), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget failed");
        return 1;
    }

    shared_dict = shmat(shmid, NULL, 0);
    if (shared_dict == (void *)-1) {
        perror("shmat failed");
        return 1;
    }

    shared_dict->size = 0;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shared_dict->mutex, &attr);

    // Setup message queue
    int msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msgid == -1) {
        perror("msgget failed");
        return 1;
    }

    // Create writer and reader threads
    pthread_t writer, reader;
    pthread_create(&writer, NULL, writer_thread, &msgid);
    pthread_create(&reader, NULL, reader_thread, &msgid);

    // Register signal handlers (for random test)
    signal(SIGUSR1, handle_usr1);
    signal(SIGUSR2, handle_usr2);
     pid_t mypid = getpid();
    if (write_pid_file(PID_FILE, mypid) != 0) {
        perror("write_pid_file");
    }
    atexit(remove_pid_file);              
    struct sigaction sa_int = {0};
    sa_int.sa_handler = handle_sigint;     
    sigaction(SIGINT, &sa_int, NULL);

    printf("Server PID: %d (pid fayl: %s)\n", mypid, PID_FILE);
    // Main loop
    while (1) pause();

    // Cleanup (never reached in this demo)
    pthread_mutex_destroy(&shared_dict->mutex);
    shmdt(shared_dict);
    shmctl(shmid, IPC_RMID, NULL);
    msgctl(msgid, IPC_RMID, NULL);

    return 0;
}
