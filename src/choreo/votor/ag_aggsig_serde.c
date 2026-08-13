#include "ag_aggsig_serde.h"

ulong
ag_aggsig_serialize( ag_aggsig_t const * agg,
                     uchar *             out,
                     ulong               out_max ) {
  ulong num_words = AG_AGGSIG_WORDS_FOR_BITS( agg->nbits );
  ulong sz        = AG_AGGSIG_SERIALIZED_SZ( agg->nbits );
  if( FD_UNLIKELY( out_max<sz ) ) return 0UL;

  ulong o = 0UL;
  fd_memcpy( out+o, agg->sig, AG_AGGSIG_SIG_SZ ); o += AG_AGGSIG_SIG_SZ;
  FD_STORE( ulong, out+o, agg->nbits  ); o += 8UL;
  FD_STORE( ulong, out+o, num_words   ); o += 8UL;
  for( ulong w=0UL; w<num_words; w++ ) {
    FD_STORE( ulong, out+o, (ulong)agg->bitmask[w] ); o += 8UL;
  }
  FD_TEST( o==sz );
  return sz;
}

ulong
ag_aggsig_deserialize( ag_aggsig_t * agg,
                       uchar const * in,
                       ulong         in_sz ) {
  if( FD_UNLIKELY( in_sz < AG_AGGSIG_SIG_SZ + 16UL ) ) return 0UL;

  ulong o = 0UL;
  fd_memcpy( agg->sig, in+o, AG_AGGSIG_SIG_SZ ); o += AG_AGGSIG_SIG_SZ;
  ulong num_bits  = FD_LOAD( ulong, in+o ); o += 8UL;
  ulong num_words = FD_LOAD( ulong, in+o ); o += 8UL;

  if( FD_UNLIKELY( num_words > AG_AGGSIG_WORDS_FOR_BITS( AG_AGGSIG_MAX_SIGNERS ) ) ) return 0UL;
  if( FD_UNLIKELY( num_bits  > num_words*64UL                                   ) ) return 0UL;
  if( FD_UNLIKELY( in_sz < o + num_words*8UL                                    ) ) return 0UL;

  voter_set_null( agg->bitmask );
  agg->nbits = num_bits;
  for( ulong w=0UL; w<num_words; w++ ) {
    agg->bitmask[w] = (voter_set_t)FD_LOAD( ulong, in+o ); o += 8UL;
  }
  return o;
}
