/* Copyright (c) 2003-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "buffer.h"
#include "hash.h"
#include "str-sanitize.h"
#include "mmap-util.h"
#include "nfs-workarounds.h"
#include "read-full.h"
#include "write-full.h"
#include "mail-index-private.h"
#include "mail-index-view-private.h"
#include "mail-index-sync-private.h"
#include "mail-index-modseq.h"
#include "mail-transaction-log.h"
#include "mail-cache.h"

#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include <sys/stat.h>

struct mail_index_module_register mail_index_module_register = { 0 };

struct mail_index *mail_index_alloc(const char *dir, const char *prefix)
{
	struct mail_index *index;

	index = i_new(struct mail_index, 1);
	index->dir = i_strdup(dir);
	index->prefix = i_strdup(prefix);
	index->fd = -1;

	index->extension_pool =
		pool_alloconly_create(MEMPOOL_GROWING"index extension", 1024);
	p_array_init(&index->extensions, index->extension_pool, 5);
	i_array_init(&index->sync_lost_handlers, 4);
	i_array_init(&index->module_contexts,
		     I_MIN(5, mail_index_module_register.id));

	index->mode = 0600;
	index->gid = (gid_t)-1;

	index->keywords_ext_id =
		mail_index_ext_register(index, MAIL_INDEX_EXT_KEYWORDS,
					128, 2, 1);
	index->keywords_pool = pool_alloconly_create("keywords", 512);
	i_array_init(&index->keywords, 16);
	index->keywords_hash =
		hash_table_create(default_pool, index->keywords_pool, 0,
				  strcase_hash, (hash_cmp_callback_t *)strcasecmp);
	index->log = mail_transaction_log_alloc(index);
	mail_index_modseq_init(index);
	return index;
}

void mail_index_free(struct mail_index **_index)
{
	struct mail_index *index = *_index;

	*_index = NULL;
	mail_index_close(index);

	mail_transaction_log_free(&index->log);
	hash_table_destroy(&index->keywords_hash);
	pool_unref(&index->extension_pool);
	pool_unref(&index->keywords_pool);

	array_free(&index->sync_lost_handlers);
	array_free(&index->keywords);
	array_free(&index->module_contexts);

	i_free(index->error);
	i_free(index->dir);
	i_free(index->prefix);
	i_free(index);
}

void mail_index_set_fsync_types(struct mail_index *index,
				enum mail_index_sync_type fsync_mask)
{
	index->fsync_mask = fsync_mask;
}

void mail_index_set_permissions(struct mail_index *index,
				mode_t mode, gid_t gid)
{
	index->mode = mode & 0666;
	index->gid = gid;
}

uint32_t mail_index_ext_register(struct mail_index *index, const char *name,
				 uint32_t default_hdr_size,
				 uint16_t default_record_size,
				 uint16_t default_record_align)
{
	struct mail_index_registered_ext rext;
	uint32_t ext_id;

	if (*name == '\0' || strcmp(name, str_sanitize(name, -1)) != 0)
		i_panic("mail_index_ext_register(%s): Invalid name", name);

	if (default_record_size != 0 && default_record_align == 0) {
		i_panic("mail_index_ext_register(%s): "
			"Invalid record alignment", name);
	}

	if (mail_index_ext_lookup(index, name, &ext_id))
		return ext_id;

	memset(&rext, 0, sizeof(rext));
	rext.name = p_strdup(index->extension_pool, name);
	rext.index_idx = array_count(&index->extensions);
	rext.hdr_size = default_hdr_size;
	rext.record_size = default_record_size;
	rext.record_align = default_record_align;

	array_append(&index->extensions, &rext, 1);
	return rext.index_idx;
}

bool mail_index_ext_lookup(struct mail_index *index, const char *name,
			   uint32_t *ext_id_r)
{
        const struct mail_index_registered_ext *extensions;
	unsigned int i, count;

	extensions = array_get(&index->extensions, &count);
	for (i = 0; i < count; i++) {
		if (strcmp(extensions[i].name, name) == 0) {
			*ext_id_r = i;
			return TRUE;
		}
	}

	*ext_id_r = (uint32_t)-1;
	return FALSE;
}

void mail_index_register_expunge_handler(struct mail_index *index,
					 uint32_t ext_id, bool call_always,
					 mail_index_expunge_handler_t *cb,
					 void *context)
{
	struct mail_index_registered_ext *rext;

	rext = array_idx_modifiable(&index->extensions, ext_id);
	i_assert(rext->expunge_handler == NULL || rext->expunge_handler == cb);

	rext->expunge_handler = cb;
	rext->expunge_context = context;
	rext->expunge_handler_call_always = call_always;
}

void mail_index_unregister_expunge_handler(struct mail_index *index,
					   uint32_t ext_id)
{
	struct mail_index_registered_ext *rext;

	rext = array_idx_modifiable(&index->extensions, ext_id);
	i_assert(rext->expunge_handler != NULL);

	rext->expunge_handler = NULL;
}

void mail_index_register_sync_handler(struct mail_index *index, uint32_t ext_id,
				      mail_index_sync_handler_t *cb,
				      enum mail_index_sync_handler_type type)
{
	struct mail_index_registered_ext *rext;

	rext = array_idx_modifiable(&index->extensions, ext_id);
	i_assert(rext->sync_handler.callback == NULL);

	rext->sync_handler.callback = cb;
	rext->sync_handler.type = type;
}

void mail_index_unregister_sync_handler(struct mail_index *index,
					uint32_t ext_id)
{
	struct mail_index_registered_ext *rext;

	rext = array_idx_modifiable(&index->extensions, ext_id);
	i_assert(rext->sync_handler.callback != NULL);

	rext->sync_handler.callback = NULL;
	rext->sync_handler.type = 0;
}

void mail_index_register_sync_lost_handler(struct mail_index *index,
					   mail_index_sync_lost_handler_t *cb)
{
	array_append(&index->sync_lost_handlers, &cb, 1);
}

void mail_index_unregister_sync_lost_handler(struct mail_index *index,
					     mail_index_sync_lost_handler_t *cb)
{
	mail_index_sync_lost_handler_t *const *handlers;
	unsigned int i, count;

	handlers = array_get(&index->sync_lost_handlers, &count);
	for (i = 0; i < count; i++) {
		if (handlers[i] == cb) {
			array_delete(&index->sync_lost_handlers, i, 1);
			break;
		}
	}
}

bool mail_index_keyword_lookup(struct mail_index *index,
			       const char *keyword, unsigned int *idx_r)
{
	void *value;

	/* keywords_hash keeps a name => index mapping of keywords.
	   Keywords are never removed from it, so the index values are valid
	   for the lifetime of the mail_index. */
	if (hash_table_lookup_full(index->keywords_hash, keyword,
				   NULL, &value)) {
		*idx_r = POINTER_CAST_TO(value, unsigned int);
		return TRUE;
	}

	*idx_r = (unsigned int)-1;
	return FALSE;
}

