
#include "platform_conf.h"
#ifdef BUILD_WEB_SERVER

#include "type.h"
#include "uip.h"
#include "uip_arp.h"
#include "platform.h"
#include "utils.h"
#include "uip-split.h"
#include "dhcpc.h"
#include "resolv.h"
#include <string.h>

// *****************************************************************************
// Platform independenet eLua UIP "main loop" implementation

// Timers
static u32 periodic_timer, arp_timer;

// Macro for accessing the Ethernet header information in the buffer.
#define BUF                     ((struct uip_eth_hdr *)&uip_buf[0])

// UIP Timers (in ms)
#define UIP_PERIODIC_TIMER_MS   500
#define UIP_ARP_TIMER_MS        10000

#define IP_TCP_HEADER_LENGTH 40
#define TOTAL_HEADER_LENGTH (IP_TCP_HEADER_LENGTH+UIP_LLH_LEN)

// This gets called on both Ethernet RX interrupts and timer requests,
// but it's called only from the Ethernet interrupt handler
void httpd_uip_mainloop()
{
  u32 temp, packet_len;

  // Increment uIP timers
  temp = platform_eth_get_elapsed_time();
  periodic_timer += temp;
  arp_timer += temp;

  // Check for an RX packet and read it
  if( ( packet_len = platform_eth_get_packet_nb( uip_buf, sizeof( uip_buf ) ) ) > 0 )
  {
    // Set uip_len for uIP stack usage.
    uip_len = ( unsigned short )packet_len;

    // Process incoming IP packets here.
    if( BUF->type == htons( UIP_ETHTYPE_IP ) )
    {
      uip_arp_ipin();
      uip_input();

      // If the above function invocation resulted in data that
      // should be sent out on the network, the global variable
      // uip_len is set to a value > 0.
      if( uip_len > 0 )
      {
        uip_arp_out();
        platform_eth_send_packet( uip_buf, uip_len, TRUE);
      }
    }

    // Process incoming ARP packets here.
    else if( BUF->type == htons( UIP_ETHTYPE_ARP ) )
    {
      uip_arp_arpin();

      // If the above function invocation resulted in data that
      // should be sent out on the network, the global variable
      // uip_len is set to a value > 0.
      if( uip_len > 0 )
    	platform_eth_send_packet( uip_buf, uip_len, TRUE);
    }
  }
  else if( periodic_timer >= UIP_PERIODIC_TIMER_MS )
  {
    periodic_timer = 0;
    for( temp = 0; temp < UIP_CONNS; temp ++ )
    {
      uip_periodic( temp );

      // If the above function invocation resulted in data that
      // should be sent out on the network, the global variable
      // uip_len is set to a value > 0.
      if( uip_len > 0 )
      {
        uip_arp_out();
        platform_eth_send_packet( uip_buf, uip_len, TRUE);
      }
    }

#if UIP_UDP
    for( temp = 0; temp < UIP_UDP_CONNS; temp ++ )
    {
      uip_udp_periodic( temp );

      // If the above function invocation resulted in data that
      // should be sent out on the network, the global variable
      // uip_len is set to a value > 0.
      if( uip_len > 0 )
      {
        uip_arp_out();
        platform_eth_send_packet( uip_buf, uip_len, TRUE);
      }
    }
#endif // UIP_UDP

    // Process ARP Timer here.
    if( arp_timer >= UIP_ARP_TIMER_MS )
    {
      arp_timer = 0;
      uip_arp_timer();
    }
  }
}

// *****************************************************************************
// UIP UDP application (used for the DHCP client and the DNS resolver)

void http_uip_udp_appcall()
{
  resolv_appcall();
  dhcpc_appcall();
}
static void http_uip_conf_static()
{
  uip_ipaddr_t ipaddr;
  uip_ipaddr( ipaddr, ELUA_CONF_IPADDR0, ELUA_CONF_IPADDR1, ELUA_CONF_IPADDR2, ELUA_CONF_IPADDR3 );
  uip_sethostaddr( ipaddr );
  uip_ipaddr( ipaddr, ELUA_CONF_NETMASK0, ELUA_CONF_NETMASK1, ELUA_CONF_NETMASK2, ELUA_CONF_NETMASK3 );
  uip_setnetmask( ipaddr );
  uip_ipaddr( ipaddr, ELUA_CONF_DEFGW0, ELUA_CONF_DEFGW1, ELUA_CONF_DEFGW2, ELUA_CONF_DEFGW3 );
  uip_setdraddr( ipaddr );
  uip_ipaddr( ipaddr, ELUA_CONF_DNS0, ELUA_CONF_DNS1, ELUA_CONF_DNS2, ELUA_CONF_DNS3 );
  resolv_conf( ipaddr );
}

// Init application
void http_uip_init( const struct uip_eth_addr *paddr )
{
  // Set hardware address
  uip_setethaddr( (*paddr) );

  // Initialize the uIP TCP/IP stack.
  uip_init();
  uip_arp_init();

#ifdef BUILD_DHCPC
  dhcpc_init( paddr->addr, sizeof( *paddr ) );
  dhcpc_request();
#else
  http_uip_conf_static();
#endif

  resolv_init();

  uip_listen( HTONS( 80 ) );

}
#ifdef BUILD_DNS
volatile static int elua_resolv_req_done;
static elua_net_ip elua_resolv_ip;

void resolv_found( char *name, u16_t *ipaddr )
{
  if( !ipaddr )
    elua_resolv_ip.ipaddr = 0;
  else
  {
    elua_resolv_ip.ipwords[ 0 ] = ipaddr[ 0 ];
    elua_resolv_ip.ipwords[ 1 ] = ipaddr[ 1 ];
  }
  elua_resolv_req_done = 1;
}

#endif
#ifdef BUILD_DHCPC

void dhcpc_configured(const struct dhcpc_state *s)
{
  if( s->ipaddr[ 0 ] != 0 )
  {
    uip_sethostaddr( s->ipaddr );
    uip_setnetmask( s->netmask );
    uip_setdraddr( s->default_router );
    resolv_conf( ( u16_t* )s->dnsaddr );
  }
  else
	  http_uip_conf_static();
}
#endif

#endif

