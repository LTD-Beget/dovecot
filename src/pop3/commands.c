/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "var-expand.h"
#include "message-size.h"
#include "mail-storage.h"
#include "mail-search.h"
#include "capability.h"
#include "commands.h"

#define MSGS_BITMASK_SIZE(client) \
	((client->messages_count + (CHAR_BIT-1)) / CHAR_BIT)

static const char *get_msgnum(struct client *client, const char *args,
			      unsigned int *msgnum)
{
	unsigned int num, last_num;

	num = 0;
	while (*args != '\0' && *args != ' ') {
		if (*args < '0' || *args > '9') {
			client_send_line(client,
				"-ERR Invalid message number: %s", args);
			return NULL;
		}

		last_num = num;
		num = num*10 + (*args - '0');
		if (num < last_num) {
			client_send_line(client,
				"-ERR Message number too large: %s", args);
			return NULL;
		}
		args++;
	}

	if (num == 0 || num > client->messages_count) {
		client_send_line(client,
				 "-ERR There's no message %u.", num);
		return NULL;
	}
	num--;

	if (client->deleted) {
		if (client->deleted_bitmask[num / CHAR_BIT] &
		    (1 << (num % CHAR_BIT))) {
			client_send_line(client, "-ERR Message is deleted.");
			return NULL;
		}
	}

	while (*args == ' ') args++;

	*msgnum = num;
	return args;
}

static const char *get_size(struct client *client, const char *args,
			    uoff_t *size)
{
	uoff_t num, last_num;

	num = 0;
	while (*args != '\0' && *args != ' ') {
		if (*args < '0' || *args > '9') {
			client_send_line(client, "-ERR Invalid size: %s",
					 args);
			return NULL;
		}

		last_num = num;
		num = num*10 + (*args - '0');
		if (num < last_num) {
			client_send_line(client, "-ERR Size too large: %s",
					 args);
			return NULL;
		}
		args++;
	}

	while (*args == ' ') args++;

	*size = num;
	return args;
}

static int cmd_capa(struct client *client, const char *args __attr_unused__)
{
	client_send_line(client, "+OK\r\n"POP3_CAPABILITY_REPLY".");
	return 1;
}

static int cmd_dele(struct client *client, const char *args)
{
	unsigned int msgnum;

	if (get_msgnum(client, args, &msgnum) == NULL)
		return 0;

	if (!client->deleted) {
		client->deleted_bitmask = i_malloc(MSGS_BITMASK_SIZE(client));
		client->deleted = TRUE;
	}

	client->deleted_bitmask[msgnum / CHAR_BIT] |= 1 << (msgnum % CHAR_BIT);
	client->deleted_count++;
	client->deleted_size += client->message_sizes[msgnum];
	client_send_line(client, "+OK Marked to be deleted.");
	return 1;
}

struct cmd_list_context {
	unsigned int msgnum;
};

static void cmd_list_callback(struct client *client)
{
	struct cmd_list_context *ctx = client->cmd_context;
	int ret = 1;

	for (; ctx->msgnum != client->messages_count; ctx->msgnum++) {
		if (ret == 0) {
			/* buffer full */
			return;
		}

		if (client->deleted) {
			if (client->deleted_bitmask[ctx->msgnum / CHAR_BIT] &
			    (1 << (ctx->msgnum % CHAR_BIT)))
				continue;
		}
		ret = client_send_line(client, "%u %"PRIuUOFF_T,
				       ctx->msgnum+1,
				       client->message_sizes[ctx->msgnum]);
		if (ret < 0)
			break;
	}

	client_send_line(client, ".");

	i_free(ctx);
	client->cmd = NULL;
}

static int cmd_list(struct client *client, const char *args)
{
        struct cmd_list_context *ctx;

	if (*args == '\0') {
		ctx = i_new(struct cmd_list_context, 1);
		client_send_line(client, "+OK %u messages:",
				 client->messages_count - client->deleted_count);

		client->cmd = cmd_list_callback;
		client->cmd_context = ctx;
		cmd_list_callback(client);
	} else {
		unsigned int msgnum;

		if (get_msgnum(client, args, &msgnum) == NULL)
			return 0;

		client_send_line(client, "+OK %u %"PRIuUOFF_T, msgnum+1,
				 client->message_sizes[msgnum]);
	}

	return 1;
}

