/* Copyright (c) 2002-2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "mail-storage.h"
#include "mail-search.h"
#include "mail-search-build.h"
#include "imap-search.h"
#include "imap-parser.h"
#include "imap-messageset.h"

#include <stdlib.h>

struct search_build_data {
	pool_t pool;
        struct mailbox *box;
	const char *error;
};

struct mail_search_arg *
imap_search_args_build(pool_t pool, struct mailbox *box,
		       const struct imap_arg *args, const char **error_r)
{
	struct mail_search_arg *sargs;

	sargs = mail_search_build_from_imap_args(pool, args, error_r);
	if (sargs == NULL)
		return NULL;

	mail_search_args_init(sargs, box, TRUE);
	return sargs;
}

static bool
msgset_is_valid(ARRAY_TYPE(seq_range) *seqset, uint32_t messages_count)
{
	const struct seq_range *range;
	unsigned int count;

	/* when there are no messages, all messagesets are invalid.
	   if there's at least one message:
	    - * gives seq1 = seq2 = (uint32_t)-1
	    - n:* should work if n <= messages_count
	    - n:m or m should work if m <= messages_count
	*/
	range = array_get(seqset, &count);
	if (count == 0 || messages_count == 0)
		return FALSE;

	if (range[count-1].seq2 == (uint32_t)-1) {
		if (range[count-1].seq1 > messages_count &&
		    range[0].seq1 != (uint32_t)-1)
			return FALSE;
	} else {
		if (range[count-1].seq2 > messages_count)
			return FALSE;
	}
	return TRUE;
}

static int imap_search_get_msgset_arg(struct client_command_context *cmd,
				      const char *messageset,
				      struct mail_search_arg **arg_r,
				      const char **error_r)
{
	struct mail_search_arg *arg;

	arg = p_new(cmd->pool, struct mail_search_arg, 1);
	arg->type = SEARCH_SEQSET;
	p_array_init(&arg->value.seqset, cmd->pool, 16);
	if (imap_messageset_parse(&arg->value.seqset, messageset) < 0 ||
	    !msgset_is_valid(&arg->value.seqset, cmd->client->messages_count)) {
		*error_r = "Invalid messageset";
		return -1;
	}
	*arg_r = arg;
	return 0;
}

static int
imap_search_get_uidset_arg(pool_t pool, struct mailbox *box, const char *uidset,
			   struct mail_search_arg **arg_r, const char **error_r)
{
	struct mail_search_arg *arg;

	arg = p_new(pool, struct mail_search_arg, 1);
	arg->type = SEARCH_UIDSET;
	p_array_init(&arg->value.seqset, pool, 16);
	if (imap_messageset_parse(&arg->value.seqset, uidset) < 0) {
		*error_r = "Invalid uidset";
		return -1;
	}

	mail_search_args_init(arg, box, TRUE);
	*arg_r = arg;
	return 0;
}

struct mail_search_arg *
imap_search_get_arg(struct client_command_context *cmd,
		    const char *set, bool uid)
{
	struct mail_search_arg *search_arg = NULL;
	const char *error = NULL;
	int ret;

	if (!uid) {
		ret = imap_search_get_msgset_arg(cmd, set, &search_arg, &error);
	} else {
		ret = imap_search_get_uidset_arg(cmd->pool,
						 cmd->client->mailbox, set,
						 &search_arg, &error);
	}
	if (ret < 0) {
		client_send_command_error(cmd, error);
		return NULL;
	}

	return search_arg;
}
