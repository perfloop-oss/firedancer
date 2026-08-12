/* test_gui_hist_evict exercises the space-pressure epoch-cascade eviction in
   fd_gui_hist: fd_gui_hist_evict_oldest (the synchronous drain used by the
   map-full fallback and driven one batch at a time by
   fd_gui_hist_evict_step).  It builds multiple epochs' worth of records -- the
   EPOCH records, the per-slot (slot,bank_seq) entity rows, and the
   time-bucketed time-series rows -- then evicts the oldest epoch and asserts
   that exactly that epoch's rows are gone while the newer epochs survive,
   including the SHRED_EVENTS boundary case (a slot of the NEXT epoch whose
   shred landed in a wallclock second shared with the oldest epoch's tail).

   The eviction path only touches gui->db / gui->hist, so the test allocates a
   bare fd_gui_t (like test_gui_consensus) and wires up the two store layers
   by hand -- no http server / topology / fd_gui_new. */

#include "../../util/fd_util.h"
#include "fd_gui.h"
#include "fd_gui_printf.h"
#include "fd_gui_store.h"
#include "fd_gui_hist.h"
#include "../shred/fd_shred_tile.h"
#include "../../waltz/http/fd_http_server_private.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EPOCH_A      (10UL)
#define EPOCH_B      (11UL)
#define EPOCH_C      (12UL)
#define A_START_SLOT (1000UL)
#define B_START_SLOT (1010UL)
#define C_START_SLOT (1020UL)
#define SLOT_CNT     (10UL)
#define A_END_SLOT   (A_START_SLOT+SLOT_CNT-1UL) /* 1009 */
#define B_END_SLOT   (B_START_SLOT+SLOT_CNT-1UL) /* 1019 */
#define C_END_SLOT   (C_START_SLOT+SLOT_CNT-1UL) /* 1029 */
#define BANK_SEQ     (0UL)

/* slot -> completion wallclock ns.  slot 1000 -> 10s, 1001 -> 11s, ... so the
   window (floored second) equals (slot-990). */
static long
slot_complete_ns( ulong slot ) {
  ulong sec = slot - 990UL; /* 1000->10s ... 1019->29s */
  return (long)( sec * 1000000000UL );
}
static long
sec_ns( ulong sec ) { return (long)( sec*1000000000UL ); }

/* Put epoch A immediately before a UTC-day boundary and epochs B/C after it.
   This lets the cascade test verify that timeline-day eviction retains the
   day shared by the first surviving epoch while reclaiming older days. */
static long
epoch_slot_complete_ns( ulong slot ) {
  return sec_ns( 86390UL+(slot-A_START_SLOT) );
}

static void
rm_tmpdir( char const * path ) {
  char cmd[ 256 ];
  /* MDB_NOSUBDIR: data file at `path`, lock file at `path-lock`. */
  fd_cstr_printf_check( cmd, sizeof(cmd), NULL, "rm -rf %s %s-lock", path, path );
  if( FD_UNLIKELY( system( cmd ) ) ) FD_LOG_WARNING(( "failed to clean up %s", path ));
}

/* ---- write helpers ---------------------------------------------------- */

static void
put_epoch( fd_gui_t * gui, ulong epoch, ulong start_slot, ulong slot_cnt ) {
  fd_gui_hist_epoch_key_t key[ 1 ];
  key->epoch = epoch;

  fd_gui_epoch_t * rec = fd_gui_hist_kv_get_or_create( gui, FD_GUI_HIST_EPOCH, key );
  FD_TEST( rec );
  memset( rec, 0, sizeof(fd_gui_epoch_t) );
  rec->epoch      = epoch;
  rec->start_slot = start_slot;
  rec->slot_cnt   = slot_cnt;

  gui->epoch.stored_epoch_cnt++; /* mirror fd_gui_handle_epoch_info; the >= FD_GUI_HIST_MIN_EPOCHS guard reads this */
}

static void
put_slot( fd_gui_t * gui, ulong slot, long completed_time ) {
  fd_gui_hist_slot_key_t key[ 1 ];
  key->slot = slot; key->bank_seq = BANK_SEQ;

  fd_gui_slot_t * rec = fd_gui_hist_kv_get_or_create( gui, FD_GUI_HIST_SLOT, key );
  FD_TEST( rec );
  memset( rec, 0, sizeof(*rec) );
  rec->slot           = slot;
  rec->bank_seq       = BANK_SEQ;
  rec->completed_time = completed_time;
}

static void
put_leader_slot( fd_gui_t * gui, ulong slot, long start_time ) {
  fd_gui_hist_leader_slot_key_t key[ 1 ];
  key->slot = slot; key->bank_seq = BANK_SEQ;

  fd_gui_leader_slot_t * rec = fd_gui_hist_kv_get_or_create( gui, FD_GUI_HIST_LEADER_SLOT, key );
  FD_TEST( rec );
  memset( rec, 0, sizeof(*rec) );
  rec->slot              = slot;
  rec->bank_seq          = BANK_SEQ;
  rec->leader_start_time = start_time;
}

/* put_leader_slot_seq writes a leader-slot-meta row at (slot,bank_seq).
   Used by the trigger test to pad the store with many distinct, committed
   keys (so used-bytes grows immediately, no flush needed) that still belong
   to an evictable epoch's slot.  (The trigger test batches these inline for
   speed; this single-row form documents the shape.) */
FD_FN_UNUSED static void
put_leader_slot_seq( fd_gui_t * gui, ulong slot, ulong bank_seq ) {
  fd_gui_hist_leader_slot_key_t key[ 1 ];
  key->slot = slot; key->bank_seq = bank_seq;

  fd_gui_leader_slot_t * rec = fd_gui_hist_kv_get_or_create( gui, FD_GUI_HIST_LEADER_SLOT, key );
  FD_TEST( rec );
  memset( rec, 0, sizeof(*rec) );
  rec->slot     = slot;
  rec->bank_seq = bank_seq;
}

/* SCHEDULER_COUNTS is window-only (no slot). */
static void
append_sched_counts( fd_gui_t * gui, long ts_ns ) {
  fd_gui_scheduler_counts_t rec[ 1 ];
  memset( rec, 0, sizeof(*rec) );
  rec->sample_time_ns = ts_ns;

  FD_TEST( !fd_gui_hist_ts_append( gui, FD_GUI_HIST_SCHEDULER_COUNTS, ts_ns, ts_ns, rec ) );
}

/* SHRED_EVENTS carries its slot in the record. */
static void
append_shred( fd_gui_t * gui, long ts_ns, ulong slot ) {
  fd_gui_slot_history_shred_event_t rec[ 1 ];
  memset( rec, 0, sizeof(*rec) );
  rec->slot      = (uint)slot;
  rec->timestamp = ts_ns;

  FD_TEST( !fd_gui_hist_ts_append( gui, FD_GUI_HIST_SHRED_EVENTS, ts_ns, ts_ns, rec ) );
}

static void
append_replay_txn( fd_gui_t * gui, long ts_ns, ulong slot ) {
  fd_gui_store_replay_txn_t rec[ 1 ];
  memset( rec, 0, sizeof(*rec) );
  rec->completion_time_ns = ts_ns;
  rec->slot               = slot;

  FD_TEST( !fd_gui_hist_ts_append( gui, FD_GUI_HIST_REPLAY_TXN, ts_ns, ts_ns, rec ) );
}

/* ---- read helpers (presence checks) ----------------------------------- */

static int
epoch_present( fd_gui_t * gui, ulong epoch ) {
  fd_gui_hist_epoch_key_t key[ 1 ];
  key->epoch = epoch;
  return fd_gui_hist_kv_get( gui, FD_GUI_HIST_EPOCH, key )!=NULL;
}

static int
slot_meta_present( fd_gui_t * gui, int dbi, ulong slot ) {
  return fd_gui_hist_kv_get_slot_any( gui, dbi, slot )!=NULL;
}

/* count_ts counts time-series records in DB `dbi` over the whole timeline,
   optionally restricted to a single slot (slot==ULONG_MAX -> all).  The
   per-slot restriction reads the slot from the SHRED_EVENTS record (the only
   DB this is used with a specific slot), the same way the real query call
   sites filter. */
static ulong
count_ts( fd_gui_t * gui, int dbi, ulong slot ) {
  fd_gui_hist_iter_t it[ 1 ];
  FD_TEST( !fd_gui_hist_range_begin( gui, it, dbi, LONG_MIN+1, LONG_MAX-1L, NULL, NULL ) );
  ulong cnt = 0UL;
  while( fd_gui_hist_range_next( it ) ) {
    if( slot!=ULONG_MAX ) {
      fd_gui_slot_history_shred_event_t const * e = (fd_gui_slot_history_shred_event_t const *)it->rec;
      if( e->slot!=slot ) continue;
    }
    cnt++;
  }
  fd_gui_hist_range_end( it );
  return cnt;
}

static int
timeline_day_present( fd_gui_t * gui, ulong day ) {
  fd_gui_hist_timeline_day_key_t key = { .day=day };
  return fd_gui_hist_kv_get( gui, FD_GUI_HIST_TIMELINE_DAY, &key )!=NULL;
}

