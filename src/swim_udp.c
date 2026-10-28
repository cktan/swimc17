/* Copyright (c) 2026, CK Tan.
 * https://github.com/cktan/swimc17/blob/main/LICENSE
 */
/*
 * swim_udp.c — Non-blocking UDP socket abstraction.
 *
 * Creates a bound, non-blocking UDP socket (IPv4 or IPv6)
 * via getaddrinfo + bind + fcntl(O_NONBLOCK). recv returns
 * 0 instead of blocking when no data is available.
 *
 * On send, the destination is resolved with inet_pton
 * first (fast path for numeric IP literals). Hostname
 * destinations go through a per-transport cache: a
 * successful lookup is reused for 60 s. A failed refresh
 * keeps the previous address for another 60 s; a host that
 * has never resolved is retried after 5 s.
 * A lookup still blocks the protocol loop (under the
 * instance lock) on first use and on each expiry.
 *
 * recv does not report the source address; the protocol
 * replies to the sender's node ID carried in the message.
 */
#define _GNU_SOURCE
#include "swim_udp.h"
#include "swim_errno.h"
#include <stdint.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// Lifetimes of a cached hostname lookup: a resolved address is reused for
// RESOLVE_OK_MS, as is the old address after a failed refresh; a host that
// has never resolved is retried after RESOLVE_FAIL_MS.
#define RESOLVE_OK_MS 60000
#define RESOLVE_FAIL_MS 5000

// A cached hostname lookup. addr holds the address without a port; addr_len
// is 0 until a lookup has succeeded. A failed refresh keeps the old address.
typedef struct {
  char host[256];
  struct sockaddr_storage addr;
  socklen_t addr_len;
  uint64_t expires_ms; // monotonic; re-resolve at or after this time
} resolved_t;

struct swim_udp_t {
  int fd;
  int family; // address family the socket is bound to
  char host[256];
  uint16_t port;
  resolved_t *cache; // hostname destinations; IP literals bypass it
  int cache_count;
  int cache_cap;
  int resolve_count; // getaddrinfo calls for destinations, for tests
};

#ifndef NDEBUG
static int g_loss[65536];
static swim_udp_drop_fn g_drop_filter = NULL;

void swim_udp_set_packet_loss(int port, int pct) {
  if (port >= 0 && port < 65536)
    g_loss[port] = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void swim_udp_set_drop_filter(swim_udp_drop_fn fn) { g_drop_filter = fn; }

void swim_clear_udp_loss(void) {
  memset(g_loss, 0, sizeof(g_loss));
  g_drop_filter = NULL;
}

static _Atomic uint64_t g_clock_offset_ms;
static atomic_int g_resolve_failure;

int swim_udp_resolve_count(const swim_udp_t *u) { return u->resolve_count; }

void swim_udp_advance_clock(uint64_t ms) {
  atomic_fetch_add(&g_clock_offset_ms, ms);
}

void swim_udp_set_resolve_failure(int on) {
  atomic_store(&g_resolve_failure, on);
}
#endif

// Monotonic clock for cache lifetimes; tests can advance it in debug builds.
static uint64_t cache_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#ifndef NDEBUG
  now += atomic_load(&g_clock_offset_ms);
#endif
  return now;
}

