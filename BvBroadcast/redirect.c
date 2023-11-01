#define _GNU_SOURCE
#include <sys/wait.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

// Used with LD_PRELOAD to intercept the calls to send and recv 
// redirect those calls through the controller

static ssize_t (*real_send) (int sockfd, const void *buf, size_t len,
			     int flags) = NULL;
static ssize_t (*real_recv) (int sockfd, void *buf, size_t len, int flags) =
  NULL;

#define CONTROLLER_PATH "./controller_socket"

int controller_socket = 0;

int forkId = 0; // Only 0 at first for each process

// send override
ssize_t
send (int sockfd, const void *buf, size_t len, int flags)
{
  printf ("[Intercept] Enter send\n");

  // Load the real send
  if (!real_send)
    {
      real_send = dlsym (RTLD_NEXT, "send");
    }

    // If controller socket already exist use it
    // The only time it will be the case is in a child right after a recv
    // This will be used to send the state to the controller, and closed
    // right after 
    if (controller_socket == 0) {
      struct sockaddr_un address;
      int controller_socket;
      if ((controller_socket = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
	{
	  perror ("[Intercept] socket");
	  exit (EXIT_FAILURE);
	}


      memset (&address, 0, sizeof (address));
      address.sun_family = AF_UNIX;
      strncpy (address.sun_path, CONTROLLER_PATH,
	       sizeof (address.sun_path) - 1);

      if (connect
	  (controller_socket, (struct sockaddr *) &address,
	   sizeof (struct sockaddr_un)) == -1)
	{
	  perror ("[Intercept] connect");
	  exit (EXIT_FAILURE);
	}
    }

  // Send (redirect) the message to the controller
  printf ("[Intercept] Send %s\n", (char *) buf);
  ssize_t bytes_sent = real_send (controller_socket, buf, len, flags);

  close (controller_socket);

  return bytes_sent;
}


// Override recv
ssize_t
recv (int sockfd, void *buf, size_t len, int flags)
{
  printf ("[Intercept] Enter recv\n");

  // Load the real recv
  if (!real_recv)
    {
      real_recv = dlsym (RTLD_NEXT, "recv");
    }

  // We also need the real send
  if (!real_send)
    {
      real_send = dlsym (RTLD_NEXT, "send");
    }

  struct sockaddr_un address;
  int controller_socket;

  // Create a socket to the controller

  if ((controller_socket = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
      perror ("[Intercept] socket");
      exit (EXIT_FAILURE);
    }


  memset (&address, 0, sizeof (address));
  address.sun_family = AF_UNIX;
  strncpy (address.sun_path, CONTROLLER_PATH, sizeof (address.sun_path) - 1);

  if (connect
      (controller_socket, (struct sockaddr *) &address,
       sizeof (struct sockaddr_un)) == -1)
    {
      perror ("[Intercept] connect");
      exit (EXIT_FAILURE);
    }

  // For now no timeout, check in SimpleSendRecv if needed

  // Maybe send state here. That way controller can tell to kill if redondant ?
  // then need to consider the last recv controller receives, for which it
  // has no more msg to transmit, to get the final state (impacted by the
  // last actual recv just before)
  // Yeah just send back state directly I guess, for now
  // If i need to open a new socket / co in the controller just after sending
  // the message to fork, and wait for reply from child, well i will do that,
  // for now

  // Send a message to the controller that this process is ready to receive
  // Right now the instruction is read directly from the message value, but
  // the idea is to include this in format : 
  // msg = this process is ready to receive from a certain sender process
  printf ("[Intercept] send to controller\n");
  char *msg = "rec";

  ssize_t bytes_sent = real_send (controller_socket, msg, sizeof (msg), 0);

  // Wait for instructions from the controller
  // Carefull with recv, blocking but stream ? Insure that I recv the 
  // right thing. 
  // flags MSG_PEEK or MSG_WAITALL might help (using len)
  // This is Unix socket, maybe just put it SOCK_DGRAM (UDP like but
  // reliable and no reorder) instead of SOCK_STREAM (TCP like)
  
  // Message format
  int receivedMessage[2];

  pid_t children[2]; // Only 2 here no ?
  int i = 0;

  while (1) {

    ssize_t bytes_received = real_recv (controller_socket, &receivedMessage, sizeof(receivedMessage), 0);
    if (bytes_received == -1) {
        perror ("Recv failure");
        exit (EXIT_FAILURE);
    }


      int instruction = receivedMessage[0];
      int data = receivedMessage[1];
      printf ("[Intercept] recv from controller %d : %d\n", instruction, data);

      // Say instruction 1 is fork
      if (instruction == 1) {
        printf("[Intercept] fork\n");
        children[i] = fork ();
        if (children[i] < 0)
	    {
	        perror ("[Intercept] fork");
	        exit (EXIT_FAILURE);
	    }
        if (children[i] == 0)
	    {
	        // The child process continues the execution with the msg value
	        // received by the controller 

            forkId = getpid();

            // Unwrap message from controller and transmit data to process 
            int * intBuf = (int *)buf;
            intBuf[0] = data;

            // don't close controller socket so that child can send state 
            // to controller directly (this will close)
	        return bytes_received;
	    }
        i = i + 1;
      } else if (instruction == 2) { 
        printf("[Intercept] kill\n");
        // Say instruction 2 is kill
        // Would it be better to have an instruction to kill a child process ?
        // But then which one, here again only 2 options and if kill its that 
        // they are the same so any works, but more generally.. well who cares

        // how about children processes ?

        //close (controller_socket);
        //exit(0);

        // In fact here I kill the last child (same because 2 and kill if =)
        if (kill(children[i - 1], SIGTERM) < 0) {
            perror("[Intercept] kill");
            exit(EXIT_FAILURE);
        }
      }

      

    }
  
  close (controller_socket);
  return 0;

}