static int cmd_last(struct client *client, const char *args __attr_unused__)
{
	client_send_line(client, "+OK %u", client->last_seen);
	return 1;
}

static int cmd_noop(struct client *client, const char *args __attr_unused__)
{
	client_send_line(client, "+OK");
	return 1;
}

static bool expunge_mails(struct client *client)
{
	struct mail_search_arg search_arg;
        struct mail_search_seqset seqset;
	struct mail_search_context *ctx;
	struct mail *mail;
	uint32_t idx;
	bool ret = TRUE;

	if (client->deleted_bitmask == NULL)
		return TRUE;

	if (mailbox_is_readonly(client->mailbox)) {
		/* silently ignore */
		return TRUE;
	}

	memset(&seqset, 0, sizeof(seqset));
	memset(&search_arg, 0, sizeof(search_arg));
	seqset.seq1 = 1;
	seqset.seq2 = client->messages_count;
	search_arg.type = SEARCH_SEQSET;
	search_arg.value.seqset = &seqset;

	ctx = mailbox_search_init(client->trans, NULL, &search_arg, NULL);
	mail = mail_alloc(client->trans, 0, NULL);
	while (mailbox_search_next(ctx, mail) > 0) {
		idx = mail->seq - 1;
		if ((client->deleted_bitmask[idx / CHAR_BIT] &
		     1 << (idx % CHAR_BIT)) != 0) {
			if (mail_expunge(mail) < 0) {
				ret = FALSE;
				break;
			}
			client->expunged_count++;
		}
	}
	mail_free(&mail);

	if (mailbox_search_deinit(&ctx) < 0)
		ret = FALSE;
	return ret;
}

static int cmd_quit(struct client *client, const char *args __attr_unused__)
{
	if (client->deleted) {
		if (!expunge_mails(client)) {
			client_send_storage_error(client);
			client_disconnect(client,
				"Storage error during logout.");
			return 1;
		}
	}

	if (mailbox_transaction_commit(&client->trans,
				       MAILBOX_SYNC_FLAG_FULL_WRITE) < 0) {
		client_send_storage_error(client);
		client_disconnect(client, "Storage error during logout.");
		return 1;
	}

	if (!client->deleted)
		client_send_line(client, "+OK Logging out.");
	else
		client_send_line(client, "+OK Logging out, messages deleted.");

	client_disconnect(client, "Logged out");
	return 1;
}

struct fetch_context {
	struct mail_search_context *search_ctx;
	struct mail *mail;
	struct istream *stream;
	uoff_t body_lines;

	struct mail_search_arg search_arg;
        struct mail_search_seqset seqset;

	unsigned char last;
	bool cr_skipped, in_body;
};

static void fetch_deinit(struct fetch_context *ctx)
{
	(void)mailbox_search_deinit(&ctx->search_ctx);
	mail_free(&ctx->mail);
	i_free(ctx);
}

