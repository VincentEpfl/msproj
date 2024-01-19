#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>

// Controller that spawns processes, intercepts communications between
// the processes, and explore the execution state

#define CONTROLLER_FEEDBACK_PATH "./controller_feedback_socket"
#define CONTROLLER_PATH "./controller_socket"
#define MAXMSG 256

#define N 5 // Total number of processes
#define T 1 // Maximum number of Byzantine processes
#define NUM_ROUNDS 1

#define SIZE_MSG_DELIVERED_BUF 500
#define SIZE_STATE_FORK_PATH 500
#define SIZE_MSG_BUF 30000

#define MAX_NUM_PROCESSES 10000
#define MAX_NUM_SYS_STATES 30000
#define MAX_FILE_DESCRIPTORS 1000

#define PROCESS_MESSAGE_SIZE 8
#define INSTRUCTION_MESSAGE_SIZE 6
#define SOCKET_QUEUE_CAPACITY 1000

#define SLEEP_DURATION_MS 10000
#define TIMEOUT_DURATION_S 0
#define TIMEOUT_DURATION_MS 10000

// ALGO CHG
// Message struct
// Messages received from the processes
// Format : [ type, round, tag, from, to, msg, connfd, forkId, echo, numDelivered, [delivered] ]
typedef struct
{
  int type;   // send:0 or recv:1
  int round;  // round where the message was sent
  int tag;  // tag of the message (AUX/EST)
  int from;   // id of process who sent this message
  int to;     // id of process the message was sent to
  int msg;    // message value
  int connfd; // file descriptor for the connection between the controller and the process
  int forkId; // id of the process that sent the message
  int echo;           // 0 if not an echo msg, 1 if echo msg
  int numDelivered;   // number of times the message was delivered
  int delivered[SIZE_MSG_DELIVERED_BUF]; // ids of the processes where the message was delivered
} Message;

// MessageTrace struct
// Minimal informations on the messages 
// Format : [ from, to, msg, no action, fork id ]
typedef struct
{
  int from;   // id of process who sent this message
  int to;     // id of process the message was sent to
  int msg;    // message value
  int forkId; // id of the process that sent the message
} MessageTrace;

// State struct
// Represents the state at one point of one execution of the algorithm
// Format : [ fork path len, [fork path], [[[[process values count]]]], killed ]
typedef struct
{
  int len;             // len of forkPath
  pid_t forkPath[SIZE_STATE_FORK_PATH]; 

// ALGO CHG
  // received value format :
  // { for each process :
  //   { at round :
  //    { tag (EST/AUX):
  //      {#0s recv from different processes, #1s recv from different processes},
  //    },
  //  },
  // }
  int valuesCount[N][NUM_ROUNDS][2][2];
  int decided_values[N]; // TODO need add a dimension for the rounds to check one shot property (trivial)
  int killed; // 1 if state was killed because redundant, 0 if not
} State;

sem_t *sem; // Semaphore to synchronize processes and allow the setup
sem_t *sem_init_brd; // Semaphore to synchronize processes and allow the setup

Message msgbuffer[SIZE_MSG_BUF]; // Array to store messages from the processes
MessageTrace msghistory[SIZE_MSG_BUF]; // Array to store minimal information on the messages
int nummsg = 0; // Number of messages in msghistory[]

pid_t processes[MAX_NUM_PROCESSES]; // Array to store running processes
int numProcesses = N; // Number of running processes in processes[]
pid_t current_process; // Id of the process currently running 
int current_process_index; // Index of the process currently running in the processes[] array

// State of the system: all the states of the different executions of the algorithm as it is 
// at one point 
State systemStates[MAX_NUM_SYS_STATES] = {
    {
        0,
        {0},
        {{{{0, 0}}}},
        {0},
        0},
};

int numStates = 1; // Number of states of the algorithm existing in parallel in systemStates[]
int numStatesKilled = 0; // Number of states killed because they were redundant

int sockfd; // File descriptor of the controller socket listening for messages from processes
int feedback_sockfd; // File descriptor of the controller socket listening for feedback from processes who received controller instruction

int fds[MAX_FILE_DESCRIPTORS]; // Array of all the open file descriptors
int numOpenFd = 0; // Number of open file descriptors in fds[]

// Controller signal handler 
void signal_handler(int signal) {
    // On keyboard interrupt
    // Clean up resources (processes, file descriptors, semaphores)
    // Exit
    if (signal == SIGINT) {
        printf("\nCaught Ctrl+C! Cleaning up and exiting...\n");
        
        for (int f = 0; f < numOpenFd; f++) {
          close(fds[f]);
        }
        for (int p = 0; p < numProcesses; p++) {
          kill(processes[p], SIGTERM);
        }

        close(sockfd);
        unlink(CONTROLLER_PATH);
        unlink(CONTROLLER_FEEDBACK_PATH);

        sem_close(sem);
        sem_unlink("/sem_bv_broadcast"); // Cleanup the semaphore

        sem_close(sem_init_brd);
        sem_unlink("/sem_bv_broadcast_init_brd"); // Cleanup the semaphore

        exit(EXIT_SUCCESS);
    }
}

// Get the states that are potentially affected by a message
int get_states_to_update(int *res, int *statesToUpdate, int recv_msg_index)
{
  int numStatesToUpdate = 0;
  int posInForkPath = 0;

  for (int s = 0; s < numStates; s++)
  {
    // If a state is killed, it is not affected by any message
    if (systemStates[s].killed == 1)
    {
      continue;
    }

    if (systemStates[s].len == 0)
    { 
      systemStates[s].len = 1;
      systemStates[s].forkPath[0] = 0;
    }
    // Otherwise, it is affected if the process id of the process receiving
    // the message is in the path of the state
    for (int f = 0; f < systemStates[s].len; f++)
    {
      if (systemStates[s].forkPath[f] == msgbuffer[recv_msg_index].forkId)
      {
        statesToUpdate[numStatesToUpdate++] = s;
        posInForkPath = f;
        break;
      }
    }
  }

  // If no state is affected, then this message doesn't do anything,
  // look at the next message
  if (numStatesToUpdate == 0)
  {
    return -1;
  }

  res[0] = numStatesToUpdate;
  res[1] = posInForkPath;
  return 0;
}

