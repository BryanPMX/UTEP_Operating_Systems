/*

  University of Texas at El Paso, Department of Computer Science.

  Contributor: Bryan Perez
  
*/

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#include <netdb.h>

static ssize_t my_write(int fd, const void *buf, size_t count) {
  size_t to_be_written, already_written, written_this_time;
  ssize_t tmp;

  if (count == ((size_t) 0)) return (ssize_t) 0;
  
  for (to_be_written=count, already_written=(size_t) 0;
       to_be_written > (size_t) 0;
       to_be_written -= written_this_time, already_written += written_this_time) {
    tmp = write(fd, buf+already_written, to_be_written);
    if (tmp < ((ssize_t) 0)) return tmp;
    written_this_time = (size_t) tmp;
  }
  return (ssize_t) already_written;
}

static int int_fits_uint16(uint16_t *res, int i) {
  uint16_t t;
  int tt;

  if (i < 0) return 0;
  t = (uint16_t) i;
  tt = (int) t;
  if (tt < 0) return 0;
  if (tt != i) return 0;
  *res = t;
  return 1;
}

static int size_fits_uint16(uint16_t *res, size_t s) {
  uint16_t t;
  size_t tt;

  t = (uint16_t) s;
  tt = (size_t) t;
  if (tt != s) return 0;
  *res = t;
  return 1;
}

static int connect_to_server(char *server_name, char *port_name) {
  struct addrinfo hints;
  int gai_code;
  struct addrinfo *result;
  struct addrinfo *curr;
  int found;
  int sockfd;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = 0;
  hints.ai_flags = 0;
  result = NULL;
  gai_code = getaddrinfo(server_name, port_name, &hints, &result);
  if (gai_code != 0) {
    fprintf(stderr, "Could not look up server address info for %s %s\n",
	    server_name, port_name);
    return -1;
  }
  found = 0;
  for (curr=result; curr!=NULL; curr=curr->ai_next) {
    sockfd = socket(curr->ai_family, curr->ai_socktype, curr->ai_protocol);
    if (sockfd >= 0) {
      if (connect(sockfd, curr->ai_addr, curr->ai_addrlen) >= 0) {
	found = 1;
	break;
      } else {
	if (close(sockfd) < 0) {
	  fprintf(stderr, "Could not close a socket: %s\n",
		  strerror(errno));
	  freeaddrinfo(result);
	  return -1;
	}
      }
    }
  }
  freeaddrinfo(result);
  if (!found) {
    fprintf(stderr, "Could not connect to any possible results for %s %s\n",
	    server_name, port_name);
    return -1;
  }
  return sockfd;
}

static int copy_data_fds(int infd, int outfd) {
  ssize_t read_res;
  size_t len;
  char buf[4096];

  read_res = read(infd, buf, sizeof(buf));
  if (read_res < ((ssize_t) 0)) {
    fprintf(stderr, "Cannot read: %s\n",
	    strerror(errno));
    return -1;
  }
  if (read_res == ((ssize_t) 0)) {
    return 0;
  }
  len = (size_t) read_res;
  if (my_write(outfd, buf, len) < ((ssize_t) 0)) {
    fprintf(stderr, "Cannot write: %s\n",
	    strerror(errno));
    return -1;
  }
  return 1;
}

static int use_client(int fd) {
  int res_select;
  int nfds;
  fd_set rdfds;
  int res;

  for (;;) {
    nfds = fd;
    nfds++;
    FD_ZERO(&rdfds);
    FD_SET(0, &rdfds);
    FD_SET(fd, &rdfds);
    res_select = select(nfds, &rdfds, NULL, NULL, NULL);
    if (res_select < 0) {
      fprintf(stderr, "Could not use select: %s\n",
	      strerror(errno));
      return -1;
    }
    if (res_select > 0) {
      if (FD_ISSET(0, &rdfds)) {
	res = copy_data_fds(0, fd);
	if (res < 0) return -1;
	if (res == 0) return 0;
      }
      if (FD_ISSET(fd, &rdfds)) {
	res = copy_data_fds(fd, 1);
	if (res < 0) return -1;
	if (res == 0) return 0;
      }
    }
  }
  return 0;
}

static int run_client(char *server_name, char *port_name,
		      uint16_t remote_argc, char **remote_argv) {
  int fd;
  uint16_t remote_argc_n;
  size_t l;
  uint16_t len;
  uint16_t len_n;
  int32_t i;

  fd = connect_to_server(server_name, port_name);
  if (fd < 0) return -1;

  remote_argc_n = htons(remote_argc);
  if (my_write(fd, &remote_argc_n, sizeof(remote_argc_n)) < ((ssize_t) 0)) {
    fprintf(stderr, "Cannot not write: %s\n",
	    strerror(errno));
    goto close_and_fail;
  }
  for (i=((int32_t) 0);i<((int32_t) remote_argc);i++) {
    l = strlen(remote_argv[i]);
    len = (uint16_t) l;
    len_n = htons(len);
    if (my_write(fd, &len_n, sizeof(len_n)) < ((ssize_t) 0)) {
      fprintf(stderr, "Cannot not write: %s\n",
	      strerror(errno));
      goto close_and_fail;
    }
    if (my_write(fd, remote_argv[i], l) < ((ssize_t) 0)) {
      fprintf(stderr, "Cannot not write: %s\n",
	      strerror(errno));
      goto close_and_fail;
    }
  }

  if (use_client(fd) < 0) {
    goto close_and_fail;
  }

  if (close(fd) < 0) {
    fprintf(stderr, "Cannot not use close: %s\n",
	    strerror(errno));
    return -1;
  }
  
  return 0;
 close_and_fail:
  if (close(fd) < 0) {
    fprintf(stderr, "Cannot not use close: %s\n",
	    strerror(errno));
  }
  return -1;
}

int main(int argc, char **argv) {
  char *server_name;
  char *port_name;
  int remote_argc;
  char **remote_argv;
  uint16_t remote_argc_u16;
  uint16_t dummy_u16;
  int i;
  
  if (argc < 4) {
    fprintf(stderr,
	    "Not enough arguments\n\n"
	    "Usage: %s <server address> <server port> <remote command> [<arg> [<arg> ...]]\n",
	    (argc > 0) ? argv[0] : "client");
    return 1;
  }
  server_name = argv[1];
  port_name = argv[2];
  remote_argc = argc - 3;
  remote_argv = &argv[3];
  
  if (!int_fits_uint16(&remote_argc_u16, remote_argc)) {
    fprintf(stderr, "Too many arguments\n");
    return 1;
  }

  for (i=0;i<remote_argc;i++) {
    if (!size_fits_uint16(&dummy_u16, strlen(remote_argv[i]))) {
      fprintf(stderr, "Argument too long\n");
      return 1;
    }
  }

  if (run_client(server_name, port_name, remote_argc_u16, remote_argv) < 0) {
    return 1;
  }
  
  return 0;
}