static fd_http_server_t *
test_http_new( void ** mem ) {
  fd_http_server_params_t params = {
    .max_connection_cnt    = 1UL,
    .max_ws_connection_cnt = 1UL,
    .max_request_len       = 4096UL,
    .max_ws_recv_frame_len = 4096UL,
    .max_ws_send_frame_cnt = 1UL,
    .outgoing_buffer_sz    = 1UL<<20,
    .compress_websocket    = 0,
  };
  ulong footprint = fd_http_server_footprint( params );
  *mem = aligned_alloc( fd_http_server_align(), fd_ulong_align_up( footprint, fd_http_server_align() ) );
  FD_TEST( *mem );
  fd_http_server_callbacks_t callbacks = {0};
  fd_http_server_t * http = fd_http_server_join( fd_http_server_new( *mem, params, callbacks, NULL ) );
  FD_TEST( http );
  return http;
}

static void
test_http_expect( fd_http_server_t * http,
                  char const *       expected ) {
  ulong len = strlen( expected );
  FD_TEST( http->stage_len==len );
  FD_TEST( !memcmp( http->oring+(http->stage_off%http->oring_sz), expected, len ) );
  fd_http_server_unstage( http );
}

static void
test_timeline_agg_queries( fd_gui_t * gui ) {
  void * http_mem = NULL;
  fd_http_server_t * http = test_http_new( &http_mem );
  gui->http = http;
  gui->summary.slot_rooted = 7000UL;

  /* One metric-bearing slot on each side of midnight exercises the renderer's
     daily-record cache and verifies that dense arrays preserve the common
     bucket index. */
  fd_gui_slot_t slots[ 2 ];
  memset( slots, 0xFF, sizeof(slots) );
  ulong const slot_number[ 2 ] = { 6000UL, 6002UL };
  for( ulong i=0UL; i<2UL; i++ ) {
    slots[ i ].slot                          = slot_number[ i ];
    slots[ i ].bank_seq                      = 1UL;
    slots[ i ].parent_slot                   = i ? slot_number[ i-1UL ] : slot_number[ i ]-1UL;
    slots[ i ].completed_time                = (long)((i+1UL)*FD_GUI_TIMELINE_DAY_NS)+(i ? 500000000L : FD_GUI_TIMELINE_DAY_NS-500000000L);
    slots[ i ].skip                          = FD_GUI_SKIP_STATUS_NOT_SKIPPED;
    slots[ i ].mine                          = 0;
    slots[ i ].shred_cnt                     = (uint)(i+1UL);
    slots[ i ].block_shred_cnt               = (uint)(i+1UL);
    slots[ i ].turbine_shred_cnt             = (uint)(10UL+i);
    slots[ i ].repair_shred_cnt              = (uint)(20UL+i);
    slots[ i ].reconstructed_shred_cnt       = (uint)(30UL+i);
    slots[ i ].published_shred_cnt           = UINT_MAX;
    slots[ i ].compute_units                 = (uint)(40UL+i);
    slots[ i ].max_compute_units             = (uint)(50UL+i);
    slots[ i ].transaction_fee               = 60UL+i;
    slots[ i ].priority_fee                  = 70UL+i;
    slots[ i ].tips                          = 80UL+i;
    fd_gui_timeline_root_slot( gui, &slots[ i ] );
  }

  /* A second landed slot in the latter bucket extends that bucket's range
     without contributing any additional metrics. */
  fd_gui_slot_t same_bucket;
  memset( &same_bucket, 0xFF, sizeof(same_bucket) );
  same_bucket.slot           = 6003UL;
  same_bucket.parent_slot    = slot_number[ 1 ];
  same_bucket.completed_time = 2L*FD_GUI_TIMELINE_DAY_NS+750000000L;
  same_bucket.skip           = FD_GUI_SKIP_STATUS_NOT_SKIPPED;
  same_bucket.mine           = 0;
  fd_gui_timeline_root_slot( gui, &same_bucket );

  fd_gui_printf_timeline_query_agg( gui, "query_agg_shreds", "1s", 0UL,
                                    2L*FD_GUI_TIMELINE_DAY_NS-1000000000L,
                                    2UL, gui->summary.slot_rooted, 33UL );
  test_http_expect( http,
    "{\"topic\":\"timeline\",\"key\":\"query_agg_shreds\",\"id\":33,\"value\":{\"granularity\":\"1s\",\"reference_ts_ns\":\"172799000000000\",\"root_slot\":7000,\"shreds\":[1,2],\"turbine\":[10,11],\"repair\":[20,21],\"reconstructed\":[30,31],\"published\":[0,0]}}" );

  fd_gui_printf_timeline_query_agg( gui, "query_agg_slots", "1s", 0UL,
                                    2L*FD_GUI_TIMELINE_DAY_NS-1000000000L,
                                    3UL, gui->summary.slot_rooted, 32UL );
  test_http_expect( http,
    "{\"topic\":\"timeline\",\"key\":\"query_agg_slots\",\"id\":32,\"value\":{\"granularity\":\"1s\",\"reference_ts_ns\":\"172799000000000\",\"start_slot\":[6000,6001,null],\"end_slot\":[6000,6003,null]}}" );

  fd_gui_printf_timeline_query_agg( gui, "query_agg_compute", "1s", 0UL,
                                    2L*FD_GUI_TIMELINE_DAY_NS-1000000000L,
                                    2UL, ULONG_MAX, 34UL );
  test_http_expect( http,
    "{\"topic\":\"timeline\",\"key\":\"query_agg_compute\",\"id\":34,\"value\":{\"granularity\":\"1s\",\"reference_ts_ns\":\"172799000000000\",\"root_slot\": null,\"compute_units\":[null,null],\"max_compute_units\": null}}" );

  fd_gui_printf_timeline_query_agg( gui, "query_agg_slots", "1s", 0UL,
                                    2L*FD_GUI_TIMELINE_DAY_NS-1000000000L,
                                    2UL, ULONG_MAX, 31UL );
  test_http_expect( http,
    "{\"topic\":\"timeline\",\"key\":\"query_agg_slots\",\"id\":31,\"value\":{\"granularity\":\"1s\",\"reference_ts_ns\":\"172799000000000\",\"start_slot\":[null,null],\"end_slot\":[null,null]}}" );

  /* Full websocket dispatch covers strict decimal parsing and the inclusive
     150-bucket limit.  The synthetic websocket is closed, so successful
     sends simply discard their staged response. */
#define WS_REQ(json,expect) do {                                                   \
    char const * _json = (json);                                                   \
    FD_TEST( fd_gui_ws_message( gui, 0UL, (uchar const *)_json, strlen( _json ) )==(expect) ); \
  } while(0)
  WS_REQ( "{\"topic\":\"timeline\",\"key\":\"query_agg_slots\",\"id\":1,\"params\":{\"start_ns\":\"0\",\"end_ns\":\"149000000000\",\"granularity\":\"1s\"}}", 0 );
  WS_REQ( "{\"topic\":\"timeline\",\"key\":\"query_agg_revenue\",\"id\":1,\"params\":{\"start_ns\":\"0\",\"end_ns\":\"149000000000\",\"granularity\":\"1s\"}}", 0 );
  WS_REQ( "{\"topic\":\"timeline\",\"key\":\"query_agg_revenue\",\"id\":1,\"params\":{\"start_ns\":\"0\",\"end_ns\":\"150000000000\",\"granularity\":\"1s\"}}", FD_HTTP_SERVER_CONNECTION_CLOSE_BAD_REQUEST );
  WS_REQ( "{\"topic\":\"timeline\",\"key\":\"query_agg_revenue\",\"id\":1,\"params\":{\"start_ns\":0,\"end_ns\":\"1\",\"granularity\":\"1s\"}}", FD_HTTP_SERVER_CONNECTION_CLOSE_BAD_REQUEST );
  WS_REQ( "{\"topic\":\"timeline\",\"key\":\"query_agg_revenue\",\"id\":1,\"params\":{\"start_ns\":\"2\",\"end_ns\":\"1\",\"granularity\":\"1s\"}}", FD_HTTP_SERVER_CONNECTION_CLOSE_BAD_REQUEST );
  WS_REQ( "{\"topic\":\"timeline\",\"key\":\"query_agg_revenue\",\"id\":1,\"params\":{\"start_ns\":\"0\",\"end_ns\":\"1\",\"granularity\":\"5s\"}}", FD_HTTP_SERVER_CONNECTION_CLOSE_BAD_REQUEST );
  WS_REQ( "{\"topic\":\"timeline\",\"key\":\"query_agg_revenue\",\"id\":1,\"params\":{\"start_ns\":\"9223372036854775808\",\"end_ns\":\"9223372036854775808\",\"granularity\":\"1s\"}}", FD_HTTP_SERVER_CONNECTION_CLOSE_BAD_REQUEST );
#undef WS_REQ

  gui->http = NULL;
  free( fd_http_server_delete( fd_http_server_leave( http ) ) );
  FD_LOG_NOTICE(( "test_timeline_agg_queries: ok" ));
}