// ALGO CHG
// Put the message received over the socket in the message buffer
void put_msg_in_buffer(int index, int *receivedMessage)
{
  msgbuffer[index].type = receivedMessage[0];
  msgbuffer[index].round = receivedMessage[2];
  msgbuffer[index].tag = receivedMessage[1]; 
  msgbuffer[index].from = receivedMessage[3];
  msgbuffer[index].to = receivedMessage[5];
  msgbuffer[index].msg = receivedMessage[4];
  msgbuffer[index].connfd = -1;
  msgbuffer[index].forkId = receivedMessage[6];
  msgbuffer[index].echo = receivedMessage[7];
  msgbuffer[index].numDelivered = 0;
  for (int d = 0; d < SIZE_MSG_DELIVERED_BUF; d++)
  {
    msgbuffer[index].delivered[d] = 0;
  }
}

// Initialize the controller sockets 
int initSocket(bool feedback)
{
  // The socket to receive messages has a timeout, the socket to receive feedback hasn't 
  bool timeout = !feedback;
  struct sockaddr_un address;
  int sockfd;

  if (feedback)
  {
    printf("[Controller] Create feedback socket\n");
  }
  else
  {
    printf("[Controller] Create controller socket\n");
  }

  if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Controller] receive socket");
    exit(EXIT_FAILURE);
  }

  if (timeout)
  {
    struct timeval tv; // timeval structure to set the timeout

    // Set the timeout value
    tv.tv_sec = TIMEOUT_DURATION_S;       // 0 seconds
    tv.tv_usec = TIMEOUT_DURATION_MS; // 10000 microseconds 

    // Set the timeout option
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv))
    {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;

  if (feedback)
  {
    strncpy(address.sun_path, CONTROLLER_FEEDBACK_PATH, sizeof(address.sun_path) - 1);
    unlink(CONTROLLER_FEEDBACK_PATH);
  }
  else
  {
    strncpy(address.sun_path, CONTROLLER_PATH, sizeof(address.sun_path) - 1);
    unlink(CONTROLLER_PATH);
  }

  if (bind(sockfd, (struct sockaddr *)&address, sizeof(address)) == -1)
  {
    perror("[Controller]  bind");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  if (listen(sockfd, SOCKET_QUEUE_CAPACITY) == -1) 
  {
    perror("[Controller] listen");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  return sockfd;
}

// Spawn processes and execute the algorithm
void spawnProcesses()
{
  for (int i = 0; i < N; i++)
  {
    if ((processes[i] = fork()) == 0)
    {
      char processIdStr[10], initialValueStr[10];

      // Process id
      sprintf(processIdStr, "%d", i);

      // Process value
      if (i < 2)
      {
        sprintf(initialValueStr, "%d", 1);
      }
      else
      {
        sprintf(initialValueStr, "%d", 0);
      }

      // Replace child process with bv_consensus process
      setenv("LD_PRELOAD", "./redirect.so", 1);
      // ALGO CHG
      execl("./bv_consensus", "bv_consensus", processIdStr, initialValueStr, (char *)NULL);
      perror("execl failed");
      exit(EXIT_FAILURE); // Exit if execl fails
    }
  }

  // Wait until all processes have setup their sockets
  sleep(2);

  // Signal all children to proceed, but only schedule 1 to continue running
  for (int i = 0; i < N; i++)
  {
    if (i == 0)
    {
      sem_post(sem);
      // printf("[Controller] Schedule process 0\n");
      // current_process = processes[0];
      // current_process_index = 0;
    }
    else
    {
      sem_post(sem);
      // kill(processes[i], SIGSTOP);
    }
  }

  // Wait until all processes have done init broadcast TEMPORARY
  sleep(5);

  // Signal all children to proceed, but only schedule 1 to continue running
  for (int i = 0; i < N; i++)
  {
    if (i == 0)
    {
      sem_post(sem_init_brd);
      printf("[Controller] Schedule process 0\n");
      current_process = processes[0];
      current_process_index = 0;
    }
    else
    {
      sem_post(sem_init_brd);
      kill(processes[i], SIGSTOP);
    }
  }
}

// Setup semaphores, sockets, processes
void init()
{
  systemStates[0].len = 1;
  systemStates[0].forkPath[0] = 0;

// ALGO CHG
  // Create the semaphore
  sem = sem_open("/sem_bv_consensus", O_CREAT, 0644, 0);
  if (sem == SEM_FAILED)
  {
    if (errno == EEXIST)
    {
      // Semaphore already exists, try to unlink and create again
      printf("Semaphore already exists, trying to recreate it.\n");
      if (sem_unlink("/sem_bv_consensus") == -1)
      {
        perror("Error unlinking semaphore");
        exit(EXIT_FAILURE);
      }
      sem = sem_open("/sem_bv_consensus", O_CREAT, 0644, 1);
      if (sem == SEM_FAILED)
      {
        perror("Error creating semaphore after unlinking");
        exit(EXIT_FAILURE);
      }
    }
    else
    {
      // Some other error occurred
      perror("Error creating semaphore");
      exit(EXIT_FAILURE);
    }
  }

  // Create the semaphore init brd
  sem_init_brd = sem_open("/sem_bv_consensus_init_brd", O_CREAT, 0644, 0);
  if (sem_init_brd == SEM_FAILED)
  {
    if (errno == EEXIST)
    {
      // Semaphore already exists, try to unlink and create again
      printf("Semaphore already exists, trying to recreate it.\n");
      if (sem_unlink("/sem_bv_consensus_init_brd") == -1)
      {
        perror("Error unlinking semaphore");
        exit(EXIT_FAILURE);
      }
      sem_init_brd = sem_open("/sem_bv_consensus_init_brd", O_CREAT, 0644, 1);
      if (sem == SEM_FAILED)
      {
        perror("Error creating semaphore after unlinking");
        exit(EXIT_FAILURE);
      }
    }
    else
    {
      // Some other error occurred
      perror("Error creating semaphore");
      exit(EXIT_FAILURE);
    }
  }

  // Create controller socket to intercept processes communication
  sockfd = initSocket(false);

  // Create feedback socket to receive state from processes resulting
  // from controller instructions
  feedback_sockfd = initSocket(true);

  // Spawn processes to execute algorithm
  spawnProcesses();
}

// Schedule the next process to run
// Only 1 process runs at a time
void schedule_new_process()
{
  kill(current_process, SIGSTOP);
  current_process_index = (current_process_index + 1) % numProcesses;
  current_process = processes[current_process_index];
  kill(current_process, SIGCONT);
  usleep(SLEEP_DURATION_MS);
  // printf("[Controller] scheduling process %d on forkId %d\n", current_process_index, current_process);
}

// ALGO CHG
// Compares 2 states of the algorithm
bool compareState(int state1[N][NUM_ROUNDS][2][2], int state2[N][NUM_ROUNDS][2][2])
{
  for (int p = 0; p < N; p++) {
    for (int r = 0; r < NUM_ROUNDS; r++) { 
      for (int t = 0; t < 2; t++) {
          for (int v = 0; v < 2; v++)
          {
            if (state1[p][r][t][v] != state2[p][r][t][v])
            {
              return false;
            }
          }
        }
      }
    }
  
  return true;
}

// ALGO CHG
// Compares the state of 2 processes
bool compareProcessState(int processState1[NUM_ROUNDS][2][2], int processState2[NUM_ROUNDS][2][2])
{
  for (int r = 0; r < NUM_ROUNDS; r++) { 
    for (int t = 0; t < 2; t++) {
        for (int v = 0; v < 2; v++)
        {
          if (processState1[r][t][v] != processState2[r][t][v])
          {
            return false;
          }
        }
      
    }
  }
  return true;
}

// Common "random" number generator
int randomBit(int r) {
    if (r % 2 == 0) {
        return 1;
    } else {
        return 0;
    }
}

// ALGO CHG
// TODO check properties
// Checks if the state of the algorithm is valid
bool checkStateValid(int state[N][NUM_ROUNDS][2][2], int decided_values[N])
{
  /*
  bool valid = true;
  int decided_values[N];
  for (int p = 0; p < N; p++) {
    decided_values[p] = -1;
  }
  for (int p = 0; p < N; p++) {
    bool decided = false;
    for (int r = 0; r < 2; r++) {
      int values[2] = {0, 0};
      if (state[0][r][1][0] >= N - T - 1 && state[0][r][0][0] > 2*T - 1) {
        values[0] = 1;
      }
      if (state[0][r][1][1] >= N - T - 1 && state[0][r][0][1] > 2*T - 1) {
        values[1] = 1;
      }

      int s = randomBit(r);
      if (values[s]) {
        if (!decided) {
          decided = true;
          decided_values[p] = s;
          printf("Process %d decided value %d\n", p, s);
        }
      }
    }
  }

  if (!(decided_values[0] == 0 || decided_values[0] == 1)) {
    printf("Process 0 didn't decide\n");
    valid = false;
  }
  
  for (int p = 1; p < N; p++) { 
    if (p == 1) { // p1 byzantine
      continue;
    }
    if (decided_values[p] != decided_values[0]) {
      valid = false;
    }
  }

  return valid;
  */

 bool valid = true;

for (int p = 0; p < N; p++) {
    printf("Process %d decided %d\n", p, decided_values[p]);
  }

 if (!(decided_values[0] == 0 || decided_values[0] == 1)) {
    printf("Process 0 didn't decide\n");
    valid = false;
  }

 for (int p = 1; p < N; p++) { 
    if (p == 1) { // p1 byzantine
      continue;
    }
    if (decided_values[p] != decided_values[0]) {
      valid = false;
    }
  }

  //return valid; TODO rollback
  return true;


}

bool checkAllStates()
{
  bool invalid = false;
  for (int s = 0; s < numStates; s++)
  {
    if (systemStates[s].killed == 1)
    {
      continue;
    }
    printf("Check state %d\n", s);

    if (!checkStateValid(systemStates[s].valuesCount, systemStates[s].decided_values))
    {
      invalid = true;
      // maybe shut down everything, every process etc
      printf("[Controller] INVALID STATE FOUND\n");
      printf("State %d:\n", s);
      printf("forkPath: ");
      for (int f = 0; f < systemStates[s].len; f++)
      {
        printf("%d/", systemStates[s].forkPath[f]);
      }
      printf("\n");
      // ALGO CHG
      printf("values count: \n");
      for (int p = 0; p < N; p++)
      {
        printf("process %d : {\n", p);
        for (int r = 0; r < NUM_ROUNDS; r++) { 
          printf("round %d : {\n", r);
          for (int t = 0; t < 2; t++) {
            printf("tag %d : {", t);
              for (int v = 0; v < 2; v++)
              {
                printf("%d, ", systemStates[s].valuesCount[p][r][t][v]);
              }
            printf("}\n");
          }
          printf("}\n");
        }
        printf("}\n");
      }
      // TODO print messages ? Would be a lot... maybe print in a log file ... 
    }
  }
  if (!invalid)
  {
    printf("[Controller] NO INVALID STATE FOUND\n");
  }
}

// Deliver a message to another message
void deliver_message(int delivered, int to)
{
  msgbuffer[delivered].delivered[msgbuffer[delivered].numDelivered] = msgbuffer[to].forkId;
  msgbuffer[delivered].numDelivered = msgbuffer[delivered].numDelivered + 1;
}

// Deliver a message to a process id
void deliver_message_forkid(int delivered, int forkid)
{
  msgbuffer[delivered].delivered[msgbuffer[delivered].numDelivered] = forkid;
  msgbuffer[delivered].numDelivered = msgbuffer[delivered].numDelivered + 1;
}

// ALGO CHG
// Print a message
void printMessage(int index)
{
  printf("msg:[t:%d, round:%d, tag:%d, from:%d, to:%d, value:%d, connfd:%d, forkId:%d, numDelivered:%d]\n",
         msgbuffer[index].type, msgbuffer[index].round, msgbuffer[index].tag, msgbuffer[index].from, msgbuffer[index].to, msgbuffer[index].msg, msgbuffer[index].connfd,
         msgbuffer[index].forkId, msgbuffer[index].numDelivered);
  if (msgbuffer[index].numDelivered > 0)
  {
    printf("delivered:[");
    for (int d = 0; d < msgbuffer[index].numDelivered; d++)
    {
      printf("%d, ", msgbuffer[index].delivered[d]);
    }
    printf("]\n");
  }
}

// Check if this state is affected by those messages
bool canDeliverState(int posInForkPath, int stateToUpdate, int sendIndex, int recvIndex)
{
  // Check if the message comes from a parallel execution/state
  bool forkOk = true;
  if (numStates > 1) 
  {
    forkOk = false;
    for (int f = 0; f < systemStates[stateToUpdate].len; f++) 
    {
      // printf("[CONTROLLER TEST] state fork %d / send msg fork %d\n", systemStates[statesToUpdate[0]].forkPath[f], msgbuffer[sendIndex].forkId);
      if (systemStates[stateToUpdate].forkPath[f] == msgbuffer[sendIndex].forkId)
      {
        forkOk = true;
        break;
      }
    }
  }

  // Check if the send message was already delivered to this state
  bool sendDeliverOk = true;

  if (msgbuffer[sendIndex].numDelivered > 0)
  {

    for (int f = 0; f < msgbuffer[sendIndex].numDelivered; f++)
    {
      for (int g = 0; g < posInForkPath + 1; g++) // pos in fork path -> all ? (no need forkpath...)
      {
        // printf("[CONTROLLER TEST] send msg delivered %d\n", msgbuffer[sendIndex].delivered[f]);
        if (msgbuffer[sendIndex].delivered[f] == systemStates[stateToUpdate].forkPath[g])
        {
          sendDeliverOk = false;
          break;
        }
      }
    }
  }
  return sendDeliverOk && forkOk;
}

// Check if the message comes from a parallel execution/state
bool canDeliverForkState(int stateToUpdate, int sendIndex)
{
  bool forkOk = true;
  if (numStates > 1)
  {
    forkOk = false;
    for (int f = 0; f < systemStates[stateToUpdate].len; f++) 
    {
      // printf("[CONTROLLER TEST] state fork %d / send msg fork %d\n", systemStates[statesToUpdate[0]].forkPath[f], msgbuffer[sendIndex].forkId);
      if (systemStates[stateToUpdate].forkPath[f] == msgbuffer[sendIndex].forkId)
      {
        forkOk = true;
        break;
      }
    }
  }

  return forkOk;
}

// Check if the recv message was already delivered to this state
bool canDeliverRecvState(int stateToUpdate, int recvIndex)
{
  bool recvDeliver = true;
  if (msgbuffer[recvIndex].numDelivered > 0)
  {
    for (int f = 0; f < msgbuffer[recvIndex].numDelivered; f++)
    {
      for (int g = 0; g < systemStates[stateToUpdate].len; g++) 
      {
        if (msgbuffer[recvIndex].delivered[f] == systemStates[stateToUpdate].forkPath[g])
        {
          recvDeliver = false;
          break;
        }
      }
    }
  }
  return recvDeliver;
}

// Check if the send message was already delivered to this state
bool canDeliverSendState(int stateToUpdate, int sendIndex)
{
  bool sendDeliver = true;
  
  if (msgbuffer[sendIndex].numDelivered > 0)
  {
    for (int f = 0; f < msgbuffer[sendIndex].numDelivered; f++)
    {
      for (int g = 0; g < systemStates[stateToUpdate].len; g++) 
      {
        if (msgbuffer[sendIndex].delivered[f] == systemStates[stateToUpdate].forkPath[g])
        {
          sendDeliver = false;
          break;
        }
      }
    }
  }
  
  return sendDeliver;
}

// Send controller instruction to a process
void sendMsgToProcess(int connfd, const void *message, int msglen, void *recmsg, int recmsglen)
{
  // Send message
  int *messageint = (int *)message;
  // printf("[Controller] Send msg %d %d %d\n", messageint[0], messageint[1], messageint[2]);
  if (send(connfd, message, msglen, 0) == -1)
  {
    perror("[Controller] send failure");
    exit(EXIT_FAILURE);
  }

  // Recover the resulting state
  // format [forkid, processState]

  int feedback_connfd;
  if ((feedback_connfd = accept(feedback_sockfd, NULL, NULL)) < 0)
  {
    //close(feedback_connfd); TODO necessary ?
    perror("[Controller] accept failure");
    exit(EXIT_FAILURE);
  }
  else
  {
    int nb = recv(feedback_connfd, recmsg, recmsglen, 0);
    if (nb < 0)
    {
      perror("[Controller] recv state feedback socket failure");
      exit(EXIT_FAILURE);
    } else if (nb == 0) {
      printf("[Controller] ERROR FEEDBACK RECEIVE NOTHING\n");
    }
    close(feedback_connfd);
  }
}

// ALGO CHG
// Send controller instruction to a process
// Recover the state of this process resulting from this instruction
int sendMsgAndRecvState(int connfd, const void *message, int msglen, int send_msg_index, void *newProcessState, void *forkInfo)
{
  int forkId;
  int decided_value;
  int msg[NUM_ROUNDS][2][2];
  char recmsg[sizeof(forkId) + sizeof(decided_value) + sizeof(msg)];
  sendMsgToProcess(connfd, message, msglen, &recmsg, sizeof(recmsg));

  //printf("[Controller] state recovered\n");

  // Recover state
  memcpy(&forkId, recmsg, sizeof(int));
  memcpy(&decided_value, recmsg + sizeof(int), sizeof(int));
  memcpy(newProcessState, recmsg + sizeof(int) + sizeof(int), sizeof(msg));
  
  // Controller instruction spawns a new process
  // Recover process id and index in processes[]
  int *forkInfoInt = (int *)forkInfo;
  forkInfoInt[0] = forkId;
  forkInfoInt[1] = numProcesses;

  // Update processes[]
  processes[numProcesses++] = forkInfoInt[0];

  kill(forkInfoInt[0], SIGSTOP);

  return decided_value;

  //printf("[Controller] process %d state is now {%d, %d} in forkid %d\n", msgbuffer[send_msg_index].to, newProcessStateInt[0], newProcessStateInt[1], forkInfoInt[0]);
}

// ALGO CHG
// Copies the state into a new state object in the array of states
void duplicateState(int originState, int destState)
{
  for (int p = 0; p < N; p++)
  {
    for (int r = 0; r < NUM_ROUNDS; r++) {
      for (int t = 0; t < 2; t++) {
          for (int v = 0; v < 2; v++)
          {
            systemStates[destState].valuesCount[p][r][t][v] = systemStates[originState].valuesCount[p][r][t][v];
          }
      }
    }
  }

  for (int k = 0; k < systemStates[originState].len; k++)
  {
    systemStates[destState].forkPath[k] = systemStates[originState].forkPath[k];
  }

  systemStates[destState].len = systemStates[originState].len;

  for (int p = 0; p < N; p++) {
    systemStates[destState].decided_values[p] = systemStates[originState].decided_values[p];
  }
  
}

// AGLO CHG
// Update a state with the new state of a process
void updateState(int stateToUpdate, int forkid, int newProcessState[][2][2], int updatedProcess, int decided_value)
{
  systemStates[stateToUpdate].forkPath[systemStates[stateToUpdate].len] = forkid;
  systemStates[stateToUpdate].len = systemStates[stateToUpdate].len + 1;
  for (int r = 0; r < NUM_ROUNDS; r++) { 
    for (int t = 0; t < 2; t++) {
        for (int v = 0; v < 2; v++)
        {
          systemStates[stateToUpdate].valuesCount[updatedProcess][r][t][v] = newProcessState[r][t][v];
        }
    }
  }

  systemStates[stateToUpdate].decided_values[updatedProcess] = decided_value;
}

// Kill redundant states: all states equal to a reference state
// TODO killHandle and forkid_killed, even numStates NO NEED
bool killStateAlreadyThere(int state, int numStates, int killHandle, bool forkid_killed)
{
  bool forkid_killed_temp = forkid_killed;
  for (int z = 0; z < numStates; z++)
  {
    if (z == state || systemStates[z].killed == 1)
    {
      continue;
    }
    if (compareState(systemStates[z].valuesCount, systemStates[state].valuesCount))
    {

      if (!forkid_killed_temp) // TODO I think no need anymore
      {
        forkid_killed_temp = true;
      }

      systemStates[z].killed = 1;
      numStatesKilled = numStatesKilled + 1;
    }
  }
  return forkid_killed_temp;
}

// Print the state of the controller: systemStates[]: all the states of the different executions of the algorithm
// TODO no need arguments
void printControllerState(State *systemStates, int numStates)
{
  printf("[Controller] Print Controller State :\n");
  for (int s = 0; s < numStates; s++)
  {
    if (systemStates[s].killed == 1)
    {
      continue;
    }
    printf("[Controller] State %d:\n", s);
    printf("[Controller] forkPath: ");
    for (int f = 0; f < systemStates[s].len; f++)
    {
      printf("%d/", systemStates[s].forkPath[f]);
    }
    printf("\n");
    printf("[Controller] values count: \n");
    // ALGO CHG
    for (int p = 0; p < N; p++)
      {
        printf("process %d : {\n", p);
        for (int r = 0; r < NUM_ROUNDS; r++) { 
          printf("round %d : {\n", r);
          for (int t = 0; t < 2; t++) {
            printf("tag %d : {", t);
              for (int v = 0; v < 2; v++)
              {
                printf("%d, ", systemStates[s].valuesCount[p][r][t][v]);
              }
            printf("}\n");
          }
          printf("}\n");
        }
        printf("}\n");
      }
    printf("[Controller] messages exchanged: \n");
    for (int f = 0; f < systemStates[s].len; f++)
    {
      for (int g = 0; g < nummsg; g++) {
        if (msghistory[g].forkId == systemStates[s].forkPath[f]) {
          printf("Message %d ", msghistory[g].msg);
          printf("from %d ", msghistory[g].from);
          printf("to %d\n", msghistory[g].to);
        }
      }
      
    }

    for (int p = 0; p < N; p++) {
      printf("[Controller] messages sent by process %d : { \n", p);
      for (int f = 0; f < systemStates[s].len; f++) {
        for (int g = 0; g < nummsg; g++) {
          if (msghistory[g].forkId == systemStates[s].forkPath[f] && msghistory[g].from == p) {
            printf("value %d to process %d\n", msghistory[g].msg, msghistory[g].to);
          }
        }
      }
      printf("}\n");
    }

    for (int p = 0; p < N; p++) {
      printf("[Controller] messages received by process %d : { \n", p);
      for (int f = 0; f < systemStates[s].len; f++) {
        for (int g = 0; g < nummsg; g++) {
          if (msghistory[g].forkId == systemStates[s].forkPath[f] && msghistory[g].to == p) {
            printf("value %d from process %d\n", msghistory[g].msg, msghistory[g].from);
          }
        }
      }
      printf("}\n");
    }

  }
}

// Add a message's minimal informations to msghistory[] 
void addMsgToHistory(int forkid, int from, int to, int val) {
        msghistory[nummsg].forkId = forkid;
        msghistory[nummsg].from = from;
        msghistory[nummsg].to = to;
        msghistory[nummsg].msg = val;
        nummsg = nummsg + 1;
}

// Process a pair of messages
// Match send - recv messages with corresponding source - destination 
// Identify affected states
// Send instructions to process
// Update states as a result
// Kill redundant states
int handleMessagePair(int recvIndex, int sendIndex, int fd, bool recv)
{
  // Select the right file descriptor to send instruction to the process
  int connfd = 0;
  if (recv) {
    connfd = fd;
  } else {
    connfd = msgbuffer[recvIndex].connfd;
  }

  // Check if the 2 messages are send - recv messages with corresponding source - destination 
  if (!(msgbuffer[sendIndex].type == 0 && msgbuffer[recvIndex].type == 1 && msgbuffer[sendIndex].to == msgbuffer[recvIndex].to)) {
    return -1; // continue
  }

  // Identify affected states
  int statesToUpdateTemp[numStates];
  int res[2];
  if (get_states_to_update(res, statesToUpdateTemp, recvIndex) == -1)
  {
    return -1; // continue;
  }
  int numStatesToUpdateTemp = res[0];
  int posInForkPath = res[1];

  int statesToUpdate[numStatesToUpdateTemp];
  int numStatesToUpdate = 0;

  int statesNoAction[numStatesToUpdateTemp];
  int numStatesNoAction = 0;

  for (int s = 0; s < numStatesToUpdateTemp; s++)
  {
    if (canDeliverRecvState(statesToUpdateTemp[s], recvIndex) && canDeliverSendState(statesToUpdateTemp[s], sendIndex) && canDeliverForkState(statesToUpdateTemp[s], sendIndex)) 
    {
      statesToUpdate[numStatesToUpdate++] = statesToUpdateTemp[s];
    }
    else
    { 
      statesNoAction[numStatesNoAction++] = statesToUpdateTemp[s];
    }
  }

  // TODO if a pair of message affect 0 state at some point
  // Then it will never affect any state at a later point ?
  // Then could mark those to speed up the elimination process
  if (numStatesToUpdate == 0) {
    return -1; // continue
  }

  // deliver send msg fork id in recv msg
  deliver_message(recvIndex, sendIndex); 

  // it's possible the process currently running didn't send this recv msg
  kill(current_process, SIGSTOP); 

    // schedule the process that sent the recv message and is waiting for controller instructions
    if (msgbuffer[recvIndex].forkId == 0)
    {
      current_process_index = msgbuffer[recvIndex].to;
      current_process = processes[current_process_index];
    }
    else
    {
      current_process = msgbuffer[recvIndex].forkId;
      for (int p = 0; p < numProcesses; p++)
      {
        if (processes[p] == current_process)
        {
          current_process_index = p;
          break;
        }
      }
    }
    kill(current_process, SIGCONT);
    usleep(SLEEP_DURATION_MS);
    // printf("[Controller] Schedule process %d on forkId %d to send instructions\n", current_process_index, current_process);

    // #############################################################################################################################
    // Only keep because similar if I want a byz process to not send any message
    // But I dont use it now
    int newProcessStateNoAction[NUM_ROUNDS][2][2]; // ALGO CHG
    int forkInfoNoAction[2];
    int forkidNoAction;
    int forkidNoAction_index;
    if (false)
    {
     // ALGO CHG
      int messageNoAction[INSTRUCTION_MESSAGE_SIZE] = {3, msgbuffer[sendIndex].tag, msgbuffer[sendIndex].round, msgbuffer[sendIndex].from, msgbuffer[sendIndex].msg, msgbuffer[sendIndex].to};
      int decided_value = sendMsgAndRecvState(connfd, &messageNoAction, sizeof(messageNoAction), sendIndex, &newProcessStateNoAction, &forkInfoNoAction);
      forkidNoAction = forkInfoNoAction[0];
      forkidNoAction_index = forkInfoNoAction[1];

      // here no need to update msghistory because for this id no message was received ?
      addMsgToHistory(forkidNoAction, msgbuffer[sendIndex].from, msgbuffer[sendIndex].to, msgbuffer[sendIndex].msg);

      // Update the system states This doesnt act on the same state than the rest so should compose fine
      for (int s = 0; s < numStatesNoAction; s++)
      {
        // just update forkpath and len
        updateState(statesNoAction[s], forkidNoAction, newProcessStateNoAction, msgbuffer[recvIndex].to, decided_value);
        // actual state should not change so no need to kill
      }

      deliver_message_forkid(sendIndex, forkidNoAction); // deliver forkids in send msg
    }
    // #############################################################################################################################

    // Send the message
    int newProcessState[NUM_ROUNDS][2][2]; // ALGO CHG
    int forkInfo[2];
    // ALGO CHG
    int message[INSTRUCTION_MESSAGE_SIZE] = {1, msgbuffer[sendIndex].tag, msgbuffer[sendIndex].round, msgbuffer[sendIndex].from, msgbuffer[sendIndex].msg, msgbuffer[sendIndex].to};
    int decided_value = sendMsgAndRecvState(connfd, &message, sizeof(message), sendIndex, &newProcessState, &forkInfo);
    int forkid0 = forkInfo[0];
    int forkid0_index = forkInfo[1];

    // add msg to history
    addMsgToHistory(forkid0, msgbuffer[sendIndex].from, msgbuffer[sendIndex].to, msgbuffer[sendIndex].msg);

    // deliver id of process resulting from instruction in send msg
    deliver_message_forkid(sendIndex, forkid0); 

    // STATE EXPLORATION CONDITION
    if (msgbuffer[sendIndex].from == 3 && msgbuffer[sendIndex].round == 0) // msgbuffer[sendIndex].from == 2  msgbuffer[sendIndex].from == 3
    {
      // Send message with the opposite value
      //printf("[Controller] send opposite msg to receiver\n");
      int opValue = 1 - msgbuffer[sendIndex].msg;
      // ALGO CHG TODO
      int messageOp[INSTRUCTION_MESSAGE_SIZE] = {1, msgbuffer[sendIndex].tag, msgbuffer[sendIndex].round, msgbuffer[sendIndex].from, opValue, msgbuffer[sendIndex].to};
      int newProcessStateOp[NUM_ROUNDS][2][2]; // ALGO CHG
      int forkInfoOp[2];
      int decided_value_op = sendMsgAndRecvState(connfd, &messageOp, sizeof(messageOp), sendIndex, &newProcessStateOp, &forkInfoOp);
      int forkid1 = forkInfoOp[0];
      int forkid1_index = forkInfoOp[1];

      // add op msg to history
      addMsgToHistory(forkid1, msgbuffer[sendIndex].from, msgbuffer[sendIndex].to, opValue);

      // deliver id of process resulting from instruction in send msg
      deliver_message_forkid(sendIndex, forkid1); 

      // If both values lead the process to the same state, then we already know there is redundant state
      // Continue only 1 path
      if (compareProcessState(newProcessState, newProcessStateOp))
      {
        // Here I consider that I kill forkid1 by default

        //printf("[Controller] Same result: kill a child\n");
        // ALGO CHG 
        int killMessage[INSTRUCTION_MESSAGE_SIZE] = {2, -1, -1, -1, -1, -1};
        if (send(connfd, &killMessage, sizeof(killMessage), 0) == -1)
        {
          perror("[Controller] send fail");
          exit(EXIT_FAILURE);
        }

        
        // Update the system states
        for (int s = 0; s < numStatesToUpdate; s++)
        {
          updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[recvIndex].to, decided_value);
        }

        // Kill redundant states
        bool forkid0_killed = false;
        for (int s = 0; s < numStatesToUpdate; s++)
        {
          forkid0_killed = killStateAlreadyThere(statesToUpdate[s], numStates, forkid0, forkid0_killed);
        }

        // "delete" forkid1
        numProcesses = numProcesses - 1;
        processes[numProcesses] = -1; 

        if (forkid0_killed)
        {
          schedule_new_process();
        }
        else // TODO both equivalent, no need if - else
        { 
          kill(current_process, SIGSTOP);
          current_process = forkid0;
          current_process_index = forkid0_index;
          kill(forkid0, SIGCONT);
          usleep(SLEEP_DURATION_MS);
          // printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
        }
      }
      else // Values lead the process to different states, more paths to explore
      {
        // Copy sys state to update in 1 new state for each fork
        for (int s = 0; s < numStatesToUpdate; s++)
        {

          // Copies the state to update into a new state object in the array of states
          duplicateState(statesToUpdate[s], numStates);

          // Update the states
          updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[recvIndex].to, decided_value);
          updateState(numStates, forkid1, newProcessStateOp, msgbuffer[recvIndex].to, decided_value_op);

          numStates = numStates + 1;
        }

        // Kill redundant states
        bool forkid0_killed = false;
        for (int s = 0; s < numStatesToUpdate; s++)
        {
          forkid0_killed = killStateAlreadyThere(statesToUpdate[s], numStates, forkid0, forkid0_killed);
        }

        bool forkid1_killed = false;
        for (int s = 0; s < numStatesToUpdate; s++)
        {
          forkid1_killed = killStateAlreadyThere(numStates - 1, numStates, forkid1, forkid1_killed);
        }

        schedule_new_process();
      }
    }
    else // No state exploration
    { 
      // In this case we only transmit the message with its actual value

      // Update the system states
      for (int s = 0; s < numStatesToUpdate; s++)
      {
        updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[recvIndex].to, decided_value);
      }

      // Kill redundant states
      bool forkid0_killed = false;
      for (int s = 0; s < numStatesToUpdate; s++)
      {
        forkid0_killed = killStateAlreadyThere(statesToUpdate[s], numStates, forkid0, forkid0_killed);
      }

      schedule_new_process();
    }
    //printControllerState(systemStates, numStates);
    return 1; // msg was delivered 
}

