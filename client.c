// client.c — dual mode client
// - No arguments → Signal mode: send SIGUSR1/SIGUSR2 in two threads
// - "--signals ..." → Explicit signal mode with custom counts/intervals
// - "EN word..." or "FR ..." → Request/Reply mode: ask translations and wait for replies


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <time.h>


#include "proto.h"


// ---------- Signal mode ----------
typedef struct {
pid_t server_pid;
int count; // 0 => infinite
int interval_ms; // milliseconds
int sig; // SIGUSR1 or SIGUSR2
} sender_args_t;


static pid_t read_pid_from_file(const char *path) {
FILE *f = fopen(path, "r");
if (!f) return -1;
long v = -1;
if (fscanf(f, "%ld", &v) != 1) { fclose(f); return -1; }
fclose(f);
return (pid_t)v;
}


static void *sender_thread(void *arg) {
sender_args_t *cfg = (sender_args_t *)arg;
int sent = 0;
for (;;) {
if (kill(cfg->server_pid, cfg->sig) == -1) {
perror("kill");
break;
}
printf("[signals] sent %s\n", cfg->sig == SIGUSR1 ? "SIGUSR1" : "SIGUSR2");
fflush(stdout);
sent++;
if (cfg->count > 0 && sent >= cfg->count) break;
usleep(cfg->interval_ms * 1000);
}
return NULL;
}


static int run_signal_mode(int cnt1, int cnt2, int int1_ms, int int2_ms) {
pid_t server_pid = read_pid_from_file(PID_FILE);
if (server_pid <= 0) {
fprintf(stderr, "Could not read server PID from %s\n", PID_FILE);
return 1;
}
printf("Signal mode → server PID %d\n", server_pid);


sender_args_t a1 = { server_pid, cnt1, int1_ms, SIGUSR1 };
sender_args_t a2 = { server_pid, cnt2, int2_ms, SIGUSR2 };
pthread_t t1, t2;
pthread_create(&t1, NULL, sender_thread, &a1);
pthread_create(&t2, NULL, sender_thread, &a2);
pthread_join(t1, NULL);
pthread_join(t2, NULL);
return 0;
}


// ---------- Request/Reply mode ----------
static int run_request_mode(int dir, char **words, int nwords) {
int req_q = msgget(MSG_REQ_KEY, 0666);
int resp_q = msgget(MSG_RESP_KEY, 0666);
if (req_q == -1 || resp_q == -1) {
perror("msgget");
return 1;
}


pid_t me = getpid();
unsigned base_id = (unsigned)time(NULL);


// send all requests first
for (int i = 0; i < nwords; ++i) {
MsgReq r; memset(&r, 0, sizeof(r));
r.mtype = dir; // 1 EN->FR, 2 FR->EN
r.reply_to = me; // server will reply using this as mtype
r.req_id = base_id + i;
strncpy(r.word, words[i], MAX_WORD_LENGTH - 1);
if (msgsnd(req_q, &r, sizeof(MsgReq) - sizeof(long), 0) == -1) {
perror("msgsnd(req)");
return 1;
}
printf("[REQ] %s (%s)\n", r.word, dir == 1 ? "EN→FR" : "FR→EN");
fflush(stdout);
}


// wait and collect replies (one per request)
for (int i = 0; i < nwords; ++i) {
MsgResp s; memset(&s, 0, sizeof(s));
if (msgrcv(resp_q, &s, sizeof(MsgResp) - sizeof(long), me, 0) == -1) {
perror("msgrcv(resp)");
return 1;
}
if (s.found) printf("[RESP] %s -> %s\n", s.from, s.to);
else printf("[RESP] Not found: %s\n", s.from);
fflush(stdout);
}


return 0;
}


int main(int argc, char **argv) {
// Mode 1 (default): no args → signal mode (10 & 10 signals with 1000/1500 ms)
// Mode 2: --signals [cnt1 cnt2 int1_ms int2_ms]
// Mode 3: EN word... OR FR word...
if (argc == 1) {
return run_signal_mode(10, 10, 1000, 1500);
}


if (strcmp(argv[1], "--signals") == 0) {
int cnt1 = (argc > 2) ? atoi(argv[2]) : 10;
int cnt2 = (argc > 3) ? atoi(argv[3]) : 10;
int int1 = (argc > 4) ? atoi(argv[4]) : 1000;
int int2 = (argc > 5) ? atoi(argv[5]) : 1500;
return run_signal_mode(cnt1, cnt2, int1, int2);
}


if (strcmp(argv[1], "EN") == 0 || strcmp(argv[1], "FR") == 0) {
if (argc < 3) {
fprintf(stderr, "Usage: %s EN|FR word1 [word2 ...]\n", argv[0]);
return 1;
}
int dir = (argv[1][0] == 'E') ? 1 : 2;
return run_request_mode(dir, &argv[2], argc - 2);
}


fprintf(stderr,
"Usage:\n"
" %s # default: signal mode\n"
" %s --signals [cnt1 cnt2 int1_ms int2_ms]\n"
" %s EN word1 [word2 ...] # request EN→FR\n"
" %s FR word1 [word2 ...] # request FR→EN\n",
argv[0], argv[0], argv[0], argv[0]);
return 1;
}

// # signal mode (defaults: 10 & 10 signals)
// ./client

// # signal mode with custom counts/intervals
// ./client --signals 1 1 1000 1000    # send 1x SIGUSR1 and 1x SIGUSR2

// # request→reply
// ./client EN hello
// ./client FR bonjour
