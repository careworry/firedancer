#define _GNU_SOURCE 1
#include "fd_repair.h"
#include "../../ballet/sha256/fd_sha256.h"
#include "../../ballet/ed25519/fd_ed25519.h"
#include "../../ballet/base58/fd_base58.h"
#include "../../disco/keyguard/fd_keyguard.h"
#include "../../util/rng/fd_rng.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>

/* Max number of validators that can be actively queried */
#define FD_ACTIVE_KEY_MAX (1<<12)
/* Max number of pending shred requests */
#define FD_NEEDED_KEY_MAX (1<<20)
/* Max number of sticky repair peers */
#define FD_REPAIR_STICKY_MAX   1024
/* Max number of validator identities in stake weights */
#define FD_STAKE_WEIGHTS_MAX (1<<14)
/* Max number of validator clients that we ping */
#define FD_REPAIR_PINGED_MAX (1<<14)
/* Sha256 pre-image size for pings */
#define FD_PING_PRE_IMAGE_SZ (48UL)
/* Number of peers to send requests to. */
#define FD_REPAIR_NUM_NEEDED_PEERS (4)

/* Test if two hash values are equal */
FD_FN_PURE static int fd_hash_eq( const fd_hash_t * key1, const fd_hash_t * key2 ) {
  for (ulong i = 0; i < 32U/sizeof(ulong); ++i)
    if (key1->ul[i] != key2->ul[i])
      return 0;
  return 1;
}

/* Hash a hash value */
FD_FN_PURE static ulong fd_hash_hash( const fd_hash_t * key, ulong seed ) {
  return key->ul[0] ^ seed;
}

/* Copy a hash value */
static void fd_hash_copy( fd_hash_t * keyd, const fd_hash_t * keys ) {
  for (ulong i = 0; i < 32U/sizeof(ulong); ++i)
    keyd->ul[i] = keys->ul[i];
}

/* Test if two addresses are equal */
FD_FN_PURE int fd_repair_peer_addr_eq( const fd_repair_peer_addr_t * key1, const fd_repair_peer_addr_t * key2 ) {
  FD_STATIC_ASSERT(sizeof(fd_repair_peer_addr_t) == sizeof(ulong),"messed up size");
  return key1->l == key2->l;
}

/* Hash an address */
FD_FN_PURE ulong fd_repair_peer_addr_hash( const fd_repair_peer_addr_t * key, ulong seed ) {
  FD_STATIC_ASSERT(sizeof(fd_repair_peer_addr_t) == sizeof(ulong),"messed up size");
  return (key->l + seed + 7242237688154252699UL)*9540121337UL;
}

/* Efficiently copy an address */
void fd_repair_peer_addr_copy( fd_repair_peer_addr_t * keyd, const fd_repair_peer_addr_t * keys ) {
  FD_STATIC_ASSERT(sizeof(fd_repair_peer_addr_t) == sizeof(ulong),"messed up size");
  keyd->l = keys->l;
}

typedef uint fd_repair_nonce_t;

/* Active table element. This table is all validators that we are
   asking for repairs. */
struct fd_active_elem {
    fd_pubkey_t key;  /* Public identifier and map key */
    ulong next; /* used internally by fd_map_giant */

    fd_repair_peer_addr_t addr;
    ulong avg_reqs; /* Moving average of the number of requests */
    ulong avg_reps; /* Moving average of the number of requests */
    long  avg_lat;  /* Moving average of response latency */
    uchar sticky;
    long  first_request_time;
    ulong stake;
};
/* Active table */
typedef struct fd_active_elem fd_active_elem_t;
#define MAP_NAME     fd_active_table
#define MAP_KEY_T    fd_pubkey_t
#define MAP_KEY_EQ   fd_hash_eq
#define MAP_KEY_HASH fd_hash_hash
#define MAP_KEY_COPY fd_hash_copy
#define MAP_T        fd_active_elem_t
#include "../../util/tmpl/fd_map_giant.c"

enum fd_needed_elem_type {
  fd_needed_window_index, fd_needed_highest_window_index, fd_needed_orphan
};

struct fd_dupdetect_key {
  enum fd_needed_elem_type type;
  ulong slot;
  uint shred_index;
};
typedef struct fd_dupdetect_key fd_dupdetect_key_t;

struct fd_dupdetect_elem {
  fd_dupdetect_key_t key;
  long               last_send_time;
  uint               req_cnt;
  ulong              next;
};
typedef struct fd_dupdetect_elem fd_dupdetect_elem_t;

FD_FN_PURE
int fd_dupdetect_eq( const fd_dupdetect_key_t * key1, const fd_dupdetect_key_t * key2 ) {
  return (key1->type == key2->type) &&
         (key1->slot == key2->slot) &&
         (key1->shred_index == key2->shred_index);
}

FD_FN_PURE
ulong fd_dupdetect_hash( const fd_dupdetect_key_t * key, ulong seed ) {
  return (key->slot + seed)*9540121337UL + key->shred_index*131U;
}

void fd_dupdetect_copy( fd_dupdetect_key_t * keyd, const fd_dupdetect_key_t * keys ) {
  *keyd = *keys;
}

#define MAP_NAME     fd_dupdetect_table
#define MAP_KEY_T    fd_dupdetect_key_t
#define MAP_KEY_EQ   fd_dupdetect_eq
#define MAP_KEY_HASH fd_dupdetect_hash
#define MAP_KEY_COPY fd_dupdetect_copy
#define MAP_T        fd_dupdetect_elem_t
#include "../../util/tmpl/fd_map_giant.c"

FD_FN_PURE int fd_repair_nonce_eq( const fd_repair_nonce_t * key1, const fd_repair_nonce_t * key2 ) {
  return *key1 == *key2;
}

FD_FN_PURE ulong fd_repair_nonce_hash( const fd_repair_nonce_t * key, ulong seed ) {
  return (*key + seed + 7242237688154252699UL)*9540121337UL;
}

void fd_repair_nonce_copy( fd_repair_nonce_t * keyd, const fd_repair_nonce_t * keys ) {
  *keyd = *keys;
}

struct fd_needed_elem {
  fd_repair_nonce_t key;
  ulong next;
  fd_pubkey_t id;
  fd_dupdetect_key_t dupkey;
  long when;
};
typedef struct fd_needed_elem fd_needed_elem_t;
#define MAP_NAME     fd_needed_table
#define MAP_KEY_T    fd_repair_nonce_t
#define MAP_KEY_EQ   fd_repair_nonce_eq
#define MAP_KEY_HASH fd_repair_nonce_hash
#define MAP_KEY_COPY fd_repair_nonce_copy
#define MAP_T        fd_needed_elem_t
#include "../../util/tmpl/fd_map_giant.c"

struct fd_pinged_elem {
  fd_repair_peer_addr_t key;
  ulong next;
  fd_pubkey_t id;
  fd_hash_t token;
  int good;
};
typedef struct fd_pinged_elem fd_pinged_elem_t;
#define MAP_NAME     fd_pinged_table
#define MAP_KEY_T    fd_repair_peer_addr_t
#define MAP_KEY_EQ   fd_repair_peer_addr_eq
#define MAP_KEY_HASH fd_repair_peer_addr_hash
#define MAP_KEY_COPY fd_repair_peer_addr_copy
#define MAP_T        fd_pinged_elem_t
#include "../../util/tmpl/fd_map_giant.c"

