#include "ag_parent_ready_state.h"

void
ag_parent_ready_state_init( ag_parent_ready_state_t * state,
                            ulong                     slot ) {
  state->slot                = slot;
  state->skip                = 0;
  state->notar_fallbacks_cnt = (uchar)0;
  state->is_ready            = 0;
  state->ready_id_cnt        = 0UL;
}

void
ag_parent_ready_state_genesis( ag_parent_ready_state_t * state,
                               ulong                     slot ) {
  ag_parent_ready_state_init( state, slot );

  fd_memset( &state->notar_fallbacks[0], 0, sizeof(fd_hash_t) );
  state->notar_fallbacks_cnt = (uchar)1;
}

int
ag_parent_ready_state_mark_skip( ag_parent_ready_state_t * self ) {
  if( FD_UNLIKELY( self->skip ) ) return 0;
  self->skip = 1;
  return 1;
}

int
ag_parent_ready_state_is_skip_certified( ag_parent_ready_state_t const * self ) {
  return self->skip;
}

int
ag_parent_ready_state_mark_notar_fallback( ag_parent_ready_state_t * self,
                                           fd_hash_t const *         hash ) {
  for( ulong i=0; i<self->notar_fallbacks_cnt; i++ ) {
    if( FD_UNLIKELY( 0==memcmp( &self->notar_fallbacks[i], hash, sizeof(fd_hash_t) ) ) ) return 0;
  }
  self->notar_fallbacks[ self->notar_fallbacks_cnt++ ] = *hash;
  return 1;
}

fd_hash_t const *
ag_parent_ready_state_notar_fallback_blocks( ag_parent_ready_state_t const * self,
                                             ulong *                         cnt ) {
  *cnt = (ulong)self->notar_fallbacks_cnt;
  return self->notar_fallbacks;
}

void
ag_parent_ready_state_add_to_ready( ag_parent_ready_state_t * self,
                                    ag_block_id_t const *     id ) {
  if( FD_LIKELY( !self->is_ready ) ) {
    self->ready_ids[ 0 ] = *id;
    self->ready_id_cnt   = 1UL;
    self->is_ready       = 1;
  } else {
    self->ready_ids[ self->ready_id_cnt++ ] = *id;
  }
}

ag_block_id_t const *
ag_parent_ready_state_ready_block_ids( ag_parent_ready_state_t const * self,
                                       ulong *                         cnt ) {
  *cnt = self->ready_id_cnt;
  return self->ready_ids;
}

ag_block_id_t
ag_parent_ready_state_wait_for_parent_ready( ag_parent_ready_state_t * self ) {
  if( FD_UNLIKELY( self->is_ready ) ) {
    ag_block_id_t arg_min = self->ready_ids[0];
    for( ulong i=1; i<self->ready_id_cnt; i++ ) {
      ag_block_id_t const * id = &self->ready_ids[i];
      if( FD_UNLIKELY( id->slot<arg_min.slot ) || ( id->slot==arg_min.slot && 0<memcmp( id->hash.uc, arg_min.hash.uc, sizeof(fd_hash_t) ) ) ) {
        arg_min = *id;
      }
    }
    return arg_min;
  } else {
    return (ag_block_id_t){ .slot = ULONG_MAX };
  }
}
