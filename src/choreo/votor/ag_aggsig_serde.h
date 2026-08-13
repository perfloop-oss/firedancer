#ifndef HEADER_fd_src_choreo_votor_ag_aggsig_serde_h
#define HEADER_fd_src_choreo_votor_ag_aggsig_serde_h

#include "ag_aggsig.h"

/* The aggregate's wire form:

     sig (AG_AGGSIG_SIG_SZ) | nbits (u64 LE) | nwords (u64 LE) | bitmask

   AG_AGGSIG_SERIALIZED_SZ / _MAX in ag_aggsig.h size a buffer for it. */

FD_PROTOTYPES_BEGIN

ulong
ag_aggsig_serialize( ag_aggsig_t const * agg,
                     uchar *             out,
                     ulong               out_max );

ulong
ag_aggsig_deserialize( ag_aggsig_t * agg,
                       uchar const * in,
                       ulong         in_sz );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_choreo_votor_ag_aggsig_serde_h */