static void
test_timeline_day_order( fd_gui_t * gui ) {
  void * http_mem = NULL;
  fd_http_server_t * http = test_http_new( &http_mem );
  gui->http = http;
  gui->summary.slot_rooted = 1UL;

  fd_gui_hist_slot_key_t keys[ 2 ] = {
    { .slot=100UL, .bank_seq=1UL },
    { .slot=101UL, .bank_seq=1UL },
  };
  for( ulong i=0UL; i<2UL; i++ ) {
    fd_gui_slot_t * slot = fd_gui_hist_kv_get_or_create( gui, FD_GUI_HIST_SLOT, &keys[ i ] );
    FD_TEST( slot );
    memset( slot, 0xFF, sizeof(*slot) );
    slot->slot            = keys[ i ].slot;
    slot->bank_seq        = keys[ i ].bank_seq;
    slot->parent_slot     = i ? keys[ 0 ].slot : 1UL;
    slot->parent_bank_seq = 1UL;
    slot->completed_time  = (long)((9UL+i)*FD_GUI_TIMELINE_DAY_NS+1000000000UL);
    slot->skip            = FD_GUI_SKIP_STATUS_NOT_SKIPPED;
    slot->level           = FD_GUI_SLOT_LEVEL_COMPLETED;
    slot->mine            = 0;
    slot->block_shred_cnt = 1U;
  }

  /* Root traversal itself is newest-to-oldest.  The preparation pass must
     nevertheless insert its date records oldest-first so prefix eviction can
     reclaim day 9 without discarding day 10. */
  fd_gui_handle_root_advanced( gui, keys[ 1 ].slot, keys[ 1 ].bank_seq, 0L );
  FD_TEST( timeline_day_present( gui, 9UL  ) );
  FD_TEST( timeline_day_present( gui, 10UL ) );

  fd_gui_hist_timeline_day_key_t hi = { .day=10UL };
  ulong budget = ULONG_MAX;
  int drained = 0;
  FD_TEST( fd_gui_store_kv_evict( gui->db, FD_GUI_HIST_TIMELINE_DAY, &hi, &budget, &drained )==FD_GUI_STORE_SUCCESS );
  FD_TEST( drained );
  FD_TEST( !timeline_day_present( gui, 9UL  ) );
  FD_TEST(  timeline_day_present( gui, 10UL ) );

  gui->http = NULL;
  free( fd_http_server_delete( fd_http_server_leave( http ) ) );
  FD_LOG_NOTICE(( "test_timeline_day_order: ok" ));
}

static void
test_timeline_aggregates( fd_gui_t * gui ) {
  fd_gui_slot_t completed[ 1 ];
  memset( completed, 0xFF, sizeof(completed) );

  ulong slot = 5000UL;
  fd_gui_timeline_handle_shred( gui, slot, SHRED_SIG_SRC_TURBINE );
  fd_gui_timeline_handle_shred( gui, slot, SHRED_SIG_SRC_TURBINE );
  fd_gui_timeline_handle_shred( gui, slot, SHRED_SIG_SRC_REPAIR );
  fd_gui_timeline_handle_shred( gui, slot, SHRED_SIG_SRC_RECONSTRUCTED );
  fd_gui_timeline_handle_fec( gui, slot, 0 );
  fd_gui_timeline_complete_slot( gui, slot, completed );
  FD_TEST( completed->block_shred_cnt==2U*FD_FEC_SHRED_CNT );
  FD_TEST( completed->turbine_shred_cnt==2U );
  FD_TEST( completed->repair_shred_cnt==1U );
  FD_TEST( completed->reconstructed_shred_cnt==1U );
  FD_TEST( completed->published_shred_cnt==UINT_MAX );

  /* A newer alias replaces an older entry; subsequent old repair traffic
     cannot evict the newer live slot. */
  ulong newer = slot+FD_GUI_TIMELINE_SHRED_BUF_CNT;
  fd_gui_timeline_handle_shred( gui, newer, SHRED_SIG_SRC_TURBINE );
  fd_gui_timeline_handle_shred( gui, slot,  SHRED_SIG_SRC_REPAIR );
  memset( completed, 0xFF, sizeof(completed) );
  fd_gui_timeline_complete_slot( gui, newer, completed );
  FD_TEST( completed->turbine_shred_cnt==1U );
  FD_TEST( completed->repair_shred_cnt==0U );

  /* A completion still drains the transient entry if slot persistence ran
     out of space and could not supply a destination record. */
  fd_gui_timeline_handle_shred( gui, newer+1UL, SHRED_SIG_SRC_REPAIR );
  fd_gui_timeline_complete_slot( gui, newer+1UL, NULL );
  memset( completed, 0xFF, sizeof(completed) );
  fd_gui_timeline_complete_slot( gui, newer+1UL, completed );
  FD_TEST( completed->repair_shred_cnt==UINT_MAX );

  memset( completed, 0xFF, sizeof(completed) );
  completed->slot                          = newer;
  completed->bank_seq                      = 1UL;
  completed->completed_time                = 1500000000L;
  completed->skip                          = FD_GUI_SKIP_STATUS_NOT_SKIPPED;
  completed->mine                          = 0;
  completed->shred_cnt                     = 100U;
  completed->block_shred_cnt               = 100U;
  completed->turbine_shred_cnt             = 70U;
  completed->repair_shred_cnt              = 20U;
  completed->reconstructed_shred_cnt       = 10U;
  completed->published_shred_cnt           = UINT_MAX;
  completed->compute_units                 = 1234U;
  completed->max_compute_units             = 48000000U;
  completed->transaction_fee               = 11UL;
  completed->priority_fee                  = 12UL;
  completed->tips                          = 13UL;
  fd_gui_timeline_root_slot( gui, completed );

  fd_gui_hist_timeline_day_key_t key = { .day=0UL };
  fd_gui_timeline_day_t const * day = fd_gui_hist_kv_get( gui, FD_GUI_HIST_TIMELINE_DAY, &key );
  FD_TEST( day );
  for( ulong g=0UL; g<FD_GUI_TIMELINE_GRANULARITY_CNT; g++ ) {
    ulong idx = fd_gui_timeline_granularity_off[ g ]+1500000000UL/fd_gui_timeline_granularity_ns[ g ];
    FD_TEST( day->start_slot   [ idx ]==newer );
    FD_TEST( day->end_slot     [ idx ]==newer );
    FD_TEST( day->shreds       [ idx ]==100UL );
    FD_TEST( day->turbine      [ idx ]==70UL  );
    FD_TEST( day->repair       [ idx ]==20UL  );
    FD_TEST( day->reconstructed[ idx ]==10UL  );
    FD_TEST( day->published    [ idx ]==0UL   );
    FD_TEST( day->compute_units[ idx ]==1234UL );
    FD_TEST( day->max_compute  [ idx ]==48000000UL );
    FD_TEST( day->txn_fees     [ idx ]==11UL );
    FD_TEST( day->prio_fees    [ idx ]==12UL );
    FD_TEST( day->tips         [ idx ]==13UL );
  }

  /* Shred/FEC messages can arrive after replay completion because they use a
     different GUI input link.  They update the completed slot directly, and
     once rooted also apply deltas to the existing daily rollup. */
  fd_gui_hist_slot_key_t late_key = { .slot=500UL, .bank_seq=7UL };
  fd_gui_slot_t * late = fd_gui_hist_kv_get_or_create( gui, FD_GUI_HIST_SLOT, &late_key );
  FD_TEST( late );
  memset( late, 0xFF, sizeof(*late) );
  late->slot           = late_key.slot;
  late->bank_seq       = late_key.bank_seq;
  late->completed_time = 3500000000L;
  late->skip           = FD_GUI_SKIP_STATUS_NOT_SKIPPED;
  late->mine           = 1;
  late->level          = FD_GUI_SLOT_LEVEL_COMPLETED;

  fd_gui_timeline_complete_slot( gui, late->slot, late );
  fd_gui_timeline_handle_shred( gui, late->slot, SHRED_SIG_SRC_TURBINE );
  fd_gui_timeline_handle_fec  ( gui, late->slot, 1 );
  FD_TEST( late->block_shred_cnt==2U*FD_FEC_SHRED_CNT );
  FD_TEST( late->published_shred_cnt==2U*FD_FEC_SHRED_CNT );
  FD_TEST( late->turbine_shred_cnt==1U );
  FD_TEST( late->repair_shred_cnt==0U );
  FD_TEST( late->reconstructed_shred_cnt==0U );

  late->level = FD_GUI_SLOT_LEVEL_ROOTED;
  fd_gui_timeline_root_slot( gui, late );
  fd_gui_timeline_handle_shred( gui, late->slot, SHRED_SIG_SRC_REPAIR );
  fd_gui_timeline_handle_fec  ( gui, late->slot, 0 );

  ulong late_idx = FD_GUI_TIMELINE_1S_OFF+3UL;
  FD_TEST( day->shreds       [ late_idx ]==4UL*FD_FEC_SHRED_CNT );
  FD_TEST( day->turbine      [ late_idx ]==1UL );
  FD_TEST( day->repair       [ late_idx ]==1UL );
  FD_TEST( day->reconstructed[ late_idx ]==0UL );
  FD_TEST( day->published    [ late_idx ]==2UL*FD_FEC_SHRED_CNT );

  FD_TEST( FD_GUI_TIMELINE_BUCKET_CNT==90793UL );
  FD_LOG_NOTICE(( "test_timeline_aggregates: ok" ));
}

/* ---- the test --------------------------------------------------------- */