/* Global data for repair service */
struct fd_repair {
    /* Concurrency lock */
    volatile ulong lock;
    /* Current time in nanosecs */
    long now;
    /* My public/private key */
    fd_pubkey_t * public_key;
    uchar * private_key;
    /* My repair addresses */
    fd_repair_peer_addr_t service_addr;
    fd_repair_peer_addr_t intake_addr;
    /* Function used to deliver repair messages to the application */
    fd_repair_shred_deliver_fun deliver_fun;
    /* Functions used to handle repair requests */
    fd_repair_serv_get_shred_fun serv_get_shred_fun;
    fd_repair_serv_get_parent_fun serv_get_parent_fun;
    /* Function used to send raw packets on the network */
    fd_repair_send_packet_fun clnt_send_fun; /* Client requests */
    fd_repair_send_packet_fun serv_send_fun; /* Service responses */
    /* Function used to send packets for signing to remote tile */
    fd_repair_sign_fun sign_fun;
    /* Argument to fd_repair_sign_fun */
    void * sign_arg;
    /* Function used to deliver repair failure on the network */
    fd_repair_shred_deliver_fail_fun deliver_fail_fun;
    void * fun_arg;
    /* Table of validators that we are actively pinging, keyed by repair address */
    fd_active_elem_t * actives;
    fd_pubkey_t actives_sticky[FD_REPAIR_STICKY_MAX]; /* cache of chosen repair peer samples */
    ulong       actives_sticky_cnt;
    ulong       actives_random_seed;
    /* Duplicate request detection table */
    fd_dupdetect_elem_t * dupdetect;
    /* Table of needed shreds */
    fd_needed_elem_t * needed;
    fd_repair_nonce_t oldest_nonce;
    fd_repair_nonce_t current_nonce;
    fd_repair_nonce_t next_nonce;
    /* Table of validator clients that we have pinged */
    fd_pinged_elem_t * pinged;
    /* Last batch of sends */
    long last_sends;
    /* Last statistics decay */
    long last_decay;
    /* Last statistics printout */
    long last_print;
    /* Last write to good peer cache file */
    long last_good_peer_cache_file_write;
    /* Random number generator */
    fd_rng_t rng[1];
    /* RNG seed */
    ulong seed;
    /* Stake weights */
    ulong stake_weights_cnt;
    fd_stake_weight_t * stake_weights;
    /* Path to the file where we write the cache of known good repair peers, to make cold booting faster */
    int good_peer_cache_file_fd;
    /* Metrics */
    fd_repair_metrics_t metrics;
};

FD_FN_CONST ulong
fd_repair_align ( void ) { return 128UL; }

FD_FN_CONST ulong
fd_repair_footprint( void ) {
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof(fd_repair_t), sizeof(fd_repair_t) );
  l = FD_LAYOUT_APPEND( l, fd_active_table_align(), fd_active_table_footprint(FD_ACTIVE_KEY_MAX) );
  l = FD_LAYOUT_APPEND( l, fd_needed_table_align(), fd_needed_table_footprint(FD_NEEDED_KEY_MAX) );
  l = FD_LAYOUT_APPEND( l, fd_dupdetect_table_align(), fd_dupdetect_table_footprint(FD_NEEDED_KEY_MAX) );
  l = FD_LAYOUT_APPEND( l, fd_pinged_table_align(), fd_pinged_table_footprint(FD_REPAIR_PINGED_MAX) );
  l = FD_LAYOUT_APPEND( l, fd_stake_weight_align(), FD_STAKE_WEIGHTS_MAX * fd_stake_weight_footprint() );
  return FD_LAYOUT_FINI(l, fd_repair_align() );
}

void *
fd_repair_new ( void * shmem, ulong seed ) {
  FD_SCRATCH_ALLOC_INIT(l, shmem);
  fd_repair_t * glob = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_repair_t), sizeof(fd_repair_t) );
  fd_memset(glob, 0, sizeof(fd_repair_t));
  void * shm = FD_SCRATCH_ALLOC_APPEND( l, fd_active_table_align(), fd_active_table_footprint(FD_ACTIVE_KEY_MAX) );
  glob->actives = fd_active_table_join(fd_active_table_new(shm, FD_ACTIVE_KEY_MAX, seed));
  glob->seed = seed;
  shm = FD_SCRATCH_ALLOC_APPEND( l, fd_needed_table_align(), fd_needed_table_footprint(FD_NEEDED_KEY_MAX) );
  glob->needed = fd_needed_table_join(fd_needed_table_new(shm, FD_NEEDED_KEY_MAX, seed));
  shm = FD_SCRATCH_ALLOC_APPEND( l, fd_dupdetect_table_align(), fd_dupdetect_table_footprint(FD_NEEDED_KEY_MAX) );
  glob->dupdetect = fd_dupdetect_table_join(fd_dupdetect_table_new(shm, FD_NEEDED_KEY_MAX, seed));
  shm = FD_SCRATCH_ALLOC_APPEND( l, fd_pinged_table_align(), fd_pinged_table_footprint(FD_REPAIR_PINGED_MAX) );
  glob->pinged = fd_pinged_table_join(fd_pinged_table_new(shm, FD_REPAIR_PINGED_MAX, seed));
  glob->stake_weights = FD_SCRATCH_ALLOC_APPEND( l, fd_stake_weight_align(), FD_STAKE_WEIGHTS_MAX * fd_stake_weight_footprint() );
  glob->stake_weights_cnt = 0;
  glob->last_sends = 0;
  glob->last_decay = 0;
  glob->last_print = 0;
  glob->last_good_peer_cache_file_write = 0;
  glob->oldest_nonce = glob->current_nonce = glob->next_nonce = 0;
  fd_rng_new(glob->rng, (uint)seed, 0UL);

  glob->actives_sticky_cnt   = 0;
  glob->actives_random_seed  = 0;

  ulong scratch_top = FD_SCRATCH_ALLOC_FINI(l, 1UL);
  if ( scratch_top > (ulong)shmem + fd_repair_footprint() ) {
    FD_LOG_ERR(("Enough space not allocated for repair"));
  }

  return glob;
}

fd_repair_t *
fd_repair_join ( void * shmap ) { return (fd_repair_t *)shmap; }

void *
fd_repair_leave ( fd_repair_t * join ) { return join; }

void *
fd_repair_delete ( void * shmap ) {
  fd_repair_t * glob = (fd_repair_t *)shmap;
  fd_active_table_delete( fd_active_table_leave( glob->actives ) );
  fd_needed_table_delete( fd_needed_table_leave( glob->needed ) );
  fd_dupdetect_table_delete( fd_dupdetect_table_leave( glob->dupdetect ) );
  fd_pinged_table_delete( fd_pinged_table_leave( glob->pinged ) );
  return glob;
}

static void
fd_repair_lock( fd_repair_t * repair ) {
# if FD_HAS_THREADS
  for(;;) {
    if( FD_LIKELY( !FD_ATOMIC_CAS( &repair->lock, 0UL, 1UL) ) ) break;
    FD_SPIN_PAUSE();
  }
# else
  repair->lock = 1;
# endif
  FD_COMPILER_MFENCE();
}

static void
fd_repair_unlock( fd_repair_t * repair ) {
  FD_COMPILER_MFENCE();
  FD_VOLATILE( repair->lock ) = 0UL;
}

/* Convert an address to a human readable string */
const char * fd_repair_addr_str( char * dst, size_t dstlen, fd_repair_peer_addr_t const * src ) {
  char tmp[INET_ADDRSTRLEN];
  snprintf(dst, dstlen, "%s:%u", inet_ntop(AF_INET, &src->addr, tmp, INET_ADDRSTRLEN), (uint)ntohs(src->port));
  return dst;
}

/* Set the repair configuration */
int
fd_repair_set_config( fd_repair_t * glob, const fd_repair_config_t * config ) {
  char tmp[100];
  char keystr[ FD_BASE58_ENCODED_32_SZ ];
  fd_base58_encode_32( config->public_key->uc, NULL, keystr );
  FD_LOG_NOTICE(("configuring address %s key %s", fd_repair_addr_str(tmp, sizeof(tmp), &config->intake_addr), keystr));

  glob->public_key = config->public_key;
  glob->private_key = config->private_key;
  fd_repair_peer_addr_copy(&glob->intake_addr, &config->intake_addr);
  fd_repair_peer_addr_copy(&glob->service_addr, &config->service_addr);
  glob->deliver_fun = config->deliver_fun;
  glob->serv_get_shred_fun = config->serv_get_shred_fun;
  glob->serv_get_parent_fun = config->serv_get_parent_fun;
  glob->clnt_send_fun = config->clnt_send_fun;
  glob->serv_send_fun = config->serv_send_fun;
  glob->fun_arg = config->fun_arg;
  glob->sign_fun = config->sign_fun;
  glob->sign_arg = config->sign_arg;
  glob->deliver_fail_fun = config->deliver_fail_fun;
  glob->good_peer_cache_file_fd = config->good_peer_cache_file_fd;
  return 0;
}

