#ifndef __IMAP_SYNC_H
#define __IMAP_SYNC_H

enum imap_sync_flags {
	IMAP_SYNC_FLAG_SEND_UID	= 0x01
};

struct client;

struct imap_sync_context *
imap_sync_init(struct client *client, struct mailbox *box,
	       enum imap_sync_flags imap_flags, enum mailbox_sync_flags flags);
int imap_sync_deinit(struct imap_sync_context *ctx);
int imap_sync_more(struct imap_sync_context *ctx);

int imap_sync_nonselected(struct mailbox *box, enum mailbox_sync_flags flags);

bool cmd_sync(struct client_command_context *cmd, enum mailbox_sync_flags flags,
	      enum imap_sync_flags imap_flags, const char *tagline);

#endif
