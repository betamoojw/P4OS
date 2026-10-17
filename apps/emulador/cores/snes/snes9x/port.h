/* This file is part of Snes9x. See LICENSE file. */

#ifndef _PORT_H_
#define _PORT_H_

#include <limits.h>
#include <stddef.h> /* P4OS: ptrdiff_t, size_t (Retro-Go had them from rg_system.h) */
#include <stdio.h>  /* P4OS: likewise */
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#ifdef RETRO_GO
#include <rg_system.h>
#endif

#ifndef INLINE
#define INLINE inline
#endif

#ifdef PSP
#define PIXEL_FORMAT BGR555
#else
#define PIXEL_FORMAT RGB565
#endif
/* The above is used to disable the 16-bit graphics mode checks sprinkled
 * throughout the code, if the pixel format is always 16-bit. */

#include "pixform.h"

#ifndef _WIN32

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef _MAX_DIR
#define _MAX_DIR   PATH_MAX
#endif

#ifndef _MAX_DRIVE
#define _MAX_DRIVE 1
#endif

#ifndef _MAX_FNAME
#define _MAX_FNAME PATH_MAX
#endif

#ifndef _MAX_EXT
#define _MAX_EXT   PATH_MAX
#endif

#ifndef _MAX_PATH
#define _MAX_PATH  PATH_MAX
#endif
#endif

#define SLASH_STR "/"
#define SLASH_CHAR '/'

#if defined(__i386__)  || defined(__i486__) || defined(__i586__) || defined(_XBOX1) || defined(__alpha__)
#define FAST_LSB_WORD_ACCESS
#elif defined(__MIPSEL__)
/* On little-endian MIPS, a 16-bit word can be read directly from an address
 * only if it's aligned. */
#define FAST_ALIGNED_LSB_WORD_ACCESS
#endif

#define ABS(X)   ((X) <  0  ? -(X) : (X))
#define MIN(A,B) ((A) < (B) ?  (A) : (B))
#define MAX(A,B) ((A) > (B) ?  (A) : (B))

/* P4OS: count leading zeros without __builtin_clz(), which on the
 * board's rv32imafc (no Zbb) is a call to libgcc's __clzsi2, not in the
 * firmware's table. x is never 0 where it is used (dsp.c). */
static INLINE int s9x_clz32(uint32_t x)
{
   int n = 0;
   if (!(x & 0xffff0000u)) { n += 16; x <<= 16; }
   if (!(x & 0xff000000u)) { n += 8;  x <<= 8;  }
   if (!(x & 0xf0000000u)) { n += 4;  x <<= 4;  }
   if (!(x & 0xc0000000u)) { n += 2;  x <<= 2;  }
   if (!(x & 0x80000000u)) { n += 1; }
   return n;
}

/* Integer square root by Halleck's method, with Legalize's speedup */
static INLINE int32_t _isqrt(int32_t val)
{
   int32_t squaredbit, remainder, root;

   if (val < 1)
      return 0;

   squaredbit  = 1 << 30;
   remainder = val;
   root = 0;

   while (squaredbit > 0)
   {
      if (remainder >= (squaredbit | root))
      {
         remainder -= (squaredbit | root);
         root >>= 1;
         root |= squaredbit;
      }
      else
         root >>= 1;
      squaredbit >>= 2;
   }

   return root;
}
#endif
