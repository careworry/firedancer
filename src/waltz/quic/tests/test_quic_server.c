#include "../fd_quic.h"
#include "fd_quic_test_helpers.h"

int
main( int argc, char ** argv ) {
  fd_boot( &argc, &argv );
  fd_quic_test_boot( &argc, &argv );

  fd_rng_t _rng[1]; fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, 0U, 0UL ) );

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>=fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz = fd_env_strip_cmdline_cstr  ( &argc, &argv, "--page-sz",  NULL, "gigantic"                 );
  ulong        page_cnt = fd_env_strip_cmdline_ulong ( &argc, &argv, "--page-cnt", NULL, 2UL                        );
  ulong        numa_idx = fd_env_strip_cmdline_ulong ( &argc, &argv, "--numa-idx", NULL, fd_shmem_numa_idx(cpu_idx) );

  ulong page_sz = fd_cstr_to_shmem_page_sz( _page_sz );
  if( FD_UNLIKELY( !page_sz ) ) FD_LOG_ERR(( "unsupported --page-sz" ));

  fd_quic_limits_t quic_limits = {0};
  fd_quic_limits_from_env( &argc, &argv, &quic_limits);

  FD_LOG_NOTICE(( "Creating workspace with --page-cnt %lu --page-sz %s pages on --numa-idx %lu", page_cnt, _page_sz, numa_idx ));
  fd_wksp_t * wksp = fd_wksp_new_anonymous( page_sz, page_cnt, fd_shmem_cpu_idx( numa_idx ), "wksp", 0UL );
  FD_TEST( wksp );

  FD_LOG_NOTICE(( "Creating server QUIC" ));
  fd_quic_t * quic = fd_quic_new_anonymous( wksp, &quic_limits, FD_QUIC_ROLE_SERVER, rng );
  FD_TEST( quic );

  fd_aio_t const * aio_rx = fd_quic_get_aio_net_rx( quic );
  if( fd_quic_test_pcap ) {
    FD_TEST( 1UL==fd_aio_pcapng_start_l3( fd_quic_test_pcap ) );
    static fd_aio_pcapng_t pcap_rx[1];
    FD_TEST( fd_aio_pcapng_join( pcap_rx, aio_rx, fd_quic_test_pcap ) );
    aio_rx = fd_aio_pcapng_get_aio( pcap_rx );
  }

  fd_quic_udpsock_t _udpsock[1];
  fd_quic_udpsock_t * udpsock = fd_quic_udpsock_create( _udpsock, &argc, &argv, wksp, aio_rx );
  FD_TEST( udpsock );

  /* Transport params:
       original_destination_connection_id (0x00)         :   len(0)
       max_idle_timeout (0x01)                           : * 60000
       stateless_reset_token (0x02)                      :   len(0)
       max_udp_payload_size (0x03)                       :   0
       initial_max_data (0x04)                           : * 1048576
       initial_max_stream_data_bidi_local (0x05)         : * 1048576
       initial_max_stream_data_bidi_remote (0x06)        : * 1048576
       initial_max_stream_data_uni (0x07)                : * 1048576
       initial_max_streams_bidi (0x08)                   : * 128
       initial_max_streams_uni (0x09)                    : * 128
       ack_delay_exponent (0x0a)                         : * 3
       max_ack_delay (0x0b)                              : * 25
       disable_active_migration (0x0c)                   :   0
       preferred_address (0x0d)                          :   len(0)
       active_connection_id_limit (0x0e)                 : * 8
       initial_source_connection_id (0x0f)               : * len(8) ec 73 1b 41 a0 d5 c6 fe
       retry_source_connection_id (0x10)                 :   len(0) */

  fd_quic_config_t * quic_config = &quic->config;
  FD_TEST( quic_config );

  quic_config->role = FD_QUIC_ROLE_SERVER;
  quic_config->retry = 0;
  FD_TEST( fd_quic_config_from_env( &argc, &argv, quic_config ) );

  fd_aio_t const * aio_tx = udpsock->aio;
  if( fd_quic_test_pcap ) {
    static fd_aio_pcapng_t pcap_tx[1];
    FD_TEST( fd_aio_pcapng_join( pcap_tx, aio_tx, fd_quic_test_pcap ) );
    aio_tx = fd_aio_pcapng_get_aio( pcap_tx );
  }
  fd_quic_set_aio_net_tx( quic, aio_tx );

  quic->cb.stream_rx = NULL;
  FD_TEST( fd_quic_init( quic ) );

  /* TODO support pcap if requested */

  /* do general processing */
  FD_LOG_NOTICE(( "Running" ));
  while(1) {
    fd_quic_service( quic );
    fd_quic_udpsock_service( udpsock );
  }

  FD_TEST( fd_quic_fini( quic ) );

  fd_wksp_free_laddr( fd_quic_delete( fd_quic_leave( quic ) ) );
  fd_quic_udpsock_destroy( udpsock );
  fd_wksp_delete_anonymous( wksp );
  fd_rng_delete( fd_rng_leave( rng ) );

  FD_LOG_NOTICE(( "pass" ));
  fd_quic_test_halt();
  fd_halt();
  return 0;
}
