// Platform-dependent functions

#include "platform.h"
#include "type.h"
#include "devman.h"
#include "genstd.h"
#include "stacks.h"
#include <reent.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "utils.h"
#include "platform_conf.h"
#include "common.h"
#include "buf.h"
#include "spi.h"
#ifdef BUILD_MMCFS
#include "diskio.h"
#endif

#if defined(BUILD_UIP) || defined(BUILD_WEB_SERVER)
#include "ethernet.h"
#include "uip-conf.h"
#include "uip_arp.h"
#endif
#ifdef  BUILD_UIP
#include "elua_uip.h"
#endif

// Platform-specific includes
#include <avr32/io.h>
#include "compiler.h"
#include "flashc.h"
#include "pm.h"
#include "board.h"
#include "usart.h"
#include "gpio.h"
#include "tc.h"
#include "intc.h"

// UIP sys tick data
// NOTE: when using virtual timers, SYSTICKHZ and VTMR_FREQ_HZ should have the
// same value, as they're served by the same timer (the systick)
#define SYSTICKHZ               4
#define SYSTICKMS               (1000 / SYSTICKHZ)

#if defined(BUILD_UIP) || defined(BUILD_WEB_SERVER)
static int eth_timer_fired;
#endif

// ****************************************************************************
// Platform initialization
#if defined(BUILD_UIP) || defined(BUILD_WEB_SERVER)
u32  platform_ethernet_setup(void);
#endif

extern int pm_configure_clocks( pm_freq_param_t *param );

static u32 platform_timer_set_clock( unsigned id, u32 clock );

// Virtual timers support
#if VTMR_NUM_TIMERS > 0
#define VTMR_CH     (2)

__attribute__((__interrupt__)) static void tmr_int_handler()
{
  volatile avr32_tc_t *tc = &AVR32_TC;
  
  tc_read_sr( tc, VTMR_CH );
  cmn_virtual_timer_cb();

#ifdef BUILD_MMCFS
  disk_timerproc();
#endif


#if defined(BUILD_UIP) || defined(BUILD_WEB_SERVER)
  // Indicate that a SysTick interrupt has occurred.
  eth_timer_fired = 1;
#endif
#ifdef BUILD_UIP
  // Generate a fake Ethernet interrupt.  This will perform the actual work
  // of incrementing the timers and taking the appropriate actions.
  platform_eth_force_interrupt();
#endif
}                                
#endif

const u32 uart_base_addr[ ] = { 
    AVR32_USART0_ADDRESS, 
    AVR32_USART1_ADDRESS, 
    AVR32_USART2_ADDRESS, 
#ifdef AVR32_USART3_ADDRESS
    AVR32_USART3_ADDRESS,
#endif    
};

extern void alloc_init();