int
fd_repair_update_addr( fd_repair_t * glob, const fd_repair_peer_addr_t * intake_addr, const fd_repair_peer_addr_t * service_addr ) {
  char tmp[100];
  FD_LOG_NOTICE(("updating address %s", fd_repair_addr_str(tmp, sizeof(tmp), intake_addr)));

  fd_repair_peer_addr_copy(&glob->intake_addr, intake_addr);
  fd_repair_peer_addr_copy(&glob->service_addr, service_addr);
  return 0;
}

/* Initiate connection to a peer */
int
fd_repair_add_active_peer( fd_repair_t * glob, fd_repair_peer_addr_t const * addr, fd_pubkey_t const * id ) {
  fd_repair_lock( glob );
  char tmp[100];
  char keystr[ FD_BASE58_ENCODED_32_SZ ];
  fd_base58_encode_32( id->uc, NULL, keystr );
  FD_LOG_DEBUG(("adding active peer address %s key %s", fd_repair_addr_str(tmp, sizeof(tmp), addr), keystr));

  fd_active_elem_t * val = fd_active_table_query(glob->actives, id, NULL);
  if (val == NULL) {
    if (fd_active_table_is_full(glob->actives)) {
      FD_LOG_WARNING(("too many active repair peers, discarding new peer"));
      fd_repair_unlock( glob );
      return -1;
    }
    val = fd_active_table_insert(glob->actives, id);
    fd_repair_peer_addr_copy(&val->addr, addr);
    val->avg_reqs = 0;
    val->avg_reps = 0;
    val->avg_lat = 0;
    val->sticky = 0;
    val->first_request_time = 0;
    val->stake = 0UL;
    FD_LOG_DEBUG(( "adding repair peer %s", FD_BASE58_ENC_32_ALLOCA( val->key.uc ) ));
  }
  fd_repair_unlock( glob );
  return 0;
}

/* Set the current protocol time in nanosecs */
void
fd_repair_settime( fd_repair_t * glob, long ts ) {
  glob->now = ts;
}

/* Get the current protocol time in nanosecs */
long
fd_repair_gettime( fd_repair_t * glob ) {
  return glob->now;
}

static void
fd_repair_sign_and_send( fd_repair_t *           glob,
                         fd_repair_protocol_t *  protocol,
                         fd_gossip_peer_addr_t * addr ) {

  uchar _buf[1024];
  uchar * buf    = _buf;
  ulong   buflen = sizeof(_buf);
  fd_bincode_encode_ctx_t ctx = { .data = buf, .dataend = buf + buflen };
  if( FD_UNLIKELY( fd_repair_protocol_encode( protocol, &ctx ) != FD_BINCODE_SUCCESS ) ) {
    FD_LOG_CRIT(( "Failed to encode repair message (type %#x)", protocol->discriminant ));
  }

  buflen = (ulong)ctx.data - (ulong)buf;
  if( FD_UNLIKELY( buflen<68 ) ) {
    FD_LOG_CRIT(( "Attempted to sign unsigned repair message type (type %#x)", protocol->discriminant ));
  }

  /* At this point buffer contains

     [ discriminant ] [ signature ] [ payload ]
     ^                ^             ^
     0                4             68 */

  /* https://github.com/solana-labs/solana/blob/master/core/src/repair/serve_repair.rs#L874 */

  fd_memcpy( buf+64, buf, 4 );
  buf    += 64UL;
  buflen -= 64UL;

  /* Now it contains

     [ discriminant ] [ payload ]
     ^                ^
     0                4 */

  fd_signature_t sig;
  (*glob->sign_fun)( glob->sign_arg, sig.uc, buf, buflen, FD_KEYGUARD_SIGN_TYPE_ED25519 );

  /* Reintroduce the signature */

  buf    -= 64UL;
  buflen += 64UL;
  fd_memcpy( buf + 4U, &sig, 64U );

  uint src_ip4_addr = 0U; /* unknown */
  glob->clnt_send_fun( buf, buflen, addr, src_ip4_addr, glob->fun_arg );
}

static void
fd_repair_send_requests( fd_repair_t * glob ) {
  /* Garbage collect old requests */
  long expire = glob->now - (long)5e9; /* 5 seconds */
  fd_repair_nonce_t n;
  for ( n = glob->oldest_nonce; n != glob->next_nonce; ++n ) {
    fd_needed_elem_t * ele = fd_needed_table_query( glob->needed, &n, NULL );
    if ( NULL == ele )
      continue;
    if (ele->when > expire)
      break;
    // (*glob->deliver_fail_fun)( &ele->key, ele->slot, ele->shred_index, glob->fun_arg, FD_REPAIR_DELIVER_FAIL_TIMEOUT );
    fd_dupdetect_elem_t * dup = fd_dupdetect_table_query( glob->dupdetect, &ele->dupkey, NULL );
    if( dup && --dup->req_cnt == 0) {
      fd_dupdetect_table_remove( glob->dupdetect, &ele->dupkey );
    }
    fd_needed_table_remove( glob->needed, &n );
  }
  glob->oldest_nonce = n;

  /* Send requests starting where we left off last time */
  if ( (int)(n - glob->current_nonce) < 0 )
    n = glob->current_nonce;
  ulong j = 0;
  ulong k = 0;
  for ( ; n != glob->next_nonce; ++n ) {
    ++k;
    fd_needed_elem_t * ele = fd_needed_table_query( glob->needed, &n, NULL );
    if ( NULL == ele )
      continue;

    if(j == 128U) break;
    ++j;

    /* Track statistics */
    ele->when = glob->now;

    fd_active_elem_t * active = fd_active_table_query( glob->actives, &ele->id, NULL );
    if ( active == NULL) {
      fd_dupdetect_elem_t * dup = fd_dupdetect_table_query( glob->dupdetect, &ele->dupkey, NULL );
      if( dup && --dup->req_cnt == 0) {
        fd_dupdetect_table_remove( glob->dupdetect, &ele->dupkey );
      }
      fd_needed_table_remove( glob->needed, &n );
      continue;
    }

    active->avg_reqs++;
    glob->metrics.send_pkt_cnt++;

    fd_repair_protocol_t protocol;
    switch (ele->dupkey.type) {
      case fd_needed_window_index: {
        glob->metrics.sent_pkt_types[FD_METRICS_ENUM_REPAIR_SENT_REQUEST_TYPES_V_NEEDED_WINDOW_IDX]++;
        fd_repair_protocol_new_disc(&protocol, fd_repair_protocol_enum_window_index);
        fd_repair_window_index_t * wi = &protocol.inner.window_index;
        fd_hash_copy(&wi->header.sender, glob->public_key);
        fd_hash_copy(&wi->header.recipient, &active->key);
        wi->header.timestamp = glob->now/1000000L;
        wi->header.nonce = n;
        wi->slot = ele->dupkey.slot;
        wi->shred_index = ele->dupkey.shred_index;
        // FD_LOG_INFO(("[repair]"))
        break;
      }

      case fd_needed_highest_window_index: {
        glob->metrics.sent_pkt_types[FD_METRICS_ENUM_REPAIR_SENT_REQUEST_TYPES_V_NEEDED_HIGHEST_WINDOW_IDX]++;
        fd_repair_protocol_new_disc(&protocol, fd_repair_protocol_enum_highest_window_index);
        fd_repair_highest_window_index_t * wi = &protocol.inner.highest_window_index;
        fd_hash_copy(&wi->header.sender, glob->public_key);
        fd_hash_copy(&wi->header.recipient, &active->key);
        wi->header.timestamp = glob->now/1000000L;
        wi->header.nonce = n;
        wi->slot = ele->dupkey.slot;
        wi->shred_index = ele->dupkey.shred_index;
        break;
      }

      case fd_needed_orphan: {
        glob->metrics.sent_pkt_types[FD_METRICS_ENUM_REPAIR_SENT_REQUEST_TYPES_V_NEEDED_ORPHAN_IDX]++;
        fd_repair_protocol_new_disc(&protocol, fd_repair_protocol_enum_orphan);
        fd_repair_orphan_t * wi = &protocol.inner.orphan;
        fd_hash_copy(&wi->header.sender, glob->public_key);
        fd_hash_copy(&wi->header.recipient, &active->key);
        wi->header.timestamp = glob->now/1000000L;
        wi->header.nonce = n;
        wi->slot = ele->dupkey.slot;
        break;
      }
    }

    fd_repair_sign_and_send( glob, &protocol, &active->addr );

  }
  glob->current_nonce = n;
  if( k )
    FD_LOG_DEBUG(("checked %lu nonces, sent %lu packets, total %lu", k, j, fd_needed_table_key_cnt( glob->needed )));
}

