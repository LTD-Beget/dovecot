/* Copyright (C) 2006-2007 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "file-lock.h"
#include "imap-match.h"
#include "mail-index.h"
#include "mail-storage.h"
#include "mailbox-tree.h"
#include "mailbox-list-index.h"
#include "index-mailbox-list.h"

#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

/* min 2 seconds */
#define MAILBOX_LIST_SYNC_SECS 2

struct index_mailbox_list_module index_mailbox_list_module =
	MODULE_CONTEXT_INIT(&mailbox_list_module_register);
static void (*index_next_hook_mailbox_list_created)(struct mailbox_list *list);

static int
index_mailbox_view_sync(struct index_mailbox_list_iterate_context *ctx)
{
	struct mail_index_view_sync_ctx *sync_ctx;
	struct mail_index_view_sync_rec sync_rec;
	int ret;

	if (mail_index_view_sync_begin(ctx->mail_view, 0,
				       &sync_ctx) < 0) {
		mailbox_list_set_internal_error(ctx->ctx.list);
		return -1;
	}

	while ((ret = mail_index_view_sync_next(sync_ctx, &sync_rec)) > 0) ;

	mail_index_view_sync_end(&sync_ctx);
	return ret;
}

static int
index_mailbox_list_is_synced(struct index_mailbox_list_iterate_context *ctx)
{
	const struct mail_index_header *hdr;
	struct stat st;
	const char *path = ctx->ctx.list->set.root_dir;

	if (index_mailbox_view_sync(ctx) < 0)
		return -1;

	/* FIXME: single sync_stamp works only with maildir++ */
	if (stat(path, &st) < 0) {
		mailbox_list_set_critical(ctx->ctx.list,
					  "stat(%s) failed: %m", path);
		return -1;
	}
	/*
	   if mtime is older than 2 secs, we set the first bit on
	   if mtime is 0-2 secs old, we set the first bit off.

	   this way we'll always do a resync later when syncing a recently
	   changed directory. if the directory changes while we're syncing it
	   we'll resync it again later.

	   this would work with 1 second difference if we didn't store the
	   dirtyness flag in the stamp's first bit.
	*/
	if (st.st_mtime < ioloop_time - MAILBOX_LIST_SYNC_SECS)
		st.st_mtime |= 1;
	else
		st.st_mtime &= ~1;

	ctx->sync_stamp = st.st_mtime;

	hdr = mail_index_get_header(ctx->mail_view);
	return hdr->sync_stamp == ctx->sync_stamp;
}

static void pattern_parse(struct mailbox_list *list, const char *pattern,
			  const char **prefix_r, int *recurse_level_r)
{
	char sep = list->hierarchy_sep;
	const char *prefix_start, *prefix_end;
	bool seen_wildcards = FALSE;
	int recurse_level = 0;

	prefix_start = prefix_end = pattern;
	for (; *pattern != '\0'; pattern++) {
		if (*pattern == '%')
			seen_wildcards = TRUE;
		else if (*pattern == '*') {
			recurse_level = -1;
			break;
		}

		if (*pattern == sep) {
			if (!seen_wildcards)
				prefix_end = pattern;
			recurse_level++;
		}
	}

	*prefix_r = prefix_start == prefix_end ? "" :
		t_strdup_until(prefix_start, prefix_end);
	*recurse_level_r = recurse_level;
}

static struct mailbox_list_iterate_context *
index_mailbox_list_iter_init(struct mailbox_list *list,
			     const char *const *patterns,
			     enum mailbox_list_iter_flags flags)
{
	struct index_mailbox_list *ilist = INDEX_LIST_CONTEXT(list);
	struct index_mailbox_list_iterate_context *ctx;
	enum mailbox_list_sync_flags sync_flags;
	const char *prefix;
	int recurse_level;

	ctx = i_new(struct index_mailbox_list_iterate_context, 1);
	ctx->ctx.list = list;
	ctx->ctx.flags = flags;
	ctx->glob = imap_match_init(default_pool, patterns[0], TRUE, // FIXME
				    list->hierarchy_sep);