int platform_init()
{
  pm_freq_param_t pm_freq_param =
  {
    REQ_CPU_FREQ,
    REQ_PBA_FREQ,
    FOSC0,
    OSC0_STARTUP, 
  };
  tc_waveform_opt_t tmropt = 
  {
    0,                                 // Channel selection.
        
    TC_EVT_EFFECT_NOOP,                // Software trigger effect on TIOB.
    TC_EVT_EFFECT_NOOP,                // External event effect on TIOB.
    TC_EVT_EFFECT_NOOP,                // RC compare effect on TIOB.
    TC_EVT_EFFECT_NOOP,                // RB compare effect on TIOB.
                        
    TC_EVT_EFFECT_NOOP,                // Software trigger effect on TIOA.
    TC_EVT_EFFECT_NOOP,                // External event effect on TIOA.
    TC_EVT_EFFECT_NOOP,                // RC compare effect on TIOA: toggle.
    TC_EVT_EFFECT_NOOP,                // RA compare effect on TIOA: toggle (other possibilities are none, set and clear).
                                        
    TC_WAVEFORM_SEL_UP_MODE,           // Waveform selection: Up mode
    FALSE,                             // External event trigger enable.
    0,                                 // External event selection.
    TC_SEL_NO_EDGE,                    // External event edge selection.
    FALSE,                             // Counter disable when RC compare.
    FALSE,                             // Counter clock stopped with RC compare.
                                                                
    FALSE,                             // Burst signal selection.
    FALSE,                             // Clock inversion.
    TC_CLOCK_SOURCE_TC1                // Internal source clock 1 (32768Hz)
  };
  volatile avr32_tc_t *tc = &AVR32_TC;
  unsigned i;
         
  Disable_global_interrupt();  
  INTC_init_interrupts();
    
  // Setup clocks
  if( PM_FREQ_STATUS_FAIL == pm_configure_clocks( &pm_freq_param ) )
    return PLATFORM_ERR;  
  // Select the 32-kHz oscillator crystal
  pm_enable_osc32_crystal (&AVR32_PM );
  // Enable the 32-kHz clock
  pm_enable_clk32_no_wait( &AVR32_PM, AVR32_PM_OSCCTRL32_STARTUP_0_RCOSC );    
  
  // Initialize external memory if any.
#ifdef AVR32_SDRAMC
  sdramc_init( REQ_CPU_FREQ );
#endif 
      
  // Setup timers
  for( i = 0; i < 3; i ++ )
  {
    tmropt.channel = i;
    tc_init_waveform( tc, &tmropt );
#ifndef FOSC32
    // At reset, timers run from the 32768Hz crystal. If there is no such clock
    // then run them all at the lowest frequency available (PBA_FREQ / 128)
    platform_timer_set_clock( i, REQ_PBA_FREQ / 128 );
#endif
  }
  
  // Setup timer interrupt for the virtual timers if needed
#if VTMR_NUM_TIMERS > 0
  INTC_register_interrupt( &tmr_int_handler, AVR32_TC_IRQ2, AVR32_INTC_INT0 );  
  tmropt.channel = VTMR_CH;
  tmropt.wavsel = TC_WAVEFORM_SEL_UP_MODE_RC_TRIGGER;
  tc_init_waveform( tc, &tmropt );
  tc_interrupt_t tmrint = 
  {
    0,              // External trigger interrupt.
    0,              // RB load interrupt.
    0,              // RA load interrupt.
    1,              // RC compare interrupt.
    0,              // RB compare interrupt.
    0,              // RA compare interrupt.
    0,              // Load overrun interrupt.
    0               // Counter overflow interrupt.
  };
# ifdef FOSC32
  tc_write_rc( tc, VTMR_CH, FOSC32 / VTMR_FREQ_HZ );
# else
  // Run VTMR from the slowest available PBA clock divisor
  { u32 vt_clock_freq = platform_timer_set_clock( VTMR_CH, REQ_PBA_FREQ / 128 );
    u32 div = vt_clock_freq / VTMR_FREQ_HZ;
    if (div > 0xffff) div = 0xffff;
    tc_write_rc( tc, VTMR_CH, div );
  }
# endif
  tc_configure_interrupts( tc, VTMR_CH, &tmrint );
  Enable_global_interrupt();
  tc_start( tc, VTMR_CH );  
#endif

    // Setup spi controller(s) : up to 4 slave by controller.
#if NUM_SPI > 0
    spi_master_options_t spiopt;
    spiopt.modfdis = TRUE;
    spiopt.pcs_decode = FALSE;
    spiopt.delay = 0;
    spi_initMaster(&AVR32_SPI0, &spiopt, REQ_CPU_FREQ);
    
#if NUM_SPI > 4
    spi_initMaster(&AVR32_SPI1, &spiopt, REQ_CPU_FREQ);
#endif    

#endif

#if defined(BUILD_UIP) || defined(BUILD_WEB_SERVER)
    platform_ethernet_setup();
#endif

  cmn_platform_init();
#ifdef ELUA_BOARD_MIZAR32
  platform_pio_op( 0, ( pio_type )1 << 0 , PLATFORM_IO_PIN_PULLUP );
#endif
    
  // All done  
  return PLATFORM_OK;
} 

// ****************************************************************************
// PIO functions

// Reg types for our helper function
#define PIO_REG_PVR   0
#define PIO_REG_OVR   1
#define PIO_REG_GPER  2
#define PIO_REG_ODER  3
#define PIO_REG_PUER  4

#define GPIO          AVR32_GPIO

