/* Copyright (c) 2026, CK Tan.
 * https://github.com/cktan/swimc17/blob/main/LICENSE
 */
#ifndef SWIM_UDP_H
#define SWIM_UDP_H

#include "swim.h"
#include "swim_node_id.h"

typedef struct swim_udp_t swim_udp_t;

/**
 * Create and bind a non-blocking UDP socket.
 * Supports both IPv4 and IPv6 local addresses.
 *
 * @param host The local address to bind to.
 * @param port The local port to bind to.
 * @return a new swim_udp_t instance, or NULL on failure.
 */
SWIM_EXTERN swim_udp_t *swim_udp_create(const char *host, uint16_t port);

/**
 * Destroy the UDP transport and close the socket.
 *
 * @param u The UDP transport instance.
 */
SWIM_EXTERN void swim_udp_destroy(swim_udp_t *u);

/**
 * Send a packet to a target destination.
 *
 * @param u    The UDP transport instance.
 * @param dest The target node ID (contains host and port).
 * @param buf  The binary payload to send.
 * @param size The size of the payload.
 * @return 0 on success, -1 on failure.
 */
SWIM_EXTERN int swim_udp_send(swim_udp_t *u, const swim_node_id_t *dest,
                              const uint8_t *buf, size_t size);

/**
 * Receive a packet. Non-blocking.
 *
 * @param u       The UDP transport instance.
 * @param out_src Pointer to a swim_node_id_t to receive the sender's host/port.
 * @param buf     Buffer to store the received payload.
 * @param size    Size of the buffer.
 * @return The number of bytes received on success, 0 if no data is available
 * (EWOULDBLOCK), -1 on error.
 */
SWIM_EXTERN int swim_udp_recv(swim_udp_t *u, swim_node_id_t *out_src,
                              uint8_t *buf, size_t size);

/**
 * Return the underlying socket file descriptor.
 *
 * @param u The UDP transport instance.
 * @return The socket file descriptor, or -1.
 */
SWIM_EXTERN int swim_udp_fd(const swim_udp_t *u);

#ifndef NDEBUG
/**
 * Simulate packet loss on a UDP port. pct is clamped to 0..100.
 * Each call to swim_udp_send on that port drops the packet with
 * probability pct/100. Debug builds only.
 */
SWIM_EXTERN void swim_udp_set_packet_loss(int port, int pct);

/**
 * Install a drop filter. Called before each send with (src_port,
 * dst_port); return non-zero to drop the packet. Replaces any
 * previously installed filter. Pass NULL to remove. Debug builds
 * only.
 */
typedef int (*swim_udp_drop_fn)(int src_port, int dst_port);
SWIM_EXTERN void swim_udp_set_drop_filter(swim_udp_drop_fn fn);

/**
 * Clear all packet loss settings and the drop filter. Debug builds
 * only.
 */
SWIM_EXTERN void swim_clear_udp_loss(void);
#endif

#endif // SWIM_UDP_H
