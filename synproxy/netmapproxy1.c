#define _GNU_SOURCE
#define NETMAP_WITH_LIBS
#include <pthread.h>
#include "llalloc.h"
#include "synproxy.h"
#include "iphdr.h"
#include "ipcksum.h"
#include "packet.h"
#include "net/netmap_user.h"
#include "hashseed.h"
#include "yyutils.h"
#include "mypcapng.h"
#include "netmapports.h"
#include <unistd.h>
#include <sys/poll.h>
#include <sys/time.h>
#include "time64.h"

struct uniform_userdata {
  struct worker_local *local;
  uint32_t table_start;
  uint32_t table_end;
};

static void uniform_fn(
  struct timer_link *timer, struct timer_linkheap *heap, void *userdata)
{
  struct uniform_userdata *ud = userdata;
  uint64_t time64 = gettime64();
  uint32_t bucket;
  hash_table_lock_bucket(&ud->local->hash, ud->table_start);
  for (bucket = ud->table_start; bucket < ud->table_end; bucket++)
  {
    struct hash_list_node *node;
    HASH_TABLE_FOR_EACH_POSSIBLE(&ud->local->hash, node, bucket)
    {
      struct synproxy_hash_entry *entry;
      entry = CONTAINER_OF(node, struct synproxy_hash_entry, node);
      if (entry->timer.time64 < time64)
      {
        entry->timer.fn(&entry->timer, heap, entry->timer.userdata);
      }
    }
    if (bucket + 1 < ud->table_end)
    {
      hash_table_lock_next_bucket(&ud->local->hash, bucket);
    }
  }
  hash_table_unlock_bucket(&ud->local->hash, ud->table_end - 1);
  timer->time64 += (1000*1000);
  worker_local_wrlock(ud->local);
  timer_linkheap_add(heap, timer);
  worker_local_wrunlock(ud->local);
}

const int uniformcnt = 32;
struct uniform_userdata uniform_userdata[32] = {};
struct timer_link uniformtimer[32] = {};

#define MAX_WORKERS 64
#define MAX_RX_TX 64
#define MAX_TX 64
#define MAX_RX 64

const int num_rx = 2;

struct nm_desc *dlnmds[MAX_RX_TX], *ulnmds[MAX_RX_TX];

int in = 0;
struct pcapng_out_ctx inctx;
int out = 0;
struct pcapng_out_ctx outctx;
int lan = 0;
struct pcapng_out_ctx lanctx;
int wan = 0;
struct pcapng_out_ctx wanctx;

#define POOL_SIZE 300
#define CACHE_SIZE 100
#define QUEUE_SIZE 512
#define BLOCK_SIZE 1800

struct tx_args {
  struct queue *txq;
  int idx;
};

static inline void nm_my_inject(struct nm_desc *nmd, void *data, size_t sz)
{
  int i, j;
  for (i = 0; i < 3; i++)
  {
    for (j = 0; j < 3; j++)
    {
      if (nm_inject(nmd, data, sz) == 0)
      {
        struct pollfd pollfd;
        pollfd.fd = nmd->fd;
        pollfd.events = POLLOUT;
        poll(&pollfd, 1, 0);
      }
      else
      {
        return;
      }
    }
    ioctl(nmd->fd, NIOCTXSYNC, NULL);
  }
}

static void periodic(uint64_t count, struct timeval *tv1ptr)
{
  struct timeval tv2;
  if ((count & (16*1024*1024-1)) == 0)
  {
    double diff;
    gettimeofday(&tv2, NULL);
    diff = tv2.tv_sec - tv1ptr->tv_sec + (tv2.tv_usec - tv1ptr->tv_usec)/1000.0/1000.0;
    printf("%g Mpps\n", 16*1024*1024/diff/1000.0/1000.0);
    *tv1ptr = tv2;
  }
}

struct rx_args {
  struct synproxy *synproxy;
  struct worker_local *local;
  int idx;
};

