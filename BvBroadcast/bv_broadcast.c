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
            // * 
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
        // *
    }
    if (distinctCount > T && !hasBroadcasted[value]) {
        BV_broadcast(value);
    }
    // actually this is a problem because might exec after a broadcast
    // = not the first send after recv
    // = not what controller waits for (but does)
    // need replace register state by a function in redirect
    // then put logic so that first call to this fct after a recv remembers socket
    // but send create another
    // if i can't make a shared lib that works just override an existing fct
    // that i dont use
    // or need to send state at * and here, be careful not to call twice...
    int nb = register_state (state); // state = committedValues
      if (nb == -1)
	{
	  perror ("[Receiver] register state");
	  exit (EXIT_FAILURE);
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
        int nbytes = recv(connfd, &receivedMessage, sizeof(receivedMessage), 0);
        if (nbytes == -1) {
            perror ("Recv failure");
            exit (EXIT_FAILURE);
        }
        sleep(1); // just chill
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
