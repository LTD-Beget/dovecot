/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "ibuffer.h"
#include "mail-custom-flags.h"
#include "index-storage.h"
#include "index-messageset.h"

#include <unistd.h>

typedef struct {
	Mailbox *dest;
	const char **custom_flags;
	int copy_inside_mailbox;
} CopyContext;

static int copy_func(MailIndex *index, MailIndexRecord *rec,
		     unsigned int client_seq __attr_unused__,
		     unsigned int idx_seq __attr_unused__, void *context)
{
	CopyContext *ctx = context;
	IndexMailbox *dest_ibox = NULL;
	IBuffer *inbuf;
	time_t internal_date;
	int failed, deleted;

	inbuf = index->open_mail(index, rec, &internal_date, &deleted);
	if (inbuf == NULL)
		return FALSE;

	if (ctx->copy_inside_mailbox) {
                /* kludgy.. */
		dest_ibox = (IndexMailbox *) ctx->dest;
		dest_ibox->delay_save_unlocking = TRUE;
	}

	/* save it in destination mailbox */
	failed = !ctx->dest->save(ctx->dest, rec->msg_flags,
				  ctx->custom_flags, internal_date, 0,
				  inbuf, inbuf->v_limit);

	if (ctx->copy_inside_mailbox)
		dest_ibox->delay_save_unlocking = FALSE;

	i_buffer_unref(inbuf);
	return !failed;
}

int index_storage_copy(Mailbox *box, Mailbox *destbox,
		       const char *messageset, int uidset)
{
	IndexMailbox *ibox = (IndexMailbox *) box;
        CopyContext ctx;
	MailLockType lock_type;
	int failed;

	if (destbox->readonly) {
		mail_storage_set_error(box->storage,
				       "Destination mailbox is read-only");
		return FALSE;
	}

	ctx.copy_inside_mailbox =
		destbox->storage == box->storage &&
		strcmp(destbox->name, box->name) == 0;

	if (ctx.copy_inside_mailbox) {
		/* copying inside same mailbox */
		if (!ibox->index->set_lock(ibox->index, MAIL_LOCK_EXCLUSIVE))
			return mail_storage_set_index_error(ibox);

		lock_type = MAIL_LOCK_EXCLUSIVE;
	} else {
		lock_type = MAIL_LOCK_SHARED;
	}

	if (!index_storage_sync_and_lock(ibox, TRUE, lock_type))
		return FALSE;

	ctx.custom_flags =
		mail_custom_flags_list_get(ibox->index->custom_flags);
	ctx.dest = destbox;

	failed = index_messageset_foreach(ibox, messageset, uidset,
					  copy_func, &ctx) <= 0;

	if (!ibox->index->set_lock(ibox->index, MAIL_LOCK_UNLOCK))
		return mail_storage_set_index_error(ibox);

	return !failed;
}
