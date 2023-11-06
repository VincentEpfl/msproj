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

// Controller that spawns processes, intercepts communications between
// the processes, and explore the execution state


#define CONTROLLER_FEEDBACK_PATH "./controller_feedback_socket" 
#define CONTROLLER_PATH "./controller_socket"
#define MAXMSG 256

#define N 4 // Total number of processes

// Message struct
typedef struct
{
  int type;   // send:0 or recv:1
  int from;   // -1 for recv msg, because can recv from any process
  int to;     // for recv msg this is the process that wants to recv
  int msg;    // for recv put -1
  int connfd; // -1 for send msg, because we don't keep the connection
  int forkId;
  int numDelivered;  // number of times it was delivered, always 0 or 1 for recv
  int delivered[10]; // forkIds where it was delivered
} Message;

// TODO actually state should be
typedef struct
{
  int len;            // len of forkPath
  pid_t forkPath[10]; // what should be max length ?

  // received value format :
  // { process i :
  //     {#0s i recv from different processes, #1s i recv from different processes},
  // }
  int valuesCount[N][2];
  int killed; // 1 if state was killed because redundant, 0 if not
} StateTODO;

sem_t *sem;

// Array to store messages
Message msgbuffer[100];

// Array to store processes
pid_t processes[100];
int numProcesses = N;
pid_t current_process;
int current_process_index;

// What should be max number of system state that we can track in parallel ?
StateTODO systemStates[50] = {
    // good or need init all inside ?
    {
        0,
        {0},
        {{0, 0}},
        0},
};

int sockfd;
int feedback_sockfd;
int numStates = 1;


