#ifndef __ISTREAM_HEADER_FILTER_H
#define __ISTREAM_HEADER_FILTER_H

struct message_header_line;

typedef void header_filter_callback(struct message_header_line *hdr,
				    int *matched, void *context);

/* NOTE: headers list must be sorted. If filter is TRUE, given headers are
   removed from output, otherwise only given headers are included in output. */
struct istream *
i_stream_create_header_filter(pool_t pool, struct istream *input, int filter,
			      const char *const *headers, size_t headers_count,
			      header_filter_callback *callback, void *context);

#endif
