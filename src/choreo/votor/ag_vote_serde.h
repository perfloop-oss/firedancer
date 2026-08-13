#ifndef HEADER_fd_src_choreo_votor_ag_vote_serde_h
#define HEADER_fd_src_choreo_votor_ag_vote_serde_h

#include "ag_vote.h"

/* A vote's wire form, as carried in a V1 consensus message:

     version (1) | kind (1) | body | shred_version (u16 LE)

   where kind is AG_VOTE_TYPE_* + 1 and body is the vote struct itself --
   ag_notar_vote_t for the two that carry a block hash, ag_final_vote_t
   for the three that do not. */

FD_PROTOTYPES_BEGIN

ulong
ag_vote_serialize( ag_vote_t const * self,
                   uchar *           out,
                   ulong             out_max,
                   ushort            shred_version );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_choreo_votor_ag_vote_serde_h */
