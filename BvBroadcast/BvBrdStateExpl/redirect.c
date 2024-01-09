#define _GNU_SOURCE
#include <sys/wait.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

// Used with LD_PRELOAD to intercept the calls to send and recv
// redirect those calls through the controller

static ssize_t (*real_send)(int sockfd, const void *buf, size_t len,
                            int flags) = NULL;
static ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags) =
    NULL;

#define CONTROLLER_PATH "./controller_socket"

#define CONTROLLER_FEEDBACK_PATH "./controller_feedback_socket"

#define N 3 // Total number of processes

int forkId = 0; // Only 0 at first for each process

int sendMsgCounter = 0;
int initValue = -1; // Obviously should not be -1

int feedback_socket;
int controller_socket;

void signal_handler(int signal) {
    if (signal == SIGTERM) {
        printf("\nCleaning up and exiting...\n");

        close(feedback_socket);
        close(controller_socket);
        
        unlink(CONTROLLER_PATH);
        unlink(CONTROLLER_FEEDBACK_PATH);

        exit(EXIT_SUCCESS);
    }
}

// send override
ssize_t
send(int sockfd, const void *buf, size_t len, int flags)
{
  //printf("[Intercept] Enter send\n");

  // Load the real send
  if (!real_send)
  {
    real_send = dlsym(RTLD_NEXT, "send");
  }

  if (sockfd == -1) { // this is a send to the feedback socket TODO -1 probablement pas le + intelligent

  struct sockaddr_un address;
  
  if ((feedback_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Intercept] socket");
    exit(EXIT_FAILURE);
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_FEEDBACK_PATH,
          sizeof(address.sun_path) - 1);

  if (connect(feedback_socket, (struct sockaddr *)&address,
              sizeof(struct sockaddr_un)) == -1)
  {
    perror("[Intercept] connect");
    exit(EXIT_FAILURE);
  }

  // Send feedback message to the controller
  //printf("[Intercept] Send feedback\n");
  int *intBuf = (int *)buf; 
  int feedbackMessage[3];
  feedbackMessage[0] = forkId;         
  feedbackMessage[1] = intBuf[0]; 
  feedbackMessage[2] = intBuf[1]; 

  ssize_t bytes_sent = real_send(feedback_socket, &feedbackMessage, sizeof(feedbackMessage), 0);

  close(feedback_socket);
  return bytes_sent;

  } else { // This is normal message
  // maybe mark as a echo message if it is # i > N - 1, just need sent msg counter
  sendMsgCounter = sendMsgCounter + 1;
  int *intBuf = (int *)buf; 
  if (sendMsgCounter == 1) {
    initValue = intBuf[1];
  }
  

  struct sockaddr_un address;
  
  if ((controller_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Intercept] socket");
    exit(EXIT_FAILURE);
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_PATH,
          sizeof(address.sun_path) - 1);

  if (connect(controller_socket, (struct sockaddr *)&address,
              sizeof(struct sockaddr_un)) == -1)
  {
    perror("[Intercept] connect");
    exit(EXIT_FAILURE);
  }

  // Send (redirect) the message to the controller
  //printf("[Intercept] Send\n");
  
  int sendMessage[6];
  sendMessage[0] = 0;         // type = send
  sendMessage[1] = intBuf[0]; // from = first elem of msg
  //sendMessage[2] = processId; // to = determine from sock fd dest port
  sendMessage[2] = intBuf[2]; 
  sendMessage[3] = intBuf[1]; // msg = second elem of msg
  sendMessage[4] = forkId;    // forkId

  if (sendMsgCounter > N - 1) { // if echo message
    sendMessage[5] = 1; // set the echo tag
  } else {
    sendMessage[5] = 0; // not an echo message
  }

  ssize_t bytes_sent = real_send(controller_socket, &sendMessage, sizeof(sendMessage), 0);

  //close(controller_socket);

  return bytes_sent;
}
}

