#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h> 
#include <string.h> 
#include <stdlib.h> 
#include "llama.h"


#define MAX 512
#define MAX_USERS 50
#define PORT 64525

LLaMA llama;
llama.load("/LLM-TPU/bmodels/llama2-7b_int4_1dev_seq512.bmodel");

// Structure necessary for exclusive access
pthread_mutex_t client_list_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int client_socket;
} ClientConnection;

typedef struct {
    ClientConnection clients[MAX_USERS];
    int client_count;
} ClientList;

ClientList myClientList;

int AddClient(ClientList *list, int socket) {
    pthread_mutex_lock(&client_list_mutex);
    if (list->client_count == MAX_USERS) {
        pthread_mutex_unlock(&client_list_mutex);
        return -1;
    } else {
        list->clients[list->client_count].client_socket = socket;
        list->client_count++;
        pthread_mutex_unlock(&client_list_mutex);
        return 0;
    }
}

int RemoveClient(ClientList *list, int socket) {
    pthread_mutex_lock(&client_list_mutex);
    int pos = -1;
    for (int i = 0; i < list->client_count; i++) {
        if (list->clients[i].client_socket == socket) {
            pos = i;
            break;
        }
    }
    if (pos == -1) {
        pthread_mutex_unlock(&client_list_mutex);
        return -1;
    } else {
        for (int i = pos; i < list->client_count - 1; i++) {
            list->clients[i] = list->clients[i + 1];
        }
        list->client_count--;
        pthread_mutex_unlock(&client_list_mutex);
        return 0;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// This is for the server to act upon the message it received from user

void *AttendClient(void *socket) {
    int client_socket;
    int *s;
    s = (int *)socket;
    client_socket = *s;

    char request[MAX];
    char response[MAX];
    int ret;

    int terminate = 0;
    // We enter a loop to attend all requests from this client until it disconnects
    while (terminate == 0) {
        // Now we receive the request
        ret = read(client_socket, request, sizeof(request));
        if (ret <= 0) {
            // Error or disconnection, close the socket and exit the loop
            printf("Error reading from client or client disconnected\n");
            close(client_socket);
            pthread_mutex_lock(&client_list_mutex);
            RemoveClient(&myClientList, client_socket);
            pthread_mutex_unlock(&client_list_mutex);
            terminate = 1;
        } else {
            printf("Received\n");
            request[ret] = '\0';
            printf("Request: %s\n", request);
            // let's see what they want
            char *p = strtok(request, "/");
            int code = atoi(p);
            int i = 0;

            // disconnection request
            if (code == 0 || ret <= 0) {
                pthread_mutex_lock(&client_list_mutex);
                int res = RemoveClient(&myClientList, client_socket);
                pthread_mutex_unlock(&client_list_mutex);
                terminate = 1;
            } else if (code == 1) { // send chat message
                char message[MAX];

                if (p != NULL) {
                    p = strtok(NULL, "/");
                    strcpy(message, p);

                    // Generate response using LLaMA2 model
                    char llama_response[MAX];
                    llama.generate(message, llama_response, MAX);

                    // Send the response back to the client
                    snprintf(response, MAX, "1/BOT/%.*s/", (int)(MAX - 8 - 1), llama_response);
                    ret = write(client_socket, response, strlen(response));
                    if (ret < 0) {
                        printf("Error writing to client\n");
                        close(client_socket);
                        pthread_mutex_lock(&client_list_mutex);
                        RemoveClient(&myClientList, client_socket);
                        pthread_mutex_unlock(&client_list_mutex);
                        terminate = 1;
                    }
                }
            }
            // This is for the server to send the message to the user
            if ((code != 0)) {
                printf("Response: %s\n", response);
            }
        }
    }
    // Close the socket when the client disconnects
    close(client_socket);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[]) {
    ClientList myClientList;
    myClientList.client_count = 0;
    int client_socket, listen_socket;
    struct sockaddr_in server_address;
    pthread_t thread;

    // Open the socket
    if ((listen_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error creating socket");
        printf("Server will continue to run, but may not be able to accept new connections.\n");
    } else {
        // Bind to the port
        memset(&server_address, 0, sizeof(server_address));
        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr = htonl(INADDR_ANY);
        server_address.sin_port = htons(PORT);
        if (bind(listen_socket, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
            perror("Error binding");
            printf("Server will continue to run, but may not be able to accept new connections.\n");
            close(listen_socket);
        } else {
            // Listen for incoming connections
            if (listen(listen_socket, 3) < 0) {
                perror("Error in the Listen");
                printf("Server will continue to run, but may not be able to accept new connections.\n");
                close(listen_socket);
            } else {
                int i = 0;
                for (;;) {
                    printf("Listening\n");

                    client_socket = accept(listen_socket, NULL, NULL);
                    if (client_socket < 0) {
                        perror("Error accepting connection");
                        continue;
                    }

                    printf("I have received a connection\n");
                    int sockets[MAX_USERS];
                    sockets[i] = client_socket;

                    // Create thread and tell it what to do
                    if (pthread_create(&thread, NULL, AttendClient, &sockets[i]) != 0) {
                        perror("Error creating thread");
                        close(client_socket);
                        continue;
                    }

                    // Detach the thread
                    if (pthread_detach(thread) != 0) {
                        perror("Error detaching thread");
                    }

                    i = (i + 1) % MAX_USERS; // wrap around to avoid overflow
                }
            }
        }
    }
    return 0;
}

