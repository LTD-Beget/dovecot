/* Copyright (C) 2006 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "bsearch-insert-pos.h"
#include "file-lock.h"
#include "istream.h"
#include "ostream.h"
#include "mmap-util.h"
#include "squat-uidlist.h"
#include "squat-trie.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

#define TRIE_COMPRESS_PERCENTAGE 30
#define TRIE_COMPRESS_MIN_SIZE (1024*50)

#define SQUAT_TRIE_VERSION 1
#define SQUAT_TRIE_LOCK_TIMEOUT 60

/* for non-x86 use memcpy() when accessing unaligned int* addresses */
#if defined(__i386__) || defined(__x86_64__)
#  define ALLOW_UNALIGNED_ACCESS
#endif

#define BLOCK_SIZE 4

#define ALIGN(size) \
	(((size) + sizeof(void *)-1) & ~((unsigned int) sizeof(void *)-1))

struct squat_trie {
	char *filepath;
	int fd;
	dev_t dev;
	ino_t ino;

	enum file_lock_method lock_method;
	struct file_lock *file_lock;
	int lock_count;
	int lock_type; /* F_RDLCK / F_WRLCK */

	void *mmap_base;
	size_t mmap_size;

	const struct squat_trie_header *hdr;

	struct squat_uidlist *uidlist;
	struct trie_node *root;
	buffer_t *buf;

	unsigned int corrupted:1;
};

struct squat_trie_build_context {
	struct squat_trie *trie;

	struct ostream *output;

	uint32_t prev_uid;
	unsigned int prev_added_size;
	uint16_t prev_added[BLOCK_SIZE-1];

	unsigned int node_count;
	unsigned int deleted_space;

	unsigned int failed:1;
	unsigned int locked:1;
};

struct squat_trie_compress_context {
	struct squat_trie *trie;

	const char *tmp_path;
	struct ostream *output;

	struct squat_uidlist_compress_ctx *uidlist_ctx;

	unsigned int node_count;
};

struct squat_trie_header {
	uint8_t version;
	uint8_t unused[3];

	uint32_t uidvalidity;
	uint32_t used_file_size;
	uint32_t deleted_space;
	uint32_t node_count;

	uint32_t root_offset;
};

/*
packed_node {
	packed ((8bit_chars_count << 1) | have_16bit_chars);
	uint8_t 8bit_chars[8bit_chars_count];
	uint32_t idx[8bit_chars_count];
	if (have_16bit_chars) {
		packed 16bit_chars_count;
		uint16_t 16bit_chars[16bit_chars_count];
		uint32_t idx[16bit_chars_count];
	}
}
*/

struct trie_node {
	/* new characters have been added to this node */
	uint8_t resized:1;
	/* idx pointers have been updated */
	uint8_t modified:1;
	uint8_t chars_8bit_count;
	uint16_t chars_16bit_count;

	uint32_t file_offset;
	uint32_t orig_size;

	/* the node pointers are valid as long as their lowest bit is 0,
	   otherwise they're offsets to the trie file (>> 1).

	   in leaf nodes the children pointers are uint32_t uid_list_idx[]; */
	/* uint8_t 8bit_chars[chars_8bit_count]; */
	/* struct trie_node *children[chars_8bit_count]; */
	/* uint16_t 16bit_chars[chars_16bit_count]; */
	/* struct trie_node *children[chars_16bit_count]; */
};
#define NODE_CHARS8(node) \
	(uint8_t *)(node + 1)
#define NODE_CHILDREN8(node) \
	(struct trie_node **) \
		((char *)((node) + 1) + \
		 ALIGN(sizeof(uint8_t) * ((node)->chars_8bit_count)))
#define NODE_CHARS16(node) \
	(uint16_t *)((char *)NODE_CHILDREN8(node) + \
		     sizeof(struct trie_node *) * ((node)->chars_8bit_count))
#define NODE_CHILDREN16(node) \
	(struct trie_node **) \
		((char *)NODE_CHARS16(node) + \
		 ALIGN(sizeof(uint16_t) * ((node)->chars_16bit_count)))

static int
squat_trie_compress_node(struct squat_trie_compress_context *ctx,
			 struct trie_node *node, unsigned int level);
static int trie_write_node(struct squat_trie_build_context *ctx,
			   unsigned int level, struct trie_node *node);
static int squat_trie_build_flush(struct squat_trie_build_context *ctx);

static int chr_8bit_cmp(const void *_key, const void *_chr)
{
	const uint8_t *key = _key, *chr = _chr;

	return *key - *chr;
}

static int chr_16bit_cmp(const void *_key, const void *_chr)
{
	const uint16_t *key = _key, *chr = _chr;

	return *key - *chr;
}

void _squat_trie_pack_num(buffer_t *buffer, uint32_t num)
{
	uint8_t c;

	/* number continues as long as the highest bit is set */
	while (num >= 0x80) {
		c = (num & 0x7f) | 0x80;
		num >>= 7;

		buffer_append(buffer, &c, 1);
	}

	c = num;
	buffer_append(buffer, &c, 1);
}

uint32_t _squat_trie_unpack_num(const uint8_t **p, const uint8_t *end)
{
	const uint8_t *c = *p;
	uint32_t value = 0;
	unsigned int bits = 0;

	while (c != end && *c >= 0x80) {
		value |= (*c & 0x7f) << bits;
		bits += 7;
		c++;
	}

	if (c == end) {
		/* last number shouldn't end with high bit */
		return 0;
	}
	if (bits > 32-7) {
		/* we have only 32bit numbers */
		return 0;
	}

	value |= (*c & 0x7f) << bits;
	*p = c + 1;
	return value;
}

static const void *data_normalize(const void *data, size_t size, buffer_t *dest)
{
	const uint8_t *src = data;
	size_t i;

	buffer_set_used_size(dest, 0);
	for (i = 0; i < size; i++) {
		uint16_t chr;

		if (src[i] <= 32)
			chr = 0;
		else if (src[i] > 'z')
			chr = src[i] - 32 - 26;
		else
			chr = i_toupper(src[i]) - 32;
		buffer_append(dest, &chr, sizeof(chr));
	}
	return dest->data;
}

