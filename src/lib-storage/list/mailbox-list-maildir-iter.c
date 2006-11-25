/* Copyright (C) 2002-2006 Timo Sirainen */

#include "lib.h"
#include "str.h"
#include "home-expand.h"
#include "imap-match.h"
#include "subscription-file.h"
#include "mailbox-tree.h"
#include "mailbox-list-maildir.h"

#include <dirent.h>

#define MAILBOX_FLAG_MATCHED 0x40000000

struct maildir_list_iterate_context {
	struct mailbox_list_iterate_context ctx;
	pool_t pool;

	const char *dir, *prefix;

        struct mailbox_tree_context *tree_ctx;

	string_t *node_path;
	size_t parent_pos;
	struct mailbox_node *root, *next_node;
	struct mailbox_info info;
};

static void maildir_nodes_fix(struct mailbox_node *node, bool is_subs)
{
	while (node != NULL) {
		if (node->children != NULL) {
			node->flags |= MAILBOX_CHILDREN;
			node->flags &= ~MAILBOX_NOCHILDREN;
			maildir_nodes_fix(node->children, is_subs);
		} else if ((node->flags & MAILBOX_NONEXISTENT) != 0) {
			if (!is_subs) {
				node->flags &= ~MAILBOX_NONEXISTENT;
				node->flags |= MAILBOX_NOSELECT;
			}
			node->flags |= MAILBOX_CHILDREN;
		}
		node = node->next;
	}
}

static int
maildir_fill_readdir(struct maildir_list_iterate_context *ctx,
		     struct imap_match_glob *glob, bool update_only)
{
	DIR *dirp;
	struct dirent *d;
	const char *p, *mailbox_c;
	string_t *mailbox;
	enum mailbox_info_flags flags;
	enum imap_match_result match;
	struct mailbox_node *node;
	bool created;
	char hierarchy_sep;
	int ret;

	dirp = opendir(ctx->dir);
	if (dirp == NULL) {
		if (errno != ENOENT) {
			mailbox_list_set_critical(ctx->ctx.list,
				"opendir(%s) failed: %m", ctx->dir);
			return -1;
		}
		return 0;
	}

	hierarchy_sep = ctx->ctx.list->hierarchy_sep;

	t_push();
	mailbox = t_str_new(PATH_MAX);
	while ((d = readdir(dirp)) != NULL) {
		const char *fname = d->d_name;

		if (fname[0] != hierarchy_sep)
			continue;

		/* skip . and .. */
		if (fname[0] == '.' &&
		    (fname[1] == '\0' || (fname[1] == '.' && fname[2] == '\0')))
			continue;

		/* make sure the mask matches */
		str_truncate(mailbox, 0);
		str_append(mailbox, ctx->prefix);
		str_append(mailbox, fname + 1);
                mailbox_c = str_c(mailbox);

		match = imap_match(glob, mailbox_c);

		if (match != IMAP_MATCH_YES &&
		    match != IMAP_MATCH_PARENT)
			continue;

		/* check if this is an actual mailbox */
		flags = 0;
		ret = ctx->ctx.list->callback(ctx->dir, fname,
					      mailbox_list_get_file_type(d),
					      ctx->ctx.flags, &flags,
					      ctx->ctx.list->context);
		if (ret < 0) {
			t_pop();
			return -1;
		}
		if (ret == 0)
			continue;

		if (match == IMAP_MATCH_PARENT) {
			t_push();
			while ((p = strrchr(mailbox_c,
					    hierarchy_sep)) != NULL) {
				str_truncate(mailbox, (size_t) (p-mailbox_c));
				mailbox_c = str_c(mailbox);
				if (imap_match(glob, mailbox_c) > 0)
					break;
			}
			i_assert(p != NULL);

			created = FALSE;
			node = update_only ?
				mailbox_tree_update(ctx->tree_ctx, mailbox_c) :
				mailbox_tree_get(ctx->tree_ctx,
						 mailbox_c, &created);
			if (node != NULL) {
				if (created)
					node->flags = MAILBOX_NONEXISTENT;

				node->flags |= MAILBOX_CHILDREN |
					MAILBOX_FLAG_MATCHED;
				node->flags &= ~MAILBOX_NOCHILDREN;
			}

			t_pop();
		} else {
			created = FALSE;
			node = update_only ?
				mailbox_tree_update(ctx->tree_ctx, mailbox_c) :
				mailbox_tree_get(ctx->tree_ctx,
						 mailbox_c, &created);

			if (node != NULL) {
				if (created)
					node->flags = MAILBOX_NOCHILDREN;
				node->flags &= ~MAILBOX_NONEXISTENT;
				node->flags |= MAILBOX_FLAG_MATCHED;
			}
		}
	}
	t_pop();

