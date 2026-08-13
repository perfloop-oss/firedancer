#include "ag_parent_ready_tracker.h"

/* Window arithmetic.  Only the parent-ready walk cares where a window
   starts and ends -- readiness is granted at window starts -- so it keeps
   these to itself. */

FD_FN_CONST static inline ulong
ag_slot_first_slot_in_window( ulong slot ) {
  return ( slot / AG_SLOTS_PER_WINDOW ) * AG_SLOTS_PER_WINDOW;
}

FD_FN_CONST static inline ulong
ag_slot_last_slot_in_window( ulong slot ) {
  return ag_slot_first_slot_in_window( slot ) + AG_SLOTS_PER_WINDOW - 1UL;
}

FD_FN_CONST static inline int
ag_slot_is_start_of_window( ulong slot ) {
  return ( slot % AG_SLOTS_PER_WINDOW )==0UL;
}

FD_FN_CONST static inline int
ag_slot_is_genesis_window( ulong slot ) {
  return slot < AG_SLOTS_PER_WINDOW;
}

#define PARENT_READY_PER_WINDOW (AG_SLOTS_PER_WINDOW*AG_NOTAR_FALLBACK_CERT_MAX)

ulong
ag_parent_ready_tracker_align( void ) {
  return alignof(ag_parent_ready_tracker_t);
}

ulong
ag_parent_ready_tracker_footprint( ulong slot_max ) {
  slot_max = fd_ulong_pow2_up( slot_max );
  ulong chain_cnt = ag_parent_ready_state_map_chain_cnt_est( slot_max );
  return FD_LAYOUT_FINI(
    FD_LAYOUT_APPEND(
    FD_LAYOUT_APPEND(
    FD_LAYOUT_APPEND(
    FD_LAYOUT_INIT,
      alignof(ag_parent_ready_tracker_t), sizeof(ag_parent_ready_tracker_t)                            ),
      ag_parent_ready_state_pool_align(), ag_parent_ready_state_pool_footprint( slot_max )             ),
      ag_parent_ready_state_map_align(),  ag_parent_ready_state_map_footprint ( chain_cnt )            ),
    ag_parent_ready_tracker_align() );
}

void *
ag_parent_ready_tracker_new( void * shmem,
                             ulong  slot_max,
                             ulong  seed ) {
  if( FD_UNLIKELY( !shmem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)shmem, ag_parent_ready_tracker_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned mem" ));
    return NULL;
  }

  ulong footprint = ag_parent_ready_tracker_footprint( slot_max );
  if( FD_UNLIKELY( !footprint ) ) {
    FD_LOG_WARNING(( "bad slot_max (%lu)", slot_max ));
    return NULL;
  }

  slot_max = fd_ulong_pow2_up( slot_max );

  fd_memset( shmem, 0, footprint );

  ulong chain_cnt = ag_parent_ready_state_map_chain_cnt_est( slot_max );

  FD_SCRATCH_ALLOC_INIT( l, shmem );
  ag_parent_ready_tracker_t * tracker    = FD_SCRATCH_ALLOC_APPEND( l, alignof(ag_parent_ready_tracker_t), sizeof(ag_parent_ready_tracker_t)                 );
  void *                      state_pool = FD_SCRATCH_ALLOC_APPEND( l, ag_parent_ready_state_pool_align(), ag_parent_ready_state_pool_footprint( slot_max )  );
  void *                      state_map  = FD_SCRATCH_ALLOC_APPEND( l, ag_parent_ready_state_map_align(),  ag_parent_ready_state_map_footprint ( chain_cnt )           );
  FD_TEST( FD_SCRATCH_ALLOC_FINI( l, ag_parent_ready_tracker_align() ) == (ulong)shmem + footprint );

  tracker->root        = 0UL;
  tracker->states.pool = ag_parent_ready_state_pool_join( ag_parent_ready_state_pool_new( state_pool, slot_max        ) );
  tracker->states.map  = ag_parent_ready_state_map_join ( ag_parent_ready_state_map_new ( state_map,  chain_cnt, seed ) );


  return shmem;
}

