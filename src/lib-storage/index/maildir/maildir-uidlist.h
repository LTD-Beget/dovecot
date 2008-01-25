#ifndef MAILDIR_UIDLIST_H
#define MAILDIR_UIDLIST_H

#define MAILDIR_UIDLIST_NAME "dovecot-uidlist"

struct maildir_mailbox;
struct maildir_uidlist;
struct maildir_uidlist_sync_ctx;

enum maildir_uidlist_sync_flags {
	MAILDIR_UIDLIST_SYNC_PARTIAL	= 0x01,
	MAILDIR_UIDLIST_SYNC_KEEP_STATE	= 0x02,
	MAILDIR_UIDLIST_SYNC_FORCE	= 0x04,
	MAILDIR_UIDLIST_SYNC_TRYLOCK	= 0x08
};

enum maildir_uidlist_rec_flag {
	MAILDIR_UIDLIST_REC_FLAG_NEW_DIR	= 0x01,
	MAILDIR_UIDLIST_REC_FLAG_MOVED		= 0x02,
	MAILDIR_UIDLIST_REC_FLAG_RECENT		= 0x04,
	MAILDIR_UIDLIST_REC_FLAG_NONSYNCED	= 0x08,
	MAILDIR_UIDLIST_REC_FLAG_RACING		= 0x10
};

enum maildir_uidlist_hdr_ext_key {
	MAILDIR_UIDLIST_HDR_EXT_UID_VALIDITY		= 'V',
	MAILDIR_UIDLIST_HDR_EXT_NEXT_UID		= 'N',
	/* POP3 UIDL format unless overridden by records */
	MAILDIR_UIDLIST_HDR_EXT_POP3_UIDL_FORMAT	= 'P'
};

enum maildir_uidlist_rec_ext_key {
	/* Physical message size. If filename also contains ,S=<vsize> this
	   isn't written to uidlist. */
	MAILDIR_UIDLIST_REC_EXT_PSIZE		= 'S',
	/* Virtual message size. If filename also contains ,W=<vsize> this
	   isn't written to uidlist. */
	MAILDIR_UIDLIST_REC_EXT_VSIZE		= 'W',
	/* POP3 UIDL overriding the default format */
	MAILDIR_UIDLIST_REC_EXT_POP3_UIDL	= 'P'
};

int maildir_uidlist_lock(struct maildir_uidlist *uidlist);
int maildir_uidlist_try_lock(struct maildir_uidlist *uidlist);
int maildir_uidlist_lock_touch(struct maildir_uidlist *uidlist);
void maildir_uidlist_unlock(struct maildir_uidlist *uidlist);
bool maildir_uidlist_is_locked(struct maildir_uidlist *uidlist);

struct maildir_uidlist *maildir_uidlist_init(struct maildir_mailbox *mbox);
struct maildir_uidlist *
maildir_uidlist_init_readonly(struct index_mailbox *ibox);
void maildir_uidlist_deinit(struct maildir_uidlist **uidlist);

/* Returns -1 if error, 0 if file is broken or lost, 1 if ok. If nfs_flush=TRUE
   and storage has NFS_FLUSH flag set, the NFS attribute cache is flushed to
   make sure that we see the latest uidlist file. */
int maildir_uidlist_refresh(struct maildir_uidlist *uidlist);

/* Returns uidlist record for given filename, or NULL if not found. */
const char *
maildir_uidlist_lookup(struct maildir_uidlist *uidlist, uint32_t uid,
		       enum maildir_uidlist_rec_flag *flags_r);
const char *
maildir_uidlist_lookup_nosync(struct maildir_uidlist *uidlist, uint32_t uid,
			      enum maildir_uidlist_rec_flag *flags_r);
/* Returns extension's value or NULL if it doesn't exist. */
const char *
maildir_uidlist_lookup_ext(struct maildir_uidlist *uidlist, uint32_t uid,
			   enum maildir_uidlist_rec_ext_key key);

uint32_t maildir_uidlist_get_uid_validity(struct maildir_uidlist *uidlist);
uint32_t maildir_uidlist_get_next_uid(struct maildir_uidlist *uidlist);

void maildir_uidlist_set_uid_validity(struct maildir_uidlist *uidlist,
				      uint32_t uid_validity);
void maildir_uidlist_set_next_uid(struct maildir_uidlist *uidlist,
				  uint32_t next_uid, bool force);

void maildir_uidlist_set_ext(struct maildir_uidlist *uidlist, uint32_t uid,
			     enum maildir_uidlist_rec_ext_key key,
			     const char *value);

/* If uidlist has changed, update it. This is mostly meant to be used with
   maildir_uidlist_set_ext() */
int maildir_uidlist_update(struct maildir_uidlist *uidlist);

/* Sync uidlist with what's actually on maildir. Returns same as
   maildir_uidlist_lock(). */
int maildir_uidlist_sync_init(struct maildir_uidlist *uidlist,
			      enum maildir_uidlist_sync_flags sync_flags,
			      struct maildir_uidlist_sync_ctx **sync_ctx_r);
int maildir_uidlist_sync_next(struct maildir_uidlist_sync_ctx *ctx,
			      const char *filename,
			      enum maildir_uidlist_rec_flag flags);
void maildir_uidlist_sync_remove(struct maildir_uidlist_sync_ctx *ctx,
				 const char *filename);
const char *
maildir_uidlist_sync_get_full_filename(struct maildir_uidlist_sync_ctx *ctx,
				       const char *filename);
void maildir_uidlist_sync_finish(struct maildir_uidlist_sync_ctx *ctx);
int maildir_uidlist_sync_deinit(struct maildir_uidlist_sync_ctx **ctx);

bool maildir_uidlist_get_uid(struct maildir_uidlist *uidlist,
			     const char *filename, uint32_t *uid_r);
const char *
maildir_uidlist_get_full_filename(struct maildir_uidlist *uidlist,
				  const char *filename);

void maildir_uidlist_add_flags(struct maildir_uidlist *uidlist,
			       const char *filename,
			       enum maildir_uidlist_rec_flag flags);

/* List all maildir files. */
struct maildir_uidlist_iter_ctx *
maildir_uidlist_iter_init(struct maildir_uidlist *uidlist);
bool maildir_uidlist_iter_next(struct maildir_uidlist_iter_ctx *ctx,
			       uint32_t *uid_r,
			       enum maildir_uidlist_rec_flag *flags_r,
			       const char **filename_r);
void maildir_uidlist_iter_deinit(struct maildir_uidlist_iter_ctx **ctx);

#endif
