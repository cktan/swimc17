#ifndef SWIM_NODE_ID_H
#define SWIM_NODE_ID_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include "swim_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char host[256]; // RFC 1035 hostnames can be up to 253 characters, plus space for IPv6 addresses
  uint16_t port;
  char cookie[64];
} swim_node_id_t;



/**
 * Compare two node IDs for sorting/equality.
 *
 * @param a First node ID.
 * @param b Second node ID.
 * @return negative if a < b, 0 if a == b, positive if a > b.
 */
static inline int swim_node_id_compare(const swim_node_id_t *a, const swim_node_id_t *b) {
  assert(a && b);
  int r = strcmp(a->host, b->host);
  r = (r ? r : (a->port != b->port) ? ((a->port < b->port) ? -1 : 1) : 0);
  r = (r ? r : strcmp(a->cookie, b->cookie));
  return r;
}

/**
 * Format a node ID into a string buffer.
 * If cookie is empty, formats as "host:port".
 * If cookie is present, formats as "host:port:cookie".
 * Wrapped in brackets if the host is an IPv6 address.
 *
 * @param id   The node ID to format.
 * @param buf  The output buffer.
 * @param size The size of the output buffer.
 * @return 0 on success, -1 if the buffer is too small.
 */
int swim_node_id_format(const swim_node_id_t *id, char *buf, size_t size);

/**
 * Parse a node ID from a string formatted as "host:port" or "host:port:cookie".
 * Bracketed IPv6 hosts (e.g., "[::1]:8080:cookie") are supported.
 *
 * @param id  Pointer to the swim_node_id_t to populate.
 * @param str The string to parse.
 * @return 0 on success, -1 on parsing failure.
 */
int swim_node_id_parse(swim_node_id_t *id, const char *str);

#ifdef __cplusplus
}
#endif

#endif // SWIM_NODE_ID_H
