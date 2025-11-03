#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/limits.h>
#define MAX_WORD_LENGTH 50

typedef struct {
    char english[MAX_WORD_LENGTH];
    char french[MAX_WORD_LENGTH];
} WordPair;

// Function to read a file and add word pairs to the dictionary
int read_word_pairs_from_file(const char *filename, WordPair **dictionary, int *size) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening file");
        return 0;
    }

    char english[MAX_WORD_LENGTH];
    char french[MAX_WORD_LENGTH];

    // Read word pairs from the file
    while (fscanf(file, "%49[^;];%49s\n", english, french) == 2) {
        // Allocate memory for the new word pair
        *size += 1;
        *dictionary = realloc(*dictionary, *size * sizeof(WordPair));

        if (*dictionary == NULL) {
            perror("Realloc failed");
            fclose(file);
            return 0;
        }

        // Store the word pair in the dictionary
        strncpy((*dictionary)[*size - 1].english, english, MAX_WORD_LENGTH);
        strncpy((*dictionary)[*size - 1].french, french, MAX_WORD_LENGTH);
    }

    fclose(file);
    return 1;
}

// Function to check the directory and process files
int check_directory(char *pathname) {
    DIR *openedDir = opendir(pathname);
    if (!openedDir) {
        perror("Error opening directory");
        return 0;
    }

    struct dirent *dir;
    WordPair *dictionary = NULL;
    int size = 0;

    // Loop through all the entries in the directory
    while ((dir = readdir(openedDir)) != NULL) {
        // Skip "." and ".." entries
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        char file_path[PATH_MAX];
        snprintf(file_path, sizeof(file_path), "%s/%s", pathname, dir->d_name);

        // Read word pairs from the current file
        if (!read_word_pairs_from_file(file_path, &dictionary, &size)) {
            closedir(openedDir);
            return 0;
        }
    }

    closedir(openedDir);

    // Print the dictionary (for debugging)
    for (int i = 0; i < size; i++) {
        printf("English: %s, French: %s\n", dictionary[i].english, dictionary[i].french);
    }

    // Free the allocated memory
    free(dictionary);
    return 1;
}

int main() {
    char *directory = "./dictionary_files"; // Change this to your directory
    if (!check_directory(directory)) {
        printf("Error processing the directory\n");
    }
    return 0;
}