static void
test_evict_oldest_epoch( fd_gui_t * gui ) {
  /* --- populate three epochs ----------------------------------------- */
  put_epoch( gui, EPOCH_A, A_START_SLOT, SLOT_CNT );
  put_epoch( gui, EPOCH_B, B_START_SLOT, SLOT_CNT );
  put_epoch( gui, EPOCH_C, C_START_SLOT, SLOT_CNT );

  for( ulong s=A_START_SLOT; s<=C_END_SLOT; s++ ) {
    put_slot( gui, s, epoch_slot_complete_ns( s ) );
    put_leader_slot( gui, s, epoch_slot_complete_ns( s ) );
  }

  /* time-series: one scheduler-counts sample per second across all epochs'
     windows [86390,86419], straddling midnight at the A/B boundary. */
  for( ulong sec=86390UL; sec<=86419UL; sec++ ) append_sched_counts( gui, sec_ns( sec ) );

  /* shred events: one per slot at the slot's own completion second */
  for( ulong s=A_START_SLOT; s<=C_END_SLOT; s++ ) {
    append_shred( gui, epoch_slot_complete_ns( s ), s );
    append_replay_txn( gui, epoch_slot_complete_ns( s ), s );
  }

  /* boundary case: a shred for epoch B's first slot (1010) that landed in
     window 86399 -- the same second as epoch A's last slot (1009).  Eviction
     of epoch A bounds the window there, but the slot watermark
     (1010 > 1009) must keep this row. */
  append_shred( gui, sec_ns( 86399UL ), B_START_SLOT );

  /* Day zero is owned exclusively by epoch A.  Day one belongs to epoch B
     and must survive eviction of epoch A. */
  fd_gui_slot_t timeline_slot;
  memset( &timeline_slot, 0xFF, sizeof(timeline_slot) );
  timeline_slot.slot           = A_END_SLOT;
  timeline_slot.parent_slot    = A_END_SLOT-1UL;
  timeline_slot.completed_time = sec_ns( 86399UL );
  timeline_slot.skip           = FD_GUI_SKIP_STATUS_NOT_SKIPPED;
  timeline_slot.mine           = 0;
  timeline_slot.shred_cnt      = 1U;
  timeline_slot.block_shred_cnt= 1U;
  fd_gui_timeline_root_slot( gui, &timeline_slot );
  timeline_slot.slot           = B_START_SLOT;
  timeline_slot.parent_slot    = A_END_SLOT;
  timeline_slot.completed_time = sec_ns( 86400UL );
  fd_gui_timeline_root_slot( gui, &timeline_slot );
  FD_TEST( timeline_day_present( gui, 0UL ) );
  FD_TEST( timeline_day_present( gui, 1UL ) );

  /* flush time-series so the writes are visible to range reads */
  /* (range_begin flushes internally, but count_ts below relies on that) */

  /* --- baseline assertions ------------------------------------------- */
  FD_TEST( epoch_present( gui, EPOCH_A ) );
  FD_TEST( epoch_present( gui, EPOCH_B ) );
  FD_TEST( epoch_present( gui, EPOCH_C ) );
  FD_TEST( slot_meta_present( gui, FD_GUI_HIST_SLOT, A_START_SLOT ) );
  FD_TEST( slot_meta_present( gui, FD_GUI_HIST_SLOT, B_START_SLOT ) );
  FD_TEST( slot_meta_present( gui, FD_GUI_HIST_LEADER_SLOT, A_END_SLOT ) );
  FD_TEST( slot_meta_present( gui, FD_GUI_HIST_LEADER_SLOT, B_END_SLOT ) );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SCHEDULER_COUNTS, ULONG_MAX )==30UL ); /* secs 10..39 */
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS,     ULONG_MAX )==31UL ); /* 30 slots + 1 boundary */
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS,     B_START_SLOT )==2UL ); /* slot 1010: its own + boundary */
  FD_TEST( count_ts( gui, FD_GUI_HIST_REPLAY_TXN,       ULONG_MAX )==30UL );

  /* --- evict the oldest epoch (A); B and C stay resident (the current +
     next epochs the floor protects) --------------------------------- */
  FD_TEST( fd_gui_hist_evict_oldest( gui )==1 );

  /* epoch A entirely gone; epochs B and C intact */
  FD_TEST( !epoch_present( gui, EPOCH_A ) );
  FD_TEST(  epoch_present( gui, EPOCH_B ) );
  FD_TEST(  epoch_present( gui, EPOCH_C ) );
  FD_TEST( !timeline_day_present( gui, 0UL ) );
  FD_TEST(  timeline_day_present( gui, 1UL ) );

  for( ulong s=A_START_SLOT; s<=A_END_SLOT; s++ ) {
    FD_TEST( !slot_meta_present( gui, FD_GUI_HIST_SLOT, s ) );
    FD_TEST( !slot_meta_present( gui, FD_GUI_HIST_LEADER_SLOT, s ) );
  }
  for( ulong s=B_START_SLOT; s<=C_END_SLOT; s++ ) {
    FD_TEST( slot_meta_present( gui, FD_GUI_HIST_SLOT, s ) );
    FD_TEST( slot_meta_present( gui, FD_GUI_HIST_LEADER_SLOT, s ) );
  }

  /* time-series: epoch A's ten windows are gone and epochs B+C stay. */
  FD_TEST( count_ts( gui, FD_GUI_HIST_SCHEDULER_COUNTS, ULONG_MAX )==20UL );
  FD_TEST( count_ts( gui, FD_GUI_HIST_REPLAY_TXN,       ULONG_MAX )==20UL );

  /* shred events: TS eviction is an approximate watermark advance (a
     monotonic prefix bump on the partition's evict_cur), not a precise
     by-window delete.  Records are stored in arrival order, which is only
     approximately window-ordered: the boundary row (slot 1010, epoch B) was
     appended LAST but carries window 19, so it sits in the ring *past* the
     watermark and survives eviction even though its window is in epoch A's
     range.  This is intentional -- readers re-filter on the record's own
     timestamp/slot, and the watermark never touches records below it.  So
     epoch A's 10 in-order slot rows (windows 10..19) are evicted, epochs B+C's
     20 rows (windows 20..39) are kept, and the straggler boundary row
     survives: 21 live rows, with slot 1010 keeping its own row plus the
     boundary row. */
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS, ULONG_MAX )==21UL );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS, B_START_SLOT )==2UL );
  /* an evicted epoch-A slot has no shred rows left */
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS, A_START_SLOT )==0UL );

  /* --- guard: only epochs B and C remain (== FD_GUI_HIST_MIN_EPOCHS-1) so
     eviction refuses.  The current in-progress epoch and the next epoch must
     always stay resident, so fd_gui_hist_evict_oldest is a no-op here. */
  FD_TEST( fd_gui_hist_evict_oldest( gui )==0 );
  FD_TEST( epoch_present( gui, EPOCH_B ) );
  FD_TEST( epoch_present( gui, EPOCH_C ) );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SCHEDULER_COUNTS, ULONG_MAX )==20UL );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS,     ULONG_MAX )==21UL );
  FD_TEST( count_ts( gui, FD_GUI_HIST_REPLAY_TXN,       ULONG_MAX )==20UL );

  FD_LOG_NOTICE(( "test_evict_oldest_epoch: ok" ));
}

/* test_evict_large_batch checks the resumable batching: an epoch with more
   than FD_GUI_HIST_EVICT_BATCH (512) distinct time-series keys must still be
   fully drained by fd_gui_hist_evict_oldest (which loops the bounded
   per-batch fd_gui_hist_evict_one until the cascade completes). */

#define BIG_EPOCH       (20UL)
#define BIG_START_SLOT  (2000UL)
#define BIG_SLOT_CNT    (1000UL) /* > 512, forces multiple delete batches */
#define BIG_KEEP_EPOCH  (21UL)   /* a newer epoch so BIG is the oldest + survivors remain */
#define BIG_KEEP_START  (3000UL)
#define BIG_KEEP2_EPOCH (22UL)   /* second keeper so we stay above FD_GUI_HIST_MIN_EPOCHS */
#define BIG_KEEP2_START (4000UL)

static void
test_evict_large_batch( fd_gui_t * gui ) {
  put_epoch( gui, BIG_EPOCH, BIG_START_SLOT, BIG_SLOT_CNT );

  /* one shred-event key per slot; distinct slots -> distinct keys.  Pack the
     timestamps into a compact window range. */
  ulong end_slot = BIG_START_SLOT + BIG_SLOT_CNT - 1UL;
  for( ulong s=BIG_START_SLOT; s<=end_slot; s++ ) {
    put_slot( gui, s, slot_complete_ns( 1000UL + (s-BIG_START_SLOT) ) );
    append_shred( gui, slot_complete_ns( 1000UL + (s-BIG_START_SLOT) ), s );
  }
  /* newer epochs (so BIG is the oldest, and the >= FD_GUI_HIST_MIN_EPOCHS guard
     is satisfied); the immediately-following epoch's first slot replay meta
     bounds BIG's time-series eviction window. */
  put_epoch( gui, BIG_KEEP_EPOCH, BIG_KEEP_START, BIG_SLOT_CNT );
  put_slot( gui, BIG_KEEP_START, slot_complete_ns( 1000UL + BIG_SLOT_CNT ) );
  put_epoch( gui, BIG_KEEP2_EPOCH, BIG_KEEP2_START, BIG_SLOT_CNT );
  put_slot( gui, BIG_KEEP2_START, slot_complete_ns( 1000UL + 2UL*BIG_SLOT_CNT ) );

  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS, ULONG_MAX )==BIG_SLOT_CNT );
  FD_TEST( epoch_present( gui, BIG_EPOCH ) );

  /* single synchronous drain must clear all 1000 keys (crossing the 512
     per-batch budget several times) */
  FD_TEST( fd_gui_hist_evict_oldest( gui )==1 );
  FD_TEST( !epoch_present( gui, BIG_EPOCH ) );
  FD_TEST(  epoch_present( gui, BIG_KEEP_EPOCH ) );
  FD_TEST(  epoch_present( gui, BIG_KEEP2_EPOCH ) );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS, ULONG_MAX )==0UL );
  for( ulong s=BIG_START_SLOT; s<=end_slot; s++ ) {
    FD_TEST( !slot_meta_present( gui, FD_GUI_HIST_SLOT, s ) );
  }

  FD_LOG_NOTICE(( "test_evict_large_batch: ok" ));
}