static void *rx_func(void *userdata)
{
  struct rx_args *args = userdata;
  struct ll_alloc_st st;
  int i;
  struct port outport;
  struct netmapfunc2_userdata ud;
  struct timeval tv1;
  uint64_t count = 0;

  gettimeofday(&tv1, NULL);

  ud.loc = &loc;
  ud.dlnmd = dlnmds[args->idx];
  ud.ulnmd = ulnmds[args->idx];
  outport.portfunc = netmapfunc2;
  outport.userdata = &ud;

  if (ll_alloc_st_init(&st, POOL_SIZE, BLOCK_SIZE) != 0)
  {
    abort();
  }

  for (;;)
  {
    uint64_t time64;
    uint64_t expiry;
    int try;
    uint32_t timeout;
    struct pollfd pfds[2];

    pfds[0].fd = dlnmds[args->idx]->fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = ulnmds[args->idx]->fd;
    pfds[1].events = POLLIN;

    worker_local_rdlock(args->local);
    expiry = timer_linkheap_next_expiry_time(&args->local->timers);
    time64 = gettime64();
    worker_local_rdunlock(args->local);

    timeout = (expiry > time64 ? (999 + expiry - time64)/1000 : 0);
    if (timeout > 0)
    {
      poll(pfds, 2, timeout);
    }

    time64 = gettime64();
    worker_local_rdlock(args->local);
    try = (timer_linkheap_next_expiry_time(&args->local->timers) < time64);
    worker_local_rdunlock(args->local);

    if (try)
    {
      worker_local_wrlock(args->local);
      while (timer_linkheap_next_expiry_time(&args->local->timers) < time64)
      {
        struct timer_link *timer = timer_linkheap_next_expiry_timer(&args->local->timers);
        timer_linkheap_remove(&args->local->timers, timer);
        worker_local_wrunlock(args->local);
        timer->fn(timer, &args->local->timers, timer->userdata);
        worker_local_wrlock(args->local);
      }
      worker_local_wrunlock(args->local);
    }
    for (i = 0; i < 1000; i++)
    {
      struct packet *pktstruct;
      struct nm_pkthdr hdr;
      unsigned char *pkt;
      pkt = nm_nextpkt(dlnmds[args->idx], &hdr);
      if (pkt == NULL)
      {
        break;
      }

      pktstruct = ll_alloc_st(&st, packet_size(hdr.len));
      pktstruct->direction = PACKET_DIRECTION_UPLINK;
      pktstruct->sz = hdr.len;
      memcpy(packet_data(pktstruct), pkt, hdr.len);

      if (uplink(args->synproxy, args->local, pktstruct, &outport, time64, &st))
      {
        ll_free_st(&st, pktstruct);
      }
      count++;
      periodic(count, &tv1);
      if (in)
      {
        if (pcapng_out_ctx_write(&inctx, pkt, hdr.len, gettime64(), "out"))
        {
          printf("can't record packet\n");
          exit(1);
        }
      }
      if (lan)
      {
        if (pcapng_out_ctx_write(&lanctx, pkt, hdr.len, gettime64(), "in"))
        {
          printf("can't record packet\n");
          exit(1);
        }
      }
    }
    for (i = 0; i < 1000; i++)
    {
      struct packet *pktstruct;
      struct nm_pkthdr hdr;
      unsigned char *pkt;
      pkt = nm_nextpkt(ulnmds[args->idx], &hdr);
      if (pkt == NULL)
      {
        break;
      }

      pktstruct = ll_alloc_st(&st, packet_size(hdr.len));
      pktstruct->direction = PACKET_DIRECTION_DOWNLINK;
      pktstruct->sz = hdr.len;
      memcpy(packet_data(pktstruct), pkt, hdr.len);

      if (downlink(args->synproxy, args->local, pktstruct, &outport, time64, &st))
      {
        ll_free_st(&st, pktstruct);
      }
      count++;
      periodic(count, &tv1);
      if (in)
      {
        if (pcapng_out_ctx_write(&inctx, pkt, hdr.len, gettime64(), "in"))
        {
          printf("can't record packet\n");
          exit(1);
        }
      }
      if (wan)
      {
        if (pcapng_out_ctx_write(&wanctx, pkt, hdr.len, gettime64(), "in"))
        {
          printf("can't record packet\n");
          exit(1);
        }
      }
    }
  }
}