	ctx->mail_view = mail_index_view_open(ilist->mail_index);
	ctx->view = mailbox_list_index_view_init(ilist->list_index,
						 ctx->mail_view);
	if ((flags & MAILBOX_LIST_ITER_RAW_LIST) != 0) {
		/* Ignore indexes completely */
		ctx->backend_ctx = ilist->module_ctx.super.
			iter_init(list, patterns, flags);
	} else if (index_mailbox_list_is_synced(ctx) > 0) {
		/* synced, list from index */
		pattern_parse(list, patterns[0], &prefix, &recurse_level); // FIXME

		ctx->info_pool =
			pool_alloconly_create("mailbox name pool", 128);
		ctx->iter_ctx = mailbox_list_index_iterate_init(ctx->view,
								prefix,
								recurse_level);
		ctx->recurse_level = recurse_level;
		ctx->prefix = *prefix == '\0' ? i_strdup("") :
			i_strdup_printf("%s%c", prefix, list->hierarchy_sep);
	} else {
		/* FIXME: this works nicely with maildir++, but not others */
		sync_flags = MAILBOX_LIST_SYNC_FLAG_RECURSIVE;

		if (strchr(patterns[0], '*') != NULL) // FIXME
			ctx->recurse_level = -1;
		else {
			ctx->mailbox_tree =
				mailbox_tree_init(list->hierarchy_sep);
		}

		ctx->info_pool =
			pool_alloconly_create("mailbox name pool", 128);
		if (mailbox_list_index_sync_init(ilist->list_index, "",
						 sync_flags,
						 &ctx->sync_ctx) == 0) {
			//FIXME:patterns = "*";
			prefix = "";
			ctx->trans = mailbox_list_index_sync_get_transaction(
								ctx->sync_ctx);
		}

		ctx->backend_ctx = ilist->module_ctx.super.
			iter_init(list, patterns, flags);
	}
	return &ctx->ctx;
}

static enum mailbox_info_flags
index_mailbox_list_index_flags_translate(enum mailbox_list_index_flags flags)
{
	enum mailbox_info_flags info_flags = 0;

	if ((flags & MAILBOX_LIST_INDEX_FLAG_CHILDREN) != 0)
		info_flags |= MAILBOX_CHILDREN;
	if ((flags & MAILBOX_LIST_INDEX_FLAG_NOCHILDREN) != 0)
		info_flags |= MAILBOX_NOCHILDREN;

	if ((flags & MAILBOX_LIST_INDEX_FLAG_NONEXISTENT) != 0)
		info_flags |= MAILBOX_NONEXISTENT;
	if ((flags & MAILBOX_LIST_INDEX_FLAG_NOSELECT) != 0)
		info_flags |= MAILBOX_NOSELECT;
	return info_flags;
}

static enum mailbox_list_index_flags
index_mailbox_list_info_flags_translate(enum mailbox_info_flags info_flags)
{
	enum mailbox_list_index_flags flags = 0;

	if ((info_flags & MAILBOX_CHILDREN) != 0)
		flags |= MAILBOX_LIST_INDEX_FLAG_CHILDREN;
	if ((info_flags & MAILBOX_NOCHILDREN) != 0)
		flags |= MAILBOX_LIST_INDEX_FLAG_NOCHILDREN;

	if ((info_flags & MAILBOX_NONEXISTENT) != 0)
		flags |= MAILBOX_LIST_INDEX_FLAG_NONEXISTENT;
	if ((info_flags & MAILBOX_NOSELECT) != 0)
		flags |= MAILBOX_LIST_INDEX_FLAG_NOSELECT;
	return flags;
}

/* skip nonexistent mailboxes when finding with "*" */
#define info_flags_match(ctx, info) \
	(((info)->flags & MAILBOX_NONEXISTENT) == 0 || \
	 (ctx)->recurse_level >= 0)

static int iter_next_nonsync(struct index_mailbox_list_iterate_context *ctx,
			     const struct mailbox_info **info_r)
{
	struct index_mailbox_list *ilist = INDEX_LIST_CONTEXT(ctx->ctx.list);
	struct mailbox_list_index_info iinfo;
	const struct mail_index_record *rec;
	uint32_t seq;
	int ret;

	/* find the next matching mailbox */
	do {
		p_clear(ctx->info_pool);
		ret = mailbox_list_index_iterate_next(ctx->iter_ctx, &iinfo);
		if (ret <= 0) {
			*info_r = NULL;
			return ret;
		}

		ctx->info.name = *ctx->prefix == '\0' ? iinfo.name :
			p_strconcat(ctx->info_pool, ctx->prefix,
				    iinfo.name, NULL);
	} while (imap_match(ctx->glob, ctx->info.name) != IMAP_MATCH_YES);

	/* get the mailbox's flags */
	if (mail_index_lookup_uid_range(ctx->mail_view, iinfo.uid, iinfo.uid,
					&seq, &seq) < 0)
		return -1;
	if (seq == 0) {
		mailbox_list_index_set_corrupted(ilist->list_index,
			"Desynced: Record expunged from mail index");
		return -1;
	}

	if (mail_index_lookup(ctx->mail_view, seq, &rec) < 0)
		return -1;
	ctx->info.flags = index_mailbox_list_index_flags_translate(rec->flags);

	/* do some sanity checks to the flags */
	if ((ctx->info.flags & MAILBOX_CHILDREN) != 0 &&
	    (ctx->info.flags & MAILBOX_NOCHILDREN) != 0) {
		mailbox_list_index_set_corrupted(ilist->list_index,
			"Mail index has both children and nochildren flags");
		return -1;
	}
	if ((ctx->info.flags & MAILBOX_NOCHILDREN) != 0 &&
	    iinfo.has_children) {
		mailbox_list_index_set_corrupted(ilist->list_index,
			"Desynced: Children flags wrong in mail index");
		return -1;
	}

	if (!info_flags_match(ctx, &ctx->info))
		return iter_next_nonsync(ctx, info_r);

	*info_r = &ctx->info;
	return 0;
}

