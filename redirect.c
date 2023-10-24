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

  // If a connection = socket between this process and the controller 
  // doesn't exist, create it
  if (controller_socket == 0)
    {
      struct sockaddr_un address;
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
  else
    {
      // connection already exist, just use it

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

  if (!real_send)
    {
      real_send = dlsym (RTLD_NEXT, "send");
    }

  struct sockaddr_un address;

  // Create a socket to the controller
  // Right now no need but maybe check if already one to reuse

  if ((controller_socket = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
      perror ("[Intercept] socket");
      exit (EXIT_FAILURE);
    }


  memset (&address, 0, sizeof (address));
  address.sun_family = AF_UNIX;
  strncpy (address.sun_path, CONTROLLER_PATH, sizeof (address.sun_path) - 1);

  // Get the timeout value set in the receiver process
  struct timeval tv;
  socklen_t l = sizeof (tv);
  if (getsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, &l) < 0)
    {
      perror ("[Intercept] getsockopt");
      exit (EXIT_FAILURE);
    }

  if (connect
      (controller_socket, (struct sockaddr *) &address,
       sizeof (struct sockaddr_un)) == -1)
    {
      perror ("[Intercept] connect");
      exit (EXIT_FAILURE);
    }

  // Send a message to the controller that this process is ready to receive
  // Right now the instruction is read directly from the message value, but
  // the idea is to include this in format : 
  // msg = this process is ready to receive from a certain sender process
  printf ("[Intercept] send to controller\n");
  char *msg = "rec";

  ssize_t bytes_sent = real_send (controller_socket, msg, sizeof (msg), 0);

  // Here we have different path
  // Loop + fork to explore
  // Right now number of path to explore is hardcoded
  // maybe receive this info from the controller (knows how many
  // options it wants to try) 
  for (int j = 0; j < 2; j++)
    {
      printf ("[Intercept] recv from controller and fork\n");

      int pid = fork ();
      if (pid < 0)
	{
	  perror ("[Intercept] fork");
	  exit (EXIT_FAILURE);
	}
      if (pid == 0)
	{
	  // The child process continues the execution with the msg value
	  // received by the controller  

	  ssize_t bytes_received =
	    real_recv (controller_socket, buf, len, flags);

	  // Check if we received a legit message or a timeout
	  // maybe just put every time a legit message and a 
	  // different delay
	  // if the delay is superior to the socket time out 
	  // that we recover then trigger time out
	  // otherwise continue
	  // Right now timeout is hardcoded but idea is to 
	  // receive msg = data + delay
	  // then unpack, check delay > time out
	  int t;
	  sscanf ((char *) buf, "%d", &t);
	  if (t == 15 && t > tv.tv_sec)
	    {
	      // Time out
	      printf ("[Intercept] timeout triggered\n");
	      bytes_received = -1;
	      errno = EWOULDBLOCK;
	      return bytes_received;
	    }


	  // No time out

	  return bytes_received;
	}

    }

  // wait for the children to terminate the exploration before we move on 
  // with the execution
  // Necessary ?
  // For now moving on is just return 0 and is handled in the receiver
  // Potentially wait for a message from the controller to set the value
  // that we want to continue th execution with.
  // Can select the value that gives the most interesting state.
  // Would need mapping (state -> msg that results in this state)
  // in the controller
  while (wait (NULL) != -1);
  printf ("[Intercept] every child for state exploration has finished\n");
  close (controller_socket);
  return 0;

}
