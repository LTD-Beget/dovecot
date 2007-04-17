/* Copyright (C) 2007 Timo Sirainen */

#include "lib.h"
#include "hostpid.h"
#include "istream.h"
#include "ostream.h"
#include "ostream-crlf.h"
#include "str.h"
#include "index-mail.h"
#include "cydir-storage.h"
#include "cydir-sync.h"

#include <stdio.h>

struct cydir_save_context {
	struct mail_save_context ctx;

	struct cydir_mailbox *mbox;
	struct mail_index_transaction *trans;

	char *tmp_basename;
	unsigned int mail_count;

	struct cydir_sync_context *sync_ctx;

	/* updated for each appended mail: */
	uint32_t seq;
	struct istream *input;
	struct ostream *output;
	struct mail *mail;

	unsigned int failed:1;
	unsigned int finished:1;
};

static char *cydir_generate_tmp_filename(void)
{
	static unsigned int create_count;

	return i_strdup_printf("%s.P%sQ%uM%s.%s",
			       dec2str(ioloop_timeval.tv_sec), my_pid,
			       create_count++,
			       dec2str(ioloop_timeval.tv_usec), my_hostname);
}

static const char *
cydir_get_save_path(struct cydir_save_context *ctx, unsigned int num)
{
	const char *dir;

	dir = mailbox_list_get_path(ctx->mbox->storage->storage.list,
				    ctx->mbox->ibox.box.name,
				    MAILBOX_LIST_PATH_TYPE_MAILBOX);
	return t_strdup_printf("%s/%s.%u", dir, ctx->tmp_basename, num);
}

int cydir_save_init(struct mailbox_transaction_context *_t,
		    enum mail_flags flags, struct mail_keywords *keywords,
		    time_t received_date, int timezone_offset __attr_unused__,
		    const char *from_envelope __attr_unused__,
		    struct istream *input, struct mail *dest_mail,
		    struct mail_save_context **ctx_r)
{
	struct cydir_transaction_context *t =
		(struct cydir_transaction_context *)_t;
	struct cydir_mailbox *mbox = (struct cydir_mailbox *)t->ictx.ibox;
	struct cydir_save_context *ctx = t->save_ctx;
	enum mail_flags save_flags;
	struct ostream *output;
	const char *path;
	int fd;

