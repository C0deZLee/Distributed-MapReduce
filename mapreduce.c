/*
 * Implementation file for simple MapReduce framework.  Fill in the functions
 * and definitions in this file to complete the assignment.
 *
 * Place all of your implementation code in this file.  You are encouraged to
 * create helper functions wherever it is necessary or will make your code
 * clearer.  For these functions, you should follow the practice of declaring
 * them "static" and not including them in the header file (which should only be
 * used for the *public-facing* API.
 */


/* Header includes */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <fcntl.h>
#include "mapreduce.h"

/* Size of shared memory buffers */
#define MR_BUFFER_SIZE 1024

// The args for map/reduce function
struct args_helper {
 struct map_reduce *mr;
 int infd, outfd, nmaps, id;
 map_fn map;
 reduce_fn reduce;
};

/* Helper function that can be passed to the pthread_create to call the map_fn
 */
static void *map_wrapper(void* map_args) {
  // Reconstruct the Arguments
  struct args_helper *args = (struct args_helper *) map_args;
  // Call the map function and save the return value
  args->mr->mapfn_status[args->id] =
      args->map(args->mr, args->infd,  args->id, args->nmaps);
  // Send a signal to mr_consume after the function returns
  pthread_cond_signal(&args->mr->not_empty[args->id]);
  return NULL;
}

/* Helper function that can be passed to the pthread_create to call the reduce_fn
 */
static void *reduce_wrapper(void* reduce_args) {
  // Reconstruct the Arguments
  struct args_helper *args = (struct args_helper *) reduce_args;
  // Call the reduce function and save the return value
  args->mr->reducefn_status =
    args->reduce(args->mr, args->outfd, args->nmaps);
  return NULL;
}

/*
Refs:
http://stackoverflow.com/questions/29350073/invalid-write-of-size-8-after-a-malloc
*/
struct map_reduce*
mr_create(map_fn map, reduce_fn reduce, int nmaps) {
   struct map_reduce *mr = malloc (sizeof(struct map_reduce));

   if(mr == 0) {  // Check Success
     free(mr);
     return NULL;
   }
   // Check if it's server or client
   if(map == NULL){
     //client
     mr->client = true;
     mr->server = false;
   }
   else if (reduce == NULL){
     //server
     mr->client = false;
     mr->server = true;
   }
   else return NULL;
   // Save the Parameters
   mr->map             = map;
   mr->reduce          = reduce;
   mr->n_threads       = nmaps;

   // File Descriptors
   mr->outfd           = -1;
   mr->infd            = malloc(nmaps * sizeof(int));
   mr->server_sockfd   = -1;
   mr->client_sockfd   = malloc(nmaps * sizeof(int));
   for(int i=0; i<nmaps; i++)
      mr->client_sockfd[i] = 0;

   // Threads
   mr->map_threads     = malloc(nmaps * sizeof(pthread_t));
   mr->mapfn_status    = malloc(nmaps * sizeof(int));
   mr->reducefn_status = -1;

   // Arguments of Funtion Wappers
   mr->args            = malloc((nmaps + 1) * sizeof(struct args_helper));

   // Lock & Conditional Variables
   mr->_lock           = malloc(nmaps * sizeof(pthread_mutex_t));
   mr->not_full        = malloc(nmaps * sizeof(pthread_cond_t));
   mr->not_empty       = malloc(nmaps * sizeof(pthread_cond_t));

   for (int i=0; i<nmaps; i++) {  // Init
       pthread_mutex_init(&mr->_lock[i], NULL);
       pthread_cond_init(&mr->not_full[i], NULL);
       pthread_cond_init(&mr->not_empty[i], NULL);
   }

   // Init the Buffer List
   mr->buffer = malloc(nmaps * sizeof(char*));
   mr->size   = malloc(nmaps * sizeof(int));

   for(int i = 0; i < nmaps; i++){
     mr->buffer[i] = malloc(MR_BUFFER_SIZE * sizeof(char));
     mr->size[i] = 0;
   }
	 return mr;
}


