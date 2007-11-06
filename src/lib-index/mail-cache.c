/* Copyright (c) 2003-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "buffer.h"
#include "hash.h"
#include "nfs-workarounds.h"
#include "file-cache.h"
#include "mmap-util.h"
#include "write-full.h"
#include "mail-cache-private.h"

#include <unistd.h>

void mail_cache_set_syscall_error(struct mail_cache *cache,
				  const char *function)
{
	i_assert(function != NULL);

	if (ENOSPACE(errno)) {
		cache->index->nodiskspace = TRUE;
		return;
	}

	mail_index_set_error(cache->index,
			     "%s failed with index cache file %s: %m",
			     function, cache->filepath);
}

void mail_cache_set_corrupted(struct mail_cache *cache, const char *fmt, ...)
{
	va_list va;

	(void)unlink(cache->filepath);

	/* mark the cache as unusable */
	cache->hdr = NULL;

	va_start(va, fmt);
	t_push();
	mail_index_set_error(cache->index, "Corrupted index cache file %s: %s",
			     cache->filepath, t_strdup_vprintf(fmt, va));
	t_pop();
	va_end(va);
}

void mail_cache_file_close(struct mail_cache *cache)
{
	if (cache->mmap_base != NULL) {
		if (munmap(cache->mmap_base, cache->mmap_length) < 0)
			mail_cache_set_syscall_error(cache, "munmap()");
	}

	if (cache->file_cache != NULL)
		file_cache_set_fd(cache->file_cache, -1);

	cache->mmap_base = NULL;
	cache->data = NULL;
	cache->hdr = NULL;
	cache->mmap_length = 0;

	if (cache->file_lock != NULL)
		file_lock_free(&cache->file_lock);
	cache->locked = FALSE;

	if (cache->fd != -1) {
		if (close(cache->fd) < 0)
			mail_cache_set_syscall_error(cache, "close()");
		cache->fd = -1;
	}
}

static void mail_cache_init_file_cache(struct mail_cache *cache)
{
	struct stat st;

	if (cache->file_cache == NULL)
		return;

	if (cache->index->nfs_flush) {
		nfs_flush_attr_cache_fd(cache->filepath, cache->fd);
		nfs_flush_read_cache(cache->filepath, cache->fd,
				     F_UNLCK, FALSE);
	}

	file_cache_set_fd(cache->file_cache, cache->fd);

	if (fstat(cache->fd, &st) < 0)
		mail_cache_set_syscall_error(cache, "fstat()");
	else
		file_cache_set_size(cache->file_cache, st.st_size);
}

static bool mail_cache_need_reopen(struct mail_cache *cache)
{
	struct stat st1, st2;

	if (MAIL_CACHE_IS_UNUSABLE(cache)) {
		if (cache->need_compress_file_seq != 0) {
			/* we're waiting for compression */
			return FALSE;
		}
		if (MAIL_INDEX_IS_IN_MEMORY(cache->index)) {
			/* disabled */
			return FALSE;
		}
	}

	if (cache->fd == -1)
		return TRUE;

	/* see if the file has changed */
	if (fstat(cache->fd, &st1) < 0) {
		mail_cache_set_syscall_error(cache, "fstat()");
		return TRUE;
	}
	if (stat(cache->filepath, &st2) < 0) {
		mail_cache_set_syscall_error(cache, "stat()");
		return TRUE;
	}
	return st1.st_ino != st2.st_ino ||
		!CMP_DEV_T(st1.st_dev, st2.st_dev);
}

