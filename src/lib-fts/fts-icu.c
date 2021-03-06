/* Copyright (c) 2014-2015 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "unichar.h"
#include "fts-icu.h"

void fts_icu_utf8_to_utf16(buffer_t *dest_utf16, const char *src_utf8)
{
	UErrorCode err = U_ZERO_ERROR;
	unsigned int src_bytes = strlen(src_utf8);
	int32_t utf16_len;
	UChar *dest_data, *retp = NULL;
	int32_t avail_uchars = 0;

	/* try to encode with the current buffer size */
	avail_uchars = buffer_get_writable_size(dest_utf16) / sizeof(UChar);
	dest_data = buffer_get_space_unsafe(dest_utf16, 0,
				buffer_get_writable_size(dest_utf16));
	retp = u_strFromUTF8Lenient(dest_data, avail_uchars,
				    &utf16_len, src_utf8, src_bytes, &err);
	if (err == U_BUFFER_OVERFLOW_ERROR) {
		/* try again with a larger buffer */
		dest_data = buffer_get_space_unsafe(dest_utf16, 0,
						    utf16_len * sizeof(UChar));
		err = U_ZERO_ERROR;
		retp = u_strFromUTF8Lenient(dest_data, utf16_len,
					    &utf16_len, src_utf8,
					    src_bytes, &err);
	}
	if (U_FAILURE(err)) {
		i_panic("LibICU u_strFromUTF8Lenient() failed: %s",
			u_errorName(err));
	}
	buffer_set_used_size(dest_utf16, utf16_len * sizeof(UChar));
	i_assert(retp == dest_data);
}

void fts_icu_utf16_to_utf8(string_t *dest_utf8, const UChar *src_utf16,
			   unsigned int src_len)
{
	int32_t dest_len = 0;
	int32_t sub_num = 0;
	char *dest_data, *retp = NULL;
	UErrorCode err = U_ZERO_ERROR;

	/* try to encode with the current buffer size */
	dest_data = buffer_get_space_unsafe(dest_utf8, 0,
					    buffer_get_writable_size(dest_utf8));
	retp = u_strToUTF8WithSub(dest_data, buffer_get_writable_size(dest_utf8),
				  &dest_len, src_utf16, src_len,
				  UNICODE_REPLACEMENT_CHAR, &sub_num, &err);
	if (err == U_BUFFER_OVERFLOW_ERROR) {
		/* try again with a larger buffer */
		dest_data = buffer_get_space_unsafe(dest_utf8, 0, dest_len);
		err = U_ZERO_ERROR;
		retp = u_strToUTF8WithSub(dest_data, buffer_get_writable_size(dest_utf8), &dest_len,
					  src_utf16, src_len,
					  UNICODE_REPLACEMENT_CHAR,
					  &sub_num, &err);
	}
	if (U_FAILURE(err)) {
		i_panic("LibICU u_strToUTF8WithSub() failed: %s",
			u_errorName(err));
	}
	buffer_set_used_size(dest_utf8, dest_len);
	i_assert(retp == dest_data);
}

int fts_icu_translate(buffer_t *dest_utf16, const UChar *src_utf16,
		      unsigned int src_len, UTransliterator *transliterator,
		      const char **error_r)
{
	UErrorCode err = U_ZERO_ERROR;
	int32_t utf16_len = src_len;
	UChar *dest_data;
	int32_t avail_uchars, limit = src_len;
	size_t dest_pos = dest_utf16->used;

	/* translation is done in-place in the buffer. try first with the
	   current buffer size. */
	buffer_append(dest_utf16, src_utf16, src_len*sizeof(UChar));

	avail_uchars = (buffer_get_writable_size(dest_utf16)-dest_pos) / sizeof(UChar);
	dest_data = buffer_get_space_unsafe(dest_utf16, dest_pos,
				buffer_get_writable_size(dest_utf16)-dest_pos);
	utrans_transUChars(transliterator, dest_data, &utf16_len,
			   avail_uchars, 0, &limit, &err);
	if (err == U_BUFFER_OVERFLOW_ERROR) {
		/* try again with a larger buffer */
		err = U_ZERO_ERROR;
		avail_uchars = utf16_len;
		limit = utf16_len = src_len;
		buffer_write(dest_utf16, dest_pos,
			     src_utf16, src_len*sizeof(UChar));
		dest_data = buffer_get_space_unsafe(dest_utf16, dest_pos,
						    avail_uchars * sizeof(UChar));
		utrans_transUChars(transliterator, dest_data, &utf16_len,
				   avail_uchars, 0, &limit, &err);
		i_assert(err != U_BUFFER_OVERFLOW_ERROR);
	}
	if (U_FAILURE(err)) {
		*error_r = t_strdup_printf("LibICU utrans_transUChars() failed: %s",
					   u_errorName(err));
		buffer_set_used_size(dest_utf16, dest_pos);
		return -1;
	}
	buffer_set_used_size(dest_utf16, utf16_len * sizeof(UChar));
	return 0;
}
