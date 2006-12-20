/* Copyright (C) 2006 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "mail-search.h"
#include "mail-storage.h"
#include "quota-private.h"

static int quota_count_mailbox(struct mail_storage *storage, const char *name,
			       uint64_t *bytes_r, uint64_t *count_r)
{
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
	struct mail_search_context *ctx;
	struct mail *mail;
	struct mail_search_arg search_arg;
	uoff_t size;
	int ret = 0;

	box = mailbox_open(storage, name, NULL,
			   MAILBOX_OPEN_READONLY | MAILBOX_OPEN_KEEP_RECENT);
	if (box == NULL)
		return -1;

	memset(&search_arg, 0, sizeof(search_arg));
	search_arg.type = SEARCH_ALL;

	trans = mailbox_transaction_begin(box, 0);
	ctx = mailbox_search_init(trans, NULL, &search_arg, NULL);
	mail = mail_alloc(trans, MAIL_FETCH_PHYSICAL_SIZE, NULL);
	while (mailbox_search_next(ctx, mail) > 0) {
		size = mail_get_physical_size(mail);
		if (size != (uoff_t)-1)
			*bytes_r += size;
		*count_r += 1;
	}
	mail_free(&mail);
	if (mailbox_search_deinit(&ctx) < 0)
		ret = -1;

	if (ret < 0)
		mailbox_transaction_rollback(&trans);
	else
		(void)mailbox_transaction_commit(&trans, 0);

	mailbox_close(&box);
	return ret;
}

static int quota_count_storage(struct mail_storage *storage,
			       uint64_t *bytes, uint64_t *count)
{
	struct mailbox_list_iterate_context *ctx;
	struct mailbox_info *info;
	int ret = 0;

	ctx = mailbox_list_iter_init(storage->list, "*",
				     MAILBOX_LIST_ITER_FAST_FLAGS);
	while ((info = mailbox_list_iter_next(ctx)) != NULL) {
		if ((info->flags & (MAILBOX_NONEXISTENT |
				    MAILBOX_NOSELECT)) == 0) {
			ret = quota_count_mailbox(storage, info->name,
						  bytes, count);
			if (ret < 0)
				break;
		}
	}
	if (mailbox_list_iter_deinit(&ctx) < 0)
		ret = -1;

	return ret;
}

int quota_count(struct quota *quota, uint64_t *bytes_r, uint64_t *count_r)
{
	struct mail_storage *const *storages;
	unsigned int i, count;
	int ret = 0;

	i_assert(!quota->counting);

	*bytes_r = *count_r = 0;

	quota->counting = TRUE;

	storages = array_get(&quota->storages, &count);
	for (i = 0; i < count; i++) {
		ret = quota_count_storage(storages[i], bytes_r, count_r);
		if (ret < 0)
			break;
	}
	quota->counting = FALSE;

	return ret;
}
