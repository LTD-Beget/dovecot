#ifndef __LIB_H
#define __LIB_H

/* default lib includes */
#ifdef HAVE_CONFIG_H
#  include "../../config.h"
#endif

/* default system includes - keep these at minimum.. */
#include <string.h> /* strcmp() etc. */
#include <stdarg.h> /* va_list is used everywhere */
#include <limits.h> /* INT_MAX, etc. */
#include <errno.h> /* error checking is good */
#include <sys/types.h> /* many other includes want this */

typedef struct _IOLoop *IOLoop;
typedef struct _IO *IO;
typedef struct _Timeout *Timeout;

typedef struct _IPADDR IPADDR;
typedef struct _IOBuffer IOBuffer;
typedef struct _TempString TempString;

#include "compat.h"
#include "macros.h"
#include "failures.h"

#include "data-stack.h"
#include "mempool.h"
#include "imem.h"

#include "strfuncs.h"

size_t nearest_power(size_t num);

void lib_init(void);
void lib_deinit(void);

#endif
