#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <linux/limits.h>

#define MAX_WORD_LENGTH 50
#define DICTIONARY_DIR "./dictionary_files"

typedef struct {
    char english[MAX_WORD_LENGTH];
    char french[MAX_WORD_LENGTH];
} WordPair;

WordPair *dictionary = NULL;
int dictionary_size = 0;

void handle_usr1(int sig) {
    // Handle SIGUSR1: Print English-to-French translation
    if (dictionary_size > 0) {
        int index = rand() % dictionary_size;  // Select a random index
        printf("English: %s -> French: %s\n", dictionary[index].english, dictionary[index].french);
    }
}

void handle_usr2(int sig) {
    // Handle SIGUSR2: Print French-to-English translation
    if (dictionary_size > 0) {
        int index = rand() % dictionary_size;  // Select a random index
        printf("French: %s -> English: %s\n", dictionary[index].french, dictionary[index].english);
    }
}

int read_word_pairs_from_file(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening file");
        return 0;
    }

    char english[MAX_WORD_LENGTH], french[MAX_WORD_LENGTH];

    while (fscanf(file, "%49[^;];%49s\n", english, french) == 2) {
        dictionary_size += 1;
        dictionary = realloc(dictionary, dictionary_size * sizeof(WordPair));

        if (dictionary == NULL) {
            perror("Realloc failed");
            fclose(file);
            return 0;
        }

        strncpy(dictionary[dictionary_size - 1].english, english, MAX_WORD_LENGTH);
        strncpy(dictionary[dictionary_size - 1].french, french, MAX_WORD_LENGTH);
    }

    fclose(file);
    return 1;
}

int check_directory_for_new_files() {
    DIR *openedDir = opendir(DICTIONARY_DIR);
    if (!openedDir) {
        perror("Error opening directory");
        return 0;
    }

    struct dirent *dir;
    while ((dir = readdir(openedDir)) != NULL) {
        // Skip "." and ".." entries
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        char file_path[PATH_MAX];
        snprintf(file_path, sizeof(file_path), "%s/%s", DICTIONARY_DIR, dir->d_name);

        // Read word pairs from the current file
        if (!read_word_pairs_from_file(file_path)) {
            closedir(openedDir);
            return 0;
        }
    }

    closedir(openedDir);
    return 1;
}

int main() {
    // Register signal handlers
    signal(SIGUSR1, handle_usr1);
    signal(SIGUSR2, handle_usr2);

    // Load dictionary initially
    if (!check_directory_for_new_files()) {
        printf("Error processing the directory\n");
        return 1;
    }

    // Main loop to keep the server running and responsive to signals
    while (1) {
        pause();  // Wait for signals (blocks until a signal is received)
    }

    // Clean up
    free(dictionary);
    return 0;
}
