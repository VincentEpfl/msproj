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

// TODO verify msg formats everything compatible

#define CONTROLLER_PATH "./controller_socket"
#define MAXMSG 256

#define N 4  // Total number of processes

sem_t *sem;

// Controller that spawns processes, intercepts communications between
// the processes, and explore the execution state

// Message struct
// contains the data
// the file descriptor of the socket used to receive the message
// When this is a recv message that came before a send message,
// this is used to reply to the message later when the send message arrives
// We might store information about sender/receiver of messages here when 
// there are more than 2 processes

typedef struct
{
  int type; // send:0 or recv:1
  int from; // -1 for recv msg, because can recv from any process
  int to;  // for recv msg this is the process that wants to recv
  int msg; // for recv put -1 
  int connfd; // -1 for send msg, because we don't keep the connection
  int forkId;
} Message;

// Spawn processes
pid_t
spawn_process (const char *process_name, int processId, int initialValue)
{
  printf ("[Controller] Create process %s\n", process_name);
  pid_t pid = fork ();

  if (pid == -1)
    {
      perror ("[Controller] fork");
      exit (EXIT_FAILURE);
    }

  if (pid == 0)
    {
        char processIdStr[10], initialValueStr[10];
        sprintf(processIdStr, "%d", processId);
        sprintf(initialValueStr, "%d", initialValue);
      // Set LD_PRELOAD so that our file intercepts function calls
      setenv ("LD_PRELOAD", "./redirect.so", 1);
      execlp (process_name, process_name, processIdStr, initialValueStr, (char *) NULL);
      perror ("[Controller] execlp");
      exit (EXIT_FAILURE);
    }
  return pid;
}

// Compares the state of the system
// Returns false if state1 != state2
bool compareState(int state1[N][2], int state2[N][2]) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 2; j++) {
            if (state1[i][j] != state2[i][j]) {
                return false;
            }
        }
    }
    return true;
}

// Compares the state of 2 processes
// Returns false if processState1 != processState2
bool compareProcessState(int processState1[2], int processState2[2]) {
    for (int i = 0; i < 2; i++) {
        if (processState1[i] != processState2[i]) {
            return false;
        }
    }
    return true;
}

typedef struct
{
  int len; // len of forkPath
  pid_t forkPath[10]; // what should be max length ?  
  int committedValues[N][2];
} State;