// Controller main function
int main()
{

  // TODO put that in init

  for (int s = 0; s < MAX_NUM_SYS_STATES; s++) {
    for (int p = 0; p < N; p++) {
      systemStates[s].decided_values[p] = -1;
    }
    
  }

    // Start timer to measure execution time
    clock_t start = clock();
    clock_t nothDelTime;
    clock_t noNewCoTime;

    // Setup signal handler
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0; // or SA_RESTART to restart system calls
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

  init();

  // Events that make the simulation progress = messages between processes
  // = messages intercepted by controller
  // Wait for messages to advance execution
  // On msg received : stop process that sent, exec/continue process that receives
  // Send instructions, update states
  // Schedule another process to continue running

  // TODO format : []
  int receivedMessage[PROCESS_MESSAGE_SIZE];
  int connfd;
  int i = 0;
  int noNewConnection = 0;
  int nothingDelivered = 0;
  printf("[Controller] Listen for incoming messages\n");
  // Main loop of the controller
  // Listening for messages from processes
  while (1)
  {
    // End, exit loop
    if (nothingDelivered > 150)
    {
      break;
    }
    if ((connfd = accept(sockfd, NULL, NULL)) < 0)
    {
      // Error was due to a timeout
      if (errno == EWOULDBLOCK || errno == EAGAIN)
      {
        // printf("[Controller] No connections within the timeout period.\n");
        usleep(SLEEP_DURATION_MS);
        schedule_new_process();
        printf("[Controller] ACCEPT TIMEOUT. Try process %d of %d\n", current_process_index, numProcesses);

        // It might be the end, set timer
        if (noNewConnection == 0) {
          noNewCoTime = clock();
        }

        noNewConnection = noNewConnection + 1;

        // End, exit loop
        if (noNewConnection > 2*numProcesses) 
        {
          break;
        }
      }
      else
      {
        // An error occurred that wasn't a timeout
        perror("[Controller] Accept failure");
        exit(EXIT_FAILURE);
      }
    }
    else // Connection established
    {
      printf("[Controller] NUM PROCESSES : %d\n", numProcesses);
      fds[numOpenFd] = connfd;
      numOpenFd++;
      printf("[Controller] NUM FD : %d\n", numOpenFd);

      // Set timeout on the new connection 

      struct timeval tmv; // timeval structure to set the timeout

      // Set the timeout value
      tmv.tv_sec = TIMEOUT_DURATION_S;       // 0 seconds 
      tmv.tv_usec = TIMEOUT_DURATION_MS; // 10000 microseconds

      // Set the timeout option
      if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmv, sizeof(tmv)) < 0)
      {
        perror("setsockopt");
        exit(EXIT_FAILURE);
      }


      printf("[Controller] New connection\n");
      noNewConnection = 0;

      int nothingToRecv = 0;
      while (1){ 
        ssize_t len = recv(connfd, &receivedMessage, sizeof(receivedMessage), 0);

        if (len < 0) {
          // Error due to timeout, try again
          if (errno == EWOULDBLOCK || errno == EAGAIN)
          {
            schedule_new_process();
            printf("[Controller] RECV TIMEOUT. Try process %d of %d\n", current_process_index, numProcesses);
            continue;
          } else {
            // Error that wasn't a timeout
            perror("[Controller] Recv failure");
            exit(EXIT_FAILURE);
          }
        } else if (len == 0) {
          // Connection closed, try to accept a new connection
          printf("[Controller] NOTHING MORE TO RECV\n");
          nothingToRecv = 1;
          break;
        } else {
          // Message received, continue execution
          break;
        }
      }

      if (nothingToRecv == 1) {
        continue; // was break before but imo this makes more sense
      }

        printf("[Controller] Something received : [t:%d, tag:%d, round:%d, from:%d, to:%d, val:%d, forkid:%d]\n",
               receivedMessage[0], receivedMessage[1], receivedMessage[2],receivedMessage[3], receivedMessage[5], receivedMessage[4],
               receivedMessage[6]);

        // Store received message in the message array
        put_msg_in_buffer(i, receivedMessage);
        bool msg_was_delivered = false;

        // Recv message: a process wants to receive a message from another
        if (receivedMessage[0] == 1)
        {
          //printf("[Controller] This is a recv message\n");
          // kill(current_process, SIGSTOP); //*
          msgbuffer[i].connfd = connfd;
          int r = 0;

          // Look through the message buffer to match this message
          for (int j = 0; j < i; j++)
          {
            int del = 0;
            del = handleMessagePair(i, j, connfd, true);
            if (del == -1) {
              continue;
            } 
            if (del == 1) {
              msg_was_delivered = true;
            }
          }

          // if the recv message was not delivered, schedule another process
          if (!msg_was_delivered)
          {
            // It might be the end, set timer
            if (nothingDelivered == 0) {
              nothDelTime = clock();
            }
            nothingDelivered = nothingDelivered + 1;
            printf("[Controller] recv msg was not delivered\n");
            schedule_new_process();
            usleep(SLEEP_DURATION_MS);
          }
          else
          {
            nothingDelivered = 0;
            printf("[Controller] Number of states we went through : %d\n", numStates);
            printf("[Controller] Number of states we killed : %d\n", numStatesKilled);
          }
        }

        // Send message: a process sends some data to another
        if (receivedMessage[0] == 0)
        {
          //printf("[Controller] This is a send message\n");
          // kill(current_process, SIGSTOP);

          // Look through the message buffer to match this message
          for (int j = 0; j < i; j++)
          {
            int del = 0;
            del = handleMessagePair(j, i, connfd, false);
            if (del == -1) {
              continue;
            } 
            if (del == 1) {
              msg_was_delivered = true;
            }
          }

          // if the send message was not delivered, schedule another process
          if (!msg_was_delivered)
          {
            // It might be the end, set timer
            if (nothingDelivered == 0) {
              nothDelTime = clock();
            }
            nothingDelivered = nothingDelivered + 1;
            printf("[Controller] send msg was not delivered\n");
            schedule_new_process();
            usleep(SLEEP_DURATION_MS);
          }
          else
          {
            nothingDelivered = 0;
            printf("[Controller] Number of states we went through : %d\n", numStates);
            printf("[Controller] Number of states we killed : %d\n", numStatesKilled);
          }
          // close(connfd); TODO TRY NO CLOSE OBVIOUSLY NOT OPTIMAL
          //numOpenFd = numOpenFd - 1;
        }
        i++;

        // TODO every x iter do a cleanup loop like ok for every process, if its id (=forkid) doesnt appear
        // in any live state, then kill (and maybe sigterm and handle in the redirect)
        // and also for every (should be only one) recv message with that id, close the connfd
    }
  }

   // End of simulation

  printf("\n#######################################################################################\n");
  printf("[Controller] END OF SIMULATION\n");
  printf("#######################################################################################\n");

  // Print states
  printControllerState(systemStates, numStates);

  printf("\n#######################################################################################\n");

  // Check validity
  checkAllStates();

  printf("\n#######################################################################################\n");

  // Print performance metrics
  printf("[Controller] Number of states we went through : %d\n", numStates);
  printf("[Controller] Number of states we killed : %d\n", numStatesKilled);
  printf("[Controller] Number of states remaining : %d\n", numStates - numStatesKilled);

  int numStatesRemaining = 0;
  for (int s = 0; s < numStates; s++)
  {
    // Don't want to update a state that was killed
    if (systemStates[s].killed == 1)
    {
      continue;
    } else {
      numStatesRemaining++;
    }
  }

  printf("[Controller] Number of states remaining (verify ): %d\n", numStatesRemaining);
  printf("[Controller] Number of running processes : %d\n", numProcesses);
  printf("[Controller] Number of open file descriptors : %d\n", numOpenFd);

 // Calculate the elapsed time
  if (noNewConnection > 2*numProcesses) {
    double elapsed_time = (double)(noNewCoTime - start) / CLOCKS_PER_SEC;
    printf("Execution time: %.3f seconds\n", elapsed_time);
  } else {
    double elapsed_time = (double)(nothDelTime - start) / CLOCKS_PER_SEC;
    printf("Execution time: %.3f seconds\n", elapsed_time);
  }

  // Free resources

  // Close file descriptors
  for (int f = 0; f < numOpenFd; f++) {
    close(fds[f]);
  }

  // Kill processes
  for (int p = 0; p < numProcesses; p++) {
    kill(processes[p], SIGTERM);
  }

  close(sockfd);
  unlink(CONTROLLER_PATH);
  unlink(CONTROLLER_FEEDBACK_PATH);

  // Cleanup the semaphores
  sem_close(sem);
  sem_unlink("/sem_bv_consensus"); 

  sem_close(sem_init_brd);
  sem_unlink("/sem_bv_consensus_init_brd"); 

  while (wait(NULL) != -1)
    ;

  return 0;
}