int get_states_to_update(int *res, int *statesToUpdate, int recv_msg_index)
{
  int numStatesToUpdate = 0;
  int posInForkPath = 0;
  if (numStates == 1)
  {
    numStatesToUpdate = 1;
    statesToUpdate[0] = 0;
  }
  else
  {
    for (int s = 0; s < numStates; s++)
    {
      // Don't want to update a state that was killed
      if (systemStates[s].killed == 1)
      {
        continue;
      }

      if (systemStates[s].len == 0)
      { // init le 1 elem de forkpath devrait etre 0
        systemStates[s].len = 1;
        systemStates[s].forkPath[0] = 0;
      }
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
  }
  res[0] = numStatesToUpdate;
  res[1] = posInForkPath;
  return 0;

  if (numStatesToUpdate == 0)
  {
    // discard msg or something
    return -1;
  }
}

void put_msg_in_buffer(int index, int *receivedMessage)
{
  msgbuffer[index].type = receivedMessage[0];
  msgbuffer[index].from = receivedMessage[1];
  msgbuffer[index].to = receivedMessage[2];
  msgbuffer[index].msg = receivedMessage[3];
  msgbuffer[index].connfd = -1;
  msgbuffer[index].forkId = receivedMessage[4];
  msgbuffer[index].numDelivered = 0;
  for (int d = 0; d < 10; d++)
  {
    msgbuffer[index].delivered[d] = 0;
  }
}

int initSocket(bool feedback)
{
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
    tv.tv_sec = 1;  // 1 seconds timeout
    tv.tv_usec = 0; // 0 microseconds

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

  if (listen(sockfd, 100) == -1) // max queue capacity might be an issue ?
  {
    perror("[Controller] listen");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  return sockfd;
}

void spawnProcesses()
{
  for (int i = 0; i < N; i++)
  {
    if ((processes[i] = fork()) == 0)
    {
      char processIdStr[10], initialValueStr[10];
      sprintf(processIdStr, "%d", i);
      sprintf(initialValueStr, "%d", 0);
      // Replace child process with BV-broadcast process
      setenv("LD_PRELOAD", "./redirect.so", 1);
      execl("./bv_broadcast", "bv_broadcast", processIdStr, initialValueStr, (char *)NULL);
      perror("execl failed");
      exit(EXIT_FAILURE); // Exit if execl fails
    }
  }

  // Wait until all processes have setup their sockets
  sleep(15);

  // Signal all children to proceed, but only allow 1
  for (int i = 0; i < N; i++)
  {
    if (i == 0)
    {
      sem_post(sem);
      printf("[Controller] Schedule process 0\n");
      current_process = processes[0];
      current_process_index = 0;
    }
    else
    {
      sem_post(sem);
      kill(processes[i], SIGSTOP);
    }
  }
}

void init()
{
  systemStates[0].len = 1;
  systemStates[0].forkPath[0] = 0;

  // Create the semaphore
  sem = sem_open("/sem_bv_broadcast", O_CREAT, 0644, 0);
  if (sem == SEM_FAILED)
  {
    perror("Semaphore creation failed");
    exit(EXIT_FAILURE);
  }

  // Create controller socket to intercept processes communication
  // setupControllerSocket()

  sockfd = initSocket(false);
  feedback_sockfd = initSocket(true);

  // Spawn child processes
  spawnProcesses();
}

void schedule_new_process()
{
  kill(current_process, SIGSTOP);
  current_process_index = (current_process_index + 1) % numProcesses;
  current_process = processes[current_process_index];
  kill(current_process, SIGCONT);
  printf("[Controller] scheduling process %d on forkId %d\n", current_process_index, current_process);
}

// Compares the state of the system
// Returns false if state1 != state2
bool compareState(int state1[N][2], int state2[N][2])
{
  for (int i = 0; i < N; i++)
  {
    for (int j = 0; j < 2; j++)
    {
      if (state1[i][j] != state2[i][j])
      {
        return false;
      }
    }
  }
  return true;
}

// Compares the state of 2 processes
// Returns false if processState1 != processState2
bool compareProcessState(int processState1[2], int processState2[2])
{
  for (int i = 0; i < 2; i++)
  {
    if (processState1[i] != processState2[i])
    {
      return false;
    }
  }
  return true;
}


void deliver_message(int delivered, int to)
{
  msgbuffer[delivered].delivered[msgbuffer[delivered].numDelivered] = msgbuffer[to].forkId;
  msgbuffer[delivered].numDelivered = msgbuffer[delivered].numDelivered + 1;
}

void printMessage(int index)
{
  printf("msg:[t:%d, from:%d, to:%d, value:%d, connfd:%d, forkId:%d, numDelivered:%d]\n",
         msgbuffer[index].type, msgbuffer[index].from, msgbuffer[index].to, msgbuffer[index].msg, msgbuffer[index].connfd,
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

bool canDeliver(int posInForkPath, int *statesToUpdate, int sendIndex, int recvIndex, int type)
{
  // Check if the message comes from a parallel execution/state,
  // in this case we don't want it
  bool forkOk = true;
  if (numStates > 1)
  { 
    // if fork id of send msg is before (or same as) the forkid of recv msg, ok
    forkOk = false;
    for (int f = 0; f < posInForkPath + 1; f++)
    {
      if (systemStates[statesToUpdate[0]].forkPath[f] == msgbuffer[sendIndex].forkId)
      {
        forkOk = true;
        break;
      }
    }
  }

  // Check if the send message was already delivered to this state
  bool sendDeliverOk = true;
  if (numStates > 1)
  {
    if (msgbuffer[sendIndex].numDelivered > 0)
    {

      for (int f = 0; f < msgbuffer[sendIndex].numDelivered; f++)
      {
        for (int g = 0; g < posInForkPath; g++)
        {
          if (msgbuffer[sendIndex].delivered[f] == systemStates[statesToUpdate[0]].forkPath[g])
          {
            sendDeliverOk = false;
            break;
          }
        }
      }
    }
  }

  // Check if the recv message was already delivered
  bool recvDeliverOk = true;
  if (msgbuffer[sendIndex].numDelivered > 0)
  {
    recvDeliverOk = false;
  }

  return recvDeliverOk && sendDeliverOk && msgbuffer[sendIndex].type == type && msgbuffer[sendIndex].to == msgbuffer[recvIndex].to && forkOk;
}

void sendMsgToProcess(int connfd, int *message, int *recmsg)
{
  send(connfd, &message, sizeof(message), 0);

  // Recover the resulting state
  // format [forkid, processState]

  int feedback_connfd;
  if ((feedback_connfd = accept(feedback_sockfd, NULL, NULL)) != -1)
  {
    if (recv(feedback_connfd, recmsg, sizeof(recmsg), 0) == -1)
    {
      perror("[Controller] recv state");
      exit(EXIT_FAILURE);
    }
    close(feedback_connfd);
  }
}

void sendMsgAndRecvState(int connfd, int *message, int send_msg_index, int *newProcessState, int *forkInfo)
{
  // format fork: [1, from:processId, value:0/1]
  // format kill: [2, -1, -1] maybe put which child to kill
  int recmsg[3];
  sendMsgToProcess(connfd, message, recmsg);

  printf("[Controller] state recovered\n");
  newProcessState[0] = recmsg[1];
  newProcessState[1] = recmsg[2];
  forkInfo[0] = recmsg[0];
  forkInfo[1] = numProcesses;
  processes[numProcesses++] = forkInfo[0];
  kill(forkInfo[0], SIGSTOP);
  printf("[Controller] process %d state is now {%d, %d} in forkid %d\n", msgbuffer[send_msg_index].to, newProcessState[0], newProcessState[1], forkInfo[0]);
}


void duplicateState(int originState, int destState)
{
  // Copies the state to update into a new state object in the array of states
  for (int l = 0; l < N; l++)
  {
    for (int m = 0; m < 2; m++)
    {
      systemStates[destState].valuesCount[l][m] = systemStates[originState].valuesCount[l][m];
    }
  }

  for (int k = 0; k < systemStates[originState].len; k++)
  {
    systemStates[destState].forkPath[k] = systemStates[originState].forkPath[k];
  }

  systemStates[destState].len = systemStates[originState].len;
}

void updateState(int stateToUpdate, int forkid, int *newProcessState, int updatedProcess)
{
  systemStates[stateToUpdate].forkPath[systemStates[stateToUpdate].len] = forkid;
  systemStates[stateToUpdate].len = systemStates[stateToUpdate].len + 1;
  for (int l = 0; l < 2; l++)
  {
    systemStates[stateToUpdate].valuesCount[updatedProcess][l] = newProcessState[l];
  }
}

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

      if (!forkid_killed_temp)
      {
        kill(killHandle, SIGKILL);
        waitpid(killHandle, NULL, 0);
        forkid_killed_temp = true;
      }
      printf("[Controller] kill state %d on forkid %d\n", state, killHandle);

      // there I could send(connfd, kill msg with forkid0) instead of SIGKILL
      systemStates[state].killed = 1; 
    }
  }
  return forkid_killed_temp;
}

void printControllerState(StateTODO *systemStates, int numStates)
{
  printf("[Controller] Print Controller State :\n");
  for (int s = 0; s < numStates; s++)
  {
    printf("[Controller] State %d:\n", s);
    printf("[Controller] forkPath: ");
    for (int f = 0; f < systemStates[s].len; f++)
    {
      printf("%d/", systemStates[s].forkPath[f]);
    }
    printf("\n");
    printf("[Controller] values count: \n");
    for (int p = 0; p < N; p++)
    {
      printf("process %d : {", p);
      for (int v = 0; v < 2; v++)
      {
        printf("%d, ", systemStates[s].valuesCount[p][v]);
      }
      printf("}\n");
    }
  }
}

int main()
{

  init();

  // Events that make the whole thing progress = messages between processes
  // = messages intercepted by controller
  // Wait for messages to advance execution
  // = msg recveived : stop process that sent, exec/continue a process in array

  // format : [send:0/recv:1, from:processId/-1, to:processId, value:0/1, forkid]
  int receivedMessage[5];
  int connfd;
  int i = 0;
  printf("[Controller] Listen for incoming messages\n");
  while (1)
  {
    if ((connfd = accept(sockfd, NULL, NULL)) < 0)
    {
      // Check if the error was due to a timeout
      if (errno == EWOULDBLOCK || errno == EAGAIN)
      {
        printf("[Controller] No connections within the timeout period.\n");
        schedule_new_process();
      }
      else
      {
        // An error occurred that wasn't a timeout
        perror("[Controller] Accept failure");
        exit(EXIT_FAILURE);
      }
    }
    else
    {
      printf("[Controller] New connection\n");
      ssize_t len = recv(connfd, &receivedMessage, sizeof(receivedMessage), 0);
      if (len == -1)
      {
        perror("[Controller] Recv failure");
        exit(EXIT_FAILURE);
      }
      if (len > 0)
      {
        printf("[Controller] Something received : [t:%d, from:%d, to:%d, val:%d, forkid:%d]\n",
               receivedMessage[0], receivedMessage[1], receivedMessage[2], receivedMessage[3],
               receivedMessage[4]);

        // Store received message in the message array
        put_msg_in_buffer(i, receivedMessage);
        bool msg_was_delivered = false;
        if (receivedMessage[0] == 1)
        {
          // Recv message = a process wants to receive a message from another
          printf("[Controller] This is a recv message\n");
          msgbuffer[i].connfd = connfd;
          int r = 0;

          for (int j = 0; j < i; j++)
          {
            // Look through the message array if the message it wants is
            // already there

            // Get the system states to update
            int statesToUpdate[numStates];
            int res[2];
            if (get_states_to_update(res, statesToUpdate, i) == -1)
            {
              break;
            }
            int numStatesToUpdate = res[0];
            int posInForkPath = res[1];

            if (canDeliver(posInForkPath, statesToUpdate, j, i, 0))
            {
              printf("[Controller] send msg to receiver\n");
              printMessage(j);

              printf("[Controller] numStatesToUpdate: %d, posInForkPath: %d\n", numStatesToUpdate, posInForkPath);
              printf("[Controller] States to update :");
              for (int s = 0; s < numStatesToUpdate; s++)
              {
                printf("%d,", statesToUpdate[s]);
              }
              printf("\n");

              msg_was_delivered = true;

              // recv msg always need to be delivered only once
              deliver_message(i, j);

              // send msg might need to be sent to different states
              deliver_message(j, i);

              // Try to send the message
              int newProcessState[2];
              int forkInfo[2];
              int message[3] = {1, msgbuffer[j].from, msgbuffer[j].msg};
              sendMsgAndRecvState(connfd, message, j, newProcessState, forkInfo);
              int forkid0 = forkInfo[0];
              int forkid0_index = forkInfo[1];

              // Try to send the message with the opposite value
              printf("[Controller] send opposite msg to receiver\n");
              int opValue = 1 - msgbuffer[j].msg;
              int messageOp[3] = {1, msgbuffer[j].from, opValue};
              int newProcessStateOp[2];
              int forkInfoOp[2];
              sendMsgAndRecvState(connfd, messageOp, j, newProcessStateOp, forkInfoOp);
              int forkid1 = forkInfoOp[0];
              int forkid1_index = forkInfoOp[1];

              if (compareProcessState(newProcessState, newProcessStateOp))
              {
                // Here I consider that I kill forkid1 by default

                printf("[Controller] Same result: kill a child\n");
                int killMessage[3] = {2, -1, -1};
                if (send(connfd, &killMessage, sizeof(killMessage), 0) == -1)
                {
                  perror("[Controller] send fail");
                  exit(EXIT_FAILURE);
                }

                // Update the system states
                for (int s = 0; s < numStatesToUpdate; s++)
                {
                  updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[i].to);
                }

                // stop the current one ? it loops waiting for controller instructions anyway
                numProcesses = numProcesses - 1;
                processes[numProcesses] = -1; // "delete" forkid1
                kill(current_process, SIGSTOP);
                current_process = forkid0;
                current_process_index = forkid0_index;
                kill(forkid0, SIGCONT);
                printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
              }
              else
              {

                // Copy sys state to update in 1 new state for each fork 
                bool forkid0_killed = false;
                bool forkid1_killed = false;
                for (int s = 0; s < numStatesToUpdate; s++)
                {

                  // Copies the state to update into a new state object in the array of states
                  duplicateState(statesToUpdate[s], numStates);

                  // Update the states
                  updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[i].to);
                  updateState(numStates, forkid1, newProcessStateOp, msgbuffer[i].to);

                  numStates = numStates + 1;

                  // If the new system states are the same as some that are already stored, kill the new ones
                  forkid0_killed = killStateAlreadyThere(statesToUpdate[s], numStates, forkid0, forkid0_killed);
                  forkid1_killed = killStateAlreadyThere(numStates - 1, numStates, forkid1, forkid1_killed);
                }
                if (forkid0_killed)
                {
                  numProcesses = numProcesses - 1;
                  processes[numProcesses - 1] = processes[numProcesses]; // copy forkid1 in forkid0 place (overwrite forkid0)
                  processes[numProcesses] = -1;                          // "delete" forkid1 : delete forkid0
                  kill(current_process, SIGSTOP);
                  current_process = forkid1;
                  current_process_index = forkid1_index - 1;
                  kill(forkid1, SIGCONT);
                  printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid1);
                }
                else if (forkid1_killed)
                { // forkid0 and forkid1 should not both be killed
                  numProcesses = numProcesses - 1;
                  processes[numProcesses] = -1; // "delete" forkid1
                  kill(current_process, SIGSTOP);
                  current_process = forkid0;
                  current_process_index = forkid0_index;
                  kill(forkid0, SIGCONT);
                  printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
                }
                else
                { // both are alive, just chose 1
                  kill(current_process, SIGSTOP);
                  current_process = forkid0;
                  current_process_index = forkid0_index;
                  kill(forkid0, SIGCONT);
                  printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[j].to, forkid0);
                }
              }
              printControllerState(systemStates, numStates);
              close(connfd);
              break;
            }
          }

          // if the recv message was not delivered, schedule another process
          if (!msg_was_delivered)
          {
            printf("[Controller] recv msg was not delivered\n");
            schedule_new_process();
          }
        }

        if (receivedMessage[0] == 0)
        {
          // This is a send message : a process sends some data to another
          printf("[Controller] This is a send message\n");
          kill(current_process, SIGSTOP);
          // Go through the message buffer to see if the process waiting for this
          // data is already there

          for (int j = 0; j < i; j++)
          {
            // Get the system state to update
            int statesToUpdate[numStates];
            int res[2];
            if (get_states_to_update(res, statesToUpdate, j) == -1)
            {
              break;
            }
            int numStatesToUpdate = res[0];
            int posInForkPath = res[1];

            // Found a recv message from the process that the send msg is addressed to
            if (canDeliver(posInForkPath, statesToUpdate, i, j, 1))
            {
              printf("[Controller] send msg to receiver\n");
              printMessage(i);
              printf("to recv : \n");
              printMessage(j);

              printf("[Controller] numStatesToUpdate: %d, posInForkPath: %d\n", numStatesToUpdate, posInForkPath);
              printf("[Controller] States to update :");
              for (int s = 0; s < numStatesToUpdate; s++)
              {
                printf("%d,", statesToUpdate[s]);
              }
              printf("\n");

              kill(current_process, SIGSTOP); // In case the msg is delivered several times
              msg_was_delivered = true;

              // recv msg always need to be delivered only once
              deliver_message(j, i);

              // send msg might need to be sent to different states
              deliver_message(i, j);

              // schedule the process that sent the recv message and is waiting for controller instructions
              if (msgbuffer[j].forkId == 0)
              {
                current_process_index = msgbuffer[j].to;
                current_process = processes[current_process_index];
              }
              else
              {
                current_process = msgbuffer[j].forkId;
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
              printf("[Controller] Schedule process %d on forkId %d to send instructions\n", current_process_index, current_process);

              // Try to send the message
              int newProcessState[2];
              int forkInfo[2];
              int message[3] = {1, msgbuffer[i].from, msgbuffer[i].msg};
              sendMsgAndRecvState(msgbuffer[j].connfd, message, i, newProcessState, forkInfo);
              int forkid0 = forkInfo[0];
              int forkid0_index = forkInfo[1];

              // Try to send the message with opposite value
              printf("[Controller] send opposite msg to receiver\n");
              int opValue = 1 - msgbuffer[i].msg;
              int messageOp[3] = {1, msgbuffer[i].from, opValue};
              int newProcessStateOp[2];
              int forkInfoOp[2];
              // sendMsgToProcess()
              sendMsgAndRecvState(msgbuffer[j].connfd, messageOp, i, newProcessStateOp, forkInfoOp);
              int forkid1 = forkInfoOp[0];
              int forkid1_index = forkInfoOp[1];

              if (compareProcessState(newProcessState, newProcessStateOp))
              {
                // Here I consider that I kill forkid1 by default

                printf("[Controller] Same result: kill a child\n");
                int killMessage[3] = {2, -1, -1};
                if (send(msgbuffer[j].connfd, &killMessage, sizeof(killMessage), 0) == -1)
                {
                  perror("[Controller] send fail");
                  exit(EXIT_FAILURE);
                }

                // Update the system states
                for (int s = 0; s < numStatesToUpdate; s++)
                {
                  updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[j].to);
                }
                numProcesses = numProcesses - 1;
                processes[numProcesses] = -1; // "delete" forkid1
                kill(current_process, SIGSTOP);
                current_process = forkid0;
                current_process_index = forkid0_index;
                kill(forkid0, SIGCONT);
                printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[i].to, forkid0);
              }
              else
              {

                // Copy sys state to update in 1 new state for each fork 
                bool forkid0_killed = false;
                bool forkid1_killed = false;
                for (int s = 0; s < numStatesToUpdate; s++)
                {

                  // Copies the state to update into a new state object in the array of states
                  duplicateState(statesToUpdate[s], numStates);

                  // Update the states
                  updateState(statesToUpdate[s], forkid0, newProcessState, msgbuffer[j].to);
                  updateState(numStates, forkid1, newProcessStateOp, msgbuffer[j].to);

                  numStates = numStates + 1;

                  // If the new system states are the same as some that are already stored, kill the new ones
                  forkid0_killed = killStateAlreadyThere(statesToUpdate[s], numStates, forkid0, forkid0_killed);
                  forkid1_killed = killStateAlreadyThere(numStates - 1, numStates, forkid1, forkid1_killed);
                }
                if (forkid0_killed)
                {
                  numProcesses = numProcesses - 1;
                  processes[numProcesses - 1] = processes[numProcesses]; // copy forkid1 in forkid0 place (overwrite forkid0)
                  processes[numProcesses] = -1;                          // "delete" forkid1 : delete forkid0
                  kill(current_process, SIGSTOP);
                  current_process = forkid1;
                  current_process_index = forkid1_index - 1;
                  kill(forkid1, SIGCONT);
                  printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[i].to, forkid1);
                }
                else if (forkid1_killed)
                { // forkid0 and forkid1 should not both be killed
                  numProcesses = numProcesses - 1;
                  processes[numProcesses] = -1; // "delete" forkid1
                  kill(current_process, SIGSTOP);
                  current_process = forkid0;
                  current_process_index = forkid0_index;
                  kill(forkid0, SIGCONT);
                  printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[i].to, forkid0);
                }
                else
                { // both are alive, just chose 1
                  kill(current_process, SIGSTOP);
                  current_process = forkid0;
                  current_process_index = forkid0_index;
                  kill(forkid0, SIGCONT);
                  printf("[Controller] scheduling process %d on forkId %d\n", msgbuffer[i].to, forkid0);
                }
              }
              printControllerState(systemStates, numStates);
              // attention
              close(msgbuffer[j].connfd);
              // Since this is a send message, there could be other recv messages waiting to be delivered this msg
            }
          }

          // if the send message was not delivered, schedule another process
          if (!msg_was_delivered)
          {
            printf("[Controller] send msg was not delivered\n");
            current_process_index = (current_process_index + 1) % numProcesses;
            current_process = processes[current_process_index];
            kill(current_process, SIGCONT);
            printf("[Controller] scheduling process %d on forkId %d\n", current_process_index, current_process);
          }
          close(connfd);
        }
        i++;
      }
    }
  }

  // accept is blocking so this is never reached

  close(sockfd);
  unlink(CONTROLLER_PATH);

  while (wait(NULL) != -1)
    ;

  sem_close(sem);
  sem_unlink("/sem_bv_broadcast"); // Cleanup the semaphore

  return 0;
}