/* test_current_epoch_protected is the direct regression for the blank-nav-bar
   bug: at startup the validator publishes epoch info for the current
   (in-progress) epoch and the next epoch, so exactly two epochs are resident.
   Whole-epoch eviction must NEVER shed the current epoch in that state --
   doing so strips the leader schedule for "now" and the GUI nav bar goes
   blank.  The floor keeps at least FD_GUI_HIST_MIN_EPOCHS resident, so with
   only current + next present eviction must refuse. */

#define CP_CUR_EPOCH  (60UL)
#define CP_CUR_START  (10000UL)
#define CP_NEXT_EPOCH (61UL)
#define CP_NEXT_START (11000UL)
#define CP_SLOT_CNT   (5UL)

static void
test_current_epoch_protected( fd_gui_t * gui ) {
  /* current (in-progress) + next epoch, mirroring the startup publish. */
  put_epoch( gui, CP_CUR_EPOCH,  CP_CUR_START,  CP_SLOT_CNT );
  put_epoch( gui, CP_NEXT_EPOCH, CP_NEXT_START, CP_SLOT_CNT );
  put_slot( gui, CP_CUR_START,  slot_complete_ns( 1000UL ) );
  put_slot( gui, CP_NEXT_START, slot_complete_ns( 2000UL ) );

  FD_TEST( gui->epoch.stored_epoch_cnt==2UL );

  /* Two epochs resident (== FD_GUI_HIST_MIN_EPOCHS-1): eviction must refuse so
     the current epoch's schedule stays available to the GUI. */
  FD_TEST( fd_gui_hist_evict_oldest( gui )==0 );
  FD_TEST( epoch_present( gui, CP_CUR_EPOCH ) );
  FD_TEST( epoch_present( gui, CP_NEXT_EPOCH ) );
  FD_TEST( gui->epoch.stored_epoch_cnt==2UL );

  FD_LOG_NOTICE(( "test_current_epoch_protected: ok" ));
}

/* test_evict_ts_oldest_fallback covers fd_gui_hist_evict_ts_oldest, the
   last-resort reclaimer used when whole-epoch eviction is guard-blocked
   (fewer than FD_GUI_HIST_MIN_EPOCHS resident) yet space is still needed.  It
   must shed time-series data one oldest window at a time WITHOUT touching
   epoch/slot metadata. */

#define TS_EPOCH      (30UL)
#define TS_START_SLOT (4000UL)
#define TS_SLOT_CNT   (5UL)

static void
test_evict_ts_oldest_fallback( fd_gui_t * gui ) {
  /* A single epoch (below FD_GUI_HIST_MIN_EPOCHS, so the whole-epoch guard
     blocks eviction) with time-series data spread over 5 distinct windows
     [50,54]. */
  put_epoch( gui, TS_EPOCH, TS_START_SLOT, TS_SLOT_CNT );
  put_slot( gui, TS_START_SLOT, slot_complete_ns( 990UL+50UL ) );
  for( ulong sec=50UL; sec<=54UL; sec++ ) {
    append_sched_counts( gui, sec_ns( sec ) );
    append_shred( gui, sec_ns( sec ), TS_START_SLOT + (sec-50UL) );
  }

  FD_TEST(  epoch_present( gui, TS_EPOCH ) );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SCHEDULER_COUNTS, ULONG_MAX )==5UL );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS,     ULONG_MAX )==5UL );

  /* Whole-epoch eviction refuses (below FD_GUI_HIST_MIN_EPOCHS resident). */
  FD_TEST( fd_gui_hist_evict_oldest( gui )==0 );
  FD_TEST( gui->epoch.stored_epoch_cnt==1UL );

  /* The TS fallback sheds the oldest live window (50) across all TS DBs in one
     step: one scheduler-counts row and one shred row drop, the epoch and its
     slot metadata are untouched. */
  FD_TEST( fd_gui_hist_evict_ts_oldest( gui )==1 );
  FD_TEST(  epoch_present( gui, TS_EPOCH ) );
  FD_TEST(  slot_meta_present( gui, FD_GUI_HIST_SLOT, TS_START_SLOT ) );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SCHEDULER_COUNTS, ULONG_MAX )==4UL );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS,     ULONG_MAX )==4UL );

  /* Drive it to exhaustion: each call sheds the next-oldest window until the
     TS DBs are empty, at which point it reports 0 (nothing left). */
  for( int i=0; i<4; i++ ) FD_TEST( fd_gui_hist_evict_ts_oldest( gui )==1 );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SCHEDULER_COUNTS, ULONG_MAX )==0UL );
  FD_TEST( count_ts( gui, FD_GUI_HIST_SHRED_EVENTS,     ULONG_MAX )==0UL );
  FD_TEST( fd_gui_hist_evict_ts_oldest( gui )==0 ); /* genuinely nothing left */
  /* Epoch metadata survived the entire TS drain. */
  FD_TEST(  epoch_present( gui, TS_EPOCH ) );

  FD_LOG_NOTICE(( "test_evict_ts_oldest_fallback: ok" ));
}

/* test_resident_meta_mutation_survives_evict checks the in-place mutation
   model the GUI now relies on: per-epoch mutable bookkeeping lives in the DB
   EPOCH record and is written through the stable map pointer returned by
   fd_gui_hist_kv_get.  Mutating a resident (newer) epoch's record in place
   must be durable, and evicting an OLDER epoch must not disturb the newer
   epoch's record or its mutated fields. */

#define RM_OLD_EPOCH  (40UL)
#define RM_OLD_START  (5000UL)
#define RM_NEW_EPOCH  (41UL)
#define RM_NEW_START  (6000UL)
#define RM_NEW2_EPOCH (42UL)
#define RM_NEW2_START (7000UL)
#define RM_SLOT_CNT   (5UL)

static void
test_resident_meta_mutation_survives_evict( fd_gui_t * gui ) {
  /* Three epochs durable (satisfies the >= FD_GUI_HIST_MIN_EPOCHS eviction
     guard so the oldest can be evicted while two keepers remain). */
  put_epoch( gui, RM_OLD_EPOCH, RM_OLD_START, RM_SLOT_CNT );
  put_epoch( gui, RM_NEW_EPOCH, RM_NEW_START, RM_SLOT_CNT );
  put_epoch( gui, RM_NEW2_EPOCH, RM_NEW2_START, RM_SLOT_CNT );
  /* the older epoch's time-series window is bounded by the next epoch's first
     completed slot, so give each a replay meta. */
  put_slot( gui, RM_OLD_START, slot_complete_ns( 1000UL ) );
  put_slot( gui, RM_NEW_START, slot_complete_ns( 2000UL ) );
  put_slot( gui, RM_NEW2_START, slot_complete_ns( 3000UL ) );

  /* Resolve the newer (resident) epoch's record pointer and mutate the
     per-epoch bookkeeping fields in place. */
  fd_gui_hist_epoch_key_t key[ 1 ]; key->epoch = RM_NEW_EPOCH;
  fd_gui_epoch_t * rec = (fd_gui_epoch_t *)fd_gui_hist_kv_get( gui, FD_GUI_HIST_EPOCH, key );
  FD_TEST( rec );
  rec->my_total_slots          = 7UL;
  rec->my_skipped_slots        = 3UL;
  rec->latency_exact[ 0 ]      = 2;
  rec->rankings->largest_tips[ 0 ].slot  = RM_NEW_START + 4UL;
  rec->rankings->largest_tips[ 0 ].value = 12345UL;

  /* A fresh get must observe the in-place writes (no put/round-trip). */
  fd_gui_epoch_t * rec2 = (fd_gui_epoch_t *)fd_gui_hist_kv_get( gui, FD_GUI_HIST_EPOCH, key );
  FD_TEST( rec2==rec ); /* stable map pointer */
  FD_TEST( rec2->my_total_slots==7UL );
  FD_TEST( rec2->my_skipped_slots==3UL );

  /* Evict the older epoch; the newer epoch's record and its mutated fields
     must be untouched, and its map pointer must remain valid. */
  FD_TEST( fd_gui_hist_evict_oldest( gui )==1 );
  FD_TEST( !epoch_present( gui, RM_OLD_EPOCH ) );
  FD_TEST(  epoch_present( gui, RM_NEW_EPOCH ) );
  FD_TEST(  epoch_present( gui, RM_NEW2_EPOCH ) );

  fd_gui_epoch_t * rec3 = (fd_gui_epoch_t *)fd_gui_hist_kv_get( gui, FD_GUI_HIST_EPOCH, key );
  FD_TEST( rec3==rec );
  FD_TEST( rec3->epoch==RM_NEW_EPOCH );
  FD_TEST( rec3->my_total_slots==7UL );
  FD_TEST( rec3->my_skipped_slots==3UL );
  FD_TEST( rec3->latency_exact[ 0 ]==2 );
  FD_TEST( rec3->rankings->largest_tips[ 0 ].slot==RM_NEW_START + 4UL );
  FD_TEST( rec3->rankings->largest_tips[ 0 ].value==12345UL );

  FD_LOG_NOTICE(( "test_resident_meta_mutation_survives_evict: ok" ));
}

/* test_epoch_region_reclaimed tests epoch eviction shrinking the
   DB's committed footprint. */

#define RR_A_EPOCH (50UL)
#define RR_A_START (7000UL)
#define RR_SLOT_CNT (5UL)

