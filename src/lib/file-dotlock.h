#ifndef __FILE_DOTLOCK_H
#define __FILE_DOTLOCK_H

#include <unistd.h>
#include <fcntl.h>

struct dotlock;

struct dotlock_settings {
	/* Dotlock files are created by first creating a temp file and then
	   link()ing it to the dotlock. temp_prefix specifies the prefix to
	   use for temp files. It may contain a full path. Default is
	   ".temp.hostname.pid.". */
	const char *temp_prefix;
	/* Use this suffix for dotlock filenames. Default is ".lock". */
	const char *lock_suffix;

	/* Abort after this many seconds. */
	unsigned int timeout;
	/* If file specified in path doesn't change in stale_timeout seconds
	   and it's still locked, override the lock file. */
	unsigned int stale_timeout;
	/* If file is older than this, override the lock immediately. */
	unsigned int immediate_stale_timeout;

	/* Callback is called once in a while. stale is set to TRUE if stale
	   lock is detected and will be overridden in secs_left. If callback
	   returns FALSE then, the lock will not be overridden. */
	bool (*callback)(unsigned int secs_left, bool stale, void *context);
	void *context;

	/* Rely on O_EXCL locking to work instead of using hardlinks.
	   It's faster, but doesn't work with all NFS implementations. */
	unsigned int use_excl_lock:1;
};

enum dotlock_create_flags {
	/* If lock already exists, fail immediately */
	DOTLOCK_CREATE_FLAG_NONBLOCK		= 0x01,
	/* Don't actually create the lock file, only make sure it doesn't
	   exist. This is racy, so you shouldn't rely on it much. */
	DOTLOCK_CREATE_FLAG_CHECKONLY		= 0x02
};

enum dotlock_replace_flags {
	/* Check that lock file hasn't been overwritten before renaming. */
	DOTLOCK_REPLACE_FLAG_VERIFY_OWNER	= 0x01,
	/* Don't close the file descriptor. */
	DOTLOCK_REPLACE_FLAG_DONT_CLOSE_FD	= 0x02
};

/* Create dotlock. Returns 1 if successful, 0 if timeout or -1 if error.
   When returning 0, errno is also set to EAGAIN. */
int file_dotlock_create(const struct dotlock_settings *set, const char *path,
			enum dotlock_create_flags flags,
			struct dotlock **dotlock_r);

/* Delete the dotlock file. Returns 1 if successful, 0 if the file was already
   been deleted or reused by someone else, -1 if error. */
int file_dotlock_delete(struct dotlock **dotlock);

/* Use dotlock as the new content for file. This provides read safety without
   locks, but it's not very good for large files. Returns fd for lock file.
   If locking timed out, returns -1 and errno = EAGAIN. */
int file_dotlock_open(const struct dotlock_settings *set, const char *path,
		      enum dotlock_create_flags flags,
		      struct dotlock **dotlock_r);
/* Replaces the file dotlock protects with the dotlock file itself. */
int file_dotlock_replace(struct dotlock **dotlock,
			 enum dotlock_replace_flags flags);

/* Returns the lock file path. */
const char *file_dotlock_get_lock_path(struct dotlock *dotlock);

#endif
