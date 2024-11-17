// rfind_server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <dirent.h>
#include <regex.h>

#define BUFFER_SIZE 1024
#define MAX_PATH_LENGTH 4096

typedef struct {
    char command[32];
    char path[MAX_PATH_LENGTH];
    char pattern[256];
    int get_files;
} SearchRequest;

typedef struct {
    int type;  // 0: path, 1: file content, 2: end
    int size;  // tamaño del contenido
    char data[MAX_PATH_LENGTH];
} Message;

void send_message(int client_socket, int type, const char* data, int size) {
    Message msg;
    msg.type = type;
    msg.size = size;
    strncpy(msg.data, data, MAX_PATH_LENGTH - 1);
    msg.data[MAX_PATH_LENGTH - 1] = '\0';
    
    if (send(client_socket, &msg, sizeof(Message), 0) == -1) {
        perror("Error sending message");
    }
}

void send_file_content(int client_socket, const char* filepath) {
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        perror("Error opening file");
        return;
    }

    struct stat st;
    if (stat(filepath, &st) != 0) {
        perror("Error getting file size");
        fclose(file);
        return;
    }

    // Send file message
    Message msg;
    msg.type = 1;  // file content
    msg.size = st.st_size;
    strncpy(msg.data, filepath, MAX_PATH_LENGTH - 1);
    if (send(client_socket, &msg, sizeof(Message), 0) == -1) {
        perror("Error sending file message");
        fclose(file);
        return;
    }

    // Send file content
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (send(client_socket, buffer, bytes_read, 0) == -1) {
            perror("Error sending file content");
            break;
        }
    }

    fclose(file);
}

int is_directory(const char *path) {
    struct stat statbuf;
    if (stat(path, &statbuf) != 0) {
        return 0;
    }
    return S_ISDIR(statbuf.st_mode);
}

void search_directory(int client_socket, const char* base_path, const char* pattern, int get_files, regex_t* regex) {
    DIR* dir = opendir(base_path);
    if (!dir) {
        perror("Error opening directory");
        return;
    }

    struct dirent* entry;
    char full_path[MAX_PATH_LENGTH];
    char rel_path[MAX_PATH_LENGTH];
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
        snprintf(rel_path, sizeof(rel_path), "%s", entry->d_name);

        if (is_directory(full_path)) {
            search_directory(client_socket, full_path, pattern, get_files, regex);
        } else {
            if (regexec(regex, entry->d_name, 0, NULL, 0) == 0) {
                printf("Found matching file: %s\n", full_path);
                
                // Send path message
                send_message(client_socket, 0, full_path, strlen(full_path));
                
                // If -get option is enabled, send the file content
                if (get_files) {
                    printf("Sending file: %s\n", full_path);
                    send_file_content(client_socket, full_path);
                }
            }
        }
    }

    closedir(dir);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        return 1;
    }

    // Allow port reuse
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(argv[1]));
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        return 1;
    }

    if (listen(server_socket, 5) == -1) {
        perror("Listen failed");
        return 1;
    }

    printf("Server listening on port %s...\n", argv[1]);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket == -1) {
            perror("Accept failed");
            continue;
        }

        SearchRequest request;
        if (recv(client_socket, &request, sizeof(request), 0) <= 0) {
            perror("Error receiving request");
            close(client_socket);
            continue;
        }

        // Change to the requested directory
        char original_dir[MAX_PATH_LENGTH];
        getcwd(original_dir, sizeof(original_dir)); // Save current directory

        if (chdir(request.path) != 0) {
            perror("Error changing to requested directory");
            send_message(client_socket, 2, "Error: Cannot access specified directory", 0);
            close(client_socket);
            continue;
        }

        printf("Searching in directory: %s\n", request.path);
        printf("Pattern: %s\n", request.pattern);
        printf("Get files: %s\n", request.get_files ? "yes" : "no");

        regex_t regex;
        if (regcomp(&regex, request.pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "Invalid regular expression\n");
            send_message(client_socket, 2, "Error: Invalid regular expression", 0);
            close(client_socket);
            chdir(original_dir);
            continue;
        }

        // Use "." as base path since we've already changed to the target directory
        search_directory(client_socket, ".", request.pattern, request.get_files, &regex);
        
        // Send end message
        send_message(client_socket, 2, "", 0);

        // Restore original directory
        chdir(original_dir);

        regfree(&regex);
        close(client_socket);
    }

    close(server_socket);
    return 0;
}