static void
fd_repair_decay_stats( fd_repair_t * glob ) {
  for( fd_active_table_iter_t iter = fd_active_table_iter_init( glob->actives );
       !fd_active_table_iter_done( glob->actives, iter );
       iter = fd_active_table_iter_next( glob->actives, iter ) ) {
    fd_active_elem_t * ele = fd_active_table_iter_ele( glob->actives, iter );
#define DECAY(_v_) _v_ = _v_ - ((_v_)>>3U) /* Reduce by 12.5% */
    DECAY(ele->avg_reqs);
    DECAY(ele->avg_reps);
    DECAY(ele->avg_lat);
#undef DECAY
  }
}

/**
 * read_line() reads characters one by one from 'fd' until:
 *   - it sees a newline ('\n')
 *   - it reaches 'max_len - 1' characters
 *   - or EOF (read returns 0)
 * It stores the line in 'buf' and null-terminates it.
 *
 * Returns the number of characters read (not counting the null terminator),
 * or -1 on error.
 */
long read_line(int fd, char *buf) {
    long i = 0;

    while (i < 255) {
        char c;
        long n = read(fd, &c, 1);

        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        } else if (n == 0) {
            break;
        }

        buf[i++] = c;

        if (c == '\n') {
            break;
        }
    }

    buf[i] = '\0';
    return i;
}

static int
fd_read_in_good_peer_cache_file( fd_repair_t * repair ) {
  if( repair->good_peer_cache_file_fd==-1 ) {
    FD_LOG_NOTICE(( "No repair good_peer_cache_file specified, not loading cached peers" ));
    return 0;
  }

  long seek = lseek( repair->good_peer_cache_file_fd, 0UL, SEEK_SET );
  if( FD_UNLIKELY( seek!=0L ) ) {
    FD_LOG_WARNING(( "Failed to seek to the beginning of the good peer cache file" ));
    return 1;
  }

  int   loaded_peers   = 0;
  char  line[256];
  char  *saveptr      = NULL;

  long len;
  while ((len = read_line(repair->good_peer_cache_file_fd, line)) > 0) {

    /* Strip newline if present */
    size_t len = strlen( line );
    if( len>0 && line[len-1]=='\n' ) {
      line[len-1] = '\0';
      len--;
    }

    /* Skip empty or comment lines */
    if( !len || line[0]=='#' ) continue;

    /* Parse: base58EncodedPubkey/ipAddr/port */
    char * base58_str = strtok_r( line, "/", &saveptr );
    char * ip_str     = strtok_r( NULL, "/", &saveptr );
    char * port_str   = strtok_r( NULL, "/", &saveptr );

    if( FD_UNLIKELY( !base58_str || !ip_str || !port_str ) ) {
      FD_LOG_WARNING(( "Malformed line, skipping" ));
      continue;
    }

    /* Decode the base58 public key */
    fd_pubkey_t pubkey;
    if( !fd_base58_decode_32( base58_str, pubkey.uc ) ) {
      FD_LOG_WARNING(( "Failed to decode base58 public key '%s', skipping", base58_str ));
      continue;
    }

    /* Convert IP address */
    struct in_addr addr_parsed;
    if( inet_aton( ip_str, &addr_parsed )==0 ) {
      FD_LOG_WARNING(( "Invalid IPv4 address '%s', skipping", ip_str ));
      continue;
    }
    uint ip_addr = (uint)addr_parsed.s_addr;

    /* Convert the port */
    char * endptr = NULL;
    long   port   = strtol( port_str, &endptr, 10 );
    if( (port<=0L) || (port>65535L) || (endptr && *endptr!='\0') ) {
      FD_LOG_WARNING(( "Invalid port '%s', skipping", port_str ));
      continue;
    }

    /* Create the peer address struct (byte-swap the port to network order). */
    fd_repair_peer_addr_t peer_addr;
    /* already in network byte order from inet_aton */
    peer_addr.addr = ip_addr;
    /* Flip to big-endian for network order */
    peer_addr.port = fd_ushort_bswap( (ushort)port );

    /* Add to active peers in the repair tile. */
    fd_repair_add_active_peer( repair, &peer_addr, &pubkey );

    loaded_peers++;
  }

  FD_LOG_INFO(( "Loaded %d peers from good peer cache file", loaded_peers ));
  return 0;
}

/* Start timed events and other protocol behavior */
int
fd_repair_start( fd_repair_t * glob ) {
  glob->last_sends = glob->now;
  glob->last_decay = glob->now;
  glob->last_print = glob->now;
  return fd_read_in_good_peer_cache_file( glob );
}

static void fd_repair_print_all_stats( fd_repair_t * glob );
static void fd_actives_shuffle( fd_repair_t * repair );
static int fd_write_good_peer_cache_file( fd_repair_t * repair );

/* Dispatch timed events and other protocol behavior. This should be
 * called inside the main spin loop. */
int
fd_repair_continue( fd_repair_t * glob ) {
  fd_repair_lock( glob );
  if ( glob->now - glob->last_sends > (long)1e6 ) { /* 1 millisecond */
    fd_repair_send_requests( glob );
    glob->last_sends = glob->now;
  }
  if ( glob->now - glob->last_print > (long)30e9 ) { /* 30 seconds */
    fd_repair_print_all_stats( glob );
    glob->last_print = glob->now;
    fd_actives_shuffle( glob );
    fd_repair_decay_stats( glob );
    glob->last_decay = glob->now;
  } else if ( glob->now - glob->last_decay > (long)15e9 ) { /* 15 seconds */
    fd_actives_shuffle( glob );
    fd_repair_decay_stats( glob );
    glob->last_decay = glob->now;
  } else if ( glob->now - glob->last_good_peer_cache_file_write > (long)60e9 ) { /* 1 minute */
    fd_write_good_peer_cache_file( glob );
    glob->last_good_peer_cache_file_write = glob->now;
  }
  fd_repair_unlock( glob );
  return 0;
}

static void
fd_repair_handle_ping( fd_repair_t *                 glob,
                       fd_gossip_ping_t const *      ping,
                       fd_gossip_peer_addr_t const * peer_addr,
                       uint                          self_ip4_addr ) {
  fd_repair_protocol_t protocol;
  fd_repair_protocol_new_disc(&protocol, fd_repair_protocol_enum_pong);
  fd_gossip_ping_t * pong = &protocol.inner.pong;

  fd_hash_copy( &pong->from, glob->public_key );

  /* Generate response hash token */
  uchar pre_image[FD_PING_PRE_IMAGE_SZ];
  memcpy( pre_image, "SOLANA_PING_PONG", 16UL );
  memcpy( pre_image+16UL, ping->token.uc, 32UL);

  /* Generate response hash token */
  fd_sha256_hash( pre_image, FD_PING_PRE_IMAGE_SZ, &pong->token );

  /* Sign it */
  (*glob->sign_fun)( glob->sign_arg, pong->signature.uc, pre_image, FD_PING_PRE_IMAGE_SZ, FD_KEYGUARD_SIGN_TYPE_SHA256_ED25519 );

  fd_bincode_encode_ctx_t ctx;
  uchar buf[1024];
  ctx.data = buf;
  ctx.dataend = buf + sizeof(buf);
  FD_TEST(0 == fd_repair_protocol_encode(&protocol, &ctx));
  ulong buflen = (ulong)((uchar*)ctx.data - buf);

  glob->clnt_send_fun( buf, buflen, peer_addr, self_ip4_addr, glob->fun_arg );
}

