/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "ibuffer.h"
#include "imap-message-cache.h"
#include "message-part-serialize.h"
#include "mail-index.h"
#include "mail-index-util.h"

#include <unistd.h>

typedef struct {
	MailIndex *index;
	MailIndexRecord *rec;
} IndexMsgcacheContext;

void *index_msgcache_get_context(MailIndex *index, MailIndexRecord *rec)
{
	IndexMsgcacheContext *ctx;

	ctx = t_new(IndexMsgcacheContext, 1);
	ctx->index = index;
	ctx->rec = rec;
	return ctx;
}

static IBuffer *index_msgcache_open_mail(void *context)
{
	IndexMsgcacheContext *ctx = context;
	int deleted;

	return ctx->index->open_mail(ctx->index, ctx->rec, &deleted);
}

static IBuffer *index_msgcache_inbuf_rewind(IBuffer *inbuf,
					    void *context __attr_unused__)
{
	if (!i_buffer_seek(inbuf, 0)) {
		i_error("index_msgcache_inbuf_rewind: lseek() failed: %m");

		i_buffer_unref(inbuf);
		return NULL;
	}

	return inbuf;
}

static const char *index_msgcache_get_cached_field(ImapCacheField field,
						   void *context)
{
	IndexMsgcacheContext *ctx = context;
	MailField index_field;
	const char *ret;

	switch (field) {
	case IMAP_CACHE_BODY:
		index_field = FIELD_TYPE_BODY;
		break;
	case IMAP_CACHE_BODYSTRUCTURE:
		index_field = FIELD_TYPE_BODYSTRUCTURE;
		break;
	case IMAP_CACHE_ENVELOPE:
		index_field = FIELD_TYPE_ENVELOPE;
		break;
	default:
		return NULL;
	}

	ret = ctx->index->lookup_field(ctx->index, ctx->rec, index_field);
	if (ret == NULL) {
		ctx->index->cache_fields_later(ctx->index, ctx->rec,
					       index_field);
	}
	return ret;
}

static MessagePart *index_msgcache_get_cached_parts(Pool pool, void *context)
{
	IndexMsgcacheContext *ctx = context;
	MessagePart *part;
	const void *part_data;
	size_t part_size;

	part_data = ctx->index->lookup_field_raw(ctx->index, ctx->rec,
						 FIELD_TYPE_MESSAGEPART,
						 &part_size);
	if (part_data == NULL) {
		ctx->index->cache_fields_later(ctx->index, ctx->rec,
					       FIELD_TYPE_MESSAGEPART);
		return NULL;
	}

	part = message_part_deserialize(pool, part_data, part_size);
	if (part == NULL) {
		index_set_corrupted(ctx->index,
				    "Corrupted cached MessagePart data");
		return NULL;
	}

	return part;
}

ImapMessageCacheIface index_msgcache_iface = {
	index_msgcache_open_mail,
	index_msgcache_inbuf_rewind,
	index_msgcache_get_cached_field,
	index_msgcache_get_cached_parts
};
