#ifndef SWIM_CODEC_H
#define SWIM_CODEC_H

#include "swim_membership.h"
#include "swim.h"

#define SWIM_MSG_PING 1
#define SWIM_MSG_ACK 2
#define SWIM_MSG_PING_REQ 3
#define SWIM_MSG_FWD_ACK 4
#define SWIM_MSG_LEAVE 5

#define SWIM_MAX_PACKET_SIZE 1400
#define SWIM_MAX_EVENTS 64

typedef struct swim_message_t swim_message_t;
struct swim_message_t {
  uint8_t type;
  swim_node_id_t sender;
  uint32_t seq;
  swim_node_id_t
      peer; // target for ping_req, source for fwd_ack (ignored otherwise)
  uint8_t gossip_count;
  swim_member_t gossip[SWIM_MAX_EVENTS];
};

struct swim_gossip_queue_t;

SWIM_EXTERN int swim_pack_membership(const swim_member_t *m, uint8_t *p,
                                       uint8_t *q);

SWIM_EXTERN int swim_pack_message(uint8_t type, const swim_node_id_t *sender,
                                    uint32_t seq, const swim_node_id_t *peer,
                                    struct swim_gossip_queue_t *q,
                                    uint32_t active_members, uint8_t *buf,
                                    int bufsz);

SWIM_EXTERN int swim_unpack_message(const uint8_t *buf, size_t size,
                                    swim_message_t *msg);

#endif // SWIM_CODEC_H