static struct mailbox_info *
mailbox_info_move_to_parent(struct index_mailbox_list_iterate_context *ctx,
			    const struct mailbox_info *info)
{
	const char *p, *name = info->name;
	char sep = ctx->ctx.list->hierarchy_sep;

	p_clear(ctx->info_pool);

	t_push();
	while ((p = strrchr(name, sep)) != NULL) {
		name = t_strdup_until(name, p);
		if (imap_match(ctx->glob, name) == IMAP_MATCH_YES) {
			ctx->info.name = p_strdup(ctx->info_pool, name);
			break;
		}
	}
	t_pop();

	if (p == NULL)
		i_unreached();

	ctx->info.flags = MAILBOX_CHILDREN | MAILBOX_NONEXISTENT;
	return &ctx->info;
}

static const struct mailbox_info *
index_mailbox_list_iter_next(struct mailbox_list_iterate_context *_ctx)
{
	struct index_mailbox_list_iterate_context *ctx =
		(struct index_mailbox_list_iterate_context *)_ctx;
	struct index_mailbox_list *ilist = INDEX_LIST_CONTEXT(_ctx->list);
	const struct mailbox_info *info;
	uint32_t seq, flags;
	enum imap_match_result match;

	if (ctx->iter_ctx != NULL) {
		if (iter_next_nonsync(ctx, &info) < 0) {
			ctx->failed = TRUE;
			return NULL;
		}
		return info;
	}

	do {
		info = ilist->module_ctx.super.iter_next(ctx->backend_ctx);
		if (info == NULL || ctx->sync_ctx == NULL)
			return info;

		/* if the sync fails, just ignore it. we don't require synced
		   indexes to be able to return valid output. */
		if (mailbox_list_index_sync_more(ctx->sync_ctx, info->name,
						 &seq) == 0) {
			flags = index_mailbox_list_info_flags_translate(
								info->flags);
			mail_index_update_flags(ctx->trans, seq, MODIFY_REPLACE,
						flags);
		}
		match = imap_match(ctx->glob, info->name);
		if ((match & IMAP_MATCH_PARENT) != 0) {
			info = mailbox_info_move_to_parent(ctx, info);
			match = IMAP_MATCH_YES;
		}
		if (ctx->recurse_level >= 0) {
			/* no "*" wildcards, keep track of what mailboxes we
			   have returned so we don't return same parents
			   multiple times. */
			bool created;

			(void)mailbox_tree_get(ctx->mailbox_tree,
					       info->name, &created);
			if (!created)
				match = IMAP_MATCH_NO;
		}
	} while (match != IMAP_MATCH_YES || !info_flags_match(ctx, info));

	return info;
}

static int
index_mailbox_list_iter_deinit(struct mailbox_list_iterate_context *_ctx)
{
	struct index_mailbox_list_iterate_context *ctx =
		(struct index_mailbox_list_iterate_context *)_ctx;
	struct index_mailbox_list *ilist = INDEX_LIST_CONTEXT(_ctx->list);
	int ret = ctx->failed ? -1 : 0;

	if (ctx->iter_ctx != NULL)
		mailbox_list_index_iterate_deinit(&ctx->iter_ctx);
	if (ctx->info_pool != NULL)
		pool_unref(ctx->info_pool);
	if (ctx->mailbox_tree != NULL)
		mailbox_tree_deinit(&ctx->mailbox_tree);

	if (ctx->mail_view != NULL)
		mail_index_view_close(&ctx->mail_view);
	if (ctx->view != NULL)
		mailbox_list_index_view_deinit(&ctx->view);

	if (ctx->sync_ctx != NULL) {
		/* FIXME: single sync_stamp works only with maildir++ */
		mail_index_update_header(ctx->trans,
			offsetof(struct mail_index_header, sync_stamp),
			&ctx->sync_stamp, sizeof(ctx->sync_stamp), TRUE);

		ret = ilist->module_ctx.super.iter_deinit(ctx->backend_ctx);
		if (ret < 0)
			mailbox_list_index_sync_rollback(&ctx->sync_ctx);
		else {
			/* index updates aren't that important. if the commit
			   fails, we've still returned full output. */
			(void)mailbox_list_index_sync_commit(&ctx->sync_ctx);
		}
	} else if (ctx->backend_ctx != NULL) {
		ret = ilist->module_ctx.super.iter_deinit(ctx->backend_ctx);
	}

	imap_match_deinit(&ctx->glob);
	i_free(ctx->prefix);
	i_free(ctx);
	return ret;
}