int
main ()
{
	// Array to store messages
  Message msgbuffer[50];

  // Array to store processes
  pid_t processes[N];

  // for now this is the state of the system
  int committedValues[N][2] = {{0, 0}};

  // What should be max number of system state that we can track in parallel ?
  State systemStates[10] = { // good or need init all inside ?
    {
        0,
        {0},
        {{0, 0}}
    },
  };

  // Create the semaphore
    sem = sem_open("/sem_bv_broadcast", O_CREAT, 0644, 0);
    if (sem == SEM_FAILED) {
        perror("Semaphore creation failed");
        exit(EXIT_FAILURE);
    }

  // Create controller socket to intercept processes communication
  int sockfd;
  struct sockaddr_un address;
  printf ("[Controller] Create controller socket\n");
  if ((sockfd = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
      perror ("[Controller] receive socket");
      exit (EXIT_FAILURE);
    }
  memset (&address, 0, sizeof (address));
  address.sun_family = AF_UNIX;
  strncpy (address.sun_path, CONTROLLER_PATH, sizeof (address.sun_path) - 1);
  unlink (CONTROLLER_PATH);

  if (bind (sockfd, (struct sockaddr *) &address, sizeof (address)) == -1)
    {
      perror ("[Controller]  bind");
      close (sockfd);
      exit (EXIT_FAILURE);
    }

  if (listen (sockfd, 5) == -1)
    {
      perror ("[Controller] listen");
      close (sockfd);
      exit (EXIT_FAILURE);
    }

  // Spawn processes
  for (int i = 0; i < N; i++) {
    processes[i] = spawn_process ("bv_broadcast", i, 0);
  }

  // Wait until all processes have setup their sockets
  sleep(5); 

  // Signal all children to proceed
    for (int i = 0; i < N; i++) {
        sem_post(sem);
    }

  // Could start the execution of only 1 process to do 1 by 1 execution
  // Probably deal with that with SIGSTOP / SIGCONT 

  // Events that make the whole thing progress = messages between processes
  // = messages intercepted by controller
  // Wait for messages to advance execution
  // = msg recveived : stop process that sent, exec/continue a process in array
  char buffer[256];

  // format : [send:0/recv:1, from:processId/-1, to:processId, value:0/1, forkid]
  int receivedMessage[5];
  int connfd;
  int i = 0;
  int numStates = 0;
  printf ("[Controller] Listen for incoming messages\n");
  while ((connfd = accept (sockfd, NULL, NULL)) != -1)
    {
      
      ssize_t len = recv (connfd, &receivedMessage, sizeof (receivedMessage), 0);

      if (len > 0)
	{
	  printf ("[Controller] Something received : [t:%d, from:%d, to:%d, val:%d, forkid:%d]\n", 
      receivedMessage[0], receivedMessage[1], receivedMessage[2], receivedMessage[3], 
      receivedMessage[4]);

	      // Store received message in the message array (put in a fct)
	      // TODO For now I don't remove messages from the array 
          msgbuffer[i].type = receivedMessage[0];
          msgbuffer[i].from = receivedMessage[1];
          msgbuffer[i].to = receivedMessage[2];
	      msgbuffer[i].msg = receivedMessage[3];
          msgbuffer[i].connfd = -1; //maybe put a default value 
          msgbuffer[i].forkId = receivedMessage[4];

	      if (receivedMessage[0] == 1)
		{
		  // Recv message = a process wants to receive a message from another
		  printf ("[Controller] This is a recv message\n");
		  msgbuffer[i].connfd = connfd;
		  int r = 0;
		  for (int j = 0; j < i; j++)
		    {
                // Look through the message array if the message it wants is 
		        // already there

              // TODO handle init case no fork len =0 default forkid = 0 ?

              // Get the system state to update
                  int indexStateToUpdate;
                  for (int s = 0; s < numStates; s++) { // always last ?
                    if (systemStates[s].forkPath[systemStates[s].len] == msgbuffer[i].forkId) {
                        indexStateToUpdate = s;
                        break;
                    }
                  }

                  // Need forkPath[0] = 0 or something to make sure msg from base broadcast always ok
                  // Check if the message comes from a parallel execution/state,
                  // in this case we don't want it
                  bool forkOk = true;
                  if (numStates > 1) { //check init 1 ou 0, if only 1 state then ofc accept
                  
                  for (int s = 0; s < numStates; s++) {
                    int curStateLen = systemStates[indexStateToUpdate].len;
                    int sLen = systemStates[s].len;
                    int minLen = (curStateLen < sLen) ? curStateLen : sLen;
                    for (int f = 0; f < minLen; f++) { // Here start at 1 if always 0 first forkpath OR handle base case differently (potentiellement ok de rien faire comme default true)
                        if (systemStates[indexStateToUpdate].forkPath[f] == systemStates[s].forkPath[f]) {
                            forkOk = false;
                            break;
                        }
                    }
                    
                  }
                  }

		      if (msgbuffer[j].type == 0 && msgbuffer[j].to == msgbuffer[i].to && forkOk)
			{
			  printf ("[Controller] send msg to receiver\n");

			  // Try to send the message
              // format fork: [1, from:processId, value:0/1]
              // format kill: [2, -1, -1] maybe put which child to kill
              int message[3] = {1, msgbuffer[j].from, msgbuffer[j].msg};
			  int s1 = send (connfd, &message, sizeof(message), 0);
			  if (s1 == -1)
			    {
			      perror ("[Controller] send fail");
			      exit (EXIT_FAILURE);
			    }

			  // Recover the resulting state
              // format [forkid, processState]
              int recmsg[3];
              int newProcessState[2];
			  ssize_t brec = recv (connfd, recmsg, sizeof (recmsg), 0);
			  if (brec == -1)
			    {
			      perror ("[Controller] recv state");
			      exit (EXIT_FAILURE);
			    }

                printf ("[Controller] state recovered\n");
                // Useless but just to get the idea I will refactor after...
                newProcessState[0] = recmsg[1];
                newProcessState[1] = recmsg[2];
                int forkid0 = recmsg[0];

                // Everything after is if I change value and try to explore
                // For transparent stop here
                // Test transparent, test change value only 1 process...

			  // Try to send the message with the opposite value
              printf ("[Controller] send opposite msg to receiver\n");
              int opValue = 1 - msgbuffer[j].msg;
              int messageOp[3] = {1, msgbuffer[j].from, opValue};
			  ssize_t s3 = send (connfd, &messageOp, sizeof (messageOp), 0);
			  if (s3 == -1)
			    {
			      perror ("[Controller] send to fail");
			      exit (EXIT_FAILURE);
			    }
			  // Recover the resulting state
              int recmsg2[3];
			  int newProcessStateOp[2];
			  //sleep (2);
			  ssize_t brec4 = recv (connfd, recmsg2, sizeof (recmsg2), 0);
			  if (brec4 == -1)
			    {
			      perror ("[Controller] recv to fail");
			      exit (EXIT_FAILURE);
			    }

              printf ("[Controller] state recovered\n");
              // Useless but just to get the idea I will refactor after...
                newProcessStateOp[0] = recmsg2[1];
                newProcessStateOp[1] = recmsg2[2];
                int forkid1 = recmsg2[0];

              if (compareProcessState(newProcessState, newProcessStateOp)) {
                // if the resulting states are the same, kill 1
                // but then actually I have to kill a child not the parent ?
                printf("[Controller] Same result: kill a child\n");
                int killMessage[3] = {2, -1, -1};
			    if (send (connfd, &killMessage, sizeof(killMessage), 0) == -1)
			      {
			        perror ("[Controller] send fail");
			        exit (EXIT_FAILURE);
			      }
                  // Need to be careful if the child I kill already sent something
                  // to the controller
                  // But controller execs only 1 process at a time ?
                  // Then just exec another process, or exec 1 child but kill the other ?

                  // Update the system state
                  for (int l = 0; l < 2; l++) {
                    systemStates[indexStateToUpdate].committedValues[msgbuffer[i].to][l] = newProcessState[l];
                  }
                  // Update forkPath->path,forkid or not ?

                  // If the new system state is equal to another system state in store, just kill
                  // this thing TODO

                  printf ("[Controller] state\n");
                  for (int k = 0; k < N; k++) {
                    printf("Process %d : {%d, %d}\n", k, committedValues[k][0], committedValues[k][1]);
                  }
              } else {
                // Now what to do, I have 2 different system states
                // Store the system state for each branch (alive = need clean if merge)
                // Fair enough that's what I did in example but how do I keep track and handle ?
                // 1 child = 1 state
                // future actions of this child have to be committed to this sys state
                // how ?
                // when this child sends a message and another recv, and state is updated
                // as a result, then this sys state is affected not the other one
                // when this child recv a message by another process and it affects its
                // state, it should affect this sys state not the other one
                // So important : this child, this sys state
                // have to have child id -> sys state in controller
                // have to have child id in msg 
                // so that forward a msg only if msg.childid = rec.childid
                // mais c'est pas msg.childid = rec.childid plutot msg.childid < rec.childid
                // plusieurs forks, plutot chain id = base/childid0/childid1...
                // then chainid0 < chainid1 IF (len(chainid0) <= len(chainid1) AND 
                // for i:0->len(chainid0) : chainid0[i] = chainid1[i])
                // then if 2 sys state are equal at some update, one full fork can be killed
                // need to be able to kill this process, not only 1 child, so need kill
                // parent instruction (still need kill child ?)
                // and I can kill the parent of the things that just gave me equal state so 
                // no need to track another fork etc ?

                
                // Copy sys state to update in 1 new state for each fork (en vrai besoin d'en add
                // 1 seul car le fork 0 peut etre le state original, mais comment gerer les id?)

                for (int l = 0; l < N; l++) {
                    for (int m = 0; m < 2; m++) { // systemStates[0]: 0->this fork id from msg
                        systemStates[numStates].committedValues[l][m] = systemStates[indexStateToUpdate].committedValues[l][m];
                        systemStates[numStates + 1].committedValues[l][m] = systemStates[indexStateToUpdate].committedValues[l][m];
                    }
                }

                for (int k = 0; k < systemStates[numStates].len; k++) {
                    systemStates[numStates].forkPath[k] = systemStates[indexStateToUpdate].forkPath[k];
                }
                systemStates[numStates].forkPath[systemStates[numStates].len] = forkid0;
                for (int k = 0; k < systemStates[numStates].len; k++) {
                    systemStates[numStates + 1].forkPath[k] = systemStates[indexStateToUpdate].forkPath[k];
                }
                systemStates[numStates + 1].forkPath[systemStates[numStates].len] = forkid1;

                systemStates[numStates].len = systemStates[indexStateToUpdate].len + 1;
                systemStates[numStates + 1].len = systemStates[indexStateToUpdate].len + 1;

                // Update the sys states. Now we have the 2 different resulting sys states
                  for (int l = 0; l < 2; l++) {
                    systemStates[numStates].committedValues[msgbuffer[i].to][l] = newProcessState[l];
                    systemStates[numStates + 1].committedValues[msgbuffer[i].to][l] = newProcessStateOp[l];
                  }

                  numStates = numStates + 2;

                  // If any of the new system states is equal to another system state in store, 
                  // just kill this thing TODO

              } 
			  close (connfd);
			}
		    }

		}

	      if (receivedMessage[0] == 0)
		{
		  // This is a send message : a process sends some data to another
		  printf ("[Controller] This is a send message\n");
		  // Go through the message buffer to see if the process waiting for this
		  // data is already there
		  for (int j = 0; j < i; j++)
		    {

		      // Found a recv message from the process that the send msg is addressed to
		      if (msgbuffer[j].type == 1 && msgbuffer[j].to == msgbuffer[i].to)
			{
			  printf ("[Controller] send msg to receiver\n");

              // TODO update same as above 

			// Try to send the message
              int message[3] = {1, msgbuffer[i].from, msgbuffer[i].msg};
			  int s1 = send (msgbuffer[j].connfd, &message, sizeof(message), 0);
			  if (s1 == -1)
			    {
			      perror ("[Controller] send fail");
			      exit (EXIT_FAILURE);
			    }
			  
			  // Recover the resulting state
              int newState[N][2] = {{0, 0}};
			  ssize_t brec = recv (msgbuffer[j].connfd, newState, sizeof (newState), 0);
			  if (brec == -1)
			    {
			      perror ("[Controller] recv state");
			      exit (EXIT_FAILURE);
			    }

			  printf ("[Controller] state\n");
              for (int k = 0; k < N; k++) {
                printf("Process %d : {%d, %d}\n", k, newState[k][0], newState[k][1]);
              }


			  // Try to send the message with opposite value
              int opValue = 1 - msgbuffer[i].msg;
              int messageOp[3] = {1, msgbuffer[i].from, opValue};
			  ssize_t s3 = send (msgbuffer[j].connfd, &messageOp, sizeof (messageOp), 0);
			  if (s3 == -1)
			    {
			      perror ("[Controller] send to fail");
			      exit (EXIT_FAILURE);
			    }
			  // Recover the resulting state
			  int newStateOp[N][2] = {{0, 0}};
			  //sleep (2);
			  ssize_t brec4 = recv (msgbuffer[j].connfd, newStateOp, sizeof (newStateOp), 0);
			  if (brec4 == -1)
			    {
			      perror ("[Controller] recv to fail");
			      exit (EXIT_FAILURE);
			    }

			  printf ("[Controller] state\n");
              for (int k = 0; k < N; k++) {
                printf("Process %d : {%d, %d}\n", k, newStateOp[k][0], newStateOp[k][1]);
              }

			  //attention
			  close(msgbuffer[j].connfd);
			}
		    }
		  close (connfd);
		}
	      i++;
	}

    }

  // accept is blocking so this is never reached 

  close (sockfd);
  unlink (CONTROLLER_PATH);

  while (wait (NULL) != -1);

  sem_close(sem);
  sem_unlink("/sem_bv_broadcast");  // Cleanup the semaphore

  return 0;

}