/* Destroys and cleans up an existing instance of the MapReduce framework */
void
mr_destroy(struct map_reduce *mr) {
  for(int i=0; i<mr->n_threads; i++){
    free(mr->buffer[i]);
  }
  free(mr->buffer);
  free(mr->infd);
  free(mr->map_threads);
  free(mr->mapfn_status);
  free(mr->not_full);
  free(mr->not_empty);
  free(mr->_lock);
  free(mr->size);
  free(mr->args);
  free(mr);
}

/* Begins a multithreaded MapReduce operation */
int
mr_start(struct map_reduce *mr, const char *path, const char *ip, uint16_t port)
{
  if(mr->server) {
    int opt = 1, // For socket setting
        max_fd = -1,
        activity;
    fd_set readfds; // Set of socket descriptors

  	// Open the output file
    mr->outfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 644);

  	// mr->outfd = socket(AF_INET, SOCK_STREAM, 0);
  	if (mr->outfd < 0) {
	     close(mr->outfd);
	     perror("Server: Cannot open ouput file.\n");
	     return -1;
	  }

    // Open the socket
    mr->server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (mr->socket < 0) {
      close(mr->outfd);
      close(mr->server_sockfd);
      perror("Server: Cannot open socket.\n");
      return -1;
    }

    // Set server_socket to allow mutiple connections
    if(setsockopt(mr->server_sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0){
        perror("Server: Cannot set socket.\n");
        return -1;
    }

    // Setup the address info
    mr->server_addr.sin_family = AF_INET;
    mr->server_addr.sin_port = htons(port);
    mr->server_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind the socket and address
    if (bind(mr->server_sockfd, (struct sockaddr *) &mr->server_addr, sizeof(struct sockaddr)) == -1) {
      perror("Server: Cannot bind socket.\n");
      return -1;
    }

    // Start Listen
    if (listen(mr->server_sockfd, mr->n_threads) == -1 ) {
      perror("Server: Cannot start socket listen.\n");
      return -1;
    }
    printf("Waiting for connections.\n");

    //http://www.binarytides.com/multiple-socket-connections-fdset-select-linux/
    // Connect all the clients
    while(1) {
      //clear the socket set
      FD_ZERO(&readfds);

      //add server socketfd to set
      FD_SET(mr->server_sockfd, &readfds);
      max_fd = mr->server_sockfd;

      //add child sockets to set
      for (int i=0; i<mr->n_threads; i++){
        if(mr->client_socket[i] > 0)
          FD_SET(mr->client_socket[i], &readfds);

        //highest file descriptor number, need it for the select function
        if(mr->client_socket[i] > max_sd)
            max_sd = mr->client_socket[i];
      }

      //wait for an activity on one of the sockets , timeout is NULL , so wait indefinitely
      activity = select( max_sd + 1 , &readfds , NULL , NULL , NULL);

      if ((activity < 0) && (errno!=EINTR)) {
          perror("select error");
          return -1;
      }

      //If something happened on the master socket , then its an incoming connection
      if (FD_ISSET(master_socket, &readfds))
      {
          if ((new_socket = accept(master_socket, (struct sockaddr *)&address, (socklen_t*)&addrlen))<0)
          {
              perror("accept");
              exit(EXIT_FAILURE);
          }

          //inform user of socket number - used in send and receive commands
          printf("New connection , socket fd is %d , ip is : %s , port : %d \n" , new_socket , inet_ntoa(address.sin_addr) , ntohs(address.sin_port));
          //add new socket to array of sockets
          for (i = 0; i < max_clients; i++)
          {
              //if position is empty
              if( client_socket[i] == 0 )
              {
                  client_socket[i] = new_socket;
                  printf("Adding to list of sockets as %d\n" , i);

                  break;
              }
          }
      }
      //else its some IO operation on some other socket :)
      for (i = 0; i < max_clients; i++)
      {
          sd = client_socket[i];

          if (FD_ISSET( sd , &readfds))
          {
              //Check if it was for closing , and also read the incoming message
              if ((valread = read( sd , buffer, 1024)) == 0)
              {
                  //Somebody disconnected , get his details and print
                  getpeername(sd , (struct sockaddr*)&address , (socklen_t*)&addrlen);
                  printf("Host disconnected , ip %s , port %d \n" , inet_ntoa(address.sin_addr) , ntohs(address.sin_port));

                  //Close the socket and mark as 0 in list for reuse
                  close( sd );
                  client_socket[i] = 0;
              }

              //Echo back the message that came in
              else
              {
                  //set the string terminating NULL byte on the end of the data read
                  buffer[valread] = '\0';
                  send(sd , buffer , strlen(buffer) , 0 );
              }
          }
      }


      inet_len = sizeof(caddr);
      if ((client = accept(server, (struct sockaddr *)&caddr, &inet_len)) == -1 ) {
          printf( "Error on client accept [%s]\n", strerror(errno) );
          close(server);
          return -1;
      }
    }

    // Construct the reduce arguments
    struct args_helper *reduce_args;
  	reduce_args         = &(mr->args[mr->n_threads]);
  	reduce_args->mr     = mr;
  	reduce_args->reduce = mr->reduce;
  	reduce_args->map    = mr->map;
  	reduce_args->outfd  = mr->outfd;
  	reduce_args->nmaps  = mr->n_threads;

    //set up a socket to listen for connections on the given IP address and port.
    //Once this is ready, it should launch a thread to carry out the reduce operation.
    //This thread should wait and accept connections until it has one from each mapper

  	// Create reduce thread
	  if (pthread_create(&mr->reduce_thread, NULL, &reduce_wrapper, (void *)reduce_args) != 0) {
	    perror("Server: Failed to create reduce thread.\n");
	    return -1;
    }

  	// Success
  	return 0;
  } else if(mr->client) {
  // =======================================================================
  // Client Part
	struct args_helper *map_args;

	// Create n threads for map function (n = n_threads)
	for(int i=0; i<(mr->n_threads); i++) {

	 //Assign different socketfd to every map thread
        mr->infd[i] = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 644);
	if (mr->infd[i] == -1) {
	  	close(mr->infd[i]);
	  	perror("Client: Cannot open input file\n");
	  	return -1;
	 }
         mr->client_sockfd[i] = socket(AF_INET, SOCK_STREAM, 0);
         if (mr->client_sockfd[i] == -1){
            close(mr->infd[i];
            close(mr->client_sockfd[i]);
            perror("Client: Cannot open socket.\n");
        }
	// Give map status a init value
	mr->mapfn_status[i] = -1;

	// Construct the map arguments
	map_args         = &(mr->args[i]);
	map_args->mr     = mr;
	map_args->map    = mr->map;
	map_args->reduce = mr->reduce;
	map_args->infd   = mr->infd[i];
	map_args->id     = i;
	map_args->nmaps  = mr->n_threads;

	// Create map threads

    int portno = port;
    struct sockaddr_in serv_addr;
    struct hostent *server;

    if(inet_aton(ip, &serv_addr) == 0){
        return -1;

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
         (char *)&serv_addr.sin_addr.s_addr,
         server->h_length);
    serv_addr.sin_port = htons(portno);


   } //http://www.cse.psu.edu/~djp284/cmpsc311-s15/slides/25-networking.pdf

    if (connect(mr->client_sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr)) < 0)
        perror("ERROR connecting");

    int mapper_id = htonl (i);
    if (send (mr->client_sockfd[i], &mapper_id, sizeof (mapper_id), 0) < 0)
		perror ("ERROR sending value");


    printf ("Client: closing connection\n");
    close (mr->client_sockfd);

    if(pthread_create(&mr->map_threads[i], NULL, &map_wrapper, (void *)map_args) != 0) {


	perror("Failed to create map thread.\n");
	return -1;
    	}
    	// Success
    	return 0;
  } else return -1;
}

  // Create thread for reduce function
  // Assign file descriptor
 /* mr->outfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 644);
  if (mr->outfd == -1) {
    close(mr->outfd);
    perror("Cannot open output file\n");
    return -1;
  }
*/

  // Success
	return 0;
}

