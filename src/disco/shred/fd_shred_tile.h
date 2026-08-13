#ifndef HEADER_fd_src_disco_shred_fd_shred_tile_h
#define HEADER_fd_src_disco_shred_fd_shred_tile_h

#include "../tiles.h"
#include "../keyguard/fd_keyguard_client.h"
#include "fd_fec_resolver.h"

/* Forward declarations */
typedef struct fd_fec_resolver fd_fec_resolver_t;
typedef struct fd_keyswitch_private fd_keyswitch_t;
typedef struct fd_keyguard_client fd_keyguard_client_t;

/* Shred tile context structure */
typedef struct {
  fd_shredder_t      * shredder;
  fd_fec_resolver_t  * resolver;
  fd_pubkey_t          identity_key[1]; /* Just the public key */
  /* ... rest of the structure members ... */
} fd_shred_shared_ctx_t;

/* shred_out has 3 possible message types, but 8 different message sigs
   to differentiate between different data sources.  All individual
   shred messages
   SHRED_SIG_SRC_{TURBINE,LEADER,RECONSTRUCTED,REPAIR,BAD_REPAIR} have
   the same dcache type fd_shred_base_t.  Only repair/bad_repair shreds
   will populate the rnonce field.

   SHRED_SIG_FEC_{EVICTED,COMPLETE,COMPLETE_LEADER} are not generated on
   every shred, but rather on events where a FEC set is completed by the
   fec_resolver or evicted from the fec_resolver.  In the case that a
   FEC set is completed by the 32nd shred in a FEC set, both a shred
   message and a complete message will be published.  It is convenient
   for downstream consumers to have both messages available.

   The last 32 LSB of sig is the data source of the message, and the
   first 32 MSB is the shred processing result. */

/* shred_base_t sigs */
#define SHRED_SIG_SRC_TURBINE         (0U) /* turbine shred */
#define SHRED_SIG_SRC_LEADER          (1U) /* shred created by leader */
#define SHRED_SIG_SRC_RECONSTRUCTED   (2U) /* reconstructed data shred */
#define SHRED_SIG_SRC_REPAIR          (3U) /* repair shred */
#define SHRED_SIG_SRC_BAD_REPAIR      (4U) /* repair shred with unverifiable nonce */

/* fec_resolver event sigs */
#define SHRED_SIG_FEC_EVICTED         (5UL) /* evicted */
#define SHRED_SIG_FEC_COMPLETE        (6UL) /* FEC set complete */
#define SHRED_SIG_FEC_COMPLETE_LEADER (7UL) /* leader FEC set complete */

/* shred processing result (first 32 bits of sig) */
#define SHRED_SIG_RESULT_COMPLETES     ( 1)
#define SHRED_SIG_RESULT_OKAY          ( 0) /* default */
#define SHRED_SIG_RESULT_DUPLICATE     (-1)
#define SHRED_SIG_RESULT_EQVOC         (-4)

FD_STATIC_ASSERT( SHRED_SIG_RESULT_COMPLETES == FD_FEC_RESOLVER_SHRED_COMPLETES, "shred sig result does not match fec_resolver result" );
FD_STATIC_ASSERT( SHRED_SIG_RESULT_OKAY      == FD_FEC_RESOLVER_SHRED_OKAY,      "shred sig result does not match fec_resolver result" );
FD_STATIC_ASSERT( SHRED_SIG_RESULT_DUPLICATE == FD_FEC_RESOLVER_SHRED_DUPLICATE, "shred sig result does not match fec_resolver result" );
FD_STATIC_ASSERT( SHRED_SIG_RESULT_EQVOC     == FD_FEC_RESOLVER_SHRED_EQUIVOC,   "shred sig result does not match fec_resolver result" );

static inline int  fd_shred_sig_res( ulong sig ) { return (int)(sig >> 32UL); }
static inline uint fd_shred_sig_src( ulong sig ) { return (uint)sig; }

/* For all individual shred messages:
   SHRED_SIG_SRC_{TURBINE,LEADER,RECONSTRUCTED,REPAIR,BAD_REPAIR} */