	if (closedir(dirp) < 0) {
		mailbox_list_set_critical(ctx->ctx.list,
					  "readdir(%s) failed: %m", ctx->dir);
		return -1;
	}

	if ((ctx->ctx.list->flags & MAILBOX_LIST_FLAG_INBOX) != 0 &&
	    (ctx->ctx.flags & MAILBOX_LIST_ITER_SUBSCRIBED) == 0) {
		/* make sure INBOX is there */
		node = mailbox_tree_get(ctx->tree_ctx, "INBOX", &created);
		if (created)
			node->flags = MAILBOX_NOCHILDREN;
		else
			node->flags &= ~MAILBOX_NONEXISTENT;

		switch (imap_match(glob, "INBOX")) {
		case IMAP_MATCH_YES:
		case IMAP_MATCH_PARENT:
			node->flags |= MAILBOX_FLAG_MATCHED;
			break;
		default:
			break;
		}
	}
	maildir_nodes_fix(mailbox_tree_get(ctx->tree_ctx, NULL, NULL),
			  (ctx->ctx.flags & MAILBOX_LIST_ITER_SUBSCRIBED) != 0);
	return 0;
}

static int maildir_fill_subscribed(struct maildir_list_iterate_context *ctx,
				   struct imap_match_glob *glob)
{
	struct subsfile_list_context *subsfile_ctx;
	const char *path, *name, *p;
	struct mailbox_node *node;
	char hierarchy_sep;
	bool created;

	path = t_strconcat(ctx->ctx.list->set.control_dir != NULL ?
			   ctx->ctx.list->set.control_dir :
			   ctx->ctx.list->set.root_dir,
			   "/", ctx->ctx.list->set.subscription_fname, NULL);
	subsfile_ctx = subsfile_list_init(ctx->ctx.list, path);

	hierarchy_sep = ctx->ctx.list->hierarchy_sep;
	while ((name = subsfile_list_next(subsfile_ctx)) != NULL) {
		switch (imap_match(glob, name)) {
		case IMAP_MATCH_YES:
			node = mailbox_tree_get(ctx->tree_ctx, name, NULL);
			node->flags = MAILBOX_FLAG_MATCHED;
			if ((ctx->ctx.flags &
			     MAILBOX_LIST_ITER_FAST_FLAGS) == 0) {
				node->flags |= MAILBOX_NONEXISTENT |
					MAILBOX_NOCHILDREN;
			}
			break;
		case IMAP_MATCH_PARENT:
			/* placeholder */
			while ((p = strrchr(name, hierarchy_sep)) != NULL) {
				name = t_strdup_until(name, p);
				if (imap_match(glob, name) > 0)
					break;
			}
			i_assert(p != NULL);

			node = mailbox_tree_get(ctx->tree_ctx, name, &created);
			if (created) node->flags = MAILBOX_NONEXISTENT;
			node->flags |= MAILBOX_FLAG_MATCHED | MAILBOX_CHILDREN;
			node->flags &= ~MAILBOX_NOCHILDREN;
			break;
		default:
			break;
		}
	}

	return subsfile_list_deinit(subsfile_ctx);
}

struct mailbox_list_iterate_context *
maildir_list_iter_init(struct mailbox_list *_list, const char *mask,
		       enum mailbox_list_iter_flags flags)
{
	struct maildir_mailbox_list *list =
		(struct maildir_mailbox_list *)_list;
	struct maildir_list_iterate_context *ctx;
        struct imap_match_glob *glob;
	const char *dir, *p;
	pool_t pool;

	mailbox_list_clear_error(&list->list);