void mail_index_keyword_lookup_or_create(struct mail_index *index,
					 const char *keyword,
					 unsigned int *idx_r)
{
	char *keyword_dup;

	i_assert(*keyword != '\0');

	if (mail_index_keyword_lookup(index, keyword, idx_r))
		return;

	keyword = keyword_dup = p_strdup(index->keywords_pool, keyword);
	*idx_r = array_count(&index->keywords);

	hash_table_insert(index->keywords_hash,
			  keyword_dup, POINTER_CAST(*idx_r));
	array_append(&index->keywords, &keyword, 1);
}

const ARRAY_TYPE(keywords) *mail_index_get_keywords(struct mail_index *index)
{
	return &index->keywords;
}

int mail_index_try_open_only(struct mail_index *index)
{
	i_assert(index->fd == -1);
	i_assert(!MAIL_INDEX_IS_IN_MEMORY(index));

        /* Note that our caller must close index->fd by itself. */
	if (index->readonly)
		errno = EACCES;
	else {
		index->fd = nfs_safe_open(index->filepath, O_RDWR);
		index->readonly = FALSE;
	}

	if (index->fd == -1 && errno == EACCES) {
		index->fd = open(index->filepath, O_RDONLY);
		index->readonly = TRUE;
	}

	if (index->fd == -1) {
		if (errno != ENOENT)
			return mail_index_set_syscall_error(index, "open()");

		/* have to create it */
		return 0;
	}
	return 1;
}

