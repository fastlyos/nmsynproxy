#ifndef _FW_H_
#define _FW_H_

#include "ports.h"
#include "packet.h"
#include "iphdr.h"
#include "log.h"
#include "hashtable.h"
#include "linkedlist.h"
#include "containerof.h"
#include "murmur.h"
#include <stdio.h>

struct synproxy {
};

struct synproxy_hash_entry {
  struct hash_list_node node;
  uint32_t local_ip;
  uint16_t local_port;
  uint32_t remote_ip;
  uint16_t remote_port;
  uint32_t flag_state;
  int8_t wscalediff;
  uint16_t window_Size;
  uint32_t isn;
  uint32_t other_isn;
  uint32_t seqoffset;
  uint32_t timestamp;
};

static inline int synproxy_is_connected(struct synproxy_hash_entry *e)
{
  // FIXME implement properly
  return (e->flag_state & 1) == 0;
}

static inline uint32_t synproxy_hash_separate(
  uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
  struct murmurctx ctx = MURMURCTX_INITER(0x12345678);
  murmurctx_feed32(&ctx, local_ip);
  murmurctx_feed32(&ctx, local_port);
  murmurctx_feed32(&ctx, remote_ip);
  murmurctx_feed32(&ctx, remote_port);
  return murmurctx_get(&ctx);
}

static inline uint32_t synproxy_hash(struct synproxy_hash_entry *e)
{
  return synproxy_hash_separate(e->local_ip, e->local_port, e->remote_ip, e->remote_port);
}

uint32_t synproxy_hash_fn(struct hash_list_node *node, void *userdata);

struct worker_local {
  struct hash_table hash;
};

static inline struct synproxy_hash_entry *synproxy_hash_get(
  struct worker_local *local,
  uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
  uint32_t hashval;
  struct hash_list_node *node;
  hashval = synproxy_hash_separate(local_ip, local_port, remote_ip, remote_port);
  HASH_TABLE_FOR_EACH_POSSIBLE(&local->hash, node, hashval)
  {
    struct synproxy_hash_entry *entry;
    entry = CONTAINER_OF(node, struct synproxy_hash_entry, node);
    if (   entry->local_ip == local_ip
        && entry->local_port == local_port
        && entry->remote_ip == remote_ip
        && entry->remote_port == remote_port)
    {
      return entry;
    }
  }
  return NULL;
}

void synproxy_hash_put(
  struct worker_local *local,
  uint32_t local_ip,
  uint16_t local_port,
  uint32_t remote_ip,
  uint16_t remote_port);

int uplink(
  struct synproxy *synproxy, struct worker_local *local, struct packet *pkt,
  struct port *port);

#endif