static void
squat_trie_set_syscall_error(struct squat_trie *trie, const char *function)
{
	i_error("%s failed with index search file %s: %m",
		function, trie->filepath);
}

void squat_trie_set_corrupted(struct squat_trie *trie, const char *reason)
{
	i_error("Corrupted index search file %s: %s", trie->filepath, reason);

	(void)unlink(trie->filepath);
	trie->corrupted = TRUE;
}

static void
trie_map_node_save_children(unsigned int level, const uint32_t *src_idx,
			    unsigned int count, struct trie_node **children)
{
	unsigned int i, file_bit;

	file_bit = level == BLOCK_SIZE ? 0 : 1;

#ifndef ALLOW_UNALIGNED_ACCESS
	if ((POINTER_CAST_TO(src_idx, size_t) & (sizeof(uint32_t)-1)) == 0) {
#endif
		for (i = 0; i < count; i++) {
			children[i] = src_idx[i] == 0 ? NULL :
				POINTER_CAST(src_idx[i] | file_bit);
		}
#ifndef ALLOW_UNALIGNED_ACCESS
	} else {
		/* unaligned access */
		uint32_t idx;

		for (i = 0; i < count; i++) {
			memcpy(&idx, &src_idx[i], sizeof(idx));
			children[i] = idx == 0 ? NULL :
				POINTER_CAST(idx | file_bit);
		}
	}
#endif
}

static int
trie_map_node(struct squat_trie *trie, uint32_t offset, unsigned int level,
	      struct trie_node **node_r)
{
	struct trie_node *node;
	const uint8_t *p, *end, *chars8_src, *chars16_src;
	uint32_t num, chars8_count, chars16_count;
	unsigned int chars8_size, chars16_size;

	i_assert(trie->fd != -1);

	if (offset >= trie->mmap_size) {
		squat_trie_set_corrupted(trie, "trie offset too large");
		return -1;
	}

	p = CONST_PTR_OFFSET(trie->mmap_base, offset);
	end = CONST_PTR_OFFSET(trie->mmap_base, trie->mmap_size);

	/* get 8bit char count and check that it's valid */
	num = _squat_trie_unpack_num(&p, end);
	chars8_count = num >> 1;

	if (chars8_count > 256 || p + chars8_count >= end) {
		squat_trie_set_corrupted(trie, "trie offset broken");
		return -1;
	}

	chars8_src = p;
	chars8_size = ALIGN(chars8_count * sizeof(uint8_t)) +
		chars8_count * sizeof(struct trie_node *);
	if ((num & 1) == 0) {
		/* no 16bit chars */
		chars16_count = 0;
		chars16_size = 0;
		chars16_src = NULL;
	} else {
		/* get the 16bit char count and check that it's valid */
		p = CONST_PTR_OFFSET(p, chars8_count *
				     (sizeof(uint8_t) + sizeof(uint32_t)));
		chars16_count = _squat_trie_unpack_num(&p, end);
		if (chars16_count > 65536 ||
		    p + chars16_count*sizeof(uint16_t) >= end) {
			squat_trie_set_corrupted(trie, "trie offset broken");
			return -1;
		}

		chars16_src = p;
		chars16_size = ALIGN(chars16_count * sizeof(uint16_t)) +
			chars16_count * sizeof(struct trie_node *);
	}

	node = i_malloc(sizeof(*node) + chars8_size + chars16_size);
	node->chars_8bit_count = chars8_count;
	node->chars_16bit_count = chars16_count;
	node->file_offset = offset;

	{
		uint8_t *chars8 = NODE_CHARS8(node);
		uint16_t *chars16 = NODE_CHARS16(node);
		struct trie_node **children8 = NODE_CHILDREN8(node);
		struct trie_node **children16 = NODE_CHILDREN16(node);
		const uint32_t *src_idx;
		const void *end_offset;

		memcpy(chars8, chars8_src, sizeof(uint8_t) * chars8_count);
		memcpy(chars16, chars16_src, sizeof(uint16_t) * chars16_count);

		src_idx = CONST_PTR_OFFSET(chars8_src, chars8_count);
		trie_map_node_save_children(level, src_idx, chars8_count,
					    children8);
		if (chars16_count == 0)
			end_offset = &src_idx[chars8_count];
		else {
			src_idx = CONST_PTR_OFFSET(chars16_src,
						   chars16_count *
						   sizeof(uint16_t));
			trie_map_node_save_children(level, src_idx,
						    chars16_count, children16);
			end_offset = &src_idx[chars16_count];
		}

		node->orig_size = ((const char *)end_offset -
				   (const char *)trie->mmap_base) - offset;
	}

	*node_r = node;
	return 0;
}

static void trie_close(struct squat_trie *trie)
{
	if (trie->file_lock != NULL)
		file_lock_free(&trie->file_lock);

	if (trie->mmap_base != NULL) {
		if (munmap(trie->mmap_base, trie->mmap_size) < 0)
			squat_trie_set_syscall_error(trie, "munmap()");
		trie->mmap_base = NULL;
	}
	trie->mmap_size = 0;

	if (trie->fd != -1) {
		if (close(trie->fd) < 0)
			squat_trie_set_syscall_error(trie, "close()");
		trie->fd = -1;
	}

	trie->hdr = NULL;
	trie->corrupted = FALSE;
}

static int trie_map_check_header(struct squat_trie *trie,
				 const struct squat_trie_header *hdr)
{
	if (hdr->version != SQUAT_TRIE_VERSION)
		return -1;

	if (hdr->used_file_size > trie->mmap_size) {
		squat_trie_set_corrupted(trie, "used_file_size too large");
		return -1;
	}
	if (hdr->root_offset > trie->mmap_size ||
	    hdr->root_offset < sizeof(*hdr)) {
		squat_trie_set_corrupted(trie, "invalid root_offset");
		return -1;
	}

	return 0;
}

