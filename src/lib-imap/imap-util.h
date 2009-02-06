#ifndef IMAP_UTIL_H
#define IMAP_UTIL_H

#include "seq-range-array.h"

enum mail_flags;
struct imap_arg;

/* Write flags as a space separated string. */
void imap_write_flags(string_t *dest, enum mail_flags flags,
		      const char *const *keywords);

/* Write sequence range as IMAP sequence-set */
void imap_write_seq_range(string_t *dest, const ARRAY_TYPE(seq_range) *array);
/* Write IMAP args to given string. The string is mainly useful for humans. */
void imap_args_to_str(string_t *dest, const struct imap_arg *args);

#endif
