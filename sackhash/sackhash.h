#ifndef _SACKHASH_H_
#define _SACKHASH_H_

#include "hashtable.h"
#include "siphash.h"
#include "linkedlist.h"
#include "containerof.h"
#include <pthread.h>

struct sack_hash_data {
  uint16_t mss;
  uint8_t sack_supported;
};

// for fast comparisons
struct ipport {
  uint64_t ipport1;
  uint64_t ipport2;
  uint64_t ipport3;
};

struct sack_ip_port_hash_entry {
  struct hash_list_node node;
  struct linked_list_node llnode;
  struct ipport ipport;
  struct sack_hash_data data;
};

#define SACK_HASH_READ_MTX_CNT 128

struct sack_ip_port_hash {
  // Lock order, mtx first, then read_mtx.
  pthread_mutex_t read_mtx[SACK_HASH_READ_MTX_CNT];
  pthread_mutex_t mtx;
  struct hash_table hash;
  struct linked_list_head list;
};

int sack_ip_port_hash_init(
  struct sack_ip_port_hash *hash, size_t capacity);

int sack_ip_port_hash_add4(
  struct sack_ip_port_hash *hash, uint32_t ip, uint16_t port,
  const struct sack_hash_data *data);

int sack_ip_port_hash_get4(
  struct sack_ip_port_hash *hash, uint32_t ip, uint16_t port,
  struct sack_hash_data *data);

int sack_ip_port_hash_add6(
  struct sack_ip_port_hash *hash, const void *ip, uint16_t port,
  const struct sack_hash_data *data);

int sack_ip_port_hash_get6(
  struct sack_ip_port_hash *hash, const void *ip, uint16_t port,
  struct sack_hash_data *data);

void sack_ip_port_hash_free(struct sack_ip_port_hash *hash);

#endif
