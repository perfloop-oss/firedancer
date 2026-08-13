#include "ag_cert_serde.h"

#include "../../ballet/bls/fd_bls12_381.h"

#define READ_U8( dst, data, sz ) do {                               \
  if( FD_UNLIKELY( 1UL>(*(sz)) ) ) return AG_CERT_DE_ERR_TRUNCATED; \
  (dst) = FD_LOAD( uchar, *(data) );                                \
  *(data) += 1UL;                                                   \
  *(sz)   -= 1UL;                                                   \
} while( 0 )

#define READ_U16( dst, data, sz ) do {                              \
  if( FD_UNLIKELY( 2UL>(*(sz)) ) ) return AG_CERT_DE_ERR_TRUNCATED; \
  (dst) = FD_LOAD( ushort, *(data) );                               \
  *(data) += 2UL;                                                   \
  *(sz)   -= 2UL;                                                   \
} while( 0 )

#define READ_U32( dst, data, sz ) do {                              \
  if( FD_UNLIKELY( 4UL>(*(sz)) ) ) return AG_CERT_DE_ERR_TRUNCATED; \
  (dst) = FD_LOAD( uint, *(data) );                                 \
  *(data) += 4UL;                                                   \
  *(sz)   -= 4UL;                                                   \
} while( 0 )

#define READ_U64( dst, data, sz ) do {                              \
  if( FD_UNLIKELY( 8UL>(*(sz)) ) ) return AG_CERT_DE_ERR_TRUNCATED; \
  (dst) = FD_LOAD( ulong, *(data) );                                \
  *(data) += 8UL;                                                   \
  *(sz)   -= 8UL;                                                   \
} while( 0 )

#define READ_HASH( dst, data, sz ) do {                                           \
  if( FD_UNLIKELY( sizeof(fd_hash_t)>(*(sz)) ) ) return AG_CERT_DE_ERR_TRUNCATED; \
  fd_memcpy( (dst).uc, *(data), sizeof(fd_hash_t) );                              \
  *(data) += sizeof(fd_hash_t);                                                   \
  *(sz)   -= sizeof(fd_hash_t);                                                   \
} while( 0 )

#define SKIP_BYTES( n, data, sz ) do {                              \
  if( FD_UNLIKELY( (n)>(*(sz)) ) ) return AG_CERT_DE_ERR_TRUNCATED; \
  *(data) += (n);                                                   \
  *(sz)   -= (n);                                                   \
} while( 0 )

#define BASE2_BITMAP (0)
#define BASE3_BITMAP (1)

static int
de_base2_bitmap( ag_aggsig_t * agg,
                 uchar const * b,
                 ulong         b_sz ) {
  uchar  version;
  ushort nbits;
  READ_U8 ( version, &b, &b_sz );
  READ_U16( nbits,   &b, &b_sz );
  if( FD_UNLIKELY( version!=BASE2_BITMAP              ) ) return AG_CERT_DE_ERR_MALFORMED;
  if( FD_UNLIKELY( (ulong)nbits>AG_AGGSIG_MAX_SIGNERS ) ) return AG_CERT_DE_ERR_MALFORMED;
  if( FD_UNLIKELY( b_sz!=((ulong)nbits+7UL)/8UL       ) ) return AG_CERT_DE_ERR_MALFORMED;

  ag_aggsig_init( agg, (ulong)nbits );

  for( ulong i=0UL; i<(ulong)nbits; i++ ) {
    if( (b[ i>>3 ] >> (i&7U)) & 1U ) voter_set_insert( agg->bitmask, i );
  }
  return AG_CERT_DE_SUCCESS;
}

static int
de_base3_bitmap( ag_aggsig_t * base,
                 ag_aggsig_t * fb,
                 uchar const * b,
                 ulong         b_sz ) {
  uchar  version;
  ushort nbits;
  READ_U8 ( version, &b, &b_sz );
  READ_U16( nbits,   &b, &b_sz );
  if( FD_UNLIKELY( version!=BASE3_BITMAP              ) ) return AG_CERT_DE_ERR_MALFORMED;
  if( FD_UNLIKELY( (ulong)nbits>AG_AGGSIG_MAX_SIGNERS ) ) return AG_CERT_DE_ERR_MALFORMED;
  ulong nchunks = ((ulong)nbits+4UL)/5UL;
  if( FD_UNLIKELY( b_sz!=nchunks                      ) ) return AG_CERT_DE_ERR_MALFORMED;

  ag_aggsig_init( base, (ulong)nbits );
  ag_aggsig_init( fb,   (ulong)nbits );

  for( ulong chunk=0UL; chunk<nchunks; chunk++ ) {
    uint  block = (uint)b[ chunk ];
    ulong start = chunk*5UL;
    ulong end   = fd_ulong_min( start+5UL, (ulong)nbits );
    for( ulong i=start; i<end; i++ ) {
      uint digit = block % 3U; block /= 3U;
      if(      digit==1U ) voter_set_insert( base->bitmask, i );
      else if( digit==2U ) voter_set_insert( fb->bitmask,   i );
    }
  }
  return AG_CERT_DE_SUCCESS;
}