int
fd_repair_recv_clnt_packet( fd_repair_t *                 glob,
                            uchar const *                 msg,
                            ulong                         msglen,
                            fd_repair_peer_addr_t const * src_addr,
                            uint                          dst_ip4_addr ) {
  glob->metrics.recv_clnt_pkt++;

  fd_repair_lock( glob );
  FD_SCRATCH_SCOPE_BEGIN {
    while( 1 ) {
      fd_bincode_decode_ctx_t ctx = {
        .data    = msg,
        .dataend = msg + msglen
      };

      ulong total_sz = 0UL;
      if( FD_UNLIKELY( fd_repair_response_decode_footprint( &ctx, &total_sz ) ) ) {
        /* Solana falls back to assuming we got a shred in this case
           https://github.com/solana-labs/solana/blob/master/core/src/repair/serve_repair.rs#L1198 */
        break;
      }

      uchar * mem = fd_scratch_alloc( fd_repair_response_align(), total_sz );
      if( FD_UNLIKELY( !mem ) ) {
        FD_LOG_ERR(( "Unable to allocate memory for repair response" ));
      }

      fd_repair_response_t * gmsg = fd_repair_response_decode( mem, &ctx );
      if( FD_UNLIKELY( ctx.data != ctx.dataend ) ) {
        break;
      }

      switch( gmsg->discriminant ) {
      case fd_repair_response_enum_ping:
        fd_repair_handle_ping( glob, &gmsg->inner.ping, src_addr, dst_ip4_addr );
        break;
      }

      fd_repair_unlock( glob );
      return 0;
    }

    /* Look at the nonse */
    if( msglen < sizeof(fd_repair_nonce_t) ) {
      fd_repair_unlock( glob );
      return 0;
    }
    ulong shredlen = msglen - sizeof(fd_repair_nonce_t); /* Nonce is at the end */
    fd_repair_nonce_t key = *(fd_repair_nonce_t const *)(msg + shredlen);
    fd_needed_elem_t * val = fd_needed_table_query( glob->needed, &key, NULL );
    if( NULL == val ) {
      fd_repair_unlock( glob );
      return 0;
    }

    fd_active_elem_t * active = fd_active_table_query( glob->actives, &val->id, NULL );
    if( NULL != active ) {
      /* Update statistics */
      active->avg_reps++;
      active->avg_lat += glob->now - val->when;
    }

    fd_shred_t const * shred = fd_shred_parse(msg, shredlen);
    fd_repair_unlock( glob );
    if( shred == NULL ) {
      FD_LOG_WARNING(("invalid shread"));
    } else {
      glob->deliver_fun(shred, shredlen, src_addr, &val->id, glob->fun_arg);
    }
  } FD_SCRATCH_SCOPE_END;
  return 0;
}

int
fd_repair_is_full( fd_repair_t * glob ) {
  return fd_needed_table_is_full(glob->needed);
}

/* Test if a peer is good. Returns 1 if the peer is "great", 0 if the peer is "good", and -1 if the peer sucks */
static int
is_good_peer( fd_active_elem_t * val ) {
  if( FD_UNLIKELY( NULL == val ) ) return -1;                          /* Very bad */
  if( val->avg_reqs > 10U && val->avg_reps == 0U )  return -1;         /* Bad, no response after 10 requests */
  if( val->avg_reqs < 20U ) return 0;                                  /* Not sure yet, good enough for now */
  if( (float)val->avg_reps < 0.01f*((float)val->avg_reqs) ) return -1; /* Very bad */
  if( (float)val->avg_reps < 0.8f*((float)val->avg_reqs) ) return 0;   /* 80%, Good but not great */
  if( (float)val->avg_lat > 2500e9f*((float)val->avg_reps) ) return 0;  /* 300ms, Good but not great */
  return 1;                                                            /* Great! */
}

#define SORT_NAME        fd_latency_sort
#define SORT_KEY_T       long
#define SORT_BEFORE(a,b) (a)<(b)
#include "../../util/tmpl/fd_sort.c"