// Look up a hostname destination through the cache, calling getaddrinfo only
// when the entry is new or expired. Returns the entry, or NULL if no address
// is known for host (lookup failed and nothing was cached, or out of memory).
static const resolved_t *resolve_cached(swim_udp_t *u, const char *host) {
  resolved_t *r = NULL;
  for (int i = 0; i < u->cache_count && !r; i++) {
    if (strcmp(u->cache[i].host, host) == 0)
      r = &u->cache[i];
  }
  if (!r) {
    if (strlen(host) >= sizeof(r->host))
      return NULL;
    if (u->cache_count == u->cache_cap) {
      int cap = u->cache_cap ? u->cache_cap * 2 : 8;
      resolved_t *grown = realloc(u->cache, (size_t)cap * sizeof(*grown));
      if (!grown)
        return NULL;
      u->cache = grown;
      u->cache_cap = cap;
    }
    r = &u->cache[u->cache_count++];
    memset(r, 0, sizeof(*r));
    strcpy(r->host, host);
  }

  uint64_t now = cache_now_ms();
  if (now >= r->expires_ms) {
    // Only addresses of the socket's own family are reachable from it.
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = u->family;
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    u->resolve_count++;
#ifndef NDEBUG
    if (rc == 0 && atomic_load(&g_resolve_failure)) {
      freeaddrinfo(res);
      rc = EAI_FAIL;
    }
#endif
    if (rc == 0) {
      memcpy(&r->addr, res->ai_addr, res->ai_addrlen);
      r->addr_len = res->ai_addrlen;
      r->expires_ms = now + RESOLVE_OK_MS;
      freeaddrinfo(res);
    } else {
      // With an address still in hand, wait the full lifetime before trying
      // again: a failing resolver can block every attempt for seconds.
      r->expires_ms = now + (r->addr_len ? RESOLVE_OK_MS : RESOLVE_FAIL_MS);
    }
  }
  return r->addr_len ? r : NULL;
}

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
  int family = AF_UNSPEC;
  struct addrinfo *rp;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1)
      continue;

    // Enable SO_REUSEADDR to make test binds robust
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      family = rp->ai_family;
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
  u->family = family;
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
  free(u->cache);
  free(u);
}

// Send a packet to dest. Returns 0 on success, -1 on failure.
int swim_udp_send(swim_udp_t *u, const char *host, uint16_t port,
                  const uint8_t *buf, size_t size) {
  if (!u || !host || !buf || size == 0) {
    return swim_set_error(
        SWIM_ERR_INVALID,
        "Invalid NULL or zero size arguments to swim_udp_send");
  }

#ifndef NDEBUG
  if (g_loss[u->port] > 0 && (rand() % 100) < g_loss[u->port])
    return 0;
  if (g_drop_filter && g_drop_filter(u->port, port))
    return 0;
#endif

  // Resolve the destination to a sockaddr. IP-literal hosts are built directly
  // with inet_pton; hostnames go through the per-transport cache.
  struct sockaddr_storage ss;
  socklen_t ss_len = 0;
  memset(&ss, 0, sizeof(ss));

  struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
  struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;

  if (inet_pton(AF_INET, host, &sin->sin_addr) == 1) {
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    ss_len = sizeof(*sin);
  } else if (inet_pton(AF_INET6, host, &sin6->sin6_addr) == 1) {
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(port);
    ss_len = sizeof(*sin6);
  } else {
    const resolved_t *r = resolve_cached(u, host);
    if (!r)
      return swim_set_error(SWIM_ERR_INVALID,
                            "cannot resolve destination host %s", host);
    memcpy(&ss, &r->addr, r->addr_len);
    ss_len = r->addr_len;
    if (ss.ss_family == AF_INET)
      sin->sin_port = htons(port);
    else
      sin6->sin6_port = htons(port);
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
// (EWOULDBLOCK), -1 on error.
int swim_udp_recv(swim_udp_t *u, uint8_t *buf, size_t size) {
  if (!u || !buf || size == 0) {
    return swim_set_error(
        SWIM_ERR_INVALID,
        "Invalid NULL or zero size arguments to swim_udp_recv");
  }

  ssize_t n = recvfrom(u->fd, buf, size, 0, NULL, NULL);
  if (n < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return 0; // No data available
    }
    return swim_set_error(SWIM_ERR_BAD_STATE, "recvfrom failed");
  }

  return (int)n;
}

// Return the underlying socket fd, or -1.
int swim_udp_fd(const swim_udp_t *u) { return u ? u->fd : -1; }