int
ag_cert_de( ag_cert_t *   out,
            uint          tag,
            uchar const * in,
            ulong         in_sz,
            ulong *       opt_consumed ) {
  ulong remaining = in_sz;

  if( tag==AG_CERT_TYPE_GENESIS ) return AG_CERT_DE_ERR_UNSUPPORTED;

  ulong     slot = 0UL;
  fd_hash_t block_hash;
  fd_memset( &block_hash, 0, sizeof(fd_hash_t) );

  switch( tag ) {
  case AG_CERT_TYPE_FINAL:
  case AG_CERT_TYPE_SKIP:
    READ_U64( slot, &in, &remaining );
    break;
  case AG_CERT_TYPE_FAST_FINAL:
  case AG_CERT_TYPE_NOTAR:
  case AG_CERT_TYPE_NOTAR_FALLBACK:
    READ_U64( slot, &in, &remaining );
    READ_HASH( block_hash, &in, &remaining );
    break;
  default:
    return AG_CERT_DE_ERR_MALFORMED;
  }

  uchar const * sig = in;
  SKIP_BYTES( AG_AGGSIG_SIG_SZ, &in, &remaining );

  ulong bm_len;
  READ_U64( bm_len, &in, &remaining );
  uchar const * bm = in;
  SKIP_BYTES( bm_len, &in, &remaining );

  fd_memset( out, 0, sizeof(ag_cert_t) );
  int err;
  switch( tag ) {
  case AG_CERT_TYPE_FINAL:
    out->kind             = AG_CERT_TYPE_FINAL;
    out->inner.final.slot = slot;
    if( FD_UNLIKELY( err = de_base2_bitmap(  &out->inner.final.agg_sig, bm, bm_len ) ) ) return err;
    fd_memcpy( out->inner.final.agg_sig.sig, sig, AG_AGGSIG_SIG_SZ );
    break;
  case AG_CERT_TYPE_FAST_FINAL:
    out->kind                        = AG_CERT_TYPE_FAST_FINAL;
    out->inner.fast_final.slot       = slot;
    out->inner.fast_final.block_hash = block_hash;
    if( FD_UNLIKELY( err = de_base2_bitmap(  &out->inner.fast_final.agg_sig, bm, bm_len ) ) ) return err;
    fd_memcpy( out->inner.fast_final.agg_sig.sig, sig, AG_AGGSIG_SIG_SZ );
    break;
  case AG_CERT_TYPE_NOTAR:
    out->kind                   = AG_CERT_TYPE_NOTAR;
    out->inner.notar.slot       = slot;
    out->inner.notar.block_hash = block_hash;
    if( FD_UNLIKELY( err = de_base2_bitmap(  &out->inner.notar.agg_sig, bm, bm_len ) ) ) return err;
    fd_memcpy( out->inner.notar.agg_sig.sig, sig, AG_AGGSIG_SIG_SZ );
    break;
  case AG_CERT_TYPE_NOTAR_FALLBACK: {
    ag_aggsig_t * b = &out->inner.notar_fallback.agg_sig_notar;
    ag_aggsig_t * f = &out->inner.notar_fallback.agg_sig_notar_fallback;
    out->kind                            = AG_CERT_TYPE_NOTAR_FALLBACK;
    out->inner.notar_fallback.slot       = slot;
    out->inner.notar_fallback.block_hash = block_hash;
    if( FD_UNLIKELY( bm_len<1UL ) ) return AG_CERT_DE_ERR_TRUNCATED;
    if( bm[0]==BASE2_BITMAP ) { if( FD_UNLIKELY( err = de_base2_bitmap( b,    bm, bm_len ) ) ) return err; ag_aggsig_init( f, b->nbits ); }
    else                      { if( FD_UNLIKELY( err = de_base3_bitmap( b, f, bm, bm_len ) ) ) return err; }
    fd_memcpy( b->sig, sig, AG_AGGSIG_SIG_SZ );
    break;
  }
  case AG_CERT_TYPE_SKIP: {
    ag_aggsig_t * b = &out->inner.skip.agg_sig_skip;
    ag_aggsig_t * f = &out->inner.skip.agg_sig_skip_fallback;
    out->kind            = AG_CERT_TYPE_SKIP;
    out->inner.skip.slot = slot;
    if( FD_UNLIKELY( bm_len<1UL ) ) return AG_CERT_DE_ERR_TRUNCATED;
    if( bm[0]==BASE2_BITMAP ) { if( FD_UNLIKELY( err = de_base2_bitmap( b,    bm, bm_len ) ) ) return err; ag_aggsig_init( f, b->nbits ); }
    else                      { if( FD_UNLIKELY( err = de_base3_bitmap( b, f, bm, bm_len ) ) ) return err; }
    fd_memcpy( b->sig, sig, AG_AGGSIG_SIG_SZ );
    break;
  }
  default:
    return AG_CERT_DE_ERR_MALFORMED;
  }

  if( opt_consumed ) *opt_consumed = in_sz - remaining;

  return AG_CERT_DE_SUCCESS;
}