static void
fd_actives_shuffle( fd_repair_t * repair ) {
  /* Since we now have stake weights very quickly after reading the manifest, we wait
     until we have the stake weights before we start repairing. This ensures that we always
     sample from the available peers using stake weights. */
  if( repair->stake_weights_cnt == 0 ) {
    FD_LOG_NOTICE(( "repair does not have stake weights yet, not selecting any sticky peers" ));
    return;
  }

  FD_SCRATCH_SCOPE_BEGIN {
    ulong prev_sticky_cnt = repair->actives_sticky_cnt;
    /* Find all the usable stake holders */
    fd_active_elem_t ** leftovers = fd_scratch_alloc(
        alignof( fd_active_elem_t * ),
        sizeof( fd_active_elem_t * ) * repair->stake_weights_cnt );
    ulong leftovers_cnt = 0;

    ulong total_stake = 0UL;
    if( repair->stake_weights_cnt==0 ) {
      leftovers = fd_scratch_alloc(
        alignof( fd_active_elem_t * ),
        sizeof( fd_active_elem_t * ) * fd_active_table_key_cnt( repair->actives ) );

      for( fd_active_table_iter_t iter = fd_active_table_iter_init( repair->actives );
         !fd_active_table_iter_done( repair->actives, iter );
         iter = fd_active_table_iter_next( repair->actives, iter ) ) {
        fd_active_elem_t * peer = fd_active_table_iter_ele( repair->actives, iter );
        if( peer->sticky ) continue;
        leftovers[leftovers_cnt++] = peer;
      }
    } else {
      leftovers = fd_scratch_alloc(
        alignof( fd_active_elem_t * ),
        sizeof( fd_active_elem_t * ) * repair->stake_weights_cnt );

      for( ulong i = 0; i < repair->stake_weights_cnt; i++ ) {
        fd_stake_weight_t const * stake_weight = &repair->stake_weights[i];
        ulong stake = stake_weight->stake;
        if( !stake ) continue;
        fd_pubkey_t const * key = &stake_weight->key;
        fd_active_elem_t * peer = fd_active_table_query( repair->actives, key, NULL );
        if( peer!=NULL ) {
          peer->stake = stake;
          total_stake = fd_ulong_sat_add( total_stake, stake );
        }
        if( NULL == peer || peer->sticky ) continue;
        leftovers[leftovers_cnt++] = peer;
      }
    }

    fd_active_elem_t * best[FD_REPAIR_STICKY_MAX];
    ulong              best_cnt = 0;
    fd_active_elem_t * good[FD_REPAIR_STICKY_MAX];
    ulong              good_cnt = 0;

    long  latencies[ FD_REPAIR_STICKY_MAX ];
    ulong latencies_cnt = 0UL;

    long first_quartile_latency = LONG_MAX;

    /* fetch all latencies */
    for( fd_active_table_iter_t iter = fd_active_table_iter_init( repair->actives );
            !fd_active_table_iter_done( repair->actives, iter );
            iter = fd_active_table_iter_next( repair->actives, iter ) ) {
            fd_active_elem_t * peer = fd_active_table_iter_ele( repair->actives, iter );

      if( !peer->sticky ) {
        continue;
      }

      if( peer->avg_lat==0L || peer->avg_reps==0UL ) {
        continue;
      }

      latencies[ latencies_cnt++ ] = peer->avg_lat/(long)peer->avg_reps;
    }

    if( latencies_cnt >= 4 ) {
      /* we probably want a few peers before sorting and pruning them based on
         latency. */
      fd_latency_sort_inplace( latencies, latencies_cnt );
      first_quartile_latency = latencies[ latencies_cnt / 4UL ];
      FD_LOG_NOTICE(( "repair peers first quartile latency - latency: %6.6f ms", (double)first_quartile_latency * 1e-6 ));
    }

    /* Build the new sticky peers set based on the latency and stake weight */

    /* select an upper bound */
    /* acceptable latency is 2 * first quartile latency  */
    long acceptable_latency = first_quartile_latency != LONG_MAX ? 2L * first_quartile_latency : LONG_MAX;
    for( fd_active_table_iter_t iter = fd_active_table_iter_init( repair->actives );
         !fd_active_table_iter_done( repair->actives, iter );
         iter = fd_active_table_iter_next( repair->actives, iter ) ) {
      fd_active_elem_t * peer = fd_active_table_iter_ele( repair->actives, iter );
      uchar sticky = peer->sticky;
      peer->sticky = 0; /* Already clear the sticky bit */
      if( sticky ) {
        /* See if we still like this peer */
        if( peer->avg_reps>0UL && ( peer->avg_lat/(long)peer->avg_reps ) >= acceptable_latency ) {
          continue;
        }
        int r = is_good_peer( peer );
        if( r == 1 ) best[best_cnt++] = peer;
        else if( r == 0 ) good[good_cnt++] = peer;
      }
    }

    ulong tot_cnt = 0;
    for( ulong i = 0; i < best_cnt && tot_cnt < FD_REPAIR_STICKY_MAX - 2U; ++i ) {
      repair->actives_sticky[tot_cnt++] = best[i]->key;
      best[i]->sticky                       = (uchar)1;
    }
    for( ulong i = 0; i < good_cnt && tot_cnt < FD_REPAIR_STICKY_MAX - 2U; ++i ) {
      repair->actives_sticky[tot_cnt++] = good[i]->key;
      good[i]->sticky                       = (uchar)1;
    }
    if( leftovers_cnt ) {
      /* Sample 64 new sticky peers using stake-weighted sampling */
      for( ulong i = 0; i < 64 && tot_cnt < FD_REPAIR_STICKY_MAX && tot_cnt < fd_active_table_key_cnt( repair->actives ); ++i ) {
        /* Generate a random amount of culmative stake at which to sample the peer */
        ulong target_culm_stake = fd_rng_ulong( repair->rng ) % total_stake;

        /* Iterate over the active peers until we find the randomly selected peer */
        ulong culm_stake = 0UL;
        fd_active_elem_t * peer = NULL;
        for( fd_active_table_iter_t iter = fd_active_table_iter_init( repair->actives );
          !fd_active_table_iter_done( repair->actives, iter );
          iter = fd_active_table_iter_next( repair->actives, iter ) ) {
            peer = fd_active_table_iter_ele( repair->actives, iter );
            culm_stake = fd_ulong_sat_add( culm_stake, peer->stake );
            if( FD_UNLIKELY(( culm_stake >= target_culm_stake )) ) {
              break;
            }
        }

        /* Select this peer as sticky */
        if( FD_LIKELY(( peer && !peer->sticky )) ) {
          repair->actives_sticky[tot_cnt++] = peer->key;
          peer->sticky                      = (uchar)1;
        }
      }

    }
    repair->actives_sticky_cnt = tot_cnt;

    FD_LOG_NOTICE(
        ( "selected %lu (previously: %lu) peers for repair (best was %lu, good was %lu, leftovers was %lu) (nonce_diff: %u)",
          tot_cnt,
          prev_sticky_cnt,
          best_cnt,
          good_cnt,
          leftovers_cnt,
          repair->next_nonce - repair->current_nonce ) );
  }
  FD_SCRATCH_SCOPE_END;
}

static fd_active_elem_t *
actives_sample( fd_repair_t * repair ) {
  ulong seed = repair->actives_random_seed;
  ulong actives_sticky_cnt = repair->actives_sticky_cnt;
  while( actives_sticky_cnt ) {
    seed += 774583887101UL;
    fd_pubkey_t *      id   = &repair->actives_sticky[seed % actives_sticky_cnt];
    fd_active_elem_t * peer = fd_active_table_query( repair->actives, id, NULL );
    if( NULL != peer ) {
      if( peer->first_request_time == 0U ) peer->first_request_time = repair->now;
      /* Aggressively throw away bad peers */
      if( repair->now - peer->first_request_time < (long)5e9 || /* Sample the peer for at least 5 seconds */
          is_good_peer( peer ) != -1 ) {
        repair->actives_random_seed = seed;
        return peer;
      }
      peer->sticky = 0;
    }
    *id = repair->actives_sticky[--( actives_sticky_cnt )];
  }
  return NULL;
}

static int
fd_repair_create_needed_request( fd_repair_t * glob, int type, ulong slot, uint shred_index ) {
  fd_repair_lock( glob );

  /* If there are no active sticky peers from which to send requests to, refresh the sticky peers
     selection. It may be that stake weights were not available before, and now they are. */
  if ( glob->actives_sticky_cnt == 0 ) {
    fd_actives_shuffle( glob );
  }

  fd_pubkey_t * ids[FD_REPAIR_NUM_NEEDED_PEERS] = {0};
  uint found_peer = 0;
  uint peer_cnt = fd_uint_min( (uint)glob->actives_sticky_cnt, FD_REPAIR_NUM_NEEDED_PEERS );
  for( ulong i=0UL; i<peer_cnt; i++ ) {
    fd_active_elem_t * peer = actives_sample( glob );
    if(!peer) continue;
    found_peer = 1;

    ids[i] = &peer->key;
  }

  if (!found_peer) {
    FD_LOG_DEBUG( ( "failed to find a good peer." ) );
    fd_repair_unlock( glob );
    return -1;
  };

  fd_dupdetect_key_t dupkey = { .type = (enum fd_needed_elem_type)type, .slot = slot, .shred_index = shred_index };
  fd_dupdetect_elem_t * dupelem = fd_dupdetect_table_query( glob->dupdetect, &dupkey, NULL );
  if( dupelem == NULL ) {
    dupelem = fd_dupdetect_table_insert( glob->dupdetect, &dupkey );
    dupelem->last_send_time = 0L;
  } else if( ( dupelem->last_send_time+(long)200e6 )<glob->now ) {
    fd_repair_unlock( glob );
    return 0;
  }

  dupelem->last_send_time = glob->now;
  dupelem->req_cnt = peer_cnt;

  if (fd_needed_table_is_full(glob->needed)) {
    fd_repair_unlock( glob );
    FD_LOG_NOTICE(("table full"));
    ( *glob->deliver_fail_fun )(ids[0], slot, shred_index, glob->fun_arg, FD_REPAIR_DELIVER_FAIL_REQ_LIMIT_EXCEEDED );
    return -1;
  }
  for( ulong i=0UL; i<peer_cnt; i++ ) {
    fd_repair_nonce_t key = glob->next_nonce++;
    fd_needed_elem_t * val = fd_needed_table_insert(glob->needed, &key);
    fd_hash_copy(&val->id, ids[i]);
    val->dupkey = dupkey;
    val->when = glob->now;
  }
  fd_repair_unlock( glob );
  return 0;
}

