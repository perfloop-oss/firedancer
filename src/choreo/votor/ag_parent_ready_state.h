#ifndef HEADER_fd_src_choreo_votor_ag_parent_ready_state_h
#define HEADER_fd_src_choreo_votor_ag_parent_ready_state_h

#include "ag_votor_base.h"

/* notar_fallbacks holds one hash per notar(-fallback) cert the slot has,
   which Lemma 48 bounds.  ready holds the parents a window start was handed,
   and a skip run funnels the whole window below it into one start, so its
   bound is a window's worth of those. */

struct ag_parent_ready_state {
  ulong         slot; /* map key */
  ulong         next; /* reserved for fd_pool, fd_map_chain */

  int           skip;

  fd_hash_t     notar_fallbacks[AG_NOTAR_FALLBACK_CERT_MAX];
  uchar         notar_fallbacks_cnt;

  int           is_ready;
  ag_block_id_t ready_ids[AG_SLOTS_PER_WINDOW*AG_NOTAR_FALLBACK_CERT_MAX];
  ulong         ready_id_cnt;
};
typedef struct ag_parent_ready_state ag_parent_ready_state_t;

FD_PROTOTYPES_BEGIN

void
ag_parent_ready_state_init( ag_parent_ready_state_t * state,
                            ulong                     slot );

void
ag_parent_ready_state_genesis( ag_parent_ready_state_t * state,
                               ulong                     slot );

int
ag_parent_ready_state_mark_skip( ag_parent_ready_state_t * self );

FD_FN_PURE int
ag_parent_ready_state_is_skip_certified( ag_parent_ready_state_t const * self );

int
ag_parent_ready_state_mark_notar_fallback( ag_parent_ready_state_t * self,
                                           fd_hash_t const *         hash );

FD_FN_PURE fd_hash_t const *
ag_parent_ready_state_notar_fallback_blocks( ag_parent_ready_state_t const * self,
                                             ulong *                         cnt );

void
ag_parent_ready_state_add_to_ready( ag_parent_ready_state_t * self,
                                    ag_block_id_t const *     id );

FD_FN_PURE ag_block_id_t const *
ag_parent_ready_state_ready_block_ids( ag_parent_ready_state_t const * self,
                                       ulong *                         cnt );

ag_block_id_t
ag_parent_ready_state_wait_for_parent_ready( ag_parent_ready_state_t * self );

FD_PROTOTYPES_END

#endif