static int
de_footer_aggregate( ag_aggsig_t * agg,
                     uchar const * in,
                     ulong         in_sz,
                     ulong *       consumed ) {
  ulong remaining = in_sz;

  uchar const * csig = in;
  SKIP_BYTES( AG_AGGSIG_SIG_COMPRESSED_SZ, &in, &remaining );

  ushort bm_len;
  READ_U16( bm_len, &in, &remaining );

  uchar const * bm = in;
  SKIP_BYTES( bm_len, &in, &remaining );

  int err = de_base2_bitmap( agg, bm, bm_len );  if( FD_UNLIKELY( err ) ) return err;
  fd_memcpy( agg->sig, csig, AG_AGGSIG_SIG_COMPRESSED_SZ );
  *consumed = in_sz - remaining;
  return AG_CERT_DE_SUCCESS;
}

int
ag_block_final_cert_de( ag_cert_t     out[ 2 ],
                        ulong *       out_cert_cnt,
                        uchar const * in,
                        ulong         in_sz ) {
  ulong remaining = in_sz;
  ulong slot;  fd_hash_t block_hash;
  READ_U64( slot, &in, &remaining );
  READ_HASH( block_hash, &in, &remaining );

  ag_aggsig_t final_agg[1];
  ulong       consumed;
  int err = de_footer_aggregate( final_agg, in, remaining, &consumed );
  if( FD_UNLIKELY( err ) ) return err;
  SKIP_BYTES( consumed, &in, &remaining );

  uchar has_notar;
  READ_U8( has_notar, &in, &remaining );
  if( FD_UNLIKELY( has_notar>1 ) ) return AG_CERT_DE_ERR_MALFORMED;

  fd_memset( out, 0, 2UL*sizeof(ag_cert_t) );
  if( has_notar ) {
    ag_aggsig_t notar_agg[1];
    err = de_footer_aggregate( notar_agg, in, remaining, &consumed );
    if( FD_UNLIKELY( err ) ) return err;
    out[0].kind                   = AG_CERT_TYPE_FINAL;
    out[0].inner.final.slot       = slot;
    out[0].inner.final.agg_sig    = *final_agg;
    out[1].kind                   = AG_CERT_TYPE_NOTAR;
    out[1].inner.notar.slot       = slot;
    out[1].inner.notar.block_hash = block_hash;
    out[1].inner.notar.agg_sig    = *notar_agg;
    *out_cert_cnt = 2UL;
  } else {
    out[0].kind                        = AG_CERT_TYPE_FAST_FINAL;
    out[0].inner.fast_final.slot       = slot;
    out[0].inner.fast_final.block_hash = block_hash;
    out[0].inner.fast_final.agg_sig    = *final_agg;
    *out_cert_cnt = 1UL;
  }
  return AG_CERT_DE_SUCCESS;
}

int
ag_block_final_cert_decompress( ag_cert_t * certs,
                                ulong       cert_cnt ) {
  for( ulong i=0UL; i<cert_cnt; i++ ) {
    ag_aggsig_t * agg;
    switch( certs[ i ].kind ) {
    case AG_CERT_TYPE_FINAL:      agg = &certs[ i ].inner.final.agg_sig;      break;
    case AG_CERT_TYPE_FAST_FINAL: agg = &certs[ i ].inner.fast_final.agg_sig; break;
    case AG_CERT_TYPE_NOTAR:      agg = &certs[ i ].inner.notar.agg_sig;      break;
    default: return AG_CERT_DE_ERR_MALFORMED;
    }
    uchar csig[ AG_AGGSIG_SIG_COMPRESSED_SZ ];
    fd_memcpy( csig, agg->sig, AG_AGGSIG_SIG_COMPRESSED_SZ );
    if( FD_UNLIKELY( fd_bls12_381_g2_decompress_syscall( agg->sig, csig, 1  ) ) ) return AG_CERT_DE_ERR_MALFORMED;
  }
  return AG_CERT_DE_SUCCESS;
}

