// rfind.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <regex.h>
#include <libgen.h>
#include <sys/stat.h>
#include <errno.h>

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

void create_directory(const char* path) {
    char tmp[MAX_PATH_LENGTH];
    char* p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    }
    mkdir(tmp, S_IRWXU);
}

void receive_file(int socket, const Message* msg) {
    const char* remote_path = msg->data;
    char local_filename[MAX_PATH_LENGTH];
    
    // Get just the filename part
    const char* last_slash = strrchr(remote_path, '/');
    const char* filename = last_slash ? last_slash + 1 : remote_path;
    
    // Copy the filename to our local buffer
    strncpy(local_filename, filename, MAX_PATH_LENGTH - 1);
    local_filename[MAX_PATH_LENGTH - 1] = '\0';

    // Create directories if needed
    char* dir_path = strdup(msg->data);
    if (dir_path) {
        char* dir_name = dirname(dir_path);
        create_directory(dir_name);
        free(dir_path);
    }
    
    printf("Receiving file: %s (%d bytes)\n", msg->data, msg->size);
    
    FILE* file = fopen(msg->data, "wb");
    if (!file) {
        perror("Error creating file");
        return;
    }

    char buffer[BUFFER_SIZE];
    long total_received = 0;
    ssize_t bytes_received;

    while (total_received < msg->size) {
        bytes_received = recv(socket, buffer, 
            (msg->size - total_received < BUFFER_SIZE) ? msg->size - total_received : BUFFER_SIZE, 0);
        
        if (bytes_received <= 0) {
            perror("Error receiving file data");
            break;
        }
        
        size_t bytes_written = fwrite(buffer, 1, (size_t)bytes_received, file);
        if (bytes_written != (size_t)bytes_received) {
            perror("Error writing to file");
            break;
        }
        
        total_received += bytes_received;
    }

    fclose(file);
    printf("File received: %s\n", msg->data);
}

void glob_to_regex(const char* glob, char* regex) {
    char* dst = regex;
    *dst++ = '^';
    
    for (const char* src = glob; *src != '\0'; src++) {
        switch (*src) {
            case '*':
                *dst++ = '.';
                *dst++ = '*';
                break;
            case '?':
                *dst++ = '.';
                break;
            case '.':
                *dst++ = '\\';
                *dst++ = '.';
                break;
            case '\\':
                *dst++ = '\\';
                *dst++ = '\\';
                break;
            case '[':
            case ']':
            case '^':
            case '$':
            case '(':
            case ')':
            case '{':
            case '}':
            case '+':
            case '|':
                *dst++ = '\\';
                *dst++ = *src;
                break;
            default:
                *dst++ = *src;
                break;
        }
    }
    *dst++ = '$';
    *dst = '\0';
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        printf("Usage: %s <server_ip> <port> <path> -name <pattern> [-get]\n", argv[0]);
        return 1;
    }

    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return 1;
    }

    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Connection failed");
        return 1;
    }

    SearchRequest request;
    memset(&request, 0, sizeof(request));
    strncpy(request.path, argv[3], MAX_PATH_LENGTH - 1);
    
    // Check for -get option
    request.get_files = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-get") == 0) {
            request.get_files = 1;
            break;
        }
    }

    // Find the pattern after -name
    char* pattern = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-name") == 0) {
            pattern = argv[i + 1];
            break;
        }
    }

    if (pattern == NULL) {
        fprintf(stderr, "No pattern specified after -name\n");
        close(client_socket);
        return 1;
    }

    // Remove quotes if present
    if (pattern[0] == '"' && pattern[strlen(pattern)-1] == '"') {
        pattern[strlen(pattern)-1] = '\0';
        pattern++;
    }

    // Convert glob pattern to regex
    glob_to_regex(pattern, request.pattern);

    printf("Searching in: %s\n", request.path);
    printf("Pattern: %s\n", pattern);
    printf("Regex: %s\n", request.pattern);
    
    send(client_socket, &request, sizeof(request), 0);

    Message msg;
    int files_found = 0;
    
    while (1) {
        ssize_t bytes_received = recv(client_socket, &msg, sizeof(Message), 0);
        
        if (bytes_received <= 0) {
            perror("Connection error");
            break;
        }

        switch (msg.type) {
            case 0: // Path message
                printf("Found: %s\n", msg.data);
                files_found++;
                break;
            
            case 1: // File content
                if (request.get_files) {
                    receive_file(client_socket, &msg);
                }
                break;
            
            case 2: // End message
                if (strlen(msg.data) > 0) {
                    printf("Server message: %s\n", msg.data);
                }
                goto search_complete;
                break;
        }
    }

search_complete:
    printf("\nSearch complete. Found %d matching files.\n", files_found);

    close(client_socket);
    return 0;
}