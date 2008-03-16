#ifndef IMAP_THREAD_PRIVATE_H
#define IMAP_THREAD_PRIVATE_H

#include "crc32.h"
#include "mail-hash.h"
#include "imap-thread.h"

#define HDR_MESSAGE_ID "message-id"
#define HDR_IN_REPLY_TO "in-reply-to"
#define HDR_REFERENCES "references"
#define HDR_SUBJECT "subject"

struct msgid_search_key {
	const char *msgid;
	uint32_t msgid_crc32;
};

struct mail_thread_node {
	struct mail_hash_record rec;

	/* exists=TRUE: UID of the mail this node belongs to
	   exists=FALSE: UID of some message that references (in References: or
	   In-Reply-To: header) this node. Of all the valid references exactly
	   one has the same CRC32 as this node's msgid_crc32. */
	uint32_t uid;
	uint32_t parent_idx;
	uint32_t msgid_crc32;

	uint32_t link_refcount:29;
	uint32_t expunge_rebuilds:1;
	uint32_t unref_rebuilds:1;
	uint32_t exists:1;
};

struct thread_context {
	struct mail *tmp_mail;

	struct mail_hash *hash;
	struct mail_hash_transaction *hash_trans;

	/* Hash record idx -> Message-ID */
	ARRAY_DEFINE(msgid_cache, const char *);
	pool_t msgid_pool;

	unsigned int cmp_match_count;
	uint32_t cmp_last_idx;

	unsigned int failed:1;
	unsigned int rebuild:1;
};

static inline bool thread_node_is_root(const struct mail_thread_node *node)
{
	if (node == NULL)
		return TRUE;

	/* check also if expunging had changed this node to a root node */
	return !node->exists && node->link_refcount == 0;
}

static inline uint32_t crc32_str_nonzero(const char *str)
{
	uint32_t value = crc32_str(str);
	return value == 0 ? 1 : value;
}

int mail_thread_add(struct thread_context *ctx, struct mail *mail);
int mail_thread_remove(struct thread_context *ctx, uint32_t seq);

int mail_thread_finish(struct mail *tmp_mail,
		       struct mail_hash_transaction *hash_trans,
		       enum mail_thread_type thread_type,
		       struct ostream *output, bool id_is_uid);

#endif
