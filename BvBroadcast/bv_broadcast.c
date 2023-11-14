#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <semaphore.h>
#include <fcntl.h>

#define N 4 // Total number of processes
#define T 1 // Maximum number of Byzantine processes
#define PORT_BASE 8080

int received_values[N][2] = {{0, 0}}; // Using an array to store the binary values (0 and 1)
int processId;
int hasBroadcasted[2] = {0, 0};  // Flags to check if the process has already broadcasted the values 0 and 1
int committedValues[2] = {0, 0}; // Flags to check if the process has committed the values 0 and 1
// actually this should be bin_values

sem_t *sem;
sem_t *sem_init_brd;

int countDistinctProcessesForValue(int value)
{
    int count = 0;
    for (int i = 0; i < N; i++)
    {
        if (received_values[i][value] == 1)
        {
            count++;
        }
    }
    return count;
}

void BV_broadcast(int value)
{
    int sockfd;
    struct sockaddr_in serverAddr;
    //int message[2] = {processId, value}; // First element is the process ID, second is the value

    printf("Process %d: Start BV broadcast\n", processId);
    hasBroadcasted[value] = 1; // Mark the value as broadcasted. Mettre apres fin broadcast + clean (il y avait une raison de mettre avant mais now no)

    for (int i = 0; i < N; i++)
    {
        if (i == processId)
        {
            // Send value to itself + receive
            //printf("Process %d Value %d sent to myself\n", processId, value);
            received_values[processId][value] = 1;
            int distinctCount = countDistinctProcessesForValue(value);
            //printf("Process %d Value %d distinct count: %d\n", processId, value, distinctCount);
            // Introduce bug 2T -> 2T - 1
            if (distinctCount > 2 * T && !committedValues[value])
            {
                //printf("Process %d commits value %d\n", processId, value);
                committedValues[value] = 1; // Mark the value as committed
            }
            
            continue;
        }

        int message[3] = {processId, value, i}; // add destination in message

        /*
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(PORT_BASE + i);
        inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
        */

        /*
        if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1)
        {
            perror("[Process] Connect failure");
            exit(EXIT_FAILURE);
        }
        */
        sockfd = 1;
        sleep(1); // 
        send(sockfd, &message, sizeof(message), 0);

        //printf("Process %d Value %d sent to process %d\n", processId, value, i);

        //close(sockfd);
    }
}

void processMessages(int value, int fromProcess)
{
    received_values[fromProcess][value] = 1;
    int distinctCount = countDistinctProcessesForValue(value);
    //printf("Process %d Value %d distinct count: %d\n", processId, value, distinctCount);
    // Introduce bug 2T -> 2T - 1
    if (distinctCount > 2 * T - 1 && !committedValues[value])
    {
        //printf("Process %d commits value %d\n", processId, value);
        committedValues[value] = 1; // Mark the value as committed
        
    }
    if (distinctCount > T && !hasBroadcasted[value])
    {
        BV_broadcast(value);
    }
    // This is where it registers its state to the controller 
    int valuesCount[2];
    //valuesCount[0] = getpid();
    valuesCount[0] = countDistinctProcessesForValue(0);
    valuesCount[1] = countDistinctProcessesForValue(1);
    send(-1, &valuesCount, sizeof(valuesCount), 0);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("Usage: %s <processId (0 to %d)> <initialValue (0 or 1)>\n", argv[0], N - 1);
        exit(1);
    }

    sem = sem_open("/sem_bv_broadcast", O_CREAT, 0644, 0);
    if (sem == SEM_FAILED)
    {
        perror("[Process] Semaphore open failed");
        exit(1);
    }
    sem_init_brd = sem_open("/sem_bv_broadcast_init_brd", O_CREAT, 0644, 0);
    if (sem_init_brd == SEM_FAILED)
    {
        perror("[Process] Semaphore open failed");
        exit(1);
    }

    processId = atoi(argv[1]);
    int initialValue = atoi(argv[2]);

    if (initialValue != 0 && initialValue != 1)
    {
        printf("[Process] Initial value must be 0 or 1.\n");
        exit(1);
    }

    int listenfd, connfd;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    int receivedValue;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    int yes = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("[Process] ERROR on setsockopt");
        exit(EXIT_FAILURE);
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT_BASE + processId);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1)
    {
        perror("[Process] Bind failure");
        exit(EXIT_FAILURE);
    }

    if (listen(listenfd, 10) == -1)
    {
        perror("[Process] Listen failure");
        exit(EXIT_FAILURE);
    }

    printf("Process %d with initial value %d listening on port %d...\n", processId, initialValue, PORT_BASE + processId);

    sem_wait(sem);
    printf("Process %d done waiting for sockets init\n", processId);
    sem_close(sem);

    if (processId == 3) { // just so the "problematic" messages arrive last, remove after
        sleep(5);
    }

    // Now, broadcast the initial value
    BV_broadcast(initialValue);

    // Something like that to be sure that all "normal" send messages are in the buffer first
    // and then we get only echo messages
    // after I can just mark msg i for i > N - 1 in redirect as echo messages I guess
    // remove after

    sem_wait(sem_init_brd);
    printf("Process %d done waiting for broadcast init\n", processId);
    sem_close(sem_init_brd);

    int receivedMessage[3]; // To store both the sender's process ID and the value

    // Listening loop
    while (1)
    {
        
        //connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
        /*
        if (connfd == -1)
        {
            perror("[Process] Accept failure");
            exit(EXIT_FAILURE);
        } */
        
        int nbytes = recv(listenfd, &receivedMessage, sizeof(receivedMessage), 0); // connfd-listenfd
        if (nbytes == -1)
        {
            perror("[Process] Recv failure");
            exit(EXIT_FAILURE);
        }
        sleep(3); //
        int senderId = receivedMessage[0];
        int receivedValue = receivedMessage[1];
        int destinationId = receivedMessage[2];
        //printf("Process %d: Value %d received from process %d\n", processId, receivedValue, senderId);
        if (receivedValue >= 0)
        { // Ignore special signals
            processMessages(receivedValue, senderId);
        }
        //close(connfd);
    }

    return 0;
}
