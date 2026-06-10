/*
 * swim_udp.c — Non-blocking UDP socket abstraction.
 *
 * Creates a bound, non-blocking UDP socket (IPv4 or IPv6)
 * via getaddrinfo + bind + fcntl(O_NONBLOCK). recv returns
 * 0 instead of blocking when no data is available.
 *
 * On send, the destination is resolved with inet_pton
 * first (fast path for numeric IP literals, which is the
 * common case since recv normalizes all peer addresses
 * numerically). getaddrinfo is used only when inet_pton
 * fails, i.e. for hostname destinations. This avoids
 * synchronous DNS lookups in the hot protocol loop.
 *
 * recv populates the out_src node ID with the sender's
 * numeric address and port; the cookie field is cleared
 * because the UDP layer has no knowledge of it.
 */
#define _GNU_SOURCE
#include "swim_udp.h"
#include "swim_errno.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct swim_udp_t {
  int fd;
  char host[256];
  uint16_t port;
};

#ifndef NDEBUG
static int g_loss[65536];

void swim_udp_set_packet_loss(int port, int pct) {
  if (port >= 0 && port < 65536)
    g_loss[port] = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void swim_clear_udp_loss(void) { memset(g_loss, 0, sizeof(g_loss)); }
#endif

// Create and bind a non-blocking UDP socket (IPv4 or IPv6). Returns NULL on
// failure.
swim_udp_t *swim_udp_create(const char *host, uint16_t port) {
  if (!host) {
    swim_set_error(SWIM_ERR_INVALID, "Invalid NULL host in swim_udp_create");
    return NULL;
  }

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;    // IPv4 or IPv6
  hints.ai_socktype = SOCK_DGRAM; // UDP
  hints.ai_flags = AI_PASSIVE;    // wildcard addresses if host is NULL

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
    if (fd == -1)
      continue;

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
    swim_set_error(SWIM_ERR_BAD_STATE, "Failed to bind socket to %s:%u", host,
                   port);
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

// Destroy the UDP transport and close the socket.
void swim_udp_destroy(swim_udp_t *u) {
  if (!u)
    return;
  if (u->fd != -1) {
    close(u->fd);
  }
  free(u);
}

// Send a packet to dest. Returns 0 on success, -1 on failure.
int swim_udp_send(swim_udp_t *u, const swim_node_id_t *dest, const uint8_t *buf,
                  size_t size) {
  if (!u || !dest || !buf || size == 0) {
    return swim_set_error(
        SWIM_ERR_INVALID,
        "Invalid NULL or zero size arguments to swim_udp_send");
  }

#ifndef NDEBUG
  if (g_loss[u->port] > 0 && (rand() % 100) < g_loss[u->port])
    return 0;
#endif

  // Resolve the destination to a sockaddr. IP-literal hosts (the common case
  // here, since recv normalizes every peer to a numeric address) are built
  // directly with inet_pton, avoiding a synchronous DNS lookup in the protocol
  // loop. getaddrinfo is used only for actual hostnames.
  struct sockaddr_storage ss;
  socklen_t ss_len = 0;
  memset(&ss, 0, sizeof(ss));

  struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
  struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;

  if (inet_pton(AF_INET, dest->host, &sin->sin_addr) == 1) {
    sin->sin_family = AF_INET;
    sin->sin_port = htons(dest->port);
    ss_len = sizeof(*sin);
  } else if (inet_pton(AF_INET6, dest->host, &sin6->sin6_addr) == 1) {
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(dest->port);
    ss_len = sizeof(*sin6);
  } else {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", dest->port);

    int s = getaddrinfo(dest->host, port_str, &hints, &res);
    if (s != 0) {
      return swim_set_error(SWIM_ERR_INVALID,
                            "getaddrinfo failed for destination: %s",
                            gai_strerror(s));
    }
    memcpy(&ss, res->ai_addr, res->ai_addrlen);
    ss_len = res->ai_addrlen;
    freeaddrinfo(res);
  }

  // Retry on EINTR (always — not a real failure) and EAGAIN/EWOULDBLOCK
  // (once — send buffer momentarily full on a non-blocking socket).
  ssize_t n;
  int eagain_retries = 0;
  for (;;) {
    n = sendto(u->fd, buf, size, 0, (struct sockaddr *)&ss, ss_len);
    if (n >= 0)
      break;
    if (errno == EINTR)
      continue;
    if ((errno == EAGAIN || errno == EWOULDBLOCK) && eagain_retries++ < 1)
      continue;
    break;
  }
  if (n < 0) {
    return swim_set_error(SWIM_ERR_BAD_STATE, "sendto failed");
  }

  return 0;
}

// Receive a packet (non-blocking). Returns bytes received, 0 if no data
// (EWOULDBLOCK), -1 on error. Fills out_src with sender's host/port.
int swim_udp_recv(swim_udp_t *u, swim_node_id_t *out_src, uint8_t *buf,
                  size_t size) {
  if (!u || !out_src || !buf || size == 0) {
    return swim_set_error(
        SWIM_ERR_INVALID,
        "Invalid NULL or zero size arguments to swim_udp_recv");
  }

  struct sockaddr_storage src_addr;
  socklen_t addr_len = sizeof(src_addr);

  ssize_t n =
      recvfrom(u->fd, buf, size, 0, (struct sockaddr *)&src_addr, &addr_len);
  if (n < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return 0; // No data available
    }
    return swim_set_error(SWIM_ERR_BAD_STATE, "recvfrom failed");
  }

  // Format the source address numerically (no DNS / reverse lookup). cookie is
  // cleared since the socket sender doesn't carry it.
  const char *ok = NULL;
  if (src_addr.ss_family == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&src_addr;
    ok = inet_ntop(AF_INET, &sin->sin_addr, out_src->host,
                   sizeof(out_src->host));
    out_src->port = ntohs(sin->sin_port);
  } else if (src_addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&src_addr;
    ok = inet_ntop(AF_INET6, &sin6->sin6_addr, out_src->host,
                   sizeof(out_src->host));
    out_src->port = ntohs(sin6->sin6_port);
  }
  if (!ok) {
    return swim_set_error(SWIM_ERR_BAD_STATE,
                          "Failed to format source address");
  }
  out_src->cookie[0] = '\0';

  return (int)n;
}

// Return the underlying socket fd, or -1.
int swim_udp_fd(const swim_udp_t *u) { return u ? u->fd : -1; }
