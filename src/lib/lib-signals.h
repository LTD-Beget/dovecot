#ifndef LIB_SIGNALS_H
#define LIB_SIGNALS_H

#include <signal.h>

typedef void signal_handler_t(int signo, void *context);

/* Set signal handler for specific signal. If delayed is TRUE, the handler
   will be called later, ie. not as a real signal handler. */
void lib_signals_set_handler(int signo, bool delayed,
			     signal_handler_t *handler, void *context);
/* Ignore given signal. */
void lib_signals_ignore(int signo, bool restart_syscalls);
void lib_signals_unset_handler(int signo,
			       signal_handler_t *handler, void *context);

/* Remove and add the internal I/O handler back. This is necessary to get
   the delayed signals to work when using multiple I/O loops. */
void lib_signals_reset_ioloop(void);

void lib_signals_init(void);
void lib_signals_deinit(void);

#endif
