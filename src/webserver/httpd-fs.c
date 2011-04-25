/*
 * Copyright (c) 2001, Swedish Institute of Computer Science.
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
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 * $Id: httpd-fs.c,v 1.1 2006/06/07 09:13:08 adam Exp $
 */
#include "platform_conf.h"
#ifdef BUILD_WEB_SERVER

#include "httpd.h"
#include "httpd-fs.h"
#include "httpd-fsdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "type.h"
#include "devman.h"
#include "platform.h"
#include "romfs.h"
#include "shell.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "term.h"
#include "elua_net.h"
#include <fcntl.h>
#include <sys/stat.h>


#define ONE_KB (1024)
#define FILE_HUGE_SIZE   (ONE_KB*300)
//#define FILE_CACHE_SIZE  (ONE_KB*10)
#define FILE_CACHE_SIZE  0
#define FILE_NAME_MAX_SIZE   10
#define FILE_CACHE_NR    10
#define FILE_NAME_PREFIX "/mmc"

typedef struct  {
  char *huge;
  struct {
	  char         *data;
	  char         name[FILE_NAME_MAX_SIZE];
	  _ssize_t     len;
  } cache[FILE_CACHE_NR];
} file_fs_t;

file_fs_t file_fs;


/*-----------------------------------------------------------------------------------*/
int
httpd_fs_int()
{
  int i;

  if (file_fs.huge == NULL) {
    if((file_fs.huge = malloc(FILE_HUGE_SIZE)) == NULL) {
      fprintf(stderr, "httpd_fs_init(): malloc error\n");
	  return 1;
	}
  }
  else
    return 0; /* already initialized */

  for (i = 0; i < FILE_CACHE_NR; i++ ) {
    if((file_fs.cache[i].data = malloc(FILE_CACHE_SIZE)) == NULL) {
      fprintf(stderr, "httpd_fs_init(): malloc error\n");
      return 1;
    }
    file_fs.cache[i].name[0] = 0;
    file_fs.cache[i].len     = 0;
  }

  return 0;
}

/*-----------------------------------------------------------------------------------*/
int
httpd_fs_get_free_index()
{
  int i;

  for (i = 0; i < FILE_CACHE_NR; i++ ) {
    if ((file_fs.cache[i].len) == 0)
      return i;
  }

  return FILE_CACHE_NR;
}

/*-----------------------------------------------------------------------------------*/
int
httpd_fs_search_name(const char *name)
{
  int i;

  for (i = 0; i < FILE_CACHE_NR; i++ ) {
    if (strcmp(file_fs.cache[i].name,name) == 0)
      return i;
  }

  return FILE_CACHE_NR;
}

/*-----------------------------------------------------------------------------------*/

int httpd_fs_open(const char *name, struct httpd_fs_file *file)
{
  FILE                             *fd;
  char                             fullpath[sizeof(FILE_NAME_PREFIX)+FILE_NAME_MAX_SIZE] = FILE_NAME_PREFIX;
  struct stat                      fstat_buf;
  struct                           _reent r; /* it needs a better solution */
  int                              index;

  file->len = 0;

  if (httpd_fs_int())
  {
    printf( "httpd_fs_open(): httpd_fs_int() error\n");
    return 0;
  }

  if((index = httpd_fs_search_name(name)) < FILE_CACHE_NR) {
    file->len  = file_fs.cache[index].len;
    file->data = file_fs.cache[index].data;
    return 1;
  }

  strcat (fullpath, name);

  if ((fd = fopen(fullpath, "r" )) == NULL )
  {
    fprintf(stderr, "httpd_fs_open(): %s not found.\n",name);
    return 0;
  }

  if(_fstat_r(&r,fileno(fd), &fstat_buf))
  {
    	  file->len=0; 
    fprintf(stderr, "httpd_fs_open(): fstat error in %s.\n",name);
	fclose(fd);
	return 0;
  }

  file->len = fstat_buf.st_size;

  if (file->len > FILE_HUGE_SIZE)
  {
    fprintf(stderr, "httpd_fs_open(): file too big.\n");
    fclose(fd);
    return 0;
  }

  if (file->len < FILE_CACHE_SIZE)
  {
	if((index = httpd_fs_get_free_index()) < FILE_CACHE_NR)
	{
      if (file->len != fread(file_fs.cache[index].data,1,file->len,fd))
      {
        fprintf(stderr,"httpd_fs_open(): file size error.\n");
        fclose(fd);
        return 0;
      }
      file_fs.cache[index].data[file->len + 1] = 0;
      file_fs.cache[index].len = file->len;
      strcpy(file_fs.cache[index].name,name);
      file->data = file_fs.cache[index].data;
	} else {
      if (file->len != fread(file_fs.huge,1,file->len,fd))
      {
        printf( "httpd_fs_open(): file size error.\n");
        fclose(fd);
        return 0;
      }
      file_fs.huge[file->len + 1] = 0;
      file->data = file_fs.huge;
	}
  } else {
    if (file->len != fread(file_fs.huge,1,file->len,fd))
    {
      fprintf(stderr, "httpd_fs_open(): file size error.\n");
      fclose(fd);
      return 0;
    }
    file_fs.huge[file->len + 1] = 0;
    file->data = file_fs.huge;
  }

  fclose(fd);

  return 1;
}

/*-----------------------------------------------------------------------------------*/
#endif
