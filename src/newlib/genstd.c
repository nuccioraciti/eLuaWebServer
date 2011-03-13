// Generic stdin/stdout/stderr support functions

#include "platform_conf.h"
#ifdef BUILD_CON_GENERIC

#include "type.h"
#include "devman.h"
#include "genstd.h"
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include "utils.h"
#ifdef BUILD_WEB_SERVER
#include "httpd.h"
#endif

#ifndef BUILD_WEB_SERVER
static p_std_send_char std_send_char_func;
static p_std_get_char std_get_char_func;
int std_prev_char = -1;

// 'read'
static _ssize_t std_read( struct _reent *r, int fd, void* vptr, size_t len )
{
  int i, c;
  char* ptr = ( char* )vptr;
  
  // Check pointers
  if( !std_send_char_func || !std_get_char_func )
  {
    r->_errno = EBADF;
    return -1;  
  }
  
  // Check file number
  if( ( fd < DM_STDIN_NUM ) || ( fd > DM_STDERR_NUM ) )
  {
    r->_errno = EBADF;
    return -1;
  }  
  if( fd != DM_STDIN_NUM )
  {
    r->_errno = EINVAL;
    return -1;
  }      
  
  i = 0;
  while( i < len )
  {  
    if( std_prev_char != -1 )
    {
      // We have a char from the previous run of std_read, so put it in the buffer
      ptr[ i ++ ] = ( char )std_prev_char;
      std_prev_char = -1;
      continue;
    }
    if( ( c = std_get_char_func( STD_INFINITE_TIMEOUT ) ) == -1 )
      break;
    if( ( c == 8 ) || ( c == 0x7F ) ) // Backspace
    {
      if( i > 0 )
      {
        i --;        
        std_send_char_func( DM_STDOUT_NUM, 8 );      
        std_send_char_func( DM_STDOUT_NUM, ' ' );      
        std_send_char_func( DM_STDOUT_NUM, 8 );                    
      }      
      continue;
    }
    if( !isprint( c ) && c != '\n' && c != '\r' && c != STD_CTRLZ_CODE )
      continue;
    if( c == STD_CTRLZ_CODE )
      return 0;
    std_send_char_func( DM_STDOUT_NUM, c );
    if( c == '\r' || c == '\n' )
    {
      // Handle both '\r\n' and '\n\r' here
      std_prev_char = std_get_char_func( STD_INTER_CHAR_TIMEOUT ); // consume the next char (\r or \n) if any
      if( std_prev_char + c == '\r' + '\n' ) // we must ignore this character
        std_prev_char = -1;
      std_send_char_func( DM_STDOUT_NUM, '\r' + '\n' - c );
      ptr[ i ] = '\n';
      return i + 1;
    }
    ptr[ i ++ ] = c;
  }
  return len;
}

// 'write'
static _ssize_t std_write( struct _reent *r, int fd, const void* vptr, size_t len )
{   
  int i;
  const char* ptr = ( const char* )vptr;
  
  // Check pointers
  if( !std_send_char_func || !std_get_char_func )
  {
    r->_errno = EBADF;
    return -1;  
  }
    
  // Check file number
  if( ( fd < DM_STDIN_NUM ) || ( fd > DM_STDERR_NUM ) )
  {
    r->_errno = EBADF;
    return -1;
  }    
  if( ( fd != DM_STDOUT_NUM ) && ( fd != DM_STDERR_NUM ) )
  {
    r->_errno = EINVAL;
    return -1;
  }  
  
  for( i = 0; i < len; i ++ ) 
  {
    if( ptr[ i ] == '\n' )
      std_send_char_func( fd, '\r' );
    std_send_char_func( fd, ptr[ i ] ); 
  }
  return len;
}
#endif

#ifdef BUILD_WEB_SERVER

// 'write'
static _ssize_t uip_write( struct _reent *r, int fd, const void* vptr, size_t len )
{
  const char* ptr = ( const char* )vptr;
  struct httpd_state *s;
  size_t i;

  // Check file number
  if( ( fd < DM_STDIN_NUM ) || ( fd > DM_STDERR_NUM ) )
  {
    r->_errno = EBADF;
    return -1;
  }
  if( ( fd != DM_STDOUT_NUM ) && ( fd != DM_STDERR_NUM ) )
  {
    r->_errno = EINVAL;
    return -1;
  }

  if (fd == DM_STDERR_NUM)
  {
	  http_uart_send_str(ptr, len);
	  return len;
  }

  s = get_httpd_state_struct();

  assert((s->write_buffer_len + len) < WRITE_BUFFER_SIZE);

  for(i=0; i < len; i++)
  {
	if(ptr[i] != '\n')
	  s->write_buffer[s->write_buffer_len + i] = ptr[i];
	else
	  len--;
  }

  s->write_buffer_len += len;

  return len;
}

// 'read'
static _ssize_t null_read( struct _reent *r, int fd, void* vptr, size_t len )
{
  return 0;
}
#endif

#ifndef BUILD_WEB_SERVER

// Set send/recv functions
void std_set_send_func( p_std_send_char pfunc )
{
  std_send_char_func = pfunc;
}

void std_set_get_func( p_std_get_char pfunc )
{
  std_get_char_func = pfunc;
}

// Our UART device descriptor structure
static const DM_DEVICE std_device = 
{
  STD_DEV_NAME,
  NULL,                 // open
  NULL,                 // close
  std_write,            // write
  std_read,             // read
  NULL,                 // lseek
  NULL,                 // fstat
  NULL,                 // opendir
  NULL,                 // readdir
  NULL                  // closedir
};
#endif
#ifdef BUILD_WEB_SERVER
// Our UIP (HTTP) device descriptor structure
static const DM_DEVICE std_device =
{
  STD_DEV_NAME,
  NULL,                 // open
  NULL,                 // close
  uip_write,            // write
  null_read,            // read
  NULL,                 // lseek
  NULL,                 // fstat
  NULL,                 // opendir
  NULL,                 // readdir
  NULL                  // closedir
};
#endif

const DM_DEVICE* std_get_desc()
{
  return &std_device;
}

#endif // #ifdef BUILD_CON_GENERIC
