/* Copyright (C) 2004 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "mbox-storage.h"
#include "mbox-lock.h"
#include "mbox-sync-private.h"

static void (*next_hook_mail_index_transaction_created)
	(struct mail_index_transaction *t) = NULL;

static int mbox_transaction_commit(struct mail_index_transaction *t,
				   uint32_t *log_file_seq_r,
				   uoff_t *log_file_offset_r)
{
	struct mbox_transaction_context *mt = MAIL_STORAGE_CONTEXT(t);
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mt->ictx.ibox;
	unsigned int lock_id = mt->mbox_lock_id;
	enum mailbox_sync_flags flags = mt->ictx.commit_flags;
	bool mbox_modified;
	bool syncing = t->sync_transaction;
	int ret = 0;

	if (mt->save_ctx != NULL)
		ret = mbox_transaction_save_commit(mt->save_ctx);
	mbox_modified = mt->mbox_modified;

	if (ret < 0)
		index_transaction_finish_rollback(&mt->ictx);
	else {
		if (index_transaction_finish_commit(&mt->ictx, log_file_seq_r,
						    log_file_offset_r) < 0)
			ret = -1;
	}

	/* transaction is destroyed now. */
	mt = NULL;

	if (lock_id != 0 && mbox->mbox_lock_type != F_WRLCK) {
		/* unlock before writing any changes */
		(void)mbox_unlock(mbox, lock_id);
		lock_id = 0;
	}

	if (ret == 0 && !syncing) {
		enum mbox_sync_flags mbox_sync_flags = MBOX_SYNC_LAST_COMMIT;
		if ((flags & MAILBOX_SYNC_FLAG_FULL_READ) != 0 &&
		    !mbox->mbox_very_dirty_syncs)
			mbox_sync_flags |= MBOX_SYNC_UNDIRTY;
		if ((flags & MAILBOX_SYNC_FLAG_FULL_WRITE) != 0)
			mbox_sync_flags |= MBOX_SYNC_REWRITE;
		if (mbox_modified) {
			/* after saving mails we want to update the last-uid */
			mbox_sync_flags |= MBOX_SYNC_HEADER | MBOX_SYNC_REWRITE;
		}
		if (mbox_sync(mbox, mbox_sync_flags) < 0)
			ret = -1;
	}

	if (lock_id != 0) {
		if (mbox_unlock(mbox, lock_id) < 0)
			ret = -1;
	}
	return ret;
}

static void mbox_transaction_rollback(struct mail_index_transaction *t)
{
	struct mbox_transaction_context *mt = MAIL_STORAGE_CONTEXT(t);
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mt->ictx.ibox;

	if (mt->save_ctx != NULL)
		mbox_transaction_save_rollback(mt->save_ctx);

	if (mt->mbox_lock_id != 0)
		(void)mbox_unlock(mbox, mt->mbox_lock_id);
	index_transaction_finish_rollback(&mt->ictx);
}

static void mbox_transaction_created(struct mail_index_transaction *t)
{
	struct mailbox *box = MAIL_STORAGE_CONTEXT(t->view);

	/* index can be for mailbox list index, in which case box=NULL */
	if (box != NULL && strcmp(box->storage->name, MBOX_STORAGE_NAME) == 0) {
		struct mbox_mailbox *mbox = (struct mbox_mailbox *)box;
		struct mbox_transaction_context *mt;

		mt = i_new(struct mbox_transaction_context, 1);
		mt->ictx.trans = t;
		mt->ictx.super = t->v;

		t->v.commit = mbox_transaction_commit;
		t->v.rollback = mbox_transaction_rollback;
		MODULE_CONTEXT_SET(t, mail_storage_mail_index_module, mt);

		index_transaction_init(&mt->ictx, &mbox->ibox);
	}

	if (next_hook_mail_index_transaction_created != NULL)
		next_hook_mail_index_transaction_created(t);
}

void mbox_transaction_class_init(void)
{
	next_hook_mail_index_transaction_created =
		hook_mail_index_transaction_created;
	hook_mail_index_transaction_created = mbox_transaction_created;
}

void mbox_transaction_class_deinit(void)
{
	i_assert(hook_mail_index_transaction_created ==
		 mbox_transaction_created);
	hook_mail_index_transaction_created =
		next_hook_mail_index_transaction_created;
}