static int trie_map(struct squat_trie *trie)
{
	struct stat st;

	i_assert(trie->lock_count > 0);

	if (fstat(trie->fd, &st) < 0) {
		squat_trie_set_syscall_error(trie, "fstat()");
		return -1;
	}
	trie->dev = st.st_dev;
	trie->ino = st.st_ino;

	if (trie->mmap_base != NULL) {
		if (munmap(trie->mmap_base, trie->mmap_size) < 0)
			squat_trie_set_syscall_error(trie, "munmap()");
	}
	trie->mmap_size = st.st_size;

	trie->mmap_base = mmap(NULL, trie->mmap_size, PROT_READ | PROT_WRITE,
			       MAP_SHARED, trie->fd, 0);
	if (trie->mmap_base == MAP_FAILED) {
		trie->mmap_size = 0;
		trie->mmap_base = NULL;
		squat_trie_set_syscall_error(trie, "mmap()");
		return -1;
	}

	trie->hdr = trie->mmap_base;
	if (trie_map_check_header(trie, trie->hdr) < 0)
		return -1;

	if (trie_map_node(trie, trie->hdr->root_offset, 1, &trie->root) < 0) {
		trie_close(trie);
		return 0;
	}
	return 1;
}

static int trie_open(struct squat_trie *trie)
{
	trie->fd = open(trie->filepath, O_RDWR);
	if (trie->fd == -1) {
		if (errno == ENOENT)
			return 0;

		squat_trie_set_syscall_error(trie, "open()");
		return -1;
	}

	return trie_map(trie);
}

struct squat_trie *
squat_trie_open(const char *path, enum file_lock_method lock_method)
{
	struct squat_trie *trie;
	const char *uidlist_path;

	trie = i_new(struct squat_trie, 1);
	trie->fd = -1;
	trie->filepath = i_strdup(path);
	trie->lock_method = lock_method;
	trie->buf = buffer_create_dynamic(default_pool, 1024);

	uidlist_path = t_strconcat(path, ".uids", NULL);
	trie->uidlist = squat_uidlist_init(trie, uidlist_path);

	(void)trie_open(trie);
	return trie;
}

void squat_trie_close(struct squat_trie *trie)
{
	buffer_free(trie->buf);
	squat_uidlist_deinit(trie->uidlist);
	i_free(trie->filepath);
	i_free(trie);
}

int squat_trie_get_last_uid(struct squat_trie *trie, uint32_t *uid_r)
{
	return squat_uidlist_get_last_uid(trie->uidlist, uid_r);
}

int squat_trie_lock(struct squat_trie *trie, int lock_type)
{
	int ret;

	i_assert(lock_type == F_RDLCK || lock_type == F_WRLCK);

	if (trie->lock_count > 0) {
		/* read lock -> write lock would deadlock */
		i_assert(trie->lock_type == lock_type || lock_type == F_RDLCK);

		trie->lock_count++;
		return 1;
	}

	i_assert(trie->file_lock == NULL);
	ret = file_wait_lock(trie->fd, trie->filepath, lock_type,
			     trie->lock_method, SQUAT_TRIE_LOCK_TIMEOUT,
			     &trie->file_lock);
	if (ret <= 0)
		return ret;

	trie->lock_count++;
	trie->lock_type = lock_type;
	return 1;
}

void squat_trie_unlock(struct squat_trie *trie)
{
	i_assert(trie->lock_count > 0);

	if (--trie->lock_count > 0)
		return;

	file_unlock(&trie->file_lock);
}

static struct trie_node *
node_alloc(uint16_t chr, unsigned int level)
{
	struct trie_node *node;
	unsigned int idx_size, idx_offset = sizeof(*node);

	idx_size = level < BLOCK_SIZE ?
		sizeof(struct trie_node *) : sizeof(uint32_t);

	if (chr < 256) {
		uint8_t *chrp;

		idx_offset += ALIGN(sizeof(*chrp));
		node = i_malloc(idx_offset + idx_size);
		node->chars_8bit_count = 1;

		chrp = PTR_OFFSET(node, sizeof(*node));
		*chrp = chr;
	} else {
		uint16_t *chrp;

		idx_offset += ALIGN(sizeof(*chrp));
		node = i_malloc(idx_offset + idx_size);
		node->chars_16bit_count = 1;

		chrp = PTR_OFFSET(node, sizeof(*node));
		*chrp = chr;
	}

	node->resized = TRUE;
	return node;
}

static struct trie_node *
node_realloc(struct trie_node *node, uint32_t char_idx, uint16_t chr,
	     unsigned int level)
{
	struct trie_node *new_node;
	unsigned int old_size_8bit, old_size_16bit, old_idx_offset;
	unsigned int idx_size, old_size, new_size, new_idx_offset;
	unsigned int hole1_pos, hole2_pos, skip;

	idx_size = level < BLOCK_SIZE ?
		sizeof(struct trie_node *) : sizeof(uint32_t);

	old_size_8bit = ALIGN(node->chars_8bit_count) +
		node->chars_8bit_count * idx_size;
	old_size_16bit = ALIGN(sizeof(uint16_t) * node->chars_16bit_count) +
		node->chars_16bit_count * idx_size;
	old_size = sizeof(*node) + old_size_8bit + old_size_16bit;

	if (chr < 256) {
		new_idx_offset = sizeof(*node) +
			ALIGN(node->chars_8bit_count + sizeof(uint8_t));
		new_size = new_idx_offset + old_size_16bit +
			(node->chars_8bit_count + 1) * idx_size;
	} else {
		new_idx_offset = sizeof(*node) + old_size_8bit +
			ALIGN((node->chars_16bit_count + 1) * sizeof(uint16_t));
		new_size = new_idx_offset +
			(node->chars_16bit_count + 1) * idx_size;
	}

	new_node = i_malloc(new_size);
	if (chr < 256) {
		hole1_pos = sizeof(*node) + char_idx;
		old_idx_offset = sizeof(*node) + ALIGN(node->chars_8bit_count);
	} else {
		hole1_pos = sizeof(*node) + old_size_8bit +
			char_idx * sizeof(uint16_t);
		old_idx_offset = sizeof(*node) + old_size_8bit +
			ALIGN(node->chars_16bit_count * sizeof(uint16_t));
	}
	hole2_pos = old_idx_offset + idx_size * char_idx;

	memcpy(new_node, node, hole1_pos);
	if (chr < 256) {
		uint8_t *chrp = PTR_OFFSET(new_node, hole1_pos);
		*chrp = chr;
		new_node->chars_8bit_count++;

		memcpy(PTR_OFFSET(new_node, hole1_pos + sizeof(uint8_t)),
		       PTR_OFFSET(node, hole1_pos), old_idx_offset - hole1_pos);
	} else {
		uint16_t *chrp = PTR_OFFSET(new_node, hole1_pos);
		*chrp = chr;
		new_node->chars_16bit_count++;

		memcpy(PTR_OFFSET(new_node, hole1_pos + sizeof(uint16_t)),
		       PTR_OFFSET(node, hole1_pos), old_idx_offset - hole1_pos);
	}

	memcpy(PTR_OFFSET(new_node, new_idx_offset),
	       PTR_OFFSET(node, old_idx_offset),
	       hole2_pos - old_idx_offset);

	skip = new_idx_offset - old_idx_offset;
	memset(PTR_OFFSET(new_node, hole2_pos + skip), 0, idx_size);
	skip += sizeof(uint32_t);
	memcpy(PTR_OFFSET(new_node, hole2_pos + skip),
	       PTR_OFFSET(node, hole2_pos),
	       old_size - hole2_pos);

	new_node->resized = TRUE;
	i_free(node);
	return new_node;
}

