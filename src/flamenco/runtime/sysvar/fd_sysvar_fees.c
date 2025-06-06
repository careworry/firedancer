#include "fd_sysvar_fees.h"
#include "fd_sysvar.h"
#include "../fd_system_ids.h"
#include "../fd_runtime.h"
#include "../context/fd_exec_epoch_ctx.h"

static void
write_fees( fd_exec_slot_ctx_t* slot_ctx, fd_sysvar_fees_t* fees ) {
  ulong sz = fd_sysvar_fees_size( fees );
  uchar enc[sz];
  fd_memset( enc, 0, sz );
  fd_bincode_encode_ctx_t ctx;
  ctx.data = enc;
  ctx.dataend = enc + sz;
  if ( fd_sysvar_fees_encode( fees, &ctx ) )
    FD_LOG_ERR(("fd_sysvar_fees_encode failed"));

  fd_sysvar_set( slot_ctx, &fd_sysvar_owner_id, &fd_sysvar_fees_id, enc, sz, slot_ctx->slot_bank.slot );
}

fd_sysvar_fees_t *
fd_sysvar_fees_read( fd_sysvar_fees_t *        result,
                     fd_sysvar_cache_t const * sysvar_cache,
                     fd_funk_t *               funk,
                     fd_funk_txn_t *           funk_txn ) {
  fd_sysvar_fees_t const * ret = fd_sysvar_cache_fees( sysvar_cache );
  if( FD_UNLIKELY( NULL != ret ) ) {
    fd_memcpy(result, ret, sizeof(fd_sysvar_fees_t));
    return result;
  }

  FD_TXN_ACCOUNT_DECL( acc );
  int err = fd_txn_account_init_from_funk_readonly( acc, &fd_sysvar_fees_id, funk, funk_txn );
  if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) )
    return NULL;

  fd_bincode_decode_ctx_t decode = {
    .data    = acc->vt->get_data( acc ),
    .dataend = acc->vt->get_data( acc ) + acc->vt->get_data_len( acc )
  };

  ulong total_sz = 0UL;
  err = fd_sysvar_fees_decode_footprint( &decode, &total_sz );
  if( FD_UNLIKELY( err!=FD_BINCODE_SUCCESS ) ) {
    return NULL;
  }

  fd_sysvar_fees_decode( result, &decode );
  return result;
}

/*
https://github.com/firedancer-io/solana/blob/dab3da8e7b667d7527565bddbdbecf7ec1fb868e/sdk/program/src/fee_calculator.rs#L105-L165
*/
void
fd_sysvar_fees_new_derived(
  fd_exec_slot_ctx_t * slot_ctx,
  fd_fee_rate_governor_t base_fee_rate_governor,
  ulong latest_singatures_per_slot
) {
  fd_fee_rate_governor_t me = {
    .target_signatures_per_slot = base_fee_rate_governor.target_signatures_per_slot,
    .target_lamports_per_signature = base_fee_rate_governor.target_lamports_per_signature,
    .max_lamports_per_signature = base_fee_rate_governor.max_lamports_per_signature,
    .min_lamports_per_signature = base_fee_rate_governor.min_lamports_per_signature,
    .burn_percent = base_fee_rate_governor.burn_percent
  };

  ulong lamports_per_signature = 0;
  if ( me.target_signatures_per_slot > 0 ) {
    me.min_lamports_per_signature = fd_ulong_max( 1UL, (ulong)(me.target_lamports_per_signature / 2) );
    me.max_lamports_per_signature = me.target_lamports_per_signature * 10;
    ulong desired_lamports_per_signature = fd_ulong_min(
      me.max_lamports_per_signature,
      fd_ulong_max(
        me.min_lamports_per_signature,
        me.target_lamports_per_signature
        * fd_ulong_min(latest_singatures_per_slot, (ulong)UINT_MAX)
        / me.target_signatures_per_slot
      )
    );
    long gap = (long)desired_lamports_per_signature - (long)slot_ctx->slot_bank.lamports_per_signature;
    if ( gap == 0 ) {
      lamports_per_signature = desired_lamports_per_signature;
    } else {
      long gap_adjust = (long)(fd_ulong_max( 1UL, (ulong)(me.target_lamports_per_signature / 20) ))
        * (gap != 0)
        * (gap > 0 ? 1 : -1);
      lamports_per_signature = fd_ulong_min(
        me.max_lamports_per_signature,
        fd_ulong_max(
          me.min_lamports_per_signature,
          (ulong)((long) slot_ctx->slot_bank.lamports_per_signature + gap_adjust)
        )
      );
    }
  } else {
    lamports_per_signature = base_fee_rate_governor.target_lamports_per_signature;
    me.min_lamports_per_signature = me.target_lamports_per_signature;
    me.max_lamports_per_signature = me.target_lamports_per_signature;
  }

  if( FD_UNLIKELY( slot_ctx->slot_bank.lamports_per_signature==0UL ) ) {
    slot_ctx->prev_lamports_per_signature = lamports_per_signature;
  } else {
    slot_ctx->prev_lamports_per_signature = slot_ctx->slot_bank.lamports_per_signature;
  }

  slot_ctx->slot_bank.lamports_per_signature = lamports_per_signature;
  fd_memcpy(&slot_ctx->slot_bank.fee_rate_governor, &me, sizeof(fd_fee_rate_governor_t));
}

void
fd_sysvar_fees_update( fd_exec_slot_ctx_t * slot_ctx ) {
  if( FD_FEATURE_ACTIVE( slot_ctx->slot_bank.slot, slot_ctx->epoch_ctx->features, disable_fees_sysvar ))
    return;
  fd_sysvar_fees_t fees;
  fd_sysvar_fees_read( &fees,
                       slot_ctx->sysvar_cache,
                       slot_ctx->funk,
                       slot_ctx->funk_txn );
  /* todo: I need to the lamports_per_signature field */
  fees.fee_calculator.lamports_per_signature = slot_ctx->slot_bank.lamports_per_signature;
  write_fees( slot_ctx, &fees );
}

void fd_sysvar_fees_init( fd_exec_slot_ctx_t * slot_ctx ) {
  /* Default taken from https://github.com/solana-labs/solana/blob/8f2c8b8388a495d2728909e30460aa40dcc5d733/sdk/program/src/fee_calculator.rs#L110 */
  /* TODO: handle non-default case */
  fd_sysvar_fees_t fees = {
    {
      .lamports_per_signature = 0,
    }
  };
  write_fees( slot_ctx, &fees );
}