// Helper function: for a given port, return the address of a specific register (value, direction, pullup ...)
static volatile unsigned long* platform_pio_get_port_reg_addr( unsigned port, int regtype )
{
  volatile avr32_gpio_port_t *gpio_port = &GPIO.port[ port ];
  
  switch( regtype )
  {
    case PIO_REG_PVR:
      return ( unsigned long * )&gpio_port->pvr;
    case PIO_REG_OVR:
      return &gpio_port->ovr;
    case PIO_REG_GPER:
      return &gpio_port->gper;
    case PIO_REG_ODER:
      return &gpio_port->oder;
    case PIO_REG_PUER:
      return &gpio_port->puer;
  }
  // Should never get here
  return ( unsigned long* )&gpio_port->pvr;
}

// Helper function: get port value, get direction, get pullup, ...
static pio_type platform_pio_get_port_reg( unsigned port, int reg )
{
  volatile unsigned long *pv = platform_pio_get_port_reg_addr( port, reg );
  
  return *pv;
}

// Helper function: set port value, set direction, set pullup ...
static void platform_pio_set_port_reg( unsigned port, pio_type val, int reg )
{
  volatile unsigned long *pv = platform_pio_get_port_reg_addr( port, reg );
    
  *pv = val;
}

pio_type platform_pio_op( unsigned port, pio_type pinmask, int op )
{
  pio_type retval = 1;
  
  switch( op )
  {
    case PLATFORM_IO_PORT_SET_VALUE:    
      platform_pio_set_port_reg( port, pinmask, PIO_REG_OVR );
      break;
      
    case PLATFORM_IO_PIN_SET:
      platform_pio_set_port_reg( port, platform_pio_get_port_reg( port, PIO_REG_OVR ) | pinmask, PIO_REG_OVR );
      break;
      
    case PLATFORM_IO_PIN_CLEAR:
      platform_pio_set_port_reg( port, platform_pio_get_port_reg( port, PIO_REG_OVR ) & ~pinmask, PIO_REG_OVR );
      break;
      
    case PLATFORM_IO_PORT_DIR_INPUT:
      pinmask = 0xFFFFFFFF;      
    case PLATFORM_IO_PIN_DIR_INPUT:
      platform_pio_set_port_reg( port, platform_pio_get_port_reg( port, PIO_REG_ODER ) & ~pinmask, PIO_REG_ODER );
      platform_pio_set_port_reg( port, platform_pio_get_port_reg( port, PIO_REG_GPER ) | pinmask, PIO_REG_GPER );
      break;
      
    case PLATFORM_IO_PORT_DIR_OUTPUT:      
      pinmask = 0xFFFFFFFF;
    case PLATFORM_IO_PIN_DIR_OUTPUT:
      platform_pio_set_port_reg( port, platform_pio_get_port_reg( port, PIO_REG_ODER ) | pinmask, PIO_REG_ODER );
      platform_pio_set_port_reg( port, platform_pio_get_port_reg( port, PIO_REG_GPER ) | pinmask, PIO_REG_GPER );    
      break;      
            
    case PLATFORM_IO_PORT_GET_VALUE:
      retval = platform_pio_get_port_reg( port, pinmask == PLATFORM_IO_READ_IN_MASK ? PIO_REG_PVR : PIO_REG_OVR );
      break;
      
    case PLATFORM_IO_PIN_GET:
      retval = platform_pio_get_port_reg( port, PIO_REG_PVR ) & pinmask ? 1 : 0;
      break;
      
    case PLATFORM_IO_PIN_PULLUP:
      platform_pio_set_port_reg( port, platform_pio_get_port_reg( port, PIO_REG_PUER ) | pinmask, PIO_REG_PUER );
      break;
      
    case PLATFORM_IO_PIN_NOPULL:
      platform_pio_set_port_reg( port, platform_pio_get_port_reg( port, PIO_REG_PUER ) & ~pinmask, PIO_REG_PUER );    
      break;
      
    default:
      retval = 0;
      break;
  }
  return retval;
}

// ****************************************************************************
// UART functions


