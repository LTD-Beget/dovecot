#ifndef SQUAT_TRIE_PRIVATE_H
#define SQUAT_TRIE_PRIVATE_H

#include "squat-trie.h"

#define SQUAT_TRIE_VERSION 2
#define SQUAT_TRIE_LOCK_TIMEOUT 60

struct squat_file_header {
	uint8_t version;
	uint8_t unused[3];

	uint32_t indexid;
	uint32_t uidvalidity;
	uint32_t used_file_size;
	uint32_t deleted_space;
	uint32_t node_count;

	uint32_t root_offset;
	uint32_t root_unused_uids;
	uint32_t root_next_uid;
	uint32_t root_uidlist_idx;
};

/*
   node file: FIXME: no up-to-date

   struct squat_file_header;

   // children are written before their parents
   node[] {
     uint8_t child_count;
     unsigned char chars[child_count];
     packed neg_diff_to_first_child_offset; // relative to node
     packed diff_to_prev_offset[child_count-1];
     packed[child_count] {
       // unused_uids_count == uid if have_uid_offset bit is zero
       (unused_uids_count << 1) | (have_uid_offset);
       [diff_to_prev_uid_offset;] // first one is relative to zero
     }
   }
*/

struct squat_node {
	unsigned int child_count:8;

	/* children.leaf_string contains this many bytes */
	unsigned int leaf_string_length:16;

	/* TRUE = children.data contains our children.
	   FALSE = children.offset contains offset to our children in the
	   index file. */
	unsigned int children_not_mapped:1;
	/* When allocating our children, use a sequential array. */
	unsigned int want_sequential:1;
	/* This node's children are in a sequential array, meaning that the
	   first SEQUENTIAL_COUNT children have chars[n] = n. */
	unsigned int have_sequential:1;

	/* Number of UIDs that exists in parent node but not in this one.
	   This is mainly used when adding new UIDs to our children to set
	   the UID to be relative to this node's UID list. */
	uint32_t unused_uids;

	/* next_uid=0 means there are no UIDs in this node, otherwise
	   next_uid-1 is the last UID added to this node. */
	uint32_t next_uid;
	uint32_t uid_list_idx;

	/*
	   struct {
	     unsigned char chars[child_count];
	     struct squat_node[child_count];
	   } *children;
	*/
	union {
		/* children_not_mapped determines if data or offset should
		   be used. */
		void *data;
		unsigned char *leaf_string;
		unsigned char static_leaf_string[sizeof(void *)];
		uint32_t offset;
	} children;
};
/* Return pointer to node.children.chars[] */
#define NODE_CHILDREN_CHARS(node) \
	((unsigned char *)(node)->children.data)
/* Return pointer to node.children.node[] */
#define NODE_CHILDREN_NODES(_node) \
	((struct squat_node *)(NODE_CHILDREN_CHARS(_node) + \
			       MEM_ALIGN((_node)->child_count)))
/* Return number of bytes allocated in node.children.data */
#define NODE_CHILDREN_ALLOC_SIZE(child_count) \
	(MEM_ALIGN(child_count) + \
	 ((child_count) / 8 + 1) * 8 * sizeof(struct squat_node))
/* Return TRUE if children.leaf_string is set. */
#define NODE_IS_DYNAMIC_LEAF(node) \
	((node)->leaf_string_length > \
		sizeof((node)->children.static_leaf_string))
/* Return node's leaf string. Assumes that it is set. */
#define NODE_LEAF_STRING(node) \
	(NODE_IS_DYNAMIC_LEAF(node) ? \
	 (node)->children.leaf_string : (node)->children.static_leaf_string)
struct squat_trie {
	struct squat_node root;
	struct squat_uidlist *uidlist;

	struct squat_file_header hdr;
	size_t node_alloc_size;
	unsigned int unmapped_child_count;

	enum file_lock_method lock_method;
	uint32_t uidvalidity;

	char *path;
	int fd;
	uoff_t locked_file_size;

	void *mmap_base;
	size_t mmap_size;

	unsigned char normalize_map[256];

	unsigned int corrupted:1;
};

#define SQUAT_PACK_MAX_SIZE ((sizeof(uint32_t) * 8 + 7) / 7)

static inline void squat_pack_num(uint8_t **p, uint32_t num)
{
	/* number continues as long as the highest bit is set */
	while (num >= 0x80) {
		**p = (num & 0x7f) | 0x80;
		*p += 1;
		num >>= 7;
	}

	**p = num;
	*p += 1;
}

static inline uint32_t squat_unpack_num(const uint8_t **p, const uint8_t *end)
{
	const uint8_t *c = *p;
	uint32_t value = 0;
	unsigned int bits = 0;

	for (;;) {
		if (unlikely(c == end)) {
			/* we should never see EOF */
			return 0;
		}

		value |= (*c & 0x7f) << bits;
		if (*c < 0x80)
			break;

		bits += 7;
		c++;
	}

	if (unlikely(bits > 32-7)) {
		/* broken input */
		*p = end;
		return 0;
	}

	*p = c + 1;
	return value;
}

void squat_trie_delete(struct squat_trie *trie);

#endif