static void fetch_callback(struct client *client)
{
	struct fetch_context *ctx = client->cmd_context;
	const unsigned char *data;
	unsigned char add;
	size_t i, size;
	int ret;

	while ((ctx->body_lines > 0 || !ctx->in_body) &&
	       i_stream_read_data(ctx->stream, &data, &size, 0) > 0) {
		if (size > 4096)
			size = 4096;

		add = '\0';
		for (i = 0; i < size; i++) {
			if ((data[i] == '\r' || data[i] == '\n') &&
			    !ctx->in_body) {
				if (i == 0 && (ctx->last == '\0' ||
					       ctx->last == '\n'))
					ctx->in_body = TRUE;
				else if (i > 0 && data[i-1] == '\n')
					ctx->in_body = TRUE;
			}

			if (data[i] == '\n') {
				if ((i == 0 && ctx->last != '\r') ||
				    (i > 0 && data[i-1] != '\r')) {
					/* missing CR */
					add = '\r';
					break;
				}

				if (ctx->in_body) {
					if (--ctx->body_lines == 0) {
						i++;
						break;
					}
				}
			} else if (data[i] == '.' &&
				   ((i == 0 && ctx->last == '\n') ||
				    (i > 0 && data[i-1] == '\n'))) {
				/* escape the dot */
				add = '.';
				break;
			} else if (data[i] == '\0' &&
				   (client_workarounds &
				    WORKAROUND_OUTLOOK_NO_NULS) != 0) {
				add = 0x80;
				break;
			}
		}

		if (i > 0) {
			if (o_stream_send(client->output, data, i) < 0)
				break;
			ctx->last = data[i-1];
			i_stream_skip(ctx->stream, i);
		}

		if (o_stream_get_buffer_used_size(client->output) >= 4096) {
			if ((ret = o_stream_flush(client->output)) < 0)
				break;
			if (ret == 0) {
				/* continue later */
				return;
			}
		}

		if (add != '\0') {
			if (o_stream_send(client->output, &add, 1) < 0)
				break;

			ctx->last = add;
			if (add == 0x80)
				i_stream_skip(ctx->stream, 1);
		}
	}

	if (ctx->last != '\n') {
		/* didn't end with CRLF */
		(void)o_stream_send(client->output, "\r\n", 2);
	}

	if (!ctx->in_body && (client_workarounds & WORKAROUND_OE_NS_EOH) != 0) {
		/* Add the missing end of headers line. */
		(void)o_stream_send(client->output, "\r\n", 2);
	}

	*client->byte_counter +=
		client->output->offset - client->byte_counter_offset;
        client->byte_counter = NULL;

	client_send_line(client, ".");
	fetch_deinit(ctx);
	client->cmd = NULL;
}

static void fetch(struct client *client, unsigned int msgnum, uoff_t body_lines)
{
        struct fetch_context *ctx;

	ctx = i_new(struct fetch_context, 1);

	ctx->seqset.seq1 = ctx->seqset.seq2 = msgnum+1;
	ctx->search_arg.type = SEARCH_SEQSET;
	ctx->search_arg.value.seqset = &ctx->seqset;

	ctx->search_ctx = mailbox_search_init(client->trans, NULL,
					      &ctx->search_arg, NULL);
	ctx->mail = mail_alloc(client->trans, MAIL_FETCH_STREAM_HEADER |
			       MAIL_FETCH_STREAM_BODY, NULL);

	if (mailbox_search_next(ctx->search_ctx, ctx->mail) <= 0)
		ctx->stream = NULL;
	else
		ctx->stream = mail_get_stream(ctx->mail, NULL, NULL);

	if (ctx->stream == NULL) {
		client_send_line(client, "-ERR Message not found.");
		fetch_deinit(ctx);
		return;
	}

	if (body_lines == (uoff_t)-1 && !no_flag_updates) {
		if ((mail_get_flags(ctx->mail) & MAIL_SEEN) == 0) {
			/* mark the message seen with RETR command */
			(void)mail_update_flags(ctx->mail,
						MODIFY_ADD, MAIL_SEEN);
		}
	}

	ctx->body_lines = body_lines;
	if (body_lines == (uoff_t)-1) {
		client_send_line(client, "+OK %"PRIuUOFF_T" octets",
				 client->message_sizes[msgnum]);
	} else {
		client_send_line(client, "+OK");
		ctx->body_lines++; /* internally we count the empty line too */
	}

	client->cmd = fetch_callback;
	client->cmd_context = ctx;
	fetch_callback(client);
}

static int cmd_retr(struct client *client, const char *args)
{
	unsigned int msgnum;

	if (get_msgnum(client, args, &msgnum) == NULL)
		return 0;

	if (client->last_seen <= msgnum)
		client->last_seen = msgnum+1;

	client->retr_count++;
	client->byte_counter = &client->retr_bytes;
	client->byte_counter_offset = client->output->offset;

	fetch(client, msgnum, (uoff_t)-1);
	return 1;
}