	i_assert((t->ictx.flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

	if (received_date == (time_t)-1)
		received_date = ioloop_time;

	if (ctx == NULL) {
		ctx = t->save_ctx = i_new(struct cydir_save_context, 1);
		ctx->ctx.transaction = &t->ictx.mailbox_ctx;
		ctx->mbox = mbox;
		ctx->trans = t->ictx.trans;
		ctx->tmp_basename = cydir_generate_tmp_filename();
	}
	ctx->input = input;

	t_push();
	path = cydir_get_save_path(ctx, ctx->mail_count);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0660);
	if (fd != -1) {
		output = o_stream_create_file(fd, default_pool, 0, TRUE);
		ctx->output = o_stream_create_crlf(default_pool, output);
		o_stream_unref(&output);
	} else {
		mail_storage_set_critical(_t->box->storage,
					  "open(%s) failed: %m", path);
		ctx->failed = TRUE;
		t_pop();
		return -1;
	}
	t_pop();

	/* add to index */
	save_flags = (flags & ~MAIL_RECENT) | MAIL_RECENT;
	mail_index_append(ctx->trans, 0, &ctx->seq);
	mail_index_update_flags(ctx->trans, ctx->seq, MODIFY_REPLACE,
				save_flags);
	if (keywords != NULL) {
		mail_index_update_keywords(ctx->trans, ctx->seq,
					   MODIFY_REPLACE, keywords);
	}

	if (dest_mail == NULL) {
		if (ctx->mail == NULL)
			ctx->mail = index_mail_alloc(_t, 0, NULL);
		dest_mail = ctx->mail;
	}
	if (mail_set_seq(dest_mail, ctx->seq) < 0)
		i_unreached();

	*ctx_r = &ctx->ctx;
	return ctx->failed ? -1 : 0;
}

int cydir_save_continue(struct mail_save_context *_ctx)
{
	struct cydir_save_context *ctx = (struct cydir_save_context *)_ctx;

	if (ctx->failed)
		return -1;

	if (o_stream_send_istream(ctx->output, ctx->input) < 0) {
		if (ENOSPACE(ctx->output->stream_errno)) {
			mail_storage_set_error(&ctx->mbox->storage->storage,
					       "Not enough disk space");
		} else {
			mail_storage_set_critical(&ctx->mbox->storage->storage,
				"o_stream_send_istream(%s) failed: %m",
				cydir_get_save_path(ctx, ctx->mail_count));
		}
		ctx->failed = TRUE;
		return -1;
	}
	return 0;
}

int cydir_save_finish(struct mail_save_context *_ctx)
{
	struct cydir_save_context *ctx = (struct cydir_save_context *)_ctx;

	ctx->finished = TRUE;

	if (!ctx->failed)
		ctx->mail_count++;

	return ctx->failed ? -1 : 0;
}

void cydir_save_cancel(struct mail_save_context *_ctx)
{
	struct cydir_save_context *ctx = (struct cydir_save_context *)_ctx;

	ctx->failed = TRUE;
	(void)cydir_save_finish(_ctx);
}

int cydir_transaction_save_commit_pre(struct cydir_save_context *ctx)
{
	struct cydir_transaction_context *t =
		(struct cydir_transaction_context *)ctx->ctx.transaction;
	const struct mail_index_header *hdr;
	uint32_t i, uid, next_uid;
	const char *dir;
	string_t *src_path, *dest_path;
	unsigned int src_prefixlen, dest_prefixlen;

	i_assert(ctx->finished);

	if (cydir_sync_begin(ctx->mbox, &ctx->sync_ctx) < 0) {
		ctx->failed = TRUE;
		cydir_transaction_save_rollback(ctx);
		return -1;
	}

	hdr = mail_index_get_header(ctx->sync_ctx->sync_view);
	uid = hdr->next_uid;
	mail_index_append_assign_uids(ctx->trans, uid, &next_uid);

	*t->ictx.first_saved_uid = uid;
	*t->ictx.last_saved_uid = next_uid - 1;

	dir = mailbox_list_get_path(ctx->mbox->storage->storage.list,
				    ctx->mbox->ibox.box.name,
				    MAILBOX_LIST_PATH_TYPE_MAILBOX);

	src_path = t_str_new(256);
	str_printfa(src_path, "%s/%s.", dir, ctx->tmp_basename);
	src_prefixlen = str_len(src_path);

	dest_path = t_str_new(256);
	str_append(dest_path, dir);
	str_append_c(dest_path, '/');
	dest_prefixlen = str_len(dest_path);

	for (i = 0; i < ctx->mail_count; i++, uid++) {
		str_truncate(src_path, src_prefixlen);
		str_truncate(dest_path, dest_prefixlen);
		str_printfa(src_path, "%u", i);
		str_printfa(dest_path, "%u.", uid);

		if (rename(str_c(src_path), str_c(dest_path)) < 0) {
			mail_storage_set_critical(&ctx->mbox->storage->storage,
				"rename(%s, %s) failed: %m",
				str_c(src_path), str_c(dest_path));
			ctx->failed = TRUE;
			cydir_transaction_save_rollback(ctx);
			return -1;
		}
	}

	return 0;
}

void cydir_transaction_save_commit_post(struct cydir_save_context *ctx)
{
	(void)cydir_sync_finish(&ctx->sync_ctx, TRUE);
	cydir_transaction_save_rollback(ctx);
}

void cydir_transaction_save_rollback(struct cydir_save_context *ctx)
{
	if (!ctx->finished)
		cydir_save_cancel(&ctx->ctx);

	if (ctx->sync_ctx != NULL)
		(void)cydir_sync_finish(&ctx->sync_ctx, FALSE);

	if (ctx->mail != NULL)
		index_mail_free(ctx->mail);
	i_free(ctx->tmp_basename);
	i_free(ctx);
}