static void index_mailbox_list_deinit(struct mailbox_list *list)
{
	struct index_mailbox_list *ilist = INDEX_LIST_CONTEXT(list);

	mailbox_list_index_free(&ilist->list_index);
	mailbox_list_index_view_deinit(&ilist->list_sync_view);
	mail_index_free(&ilist->mail_index);

	ilist->module_ctx.super.deinit(list);
}

static int index_mailbox_list_open_indexes(struct mailbox_list *list,
					   const char *dir)
{
	struct index_mailbox_list *ilist = INDEX_LIST_CONTEXT(list);
	const char *path;
	enum mail_index_open_flags index_flags;
	enum mail_storage_flags storage_flags;
	int ret;

	/* FIXME: a bit ugly way to get the flags, but this will do for now.. */
	index_flags = MAIL_INDEX_OPEN_FLAG_CREATE;
	storage_flags = *list->set.mail_storage_flags;
#ifndef MMAP_CONFLICTS_WRITE
	if ((storage_flags & MAIL_STORAGE_FLAG_MMAP_DISABLE) != 0)
#endif
		index_flags |= MAIL_INDEX_OPEN_FLAG_MMAP_DISABLE;

	if (mail_index_open(ilist->mail_index, index_flags,
			    *list->set.lock_method) < 0) {
		if (mail_index_move_to_memory(ilist->mail_index) < 0) {
			/* try opening once more. it should be created
			   directly into memory now. */
			ret = mail_index_open(ilist->mail_index, index_flags,
					      *list->set.lock_method);
			if (ret <= 0) {
				/* everything failed. there's a bug in the
				   code, but just work around it by disabling
				   the index completely */
				return -1;
			}
		}
	}

	path = t_strconcat(dir, "/"MAILBOX_LIST_INDEX_NAME, NULL);
	ilist->list_index = mailbox_list_index_alloc(path, list->hierarchy_sep,
						     ilist->mail_index);
	if (mailbox_list_index_open_or_create(ilist->list_index) < 0) {
		/* skip indexing */
		mailbox_list_index_free(&ilist->list_index);
		return -1;
	}
	ilist->list_sync_view = mailbox_list_index_view_init(ilist->list_index,
							     NULL);
	return 0;
}

static void index_mailbox_list_created(struct mailbox_list *list)
{
	struct index_mailbox_list *ilist = NULL;
	const char *dir;

	/* FIXME: for now we only work with maildir++ */
	dir = mailbox_list_get_path(list, NULL, MAILBOX_LIST_PATH_TYPE_INDEX);
	if (*dir == '\0' || getenv("MAILBOX_LIST_INDEX_DISABLE") != NULL ||
	    strcmp(list->name, "maildir++") != 0) {
		/* reserve the module context anyway, so syncing code knows
		   that the index is disabled */
		MODULE_CONTEXT_SET(list, index_mailbox_list_module, ilist);
		return;
	}

	ilist = p_new(list->pool, struct index_mailbox_list, 1);
	ilist->module_ctx.super = list->v;

	list->v.deinit = index_mailbox_list_deinit;
	list->v.iter_init = index_mailbox_list_iter_init;
	list->v.iter_deinit = index_mailbox_list_iter_deinit;
	list->v.iter_next = index_mailbox_list_iter_next;
	MODULE_CONTEXT_SET(list, index_mailbox_list_module, ilist);

	ilist->mail_index = mail_index_alloc(dir, MAIL_INDEX_PREFIX);

	/* sync_init allocates the extensions. do it here before opening the
	   index files, so that our initial memory pool size guesses are a
	   bit more optimal */
	index_mailbox_list_sync_init_list(list);

	if (index_mailbox_list_open_indexes(list, dir) < 0) {
		list->v = ilist->module_ctx.super;
		mail_index_free(&ilist->mail_index);
		MODULE_CONTEXT_UNSET(list, index_mailbox_list_module);
	}
}

void index_mailbox_list_init(void); /* called in mailbox-list-register.c */

void index_mailbox_list_init(void)
{
	index_next_hook_mailbox_list_created = hook_mailbox_list_created;
	hook_mailbox_list_created = index_mailbox_list_created;

	index_mailbox_list_sync_init();
}