static const gpio_map_t uart_pins = 
{
  // UART 0
  { AVR32_USART0_RXD_0_0_PIN, AVR32_USART0_RXD_0_0_FUNCTION },
  { AVR32_USART0_TXD_0_0_PIN, AVR32_USART0_TXD_0_0_FUNCTION },
  
  // UART 1
  { AVR32_USART1_RXD_0_0_PIN, AVR32_USART1_RXD_0_0_FUNCTION },
  { AVR32_USART1_TXD_0_0_PIN, AVR32_USART1_TXD_0_0_FUNCTION },
  
  // UART 2
  { AVR32_USART2_RXD_0_0_PIN, AVR32_USART2_RXD_0_0_FUNCTION },
  { AVR32_USART2_TXD_0_0_PIN, AVR32_USART2_TXD_0_0_FUNCTION },
  
#ifdef AVR32_USART3_ADDRESS  
  // UART 3
  { AVR32_USART3_RXD_0_0_PIN, AVR32_USART3_RXD_0_0_FUNCTION },
  { AVR32_USART3_TXD_0_0_PIN, AVR32_USART3_TXD_0_0_FUNCTION },
#endif  
};

u32 platform_uart_setup( unsigned id, u32 baud, int databits, int parity, int stopbits )
{
  volatile avr32_usart_t *pusart = ( volatile avr32_usart_t* )uart_base_addr[ id ];  
  usart_options_t opts;
  
  opts.channelmode = USART_NORMAL_CHMODE;
  opts.charlength = databits;
  opts.baudrate = baud;
  
  // Set stopbits
  if( stopbits == PLATFORM_UART_STOPBITS_1 )
    opts.stopbits = USART_1_STOPBIT;
  else if( stopbits == PLATFORM_UART_STOPBITS_1_5 )
    opts.stopbits = USART_1_5_STOPBITS;
  else
    opts.stopbits = USART_2_STOPBITS;    
    
  // Set parity
  if( parity == PLATFORM_UART_PARITY_EVEN )
    opts.paritytype = USART_EVEN_PARITY;
  else if( parity == PLATFORM_UART_PARITY_ODD )
    opts.paritytype = USART_ODD_PARITY;
  else
    opts.paritytype = USART_NO_PARITY;  
    
  // Set actual interface
  gpio_enable_module(uart_pins + id * 2, 2 );
  usart_init_rs232( pusart, &opts, REQ_PBA_FREQ );  
  
  // [TODO] Return actual baud here
  return baud;
}

void platform_s_uart_send( unsigned id, u8 data )
{
  volatile avr32_usart_t *pusart = ( volatile avr32_usart_t* )uart_base_addr[ id ];  
  
  while( !usart_tx_ready( pusart ) );
  pusart->thr = ( data << AVR32_USART_THR_TXCHR_OFFSET ) & AVR32_USART_THR_TXCHR_MASK;
}    

int platform_s_uart_recv( unsigned id, s32 timeout )
{
  volatile avr32_usart_t *pusart = ( volatile avr32_usart_t* )uart_base_addr[ id ];  
  int temp;

  if( timeout == 0 )
  {
    if( usart_read_char( pusart, &temp ) != USART_SUCCESS )
      return -1;
    else
      return temp;
  }
  else
    return usart_getchar( pusart );
}

typedef struct
{
  u8 pin;
  u8 function;
} gpio_pin_data;

// This is a complete hack and it will stay like this until eLua will be able
// to specify what pins to use for a peripheral at runtime
static const gpio_pin_data uart_flow_control_pins[] = 
{
#ifdef AVR32_USART0_RTS_0_0_PIN
  // UART 0
  { AVR32_USART0_RTS_0_0_PIN, AVR32_USART0_RTS_0_0_FUNCTION },
  { AVR32_USART0_CTS_0_0_PIN, AVR32_USART0_CTS_0_0_FUNCTION },
#else  
// UART 0
  { AVR32_USART0_RTS_0_PIN, AVR32_USART0_RTS_0_FUNCTION },
  { AVR32_USART0_CTS_0_PIN, AVR32_USART0_CTS_0_FUNCTION },
#endif
  
  // UART 1
  { AVR32_USART1_RTS_0_0_PIN, AVR32_USART1_RTS_0_0_FUNCTION },
  { AVR32_USART1_CTS_0_0_PIN, AVR32_USART1_CTS_0_0_FUNCTION },


#ifdef AVR32_USART2_RTS_0_0_PIN
  // UART 2
  { AVR32_USART2_RTS_0_0_PIN, AVR32_USART2_RTS_0_0_FUNCTION },
  { AVR32_USART2_CTS_0_0_PIN, AVR32_USART2_CTS_0_0_FUNCTION },
#else
   // UART 2
  { AVR32_USART2_RTS_0_PIN, AVR32_USART2_RTS_0_FUNCTION },
  { AVR32_USART2_CTS_0_PIN, AVR32_USART2_CTS_0_FUNCTION },
#endif
 
#ifdef AVR32_USART3_ADDRESS  
  // UART 3
  { AVR32_USART3_RTS_0_0_PIN, AVR32_USART3_RTS_0_0_FUNCTION },
  { AVR32_USART3_CTS_0_0_PIN, AVR32_USART3_CTS_0_0_FUNCTION },
#endif  
};


