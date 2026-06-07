#include "swim_node_id.h"
#include "swim_errno.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>





int swim_node_id_format(const swim_node_id_t *id, char *buf, size_t size) {
  if (!id || !buf || size == 0) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid arguments to swim_node_id_format");
  }
  int n;
  bool is_ipv6 = (strchr(id->host, ':') != NULL);
  if (id->cookie[0] != '\0') {
    n = snprintf(buf, size, is_ipv6 ? "[%s]:%u:%s" : "%s:%u:%s", id->host, id->port, id->cookie);
  } else {
    n = snprintf(buf, size, is_ipv6 ? "[%s]:%u" : "%s:%u", id->host, id->port);
  }

  if (n < 0 || (size_t)n >= size) {
    return swim_set_error(SWIM_ERR_INVALID, "Buffer too small to format node ID");
  }
  return 0;
}

int swim_node_id_parse(swim_node_id_t *id, const char *str) {
  if (!id || !str) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid NULL argument to swim_node_id_parse");
  }

  const char *host_start = str;
  const char *host_end = NULL;
  const char *port_start = NULL;

  if (*str == '[') {
    const char *bracket_end = strchr(str, ']');
    if (!bracket_end || *(bracket_end + 1) != ':') {
      return swim_set_error(SWIM_ERR_INVALID, "Invalid IPv6 bracket format in '%s'", str);
    }
    host_start = str + 1;
    host_end = bracket_end;
    port_start = bracket_end + 2;
  } else {
    const char *last_colon = strrchr(str, ':');
    if (!last_colon) {
      return swim_set_error(SWIM_ERR_INVALID, "Missing port delimiter ':' in '%s'", str);
    }

    char *endptr;
    long port_val = strtol(last_colon + 1, &endptr, 10);
    if (*endptr == '\0' && port_val > 0 && port_val <= 65535) {
      host_end = last_colon;
      port_start = last_colon + 1;
    } else {
      const char *prev_colon = NULL;
      for (const char *p = last_colon - 1; p >= str; p--) {
        if (*p == ':') {
          prev_colon = p;
          break;
        }
      }
      if (!prev_colon) {
        return swim_set_error(SWIM_ERR_INVALID, "Invalid port segment in '%s'", str);
      }
      char port_buf[32];
      size_t port_len = last_colon - (prev_colon + 1);
      if (port_len >= sizeof(port_buf)) {
        return swim_set_error(SWIM_ERR_INVALID, "Port segment too long in '%s'", str);
      }
      memcpy(port_buf, prev_colon + 1, port_len);
      port_buf[port_len] = '\0';
      port_val = strtol(port_buf, &endptr, 10);
      if (*endptr != '\0' || port_val <= 0 || port_val > 65535) {
        return swim_set_error(SWIM_ERR_INVALID, "Invalid port value in '%s'", str);
      }
      host_end = prev_colon;
      port_start = prev_colon + 1;
    }
  }

  size_t host_len = host_end - host_start;
  if (host_len >= sizeof(id->host)) {
    return swim_set_error(SWIM_ERR_INVALID, "Host name too long in '%s'", str);
  }
  memcpy(id->host, host_start, host_len);
  id->host[host_len] = '\0';

  char *endptr;
  long port_val = strtol(port_start, &endptr, 10);
  if (port_val <= 0 || port_val > 65535) {
    return swim_set_error(SWIM_ERR_INVALID, "Invalid port value in '%s'", str);
  }
  id->port = (uint16_t)port_val;

  if (*endptr == ':') {
    const char *cookie_str = endptr + 1;
    if (strlen(cookie_str) >= sizeof(id->cookie)) {
      return swim_set_error(SWIM_ERR_INVALID, "Cookie too long in '%s'", str);
    }
    strcpy(id->cookie, cookie_str);
  } else if (*endptr == '\0') {
    id->cookie[0] = '\0';
  } else {
    return swim_set_error(SWIM_ERR_INVALID, "Extra characters after port in '%s'", str);
  }

  return 0;
}