static int cmd_rset(struct client *client, const char *args __attr_unused__)
{
	struct mail_search_context *search_ctx;
	struct mail *mail;
	struct mail_search_arg search_arg;
        struct mail_search_seqset seqset;

	client->last_seen = 0;

	if (client->deleted) {
		client->deleted = FALSE;
		memset(client->deleted_bitmask, 0, MSGS_BITMASK_SIZE(client));
		client->deleted_count = 0;
		client->deleted_size = 0;
	}

	if (enable_last_command) {
		/* remove all \Seen flags (as specified by RFC 1460) */
		memset(&seqset, 0, sizeof(seqset));
		memset(&search_arg, 0, sizeof(search_arg));
		seqset.seq1 = 1;
		seqset.seq2 = client->messages_count;
		search_arg.type = SEARCH_SEQSET;
		search_arg.value.seqset = &seqset;

		search_ctx = mailbox_search_init(client->trans, NULL,
						 &search_arg, NULL);
		mail = mail_alloc(client->trans, 0, NULL);
		while (mailbox_search_next(search_ctx, mail) > 0) {
			if (mail_update_flags(mail, MODIFY_REMOVE,
					      MAIL_SEEN) < 0)
				break;
		}
		mail_free(&mail);
		(void)mailbox_search_deinit(&search_ctx);
	} else {
		/* forget all our seen flag updates.
		   FIXME: is this needed? it loses data added to cache file */
		mailbox_transaction_rollback(&client->trans);
		client->trans = mailbox_transaction_begin(client->mailbox, 0);
	}

	client_send_line(client, "+OK");
	return 1;
}

static int cmd_stat(struct client *client, const char *args __attr_unused__)
{
	client_send_line(client, "+OK %u %"PRIuUOFF_T, client->
			 messages_count - client->deleted_count,
			 client->total_size - client->deleted_size);
	return 1;
}

static int cmd_top(struct client *client, const char *args)
{
	unsigned int msgnum;
	uoff_t max_lines;

	args = get_msgnum(client, args, &msgnum);
	if (args == NULL)
		return 0;
	if (get_size(client, args, &max_lines) == NULL)
		return 0;

	client->top_count++;
	client->byte_counter = &client->top_bytes;
	client->byte_counter_offset = client->output->offset;

	fetch(client, msgnum, max_lines);
	return 1;
}

struct cmd_uidl_context {
	struct mail_search_context *search_ctx;
	struct mail *mail;
	unsigned int message;

	struct mail_search_arg search_arg;
	struct mail_search_seqset seqset;
};

static bool list_uids_iter(struct client *client, struct cmd_uidl_context *ctx)
{
	static struct var_expand_table static_tab[] = {
		{ 'v', NULL },
		{ 'u', NULL },
		{ 'm', NULL },
		{ 'f', NULL },
		{ '\0', NULL }
	};
	struct var_expand_table *tab;
	string_t *str;
	const char *uidl;
	int ret;
	bool found = FALSE;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));
	tab[0].value = t_strdup_printf("%u", client->uid_validity);

	str = str_new(default_pool, 128);
	while (mailbox_search_next(ctx->search_ctx, ctx->mail) > 0) {
		if (client->deleted) {
			uint32_t idx = ctx->mail->seq - 1;
			if (client->deleted_bitmask[idx / CHAR_BIT] &
			    (1 << (idx % CHAR_BIT)))
				continue;
		}
		found = TRUE;

		t_push();
		if ((uidl_keymask & UIDL_UID) != 0)
			tab[1].value = dec2str(ctx->mail->uid);
		if ((uidl_keymask & UIDL_MD5) != 0) {
			tab[2].value = mail_get_special(ctx->mail,
							MAIL_FETCH_HEADER_MD5);
			if (tab[2].value == NULL) {
				/* broken */
				i_fatal("UIDL: Header MD5 not found");
			}
		}
		if ((uidl_keymask & UIDL_FILE_NAME) != 0) {
			tab[3].value =
				mail_get_special(ctx->mail,
						 MAIL_FETCH_UIDL_FILE_NAME);
			if (tab[3].value == NULL) {
				/* broken */
				i_fatal("UIDL: File name not found");
			}
		}

		str_truncate(str, 0);
		str_printfa(str, ctx->message == 0 ? "%u " : "+OK %u ",
			    ctx->mail->seq);

		uidl = !reuse_xuidl ? NULL :
			mail_get_first_header(ctx->mail, "X-UIDL");
		if (uidl == NULL)
			var_expand(str, uidl_format, tab);
		else
			str_append(str, uidl);
		ret = client_send_line(client, "%s", str_c(str));
		t_pop();

		if (ret < 0)
			break;
		if (ret == 0 && ctx->message == 0) {
			/* output is being buffered, continue when there's
			   more space */
			str_free(&str);
			return 0;
		}
	}
	str_free(&str);

	/* finished */
	mail_free(&ctx->mail);
	(void)mailbox_search_deinit(&ctx->search_ctx);

	client->cmd = NULL;

	if (ctx->message == 0)
		client_send_line(client, ".");
	i_free(ctx);
	return found;
}

