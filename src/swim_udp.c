#define _GNU_SOURCE
#include "swim_udp.h"
#include "swim_errno.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct swim_udp_t {
  int fd;
  char host[256];
  uint16_t port;
};

swim_udp_t *swim_udp_init(const char *host, uint16_t port) {
  if (!host) {
    swim_set_error(SWIM_ERR_INVALID, "Invalid NULL host in swim_udp_init");
    return NULL;
  }

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
  hints.ai_socktype = SOCK_DGRAM;  // UDP
  hints.ai_flags = AI_PASSIVE;     // wildcard addresses if host is NULL

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", port);

  int s = getaddrinfo(host, port_str, &hints, &res);
  if (s != 0) {
    swim_set_error(SWIM_ERR_INVALID, "getaddrinfo failed: %s", gai_strerror(s));
    return NULL;
  }

  int fd = -1;
  struct addrinfo *rp;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) continue;

    // Enable SO_REUSEADDR to make test binds robust
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break; // Success
    }

    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);

  if (fd == -1) {
    swim_set_error(SWIM_ERR_BAD_STATE, "Failed to bind socket to %s:%u", host, port);
    return NULL;
  }

  // Set O_NONBLOCK via fcntl
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    close(fd);
    swim_set_error(SWIM_ERR_BAD_STATE, "Failed to set socket non-blocking");
    return NULL;
  }

  swim_udp_t *u = calloc(1, sizeof(*u));
  if (!u) {
    close(fd);
    swim_set_error(SWIM_ERR_NOMEM, "Failed to allocate swim_udp_t instance");
    return NULL;
  }
  u->fd = fd;
  strncpy(u->host, host, sizeof(u->host) - 1);
  u->port = port;

  return u;
}

void swim_udp_final(swim_udp_t *u) {
  if (!u) return;
  if (u->fd != -1) {
    close(u->fd);
  }
  free(u);
}

int swim_udp_send(swim_udp_t *u, const swim_node_id_t *dest,
                  const uint8_t *buf, size_t size) {
  if (!u || !dest || !buf || size == 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid NULL or zero size arguments to swim_udp_send");
  }

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", dest->port);

  int s = getaddrinfo(dest->host, port_str, &hints, &res);
  if (s != 0) {
    return swim_set_error(SWIM_ERR_INVALID, "getaddrinfo failed for destination: %s", gai_strerror(s));
  }

  ssize_t n = sendto(u->fd, buf, size, 0, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);

  if (n < 0) {
    return swim_set_error(SWIM_ERR_BAD_STATE, "sendto failed");
  }

  return 0;
}

int swim_udp_recv(swim_udp_t *u, swim_node_id_t *out_src,
                  uint8_t *buf, size_t size) {
  if (!u || !out_src || !buf || size == 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid NULL or zero size arguments to swim_udp_recv");
  }

  struct sockaddr_storage src_addr;
  socklen_t addr_len = sizeof(src_addr);

  ssize_t n = recvfrom(u->fd, buf, size, 0, (struct sockaddr *)&src_addr, &addr_len);
  if (n < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return 0; // No data available
    }
    return swim_set_error(SWIM_ERR_BAD_STATE, "recvfrom failed");
  }

  // Parse host/port back from sockaddr
  char host_buf[NI_MAXHOST];
  char port_buf[NI_MAXSERV];
  int s = getnameinfo((struct sockaddr *)&src_addr, addr_len,
                      host_buf, sizeof(host_buf),
                      port_buf, sizeof(port_buf),
                      NI_NUMERICHOST | NI_NUMERICSERV);
  if (s != 0) {
    return swim_set_error(SWIM_ERR_BAD_STATE, "getnameinfo failed: %s", gai_strerror(s));
  }

  // Populate out_src (cookie cleared since socket sender doesn't carry it)
  strncpy(out_src->host, host_buf, sizeof(out_src->host) - 1);
  out_src->host[sizeof(out_src->host) - 1] = '\0';
  out_src->port = (uint16_t)atoi(port_buf);
  out_src->cookie[0] = '\0';

  return (int)n;
}

int swim_udp_fd(const swim_udp_t *u) {
  return u ? u->fd : -1;
}
