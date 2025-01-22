/*

  University of Texas at El Paso, Department of Computer Science.

  Contributor: Bryan Perez
  
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>

#define buf_size 4096  // buffer size for data transfer

// close all file descriptors in an array
void close_fds(int *fds, int count) {
    for (int i = 0; i < count; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
}

// function to transfer data between file descriptors
// reads from infd and writes to outfd; returns -1 on failure, 0 on success
int copy_data(int infd, int outfd) {
    char buffer[buf_size];
    ssize_t bytes_read, bytes_written;

    // read data into the buffer
    bytes_read = read(infd, buffer, buf_size);
    if (bytes_read < 0) return -1;  // error in reading
    if (bytes_read == 0) return 0;  // end of data

    // write data from the buffer
    bytes_written = write(outfd, buffer, bytes_read);
    if (bytes_written != bytes_read) return -1;  // error in writing

    return 1;  // success
}

// function to handle an individual client connection
void handle_client_sock(int client_sock) {
    uint16_t argc, arg_len;      // argument count and argument length
    char *args[256];             // array to hold command arguments
    ssize_t n;                   // number of bytes read
    int pipes[2][2];             // pipes for communication between parent and child
    fd_set fds;                  // file descriptor set for select()
    int maxfd, result;           // max file descriptor and result for select()

    // step 1: read the number of arguments (argc) from the client
    n = read(client_sock, &argc, sizeof(argc));
    if (n <= 0) goto cleanup;    // if no data or error, clean up and return
    argc = ntohs(argc);          // convert to host byte order

    // step 2: read each argument from the client
    for (int i = 0; i < argc; i++) {
        n = read(client_sock, &arg_len, sizeof(arg_len));
        if (n <= 0) goto cleanup;  // clean up on error
        arg_len = ntohs(arg_len);  // convert argument length to host byte order

        // allocate memory for the argument and read it from the client
        args[i] = (char *)malloc(arg_len + 1);
        if (!args[i]) goto cleanup;  // memory allocation failed
        n = read(client_sock, args[i], arg_len);
        if (n <= 0) goto cleanup;    // read error
        args[i][arg_len] = '\0';     // null-terminate the argument string
    }
    args[argc] = NULL;  // null-terminate the argument array for execvp

    // step 3: create pipes for communication with the child process
    if (pipe(pipes[0]) < 0 || pipe(pipes[1]) < 0) {
        perror("pipe creation failed");
        goto cleanup;
    }

    // step 4: fork a child process to execute the command
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        goto cleanup;
    }

    if (pid == 0) {  // child process
        // close unused pipe ends and redirect standard input/output
        close(pipes[0][1]);
        close(pipes[1][0]);
        dup2(pipes[0][0], stdin_FILENO);     // redirect stdin to read pipe
        dup2(pipes[1][1], stdout_FILENO);    // redirect stdout to write pipe
        dup2(pipes[1][1], stderr_FILENO);    // redirect stderr to write pipe
        close(pipes[0][0]);
        close(pipes[1][1]);

        // execute the command with arguments received from the client
        execvp(args[0], args);
        perror("execvp failed");  // only if execvp fails
        exit(EXIT_FAILURE);
    } else {  // parent process
        // close unused pipe ends
        close(pipes[0][0]);
        close(pipes[1][1]);

        // step 5: use select() to manage data transfer between client and child process
        for (;;) {
            fd_zero(&fds);                // clear file descriptor set
            fd_set(client_sock, &fds);    // add client socket to set
            fd_set(pipes[1][0], &fds);    // add child stdout pipe to set
            maxfd = client_sock > pipes[1][0] ? client_sock : pipes[1][0];

            // wait for data to be available on any of the file descriptors
            result = select(maxfd + 1, &fds, NULL, NULL, NULL);
            if (result < 0) break;  // error in select, break the loop

            // if there's data from the client, forward it to the child process
            if (fd_isset(client_sock, &fds)) {
                result = copy_data(client_sock, pipes[0][1]);
                if (result <= 0) break;  // error or end of data
            }

            // if there's data from the child process, send it to the client
            if (fd_isset(pipes[1][0], &fds)) {
                result = copy_data(pipes[1][0], client_sock);
                if (result <= 0) break;  // error or end of data
            }
        }

        // close pipes and wait for the child to terminate
        close(pipes[0][1]);
        close(pipes[1][0]);
        waitpid(pid, NULL, 0);  // wait for child to finish
    }

cleanup:
    // free allocated memory for arguments
    for (int i = 0; i < argc; i++) {
        free(args[i]);
    }
    close(client_sock);  // close client socket
}

// main function to start the server and handle incoming connections
int main(int argc, char *argv[]) {
    if (argc != 2) {  // check for correct usage
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);  // convert port number from string to integer
    if (port <= 0 || port > 65535) {  // validate port range
        fprintf(stderr, "invalid port number\n");
        exit(EXIT_FAILURE);
    }

    // step 1: create a socket for the server
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // step 2: set up the server address structure and bind it to the socket
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // step 3: listen for incoming connections
    if (listen(server_sock, 5) < 0) {
        perror("listen failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    printf("server listening on port %d...\n", port);

    // main server loop to accept and handle incoming client connections
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // accept a connection from a client
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            perror("accept failed");
            continue;
        }

        // print client connection details
        printf("connection accepted from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // fork a process to handle the client connection
        if (fork() == 0) {  // child process
            close(server_sock);             // close server socket in child
            handle_client_sock(client_sock); // handle client interaction
            exit(0);                        // exit child process after handling
        }
        close(client_sock);  // parent closes client socket
    }

    close(server_sock);  // close server socket before exiting
    return 0;
}