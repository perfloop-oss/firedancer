#include "ag_epoch_info.h"

void
ag_epoch_info_init( ag_epoch_info_t *           self,
                    ag_validator_info_t const * validators,
                    ulong                       validator_cnt ) {
  FD_TEST( self );
  FD_TEST( validator_cnt>0UL && validator_cnt<=AG_VAT_MAX );

  ulong total = 0UL;
  for( ulong i=0UL; i<validator_cnt; i++ ) {
    FD_TEST( validators[i].id==i );
    self->validators[i] = validators[i];
    total              += validators[i].stake;
  }
  self->validator_cnt = validator_cnt;
  self->total_stake   = total;
}

/* The quorum comparison is cross multiplied in uint128 so it neither
   rounds nor overflows. */

FD_FN_CONST static int
fraction_is_met( ulong stake,
                 ulong total,
                 ulong numer,
                 ulong denom ) {
  return (uint128)stake*(uint128)denom >= (uint128)total*(uint128)numer;
}

FD_FN_PURE int
ag_epoch_info_is_weakest_quorum( ag_epoch_info_t const * self, ulong stake ) {
  return fraction_is_met( stake, self->total_stake, AG_WEAKEST_QUORUM_THRESHOLD_NUMER, AG_QUORUM_THRESHOLD_DENOM );
}

FD_FN_PURE int
ag_epoch_info_is_weak_quorum( ag_epoch_info_t const * self, ulong stake ) {
  return fraction_is_met( stake, self->total_stake, AG_WEAK_QUORUM_THRESHOLD_NUMER, AG_QUORUM_THRESHOLD_DENOM );
}

FD_FN_PURE int
ag_epoch_info_is_quorum( ag_epoch_info_t const * self, ulong stake ) {
  return fraction_is_met( stake, self->total_stake, AG_QUORUM_THRESHOLD_NUMER, AG_QUORUM_THRESHOLD_DENOM );
}

FD_FN_PURE int
ag_epoch_info_is_strong_quorum( ag_epoch_info_t const * self, ulong stake ) {
  return fraction_is_met( stake, self->total_stake, AG_STRONG_QUORUM_THRESHOLD_NUMER, AG_QUORUM_THRESHOLD_DENOM );
}