int mail_cache_reopen(struct mail_cache *cache)
{
	struct mail_index_view *view;
	const struct mail_index_ext *ext;

	i_assert(!cache->locked);

	if (!mail_cache_need_reopen(cache)) {
		/* reopening does no good */
		return 0;
	}

	mail_cache_file_close(cache);

	cache->fd = nfs_safe_open(cache->filepath,
				  cache->index->readonly ? O_RDONLY : O_RDWR);
	if (cache->fd == -1) {
		if (errno == ENOENT)
			cache->need_compress_file_seq = 0;
		else
			mail_cache_set_syscall_error(cache, "open()");
		return -1;
	}

	mail_cache_init_file_cache(cache);

	if (mail_cache_map(cache, 0, 0) < 0)
		return -1;

	if (mail_cache_header_fields_read(cache) < 0)
		return -1;

	view = mail_index_view_open(cache->index);
	ext = mail_index_view_get_ext(view, cache->ext_id);
	if (ext == NULL || cache->hdr->file_seq != ext->reset_id) {
		/* still different - maybe a race condition or maybe the
		   file_seq really is corrupted. either way, this shouldn't
		   happen often so we'll just mark cache to be compressed
		   later which fixes this. */
		cache->need_compress_file_seq = cache->hdr->file_seq;
		mail_index_view_close(&view);
		return 0;
	}

	mail_index_view_close(&view);
	i_assert(!MAIL_CACHE_IS_UNUSABLE(cache));
	return 1;
}

static bool mail_cache_verify_header(struct mail_cache *cache)
{
	const struct mail_cache_header *hdr = cache->data;

	/* check that the header is still ok */
	if (cache->mmap_length < sizeof(struct mail_cache_header)) {
		mail_cache_set_corrupted(cache, "File too small");
		return FALSE;
	}

	if (hdr->version != MAIL_CACHE_VERSION) {
		/* version changed - upgrade silently */
		return FALSE;
	}
	if (hdr->compat_sizeof_uoff_t != sizeof(uoff_t)) {
		/* architecture change - handle silently(?) */
		return FALSE;
	}

	if (hdr->indexid != cache->index->indexid) {
		/* index id changed - handle silently */
		return FALSE;
	}
	if (hdr->file_seq == 0) {
		mail_cache_set_corrupted(cache, "file_seq is 0");
		return FALSE;
	}

	/* only check the header if we're locked */
	if (!cache->locked)
		return TRUE;

	if (hdr->used_file_size < sizeof(struct mail_cache_header)) {
		mail_cache_set_corrupted(cache, "used_file_size too small");
		return FALSE;
	}
	if ((hdr->used_file_size % sizeof(uint32_t)) != 0) {
		mail_cache_set_corrupted(cache, "used_file_size not aligned");
		return FALSE;
	}

	if (cache->mmap_base != NULL &&
	    hdr->used_file_size > cache->mmap_length) {
		mail_cache_set_corrupted(cache, "used_file_size too large");
		return FALSE;
	}
	return TRUE;
}

int mail_cache_map(struct mail_cache *cache, size_t offset, size_t size)
{
	ssize_t ret;

	cache->remap_counter++;

	if (size == 0)
		size = sizeof(struct mail_cache_header);

	if (cache->file_cache != NULL) {
		cache->data = NULL;
		cache->hdr = NULL;

		ret = file_cache_read(cache->file_cache, offset, size);
		if (ret < 0) {
                        /* In case of ESTALE we'll simply fail without error
                           messages. The caller will then just have to
                           fallback to generating the value itself.

                           We can't simply reopen the cache flie, because
                           using it requires also having updated file
                           offsets. */
                        if (errno != ESTALE)
                                mail_cache_set_syscall_error(cache, "read()");
			return -1;
		}

		cache->data = file_cache_get_map(cache->file_cache,
						 &cache->mmap_length);

		if (offset == 0) {
			if (!mail_cache_verify_header(cache)) {
				cache->need_compress_file_seq =
					!MAIL_CACHE_IS_UNUSABLE(cache) &&
					cache->hdr->file_seq != 0 ?
					cache->hdr->file_seq : 0;
				return -1;
			}
			memcpy(&cache->hdr_ro_copy, cache->data,
			       sizeof(cache->hdr_ro_copy));
		}
		cache->hdr = &cache->hdr_ro_copy;
		return 0;
	}

	if (offset < cache->mmap_length &&
	    size <= cache->mmap_length - offset) {
		/* already mapped */
		return 0;
	}

	if (cache->mmap_base != NULL) {
		if (munmap(cache->mmap_base, cache->mmap_length) < 0)
			mail_cache_set_syscall_error(cache, "munmap()");
	} else {
		if (cache->fd == -1) {
			/* unusable, waiting for compression or
			   index is in memory */
			i_assert(cache->need_compress_file_seq != 0 ||
				 MAIL_INDEX_IS_IN_MEMORY(cache->index));
			return -1;
		}
	}

	/* map the whole file */
	cache->hdr = NULL;
	cache->mmap_length = 0;

	cache->mmap_base = mmap_ro_file(cache->fd, &cache->mmap_length);
	if (cache->mmap_base == MAP_FAILED) {
		cache->mmap_base = NULL;
		cache->data = NULL;
		mail_cache_set_syscall_error(cache, "mmap()");
		return -1;
	}
	cache->data = cache->mmap_base;

	if (!mail_cache_verify_header(cache)) {
		cache->need_compress_file_seq =
			!MAIL_CACHE_IS_UNUSABLE(cache) &&
			cache->hdr->file_seq != 0 ?
			cache->hdr->file_seq : 0;
		return -1;
	}

	cache->hdr = cache->data;
	return 0;
}