static int
fd_write_good_peer_cache_file( fd_repair_t * repair ) {
  // return 0;

  if ( repair->good_peer_cache_file_fd == -1 ) {
    return 0;
  }

  if ( repair->actives_sticky_cnt == 0 ) {
    return 0;
  }

  /* Truncate the file before we write it */
  int err = ftruncate( repair->good_peer_cache_file_fd, 0UL );
  if( FD_UNLIKELY( err==-1 ) ) {
    FD_LOG_WARNING(( "Failed to truncate the good peer cache file (%i-%s)", errno, fd_io_strerror( errno ) ));
    return 1;
  }
  long seek = lseek( repair->good_peer_cache_file_fd, 0UL, SEEK_SET );
  if( FD_UNLIKELY( seek!=0L ) ) {
    FD_LOG_WARNING(( "Failed to seek to the beginning of the good peer cache file" ));
    return 1;
  }

  /* Write the active sticky peers to file in the format:
     "base58EncodedPubkey/ipAddr/port"

     Where ipAddr is in dotted-decimal (e.g. "1.2.3.4")
     and port is decimal, in host order (e.g. "8001").
  */
  for( ulong i = 0UL; i < repair->actives_sticky_cnt; i++ ) {
    fd_pubkey_t *      id   = &repair->actives_sticky[ i ];
    fd_active_elem_t * peer = fd_active_table_query( repair->actives, id, NULL );
    if ( peer == NULL ) {
      continue;
    }

    /* Convert the public key to base58 */
    char base58_str[ FD_BASE58_ENCODED_32_SZ ];
    fd_base58_encode_32( peer->key.uc, NULL, base58_str );

    /* Convert the IP address to dotted-decimal string.  The address
       in peer->addr.addr is already in network byte order. */
    struct in_addr addr_parsed;
    addr_parsed.s_addr = peer->addr.addr; /* net-order -> struct in_addr */
    char * ip_str = inet_ntoa( addr_parsed );

    /* Convert port from network byte order to host byte order. */
    ushort port = fd_ushort_bswap( peer->addr.port );

    /* Write out line: base58EncodedPubkey/ipAddr/port */
    dprintf( repair->good_peer_cache_file_fd, "%s/%s/%u\n", base58_str, ip_str, (uint)port );
  }

  return 0;
}

int
fd_repair_need_window_index( fd_repair_t * glob, ulong slot, uint shred_index ) {
  FD_LOG_DEBUG(( "[%s] need window %lu, shred_index %u", __func__, slot, shred_index ));
  return fd_repair_create_needed_request( glob, fd_needed_window_index, slot, shred_index );
}

int
fd_repair_need_highest_window_index( fd_repair_t * glob, ulong slot, uint shred_index ) {
  FD_LOG_DEBUG(( "[%s] need highest %lu", __func__, slot ));
  return fd_repair_create_needed_request( glob, fd_needed_highest_window_index, slot, shred_index );
}

int
fd_repair_need_orphan( fd_repair_t * glob, ulong slot ) {
  FD_LOG_NOTICE(( "[%s] need orphan %lu", __func__, slot ));
  return fd_repair_create_needed_request( glob, fd_needed_orphan, slot, UINT_MAX );
}

static void
print_stats( fd_active_elem_t * val ) {
  fd_pubkey_t const * id = &val->key;
  if( FD_UNLIKELY( NULL == val ) ) return;
  if( val->avg_reqs == 0 )
    FD_LOG_INFO(( "repair peer %s: no requests sent, stake=%lu", FD_BASE58_ENC_32_ALLOCA( id ), val->stake / (ulong)1e9 ));
  else if( val->avg_reps == 0 )
    FD_LOG_INFO(( "repair peer %s: avg_requests=%lu, no responses received, stake=%lu", FD_BASE58_ENC_32_ALLOCA( id ), val->avg_reqs, val->stake / (ulong)1e9 ));
  else
    FD_LOG_INFO(( "repair peer %s: avg_requests=%lu, response_rate=%f, latency=%f, stake=%lu",
                    FD_BASE58_ENC_32_ALLOCA( id ),
                    val->avg_reqs,
                    ((double)val->avg_reps)/((double)val->avg_reqs),
                    1.0e-9*((double)val->avg_lat)/((double)val->avg_reps),
                    val->stake / (ulong)1e9 ));
}

static void
fd_repair_print_all_stats( fd_repair_t * glob ) {
  for( fd_active_table_iter_t iter = fd_active_table_iter_init( glob->actives );
       !fd_active_table_iter_done( glob->actives, iter );
       iter = fd_active_table_iter_next( glob->actives, iter ) ) {
    fd_active_elem_t * val = fd_active_table_iter_ele( glob->actives, iter );
    if( !val->sticky ) continue;
    print_stats( val );
  }
  FD_LOG_INFO( ( "peer count: %lu", fd_active_table_key_cnt( glob->actives ) ) );
}

void fd_repair_add_sticky( fd_repair_t * glob, fd_pubkey_t const * id ) {
  fd_repair_lock( glob );
  glob->actives_sticky[glob->actives_sticky_cnt++] = *id;
  fd_repair_unlock( glob );
}

void
fd_repair_set_stake_weights( fd_repair_t * repair,
                             fd_stake_weight_t const * stake_weights,
                             ulong stake_weights_cnt ) {
  if( stake_weights == NULL ) {
    FD_LOG_ERR(( "stake weights NULL" ));
  }
  if( stake_weights_cnt > FD_STAKE_WEIGHTS_MAX ) {
    FD_LOG_ERR(( "too many stake weights" ));
  }

  fd_repair_lock( repair );

  fd_memset( repair->stake_weights, 0, FD_STAKE_WEIGHTS_MAX * fd_stake_weight_footprint() );
  fd_memcpy( repair->stake_weights, stake_weights, stake_weights_cnt * sizeof(fd_stake_weight_t) );
  repair->stake_weights_cnt = stake_weights_cnt;

  fd_repair_unlock( repair );
}

static void
fd_repair_send_ping( fd_repair_t *                 glob,
                     fd_gossip_peer_addr_t const * dst_addr,
                     uint                          src_ip4_addr,
                     fd_pinged_elem_t *            val ) {
  fd_repair_response_t gmsg;
  fd_repair_response_new_disc( &gmsg, fd_repair_response_enum_ping );
  fd_gossip_ping_t * ping = &gmsg.inner.ping;
  fd_hash_copy( &ping->from, glob->public_key );

  uchar pre_image[FD_PING_PRE_IMAGE_SZ];
  memcpy( pre_image, "SOLANA_PING_PONG", 16UL );
  memcpy( pre_image+16UL, val->token.uc, 32UL );

  fd_sha256_hash( pre_image, FD_PING_PRE_IMAGE_SZ, &ping->token );

  glob->sign_fun( glob->sign_arg, ping->signature.uc, pre_image, FD_PING_PRE_IMAGE_SZ, FD_KEYGUARD_SIGN_TYPE_SHA256_ED25519 );

  fd_bincode_encode_ctx_t ctx;
  uchar buf[1024];
  ctx.data = buf;
  ctx.dataend = buf + sizeof(buf);
  FD_TEST(0 == fd_repair_response_encode(&gmsg, &ctx));
  ulong buflen = (ulong)((uchar*)ctx.data - buf);

  glob->serv_send_fun( buf, buflen, dst_addr, src_ip4_addr, glob->fun_arg );
}

static void
fd_repair_recv_pong(fd_repair_t * glob, fd_gossip_ping_t const * pong, fd_gossip_peer_addr_t const * from) {
  fd_pinged_elem_t * val = fd_pinged_table_query(glob->pinged, from, NULL);
  if( val == NULL || !fd_hash_eq( &val->id, &pong->from ) )
    return;

  /* Verify response hash token */
  uchar pre_image[FD_PING_PRE_IMAGE_SZ];
  memcpy( pre_image, "SOLANA_PING_PONG", 16UL );
  memcpy( pre_image+16UL, val->token.uc, 32UL );

  fd_hash_t pre_image_hash;
  fd_sha256_hash( pre_image, FD_PING_PRE_IMAGE_SZ, pre_image_hash.uc );

  fd_sha256_t sha[1];
  fd_sha256_init( sha );
  fd_sha256_append( sha, "SOLANA_PING_PONG", 16UL );
  fd_sha256_append( sha, pre_image_hash.uc,  32UL );
  fd_hash_t golden;
  fd_sha256_fini( sha, golden.uc );

  fd_sha512_t sha2[1];
  if( fd_ed25519_verify( /* msg */ golden.uc,
                         /* sz */ 32U,
                         /* sig */ pong->signature.uc,
                         /* public_key */ pong->from.uc,
                         sha2 )) {
    FD_LOG_WARNING(("Failed sig verify for pong"));
    return;
  }

  val->good = 1;
}

