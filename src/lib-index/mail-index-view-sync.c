/* Copyright (C) 2003-2004 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "mail-index-view-private.h"
#include "mail-index-sync-private.h"
#include "mail-transaction-log.h"
#include "mail-transaction-util.h"

struct mail_index_view_sync_ctx {
	struct mail_index_view *view;
	enum mail_index_sync_type sync_mask;
	struct mail_index_map *sync_map;
	buffer_t *expunges;

	const struct mail_transaction_header *hdr;
	const void *data;

	size_t data_offset;
	unsigned int skipped:1;
	unsigned int last_read:1;
	unsigned int sync_map_update:1;
};

static int
view_sync_get_expunges(struct mail_index_view *view, buffer_t **expunges_r)
{
	const struct mail_transaction_header *hdr;
	struct mail_transaction_expunge *src, *src_end, *dest;
	const void *data;
	size_t size;
	int ret;

	*expunges_r = buffer_create_dynamic(default_pool, 512, (size_t)-1);

	/* with mask 0 we don't get anything, we'll just read the expunges
	   while seeking to end */
	if (mail_transaction_log_view_set(view->log_view,
					  view->log_file_seq,
					  view->log_file_offset,
					  view->index->hdr->log_file_seq,
					  view->index->hdr->log_file_offset,
					  MAIL_TRANSACTION_EXPUNGE) < 0)
		return -1;
	while ((ret = mail_transaction_log_view_next(view->log_view,
						     &hdr, &data, NULL)) > 0) {
		mail_transaction_log_sort_expunges(*expunges_r,
						   data, hdr->size);
	}

	if (ret == 0) {
		/* convert to sequences */
		src = dest = buffer_get_modifyable_data(*expunges_r, &size);
		src_end = src + size / sizeof(*src);
		for (; src != src_end; src++) {
			ret = mail_index_lookup_uid_range(view, src->uid1,
							  src->uid2,
							  &dest->uid1,
							  &dest->uid2);
			i_assert(ret == 0);

			if (dest->uid1 == 0)
				size -= sizeof(*dest);
			else
				dest++;
		}
		buffer_set_used_size(*expunges_r, size);
	} else {
		buffer_set_used_size(*expunges_r, 0);
	}

	mail_transaction_log_view_unset(view->log_view);
	return ret;
}

int mail_index_view_sync_begin(struct mail_index_view *view,
                               enum mail_index_sync_type sync_mask,
			       struct mail_index_view_sync_ctx **ctx_r)
{
	const struct mail_index_header *hdr;
	struct mail_index_view_sync_ctx *ctx;
	enum mail_transaction_type mask;
	buffer_t *expunges = NULL;

	/* We must sync flags as long as view is mmap()ed, as the flags may
	   have already changed under us. */
	i_assert((sync_mask & MAIL_INDEX_SYNC_TYPE_FLAGS) != 0);
	i_assert(view->transactions == 0);
	i_assert(!view->syncing);

	if (mail_index_view_lock_head(view, TRUE) < 0)
		return -1;

	hdr = view->index->hdr;
	if ((sync_mask & MAIL_INDEX_SYNC_TYPE_EXPUNGE) != 0) {
		/* get list of all expunges first */
		if (view_sync_get_expunges(view, &expunges) < 0)
			return -1;
	}

	mask = mail_transaction_type_mask_get(sync_mask);
	if (mail_transaction_log_view_set(view->log_view,
					  view->log_file_seq,
					  view->log_file_offset,
					  hdr->log_file_seq,
					  hdr->log_file_offset, mask) < 0) {
		if (expunges != NULL)
			buffer_free(expunges);
		return -1;
	}

	ctx = i_new(struct mail_index_view_sync_ctx, 1);
	ctx->view = view;
	ctx->sync_mask = sync_mask;
	ctx->expunges = expunges;

	if ((sync_mask & MAIL_INDEX_SYNC_TYPE_EXPUNGE) != 0) {
		ctx->sync_map = view->index->map;
		ctx->sync_map->refcount++;
	} else {
		/* we need a private copy of the map if we don't want to
		   sync expunges */
		if (MAIL_INDEX_MAP_IS_IN_MEMORY(view->map))
			ctx->sync_map_update = TRUE;
		ctx->sync_map = mail_index_map_to_memory(view->map);
	}

	view->syncing = TRUE;

	*ctx_r = ctx;
	return 0;
}

static int view_is_transaction_synced(struct mail_index_view *view,
				      uint32_t seq, uoff_t offset)
{
	const unsigned char *data, *end;
	size_t size;

	if (view->log_syncs == NULL)
		return 0;

	data = buffer_get_data(view->log_syncs, &size);
	end = data + size;

	for (; data < end; ) {
		if (*((const uoff_t *)data) == offset &&
		    *((const uint32_t *)(data + sizeof(uoff_t))) == seq)
			return 1;
		data += sizeof(uoff_t) + sizeof(uint32_t);
	}

	return 0;
}