// Override recv
ssize_t
recv(int sockfd, void *buf, size_t len, int flags)
{
  //printf("[Intercept] Enter recv\n");

  // Load the real recv
  if (!real_recv)
  {
    real_recv = dlsym(RTLD_NEXT, "recv");
  }

  // We also need the real send
  if (!real_send)
  {
    real_send = dlsym(RTLD_NEXT, "send");
  }

  struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0; // or SA_RESTART to restart system calls
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

  struct sockaddr_un address;
  //int controller_socket;

  // Create a socket to the controller

  if ((controller_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
  {
    perror("[Intercept] socket");
    exit(EXIT_FAILURE);
  }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, CONTROLLER_PATH, sizeof(address.sun_path) - 1);

  if (connect(controller_socket, (struct sockaddr *)&address,
              sizeof(struct sockaddr_un)) == -1)
  {
    perror("[Intercept] connect");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in local_addr;
  socklen_t addr_len = sizeof(local_addr);
  if (getsockname(sockfd, (struct sockaddr *)&local_addr, &addr_len) < 0)
  {
    perror("ERROR getting socket name");
    exit(EXIT_FAILURE);
  }
  int port = ntohs(local_addr.sin_port);
  int processId = port - 8080;

  // Send a message to the controller that this process is ready to receive
  //printf("[Intercept] send to controller\n");
  int sendMessage[6];
  sendMessage[0] = 1;         // type = recv
  sendMessage[1] = -1;        // from = first elem of msg
  sendMessage[2] = processId; // to = determine from sock fd port IF NO do trick osef put param or put serv address in global and access here whatev... no energy for this shit
  sendMessage[3] = -1;        // msg = -1 recv msg
  sendMessage[4] = forkId;    // forkId
  sendMessage[5] = -1; // echo tag


  ssize_t bytes_sent = real_send(controller_socket, sendMessage, sizeof(sendMessage), 0);

  if (bytes_sent == -1) {
    perror("ERROR sending message to controller");
    exit(EXIT_FAILURE);
  }
  // Wait for instructions from the controller
  // Carefull with recv, blocking but stream ? Insure that I recv the
  // right thing.
  // flags MSG_PEEK or MSG_WAITALL might help (using len)
  // This is Unix socket, maybe just put it SOCK_DGRAM (UDP like but
  // reliable and no reorder) instead of SOCK_STREAM (TCP like)

  // Message format
  int receivedMessage[4];

  pid_t children[100]; // how many max ?
  int i = 0;

  //printf("[Intercept] wait for controller instructions\n");

  while (1)
  {

    ssize_t bytes_received = real_recv(controller_socket, &receivedMessage, sizeof(receivedMessage), 0);
    if (bytes_received  < 0)
    {
      perror("Recv failure");
      exit(EXIT_FAILURE);
    }
    if (bytes_received == 0) { // When the connection is closed from the controller
      printf("[Intercept in p%d] recv 0 = connection %d closed ...\n", processId, controller_socket);
      while (wait(NULL) != -1);
      exit(EXIT_SUCCESS);
    }
    printf("[Intercept in p%d] recv\n", processId);

    int instruction = receivedMessage[0];
    int from = receivedMessage[1];
    int value = receivedMessage[2];
    int to = receivedMessage[3];
    printf("[Intercept in p%d] recv from controller, instr %d : {from:%d, value:%d, to:%d}\n", processId, instruction, from, value, to);
    if (!(instruction == 1 || instruction == 2 || instruction == 3)) {
      perror("[Intercept] Bad instruction from controller");
      exit(EXIT_FAILURE);
    }
    // Say instruction 1 is fork
    if (instruction == 1)
    {
      //printf("[Intercept] fork\n");
      children[i] = fork();
      if (children[i] < 0)
      {
        perror("[Intercept] fork");
        exit(EXIT_FAILURE);
      }
      if (children[i] == 0)
      {
        // The child process continues the execution with the msg value
        // received by the controller

        forkId = getpid();

        // Unwrap message from controller and transmit data to process
        int *intBuf = (int *)buf;
        intBuf[0] = from;
        intBuf[1] = value;
        intBuf[2] = to;

        //close(controller_socket); // verify ok
        return bytes_received;
      }
      i = i + 1;
    }
    else if (instruction == 3) {
      children[i] = fork();
      if (children[i] < 0)
      {
        perror("[Intercept] fork");
        exit(EXIT_FAILURE);
      }
      if (children[i] == 0)
      {
        // The child process continues the execution with the msg value
        // received by the controller

        forkId = getpid();

        // Unwrap message from controller and transmit data to process
        int *intBuf = (int *)buf;
        intBuf[0] = processId; // from this process id
        intBuf[1] = initValue; // the initial value of this process HARDCODE 0 TODO change for other cases
        intBuf[2] = to;

        //close(controller_socket); // verify ok
        return bytes_received;
      }
      i = i + 1;
    }
    else if (instruction == 2)
    { 
      //printf("[Intercept] kill\n");
      // Say instruction 2 is kill

      // Here I kill the last child (same because 2 and kill if =)
      if (kill(children[i - 1], SIGTERM) < 0) // TODO SIGTERM OR SIGKILL
      {
        perror("[Intercept] kill");
        exit(EXIT_FAILURE);
      }
    }

  }

  close(controller_socket);
  return 0;
}