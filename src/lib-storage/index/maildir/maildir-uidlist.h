#ifndef __MAILDIR_UIDLIST_H
#define __MAILDIR_UIDLIST_H

#define MAILDIR_UIDLIST_NAME "dovecot-uidlist"

int maildir_uidlist_try_lock(struct maildir_uidlist *uidlist);
void maildir_uidlist_unlock(struct maildir_uidlist *uidlist);

struct maildir_uidlist *maildir_uidlist_init(struct index_mailbox *ibox);
void maildir_uidlist_deinit(struct maildir_uidlist *uidlist);

/* Returns -1 if error, 0 if file is broken or lost, 1 if ok. */
int maildir_uidlist_update(struct maildir_uidlist *uidlist);

/* Returns uidlist record for given filename, or NULL if not found. */
const char *maildir_uidlist_lookup(struct maildir_uidlist *uidlist,
				   uint32_t uid, int *new_dir_r);

/* Sync uidlist with what's actually on maildir. */
struct maildir_uidlist_sync_ctx *
maildir_uidlist_sync_init(struct maildir_uidlist *uidlist);
int maildir_uidlist_sync_next(struct maildir_uidlist_sync_ctx *ctx,
			      const char *filename, int new_dir);
int maildir_uidlist_sync_deinit(struct maildir_uidlist_sync_ctx *ctx);

/* List all maildir files. */
struct maildir_uidlist_iter_ctx *
maildir_uidlist_iter_init(struct maildir_uidlist *uidlist);
int maildir_uidlist_iter_next(struct maildir_uidlist_iter_ctx *ctx,
			      uint32_t *uid_r, uint32_t *flags_r,
			      const char **filename_r);
void maildir_uidlist_iter_deinit(struct maildir_uidlist_iter_ctx *ctx);

#endif