static int sync_expunge(const struct mail_transaction_expunge *e, void *context)
{
        struct mail_index_view_sync_ctx *ctx = context;
	struct mail_index_map *map = ctx->sync_map;
	uint32_t idx, count, seq1, seq2;
	int ret;

	ret = mail_index_lookup_uid_range(ctx->view, e->uid1, e->uid2,
					  &seq1, &seq2);
	i_assert(ret == 0);

	if (seq1 == 0)
		return 1;

	for (idx = seq1-1; idx < seq2; idx++) {
		mail_index_header_update_counts(&map->hdr_copy,
						map->records[idx].flags, 0);
	}

	count = seq2 - seq1 + 1;
	buffer_delete(map->buffer,
		      (seq1-1) * sizeof(struct mail_index_record),
		      count * sizeof(struct mail_index_record));
	map->records = buffer_get_modifyable_data(map->buffer, NULL);

	map->records_count -= count;
	map->hdr_copy.messages_count -= count;
	return 1;
}

static int sync_append(const struct mail_index_record *rec, void *context)
{
        struct mail_index_view_sync_ctx *ctx = context;
	struct mail_index_map *map = ctx->sync_map;

	buffer_append(map->buffer, rec, sizeof(*rec));
	map->records = buffer_get_modifyable_data(map->buffer, NULL);

	map->records_count++;
	map->hdr_copy.messages_count++;
	map->hdr_copy.next_uid = rec->uid+1;

	mail_index_header_update_counts(&map->hdr_copy, 0, rec->flags);
	mail_index_header_update_lowwaters(&map->hdr_copy, rec);
	return 1;
}

static int sync_flag_update(const struct mail_transaction_flag_update *u,
			    void *context)
{
        struct mail_index_view_sync_ctx *ctx = context;
	struct mail_index_map *map = ctx->sync_map;
	struct mail_index_record *rec;
	uint32_t i, idx, seq1, seq2;
	uint8_t old_flags;
	int ret;

	ret = mail_index_lookup_uid_range(ctx->view, u->uid1, u->uid2,
					  &seq1, &seq2);
	i_assert(ret == 0);

	if (seq1 == 0)
		return 1;

	for (idx = seq1-1; idx < seq2; idx++) {
		rec = &map->records[idx];

		old_flags = rec->flags;
		rec->flags = (rec->flags & ~u->remove_flags) | u->add_flags;
		for (i = 0; i < INDEX_KEYWORDS_BYTE_COUNT; i++) {
			rec->keywords[i] = u->add_keywords[i] |
				(rec->keywords[i] & ~u->remove_keywords[i]);
		}

		mail_index_header_update_counts(&map->hdr_copy, old_flags,
						rec->flags);
		mail_index_header_update_lowwaters(&map->hdr_copy, rec);
	}
	return 1;
}

static int sync_cache_update(const struct mail_transaction_cache_update *u,
			     void *context)
{
        struct mail_index_view_sync_ctx *ctx = context;
	uint32_t seq;
	int ret;

	ret = mail_index_lookup_uid_range(ctx->view, u->uid, u->uid,
					  &seq, &seq);
	i_assert(ret == 0);

	if (seq != 0)
		ctx->sync_map->records[seq-1].cache_offset = u->cache_offset;
	return 1;
}

static int mail_index_view_sync_map(struct mail_index_view_sync_ctx *ctx)
{
	static struct mail_transaction_map_functions map_funcs = {
		sync_expunge, sync_append, sync_flag_update, sync_cache_update
	};

	return mail_transaction_map(ctx->hdr, ctx->data, &map_funcs, ctx);
}

static int mail_index_view_sync_next_trans(struct mail_index_view_sync_ctx *ctx,
					   uint32_t *seq_r, uoff_t *offset_r)
{
        struct mail_transaction_log_view *log_view = ctx->view->log_view;
	struct mail_index_view *view = ctx->view;
	int ret, skipped;

	ret = mail_transaction_log_view_next(log_view, &ctx->hdr, &ctx->data,
					     &skipped);
	if (ret <= 0) {
		if (ret < 0)
			return -1;

		ctx->last_read = TRUE;
		return 1;
	}

	if (skipped)
		ctx->skipped = TRUE;

	mail_transaction_log_view_get_prev_pos(log_view, seq_r, offset_r);

	/* skip flag changes that we committed ourself or have already synced */
	if (view_is_transaction_synced(view, *seq_r, *offset_r))
		return 0;

	if (ctx->sync_map_update) {
		if (mail_index_view_sync_map(ctx) < 0)
			return -1;
	}

	return 1;
}

