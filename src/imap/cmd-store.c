/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "str.h"
#include "commands.h"
#include "imap-search.h"
#include "imap-util.h"

static int get_modify_type(struct client_command_context *cmd, const char *item,
			   enum modify_type *modify_type, int *silent)
{
	if (*item == '+') {
		*modify_type = MODIFY_ADD;
		item++;
	} else if (*item == '-') {
		*modify_type = MODIFY_REMOVE;
		item++;
	} else {
		*modify_type = MODIFY_REPLACE;
	}

	if (strncasecmp(item, "FLAGS", 5) != 0) {
		client_send_tagline(cmd, t_strconcat(
			"NO Invalid item ", item, NULL));
		return FALSE;
	}

	*silent = strcasecmp(item+5, ".SILENT") == 0;
	if (!*silent && item[5] != '\0') {
		client_send_tagline(cmd, t_strconcat(
			"NO Invalid item ", item, NULL));
		return FALSE;
	}

	return TRUE;
}

int cmd_store(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct imap_arg *args;
	enum mail_flags flags;
	const char *const *keywords_list;
	struct mail_keywords *keywords;
	enum modify_type modify_type;
	struct mailbox *box;
	struct mail_search_arg *search_arg;
	struct mail_search_context *search_ctx;
        struct mailbox_transaction_context *t;
	struct mail *mail;
	const char *messageset, *item;
	int silent, failed;

	if (!client_read_args(cmd, 0, 0, &args))
		return FALSE;

	if (!client_verify_open_mailbox(cmd))
		return TRUE;

	/* validate arguments */
	messageset = imap_arg_string(&args[0]);
	item = imap_arg_string(&args[1]);

	if (messageset == NULL || item == NULL) {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	if (!get_modify_type(cmd, item, &modify_type, &silent))
		return TRUE;

	if (args[2].type == IMAP_ARG_LIST) {
		if (!client_parse_mail_flags(cmd,
					     IMAP_ARG_LIST(&args[2])->args,
					     &flags, &keywords_list))
			return TRUE;
	} else {
		if (!client_parse_mail_flags(cmd, args+2,
					     &flags, &keywords_list))
			return TRUE;
	}

	box = client->mailbox;
	search_arg = imap_search_get_arg(cmd, messageset, cmd->uid);
	if (search_arg == NULL)
		return TRUE;

	t = mailbox_transaction_begin(box, silent);
	keywords = keywords_list != NULL || modify_type == MODIFY_REPLACE ?
		mailbox_keywords_create(t, keywords_list) : NULL;
	search_ctx = mailbox_search_init(t, NULL, search_arg, NULL,
					 MAIL_FETCH_FLAGS, NULL);

	failed = FALSE;
	while ((mail = mailbox_search_next(search_ctx)) != NULL) {
		if (modify_type == MODIFY_REPLACE || flags != 0) {
			if (mail->update_flags(mail, modify_type, flags) < 0) {
				failed = TRUE;
				break;
			}
		}
		if (modify_type == MODIFY_REPLACE || keywords != NULL) {
			if (mail->update_keywords(mail, modify_type,
						  keywords) < 0) {
				failed = TRUE;
				break;
			}
		}
	}

	if (keywords != NULL)
		mailbox_keywords_free(t, keywords);

	if (mailbox_search_deinit(search_ctx) < 0)
		failed = TRUE;

	if (failed)
		mailbox_transaction_rollback(t);
	else {
		if (mailbox_transaction_commit(t, 0) < 0)
			failed = TRUE;
	}

	if (!failed) {
		return cmd_sync(cmd, MAILBOX_SYNC_FLAG_FAST |
				(cmd->uid ? 0 : MAILBOX_SYNC_FLAG_NO_EXPUNGES),
				"OK Store completed.");
	} else {
		client_send_storage_error(cmd, mailbox_get_storage(box));
		return TRUE;
	}
}
