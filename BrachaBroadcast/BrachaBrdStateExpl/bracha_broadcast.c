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

// For each process, values received by each process with each flag
int received_values[N][3][N][2] = {{{{0, 0}}}}; 

// This process id
int processId;

// For each process, value of the msg from the process that this process has accepted
int acceptedValue[N] = {0}; 

// Step in the algorithm for each process and value 
int step[N][2] = {{0, 0}};

sem_t *sem;
sem_t *sem_init_brd;

// How many different processes sent a certain value with a certain 
// tag to this process
int countDistinctProcessesForValue(int originProcess, int value, int tag)
{
    int count = 0;
    for (int i = 0; i < N; i++)
    {
        if (received_values[originProcess][tag][i][value] == 1)
        {
            count++;
        }
    }
    return count;
}

// Broadcast
void Bracha_broadcast(int originProcess, int value, int tag)
{
    int sockfd;
    struct sockaddr_in serverAddr;
    
    printf("Process %d: Start Bracha broadcast\n", processId);
    
    for (int i = 0; i < N; i++)
    {
        if (i == processId)
        {
            // BUG dont send to itself
            continue;
        }

        int message[5] = {originProcess, tag, processId, value, i}; // format [origin, tag, from, value, to] | tag 0:initial,1:echo,2:ready

        /*
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(PORT_BASE + i);
        inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
        
        
        if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1)
        {
            perror("[Process] Connect failure");
            exit(EXIT_FAILURE);
        }
        */

       sockfd = 1;
        

        send(sockfd, &message, sizeof(message), 0);

        //printf("Process %d Value %d sent to process %d with tag %d\n", processId, value, i, tag);

        //close(sockfd);
    }
}

// Process a received message
void processMessages(int originProcess, int value, int fromProcess, int tag)
{
    received_values[originProcess][tag][fromProcess][value] = 1;
    int distinctCount = countDistinctProcessesForValue(originProcess, value, tag);
    //printf("Process %d Value %d Tag %d distinct count: %d\n", processId, value, tag, distinctCount);
    
    if (step[originProcess][value] == 0 && ( countDistinctProcessesForValue(originProcess, value, 0) >= 1 
    || countDistinctProcessesForValue(originProcess, value, 1) >= (N+T)/2 + 1 
    || countDistinctProcessesForValue(originProcess, value, 2) >= T+1 )) {
        //printf("Process %d goes from step 0 to step 1 for value %d\n", processId, value);
        step[originProcess][value] = step[originProcess][value] + 1;
        Bracha_broadcast(originProcess, value, step[originProcess][value]);
    }
    if (step[originProcess][value] == 1 && ( countDistinctProcessesForValue(originProcess, value, 1) >= (N+T)/2 + 1
    || countDistinctProcessesForValue(originProcess, value, 2) >= T+1 )) {
        //printf("Process %d goes from step 1 to step 2 for value %d\n", processId, value);
        step[originProcess][value] = step[originProcess][value] + 1;
        Bracha_broadcast(originProcess, value, step[originProcess][value]);
    }
    if (step[originProcess][value] == 2 && countDistinctProcessesForValue(originProcess, value, 2) >= (2*T+1)) {
        //printf("Process %d goes from step 2 to step 3 for value %d (end)\n", processId, value);
        step[originProcess][value] = step[originProcess][value] + 1;
        // Accept value
        acceptedValue[originProcess] = value;
        //printf("###############################\n");
        //printf("Process %d: Value %d accepted from process %d.\n", processId, value, originProcess);
        //printf("###############################\n");
    }
    
    
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("Usage: %s <processId (0 to %d)> <initialValue (0 or 1)>\n", argv[0], N - 1);
        exit(1);
    }

    sem = sem_open("/sem_bracha_broadcast", O_CREAT, 0644, 0);
    if (sem == SEM_FAILED)
    {
        perror("[Process] Semaphore open failed");
        exit(1);
    }
    sem_init_brd = sem_open("/sem_bracha_broadcast_init_brd", O_CREAT, 0644, 0);
    if (sem_init_brd == SEM_FAILED)
    {
        perror("[Process] Semaphore open failed");
        exit(1);
    }

    processId = atoi(argv[1]);
    int initialValue = atoi(argv[2]);

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

    // Init accepted values
    for (int i = 0; i < N; i++) { // TODO in bracha consensus ?
        acceptedValue[i] = -1;
    }

    // If we put value -1 then the process just listens
    if (initialValue != -1) {
        // Broadcast the initial value = step 0
        Bracha_broadcast(processId, initialValue, 0);
    }

    sem_wait(sem_init_brd);
    printf("Process %d done waiting for broadcast init\n", processId);
    sem_close(sem_init_brd);
    
    int receivedMessage[5]; 

    // We perform the remaining 3 steps
    while (1)
    {
        /*
        connfd = accept(listenfd, (struct sockaddr *)&clientAddr, &addrLen);
        
        if (connfd == -1)
        {
            perror("[Process] Accept failure");
            exit(EXIT_FAILURE);
        } 
        */
        
        int nbytes = recv(listenfd, &receivedMessage, sizeof(receivedMessage), 0); // connfd-listenfd
        if (nbytes == -1)
        {
            perror("[Process] Recv failure");
            exit(EXIT_FAILURE);
        }
        
        int originProcess = receivedMessage[0];
        int tag = receivedMessage[1];
        int senderId = receivedMessage[2];
        int receivedValue = receivedMessage[3];
        int destinationId = receivedMessage[4];
        //printf("Process %d: Value %d received from process %d with tag %d\n", processId, receivedValue, senderId, tag);
        if (receivedValue >= 0)
        { // Ignore special signals
            processMessages(originProcess, receivedValue, senderId, tag);
            // This is where it registers its state to the controller 
            // TODO check if ok to do it here 
            int valuesCount[3][N][2]; // TODO cast to char * ???
            for (int t = 0; t < 3; t++) {
                for (int op = 0; op < N; op++) {
                    valuesCount[t][op][0] = countDistinctProcessesForValue(op, 0, t);
                    valuesCount[t][op][1] = countDistinctProcessesForValue(op, 1, t);
                }
            }
            send(-1, &valuesCount, sizeof(valuesCount), 0);
        }
        //close(connfd);
    }

    printf("END OF BROADCAST.\n");

    for (int i = 0; i < N; i++) {
        printf("Process %d accepted value %d from process %d\n", processId, acceptedValue[i], i);
    }


    return 0;
}