static int mail_cache_try_open(struct mail_cache *cache)
{
	cache->opened = TRUE;

	if (MAIL_INDEX_IS_IN_MEMORY(cache->index))
		return 0;

	cache->fd = nfs_safe_open(cache->filepath,
				  cache->index->readonly ? O_RDONLY : O_RDWR);
	if (cache->fd == -1) {
		if (errno == ENOENT) {
			cache->need_compress_file_seq = 0;
			return 0;
		}

		mail_cache_set_syscall_error(cache, "open()");
		return -1;
	}

	mail_cache_init_file_cache(cache);

	if (mail_cache_map(cache, 0, sizeof(struct mail_cache_header)) < 0)
		return -1;

	return 1;
}

int mail_cache_open_and_verify(struct mail_cache *cache)
{
	int ret;

	ret = mail_cache_try_open(cache);
	if (ret > 0)
		ret = mail_cache_header_fields_read(cache);
	if (ret < 0) {
		/* failed for some reason - doesn't really matter,
		   it's disabled for now. */
		mail_cache_file_close(cache);
	}
	return ret;
}

static struct mail_cache *mail_cache_alloc(struct mail_index *index)
{
	struct mail_cache *cache;

	cache = i_new(struct mail_cache, 1);
	cache->index = index;
	cache->fd = -1;
	cache->filepath =
		i_strconcat(index->filepath, MAIL_CACHE_FILE_SUFFIX, NULL);
	cache->field_pool = pool_alloconly_create("Cache fields", 1024);
	cache->field_name_hash =
		hash_create(default_pool, cache->field_pool, 0,
			    strcase_hash, (hash_cmp_callback_t *)strcasecmp);

	cache->dotlock_settings.use_excl_lock = index->use_excl_dotlocks;
	cache->dotlock_settings.nfs_flush = index->nfs_flush;
	cache->dotlock_settings.timeout = MAIL_CACHE_LOCK_TIMEOUT;
	cache->dotlock_settings.stale_timeout = MAIL_CACHE_LOCK_CHANGE_TIMEOUT;

	if (!MAIL_INDEX_IS_IN_MEMORY(index)) {
		if (index->mmap_disable)
			cache->file_cache = file_cache_new(-1);
	}

	cache->ext_id =
		mail_index_ext_register(index, "cache", 0,
					sizeof(uint32_t), sizeof(uint32_t));
	mail_index_register_expunge_handler(index, cache->ext_id, FALSE,
					    mail_cache_expunge_handler, cache);
	mail_index_register_sync_handler(index, cache->ext_id,
					 mail_cache_sync_handler,
                                         MAIL_INDEX_SYNC_HANDLER_FILE |
                                         MAIL_INDEX_SYNC_HANDLER_HEAD |
					 (cache->file_cache == NULL ? 0 :
					  MAIL_INDEX_SYNC_HANDLER_VIEW));

	if (cache->file_cache != NULL) {
		mail_index_register_sync_lost_handler(index,
			mail_cache_sync_lost_handler);
	}
	return cache;
}

struct mail_cache *mail_cache_open_or_create(struct mail_index *index)
{
	struct mail_cache *cache;

	cache = mail_cache_alloc(index);
	return cache;
}

