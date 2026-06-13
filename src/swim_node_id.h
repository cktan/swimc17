/* Copyright (c) 2026, CK Tan.
 * https://github.com/cktan/swimc17/blob/main/LICENSE
 */
#ifndef SWIM_NODE_ID_H
#define SWIM_NODE_ID_H

#include "swim.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct swim_node_id_t swim_node_id_t;
struct swim_node_id_t {
  char host[256]; // RFC 1035 hostnames can be up to 253 characters, plus space
                  // for IPv6 addresses
  uint16_t port;
  char cookie[64];
};

static inline int swim_node_id_compare(const swim_node_id_t *a,
                                       const swim_node_id_t *b) {
  assert(a && b);
  int r = strcmp(a->host, b->host);
  r = (r ? r : (a->port != b->port) ? ((a->port < b->port) ? -1 : 1) : 0);
  r = (r ? r : strcmp(a->cookie, b->cookie));
  return r;
}

SWIM_EXTERN int swim_node_id_format(const swim_node_id_t *id, char *buf,
                                    size_t size);

SWIM_EXTERN int swim_node_id_parse(swim_node_id_t *id, const char *str);

#endif // SWIM_NODE_ID_H
