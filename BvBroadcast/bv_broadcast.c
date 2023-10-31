#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <semaphore.h>
#include <fcntl.h>

#define N 4  // Total number of processes
#define T 1  // Maximum number of Byzantine processes
#define PORT_BASE 8080

int received_values[N][2] = {{0, 0}};  // Using an array to store the binary values (0 and 1)
int processId;
int hasBroadcasted[2] = {0, 0};  // Flags to check if the process has already broadcasted the values 0 and 1
int committedValues[2] = {0, 0};  // Flags to check if the process has committed the values 0 and 1
// actually this should be bin_values

sem_t *sem;

int countDistinctProcessesForValue(int value) {
    int count = 0;
    for (int i = 0; i < N; i++) {
        if (received_values[i][value] == 1) {
            count++;
        }
    }
    return count;
}

void BV_broadcast(int value) {
    int sockfd;
    struct sockaddr_in serverAddr;
    int message[2] = {processId, value};  // First element is the process ID, second is the value

    printf("Start BV broadcast\n");
    hasBroadcasted[value] = 1;  // Mark the value as broadcasted. Mettre apres fin broadcast + clean (il y avait une raison de mettre avant mais now no)

    for (int i = 0; i < N; i++) {
        if (i == processId) {
            // Send value to itself + receive
            printf("Process %d Value %d sent to myself\n", processId, value);
            received_values[processId][value] = 1;
            int distinctCount = countDistinctProcessesForValue(value);
            printf("Process %d Value %d distinct count: %d\n", processId, value, distinctCount);
            // Introduce bug 2T -> 2T - 1
            if (distinctCount > 2 * T && !committedValues[value]) {
              printf("Process %d commits value %d\n", processId, value);
              committedValues[value] = 1;  // Mark the value as committed
            } 
            continue;
        }

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(PORT_BASE + i);
        inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

        if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1) {
            perror ("Connect failure");
            exit (EXIT_FAILURE);
        }
        send(sockfd, &message, sizeof(message), 0);

        printf("Process %d Value %d sent to process %d\n", processId, value, i);

        close(sockfd);
    }
}

void processMessages(int value, int fromProcess) {
    received_values[fromProcess][value] = 1;
    int distinctCount = countDistinctProcessesForValue(value);
    printf("Process %d Value %d distinct count: %d\n", processId, value, distinctCount);
    // Introduce bug 2T -> 2T - 1
    if (distinctCount > 2 * T && !committedValues[value]) {
        printf("Process %d commits value %d\n", processId, value);
        committedValues[value] = 1;  // Mark the value as committed
    }
    if (distinctCount > T && !hasBroadcasted[value]) {
        BV_broadcast(value);
    }
}

void sendReadySignal() {
    int sockfd;
    struct sockaddr_in serverAddr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT_BASE);  // Sending to process 0
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1) {
            perror ("Connect failure");
            exit (EXIT_FAILURE);
    }
    int readySignal = -1;
    send(sockfd, &readySignal, sizeof(int), 0);

    printf("Ready signal sent\n");

    close(sockfd);
}

void waitForBroadcastSignal(int listenfd, struct sockaddr_in clientAddr, socklen_t addrLen) {
    int broadcastSignal, connfd;

    printf("Wait broadcast authorization\n");

    connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
    if (connfd == -1) {
            perror ("Accept failure");
            exit (EXIT_FAILURE);
    }
    recv(connfd, &broadcastSignal, sizeof(int), 0);
    if (broadcastSignal == -1) {
            perror ("Recv failure");
            exit (EXIT_FAILURE);
    }

    if (broadcastSignal == -2) {
        printf("Received broadcast authorization\n");
        close(connfd);
        return;  // Start broadcasting
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <processId (0 to %d)> <initialValue (0 or 1)>\n", argv[0], N-1);
        exit(1);
    }

    sem = sem_open("/sem_bv_broadcast", O_CREAT, 0644, 0);
    if (sem == SEM_FAILED) {
        perror("Semaphore open failed");
        exit(1);
    }

    processId = atoi(argv[1]);
    int initialValue = atoi(argv[2]);

    if (initialValue != 0 && initialValue != 1) {
        printf("Initial value must be 0 or 1.\n");
        exit(1);
    }

    int listenfd, connfd;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    int receivedValue;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT_BASE + processId);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1) {
            perror ("Bind failure");
            exit (EXIT_FAILURE);
    }

    if (listen(listenfd, 10) == -1) {
            perror ("Listen failure");
            exit (EXIT_FAILURE);
    }

    printf("Process %d with initial value %d listening on port %d...\n", processId, initialValue, PORT_BASE + processId);

    sem_wait(sem);
    sem_close(sem);

// This is the other way of synch processes to insure the setup is done before starting broadcast
// This is used to run each process in a different terminal "manually"
/*
    // Send ready signal to process 0
    sendReadySignal();

    // If this is process 0, wait for all ready signals
    if (processId == 0) {
        printf("Process 0\n");

        int readyCount = 0;
        while (readyCount < N) {
            connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
            if (connfd == -1) {
                perror ("Accept failure");
                exit (EXIT_FAILURE);
            }
            recv(connfd, &receivedValue, sizeof(int), 0);
            if (receivedValue == -1) {
                printf("ready : %d\n", readyCount);
                readyCount++;
            }
            close(connfd);
        }

        // Send broadcast signal to all processes
        for (int i = 1; i < N; i++) {
            int sockfd;
            struct sockaddr_in serverAddr;

            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(PORT_BASE + i);
            inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

            if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1) {
                perror ("Connect failure");
                exit (EXIT_FAILURE);
            }
            int broadcastSignal = -2;
            send(sockfd, &broadcastSignal, sizeof(int), 0);
            printf("Send ok broadcast to %d\n", i);

            close(sockfd);
        }
    } else {
        // Other processes wait for the broadcast signal from process 0
        waitForBroadcastSignal(listenfd, clientAddr, addrLen);
    }

    */

    // Now, broadcast the initial value
    BV_broadcast(initialValue);


    int receivedMessage[2];  // To store both the sender's process ID and the value

    // Listening loop
    while (1) {
        connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
        if (connfd == -1) {
            perror ("Accept failure");
            exit (EXIT_FAILURE);
        }
        recv(connfd, &receivedMessage, sizeof(receivedMessage), 0);
        if (*receivedMessage == -1) {
            perror ("Recv failure");
            exit (EXIT_FAILURE);
        }
        int senderId = receivedMessage[0];
        int receivedValue = receivedMessage[1];
        printf("Process %d Value %d received from process %d\n", processId, receivedValue, senderId);
        if (receivedValue >= 0) {  // Ignore special signals
            processMessages(receivedValue, senderId);
        }
        close(connfd);
    }

    return 0;
}
