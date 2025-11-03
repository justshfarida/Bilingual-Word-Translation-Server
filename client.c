#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>

int main() {
    pid_t server_pid = 12345;  // Replace with the actual server process ID
    
    srand(time(NULL));  // Initialize random seed

    for (int i = 0; i < 100; i++) {
        int signal_type = rand() % 2; 

        if (signal_type == 0) {
            // Send SIGUSR1 (English → French)
            kill(server_pid, SIGUSR1);
            printf("Client: Sent SIGUSR1\n");
        } else {
            // Send SIGUSR2 (French → English)
            kill(server_pid, SIGUSR2);
            printf("Client: Sent SIGUSR2\n");
        }

        sleep(1);  // Wait for 1 second before sending the next signal
    }

    return 0;
}
