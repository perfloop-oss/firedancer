#ifndef HEADER_fd_src_choreo_votor_ag_cert_serde_h
#define HEADER_fd_src_choreo_votor_ag_cert_serde_h

#include "ag_cert.h"

#define AG_CERT_DE_SUCCESS         ( 0)
#define AG_CERT_DE_ERR_TRUNCATED   (-1)
#define AG_CERT_DE_ERR_MALFORMED   (-2)
#define AG_CERT_DE_ERR_UNSUPPORTED (-3)

/* A cert's wire form, as carried in a V1 consensus message:

     version (1) | kind (1) | slot (8) [| block_id (32)]
       | sig (AG_AGGSIG_SIG_SZ) | bitmap len (u64 LE) | bitmap
       | shred_version (u16 LE)

   The bitmap is base-2 when only one aggregate is carried, base-3 when a
   cert carries a fallback aggregate alongside its base one.  Both forms
   lead with a version byte and a u16 signer count. */

#define AG_CERT_SERIALIZED_MAX (2UL + 8UL + sizeof(fd_hash_t) + AG_AGGSIG_SIG_SZ + 8UL + 3UL + \
                                (AG_AGGSIG_MAX_SIGNERS+4UL)/5UL + 2UL)

FD_PROTOTYPES_BEGIN

ulong
ag_cert_serialize( ag_cert_t const * self,
                   uchar *           out,
                   ulong             out_max,
                   ushort            shred_version );

/* ag_cert_de deserializes a network cert body, everything the wire form
   above carries after the version and kind bytes (wire kind tags 7-12,
   mapped to AG_CERT_TYPE_* by the caller). */

int
ag_cert_de( ag_cert_t *   out,
            uint          kind,
            uchar const * in,
            ulong         in_sz,
            ulong *       opt_consumed );

/* ag_block_final_cert_de deserializes a finalization cert as carried in
   a block footer, which is its own form rather than the one above:

     slot (8) | block_id (32) | final_aggregate | has_notar (1)
       | [notar_aggregate]

   The aggregates arrive compressed; ag_block_final_cert_decompress
   expands the signatures in place, and must run before the certs are
   verified. */

int
ag_block_final_cert_de( ag_cert_t     out[ 2 ],
                        ulong *       out_cert_cnt,
                        uchar const * in,
                        ulong         in_sz );

int
ag_block_final_cert_decompress( ag_cert_t * certs,
                                ulong       cert_cnt );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_choreo_votor_ag_cert_serde_h */
