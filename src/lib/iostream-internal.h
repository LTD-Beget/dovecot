#ifndef __IOSTREAM_INTERNAL_H
#define __IOSTREAM_INTERNAL_H

/* This file is private to input stream and output stream implementations */

struct _iostream {
	pool_t pool;
	int refcount;

	void (*close)(struct _iostream *stream);
	void (*destroy)(struct _iostream *stream);
	void (*set_max_buffer_size)(struct _iostream *stream, size_t max_size);
};

void _io_stream_init(pool_t pool, struct _iostream *stream);
void _io_stream_ref(struct _iostream *stream);
void _io_stream_unref(struct _iostream *stream);
void _io_stream_close(struct _iostream *stream);
void _io_stream_set_max_buffer_size(struct _iostream *stream, size_t max_size);

#endif