static int
trie_insert_node(struct squat_trie_build_context *ctx,
		 struct trie_node **parent,
		 const uint16_t *data, uint32_t uid, unsigned int level)
{
	struct squat_trie *trie = ctx->trie;
	struct trie_node *node = *parent;
	uint32_t char_idx, idx_base_offset;
	bool modified = FALSE;
	int ret;

	if (*data < 256) {
		unsigned int count;

		if (node == NULL) {
			ctx->node_count++;
			node = *parent = node_alloc(*data, level);
			char_idx = 0;
			count = 1;
			modified = TRUE;
		} else {
			uint8_t *chars = PTR_OFFSET(node, sizeof(*node));
			uint8_t *pos;

			count = node->chars_8bit_count;
			pos = bsearch_insert_pos(data, chars, count,
						 sizeof(chars[0]),
						 chr_8bit_cmp);
			char_idx = pos - chars;
			if (char_idx == count || *pos != *data) {
				node = node_realloc(node, char_idx,
						    *data, level);
				*parent = node;
				modified = TRUE;
				count++;
			}
		}
		idx_base_offset = sizeof(*node) + ALIGN(count);
	} else {
		unsigned int offset = sizeof(*node);
		unsigned int count;

		if (node == NULL) {
			ctx->node_count++;
			node = *parent = node_alloc(*data, level);
			char_idx = 0;
			count = 1;
			modified = TRUE;
		} else {
			unsigned int idx_size;
			uint16_t *chars, *pos;

			idx_size = level < BLOCK_SIZE ?
				sizeof(struct trie_node *) : sizeof(uint32_t);
			offset += ALIGN(node->chars_8bit_count) +
				idx_size * node->chars_8bit_count;
			chars = PTR_OFFSET(node, offset);

			count = node->chars_16bit_count;
			pos = bsearch_insert_pos(data, chars, count,
						 sizeof(chars[0]),
						 chr_16bit_cmp);
			char_idx = pos - chars;
			if (char_idx == count || *pos != *data) {
				node = node_realloc(node, char_idx,
						    *data, level);
				*parent = node;
				modified = TRUE;
				count++;
			}
		}

		idx_base_offset = offset + ALIGN(sizeof(uint16_t) * count);
	}

	if (level < BLOCK_SIZE) {
		struct trie_node **children = PTR_OFFSET(node, idx_base_offset);
		size_t child_idx = POINTER_CAST_TO(children[char_idx], size_t);

		if ((child_idx & 1) != 0) {
			if (trie_map_node(trie, child_idx & ~1, level + 1,
					  &children[char_idx]) < 0)
				return -1;
		}
		ret = trie_insert_node(ctx, &children[char_idx],
				       data + 1, uid, level + 1);
		if (ret < 0)
			return -1;
		if (ret > 0)
			node->modified = TRUE;
	} else {
		uint32_t *uid_lists = PTR_OFFSET(node, idx_base_offset);
		if (squat_uidlist_add(trie->uidlist, &uid_lists[char_idx],
				      uid) < 0)
			return -1;

		node->modified = TRUE;
	}
	return modified ? 1 : 0;
}

static uint32_t
trie_lookup_node(struct squat_trie *trie, struct trie_node *node,
		 const uint16_t *data, unsigned int level)
{
	uint32_t char_idx, idx_base_offset;

	if (*data < 256) {
		const uint8_t *chars, *pos;
		unsigned int count;

		if (node == NULL)
			return 0;

		chars = CONST_PTR_OFFSET(node, sizeof(*node));
		count = node->chars_8bit_count;
		pos = bsearch(data, chars, count, sizeof(chars[0]),
			      chr_8bit_cmp);
		if (pos == NULL || *pos != *data)
			return 0;

		char_idx = pos - chars;
		idx_base_offset = sizeof(*node) + ALIGN(count);
	} else {
		const uint16_t *chars, *pos;
		unsigned int count, idx_size, offset;

		if (node == NULL)
			return 0;

		idx_size = level < BLOCK_SIZE ?
			sizeof(struct trie_node *) : sizeof(uint32_t);
		offset = sizeof(*node) + ALIGN(node->chars_8bit_count) +
			idx_size * node->chars_8bit_count;
		chars = PTR_OFFSET(node, offset);

		count = node->chars_16bit_count;
		pos = bsearch(data, chars, count, sizeof(chars[0]),
			      chr_16bit_cmp);
		if (pos == NULL || *pos != *data)
			return 0;

		char_idx = pos - chars;
		idx_base_offset = offset + ALIGN(sizeof(uint16_t) * count);
	}

	if (level < BLOCK_SIZE) {
		struct trie_node **children = PTR_OFFSET(node, idx_base_offset);
		size_t child_idx = POINTER_CAST_TO(children[char_idx], size_t);

		if ((child_idx & 1) != 0) {
			/* not mapped to memory yet. do it. */
			if (trie_map_node(trie, child_idx & ~1, level + 1,
					  &children[char_idx]) < 0)
				return -1;
		}

		return trie_lookup_node(trie, children[char_idx],
					data + 1, level + 1);
	} else {
		const uint32_t *uid_lists =
			CONST_PTR_OFFSET(node, idx_base_offset);

		return uid_lists[char_idx];
	}
}