static ulong
ser_base2_bitmap( uchar *             out,
                  ag_aggsig_t const * agg ) {
  ulong nbits   = agg->nbits;
  ulong payload = (nbits+7UL)/8UL;
  ulong o       = 0UL;
  out[ o++ ] = (uchar)BASE2_BITMAP;
  FD_STORE( ushort, out+o, (ushort)nbits ); o += 2UL;
  fd_memset( out+o, 0, payload );
  for( ulong i=0UL; i<nbits; i++ ) {
    if( voter_set_test( agg->bitmask, i ) ) out[ o + (i>>3) ] |= (uchar)( 1U << (i&7U) );
  }
  return o + payload;
}

static ulong
ser_base3_bitmap( uchar *             out,
                  ag_aggsig_t const * base,
                  ag_aggsig_t const * fb ) {
  ulong nbits   = base->nbits;
  ulong nchunks = (nbits+4UL)/5UL;
  ulong o       = 0UL;
  out[ o++ ] = (uchar)BASE3_BITMAP;
  FD_STORE( ushort, out+o, (ushort)nbits ); o += 2UL;
  for( ulong chunk=0UL; chunk<nchunks; chunk++ ) {
    ulong start = chunk*5UL;
    ulong end   = fd_ulong_min( start+5UL, nbits );
    uint  block = 0U;
    uint  place = 1U;
    for( ulong i=start; i<end; i++ ) {
      uint digit = voter_set_test( base->bitmask, i ) ? 1U
                 : voter_set_test( fb->bitmask,   i ) ? 2U : 0U;
      block += digit*place;
      place *= 3U;
    }
    out[ o + chunk ] = (uchar)block;
  }
  return o + nchunks;
}

ulong
ag_cert_serialize( ag_cert_t const * self,
                   uchar *           out,
                   ulong             out_max,
                   ushort            shred_version ) {
  ag_aggsig_t const * base;
  ag_aggsig_t const * fb = NULL;
  fd_hash_t   const * hash = NULL;
  ulong               slot;
  switch( self->kind ) {
  case AG_CERT_TYPE_FINAL:
    slot = self->inner.final.slot;          base = &self->inner.final.agg_sig;
    break;
  case AG_CERT_TYPE_FAST_FINAL:
    slot = self->inner.fast_final.slot;     base = &self->inner.fast_final.agg_sig;
    hash = &self->inner.fast_final.block_hash;
    break;
  case AG_CERT_TYPE_NOTAR:
    slot = self->inner.notar.slot;          base = &self->inner.notar.agg_sig;
    hash = &self->inner.notar.block_hash;
    break;
  case AG_CERT_TYPE_NOTAR_FALLBACK:
    slot = self->inner.notar_fallback.slot; base = &self->inner.notar_fallback.agg_sig_notar;
    fb   = &self->inner.notar_fallback.agg_sig_notar_fallback;
    hash = &self->inner.notar_fallback.block_hash;
    break;
  case AG_CERT_TYPE_SKIP:
    slot = self->inner.skip.slot;           base = &self->inner.skip.agg_sig_skip;
    fb   = &self->inner.skip.agg_sig_skip_fallback;
    break;
  default:
    return 0UL;
  }

  if( fb && !ag_aggsig_signer_cnt( fb ) ) fb = NULL;

  ulong bm_sz = fb ? ( 3UL + (base->nbits+4UL)/5UL )
                   : ( 3UL + (base->nbits+7UL)/8UL );
  ulong sz    = 2UL + 8UL + ( hash ? sizeof(fd_hash_t) : 0UL ) + AG_AGGSIG_SIG_SZ + 8UL + bm_sz + 2UL;
  if( FD_UNLIKELY( out_max<sz ) ) return 0UL;

  ulong o = 0UL;
  out[ o++ ] = (uchar)1;
  out[ o++ ] = (uchar)( self->kind+7U );
  FD_STORE( ulong, out+o, slot ); o += 8UL;
  if( hash ) { fd_memcpy( out+o, hash->uc, sizeof(fd_hash_t) ); o += sizeof(fd_hash_t); }
  fd_memcpy( out+o, base->sig, AG_AGGSIG_SIG_SZ ); o += AG_AGGSIG_SIG_SZ;
  FD_STORE( ulong, out+o, bm_sz ); o += 8UL;
  o += fb ? ser_base3_bitmap( out+o, base, fb ) : ser_base2_bitmap( out+o, base );
  FD_STORE( ushort, out+o, shred_version ); o += 2UL;

  FD_TEST( o==sz );
  return sz;
}