int platform_s_uart_set_flow_control( unsigned id, int type )
{
  unsigned i;
  volatile avr32_usart_t *pusart = ( volatile avr32_usart_t* )uart_base_addr[ id ];
  volatile avr32_gpio_port_t *gpio_port;
  const gpio_pin_data *ppindata = uart_flow_control_pins + id * 2;

  // AVR32 only supports combined RTS/CTS flow control
  if( type != PLATFORM_UART_FLOW_NONE && type != ( PLATFORM_UART_FLOW_RTS | PLATFORM_UART_FLOW_CTS ) )
    return PLATFORM_ERR;
  // Set UART mode register first
  pusart->mr &= ~AVR32_USART_MR_MODE_MASK;
  pusart->mr |= ( type == PLATFORM_UART_FLOW_NONE ? AVR32_USART_MR_MODE_NORMAL : AVR32_USART_MR_MODE_HARDWARE ) << AVR32_USART_MR_MODE_OFFSET;
  // Then set GPIO pins
  for( i = 0; i < 2; i ++, ppindata ++ )  
    if( type != PLATFORM_UART_FLOW_NONE ) // enable pin for UART functionality
      gpio_enable_module_pin( ppindata->pin, ppindata->function );
    else // release pin to GPIO module
    {
      gpio_port = &GPIO.port[ ppindata->pin >> 5 ];
      gpio_port->gpers = 1 << ( ppindata->pin & 0x1F );
    }
  return PLATFORM_OK;
}

// ****************************************************************************
// Timer functions

static const u16 clkdivs[] = { 0xFFFF, 2, 8, 32, 128 };

// Helper: get timer clock
static u32 platform_timer_get_clock( unsigned id )
{
  volatile avr32_tc_t *tc = &AVR32_TC;
  unsigned int clksel = tc->channel[ id ].CMR.waveform.tcclks;
        
#ifdef FOSC32
  return clksel == 0 ? FOSC32 : REQ_PBA_FREQ / clkdivs[ clksel ];
#else
  return REQ_PBA_FREQ / clkdivs[ clksel ];
#endif
}

// Helper: set timer clock
static u32 platform_timer_set_clock( unsigned id, u32 clock )
{
  unsigned i, mini;
  volatile avr32_tc_t *tc = &AVR32_TC;
  volatile unsigned long *pclksel = &tc->channel[ id ].cmr;
  
#ifdef FOSC32
  for( i = mini = 0; i < 5; i ++ )
    if( ABSDIFF( clock, i == 0 ? FOSC32 : REQ_PBA_FREQ / clkdivs[ i ] ) <
        ABSDIFF( clock, mini == 0 ? FOSC32 : REQ_PBA_FREQ / clkdivs[ mini ] ) )
      mini = i;
  *pclksel = ( *pclksel & ~0x07 ) | mini;
  return mini == 0 ? FOSC32 : REQ_PBA_FREQ / clkdivs[ mini ];
#else
  // There is no 32768Hz clock so choose from the divisors of PBA.
  for( i = mini = 1; i < 5; i ++ )
    if( ABSDIFF( clock, REQ_PBA_FREQ / clkdivs[ i ] ) <
        ABSDIFF( clock, REQ_PBA_FREQ / clkdivs[ mini ] ) )
      mini = i;
  *pclksel = ( *pclksel & ~0x07 ) | mini;
  return REQ_PBA_FREQ / clkdivs[ mini ];
#endif
}