static bool block_want_add(const uint16_t *data)
{
	unsigned int i;

	/* skip all blocks that contain spaces or control characters.
	   no-one searches them anyway */
	for (i = 0; i < BLOCK_SIZE; i++) {
		if (data[i] == 0)
			return FALSE;
	}
	return TRUE;
}

struct squat_trie_build_context *
squat_trie_build_init(struct squat_trie *trie, uint32_t *last_uid_r)
{
	struct squat_trie_build_context *ctx;

	ctx = i_new(struct squat_trie_build_context, 1);
	ctx->trie = trie;

	if (squat_trie_lock(trie, F_WRLCK) <= 0)
		ctx->failed = TRUE;
	else {
		ctx->locked = TRUE;
		ctx->node_count = trie->hdr->node_count;

		if (squat_uidlist_get_last_uid(trie->uidlist, last_uid_r) < 0)
			ctx->failed = TRUE;
	}

	if (ctx->failed)
		*last_uid_r = 0;
	return ctx;
}

int squat_trie_build_deinit(struct squat_trie_build_context *ctx)
{
	int ret = ctx->failed ? -1 : 0;

	if (ret == 0)
		ret = squat_trie_build_flush(ctx);

	if (ctx->locked)
		squat_trie_unlock(ctx->trie);

	i_free(ctx);
	return ret;
}

int squat_trie_build_more(struct squat_trie_build_context *ctx, uint32_t uid,
			  const void *data, size_t size)
{
	const uint16_t *str;
	uint16_t buf[(BLOCK_SIZE-1)*2];
	unsigned int i, tmp_size;

	if (ctx->failed)
		return -1;

	t_push();
	str = data_normalize(data, size, ctx->trie->buf);

	if (uid == ctx->prev_uid) {
		/* @UNSAFE: continue from last block */
		memcpy(buf, ctx->prev_added,
		       sizeof(buf[0]) * ctx->prev_added_size);
		tmp_size = I_MIN(size, BLOCK_SIZE-1);
		memcpy(buf + ctx->prev_added_size, str,
		       sizeof(buf[0]) * tmp_size);

		tmp_size += ctx->prev_added_size;
		for (i = 0; i + BLOCK_SIZE <= tmp_size; i++) {
			if (block_want_add(buf+i)) {
				if (trie_insert_node(ctx,
						     &ctx->trie->root,
						     buf + i, uid, 1) < 0) {
					t_pop();
					return -1;
				}
			}
		}

		if (size < BLOCK_SIZE) {
			ctx->prev_added_size = I_MIN(tmp_size, BLOCK_SIZE-1);
			memcpy(ctx->prev_added, buf + i,
			       sizeof(buf[0]) * ctx->prev_added_size);
			t_pop();
			return 0;
		}
	}

	for (i = 0; i + BLOCK_SIZE <= size; i++) {
		if (block_want_add(str+i)) {
			if (trie_insert_node(ctx, &ctx->trie->root,
					     str + i, uid, 1) < 0) {
				t_pop();
				return -1;
			}
		}
	}

	ctx->prev_added_size = I_MIN(size, BLOCK_SIZE-1);
	memcpy(ctx->prev_added, str + i,
	       sizeof(ctx->prev_added[0]) * ctx->prev_added_size);
	t_pop();
	return 0;
}

static void node_pack_children(buffer_t *buf, struct trie_node **children,
			       unsigned int count)
{
	unsigned int i;
	size_t child_idx;
	uint32_t idx;

	for (i = 0; i < count; i++) {
		child_idx = POINTER_CAST_TO(children[i], size_t);
		if ((child_idx & 1) != 0)
			idx = child_idx & ~1;
		else
			idx = children[i]->file_offset;
		buffer_append(buf, &idx, sizeof(idx));
	}
}

static void node_pack(buffer_t *buf, struct trie_node *node)
{
	uint8_t *chars8 = NODE_CHARS8(node);
	uint16_t *chars16 = NODE_CHARS16(node);
	struct trie_node **children8 = NODE_CHILDREN8(node);
	struct trie_node **children16 = NODE_CHILDREN16(node);

	buffer_set_used_size(buf, 0);
	_squat_trie_pack_num(buf, (node->chars_8bit_count << 1) |
			     (node->chars_16bit_count > 0 ? 1 : 0));
	buffer_append(buf, chars8, node->chars_8bit_count);
	node_pack_children(buf, children8, node->chars_8bit_count);

	if (node->chars_16bit_count > 0) {
		_squat_trie_pack_num(buf, node->chars_16bit_count);
		buffer_append(buf, chars16,
			      sizeof(*chars16) * node->chars_16bit_count);
		node_pack_children(buf, children16, node->chars_16bit_count);
	}
}

static int node_leaf_finish(struct squat_trie *trie, struct trie_node *node)
{
	uint32_t *idx8 = (uint32_t *)NODE_CHILDREN8(node);
	uint32_t *idx16 = (uint32_t *)NODE_CHILDREN16(node);
	unsigned int i;

	for (i = 0; i < node->chars_8bit_count; i++) {
		if (squat_uidlist_finish_list(trie->uidlist, &idx8[i]) < 0)
			return -1;
	}
	for (i = 0; i < node->chars_16bit_count; i++) {
		if (squat_uidlist_finish_list(trie->uidlist, &idx16[i]) < 0)
			return -1;
	}
	return 0;
}