#define FLAG_UPDATE_IS_INTERNAL(u, empty) \
	(((u)->add_flags | (u)->remove_flags) == MAIL_INDEX_MAIL_FLAG_DIRTY && \
	 memcmp((u)->add_keywords, empty, INDEX_KEYWORDS_BYTE_COUNT) == 0 && \
	 memcmp((u)->add_keywords, empty, INDEX_KEYWORDS_BYTE_COUNT) == 0)

static int
mail_index_view_sync_get_rec(struct mail_index_view_sync_ctx *ctx,
			     struct mail_index_sync_rec *rec)
{
	static keywords_mask_t empty_keywords = { 0, };
	const struct mail_transaction_header *hdr = ctx->hdr;
	const void *data = ctx->data;

	switch (hdr->type & MAIL_TRANSACTION_TYPE_MASK) {
	case MAIL_TRANSACTION_APPEND: {
		rec->type = MAIL_INDEX_SYNC_TYPE_APPEND;
		rec->uid1 = rec->uid2 = 0;
		ctx->data_offset += hdr->size;
		break;
	}
	case MAIL_TRANSACTION_EXPUNGE: {
		const struct mail_transaction_expunge *exp =
			CONST_PTR_OFFSET(data, ctx->data_offset);

		ctx->data_offset += sizeof(*exp);
                mail_index_sync_get_expunge(rec, exp);
		break;
	}
	case MAIL_TRANSACTION_FLAG_UPDATE: {
		const struct mail_transaction_flag_update *update =
			CONST_PTR_OFFSET(data, ctx->data_offset);

		for (;;) {
			ctx->data_offset += sizeof(*update);
			if (!FLAG_UPDATE_IS_INTERNAL(update, empty_keywords))
				break;

			if (ctx->data_offset == ctx->hdr->size)
				return 0;
		}
                mail_index_sync_get_update(rec, update);
		break;
	}
	default:
		i_unreached();
	}
	return 1;
}

int mail_index_view_sync_next(struct mail_index_view_sync_ctx *ctx,
			      struct mail_index_sync_rec *sync_rec)
{
	struct mail_index_view *view = ctx->view;
	uint32_t seq;
	uoff_t offset;
	int ret;

	do {
		if (ctx->hdr == NULL || ctx->data_offset == ctx->hdr->size) {
			ctx->data_offset = 0;
			do {
				ret = mail_index_view_sync_next_trans(ctx, &seq,
								      &offset);
				if (ret < 0)
					return -1;

				if (ctx->last_read)
					return 0;

				if (!ctx->skipped) {
					view->log_file_seq = seq;
					view->log_file_offset = offset +
						sizeof(*ctx->hdr) +
						ctx->hdr->size;
				}
			} while (ret == 0);

			if (ctx->skipped) {
				mail_index_view_add_synced_transaction(view,
								       seq,
								       offset);
			}
		}
	} while (!mail_index_view_sync_get_rec(ctx, sync_rec));

	return 1;
}

const uint32_t *
mail_index_view_sync_get_expunges(struct mail_index_view_sync_ctx *ctx,
				  size_t *count_r)
{
	const uint32_t *data;
	size_t size;

	data = buffer_get_data(ctx->expunges, &size);
	*count_r = size / (sizeof(uint32_t)*2);
	return data;
}

void mail_index_view_sync_end(struct mail_index_view_sync_ctx *ctx)
{
        struct mail_index_view *view = ctx->view;

	i_assert(view->syncing);

	if (view->log_syncs != NULL && !ctx->skipped)
		buffer_set_used_size(view->log_syncs, 0);

	if (!ctx->last_read && ctx->hdr != NULL &&
	    ctx->data_offset != ctx->hdr->size) {
		/* we didn't sync everything */
		view->inconsistent = TRUE;
	}

	mail_index_unmap(view->index, view->map);
	view->map = ctx->sync_map;
	view->map_protected = FALSE;

	if ((ctx->sync_mask & MAIL_INDEX_SYNC_TYPE_APPEND) != 0)
		view->messages_count = view->map->records_count;

        mail_transaction_log_view_unset(view->log_view);

	if (ctx->expunges != NULL)
		buffer_free(ctx->expunges);

	view->syncing = FALSE;
	i_free(ctx);
}

void mail_index_view_add_synced_transaction(struct mail_index_view *view,
					    uint32_t log_file_seq,
					    uoff_t log_file_offset)
{
	if (view->log_syncs == NULL) {
		view->log_syncs = buffer_create_dynamic(default_pool,
							128, (size_t)-1);
	}
	buffer_append(view->log_syncs, &log_file_offset,
		      sizeof(log_file_offset));
	buffer_append(view->log_syncs, &log_file_seq, sizeof(log_file_seq));
}