static int
mail_index_try_open(struct mail_index *index)
{
	int ret;

        i_assert(index->fd == -1);

	if (MAIL_INDEX_IS_IN_MEMORY(index))
		return 0;

	i_assert(index->map == NULL || index->map->rec_map->lock_id == 0);
	ret = mail_index_map(index, MAIL_INDEX_SYNC_HANDLER_HEAD);
	if (ret == 0) {
		/* it's corrupted - recreate it */
		if (index->fd != -1) {
			if (close(index->fd) < 0)
				mail_index_set_syscall_error(index, "close()");
			index->fd = -1;
		}
	}
	return ret;
}

int mail_index_create_tmp_file(struct mail_index *index, const char **path_r)
{
        mode_t old_mask;
	const char *path;
	int fd;

	i_assert(!MAIL_INDEX_IS_IN_MEMORY(index));

	path = *path_r = t_strconcat(index->filepath, ".tmp", NULL);
	old_mask = umask(0);
	fd = open(path, O_RDWR|O_CREAT|O_TRUNC, index->mode);
	umask(old_mask);
	if (fd == -1)
		return mail_index_file_set_syscall_error(index, path, "open()");

	mail_index_fchown(index, fd, path);
	return fd;
}

static int mail_index_open_files(struct mail_index *index,
				 enum mail_index_open_flags flags)
{
	int ret;
	bool created = FALSE;

	ret = mail_transaction_log_open(index->log);
	if (ret == 0) {
		if ((flags & MAIL_INDEX_OPEN_FLAG_CREATE) == 0)
			return 0;

		/* if dovecot.index exists, read it first so that we can get
		   the correct indexid and log sequence */
		(void)mail_index_try_open(index);

		if (index->indexid == 0) {
			/* Create a new indexid for us. If we're opening index
			   into memory, index->map doesn't exist yet. */
			index->indexid = ioloop_time;
			index->initial_create = TRUE;
			if (index->map != NULL)
				index->map->hdr.indexid = index->indexid;
		}

		ret = mail_transaction_log_create(index->log, FALSE);
		index->initial_create = FALSE;
		created = TRUE;
	}
	if (ret >= 0) {
		ret = index->map != NULL ? 1 : mail_index_try_open(index);
		if (ret == 0) {
			/* corrupted */
			mail_transaction_log_close(index->log);
			ret = mail_transaction_log_create(index->log, TRUE);
			if (ret == 0) {
				if (index->map != NULL)
					mail_index_unmap(&index->map);
				index->map = mail_index_map_alloc(index);
			}
		}
	}
	if (ret < 0) {
		/* open/create failed, fallback to in-memory indexes */
		if ((flags & MAIL_INDEX_OPEN_FLAG_CREATE) == 0)
			return -1;

		if (mail_index_move_to_memory(index) < 0)
			return -1;
	}

	index->cache = created ? mail_cache_create(index) :
		mail_cache_open_or_create(index);
	return 1;
}

int mail_index_open(struct mail_index *index, enum mail_index_open_flags flags,
		    enum file_lock_method lock_method)
{
	int ret;

	if (index->opened) {
		if (index->map != NULL &&
		    (index->map->hdr.flags &
		     MAIL_INDEX_HDR_FLAG_CORRUPTED) != 0) {
			/* corrupted, reopen files */
                        mail_index_close(index);
		} else {
			i_assert(index->map != NULL);
			return 1;
		}
	}

	index->filepath = MAIL_INDEX_IS_IN_MEMORY(index) ?
		i_strdup("(in-memory index)") :
		i_strconcat(index->dir, "/", index->prefix, NULL);

	index->shared_lock_count = 0;
	index->excl_lock_count = 0;
	index->lock_type = F_UNLCK;
	index->lock_id_counter = 2;

	index->readonly = FALSE;
	index->nodiskspace = FALSE;
	index->index_lock_timeout = FALSE;
	index->log_locked = FALSE;
	index->mmap_disable = (flags & MAIL_INDEX_OPEN_FLAG_MMAP_DISABLE) != 0;
	index->use_excl_dotlocks =
		(flags & MAIL_INDEX_OPEN_FLAG_DOTLOCK_USE_EXCL) != 0;
	index->fsync_disable =
		(flags & MAIL_INDEX_OPEN_FLAG_FSYNC_DISABLE) != 0;
	index->nfs_flush = (flags & MAIL_INDEX_OPEN_FLAG_NFS_FLUSH) != 0;
	index->readonly = (flags & MAIL_INDEX_OPEN_FLAG_READONLY) != 0;
	index->keep_backups = (flags & MAIL_INDEX_OPEN_FLAG_KEEP_BACKUPS) != 0;
	index->never_in_memory =
		(flags & MAIL_INDEX_OPEN_FLAG_NEVER_IN_MEMORY) != 0;
	index->lock_method = lock_method;