static void node_pack_leaf(buffer_t *buf, struct trie_node *node)
{
	uint8_t *chars8 = NODE_CHARS8(node);
	uint16_t *chars16 = NODE_CHARS16(node);
	uint32_t *idx8 = (uint32_t *)NODE_CHILDREN8(node);
	uint32_t *idx16 = (uint32_t *)NODE_CHILDREN16(node);

	buffer_set_used_size(buf, 0);
	_squat_trie_pack_num(buf, (node->chars_8bit_count << 1) |
			     (node->chars_16bit_count > 0 ? 1 : 0));
	buffer_append(buf, chars8, node->chars_8bit_count);
	buffer_append(buf, idx8, sizeof(*idx8) * node->chars_8bit_count);

	if (node->chars_16bit_count > 0) {
		_squat_trie_pack_num(buf, node->chars_16bit_count);
		buffer_append(buf, chars16,
			      sizeof(*chars16) * node->chars_16bit_count);
		buffer_append(buf, idx16,
			      sizeof(*idx16) * node->chars_16bit_count);
	}
}

static int
trie_write_node_children(struct squat_trie_build_context *ctx,
			 unsigned int level, struct trie_node **children,
			 unsigned int count)
{
	unsigned int i;
	size_t child_idx;

	for (i = 0; i < count; i++) {
		child_idx = POINTER_CAST_TO(children[i], size_t);
		if ((child_idx & 1) == 0) {
			if (trie_write_node(ctx, level, children[i]) < 0)
				return -1;
		}
	}
	return 0;
}

static int trie_write_node(struct squat_trie_build_context *ctx,
			   unsigned int level, struct trie_node *node)
{
	struct squat_trie *trie = ctx->trie;
	uoff_t offset;

	if (level < BLOCK_SIZE) {
		struct trie_node **children8 = NODE_CHILDREN8(node);
		struct trie_node **children16 = NODE_CHILDREN16(node);

		trie_write_node_children(ctx, level + 1,
					 children8, node->chars_8bit_count);
		trie_write_node_children(ctx, level + 1,
					 children16, node->chars_16bit_count);
		node_pack(trie->buf, node);
	} else {
		if (node_leaf_finish(trie, node) < 0)
			return -1;

		node_pack_leaf(trie->buf, node);
	}

	offset = ctx->output->offset;
	if ((offset & 1) != 0) {
		o_stream_send(ctx->output, "", 1);
		offset++;
	}

	if (node->resized && node->orig_size != trie->buf->used) {
		/* append to end of file. the parent node is written later. */
		node->file_offset = offset;
		o_stream_send(ctx->output, trie->buf->data, trie->buf->used);

		ctx->deleted_space += node->orig_size;
	} else if (node->modified) {
		/* overwrite node's contents */
		i_assert(node->file_offset != 0);
		i_assert(trie->buf->used <= node->orig_size);

		/* FIXME: write only the indexes if !node->resized */
		o_stream_seek(ctx->output, node->file_offset);
		o_stream_send(ctx->output, trie->buf->data, trie->buf->used);
		o_stream_seek(ctx->output, offset);

		ctx->deleted_space += trie->buf->used - node->orig_size;
	}
	return 0;
}

static int
trie_nodes_write(struct squat_trie_build_context *ctx, uint32_t *uidvalidity_r)
{
	struct squat_trie *trie = ctx->trie;
	struct squat_trie_header hdr;

	if (trie->fd == -1) {
		trie->fd = open(trie->filepath,
				O_RDWR | O_CREAT | O_TRUNC, 0600);
		if (trie->fd == -1) {
			squat_trie_set_syscall_error(trie, "open()");
			return -1;
		}

		memset(&hdr, 0, sizeof(hdr));
		hdr.version = SQUAT_TRIE_VERSION;
		hdr.uidvalidity = 0; // FIXME
	} else {
		hdr = *trie->hdr;
		if (lseek(trie->fd, hdr.used_file_size, SEEK_SET) < 0) {
			squat_trie_set_syscall_error(trie, "lseek()");
			return -1;
		}
	}

	ctx->output = o_stream_create_file(trie->fd, default_pool, 0, FALSE);
	if (hdr.used_file_size == 0)
		o_stream_send(ctx->output, &hdr, sizeof(hdr));

	ctx->deleted_space = 0;
	if (trie_write_node(ctx, 1, trie->root) < 0)
		return -1;

	/* update the header */
	hdr.root_offset = trie->root->file_offset;
	hdr.used_file_size = ctx->output->offset;
	hdr.deleted_space += ctx->deleted_space;
	hdr.node_count = ctx->node_count;
	o_stream_seek(ctx->output, 0);
	o_stream_send(ctx->output, &hdr, sizeof(hdr));

	o_stream_destroy(&ctx->output);
	*uidvalidity_r = hdr.uidvalidity;
	return 0;
}

static bool squat_trie_need_compress(struct squat_trie *trie,
				     unsigned int current_message_count)
{
	uint32_t max_del_space;

	if (trie->hdr->used_file_size >= TRIE_COMPRESS_MIN_SIZE) {
		/* see if we've reached the max. deleted space in file */
		max_del_space = trie->hdr->used_file_size / 100 *
			TRIE_COMPRESS_PERCENTAGE;
		if (trie->hdr->deleted_space > max_del_space)
			return TRUE;
	}

	return squat_uidlist_need_compress(trie->uidlist,
					   current_message_count);
}

static int squat_trie_build_flush(struct squat_trie_build_context *ctx)
{
	struct squat_trie *trie = ctx->trie;
	uint32_t uidvalidity;

	if (trie->root == NULL) {
		/* nothing changed */
		return 0;
	}

	if (trie_nodes_write(ctx, &uidvalidity) < 0)
		return -1;
	if (squat_uidlist_flush(trie->uidlist, uidvalidity) < 0)
		return -1;
	if (trie_map(trie) <= 0)
		return -1;

	if (squat_trie_need_compress(trie, (unsigned int)-1)) {
		if (squat_trie_compress(trie, NULL) < 0)
			return -1;
	}
	return 0;
}