static void
test_epoch_region_reclaimed( fd_gui_t * gui ) {
  ulong const epoch_region_capacity =
      FD_GUI_STORE_REGION_SZ / fd_ulong_align_up( sizeof(fd_gui_epoch_t), 8UL );
  FD_TEST( epoch_region_capacity>0UL );

  /* Keep three epochs durable so the oldest remains evictable while two
     keepers survive. */
  for( ulong ordinal=0UL; ordinal<3UL; ordinal++ ) {
    ulong epoch = RR_A_EPOCH + ordinal;
    ulong start = RR_A_START + ordinal*RR_SLOT_CNT;
    put_epoch( gui, epoch, start, RR_SLOT_CNT );
    put_slot( gui, start, slot_complete_ns( 1000UL + ordinal ) );
  }

  /* Rotate until the oldest live record is the final slot in its region.
     This is a no-op when each epoch record already occupies a whole region. */
  for( ulong ordinal=3UL; ordinal<epoch_region_capacity+2UL; ordinal++ ) {
    FD_TEST( fd_gui_hist_evict_oldest( gui )==1 );
    ulong epoch = RR_A_EPOCH + ordinal;
    ulong start = RR_A_START + ordinal*RR_SLOT_CNT;
    put_epoch( gui, epoch, start, RR_SLOT_CNT );
    put_slot( gui, start, slot_complete_ns( 1000UL + ordinal ) );
  }

  ulong oldest = RR_A_EPOCH + epoch_region_capacity - 1UL;
  FD_TEST( epoch_present( gui, oldest      ) );
  FD_TEST( epoch_present( gui, oldest+1UL ) );
  FD_TEST( epoch_present( gui, oldest+2UL ) );

  /* The next eviction advances the EPOCH watermark across a region boundary. */
  ulong used_before = fd_gui_store_used_bytes( gui->db );
  FD_TEST( used_before>0UL );

  FD_TEST( fd_gui_hist_evict_oldest( gui )==1 );
  FD_TEST( !epoch_present( gui, oldest      ) );
  FD_TEST(  epoch_present( gui, oldest+1UL ) );
  FD_TEST(  epoch_present( gui, oldest+2UL ) );

  ulong used_after = fd_gui_store_used_bytes( gui->db );
  FD_TEST( used_after<used_before );

  FD_LOG_NOTICE(( "test_epoch_region_reclaimed: used %lu -> %lu bytes; ok",
                  used_before, used_after ));
}

/* ---- store lifecycle (bare fd_gui_t + the two store layers) ----------- */

struct test_store {
  fd_gui_t * gui;
  void *     db_mem;
  void *     hist_mem;
  char       path[ 128 ];
};
typedef struct test_store test_store_t;

static void
store_open( test_store_t * s, ulong map_bytes, int instance ) {
  fd_cstr_printf_check( s->path, sizeof(s->path), NULL, "/tmp/fd_gui_hist_evict_test.%i.%i", (int)getpid(), instance );

  s->gui = aligned_alloc( fd_gui_align(), fd_gui_footprint( 1UL ) );
  FD_TEST( s->gui );
  memset( s->gui, 0, fd_gui_footprint( 1UL ) );

  s->db_mem = aligned_alloc( fd_gui_store_align(),
                             fd_ulong_align_up( fd_gui_store_footprint( map_bytes, fd_gui_hist_db_cnt(), fd_gui_hist_db_descs( map_bytes ) ), fd_gui_store_align() ) );
  FD_TEST( s->db_mem );
  s->gui->db = fd_gui_store_join( fd_gui_store_new( s->db_mem, s->path, map_bytes, fd_gui_hist_db_cnt(), fd_gui_hist_db_descs( map_bytes ) ) );
  FD_TEST( s->gui->db );

  s->hist_mem = aligned_alloc( fd_gui_hist_align(),
                               fd_ulong_align_up( fd_gui_hist_footprint(), fd_gui_hist_align() ) );
  FD_TEST( s->hist_mem );
  s->gui->hist = fd_gui_hist_join( fd_gui_hist_new( s->hist_mem, s->gui->db ) );
  FD_TEST( s->gui->hist );
}

static void
store_close( test_store_t * s ) {
  fd_gui_store_delete( fd_gui_store_leave( s->gui->db ) );
  free( s->hist_mem );
  free( s->db_mem );
  free( s->gui );
  rm_tmpdir( s->path );
}