struct mail_cache *mail_cache_create(struct mail_index *index)
{
	struct mail_cache *cache;

	cache = mail_cache_alloc(index);
	if (!MAIL_INDEX_IS_IN_MEMORY(index)) {
		if (unlink(cache->filepath) < 0 && errno != ENOENT)
			mail_cache_set_syscall_error(cache, "unlink()");
	}
	return cache;
}

void mail_cache_free(struct mail_cache **_cache)
{
	struct mail_cache *cache = *_cache;

	*_cache = NULL;
	if (cache->file_cache != NULL) {
		mail_index_unregister_sync_lost_handler(cache->index,
			mail_cache_sync_lost_handler);

		file_cache_free(&cache->file_cache);
	}

	mail_index_unregister_expunge_handler(cache->index, cache->ext_id);
	mail_index_unregister_sync_handler(cache->index, cache->ext_id);

	mail_cache_file_close(cache);

	hash_destroy(&cache->field_name_hash);
	pool_unref(&cache->field_pool);
	i_free(cache->field_file_map);
	i_free(cache->file_field_map);
	i_free(cache->fields);
	i_free(cache->filepath);
	i_free(cache);
}

void mail_cache_flush_read_cache(struct mail_cache *cache, bool just_locked)
{
	if (!cache->index->nfs_flush)
		return;

	/* Assume flock() is independent of fcntl() locks. This isn't true
	   with Linux 2.6 NFS, but with it there's no point in using flock() */
	if (cache->locked &&
	    cache->index->lock_method == FILE_LOCK_METHOD_FCNTL) {
		nfs_flush_read_cache(cache->filepath, cache->fd,
				     F_WRLCK, just_locked);
	} else {
		nfs_flush_read_cache(cache->filepath, cache->fd,
				     F_UNLCK, FALSE);
	}
}

static int mail_cache_lock_file(struct mail_cache *cache)
{
	int ret;

	if (cache->index->lock_method != FILE_LOCK_METHOD_DOTLOCK) {
		i_assert(cache->file_lock == NULL);
		ret = mail_index_lock_fd(cache->index, cache->filepath,
					 cache->fd, F_WRLCK,
					 MAIL_CACHE_LOCK_TIMEOUT,
					 &cache->file_lock);
	} else {
		i_assert(cache->dotlock == NULL);
		ret = file_dotlock_create(&cache->dotlock_settings,
					  cache->filepath, 0, &cache->dotlock);
	}

	if (ret <= 0)
		return ret;

	mail_cache_flush_read_cache(cache, TRUE);
	return 1;
}

static void mail_cache_unlock_file(struct mail_cache *cache)
{
	if (cache->index->lock_method != FILE_LOCK_METHOD_DOTLOCK)
		file_unlock(&cache->file_lock);
	else
		(void)file_dotlock_delete(&cache->dotlock);
}

int mail_cache_lock(struct mail_cache *cache, bool require_same_reset_id)
{
	const struct mail_index_ext *ext;
	struct mail_index_view *iview;
	uint32_t reset_id;
	int i, ret;

	i_assert(!cache->locked);

	if (!cache->opened)
		(void)mail_cache_open_and_verify(cache);

	if (MAIL_CACHE_IS_UNUSABLE(cache) ||
	    MAIL_INDEX_IS_IN_MEMORY(cache->index))
		return 0;

	iview = mail_index_view_open(cache->index);
	ext = mail_index_view_get_ext(iview, cache->ext_id);
	reset_id = ext == NULL ? 0 : ext->reset_id;
	mail_index_view_close(&iview);

	if (ext == NULL && require_same_reset_id) {
		/* cache not used */
		return 0;
	}

	if (cache->hdr->file_seq != reset_id) {
		/* we want the latest cache file */
		ret = mail_cache_reopen(cache);
		if (ret < 0 || (ret == 0 && require_same_reset_id))
			return ret;
	}

	for (i = 0; i < 3; i++) {
		ret = mail_cache_lock_file(cache);
		if (ret <= 0)
			break;
		cache->locked = TRUE;

		if (cache->hdr->file_seq == reset_id ||
		    !require_same_reset_id) {
			/* got it */
			break;
		}

		/* okay, so it was just compressed. try again. */
		(void)mail_cache_unlock(cache);
		if ((ret = mail_cache_reopen(cache)) <= 0)
			break;
		ret = 0;
	}

	if (ret > 0) {
		/* make sure our header is up to date */
		if (cache->file_cache != NULL) {
			file_cache_invalidate(cache->file_cache, 0,
					      sizeof(struct mail_cache_header));
		}
		if (mail_cache_map(cache, 0, 0) == 0)
			cache->hdr_copy = *cache->hdr;
		else {
			(void)mail_cache_unlock(cache);
			ret = -1;
		}
	}

	i_assert((ret <= 0 && !cache->locked) || (ret > 0 && cache->locked));
	return ret;
}