	pool = pool_alloconly_create("maildir_list", 1024);
	ctx = p_new(pool, struct maildir_list_iterate_context, 1);
	ctx->ctx.list = _list;
	ctx->ctx.flags = flags;
	ctx->pool = pool;
	ctx->tree_ctx = mailbox_tree_init(_list->hierarchy_sep);

	glob = imap_match_init(pool, mask, TRUE, _list->hierarchy_sep);

	ctx->dir = _list->set.root_dir;
	ctx->prefix = "";

	if ((flags & MAILBOX_LIST_ITER_SUBSCRIBED) != 0) {
		if (maildir_fill_subscribed(ctx, glob) < 0) {
			ctx->ctx.failed = TRUE;
			return &ctx->ctx;
		}
	} else if ((list->list.flags & MAILBOX_LIST_FLAG_FULL_FS_ACCESS) != 0 &&
		   (p = strrchr(mask, '/')) != NULL) {
		dir = t_strdup_until(mask, p);
		ctx->prefix = p_strdup_until(pool, mask, p+1);

		if (*mask != '/' && *mask != '~')
			dir = t_strconcat(_list->set.root_dir, "/", dir, NULL);
		ctx->dir = p_strdup(pool, home_expand(dir));
	}

	if ((flags & MAILBOX_LIST_ITER_SUBSCRIBED) == 0 ||
	    (ctx->ctx.flags & MAILBOX_LIST_ITER_FAST_FLAGS) == 0) {
		bool update_only = (flags & MAILBOX_LIST_ITER_SUBSCRIBED) != 0;
		if (maildir_fill_readdir(ctx, glob, update_only) < 0) {
			ctx->ctx.failed = TRUE;
			return &ctx->ctx;
		}
	}

	ctx->node_path = str_new(pool, 256);
	ctx->root = mailbox_tree_get(ctx->tree_ctx, NULL, NULL);
	return &ctx->ctx;
}

int maildir_list_iter_deinit(struct mailbox_list_iterate_context *_ctx)
{
	struct maildir_list_iterate_context *ctx =
		(struct maildir_list_iterate_context *)_ctx;
	int ret = ctx->ctx.failed ? -1 : 0;

	mailbox_tree_deinit(ctx->tree_ctx);
	pool_unref(ctx->pool);
	return ret;
}

static struct mailbox_node *find_next(struct mailbox_node **node,
				      string_t *path, char hierarchy_sep)
{
	struct mailbox_node *child;
	size_t len;

	while (*node != NULL) {
		if (((*node)->flags & MAILBOX_FLAG_MATCHED) != 0)
			return *node;

		if ((*node)->children != NULL) {
			len = str_len(path);
			if (len != 0)
				str_append_c(path, hierarchy_sep);
			str_append(path, (*node)->name);

			child = find_next(&(*node)->children, path,
					  hierarchy_sep);
			if (child != NULL)
				return child;

			str_truncate(path, len);
		}

		*node = (*node)->next;
	}

	return NULL;
}

struct mailbox_info *
maildir_list_iter_next(struct mailbox_list_iterate_context *_ctx)
{
	struct maildir_list_iterate_context *ctx =
		(struct maildir_list_iterate_context *)_ctx;
	struct mailbox_node *node;

	for (node = ctx->next_node; node != NULL; node = node->next) {
		if ((node->flags & MAILBOX_FLAG_MATCHED) != 0)
			break;
	}

	if (node == NULL) {
		if (ctx->root == NULL)
			return NULL;

		str_truncate(ctx->node_path, 0);
		node = find_next(&ctx->root, ctx->node_path,
				 ctx->ctx.list->hierarchy_sep);
                ctx->parent_pos = str_len(ctx->node_path);

		if (node == NULL)
			return NULL;
	}
	ctx->next_node = node->next;

	i_assert((node->flags & MAILBOX_FLAG_MATCHED) != 0);
	node->flags &= ~MAILBOX_FLAG_MATCHED;

	str_truncate(ctx->node_path, ctx->parent_pos);
	if (ctx->parent_pos != 0)
		str_append_c(ctx->node_path, ctx->ctx.list->hierarchy_sep);
	str_append(ctx->node_path, node->name);

	ctx->info.name = str_c(ctx->node_path);
	ctx->info.flags = node->flags;
	return &ctx->info;
}