static void
test_timeline_queries( fd_gui_t * gui ) {
  long const start_ns = 10000000000L;
  long const end_ns   = start_ns + 1000L;

  fd_gui_store_replay_txn_t a = {
    .completion_time_ns      = start_ns,
    .slot                    = 11UL,
    .txn_idx                 = 2UL,
    .txn_exec_idx            = 1UL,
    .sigverify_start_ns      = start_ns-5L,
    .sigverify_end_ns        = start_ns,
    .exec_start_ns           = start_ns-4L,
    .load_start_ns           = start_ns-3L,
    .check_start_ns          = start_ns-2L,
    .execute_start_ns        = LONG_MAX,
    .commit_start_ns         = LONG_MAX,
    .commit_end_ns           = start_ns-1L,
    .transaction_fee         = 5000UL,
    .priority_fee            = 1200UL,
    .tips                    = 7UL,
    .compute_units_requested = 3428U,
    .compute_units_consumed  = 3000U,
    .error_code              = 9U,
    .is_committable          = 1U,
    .is_fees_only            = 0U,
    .is_simple_vote          = 1U
  };
  fd_gui_store_replay_txn_t b = {
    .completion_time_ns      = end_ns,
    .slot                    = 10UL,
    .txn_idx                 = 5UL,
    .txn_exec_idx            = 0UL,
    .sigverify_start_ns      = start_ns-10L,
    .sigverify_end_ns        = end_ns-1L,
    .exec_start_ns           = start_ns-9L,
    .load_start_ns           = start_ns-8L,
    .check_start_ns          = LONG_MAX,
    .execute_start_ns        = start_ns-7L,
    .commit_start_ns         = start_ns-6L,
    .commit_end_ns           = end_ns,
    .transaction_fee         = 0UL,
    .priority_fee            = 0UL,
    .tips                    = 0UL,
    .compute_units_requested = 0U,
    .compute_units_consumed  = 0U,
    .error_code              = 3U,
    .is_committable          = 0U,
    .is_fees_only            = 0U,
    .is_simple_vote          = 0U
  };
  fd_gui_store_replay_txn_t outside = b;
  outside.completion_time_ns = end_ns+1L;
  outside.slot               = 12UL;
  outside.txn_idx            = 99UL;

  FD_TEST( !fd_gui_hist_ts_append( gui, FD_GUI_HIST_REPLAY_TXN, a.completion_time_ns,       a.completion_time_ns,       &a       ) );
  FD_TEST( !fd_gui_hist_ts_append( gui, FD_GUI_HIST_REPLAY_TXN, b.completion_time_ns,       b.completion_time_ns,       &b       ) );
  FD_TEST( !fd_gui_hist_ts_append( gui, FD_GUI_HIST_REPLAY_TXN, outside.completion_time_ns, outside.completion_time_ns, &outside ) );

  fd_http_server_params_t params = {
    .max_connection_cnt    = 1UL,
    .max_ws_connection_cnt = 1UL,
    .max_request_len       = 1024UL,
    .max_ws_recv_frame_len = 1024UL,
    .max_ws_send_frame_cnt = 1UL,
    .outgoing_buffer_sz    = FD_GUI_TIMELINE_QUERY_MIN_SEND_BUFFER_SZ
  };
  fd_http_server_callbacks_t callbacks = {0};
  ulong http_footprint = fd_http_server_footprint( params );
  void * http_mem = aligned_alloc( fd_http_server_align(), fd_ulong_align_up( http_footprint, fd_http_server_align() ) );
  FD_TEST( http_mem );
  gui->http = fd_http_server_join( fd_http_server_new( http_mem, params, callbacks, NULL ) );
  FD_TEST( gui->http );

  fd_wksp_t * wksp = fd_wksp_new_anonymous( FD_SHMEM_NORMAL_PAGE_SZ, 4096UL, 0UL, "gui_query", 0UL );
  FD_TEST( wksp );
  void * alloc_mem = fd_wksp_alloc_laddr( wksp, fd_alloc_align(), fd_alloc_footprint(), 1UL );
  FD_TEST( alloc_mem );
  gui->alloc = fd_alloc_join( fd_alloc_new( alloc_mem, 1UL ), 0UL );
  FD_TEST( gui->alloc );

  ulong txn_reads = fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ];
  FD_TEST( !fd_gui_printf_timeline_query_txns( gui, "timeline", "query_txn_timestamps", start_ns, end_ns, 34UL ) );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ]==txn_reads+1UL );
  FD_TEST( !gui->http->stage_err );
  char * json = malloc( gui->http->stage_len+1UL );
  FD_TEST( json );
  fd_memcpy( json, gui->http->oring+(gui->http->stage_off%gui->http->oring_sz), gui->http->stage_len );
  json[ gui->http->stage_len ] = '\0';

  FD_TEST( strstr( json, "\"topic\":\"timeline\",\"key\":\"query_txn_timestamps\",\"id\":34" ) );
  FD_TEST( strstr( json, "\"reference_slot\":10,\"reference_ts\":\"9999999990\"" ) );
  FD_TEST( strstr( json, "\"slot_delta\":[0,1]" ) );
  FD_TEST( strstr( json, "\"txn_idx\":[5,2]" ) );
  FD_TEST( strstr( json, "\"txn_exec_idx\":[0,1]" ) );
  FD_TEST( strstr( json, "\"txn_sigverify_start_ts_delta\":[\"0\",\"5\"]" ) );
  FD_TEST( strstr( json, "\"txn_check_start_ts_delta\":[null,\"8\"]" ) );
  FD_TEST( strstr( json, "\"txn_execute_start_ts_delta\":[\"3\",null]" ) );
  FD_TEST( strstr( json, "\"txn_commit_start_ts_delta\":[\"4\",null]" ) );
  FD_TEST( strstr( json, "\"txn_commit_end_ts_delta\":[\"1010\",\"9\"]" ) );
  FD_TEST( !strstr( json, "\"txn_signature\"" ) );
  FD_TEST( !strstr( json, "\"txn_compute_units_requested\"" ) );
  FD_TEST( !strstr( json, "\"txn_microblock_id\"" ) );
  FD_TEST( !strstr( json, "\"txn_exec_tile_idx\"" ) );
  free( json );

  fd_http_server_unstage( gui->http );
  txn_reads = fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ];
  FD_TEST( !fd_gui_printf_timeline_query_txns( gui, "timeline", "query_txn_meta", start_ns, end_ns, 35UL ) );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ]==txn_reads+1UL );
  FD_TEST( !gui->http->stage_err );
  json = malloc( gui->http->stage_len+1UL );
  FD_TEST( json );
  fd_memcpy( json, gui->http->oring+(gui->http->stage_off%gui->http->oring_sz), gui->http->stage_len );
  json[ gui->http->stage_len ] = '\0';

  FD_TEST( strstr( json, "\"topic\":\"timeline\",\"key\":\"query_txn_meta\",\"id\":35" ) );
  FD_TEST( strstr( json, "\"reference_slot\":10,\"reference_ts\":\"9999999990\"" ) );
  FD_TEST( strstr( json, "\"slot_delta\":[0,1]" ) );
  FD_TEST( strstr( json, "\"txn_idx\":[5,2]" ) );
  FD_TEST( strstr( json, "\"txn_exec_idx\":[0,1]" ) );
  FD_TEST( strstr( json, "\"txn_compute_units_requested\":[null,3428]" ) );
  FD_TEST( strstr( json, "\"txn_transaction_fee\":[\"0\",\"5000\"]" ) );
  FD_TEST( strstr( json, "\"txn_error_code\":[3,9]" ) );
  FD_TEST( strstr( json, "\"txn_is_committable\":[false,true]" ) );
  FD_TEST( !strstr( json, "\"txn_sigverify_start_ts_delta\"" ) );
  FD_TEST( !strstr( json, "\"txn_commit_end_ts_delta\"" ) );
  FD_TEST( !strstr( json, "\"txn_microblock_id\"" ) );
  FD_TEST( !strstr( json, "\"txn_exec_tile_idx\"" ) );
  free( json );

  fd_http_server_unstage( gui->http );
  FD_TEST( !fd_gui_printf_timeline_query_txns( gui, "timeline", "query_txn_meta", end_ns+2L, end_ns+3L, 36UL ) );
  FD_TEST( !gui->http->stage_err );
  json = malloc( gui->http->stage_len+1UL );
  FD_TEST( json );
  fd_memcpy( json, gui->http->oring+(gui->http->stage_off%gui->http->oring_sz), gui->http->stage_len );
  json[ gui->http->stage_len ] = '\0';
  FD_TEST( strstr( json, "\"reference_slot\": null,\"reference_ts\": null,\"slot_delta\":[]" ) );
  free( json );

  /* Replay transaction timestamps come from the local monotonic clock and
     remain exact even if the GUI consumes the notification much later. */
  fd_http_server_unstage( gui->http );
  long const lagged_ts = 2000000000000L;
  fd_gui_store_replay_txn_t lagged = b;
  lagged.completion_time_ns = lagged_ts;
  lagged.slot               = 13UL;
  lagged.txn_idx            = 7UL;
  lagged.sigverify_start_ns = lagged_ts-10L;
  lagged.sigverify_end_ns   = lagged_ts-1L;
  lagged.exec_start_ns      = lagged_ts-9L;
  lagged.load_start_ns      = lagged_ts-8L;
  lagged.check_start_ns     = LONG_MAX;
  lagged.execute_start_ns   = lagged_ts-7L;
  lagged.commit_start_ns    = lagged_ts-6L;
  lagged.commit_end_ns      = lagged_ts;
  FD_TEST( !fd_gui_hist_ts_append( gui, FD_GUI_HIST_REPLAY_TXN,
                                   lagged_ts+100000000000L, lagged_ts, &lagged ) );

  /* Make the exact timestamp an append-order interior extreme.  Endpoint
     bounds alone would miss it; conservative observed extrema must not. */
  fd_gui_store_replay_txn_t lagged_tail = lagged;
  lagged_tail.completion_time_ns = 1000000000000L;
  FD_TEST( !fd_gui_hist_ts_append( gui, FD_GUI_HIST_REPLAY_TXN,
                                   lagged_tail.completion_time_ns,
                                   lagged_tail.completion_time_ns,
                                   &lagged_tail ) );
  FD_TEST( !fd_gui_printf_timeline_query_txns( gui, "timeline", "query_txn_timestamps", lagged_ts, lagged_ts, 36UL ) );
  json = malloc( gui->http->stage_len+1UL );
  FD_TEST( json );
  fd_memcpy( json, gui->http->oring+(gui->http->stage_off%gui->http->oring_sz), gui->http->stage_len );
  json[ gui->http->stage_len ] = '\0';
  FD_TEST( strstr( json, "\"txn_idx\":[7]" ) );
  free( json );

  fd_http_server_unstage( gui->http );
  char const request_wide[] =
    "{\"topic\":\"timeline\",\"key\":\"query_txn_timestamps\",\"id\":36,"
    "\"params\":{\"start_ns\":\"10000000000\",\"end_ns\":\"26000000000\"}}";
  FD_TEST( !fd_gui_ws_message( gui, 0UL, (uchar const *)request_wide, sizeof(request_wide)-1UL ) );

  char const request_wider[] =
    "{\"topic\":\"timeline\",\"key\":\"query_txn_meta\",\"id\":37,"
    "\"params\":{\"start_ns\":\"10000000000\",\"end_ns\":\"26000000001\"}}";
  FD_TEST( !fd_gui_ws_message( gui, 0UL, (uchar const *)request_wider, sizeof(request_wider)-1UL ) );

  char const request_removed[] =
    "{\"topic\":\"timeline\",\"key\":\"query_transactions\",\"id\":38,"
    "\"params\":{\"start_ns\":\"10000000000\",\"end_ns\":\"26000000001\"}}";
  FD_TEST( fd_gui_ws_message( gui, 0UL, (uchar const *)request_removed, sizeof(request_removed)-1UL )==
           FD_HTTP_SERVER_CONNECTION_CLOSE_UNKNOWN_METHOD );

  char const * invalid_requests[] = {
    "{\"topic\":\"timeline\",\"key\":\"query_txn_timestamps\",\"id\":38,\"params\":{\"start_ns\":1,\"end_ns\":\"2\"}}",
    "{\"topic\":\"timeline\",\"key\":\"query_txn_meta\",\"id\":38,\"params\":{\"start_ns\":\"\",\"end_ns\":\"2\"}}",
    "{\"topic\":\"timeline\",\"key\":\"query_txn_timestamps\",\"id\":38,\"params\":{\"start_ns\":\"-1\",\"end_ns\":\"2\"}}",
    "{\"topic\":\"timeline\",\"key\":\"query_txn_meta\",\"id\":38,\"params\":{\"start_ns\":\"1x\",\"end_ns\":\"2\"}}",
    "{\"topic\":\"timeline\",\"key\":\"query_txn_timestamps\",\"id\":38,\"params\":{\"start_ns\":\"1\\u0000junk\",\"end_ns\":\"2\"}}",
    "{\"topic\":\"timeline\",\"key\":\"query_txn_meta\",\"id\":38,\"params\":{\"start_ns\":\"9223372036854775807\",\"end_ns\":\"9223372036854775807\"}}",
    "{\"topic\":\"timeline\",\"key\":\"query_txn_timestamps\",\"id\":38,\"params\":{\"start_ns\":\"2\",\"end_ns\":\"1\"}}"
  };
  for( ulong i=0UL; i<sizeof(invalid_requests)/sizeof(invalid_requests[ 0 ]); i++ )
    FD_TEST( fd_gui_ws_message( gui, 0UL, (uchar const *)invalid_requests[ i ], strlen( invalid_requests[ i ] ) )==
             FD_HTTP_SERVER_CONNECTION_CLOSE_BAD_REQUEST );

  static uchar const request_literal_nul[] =
    "{\"topic\":\"timeline\",\"key\":\"query_txn_meta\",\"id\":38,"
    "\"params\":{\"start_ns\":\"1\0junk\",\"end_ns\":\"2\"}}";
  FD_TEST( fd_gui_ws_message( gui, 0UL, request_literal_nul, sizeof(request_literal_nul)-1UL )==
           FD_HTTP_SERVER_CONNECTION_CLOSE_BAD_REQUEST );

  /* A wide range entirely outside retained history is rejected by the live
     bounds without opening a store scan. */
  long future_start = LONG_MAX-10000000000000000L;
  long future_end   = LONG_MAX-1L;
  txn_reads = fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ];
  FD_TEST( !fd_gui_printf_timeline_query_txns( gui, "timeline", "query_txn_meta", future_start, future_end, 39UL ) );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ]==txn_reads );
  fd_http_server_unstage( gui->http );

  /* Exactly the transaction limit succeeds in one scan; limit+1 produces a
     small error envelope with no partial value. */
  long const txn_limit_ts = 100000000000L;
  for( ulong i=0UL; i<FD_GUI_TIMELINE_QUERY_TRANSACTION_MAX; i++ ) append_replay_txn( gui, txn_limit_ts, i );
  append_replay_txn( gui, txn_limit_ts+1L, FD_GUI_TIMELINE_QUERY_TRANSACTION_MAX );

  txn_reads = fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ];
  ulong txn_read_records = fd_gui_store_metrics( gui->db )->ts_read_records[ FD_GUI_HIST_REPLAY_TXN ];
  FD_TEST( !fd_gui_printf_timeline_query_txns( gui, "timeline", "query_txn_timestamps", txn_limit_ts, txn_limit_ts, 40UL ) );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ]==txn_reads+1UL );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_read_records[ FD_GUI_HIST_REPLAY_TXN ]==txn_read_records+FD_GUI_TIMELINE_QUERY_TRANSACTION_MAX+1UL );
  FD_TEST( !gui->http->stage_err && gui->http->stage_len<FD_GUI_TIMELINE_QUERY_MIN_SEND_BUFFER_SZ );
  gui->http->ws_conns[ 0 ].compress_websocket = 1;
  FD_TEST( !fd_http_server_ws_send( gui->http, 0UL ) );
  FD_TEST( !gui->http->stage_err && !gui->http->stage_len && !gui->http->stage_comp_len );

  txn_reads        = fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ];
  txn_read_records = fd_gui_store_metrics( gui->db )->ts_read_records[ FD_GUI_HIST_REPLAY_TXN ];
  FD_TEST( !fd_gui_printf_timeline_query_txns( gui, "timeline", "query_txn_meta", txn_limit_ts, txn_limit_ts+1L, 41UL ) );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_REPLAY_TXN ]==txn_reads+1UL );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_read_records[ FD_GUI_HIST_REPLAY_TXN ]==txn_read_records+FD_GUI_TIMELINE_QUERY_TRANSACTION_MAX+1UL );
  json = malloc( gui->http->stage_len+1UL );
  FD_TEST( json );
  fd_memcpy( json, gui->http->oring+(gui->http->stage_off%gui->http->oring_sz), gui->http->stage_len );
  json[ gui->http->stage_len ] = '\0';
  FD_TEST( strstr( json, "\"error\":{\"code\":\"result_limit_exceeded\"}" ) );
  FD_TEST( !strstr( json, "\"max_records\"" ) );
  FD_TEST( strstr( json, "\"topic\":\"timeline\",\"key\":\"query_txn_meta\",\"id\":41" ) );
  FD_TEST( !strstr( json, "\"value\"" ) );
  free( json );
  fd_http_server_unstage( gui->http );

  /* Apply the same one-scan and exact-limit behavior to shred rows. */
  long const shred_limit_ts = 200000000000L;
  for( ulong i=0UL; i<FD_GUI_TIMELINE_QUERY_SHRED_MAX; i++ ) append_shred( gui, shred_limit_ts, 1UL );
  append_shred( gui, shred_limit_ts+1L, 1UL );

  ulong shred_reads        = fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_SHRED_EVENTS ];
  ulong shred_read_records = fd_gui_store_metrics( gui->db )->ts_read_records[ FD_GUI_HIST_SHRED_EVENTS ];
  FD_TEST( !fd_gui_printf_timeline_query_shreds( gui, "timeline", shred_limit_ts, shred_limit_ts, 42UL ) );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_SHRED_EVENTS ]==shred_reads+1UL );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_read_records[ FD_GUI_HIST_SHRED_EVENTS ]==shred_read_records+FD_GUI_TIMELINE_QUERY_SHRED_MAX+1UL );
  FD_TEST( !gui->http->stage_err && gui->http->stage_len<FD_GUI_TIMELINE_QUERY_MIN_SEND_BUFFER_SZ );
  FD_TEST( !fd_http_server_ws_send( gui->http, 0UL ) );
  FD_TEST( !gui->http->stage_err && !gui->http->stage_len && !gui->http->stage_comp_len );

  shred_reads        = fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_SHRED_EVENTS ];
  shred_read_records = fd_gui_store_metrics( gui->db )->ts_read_records[ FD_GUI_HIST_SHRED_EVENTS ];
  FD_TEST( !fd_gui_printf_timeline_query_shreds( gui, "timeline", shred_limit_ts, shred_limit_ts+1L, 43UL ) );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_reads[ FD_GUI_HIST_SHRED_EVENTS ]==shred_reads+1UL );
  FD_TEST( fd_gui_store_metrics( gui->db )->ts_read_records[ FD_GUI_HIST_SHRED_EVENTS ]==shred_read_records+FD_GUI_TIMELINE_QUERY_SHRED_MAX+1UL );
  json = malloc( gui->http->stage_len+1UL );
  FD_TEST( json );
  fd_memcpy( json, gui->http->oring+(gui->http->stage_off%gui->http->oring_sz), gui->http->stage_len );
  json[ gui->http->stage_len ] = '\0';
  FD_TEST( strstr( json, "\"error\":{\"code\":\"result_limit_exceeded\"}" ) );
  FD_TEST( !strstr( json, "\"max_records\"" ) );
  FD_TEST( !strstr( json, "\"value\"" ) );
  free( json );
  fd_http_server_unstage( gui->http );

  /* If staging unexpectedly fails, the request reports a connection-local
     close reason instead of terminating the tile. */
  fd_http_server_t * normal_http = gui->http;
  fd_http_server_params_t tiny_params = params;
  tiny_params.outgoing_buffer_sz = 32UL;
  ulong tiny_http_footprint = fd_http_server_footprint( tiny_params );
  void * tiny_http_mem = aligned_alloc( fd_http_server_align(), fd_ulong_align_up( tiny_http_footprint, fd_http_server_align() ) );
  FD_TEST( tiny_http_mem );
  gui->http = fd_http_server_join( fd_http_server_new( tiny_http_mem, tiny_params, callbacks, NULL ) );
  FD_TEST( gui->http );
  char const tiny_request[] =
    "{\"topic\":\"timeline\",\"key\":\"query_txn_meta\",\"id\":44,"
    "\"params\":{\"start_ns\":\"300000000000\",\"end_ns\":\"300000000001\"}}";
  FD_TEST( fd_gui_ws_message( gui, 0UL, (uchar const *)tiny_request, sizeof(tiny_request)-1UL )==
           FD_HTTP_SERVER_CONNECTION_CLOSE_EVICTED );
  FD_TEST( !gui->http->stage_err && !gui->http->stage_len );
  fd_http_server_delete( fd_http_server_leave( gui->http ) );
  free( tiny_http_mem );
  gui->http = normal_http;

  FD_TEST( fd_alloc_delete( fd_alloc_leave( gui->alloc ) )==alloc_mem );
  fd_wksp_free_laddr( alloc_mem );
  fd_wksp_delete_anonymous( wksp );
  fd_http_server_delete( fd_http_server_leave( gui->http ) );
  free( http_mem );
  gui->alloc = NULL;
  gui->http  = NULL;

  FD_LOG_NOTICE(( "test_timeline_queries: ok" ));
}