struct fd_shred_base {
  fd_hash_t merkle_root;
  union {
    uchar        shred_[ FD_SHRED_MAX_SZ ];
    fd_shred_t   shred;
  };
  uint      rnonce;        /* populated only for repair/bad_repair shreds */
};
typedef struct fd_shred_base fd_shred_base_t;

/* For the FEC evicted message: SHRED_SIG_FEC_EVICTED */
struct fd_fec_evicted {
  ulong slot;
  uint  fec_set_idx;
};
typedef struct fd_fec_evicted fd_fec_evicted_t;


/* For an FEC complete message: SHRED_SIG_FEC_COMPLETE or SHRED_SIG_FEC_COMPLETE_LEADER */
struct fd_fec_complete {
  fd_hash_t  merkle_root;    /* placed first to match format of shred base */
  fd_shred_t last_shred_hdr; /* header of last data shred in the FEC set */
  fd_hash_t  chained_merkle_root;

  /* Block-level telemetry snapshot, filled in by the repair tile (which
     owns the forest) when it forwards the FEC-complete to replay; see
     fd_repair_tile.c after_fec().  The shred tile emits this message too
     (on its own shred_out link, ahead of repair), but it has no forest:
     it zeroes only stats_valid and fec_completed_ts_nanos, leaving the
     blk_* fields uninitialized.  Repair fills the stats when its forest
     still tracks the block and leaves stats_valid==0 otherwise, so
     consumers must not read blk_* unless stats_valid==1.

     These are a snapshot of the whole block's forest counters as of this
     FEC.  They grow as shreds arrive, so they are only final (and read by
     replay) on the slot-complete FEC.  Per-FEC data replay needs comes
     from its own reasm stash, not from here. */
  uint  stats_valid;             /* 1 if the repair tile filled the stats below */
  uint  blk_turbine_cnt;         /* block: shreds received via turbine */
  uint  blk_repair_cnt;          /* block: data shreds received via repair */
  uint  blk_recovered_cnt;       /* block: shreds recovered via reed-solomon */
  uint  blk_data_cnt;            /* block: data shreds received so far (== complete_idx+1 at slot complete) */
  uint  blk_parity_cnt;          /* block: coding (parity) shreds received so far */
  uint  blk_lowest_verified_fec; /* block: lowest verified FEC index, UINT_MAX if none */
  uchar blk_chain_confirmed;     /* block: 1 if the whole chain has been merkle-verified */
  uchar blk_slot_complete;       /* block: 1 if the slot-complete shred has been received */

  /* Per-slot repair request/response tallies (forest req_*_cnt / response_cnt). */
  uint  blk_req_window_cnt;      /* block: window (specific-shred) repair requests sent */
  uint  blk_req_highest_cnt;     /* block: highest-window repair requests sent */
  uint  blk_req_orphan_cnt;      /* block: orphan repair requests sent */
  uint  blk_req_retransmit_cnt;  /* block: repair requests re-sent after a response timeout */
  uint  blk_repair_responses;    /* block: repair responses received */
  uchar blk_chain_verify_failed; /* block: 1 if merkle chain verification flagged this block bad */

  /* When the shred completing this FEC set arrived from the network
     (wallclock nanoseconds, converted by the repair tile from the shred
     tile's arrival stamp; for recovered FEC sets, the arrival of the
     shred that triggered recovery).  0 if unavailable.  Unlike the blk_*
     stats above this is per-FEC and meaningful on every FEC-complete. */
  ulong fec_completed_ts_nanos;

  /* Per-slot timestamps, converted from the forest's fd_tickcount() values
     to wallclock nanoseconds (unix epoch) by the repair tile.  0 if unset. */
  ulong blk_first_shred_ts_nanos;   /* when the first shred of the slot arrived */
  ulong blk_last_shred_ts_nanos;    /* when the slot became fully buffered */
  ulong blk_first_req_ts_nanos;     /* when the first repair request for the slot was sent */
  ulong blk_last_repair_resp_ts_nanos; /* when the most recent repair response for the slot arrived */
};
typedef struct fd_fec_complete fd_fec_complete_t;

union fd_shred_message {
  fd_shred_base_t   shred;
  fd_fec_evicted_t  evicted;
  fd_fec_complete_t complete;
};
typedef union fd_shred_message fd_shred_message_t;

#endif /* HEADER_fd_src_disco_shred_fd_shred_tile_h */
