// client.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

static const char *PID_FILE = "/tmp/dict_server.pid";

typedef struct {
    pid_t server_pid;
    int   count;        // how many signals
    int   interval_ms;  //interval between two signals
    int   sig;          // SIGUSR1 or SIGUSR2
} sender_args_t;

static pid_t read_pid_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long val = -1;
    if (fscanf(f, "%ld", &val) != 1) { fclose(f); return -1; }
    fclose(f);
    return (pid_t)val;
}

static void *sender_thread(void *arg) {
    sender_args_t *cfg = (sender_args_t*)arg;
    int sent = 0;
    for (;;) {
        if (kill(cfg->server_pid, cfg->sig) == -1) {
            perror("kill");
            break;
        }
        printf("Client: sent %s\n", (cfg->sig == SIGUSR1 ? "SIGUSR1" : "SIGUSR2"));
        sent++;
        if (cfg->count > 0 && sent >= cfg->count) break;
        usleep(cfg->interval_ms * 1000);
    }
    return NULL;
}

int main(int argc, char **argv) {
    // argv[1] -> SIGUSR1 say (default 10), argv[2] -> SIGUSR2 say (default 10)
    // argv[3] -> SIGUSR1 interval ms (default 1000), argv[4] -> SIGUSR2 interval ms (default 1500)
    int cnt1 = (argc > 1) ? atoi(argv[1]) : 10;
    int cnt2 = (argc > 2) ? atoi(argv[2]) : 10;
    int int1 = (argc > 3) ? atoi(argv[3]) : 1000;
    int int2 = (argc > 4) ? atoi(argv[4]) : 1500;

    pid_t server_pid = read_pid_from_file(PID_FILE);
    if (server_pid <= 0) {
        fprintf(stderr, "Could not read server PID from %s\n", PID_FILE);
        return 1;
    }
    printf("Client: using server PID %d\n", server_pid);

    // Thread config
    sender_args_t a1 = { .server_pid = server_pid, .count = cnt1, .interval_ms = int1, .sig = SIGUSR1 };
    sender_args_t a2 = { .server_pid = server_pid, .count = cnt2, .interval_ms = int2, .sig = SIGUSR2 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, sender_thread, &a1);
    pthread_create(&t2, NULL, sender_thread, &a2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    return 0;
}