void platform_s_timer_delay( unsigned id, u32 delay_us )
{
  volatile avr32_tc_t *tc = &AVR32_TC;  
  u32 freq;
  timer_data_type final;
  volatile int i;
  volatile const avr32_tc_sr_t *sr = &tc->channel[ id ].SR;
      
  freq = platform_timer_get_clock( id );
  final = ( ( u64 )delay_us * freq ) / 1000000;
  if( final > 0xFFFF )
    final = 0xFFFF;
  tc_start( tc, id );
  i = sr->covfs;
  for( i = 0; i < 200; i ++ );
  while( ( tc_read_tc( tc, id ) < final ) && !sr->covfs );  
}

u32 platform_s_timer_op( unsigned id, int op, u32 data )
{
  u32 res = 0;
  volatile int i;
  volatile avr32_tc_t *tc = &AVR32_TC;    
  
  switch( op )
  {
    case PLATFORM_TIMER_OP_START:
      res = 0;
      tc_start( tc, id );
      for( i = 0; i < 200; i ++ );      
      break;
      
    case PLATFORM_TIMER_OP_READ:
      res = tc_read_tc( tc, id );
      break;
      
    case PLATFORM_TIMER_OP_GET_MAX_DELAY:
      res = platform_timer_get_diff_us( id, 0, 0xFFFF );
      break;
      
    case PLATFORM_TIMER_OP_GET_MIN_DELAY:
      res = platform_timer_get_diff_us( id, 0, 1 );
      break;      
      
    case PLATFORM_TIMER_OP_SET_CLOCK:
      res = platform_timer_set_clock( id, data );
      break;
      
    case PLATFORM_TIMER_OP_GET_CLOCK:
      res = platform_timer_get_clock( id );
      break;
  }
  return res;
}

// ****************************************************************************
// SPI functions

/* Note about AVR32 SPI
 *
 * Each controller can handle up to 4 different settings.
 * Here, for convenience, we don't use the builtin chip select lines, 
 * it's up to the user to drive the corresponding GPIO lines.
 *
*/
static const gpio_map_t spi_pins = 
{
  // SPI0
  { BOARD_SPI0_SCK_PIN, BOARD_SPI0_SCK_PIN_FUNCTION },
  { BOARD_SPI0_MISO_PIN, BOARD_SPI0_MISO_PIN_FUNCTION },
  { BOARD_SPI0_MOSI_PIN, BOARD_SPI0_MOSI_PIN_FUNCTION },
  { BOARD_SPI0_CS_PIN, BOARD_SPI0_CS_PIN_FUNCTION },
  
  // SPI1
#if NUM_SPI > 4  
  { BOARD_SPI1_SCK_PIN, BOARD_SPI1_SCK_PIN_FUNCTION },
  { BOARD_SPI1_MISO_PIN, BOARD_SPI1_MISO_PIN_FUNCTION },
  { BOARD_SPI1_MOSI_PIN, BOARD_SPI1_MOSI_PIN_FUNCTION },
  { BOARD_SPI1_CS_PIN, BOARD_SPI1_CS_PIN_FUNCTION },
#endif  
};

static const
u32 spireg[] = 
{ 
    AVR32_SPI0_ADDRESS, 
#ifdef AVR32_SPI1_ADDRESS    
    AVR32_SPI1_ADDRESS,
#endif
};

u32 platform_spi_setup( unsigned id, int mode, u32 clock, unsigned cpol, unsigned cpha, unsigned databits )
{
    spi_options_t opt;
    
    opt.baudrate = clock;
    opt.bits = min(databits, 16);
    opt.spck_delay = 0;
    opt.trans_delay = 0;
    opt.mode = ((cpol & 1) << 1) | (cpha & 1);

    // Set actual interface
    gpio_enable_module(spi_pins + (id >> 2) * 4, 4);
    spi_setupChipReg((volatile avr32_spi_t *) spireg[id >> 2], id % 4, &opt, REQ_CPU_FREQ);
    
    // TODO: return the actual baudrate.
    return clock;
}

spi_data_type platform_spi_send_recv( unsigned id, spi_data_type data )
{
    volatile avr32_spi_t * spi = (volatile avr32_spi_t *) spireg[id >> 2];
    
    /* Since none of the builtin chip select lines are externally wired,
     * spi_selectChip() just ensure that the correct spi settings are 
     * used for the transfer.
     */
    spi_selectChip(spi, id % 4);
    return spi_single_transfer(spi, (u16) data);
}

