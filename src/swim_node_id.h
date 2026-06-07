#ifndef SWIM_NODE_ID_H
#define SWIM_NODE_ID_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "swim_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char host[64];
  uint16_t port;
  char cookie[64];
} swim_node_id_t;

/**
 * Initialize a node ID.
 *
 * @param id      Pointer to the swim_node_id_t to initialize.
 * @param host    Host name or IP address; must not be NULL.
 * @param port    Port number.
 * @param cookie  Node cookie; can be NULL (defaults to "").
 * @return 0 on success, -1 on failure (e.g., arguments too long).
 */
int swim_node_id_init(swim_node_id_t *id, const char *host, uint16_t port,
                      const char *cookie);

/**
 * Copy a node ID from src to dst.
 *
 * @param dst Destination pointer.
 * @param src Source pointer.
 * @return 0 on success, -1 on failure.
 */
static inline int swim_node_id_copy(swim_node_id_t *dst, const swim_node_id_t *src) {
  if (!dst || !src) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid NULL argument to swim_node_id_copy");
  }
  *dst = *src;
  return 0;
}

/**
 * Compare two node IDs for sorting/equality.
 *
 * @param a First node ID.
 * @param b Second node ID.
 * @return negative if a < b, 0 if a == b, positive if a > b.
 */
static inline int swim_node_id_compare(const swim_node_id_t *a, const swim_node_id_t *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;

  int r = strcmp(a->host, b->host);
  if (r != 0) return r;

  if (a->port != b->port) {
    return (a->port < b->port) ? -1 : 1;
  }

  return strcmp(a->cookie, b->cookie);
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
