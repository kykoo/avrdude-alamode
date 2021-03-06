/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2009 Lars Immisch
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* $Id: alamode.c 948 2012-9-15 14:29:56Z kevino $ */

/*
 * avrdude interface for alamode-Arduino programmer
 *
 * The Alamode programmer is mostly a STK500v1, just the signature bytes
 * are read differently. We are replacing DTR RTS with a GPIO Twiddle
 */

#include "ac_cfg.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>


#include "avrdude.h"
#include "pgm.h"
#include "stk500_private.h"
#include "stk500.h"
#include "serial.h"
// for GPIO code
#include <dirent.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
// Access from ARM Running Linux

#define BCM2836_PERI_BASE        0x3F000000
#define BCM2835_PERI_BASE        0x20000000

static volatile uint32_t gpio_base;
static volatile uint32_t piPeriphBase = 0x20000000;
static volatile uint32_t piModel = 1;



#define PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)

int  mem_fd;
char *gpio_mem, *gpio_map;
char *spi0_mem, *spi0_map;


// I/O access
volatile unsigned *gpio;


// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1 ignores bits which are 0

/* read signature bytes - alamode version */
static int alamode_read_sig_bytes(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m)
{
  unsigned char buf[32];

  /* Signature byte reads are always 3 bytes. */

  if (m->size < 3) {
    fprintf(stderr, "%s: memsize too small for sig byte read", progname);
    return -1;
  }

  buf[0] = Cmnd_STK_READ_SIGN;
  buf[1] = Sync_CRC_EOP;

  serial_send(&pgm->fd, buf, 2);

  if (serial_recv(&pgm->fd, buf, 5) < 0)
    return -1;
  if (buf[0] == Resp_STK_NOSYNC) {
    fprintf(stderr, "%s: stk500_cmd(): programmer is out of sync\n",
			progname);
	return -1;
  } else if (buf[0] != Resp_STK_INSYNC) {
    fprintf(stderr,
			"\n%s: alamode_read_sig_bytes(): (a) protocol error, "
			"expect=0x%02x, resp=0x%02x\n",
			progname, Resp_STK_INSYNC, buf[0]);
	return -2;
  }
  if (buf[4] != Resp_STK_OK) {
    fprintf(stderr,
			"\n%s: alamode_read_sig_bytes(): (a) protocol error, "
			"expect=0x%02x, resp=0x%02x\n",
			progname, Resp_STK_OK, buf[4]);
    return -3;
  }

  m->buf[0] = buf[1];
  m->buf[1] = buf[2];
  m->buf[2] = buf[3];

  return 3;
}

void alamode_reset();
static int alamode_open(PROGRAMMER * pgm, char * port)
{
  strcpy(pgm->port, port);
  if (serial_open(port, pgm->baudrate? pgm->baudrate: 115200, &pgm->fd)==-1) {
    return -1;
  }

  /* Clear GPIO18  to unload the RESET capacitor 
   * (for example in Alamode) */
  alamode_reset();

  /*
   * drain any extraneous input
   */
  stk500_drain(pgm, 0);

  if (stk500_getsync(pgm) < 0)
    return -1;

  return 0;
}

static void alamode_close(PROGRAMMER * pgm)
{
  serial_close(&pgm->fd);
  pgm->fd.ifd = -1;
}

static void setup_io();
void alamode_initpgm(PROGRAMMER * pgm)
{
	/* This is mostly a STK500; just the signature is read
     differently than on real STK500v1 
     and the DTR signal is set when opening the serial port
     for the Auto-Reset feature */
  setup_io();
  stk500_initpgm(pgm);

  strcpy(pgm->type, "Alamode");
  pgm->read_sig_bytes = alamode_read_sig_bytes;
  pgm->open = alamode_open;
  pgm->close = alamode_close;
}
// GPIO functions
//
// determine Raspberry Pi revision
//
unsigned gpioHardwareRevision(void)
{
  static unsigned rev = 0;

  FILE * filp;
  char buf[512];
  char term;
  int chars=4; /* number of chars in revision string */

  if (rev) return rev;

  piModel = 0;

  filp = fopen ("/proc/cpuinfo", "r");

  if (filp != NULL)
    {
      while (fgets(buf, sizeof(buf), filp) != NULL)
	{
	  if (piModel == 0)
	    {
	      if (!strncasecmp("model name", buf, 10))
		{
		  if (strstr (buf, "ARMv6") != NULL)
		    {
		      piModel = 1;
		      chars = 4;
		      piPeriphBase = 0x20000000;
		    }
		  else if (strstr (buf, "ARMv7") != NULL)
		    {
		      piModel = 2;
		      chars = 6;
		      piPeriphBase = 0x3F000000;
		    }
		}
	    }

	  if (!strncasecmp("revision", buf, 8))
	    {
	      if (sscanf(buf+strlen(buf)-(chars+1),
			 "%x%c", &rev, &term) == 2)
		{
		  if (term != '\n') rev = 0;
		}
	    }
	}

      fclose(filp);
    }
  return rev;
}

// Set up a memory regions to access GPIO
//
static void setup_io()
{
  /* open /dev/mem */
  if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
    printf("can't open /dev/mem \n");
    exit (-1);
  }

  /* mmap GPIO */
  // determine Pi revision
  gpioHardwareRevision(); // side effect sets piPeriphBase
  gpio_base =   (piPeriphBase + 0x200000);
  // Allocate MAP block
  if ((gpio_mem = malloc(BLOCK_SIZE + (PAGE_SIZE-1))) == NULL) {
    printf("allocation error \n");
    exit (-1);
  }

  // Make sure pointer is on 4K boundary
  if ((unsigned long)gpio_mem % PAGE_SIZE)
    gpio_mem += PAGE_SIZE - ((unsigned long)gpio_mem % PAGE_SIZE);

  // Now map it
  gpio_map = (unsigned char *)mmap(
				   (caddr_t)gpio_mem,
				   BLOCK_SIZE,
				   PROT_READ|PROT_WRITE,
				   MAP_SHARED|MAP_FIXED,
				   mem_fd,
				   gpio_base
				   );

  if ((long)gpio_map < 0) {
    printf("mmap error %d\n", (int)gpio_map);
    exit (-1);
  }

  // Always use volatile pointer!
  gpio = (volatile unsigned *)gpio_map;


} // setup_io
void alamode_reset(){
  // must use INP_GPIO before OUT_GPIO
  INP_GPIO(16);
  OUT_GPIO(16);

  //  printf("alamode_reset\n");
  
  // set GPIO 16 Low
  GPIO_CLR = 1 << 16;
  usleep(1000*1000);
  // set GPIO 16 High
  GPIO_SET = 1 << 16;
  usleep(50*1000);
}