/* Blocks until the entire MapReduce operation is complete */
int
mr_finish(struct map_reduce *mr) {

  // Close Threads
  for(int i=0; i<(mr->n_threads); i++) {
    if(pthread_join(mr->map_threads[i], NULL)) {
      perror("Failed to wait a map thead end.\n");
      return -1;
    }
  }
  if(pthread_join(mr->reduce_thread, NULL)) {
    perror("Failed to wait a map thead end.\n");
    return -1;
  }

  // Close the File Descriptors
 /* for(int i=0; i<(mr->n_threads); i++) {
    mr->infd_failed[i] = close(mr->infd[i]);
  }
  mr->outfd_failed = close(mr->outfd);
*/
  // Check
  for(int i=0; i<(mr->n_threads); i++) {
    if (mr->reducefn_status !=  0 ||
        mr->mapfn_status[i] !=  0  )
      return -1;
  }

  return 0; //success
}

/* Called by the Map function each time it produces a key-value pair */
int
mr_produce(struct map_reduce *mr, int id, const struct kvpair *kv) {
  // Lock
  pthread_mutex_lock(&mr->_lock[id]);
  // Get the kv_pair size
  int kv_size = kv->keysz + kv->valuesz + 8;

  // First check if the buffer is overflow
  while((mr->size[id]+kv_size) >= MR_BUFFER_SIZE) {
    pthread_cond_wait(&mr->not_full[id], &mr->_lock[id]); // wait
  }

  // Copy the value
  memmove(&mr->buffer[id][mr->size[id]], &kv->keysz, 4);
	mr->size[id] += 4;
	memmove(&mr->buffer[id][mr->size[id]], kv->key, kv->keysz);
	mr->size[id] += kv->keysz;
	memmove(&mr->buffer[id][mr->size[id]], &kv->valuesz, 4);
	mr->size[id] += 4;
	memmove(&mr->buffer[id][mr->size[id]], kv->value, kv->valuesz);
	mr->size[id] += kv->valuesz;

  //Send the signal
  pthread_cond_signal (&mr->not_empty[id]);
  // Unlock
  pthread_mutex_unlock(&mr->_lock[id]);
  // Success
	return 1;
}

