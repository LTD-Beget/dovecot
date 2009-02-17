#ifndef FTS_API_H
#define FTS_API_H

struct mail;
struct mailbox;
struct fts_backend_build_context;

#include "seq-range-array.h"

enum fts_lookup_flags {
	FTS_LOOKUP_FLAG_HEADER	= 0x01,
	FTS_LOOKUP_FLAG_BODY	= 0x02,
	FTS_LOOKUP_FLAG_INVERT	= 0x04
};

struct fts_backend_uid_map {
	const char *mailbox;
	uint32_t uidvalidity;
	uint32_t uid;
};
ARRAY_DEFINE_TYPE(fts_backend_uid_map, struct fts_backend_uid_map);

struct fts_score_map {
	uint32_t uid;
	float score;
};
ARRAY_DEFINE_TYPE(fts_score_map, struct fts_score_map);

struct fts_backend *
fts_backend_init(const char *backend_name, struct mailbox *box);
void fts_backend_deinit(struct fts_backend **backend);

/* Get the last_uid for the mailbox. */
int fts_backend_get_last_uid(struct fts_backend *backend, uint32_t *last_uid_r);
/* Get last_uids for all mailboxes that might be backend mailboxes for a
   virtual mailbox. Depending on virtual mailbox configuration, this function
   may also return mailboxes that don't really even match the virtual mailbox
   patterns. The caller should filter out the list itself. */
int fts_backend_get_all_last_uids(struct fts_backend *backend, pool_t pool,
				  ARRAY_TYPE(fts_backend_uid_map) *last_uids);

/* Initialize adding new data to the index. last_uid_r is set to the last UID
   that exists in the index. */
int fts_backend_build_init(struct fts_backend *backend, uint32_t *last_uid_r,
			   struct fts_backend_build_context **ctx_r);
/* Add more contents to the index. The data must contain only full valid
   UTF-8 characters, but it doesn't need to be NUL-terminated. size contains
   the data size in bytes, not characters. headers is TRUE if the data contains
   message headers instead of message body. */
int fts_backend_build_more(struct fts_backend_build_context *ctx, uint32_t uid,
			   const unsigned char *data, size_t size,
			   bool headers);
/* Finish adding new data to the index. */
int fts_backend_build_deinit(struct fts_backend_build_context **ctx);

/* Returns TRUE if there exists a build context. */
bool fts_backend_is_building(struct fts_backend *backend);

/* Expunge given mail from the backend. Note that the transaction may still
   fail later. */
void fts_backend_expunge(struct fts_backend *backend, struct mail *mail);
/* Called after transaction has been committed or rollbacked. */
void fts_backend_expunge_finish(struct fts_backend *backend,
				struct mailbox *box, bool committed);

/* Lock/unlock the backend for multiple lookups. Returns 1 if locked, 0 if
   locking timeouted, -1 if error.

   It's not required to call these functions manually, but if you're doing
   multiple lookup/filter operations this avoids multiple lock/unlock calls. */
int fts_backend_lock(struct fts_backend *backend);
void fts_backend_unlock(struct fts_backend *backend);

/* Start building a FTS lookup. */
struct fts_backend_lookup_context *
fts_backend_lookup_init(struct fts_backend *backend);
/* Add a new search key to the lookup. */
void fts_backend_lookup_add(struct fts_backend_lookup_context *ctx,
			    const char *key, enum fts_lookup_flags flags);
/* Finish the lookup and return found UIDs. */
int fts_backend_lookup_deinit(struct fts_backend_lookup_context **ctx,
			      ARRAY_TYPE(seq_range) *definite_uids,
			      ARRAY_TYPE(seq_range) *maybe_uids,
			      ARRAY_TYPE(fts_score_map) *scores);

#endif