int
fd_repair_recv_serv_packet( fd_repair_t *                 glob,
                            uchar *                       msg,
                            ulong                         msglen,
                            fd_repair_peer_addr_t const * peer_addr,
                            uint                          self_ip4_addr ) {
  //ulong recv_serv_packet;
  //ulong recv_serv_pkt_types[FD_METRICS_ENUM_SENT_REQUEST_TYPES_CNT];

  FD_SCRATCH_SCOPE_BEGIN {
    fd_bincode_decode_ctx_t ctx = {
      .data    = msg,
      .dataend = msg + msglen
    };

    ulong total_sz = 0UL;
    if( FD_UNLIKELY( fd_repair_protocol_decode_footprint( &ctx, &total_sz ) ) ) {
      glob->metrics.recv_serv_corrupt_pkt++;
      FD_LOG_WARNING(( "Failed to decode repair request packet" ));
      return 0;
    }

    glob->metrics.recv_serv_pkt++;

    uchar * mem = fd_scratch_alloc( fd_repair_protocol_align(), total_sz );
    if( FD_UNLIKELY( !mem ) ) {
      FD_LOG_ERR(( "Unable to allocate memory for repair protocol" ));
    }

    fd_repair_protocol_t * protocol = fd_repair_protocol_decode( mem, &ctx );

    if( FD_UNLIKELY( ctx.data != ctx.dataend ) ) {
      FD_LOG_WARNING(( "failed to decode repair request packet" ));
      return 0;
    }

    fd_repair_request_header_t * header;
    switch( protocol->discriminant ) {
      case fd_repair_protocol_enum_pong:
        glob->metrics.recv_serv_pkt_types[FD_METRICS_ENUM_REPAIR_SERV_PKT_TYPES_V_PONG_IDX]++;
        fd_repair_lock( glob );
        fd_repair_recv_pong( glob, &protocol->inner.pong, peer_addr );
        fd_repair_unlock( glob );
        return 0;
      case fd_repair_protocol_enum_window_index: {
        glob->metrics.recv_serv_pkt_types[FD_METRICS_ENUM_REPAIR_SERV_PKT_TYPES_V_WINDOW_IDX]++;
        fd_repair_window_index_t * wi = &protocol->inner.window_index;
        header = &wi->header;
        break;
      }
      case fd_repair_protocol_enum_highest_window_index: {
        glob->metrics.recv_serv_pkt_types[FD_METRICS_ENUM_REPAIR_SERV_PKT_TYPES_V_HIGHEST_WINDOW_IDX]++;
        fd_repair_highest_window_index_t * wi = &protocol->inner.highest_window_index;
        header = &wi->header;
        break;
      }
      case fd_repair_protocol_enum_orphan: {
        glob->metrics.recv_serv_pkt_types[FD_METRICS_ENUM_REPAIR_SERV_PKT_TYPES_V_ORPHAN_IDX]++;
        fd_repair_orphan_t * wi = &protocol->inner.orphan;
        header = &wi->header;
        break;
      }
      default: {
        glob->metrics.recv_serv_pkt_types[FD_METRICS_ENUM_REPAIR_SERV_PKT_TYPES_V_UNKNOWN_IDX]++;
        FD_LOG_WARNING(( "received repair request of unknown type: %d", (int)protocol->discriminant ));
        return 0;
      }
    }

    if( FD_UNLIKELY( !fd_hash_eq( &header->recipient, glob->public_key ) ) ) {
      FD_LOG_WARNING(( "received repair request with wrong recipient, %s instead of %s", FD_BASE58_ENC_32_ALLOCA( header->recipient.uc ), FD_BASE58_ENC_32_ALLOCA( glob->public_key ) ));
      return 0;
    }

    /* Verify the signature */
    fd_sha512_t sha2[1];
    fd_signature_t sig;
    fd_memcpy( &sig, header->signature.uc, sizeof(sig) );
    fd_memcpy( (uchar *)msg + 64U, msg, 4U );
    if( fd_ed25519_verify( /* msg */ msg + 64U,
                           /* sz */ msglen - 64U,
                           /* sig */ sig.uc,
                           /* public_key */ header->sender.uc,
                           sha2 )) {
      glob->metrics.recv_serv_invalid_signature++;
      FD_LOG_WARNING(( "received repair request with with invalid signature" ));
      return 0;
    }

    fd_repair_lock( glob );

    fd_pinged_elem_t * val = fd_pinged_table_query( glob->pinged, peer_addr, NULL) ;
    if( val == NULL || !val->good || !fd_hash_eq( &val->id, &header->sender ) ) {
      /* Need to ping this client */
      if( val == NULL ) {
        if( fd_pinged_table_is_full( glob->pinged ) ) {
          FD_LOG_WARNING(( "pinged table is full" ));
          fd_repair_unlock( glob );
          glob->metrics.recv_serv_full_ping_table++;
          return 0;
        }
        val = fd_pinged_table_insert( glob->pinged, peer_addr );
        for ( ulong i = 0; i < FD_HASH_FOOTPRINT / sizeof(ulong); i++ )
          val->token.ul[i] = fd_rng_ulong(glob->rng);
      }
      fd_hash_copy( &val->id, &header->sender );
      val->good = 0;
      fd_repair_send_ping( glob, peer_addr, self_ip4_addr, val );

    } else {
      uchar buf[FD_SHRED_MAX_SZ + sizeof(uint)];
      switch( protocol->discriminant ) {
        case fd_repair_protocol_enum_window_index: {
          fd_repair_window_index_t const * wi = &protocol->inner.window_index;
          long sz = (*glob->serv_get_shred_fun)( wi->slot, (uint)wi->shred_index, buf, FD_SHRED_MAX_SZ, glob->fun_arg );
          if( sz < 0 ) break;
          *(uint *)(buf + sz) = wi->header.nonce;
          glob->serv_send_fun( buf, (ulong)sz + sizeof(uint), peer_addr, self_ip4_addr, glob->fun_arg );
          break;
        }

        case fd_repair_protocol_enum_highest_window_index: {
          fd_repair_highest_window_index_t const * wi = &protocol->inner.highest_window_index;
          long sz = (*glob->serv_get_shred_fun)( wi->slot, UINT_MAX, buf, FD_SHRED_MAX_SZ, glob->fun_arg );
          if( sz < 0 ) break;
          *(uint *)(buf + sz) = wi->header.nonce;
          glob->serv_send_fun( buf, (ulong)sz + sizeof(uint), peer_addr, self_ip4_addr, glob->fun_arg );
          break;
        }

        case fd_repair_protocol_enum_orphan: {
          fd_repair_orphan_t const * wi = &protocol->inner.orphan;
          ulong slot = wi->slot;
          for(unsigned i = 0; i < 10; ++i) {
            slot = (*glob->serv_get_parent_fun)( slot, glob->fun_arg );
            /* We cannot serve slots <= 1 since they are empy and created at genesis. */
            if( slot == FD_SLOT_NULL || slot <= 1UL ) break;
            long sz = (*glob->serv_get_shred_fun)( slot, UINT_MAX, buf, FD_SHRED_MAX_SZ, glob->fun_arg );
            if( sz < 0 ) continue;
            *(uint *)(buf + sz) = wi->header.nonce;
            glob->serv_send_fun( buf, (ulong)sz + sizeof(uint), peer_addr, self_ip4_addr, glob->fun_arg );
          }
          break;
        }

        default:
          break;
        }
    }

    fd_repair_unlock( glob );
  } FD_SCRATCH_SCOPE_END;
  return 0;
}

fd_repair_metrics_t *
fd_repair_get_metrics( fd_repair_t * repair ) {
  return &repair->metrics;
}