ag_parent_ready_tracker_t *
ag_parent_ready_tracker_join( void * shtracker ) {
  ag_parent_ready_tracker_t * tracker = (ag_parent_ready_tracker_t *)shtracker;

  if( FD_UNLIKELY( !tracker ) ) {
    FD_LOG_WARNING(( "NULL tracker" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)tracker, ag_parent_ready_tracker_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned tracker" ));
    return NULL;
  }

  return tracker;
}

void *
ag_parent_ready_tracker_leave( ag_parent_ready_tracker_t const * tracker ) {
  if( FD_UNLIKELY( !tracker ) ) {
    FD_LOG_WARNING(( "NULL tracker" ));
    return NULL;
  }
  return (void *)tracker;
}

void *
ag_parent_ready_tracker_delete( void * shtracker ) {
  if( FD_UNLIKELY( !shtracker ) ) {
    FD_LOG_WARNING(( "NULL tracker" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)shtracker, ag_parent_ready_tracker_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned tracker" ));
    return NULL;
  }

  return shtracker;
}

FD_FN_PURE ulong
ag_parent_ready_tracker_root( ag_parent_ready_tracker_t const * self ) {
  return self->root;
}

static ag_parent_ready_state_t *
slot_state( ag_parent_ready_tracker_t * self,
            ulong                       slot ) {
  ag_parent_ready_state_t * state = ag_parent_ready_state_map_ele_query( self->states.map, &slot, NULL, self->states.pool );
  if( FD_LIKELY( state ) ) return state;

  ag_parent_ready_state_t * pool = self->states.pool;
  if( FD_UNLIKELY( !ag_parent_ready_state_pool_free( pool ) ) ) {
    FD_LOG_ERR(( "parent_ready_tracker: state pool exhausted (slot_max exceeded) at slot %lu", slot ));
  }

  state = ag_parent_ready_state_pool_ele_acquire( pool );
  ag_parent_ready_state_init( state, slot );
  ag_parent_ready_state_map_ele_insert( self->states.map, state, pool );
  return state;
}

void
ag_parent_ready_tracker_mark_notar_fallback( ag_parent_ready_tracker_t * self,
                                             ag_block_id_t const *       id,
                                             ag_parent_ready_t *         newly_certified,
                                             ulong *                     newly_certified_cnt ) {
  ag_parent_ready_t * out     = newly_certified;
  ulong *             out_cnt = newly_certified_cnt;
  *out_cnt = 0UL;

  ulong             slot = id->slot;
  fd_hash_t const * hash = &id->hash;

  if( FD_UNLIKELY( slot < self->root ) ) return;

  ag_parent_ready_state_t * state = slot_state( self, slot );
  if( !ag_parent_ready_state_mark_notar_fallback( state, hash ) ) return;

  for( ulong slot_=slot+1; ; slot_++ ) {
    ag_parent_ready_state_t * state_ = slot_state( self, slot_ );
    if( ag_slot_is_start_of_window( slot_ ) ) {
      ag_parent_ready_state_add_to_ready( state_, id );
      out[ *out_cnt ].slot   = slot_;
      out[ *out_cnt ].parent = *id;
      (*out_cnt)++;
    }
    if( !ag_parent_ready_state_is_skip_certified( state_ ) ) break;
  }
  return;
}

void
ag_parent_ready_tracker_mark_skipped( ag_parent_ready_tracker_t * self,
                                      ulong                       marked_slot,
                                      ag_parent_ready_t *         newly_certified,
                                      ulong *                     newly_certified_cnt ) {
  ag_parent_ready_t * out     = newly_certified;
  ulong *             out_cnt = newly_certified_cnt;
  *out_cnt = 0UL;

  if( FD_UNLIKELY( marked_slot < self->root ) ) return;

  ag_parent_ready_state_t * state = slot_state( self, marked_slot );
  if( !ag_parent_ready_state_mark_skip( state ) ) return;

  ag_block_id_t potential_parents[ PARENT_READY_PER_WINDOW ];
  ulong         potential_cnt = 0UL;

  ulong root            = self->root;
  ulong first           = ag_slot_first_slot_in_window( marked_slot );

  for( ulong s=marked_slot; ; s-- ) {
    if( s>=first && s<=marked_slot && s>=root ) {
      ag_parent_ready_state_t * sstate = slot_state( self, s );

      if( s!=marked_slot ) {
        ulong             nf_cnt;
        fd_hash_t const * nfs = ag_parent_ready_state_notar_fallback_blocks( sstate, &nf_cnt );
        for( ulong i=0UL; i<nf_cnt; i++ ) {
          FD_TEST( potential_cnt < PARENT_READY_PER_WINDOW );
          potential_parents[ potential_cnt ].slot = s;
          potential_parents[ potential_cnt ].hash = nfs[i];
          potential_cnt++;
        }
      }

      if( !ag_parent_ready_state_is_skip_certified( sstate ) ) break;

      ulong                 rb_cnt;
      ag_block_id_t const * rbs = ag_parent_ready_state_ready_block_ids( sstate, &rb_cnt );
      for( ulong i=0UL; i<rb_cnt; i++ ) {
        FD_TEST( potential_cnt < PARENT_READY_PER_WINDOW );
        potential_parents[ potential_cnt ] = rbs[i];
        potential_cnt++;
      }
    }
    if( s==0UL || s<first || s<root ) break;
  }

  for( ulong s=marked_slot+1UL; ; s++ ) {
    ag_parent_ready_state_t * fstate = slot_state( self, s );
    if( ag_slot_is_start_of_window( s ) ) {
      for( ulong i=0UL; i<potential_cnt; i++ ) {
        ag_parent_ready_state_add_to_ready( fstate, &potential_parents[i] );
        FD_TEST( *out_cnt < ag_parent_ready_state_pool_max( self->states.pool ) ); /* caller sized for slot_max */
        out[ *out_cnt ].slot   = s;
        out[ *out_cnt ].parent = potential_parents[i];
        (*out_cnt)++;
      }
    }
    if( !ag_parent_ready_state_is_skip_certified( fstate ) ) break;
  }
  return;
}

static void
keep_highest( ag_parent_ready_t *       best,
              ag_parent_ready_t const * ready,
              ulong                     ready_cnt ) {
  for( ulong i=0UL; i<ready_cnt; i++ ) {
    if( best->slot==ULONG_MAX || ready[i].slot>=best->slot ) *best = ready[i];
  }
}

ag_parent_ready_t
ag_parent_ready_tracker_handle_finalization( ag_parent_ready_tracker_t *     self,
                                             ag_finalization_event_t const * event,
                                             ag_parent_ready_t *             newly_certified,
                                             ulong *                         newly_certified_cnt ) {
  ag_parent_ready_t best;
  fd_memset( &best, 0, sizeof(ag_parent_ready_t) );
  best.slot = ULONG_MAX;

  if( event->finalized.slot!=ULONG_MAX ) {
    ag_parent_ready_tracker_mark_notar_fallback( self, &event->finalized, newly_certified, newly_certified_cnt );
    keep_highest( &best, newly_certified, *newly_certified_cnt );
  }

  for( ulong j=0UL; j<event->implicitly_finalized_cnt; j++ ) {
    ag_parent_ready_tracker_mark_notar_fallback( self, &event->implicitly_finalized[j], newly_certified, newly_certified_cnt );
    keep_highest( &best, newly_certified, *newly_certified_cnt );
  }

  for( ulong j=0UL; j<event->implicitly_skipped_cnt; j++ ) {
    ag_parent_ready_tracker_mark_skipped( self, event->implicitly_skipped[j], newly_certified, newly_certified_cnt );
    keep_highest( &best, newly_certified, *newly_certified_cnt );
  }

  return best;
}

ag_block_id_t const *
ag_parent_ready_tracker_parents_ready( ag_parent_ready_tracker_t * self,
                                       ulong                       slot,
                                       ulong *                     cnt ) {
  ag_parent_ready_state_t * state = ag_parent_ready_state_map_ele_query( self->states.map, &slot, NULL, self->states.pool );
  if( FD_UNLIKELY( !state ) ) { *cnt = 0UL; return NULL; }
  return ag_parent_ready_state_ready_block_ids( state, cnt );
}

ag_block_id_t
ag_parent_ready_tracker_wait_for_parent_ready( ag_parent_ready_tracker_t * self,
                                               ulong                       slot ) {
  return ag_parent_ready_state_wait_for_parent_ready( slot_state( self, slot ) );
}

void
ag_parent_ready_tracker_prune( ag_parent_ready_tracker_t * self,
                               ulong                       new_root ) {
  ag_parent_ready_state_map_t * map  = self->states.map;
  ag_parent_ready_state_t *     pool = self->states.pool;
  for( ulong slot=self->root; slot<new_root; slot++ ) {
    ag_parent_ready_state_t * ele = ag_parent_ready_state_map_ele_remove( map, &slot, NULL, pool );
    if( FD_LIKELY( ele ) ) ag_parent_ready_state_pool_ele_release( pool, ele );
  }
  self->root = new_root;
}