	if (index->nfs_flush && index->fsync_disable)
		i_fatal("nfs flush requires fsync_disable=no");
	if (index->nfs_flush && !index->mmap_disable)
		i_fatal("nfs flush requires mmap_disable=yes");

	if ((ret = mail_index_open_files(index, flags)) <= 0) {
		/* doesn't exist and create flag not used */
		mail_index_close(index);
		return ret;
	}

	i_assert(index->map != NULL);
	index->opened = TRUE;
	return 1;
}

int mail_index_open_or_create(struct mail_index *index,
			      enum mail_index_open_flags flags,
			      enum file_lock_method lock_method)
{
	int ret;

	flags |= MAIL_INDEX_OPEN_FLAG_CREATE;
	ret = mail_index_open(index, flags, lock_method);
	i_assert(ret != 0);
	return ret < 0 ? -1 : 0;
}

void mail_index_close_file(struct mail_index *index)
{
	if (index->file_lock != NULL)
		file_lock_free(&index->file_lock);

	if (index->fd != -1) {
		if (close(index->fd) < 0)
			mail_index_set_syscall_error(index, "close()");
		index->fd = -1;
	}

	index->lock_id_counter += 2;
	index->lock_type = F_UNLCK;
	index->shared_lock_count = 0;
	index->excl_lock_count = 0;
}

void mail_index_close(struct mail_index *index)
{
	if (index->map != NULL)
		mail_index_unmap(&index->map);

	mail_index_close_file(index);
	mail_transaction_log_close(index->log);
	if (index->cache != NULL)
		mail_cache_free(&index->cache);

	i_free_and_null(index->filepath);

	index->indexid = 0;
	index->opened = FALSE;
}

int mail_index_unlink(struct mail_index *index)
{
	const char *path;
	int last_errno = 0;

	if (MAIL_INDEX_IS_IN_MEMORY(index))
		return 0;

	/* main index */
	if (unlink(index->filepath) < 0 && errno != ENOENT)
		last_errno = errno;

	/* logs */
	path = t_strconcat(index->filepath, MAIL_TRANSACTION_LOG_SUFFIX, NULL);
	if (unlink(path) < 0 && errno != ENOENT)
		last_errno = errno;

	path = t_strconcat(index->filepath,
			   MAIL_TRANSACTION_LOG_SUFFIX".2", NULL);
	if (unlink(path) < 0 && errno != ENOENT)
		last_errno = errno;

	/* cache */
	path = t_strconcat(index->filepath, MAIL_CACHE_FILE_SUFFIX, NULL);
	if (unlink(path) < 0 && errno != ENOENT)
		last_errno = errno;

	if (last_errno == 0)
		return 0;
	else {
		errno = last_errno;
		return -1;
	}
}

int mail_index_reopen_if_changed(struct mail_index *index)
{
	struct stat st1, st2;

	i_assert(index->shared_lock_count == 0 || !index->nfs_flush);
	i_assert(index->excl_lock_count == 0);

	if (MAIL_INDEX_IS_IN_MEMORY(index))
		return 0;

	if (index->fd == -1)
		return mail_index_try_open_only(index);

	if (index->nfs_flush)
		nfs_flush_file_handle_cache(index->filepath);
	if (nfs_safe_stat(index->filepath, &st2) < 0) {
		if (errno == ENOENT)
			return 0;
		return mail_index_set_syscall_error(index, "stat()");
	}

	if (fstat(index->fd, &st1) < 0) {
		if (errno != ESTALE)
			return mail_index_set_syscall_error(index, "fstat()");
		/* deleted/recreated, reopen */
	} else if (st1.st_ino == st2.st_ino &&
		   CMP_DEV_T(st1.st_dev, st2.st_dev)) {
		/* the same file */
		return 1;
	}

	/* new file, new locks. the old fd can keep its locks, they don't
	   matter anymore as no-one's going to modify the file. */
	mail_index_close_file(index);

	return mail_index_try_open_only(index);
}

int mail_index_refresh(struct mail_index *index)
{
	int ret;

	ret = mail_index_map(index, MAIL_INDEX_SYNC_HANDLER_HEAD);
	return ret <= 0 ? -1 : 0;
}

