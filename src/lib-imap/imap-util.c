/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "temp-string.h"
#include "imap-util.h"

const char *imap_write_flags(MailFlags flags, const char *custom_flags[])
{
	TempString *str;
	const char *sysflags, *name;
	int i;

	if (flags == 0)
		return "";

	sysflags = t_strconcat((flags & MAIL_ANSWERED) ? " \\Answered" : "",
			       (flags & MAIL_FLAGGED) ? " \\Flagged" : "",
			       (flags & MAIL_DELETED) ? " \\Deleted" : "",
			       (flags & MAIL_SEEN) ? " \\Seen" : "",
			       (flags & MAIL_DRAFT) ? " \\Draft" : "",
			       (flags & MAIL_RECENT)  ? " \\Recent" : "",
			       NULL);

	if (*sysflags != '\0')
		sysflags++;

	if ((flags & MAIL_CUSTOM_FLAGS_MASK) == 0)
		return sysflags;

	/* we have custom flags too */
	str = t_string_new(256);
	t_string_append(str, sysflags);

	for (i = 0; i < MAIL_CUSTOM_FLAGS_COUNT; i++) {
		if (flags & (1 << (i + MAIL_CUSTOM_FLAG_1_BIT))) {
			name = custom_flags[i];
			if (name != NULL && *name != '\0') {
				if (str->len > 0)
					t_string_append_c(str, ' ');
				t_string_append(str, name);
			}
		}
	}

	return str->str;
}

const char *imap_escape(const char *str)
{
	char *ret, *p;
	unsigned int i, esc;

	/* get length of string and number of chars to escape */
	esc = 0;
	for (i = 0; str[i] != '\0'; i++) {
		if (IS_ESCAPED_CHAR(str[i]))
			esc++;
	}

	if (esc == 0)
		return str;

	/* escape them */
	p = ret = t_malloc(i + esc + 1);
	for (; *str != '\0'; str++) {
		if (IS_ESCAPED_CHAR(str[i]))
			*p++ = '\\';
		*p++ = *str;
	}
	*p = '\0';
	return ret;
}