static void cmd_uidl_callback(struct client *client)
{
	struct cmd_uidl_context *ctx = client->cmd_context;

        (void)list_uids_iter(client, ctx);
}

static struct cmd_uidl_context *
cmd_uidl_init(struct client *client, unsigned int message)
{
        struct cmd_uidl_context *ctx;
	enum mail_fetch_field wanted_fields;

	ctx = i_new(struct cmd_uidl_context, 1);

	if (message == 0) {
		ctx->seqset.seq1 = 1;
		ctx->seqset.seq2 = client->messages_count;
	} else {
		ctx->message = message;
		ctx->seqset.seq1 = ctx->seqset.seq2 = message;
	}
	ctx->search_arg.type = SEARCH_SEQSET;
	ctx->search_arg.value.seqset = &ctx->seqset;

	wanted_fields = 0;
	if ((uidl_keymask & UIDL_MD5) != 0)
		wanted_fields |= MAIL_FETCH_HEADER_MD5;

	ctx->search_ctx = mailbox_search_init(client->trans, NULL,
					      &ctx->search_arg, NULL);
	ctx->mail = mail_alloc(client->trans, wanted_fields, NULL);
	if (message == 0) {
		client->cmd = cmd_uidl_callback;
		client->cmd_context = ctx;
	}
	return ctx;
}

static int cmd_uidl(struct client *client, const char *args)
{
        struct cmd_uidl_context *ctx;

	if (*args == '\0') {
		client_send_line(client, "+OK");
		ctx = cmd_uidl_init(client, 0);
		list_uids_iter(client, ctx);
	} else {
		unsigned int msgnum;

		if (get_msgnum(client, args, &msgnum) == NULL)
			return 0;

		ctx = cmd_uidl_init(client, msgnum+1);
		if (!list_uids_iter(client, ctx))
			client_send_line(client, "-ERR Message not found.");
	}

	return 1;
}

int client_command_execute(struct client *client,
			   const char *name, const char *args)
{
	/* keep the command uppercased */
	name = t_str_ucase(name);

	while (*args == ' ') args++;

	switch (*name) {
	case 'C':
		if (strcmp(name, "CAPA") == 0)
			return cmd_capa(client, args);
		break;
	case 'D':
		if (strcmp(name, "DELE") == 0)
			return cmd_dele(client, args);
		break;
	case 'L':
		if (strcmp(name, "LIST") == 0)
			return cmd_list(client, args);
		if (strcmp(name, "LAST") == 0 && enable_last_command)
			return cmd_last(client, args);
		break;
	case 'N':
		if (strcmp(name, "NOOP") == 0)
			return cmd_noop(client, args);
		break;
	case 'Q':
		if (strcmp(name, "QUIT") == 0)
			return cmd_quit(client, args);
		break;
	case 'R':
		if (strcmp(name, "RETR") == 0)
			return cmd_retr(client, args);
		if (strcmp(name, "RSET") == 0)
			return cmd_rset(client, args);
		break;
	case 'S':
		if (strcmp(name, "STAT") == 0)
			return cmd_stat(client, args);
		break;
	case 'T':
		if (strcmp(name, "TOP") == 0)
			return cmd_top(client, args);
		break;
	case 'U':
		if (strcmp(name, "UIDL") == 0)
			return cmd_uidl(client, args);
		break;
	}

	client_send_line(client, "-ERR Unknown command: %s", name);
	return -1;
}