struct mail_cache *mail_index_get_cache(struct mail_index *index)
{
	return index->cache;
}

int mail_index_set_error(struct mail_index *index, const char *fmt, ...)
{
	va_list va;

	i_free(index->error);

	if (fmt == NULL)
		index->error = NULL;
	else {
		va_start(va, fmt);
		index->error = i_strdup_vprintf(fmt, va);
		va_end(va);

		i_error("%s", index->error);
	}

	return -1;
}

bool mail_index_is_in_memory(struct mail_index *index)
{
	return MAIL_INDEX_IS_IN_MEMORY(index);
}

int mail_index_move_to_memory(struct mail_index *index)
{
	struct mail_index_map *map;

	if (MAIL_INDEX_IS_IN_MEMORY(index))
		return index->map == NULL ? -1 : 0;

	if (index->never_in_memory)
		return -1;

	/* set the index as being into memory */
	i_free_and_null(index->dir);

	i_free(index->filepath);
	index->filepath = i_strdup("(in-memory index)");

	if (index->map == NULL) {
		/* index was never even opened. just mark it as being in
		   memory and let the caller re-open the index. */
		i_assert(index->fd == -1);
		return -1;
	}

	/* move index map to memory */
	if (!MAIL_INDEX_MAP_IS_IN_MEMORY(index->map)) {
		map = mail_index_map_clone(index->map);
		mail_index_unmap(&index->map);
		index->map = map;
	}

	if (index->log != NULL) {
		/* move transaction log to memory */
		mail_transaction_log_move_to_memory(index->log);
	}

	if (index->file_lock != NULL)
		file_lock_free(&index->file_lock);

	if (index->fd != -1) {
		if (close(index->fd) < 0)
			mail_index_set_syscall_error(index, "close()");
		index->fd = -1;
	}
	return 0;
}

void mail_index_mark_corrupted(struct mail_index *index)
{
	index->indexid = 0;

	index->map->hdr.flags |= MAIL_INDEX_HDR_FLAG_CORRUPTED;
	if (unlink(index->filepath) < 0 && errno != ENOENT && errno != ESTALE)
		mail_index_set_syscall_error(index, "unlink()");
}

void mail_index_fchown(struct mail_index *index, int fd, const char *path)
{
	mode_t mode;

	if (index->gid == (gid_t)-1) {
		/* no gid changing */
		return;
	} else if (fchown(fd, (uid_t)-1, index->gid) == 0) {
		/* success */
		return;
	} if ((index->mode & 0066) == 0) {
		/* group doesn't really matter, ignore silently. */
		return;
	} if ((index->mode & 0060) == 0) {
		/* file access was granted to everyone, except this group.
		   to make sure we don't expose it to the group, drop the world
		   permissions too. */
		mail_index_file_set_syscall_error(index, path, "fchown()");
		mode = index->mode & 0600;
	} else {
		mail_index_file_set_syscall_error(index, path, "fchown()");
		/* continue, but change group permissions to same as
		   world-permissions were. */
		mode = (index->mode & 0606) | ((index->mode & 06) << 3);
	}
	if (fchmod(fd, mode) < 0) {
		mail_index_file_set_syscall_error(index, path,
						  "fchmod()");
	}
}

int mail_index_set_syscall_error(struct mail_index *index,
				 const char *function)
{
	i_assert(function != NULL);

	if (ENOSPACE(errno)) {
		index->nodiskspace = TRUE;
		if (!index->never_in_memory)
			return -1;
	}

	return mail_index_set_error(index, "%s failed with index file %s: %m",
				    function, index->filepath);
}

int mail_index_file_set_syscall_error(struct mail_index *index,
				      const char *filepath,
				      const char *function)
{
	i_assert(filepath != NULL);
	i_assert(function != NULL);

	if (ENOSPACE(errno)) {
		index->nodiskspace = TRUE;
		if (!index->never_in_memory)
			return -1;
	}

	return mail_index_set_error(index, "%s failed with file %s: %m",
				    function, filepath);
}

const char *mail_index_get_error_message(struct mail_index *index)
{
	return index->error;
}

void mail_index_reset_error(struct mail_index *index)
{
	if (index->error != NULL) {
		i_free(index->error);
		index->error = NULL;
	}

	index->nodiskspace = FALSE;
        index->index_lock_timeout = FALSE;
}