static void mail_cache_update_need_compress(struct mail_cache *cache)
{
	const struct mail_cache_header *hdr = cache->hdr;
	unsigned int cont_percentage;
	uoff_t max_del_space;

        cont_percentage = hdr->continued_record_count * 100 /
		(cache->index->map->rec_map->records_count == 0 ? 1 :
		 cache->index->map->rec_map->records_count);
	if (cont_percentage >= MAIL_CACHE_COMPRESS_CONTINUED_PERCENTAGE &&
	    hdr->used_file_size >= MAIL_CACHE_COMPRESS_MIN_SIZE) {
		/* too many continued rows, compress */
		cache->need_compress_file_seq = hdr->file_seq;
	}

	/* see if we've reached the max. deleted space in file */
	max_del_space = hdr->used_file_size / 100 *
		MAIL_CACHE_COMPRESS_PERCENTAGE;
	if (hdr->deleted_space >= max_del_space &&
	    hdr->used_file_size >= MAIL_CACHE_COMPRESS_MIN_SIZE)
		cache->need_compress_file_seq = hdr->file_seq;
}

int mail_cache_unlock(struct mail_cache *cache)
{
	int ret = 0;

	i_assert(cache->locked);

	if (cache->field_header_write_pending)
                ret = mail_cache_header_fields_update(cache);

	cache->locked = FALSE;

	if (MAIL_CACHE_IS_UNUSABLE(cache)) {
		/* we found it to be broken during the lock. just clean up. */
		cache->hdr_modified = FALSE;
		return -1;
	}

	if (cache->hdr_modified) {
		cache->hdr_modified = FALSE;
		if (mail_cache_write(cache, &cache->hdr_copy,
				     sizeof(cache->hdr_copy), 0) < 0)
			ret = -1;
		cache->hdr_ro_copy = cache->hdr_copy;
		mail_cache_update_need_compress(cache);
	}

	if (cache->index->nfs_flush) {
		if (fdatasync(cache->fd) < 0)
			mail_cache_set_syscall_error(cache, "fdatasync()");
	}

	mail_cache_unlock_file(cache);
	return ret;
}

int mail_cache_write(struct mail_cache *cache, const void *data, size_t size,
		     uoff_t offset)
{
	if (pwrite_full(cache->fd, data, size, offset) < 0) {
		mail_cache_set_syscall_error(cache, "pwrite_full()");
		return -1;
	}

	if (cache->file_cache != NULL) {
		file_cache_write(cache->file_cache, data, size, offset);

		/* data pointer may change if file cache was grown */
		cache->data = file_cache_get_map(cache->file_cache,
						 &cache->mmap_length);
	}
	return 0;
}

struct mail_cache_view *
mail_cache_view_open(struct mail_cache *cache, struct mail_index_view *iview)
{
	struct mail_cache_view *view;

	view = i_new(struct mail_cache_view, 1);
	view->cache = cache;
	view->view = iview;
	i_array_init(&view->looping_offsets, 32);
	view->cached_exists_buf =
		buffer_create_dynamic(default_pool,
				      cache->file_fields_count + 10);
	return view;
}

void mail_cache_view_close(struct mail_cache_view *view)
{
	i_assert(view->trans_view == NULL);

	if (view->cache->field_header_write_pending)
                (void)mail_cache_header_fields_update(view->cache);

	array_free(&view->looping_offsets);
	buffer_free(&view->cached_exists_buf);
	i_free(view);
}