int main(int argc, char **argv)
{
  pthread_t rx[MAX_RX];
  struct rx_args rx_args[MAX_RX];
  struct synproxy synproxy;
  struct worker_local local;
  struct nmreq nmr;
  cpu_set_t cpuset;
  struct conf conf = CONF_INITIALIZER;
  int opt;
  char *inname = NULL;
  char *outname = NULL;
  char *lanname = NULL;
  char *wanname = NULL;
  int i;
  char nmifnamebuf[64];
  uint64_t time64;

  confyydirparse(argv[0], "conf.txt", &conf, 0);
  synproxy_init(&synproxy, &conf);

  hash_seed_init();
  setlinebuf(stdout);

  while ((opt = getopt(argc, argv, "i:o:l:w:")) != -1)
  {
    switch (opt)
    {
      case 'i':
        inname = optarg;
        break;
      case 'o':
        outname = optarg;
        break;
      case 'l':
        lanname = optarg;
        break;
      case 'w':
        wanname = optarg;
        break;
      default:
        printf("usage: %s [-i in.pcapng] [-o out.pcapng] [-l lan.pcapng] [-w wan.pcapng] vale0:1 vale1:1\n", argv[0]);
        exit(1);
        break;
    }
  }

  if (argc != optind + 2)
  {
    printf("usage: %s [-i in.pcapng] [-o out.pcapng] [-l lan.pcapng] [-w wan.pcapng] vale0:1 vale1:1\n", argv[0]);
    exit(1);
  }
  if (inname != NULL)
  {
    if (pcapng_out_ctx_init(&inctx, inname) != 0)
    {
      printf("can't open file for storing input\n");
      exit(1);
    }
    in = 1;
  }
  if (outname != NULL)
  {
    if (pcapng_out_ctx_init(&outctx, outname) != 0)
    {
      printf("can't open file for storing output\n");
      exit(1);
    }
    out = 1;
  }
  if (lanname != NULL)
  {
    if (pcapng_out_ctx_init(&lanctx, lanname) != 0)
    {
      printf("can't open file for storing LAN traffic\n");
      exit(1);
    }
    lan = 1;
  }
  if (wanname != NULL)
  {
    if (pcapng_out_ctx_init(&wanctx, wanname) != 0)
    {
      printf("can't open file for storing WAN traffic\n");
      exit(1);
    }
    wan = 1;
  }

  int max;
  max = num_rx;

  for (i = 0; i < max; i++)
  {
    memset(&nmr, 0, sizeof(nmr));
    nmr.nr_tx_rings = max;
    nmr.nr_rx_rings = max;
    nmr.nr_flags = NR_REG_ONE_NIC;
    nmr.nr_ringid = i | NETMAP_NO_TX_POLL;
#if 0
    nmr.nr_rx_slots = 256;
    nmr.nr_tx_slots = 64;
#endif
    snprintf(nmifnamebuf, sizeof(nmifnamebuf), "%s-%d", argv[optind+0], i);
    dlnmds[i] = nm_open(nmifnamebuf, &nmr, 0, NULL);
    if (dlnmds[i] == NULL)
    {
      printf("cannot open %s\n", argv[optind+0]);
      exit(1);
    }
    printf("Downlink interface:\n");
    printf("RX rings: %u %u\n", dlnmds[i]->last_rx_ring, dlnmds[i]->first_rx_ring + 1);
    printf("TX rings: %u %u\n", dlnmds[i]->last_tx_ring, dlnmds[i]->first_tx_ring + 1);
    printf("RX rings: %u\n", dlnmds[i]->last_rx_ring - dlnmds[i]->first_rx_ring + 1);
    printf("TX rings: %u\n", dlnmds[i]->last_tx_ring - dlnmds[i]->first_tx_ring + 1);
  }
  for (i = 0; i < max; i++)
  {
    memset(&nmr, 0, sizeof(nmr));
    nmr.nr_tx_rings = max;
    nmr.nr_rx_rings = max;
    nmr.nr_flags = NR_REG_ONE_NIC;
    nmr.nr_ringid = i | NETMAP_NO_TX_POLL;
#if 0
    nmr.nr_rx_slots = 256;
    nmr.nr_tx_slots = 64;
#endif
    snprintf(nmifnamebuf, sizeof(nmifnamebuf), "%s-%d", argv[optind+1], i);
    ulnmds[i] = nm_open(nmifnamebuf, &nmr, 0, NULL);
    if (ulnmds[i] == NULL)
    {
      printf("cannot open %s\n", argv[optind+1]);
      exit(1);
    }
    printf("Uplink interface:\n");
    printf("RX rings: %u %u\n", ulnmds[i]->last_rx_ring, ulnmds[i]->first_rx_ring + 1);
    printf("TX rings: %u %u\n", ulnmds[i]->last_tx_ring, ulnmds[i]->first_tx_ring + 1);
    printf("RX rings: %u\n", ulnmds[i]->last_rx_ring - ulnmds[i]->first_rx_ring + 1);
    printf("TX rings: %u\n", ulnmds[i]->last_tx_ring - ulnmds[i]->first_tx_ring + 1);
  }

  {
    int j;
    worker_local_init(&local, &synproxy, 0, 1);
    for (j = 0; j < 90*6; j++)
    {
      synproxy_hash_put_connected(
        &local, (10<<24)|(2*j+2), 12345, (11<<24)|(2*j+1), 54321,
        gettime64());
    }
  }
  time64 = gettime64();
  for (i = 0; i < uniformcnt; i++)
  {
    uniform_userdata[i].local = &local;
    uniform_userdata[i].table_start =
      local.hash.bucketcnt*i/uniformcnt;
    uniform_userdata[i].table_end =
      local.hash.bucketcnt*(i+1)/uniformcnt;
    uniformtimer[i].userdata = &uniform_userdata[i];
    uniformtimer[i].fn = uniform_fn;
    uniformtimer[i].time64 = time64 + (i+1)*1000*1000/uniformcnt;
    timer_linkheap_add(&local.timers, &uniformtimer[i]);
  }


  for (i = 0; i < num_rx; i++)
  {
    rx_args[i].idx = i;
    rx_args[i].synproxy = &synproxy;
    rx_args[i].local = &local;
  }

  char pktdl[14] = {0x02,0,0,0,0,0x04, 0x02,0,0,0,0,0x01, 0, 0};
  char pktul[14] = {0x02,0,0,0,0,0x01, 0x02,0,0,0,0,0x04, 0, 0};

  nm_my_inject(dlnmds[0], pktdl, sizeof(pktdl));
  ioctl(dlnmds[0]->fd, NIOCTXSYNC, NULL);
  nm_my_inject(ulnmds[0], pktul, sizeof(pktul));
  ioctl(ulnmds[0]->fd, NIOCTXSYNC, NULL);


  for (i = 0; i < num_rx; i++)
  {
    pthread_create(&rx[i], NULL, rx_func, &rx_args[i]);
  }
  int cpu = 0;
  if (num_rx <= sysconf(_SC_NPROCESSORS_ONLN))
  {
    for (i = 0; i < num_rx; i++)
    {
      CPU_ZERO(&cpuset);
      CPU_SET(cpu, &cpuset);
      cpu++;
      pthread_setaffinity_np(rx[i], sizeof(cpuset), &cpuset);
    }
  }
  for (i = 0; i < num_rx; i++)
  {
    pthread_join(rx[i], NULL);
  }

  synproxy_free(&synproxy);

  return 0;
}
