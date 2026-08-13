#include "ag_vote_serde.h"

ulong
ag_vote_serialize( ag_vote_t const * self,
                   uchar *           out,
                   ulong             out_max,
                   ushort            shred_version ) {
  ulong body_sz = ( self->kind==AG_VOTE_TYPE_NOTAR || self->kind==AG_VOTE_TYPE_NOTAR_FALLBACK )
                  ? sizeof(ag_notar_vote_t) : sizeof(ag_final_vote_t);
  ulong sz      = 2UL + body_sz + 2UL;
  if( FD_UNLIKELY( out_max<sz ) ) return 0UL;

  out[0] = (uchar)1;
  out[1] = (uchar)( self->kind+1U );
  fd_memcpy( out+2UL, &self->inner, body_sz );
  FD_STORE( ushort, out+2UL+body_sz, shred_version );
  return sz;
}