/* Called by the Reduce function to consume a key-value pair */
int
mr_consume(struct map_reduce *mr, int id, struct kvpair *kv) {
  pthread_mutex_lock(&mr->_lock[id]); // lock


  // Check the size to make sure there is a value
  while(mr->size[id] <= 0) {
    if(mr->mapfn_status[id] == 0) // Map function done its work
      return 0;
    // Wait for signal
    pthread_cond_wait(&mr->not_empty[id], &mr->_lock[id]);
  }

  // Copy the value
  int offset = 0;
  memmove(&kv->keysz, &mr->buffer[id][offset], 4);
	offset += 4;
	memmove(kv->key, &mr->buffer[id][offset], kv->keysz);
	offset += kv->keysz;
	memmove(&kv->valuesz, &mr->buffer[id][offset], 4);
	offset += 4;
	memmove(kv->value, &mr->buffer[id][offset], kv->valuesz);
	offset += kv->valuesz;

  // Decrease size
  mr->size[id] -= offset;
  memmove(&mr->buffer[id][0], &mr->buffer[id][offset], (MR_BUFFER_SIZE - offset));

  // Send Signal
  pthread_cond_signal (&mr->not_full[id]);
  // Unlock
  pthread_mutex_unlock(&mr->_lock[id]);
  // Success
	return 1;
}
