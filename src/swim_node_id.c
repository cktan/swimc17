#include "swim_protocol.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int swim_node_id_format(const swim_node_id_t *id, char *buf, size_t size) {
  if (!id || !buf || size == 0) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid arguments to swim_node_id_format");
  }
  int n;
  bool is_ipv6 = (strchr(id->host, ':') != NULL);
  if (id->cookie[0] != '\0') {
    n = snprintf(buf, size, is_ipv6 ? "[%s]:%u/%s" : "%s:%u/%s", id->host,
                 id->port, id->cookie);
  } else {
    n = snprintf(buf, size, is_ipv6 ? "[%s]:%u" : "%s:%u", id->host, id->port);
  }

  if (n < 0 || (size_t)n >= size) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Buffer too small to format node ID");
  }
  return 0;
}

int swim_node_id_parse(swim_node_id_t *id, const char *str) {
  if (!id || !str) {
    return swim_set_error(SWIM_ERR_INVALID,
                          "Invalid NULL argument to swim_node_id_parse");
  }

  /* Split on '/' to separate host:port from optional cookie. */
  const char *slash = strchr(str, '/');
  const char *hostport_end = slash ? slash : str + strlen(str);

  if (slash) {
    if (strlen(slash + 1) >= sizeof(id->cookie)) {
      return swim_set_error(SWIM_ERR_INVALID, "Cookie too long in '%s'", str);
    }
    strcpy(id->cookie, slash + 1);
  } else {
    id->cookie[0] = '\0';
  }

  const char *host_start;
  const char *host_end;
  const char *port_start;

  if (*str == '[') {
    const char *bracket_end = strchr(str, ']');
    if (!bracket_end || bracket_end >= hostport_end ||
        *(bracket_end + 1) != ':') {
      return swim_set_error(SWIM_ERR_INVALID,
                            "Invalid IPv6 bracket format in '%s'", str);
    }
    host_start = str + 1;
    host_end = bracket_end;
    port_start = bracket_end + 2;
  } else {
    const char *colon = NULL;
    for (const char *p = str; p < hostport_end; p++) {
      if (*p == ':')
        colon = p;
    }
    if (!colon) {
      return swim_set_error(SWIM_ERR_INVALID,
                            "Missing port delimiter ':' in '%s'", str);
    }
    host_start = str;
    host_end = colon;
    port_start = colon + 1;
  }

  size_t host_len = host_end - host_start;
  if (host_len >= sizeof(id->host)) {
    return swim_set_error(SWIM_ERR_INVALID, "Host name too long in '%s'", str);
  }
  memcpy(id->host, host_start, host_len);
  id->host[host_len] = '\0';

  char *endptr;
  long port_val = strtol(port_start, &endptr, 10);
  if (endptr != hostport_end || port_val <= 0 || port_val > 65535) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid port value in '%s'", str);
  }
  id->port = (uint16_t)port_val;

  return 0;
}
