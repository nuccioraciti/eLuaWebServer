/**
 * \addtogroup apps
 * @{
 */

/**
 * \defgroup httpd Web server
 * @{
 * The uIP web server is a very simplistic implementation of an HTTP
 * server. It can serve web pages and files from a read-only ROM
 * filesystem, and provides a very small scripting language.

 */

/**
 * \file
 *         Web server
 * \author
 *         Adam Dunkels <adam@sics.se>
 */


/*
 * Copyright (c) 2004, Adam Dunkels.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the uIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 * $Id: httpd.c,v 1.2 2006/06/11 21:46:38 adam Exp $
 */
#include "platform_conf.h"
#ifdef BUILD_WEB_SERVER

#include <stdio.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "uip.h"
#include "httpd.h"
#include "platform.h"
#include "httpd-fs.h"
#include "httpd-strings.h"

#define STATE_WAITING 0
#define STATE_OUTPUT  1

#define ISO_nl           0x0a
#define ISO_space        0x20
#define ISO_bang         0x21
#define ISO_percent      0x25
#define ISO_period       0x2e
#define ISO_slash        0x2f
#define ISO_colon        0x3a
#define ISO_minor        '<'
#define ISO_question     '?'
#define CGI_close        "?>"

#define STR_len(x)       (sizeof(x)-1)

static int       http_run_elua (struct httpd_state *, char *, int);
static void      set_httpd_state_struct(struct httpd_state *);
static           PT_THREAD(http_output_elua (struct httpd_state *));


static struct {
	  lua_State *L;
	  u16_t ripaddr[2];
} connection[WEB_MAX_CLIENT];


static struct httpd_state *g_httpd_state;

struct httpd_state *get_httpd_state_struct()
{
  return g_httpd_state;
}

void set_httpd_state_struct(struct httpd_state *s)
{
  g_httpd_state = s;
}