/* ---- space-pressure trigger ------------------------------------------

   The space-pressure *trigger* (high-water threshold via
   fd_gui_hist_evict_step) is intentionally not covered here: it depends on
   the backend partition sizing, which is a deliberate MVP placeholder slated
   for rework.  The eviction *mechanics* it drives are exercised above via
   fd_gui_hist_evict_oldest. */

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  /* cascade mechanics: a generous (1 GiB) map so writes never hit map-full;
     eviction is driven directly via fd_gui_hist_evict_oldest.  Each test gets
     its own store so leftover epochs don't perturb the next (eviction now
     keeps the last epoch, so stores do not empty between tests). */
  test_store_t sa[ 1 ];
  store_open( sa, 1UL<<30, 0 );
  test_timeline_aggregates( sa->gui );
  store_close( sa );

  test_store_t sq[ 1 ];
  store_open( sq, 1UL<<30, 0 );
  test_timeline_agg_queries( sq->gui );
  store_close( sq );

  test_store_t s0[ 1 ];
  store_open( s0, 1UL<<30, 0 );
  test_evict_oldest_epoch( s0->gui );
  store_close( s0 );

  test_store_t s1[ 1 ];
  store_open( s1, 1UL<<30, 2 );
  test_evict_large_batch( s1->gui );
  store_close( s1 );

  test_store_t so[ 1 ];
  store_open( so, 1UL<<30, 7 );
  test_timeline_day_order( so->gui );
  store_close( so );

  test_store_t sp[ 1 ];
  store_open( sp, 1UL<<30, 6 );
  test_current_epoch_protected( sp->gui );
  store_close( sp );

  test_store_t s2[ 1 ];
  store_open( s2, 1UL<<30, 3 );
  test_evict_ts_oldest_fallback( s2->gui );
  store_close( s2 );

  test_store_t s3[ 1 ];
  store_open( s3, 1UL<<30, 4 );
  test_resident_meta_mutation_survives_evict( s3->gui );
  store_close( s3 );

  test_store_t s4[ 1 ];
  store_open( s4, 1UL<<30, 5 );
  test_epoch_region_reclaimed( s4->gui );
  store_close( s4 );

  test_store_t s5[ 1 ];
  store_open( s5, 1UL<<30, 7 );
  test_timeline_queries( s5->gui );
  store_close( s5 );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}
