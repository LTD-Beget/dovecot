#ifndef __IMAP_SEARCH_H
#define __IMAP_SEARCH_H

struct imap_arg;
struct mailbox;

/* Builds search arguments based on IMAP arguments. */
struct mail_search_arg *
imap_search_args_build(pool_t pool, struct mailbox *box, struct imap_arg *args,
		       const char **error_r);

int imap_search_get_msgset_arg(const char *messageset,
			       struct mail_search_arg **arg_r,
			       const char **error_r);
int imap_search_get_uidset_arg(struct mailbox *box, const char *uidset,
			       struct mail_search_arg **arg_r,
			       const char **error_r);
struct mail_search_arg *
imap_search_get_arg(struct client *client, const char *set, int uid);

#endif