void platform_spi_select( unsigned id, int is_select )
{
  volatile avr32_spi_t * spi = (volatile avr32_spi_t *) spireg[id >> 2];

  if(is_select == PLATFORM_SPI_SELECT_ON)
    spi_selectChip(spi, id % 4);
  else
    spi_unselectChip(spi, id % 4);

}
// ****************************************************************************
// CPU functions

int platform_cpu_set_global_interrupts( int status )
{
  int previous = Is_global_interrupt_enabled();

  if( status == PLATFORM_CPU_ENABLE )
    Enable_global_interrupt();
  else
    Disable_global_interrupt();
  return previous;
}

int platform_cpu_get_global_interrupts()
{
  return Is_global_interrupt_enabled();
}


// ****************************************************************************
// Network support


#if defined(BUILD_UIP) || defined(BUILD_WEB_SERVER)
static const gpio_map_t MACB_GPIO_MAP =
{
  {AVR32_MACB_MDC_0_PIN,    AVR32_MACB_MDC_0_FUNCTION   },
  {AVR32_MACB_MDIO_0_PIN,   AVR32_MACB_MDIO_0_FUNCTION  },
  {AVR32_MACB_RXD_0_PIN,    AVR32_MACB_RXD_0_FUNCTION   },
  {AVR32_MACB_TXD_0_PIN,    AVR32_MACB_TXD_0_FUNCTION   },
  {AVR32_MACB_RXD_1_PIN,    AVR32_MACB_RXD_1_FUNCTION   },
  {AVR32_MACB_TXD_1_PIN,    AVR32_MACB_TXD_1_FUNCTION   },
  {AVR32_MACB_TX_EN_0_PIN,  AVR32_MACB_TX_EN_0_FUNCTION },
  {AVR32_MACB_RX_ER_0_PIN,  AVR32_MACB_RX_ER_0_FUNCTION },
  {AVR32_MACB_RX_DV_0_PIN,  AVR32_MACB_RX_DV_0_FUNCTION },
  {AVR32_MACB_TX_CLK_0_PIN, AVR32_MACB_TX_CLK_0_FUNCTION}
};

u32  platform_ethernet_setup()
{
	  static struct uip_eth_addr sTempAddr;
	  // Assign GPIO to MACB
	  gpio_enable_module(MACB_GPIO_MAP, sizeof(MACB_GPIO_MAP) / sizeof(MACB_GPIO_MAP[0]));

	  // initialize MACB & Phy Layers
	  if (xMACBInit(&AVR32_MACB) == FALSE ) {
		  return PLATFORM_ERR;
	  }

	  sTempAddr.addr[0] = ETHERNET_CONF_ETHADDR0;
	  sTempAddr.addr[1] = ETHERNET_CONF_ETHADDR1;
	  sTempAddr.addr[2] = ETHERNET_CONF_ETHADDR2;
	  sTempAddr.addr[3] = ETHERNET_CONF_ETHADDR3;
	  sTempAddr.addr[4] = ETHERNET_CONF_ETHADDR4;
	  sTempAddr.addr[5] = ETHERNET_CONF_ETHADDR5;

#if defined(BUILD_UIP)
	  // Initialize the eLua uIP layer
	  elua_uip_init( &sTempAddr );
#endif

#if defined(BUILD_WEB_SERVER)
	  // Initialize the uIP layer
	  http_uip_init( &sTempAddr );
#endif

	  return PLATFORM_OK;

}

void platform_eth_send_packet( const void* src, u32 size, u8 endframe )
{
   lMACBSend(&AVR32_MACB,src, size, endframe);
}

u32 platform_eth_get_packet_nb( void* buf, u32 maxlen )
{
	u32    len;

    /* Obtain the size of the packet. */
    len = ulMACBInputLength();

    if (len > maxlen) {
    	return 0;
    }

    if( len ) {
        /* Let the driver know we are going to read a new packet. */
    	vMACBRead( NULL, 0, len );
    	vMACBRead( buf, len, len );
    }

 return len;
}

#ifdef BUILD_UIP
void platform_eth_force_interrupt()
{
    elua_uip_mainloop();
}
#endif

u32 platform_eth_get_elapsed_time()
{
    if( eth_timer_fired )
    {
      eth_timer_fired = 0;
      return SYSTICKMS;
    }
    else
      return 0;
}
#endif