static void squat_trie_compress_chars8(struct trie_node *node)
{
	uint8_t *chars = NODE_CHARS8(node);
	struct trie_node **child_src = NODE_CHILDREN8(node);
	struct trie_node **child_dest;
	unsigned int i, j, old_count;

	old_count = node->chars_8bit_count;
	for (i = j = 0; i < old_count; i++) {
		if (child_src[i] != NULL)
			chars[j++] = chars[i];
	}

	node->chars_8bit_count = j;
	child_dest = NODE_CHILDREN8(node);

	for (i = j = 0; i < old_count; i++) {
		if (child_src[i] != NULL)
			child_dest[j++] = child_src[i];
	}
}

static void squat_trie_compress_chars16(struct trie_node *node)
{
	uint16_t *chars = NODE_CHARS16(node);
	struct trie_node **child_src = NODE_CHILDREN16(node);
	struct trie_node **child_dest;
	unsigned int i, j, old_count;

	old_count = node->chars_16bit_count;
	for (i = j = 0; i < old_count; i++) {
		if (child_src[i] != NULL)
			chars[j++] = chars[i];
	}

	node->chars_16bit_count = j;
	child_dest = NODE_CHILDREN16(node);

	for (i = j = 0; i < old_count; i++) {
		if (child_src[i] != NULL)
			child_dest[j++] = child_src[i];
	}
}

static int
squat_trie_compress_children(struct squat_trie_compress_context *ctx,
			     struct trie_node **children, unsigned int count,
			     unsigned int level)
{
	struct trie_node *child_node;
	size_t child_idx;
	unsigned int i;
	int ret = 0;
	bool need_char_compress = FALSE;

	for (i = 0; i < count; i++) {
		child_idx = POINTER_CAST_TO(children[i], size_t);
		i_assert((child_idx & 1) != 0);
		child_idx &= ~1;

		if (trie_map_node(ctx->trie, child_idx, level, &child_node) < 0)
			return -1;

		ret = squat_trie_compress_node(ctx, child_node, level);
		if (child_node->file_offset != 0)
			children[i] = POINTER_CAST(child_node->file_offset | 1);
		else {
			children[i] = NULL;
			need_char_compress = TRUE;
		}
		i_free(child_node);

		if (ret < 0)
			return -1;
	}
	return need_char_compress ? 0 : 1;
}

static int
squat_trie_compress_leaf_uidlist(struct squat_trie_compress_context *ctx,
				 struct trie_node *node)
{
	uint32_t *idx8 = (uint32_t *)NODE_CHILDREN8(node);
	uint32_t *idx16 = (uint32_t *)NODE_CHILDREN16(node);
	unsigned int i;
	int ret;
	bool compress_chars = FALSE;

	for (i = 0; i < node->chars_8bit_count; i++) {
		ret = squat_uidlist_compress_next(ctx->uidlist_ctx, &idx8[i]);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			idx8[i] = 0;
			compress_chars = TRUE;
		}
	}
	if (compress_chars) {
		squat_trie_compress_chars8(node);
		compress_chars = FALSE;
	}
	for (i = 0; i < node->chars_16bit_count; i++) {
		ret = squat_uidlist_compress_next(ctx->uidlist_ctx, &idx16[i]);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			idx16[i] = 0;
			compress_chars = TRUE;
		}
	}
	if (compress_chars) {
		squat_trie_compress_chars16(node);
		node->chars_16bit_count = i;
	}
	return 0;
}

static int
squat_trie_compress_node(struct squat_trie_compress_context *ctx,
			 struct trie_node *node, unsigned int level)
{
	struct squat_trie *trie = ctx->trie;
	int ret;

	if (level == BLOCK_SIZE) {
		if (squat_trie_compress_leaf_uidlist(ctx, node))
			return -1;

		if (node->chars_8bit_count == 0 &&
		    node->chars_16bit_count == 0) {
			/* everything expunged */
			ctx->node_count--;
			node->file_offset = 0;
			return 0;
		}
		node_pack_leaf(trie->buf, node);
	} else {
		struct trie_node **children8 = NODE_CHILDREN8(node);
		struct trie_node **children16 = NODE_CHILDREN16(node);

		if ((ret = squat_trie_compress_children(ctx, children8,
							node->chars_8bit_count,
							level + 1)) < 0)
			return -1;
		if (ret == 0)
			squat_trie_compress_chars8(node);
		if ((ret = squat_trie_compress_children(ctx, children16,
							node->chars_16bit_count,
							level + 1)) < 0)
			return -1;
		if (ret == 0)
			squat_trie_compress_chars16(node);

		if (node->chars_8bit_count == 0 &&
		    node->chars_16bit_count == 0) {
			/* everything expunged */
			ctx->node_count--;
			node->file_offset = 0;
			return 0;
		}
		node_pack(trie->buf, node);
	}

	if ((ctx->output->offset & 1) != 0)
		o_stream_send(ctx->output, "", 1);
	node->file_offset = ctx->output->offset;

	o_stream_send(ctx->output, trie->buf->data, trie->buf->used);
	return 0;
}

static int squat_trie_compress_init(struct squat_trie_compress_context *ctx,
				    struct squat_trie *trie)
{
	struct squat_trie_header hdr;
	int fd;

	ctx->tmp_path = t_strconcat(trie->filepath, ".tmp", NULL);
	fd = open(ctx->tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd == -1) {
		i_error("open(%s, O_CREAT) failed: %m", ctx->tmp_path);
		return -1;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->trie = trie;
	ctx->output = o_stream_create_file(fd, default_pool, 0, TRUE);
	ctx->node_count = trie->hdr->node_count;

	/* write a dummy header first */
	memset(&hdr, 0, sizeof(hdr));
	o_stream_send(ctx->output, &hdr, sizeof(hdr));
	return 0;
}

static void
squat_trie_compress_write_header(struct squat_trie_compress_context *ctx,
				 struct trie_node *root_node)
{
	struct squat_trie_header hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.version = SQUAT_TRIE_VERSION;
	hdr.uidvalidity = ctx->trie->hdr->uidvalidity;
	hdr.root_offset = root_node->file_offset;
	hdr.used_file_size = ctx->output->offset;
	hdr.node_count = ctx->node_count;

