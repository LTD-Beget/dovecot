/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "commands.h"
#include "mail-search.h"

int cmd_search(Client *client)
{
	MailSearchArg *sargs;
	ImapArg *args;
	int args_count;
	Pool pool;
	const char *error;

	args_count = imap_parser_read_args(client->parser, 0, 0, &args);
	if (args_count == -2)
		return FALSE;

	if (args_count < 1) {
		client_send_command_error(client,
					  "Missing or invalid arguments.");
		return TRUE;
	}

	if (!client_verify_open_mailbox(client))
		return TRUE;

	pool = pool_create("MailSearchArgs", 2048, FALSE);

	sargs = mail_search_args_build(pool, args, &error);
	if (sargs == NULL) {
		/* error in search arguments */
		client_send_tagline(client, t_strconcat("NO ", error, NULL));
	} else {
		if (client->mailbox->search(client->mailbox, sargs,
					    client->outbuf, client->cmd_uid)) {
			/* NOTE: syncing isn't allowed here */
			client_sync_without_expunges(client);
			client_send_tagline(client, "OK Search completed.");
		} else {
			client_send_storage_error(client);
		}
	}

	pool_unref(pool);
	return TRUE;
}