_ssize_t http_uart_send_str(const char *ptr, _ssize_t len)
{
  _ssize_t i;

  for( i = 0; i < len; i++ ) {
    if ( ptr[ i ] == '\n')
      platform_uart_send( CON_UART_ID, '\r' );
    platform_uart_send( CON_UART_ID, ptr[i] );
  }
  return i;
}
/*---------------------------------------------------------------------------*/
static unsigned short
generate_part_of_file(void *state)
{
  struct httpd_state *s = (struct httpd_state *)state;

  if(s->file.len > uip_mss()) {
    s->len = uip_mss();
  } else {
    s->len = s->file.len;
  }
  memcpy(uip_appdata, s->file.data, s->len);
  
  return s->len;
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(send_file(struct httpd_state *s))
{
  PSOCK_BEGIN(&s->sout);
  
  do {
    PSOCK_GENERATOR_SEND(&s->sout, generate_part_of_file, s);
    s->file.len -= s->len;
    s->file.data += s->len;
  } while(s->file.len > 0);
      
  PSOCK_END(&s->sout);
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(send_part_of_file(struct httpd_state *s))
{
  PSOCK_BEGIN(&s->sout);
  PSOCK_SEND(&s->sout, s->file.data, s->len);
  PSOCK_END(&s->sout);
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(handle_elua_scripts(struct httpd_state *s))
{
  PT_BEGIN(&s->scriptpt);

  http_run_elua(s,s->file.data, s->file.len);

  PT_WAIT_THREAD(&s->scriptpt, http_output_elua(s));
  PT_END(&s->scriptpt);
}

/*---------------------------------------------------------------------------*/
static
PT_THREAD(handle_elua_tags(struct httpd_state *s))
{
  char *ptr, *p;
  
  PT_BEGIN(&s->scriptpt);

  while(s->file.len > 0) {

    /* Check if we should start executing a script. */
    if(*s->file.data == ISO_minor && *(s->file.data + 1) == ISO_question && *(s->file.data + 2) == 'l'
    	&& *(s->file.data + 3) == 'u' && *(s->file.data + 4) == 'a')
    {
      s->scriptptr = s->file.data + 6;
      s->scriptlen = s->file.len - 6;

   	  p = strstr(s->scriptptr, CGI_close);

   	  if (p != NULL)
   	  {
   		p += STR_len(CGI_close);

   		http_run_elua(s, s->scriptptr, (p - (s->scriptptr + STR_len(CGI_close))));

   	    s->scriptlen -= (p - (s->scriptptr));
   	    s->scriptptr = p;
     	  /* The script is over, so we reset the pointers and continue
  	     sending the rest of the file. */
        s->file.data = s->scriptptr;
        s->file.len = s->scriptlen;
   	    PT_WAIT_THREAD(&s->scriptpt, http_output_elua(s)); /* this will do a return */
   	  } else
   	  {
   	    /* The script is over, so we reset the pointers and continue
	       sending the rest of the file. */
        s->file.data = s->scriptptr;
        s->file.len = s->scriptlen;
   	  }
    }
    else
    {
      /* See if we find the start of script marker in the block of HTML	 to be sent. */
      if(s->file.len > uip_mss())
 	    s->len = uip_mss();
      else
 	    s->len = s->file.len;

      if(*s->file.data == ISO_minor)
 	    ptr = strchr(s->file.data + 1, ISO_minor);
      else
 	    ptr = strchr(s->file.data, ISO_minor);

      if(ptr != NULL && ptr != s->file.data)
      {
	    s->len = (int)(ptr - s->file.data);
	    if(s->len >= uip_mss())
	      s->len = uip_mss();
      }
      PT_WAIT_THREAD(&s->scriptpt, send_part_of_file(s));
      s->file.data += s->len;
      s->file.len -= s->len;
    }
  }

  PT_END(&s->scriptpt);
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(send_headers(struct httpd_state *s, const char *statushdr))
{
  char *ptr;

  PSOCK_BEGIN(&s->sout);

  PSOCK_SEND_STR(&s->sout, statushdr);

  ptr = strrchr(s->filename, ISO_period);
  if(ptr == NULL) {
    PSOCK_SEND_STR(&s->sout, http_content_type_binary);
  } else if(strncmp(http_html, ptr, 5) == 0 || strncmp(http_pht, ptr, 4) == 0 || strncmp(http_lua, ptr, 4) == 0) {
    PSOCK_SEND_STR(&s->sout, http_content_type_html);
  } else if(strncmp(http_css, ptr, 4) == 0) {
    PSOCK_SEND_STR(&s->sout, http_content_type_css);
  } else if(strncmp(http_png, ptr, 4) == 0) {
    PSOCK_SEND_STR(&s->sout, http_content_type_png);
  } else if(strncmp(http_gif, ptr, 4) == 0) {
    PSOCK_SEND_STR(&s->sout, http_content_type_gif);
  } else if(strncmp(http_jpg, ptr, 4) == 0) {
    PSOCK_SEND_STR(&s->sout, http_content_type_jpg);
  } else {
    PSOCK_SEND_STR(&s->sout, http_content_type_plain);
  }
  PSOCK_END(&s->sout);
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(handle_output(struct httpd_state *s))
{
  char *ptr;
  
  PT_BEGIN(&s->outputpt);

  if(!httpd_fs_open(s->filename, &s->file)) {
    httpd_fs_open(http_404_html, &s->file);
    strcpy(s->filename, http_404_html);
    PT_WAIT_THREAD(&s->outputpt,
		   send_headers(s,
		   http_header_404));
    PT_WAIT_THREAD(&s->outputpt,
		   send_file(s));
  } else {
    PT_WAIT_THREAD(&s->outputpt, send_headers(s, http_header_200));
    ptr = strchr(s->filename, ISO_period);
    if(ptr != NULL && strncmp(ptr, http_pht, 4) == 0) {
      s->new_pht_page = TRUE;	/* force a new instance of the elua interpreter */
      PT_INIT(&s->scriptpt);
      PT_WAIT_THREAD(&s->outputpt, handle_elua_tags(s));
    }else if(ptr != NULL && strncmp(ptr, http_lua, 4) == 0) {
          PT_INIT(&s->scriptpt);
          PT_WAIT_THREAD(&s->outputpt, handle_elua_scripts(s));
    } else {
      PT_WAIT_THREAD(&s->outputpt,
		     send_file(s));
    }
  }
  PSOCK_CLOSE(&s->sout);
  PT_END(&s->outputpt);
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(handle_input(struct httpd_state *s))
{
  char *params;
  PSOCK_BEGIN(&s->sin);

  PSOCK_READTO(&s->sin, ISO_space);
  
  if(strncmp(s->inputbuf, http_get, 4) != 0) {
    PSOCK_CLOSE_EXIT(&s->sin);
  }
  PSOCK_READTO(&s->sin, ISO_space);

  if(s->inputbuf[0] != ISO_slash) {
    PSOCK_CLOSE_EXIT(&s->sin);
  }

  if(s->inputbuf[1] == ISO_space) {
    strncpy(s->filename, http_index_pht, sizeof(s->filename));
  } else {
    s->inputbuf[PSOCK_DATALEN(&s->sin) - 1] = 0;
    params = strchr(s->inputbuf, ISO_question);
	if( params != NULL)  {
	  *params = 0; /* change the ? character with the end of filename string */
	  strcpy(s->filename, &s->inputbuf[0]);
      strcpy(s->http_params, params + 1);
	} else {
      s->inputbuf[PSOCK_DATALEN(&s->sin) - 1] = 0;
      strncpy(s->filename, &s->inputbuf[0], sizeof(s->filename));
      s->http_params[0] = 0;
	}
  }

  s->state = STATE_OUTPUT;

  while(1) {
    PSOCK_READTO(&s->sin, ISO_nl);

    if(strncmp(s->inputbuf, http_referer, 8) == 0) {
      s->inputbuf[PSOCK_DATALEN(&s->sin) - 2] = 0;
    }
  }
  
  PSOCK_END(&s->sin);
}
/*---------------------------------------------------------------------------*/
static void
handle_connection(struct httpd_state *s)
{
  handle_input(s);
  if(s->state == STATE_OUTPUT) {
    handle_output(s);
  }
}
/*---------------------------------------------------------------------------*/
void
httpd_appcall(void)
{
  struct httpd_state *s;
  char   ripadress_isnew;
  int    i;

  s = (struct httpd_state *)&(uip_conn->appstate);

  /* save the remote ip address */
  uip_ipaddr_copy(s->ripaddr,uip_conn->ripaddr);

  s->L = NULL;
  ripadress_isnew = TRUE;

  /* Is the remote ip already present */
  for (i=0; i < WEB_MAX_CLIENT; i++)
  {
    if(uip_ipaddr_cmp(s->ripaddr,connection[i].ripaddr))
    {
      s->http_connection_nr = i;
	  s->L = connection[s->http_connection_nr].L;
	  uip_ipaddr_copy(connection[s->http_connection_nr].ripaddr,s->ripaddr);
	  ripadress_isnew = FALSE;
	  break;
    }
  }

  if(uip_closed() || uip_aborted() || uip_timedout()) {
  } else if(uip_connected()) {
    PSOCK_INIT(&s->sin, s->inputbuf, sizeof(s->inputbuf) - 1);
    PSOCK_INIT(&s->sout, s->inputbuf, sizeof(s->inputbuf) - 1);
    PT_INIT(&s->outputpt);
    s->state = STATE_WAITING;
    s->timer = 0;
    handle_connection(s);
  } else if(s != NULL) {
    if(uip_poll()) {
      ++s->timer;
      if(s->timer >= 20) {
	uip_abort();
      }
    } else {
      s->timer = 0;
    }
    handle_connection(s);
  } else {
    uip_abort();
  }

  /* new client ip */
  if(ripadress_isnew)
  {
	  /* try to insert the ip in an empty place */
	  for (i=0; i < WEB_MAX_CLIENT; i++)
	  {
		if(connection[i].ripaddr[0]==0 && connection[i].ripaddr[1]==0)
		{
          s->http_connection_nr = i;
		  uip_ipaddr_copy(connection[s->http_connection_nr].ripaddr,s->ripaddr);
		  connection[s->http_connection_nr].L = s->L;
		  break;
		}
	  }
	  /* no space free, use the an used one  */
	  if(i == WEB_MAX_CLIENT)
	  {
	    if(s->http_connection_nr < (WEB_MAX_CLIENT - 1))
	      s->http_connection_nr ++;
	    else
	      s->http_connection_nr = 0;

	    uip_ipaddr_copy(connection[s->http_connection_nr].ripaddr,s->ripaddr);
	    connection[s->http_connection_nr].L = s->L;
	   }
  }
  else
  {
	/* an old client has changed the elua struct */
    if(s->L != connection[s->http_connection_nr].L)
      connection[s->http_connection_nr].L = s->L;
  }

}

/*---------------------------------------------------------------------------*/
static
PT_THREAD(http_output_elua(struct httpd_state *s))
{
  PSOCK_BEGIN(&s->sout);
  PSOCK_SEND(&s->sout, s->write_buffer, s->write_buffer_len);
  PSOCK_END(&s->sout);
}

/*---------------------------------------------------------------------------*/
static
int http_run_elua (struct httpd_state *s, char *data, int len)
{
  int error;
  int i,c;
  char str[20];

  /* clean the elua output buffer */
  s->write_buffer_len = 0;
  /* set the http_state for elua write function */
  set_httpd_state_struct (s);

  if ((s->new_pht_page == TRUE) && (s->L != NULL)) {
	lua_close(s->L);
	s->L = NULL;
	s->new_pht_page = FALSE;
  }

  if (s->L == NULL) {
    char pair = FALSE;
    s->L = lua_open();   /* opens Lua */

    if (s->L == NULL) {
      fprintf (stderr,"cannot create state: not enough memory\n");
      return -1;
    }

    lua_gc(s->L, LUA_GCSTOP, 0);  /* stop collector during initialization */
    luaL_openlibs(s->L);  /* open libraries */
    lua_gc(s->L, LUA_GCRESTART, 0);

    lua_createtable(s->L, 0, 1);      // create new table
    lua_setfield(s->L, LUA_GLOBALSINDEX, HTTP_PARAMS_TABLE);  // add it to global context

    // reset table on stack
    lua_pop(s->L, 1);                                         // pop table (nil value) from stack
    lua_getfield(s->L, LUA_GLOBALSINDEX, HTTP_PARAMS_TABLE);  // push table onto stack

    if(s->http_params[0] != 0)
    {
      /* here are http parameters */
      for (i = 0, c = 0; i < sizeof(s->http_params) ; i++)
      {
        if (s->http_params[i] != '+')
          str[c] = s->http_params[i];
        else
          str[c] = ' ';

        if ((s->http_params[i] == '=') && (s->http_params[i+1] != 0))
        {
          str[ c ] = 0;
          lua_pushstring(s->L,str);
          c = -1;
          pair = TRUE;
        }

        if ((s->http_params[i] == 0) && (pair == TRUE))
        {
          str[ c ] = 0;
          lua_pushstring(s->L,str);
          lua_settable(s->L, -3);             // add key-value pair to table
          break;
        }
        if ((s->http_params[i] == '&') && (pair == TRUE))
        {
          str[ c ] = 0;
          lua_pushstring(s->L,str);
          lua_settable(s->L, -3);             // add key-value pair to table
          c = -1;
          pair = FALSE;
        }
        c++;
      }
      lua_pop(s->L, 1);                     // pop table from stack
    }
    fprintf(stderr,"remote ip: %d.%d.%d.%d [%d]\n", uip_ipaddr1(s->ripaddr), uip_ipaddr2(s->ripaddr),
                                             uip_ipaddr3(s->ripaddr), uip_ipaddr4(s->ripaddr),s->http_connection_nr);
  } else {
    fprintf(stderr,"         : %d.%d.%d.%d [%d]\n", uip_ipaddr1(s->ripaddr), uip_ipaddr2(s->ripaddr),
    			                               uip_ipaddr3(s->ripaddr), uip_ipaddr4(s->ripaddr),s->http_connection_nr);
  }
  error = luaL_loadbuffer(s->L, data, len, "tag") ||  lua_pcall(s->L, 0, 0, 0);

  if (error)
  {
    fprintf(stderr,"%s\n", lua_tostring(s->L, -1));
   	lua_pop(s->L, 1);  /* pop error message from the stack */
  }

  return error;
}
/*---------------------------------------------------------------------------*/
/** @} */
#endif