	o_stream_seek(ctx->output, 0);
	o_stream_send(ctx->output, &hdr, sizeof(hdr));
}

int squat_trie_compress(struct squat_trie *trie,
			const ARRAY_TYPE(seq_range) *existing_uids)
{
	struct squat_trie_compress_context ctx;
	struct trie_node *node;
	int ret;

	if (squat_trie_lock(trie, F_WRLCK) <= 0)
		return -1;

	if (squat_trie_compress_init(&ctx, trie) < 0) {
		squat_trie_unlock(trie);
		return -1;
	}

	ret = trie_map_node(trie, trie->hdr->root_offset, 1, &node);
	if (ret == 0) {
		/* do the compression */
		ctx.uidlist_ctx = squat_uidlist_compress_begin(trie->uidlist,
							       existing_uids);
		if ((ret = squat_trie_compress_node(&ctx, node, 1)) < 0)
			squat_uidlist_compress_rollback(&ctx.uidlist_ctx);
		else {
			ret = squat_uidlist_compress_commit(&ctx.uidlist_ctx);

			squat_trie_compress_write_header(&ctx, node);
		}
		i_free(node);
	}

	if (ret == 0) {
		if (rename(ctx.tmp_path, trie->filepath) < 0) {
			i_error("rename(%s, %s) failed: %m",
				ctx.tmp_path, trie->filepath);
			ret = -1;
		}
	}
	o_stream_destroy(&ctx.output);
	squat_trie_unlock(trie);

	if (ret < 0)
		(void)unlink(ctx.tmp_path);
	else {
		trie_close(trie);
		if (trie_open(trie) <= 0)
			ret = -1;
	}
	return ret;
}

int squat_trie_mark_having_expunges(struct squat_trie *trie,
				    const ARRAY_TYPE(seq_range) *existing_uids,
				    unsigned int current_message_count)
{
	bool compress;
	int ret;

	compress = squat_trie_need_compress(trie, current_message_count);
	ret = squat_uidlist_mark_having_expunges(trie->uidlist, compress);

	if (compress)
		ret = squat_trie_compress(trie, existing_uids);
	return ret;
}

size_t squat_trie_mem_used(struct squat_trie *trie, unsigned int *count_r)
{
	*count_r = trie->hdr->node_count;

	return trie->mmap_size;
}

static int squat_trie_lookup_init(struct squat_trie *trie, const char *str,
				  const uint16_t **data_r, unsigned int *len_r)
{
	const uint16_t *data;
	unsigned int len = strlen(str);

	if (len < BLOCK_SIZE)
		return -1;

	data = data_normalize(str, len, trie->buf);

	/* skip the blocks that can't exist */
	while (!block_want_add(data + len - BLOCK_SIZE)) {
		if (--len < BLOCK_SIZE)
			return -1;
	}

	if (squat_trie_lock(trie, F_RDLCK) <= 0)
		return -1;

	*data_r = data;
	*len_r = len;
	return 0;
}

static int
squat_trie_lookup_locked(struct squat_trie *trie, ARRAY_TYPE(seq_range) *result,
			 const uint16_t *data, unsigned int len)
{
	uint32_t list;

	list = trie_lookup_node(trie, trie->root, data + len - BLOCK_SIZE, 1);
	if (list == 0)
		return 0;

	if (squat_uidlist_get(trie->uidlist, list, result) < 0) {
		squat_trie_set_corrupted(trie, "uidlist offset broken");
		return -1;
	}
	while (len > BLOCK_SIZE) {
		len--;

		if (!block_want_add(data + len - BLOCK_SIZE))
			continue;

		list = trie_lookup_node(trie, trie->root,
					data + len - BLOCK_SIZE, 1);
		if (list == 0) {
			array_clear(result);
			return 0;
		}
		if (squat_uidlist_filter(trie->uidlist, list, result) < 0) {
			squat_trie_set_corrupted(trie, "uidlist offset broken");
			return -1;
		}
	}
	return array_count(result) > 0 ? 1 : 0;
}

int squat_trie_lookup(struct squat_trie *trie, ARRAY_TYPE(seq_range) *result,
		      const char *str)
{
	const uint16_t *data;
	unsigned int len;
	int ret;

	if (squat_trie_lookup_init(trie, str, &data, &len) < 0)
		return -1;

	ret = squat_trie_lookup_locked(trie, result, data, len);
	squat_trie_unlock(trie);
	return ret;
}

static int
squat_trie_filter_locked(struct squat_trie *trie, ARRAY_TYPE(seq_range) *result,
			 const uint16_t *data, unsigned int len)
{
	uint32_t list;

	for (; len >= BLOCK_SIZE; len--) {
		if (!block_want_add(data + len - BLOCK_SIZE))
			continue;

		list = trie_lookup_node(trie, trie->root,
					data + len - BLOCK_SIZE, 1);
		if (list == 0) {
			array_clear(result);
			return 0;
		}
		if (squat_uidlist_filter(trie->uidlist, list, result) < 0) {
			squat_trie_set_corrupted(trie, "uidlist offset broken");
			return -1;
		}
	}
	return array_count(result) > 0 ? 1 : 0;
}

int squat_trie_filter(struct squat_trie *trie, ARRAY_TYPE(seq_range) *result,
		      const char *str)
{
	const uint16_t *data;
	unsigned int len;
	int ret;

	if (squat_trie_lookup_init(trie, str, &data, &len) < 0)
		return -1;
	ret = squat_trie_filter_locked(trie, result, data, len);
	squat_trie_unlock(trie);
	return ret;
}

struct squat_uidlist *_squat_trie_get_uidlist(struct squat_trie *trie)
{
	return trie->uidlist;
}
