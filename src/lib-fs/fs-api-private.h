#ifndef FS_API_PRIVATE_H
#define FS_API_PRIVATE_H

#include "fs-api.h"

struct fs_vfuncs {
	struct fs *(*alloc)(void);
	int (*init)(struct fs *fs, const char *args,
		    const struct fs_settings *set);
	void (*deinit)(struct fs *fs);

	enum fs_properties (*get_properties)(struct fs *fs);

	struct fs_file *(*file_init)(struct fs *fs, const char *path,
				     enum fs_open_mode mode,
				     enum fs_open_flags flags);
	void (*file_deinit)(struct fs_file *file);

	void (*set_async_callback)(struct fs_file *file,
				   fs_file_async_callback_t *callback,
				   void *context);
	int (*wait_async)(struct fs *fs);

	void (*set_metadata)(struct fs_file *file, const char *key,
			     const char *value);
	int (*get_metadata)(struct fs_file *file,
			    const ARRAY_TYPE(fs_metadata) **metadata_r);

	bool (*prefetch)(struct fs_file *file, uoff_t length);
	ssize_t (*read)(struct fs_file *file, void *buf, size_t size);
	struct istream *(*read_stream)(struct fs_file *file,
				       size_t max_buffer_size);

	int (*write)(struct fs_file *file, const void *data, size_t size);
	void (*write_stream)(struct fs_file *file);
	int (*write_stream_finish)(struct fs_file *file, bool success);

	int (*lock)(struct fs_file *file, unsigned int secs,
		    struct fs_lock **lock_r);
	void (*unlock)(struct fs_lock *lock);

	int (*exists)(struct fs_file *file);
	int (*stat)(struct fs_file *file, struct stat *st_r);
	int (*copy)(struct fs_file *src, struct fs_file *dest);
	int (*rename)(struct fs_file *src, struct fs_file *dest);
	int (*delete_file)(struct fs_file *file);

	struct fs_iter *(*iter_init)(struct fs *fs, const char *path);
	const char *(*iter_next)(struct fs_iter *iter);
	int (*iter_deinit)(struct fs_iter *iter);
};

struct fs {
	const char *name;
	struct fs_vfuncs v;

	struct fs_settings set;
	string_t *last_error;

	unsigned int files_open_count;
};

struct fs_file {
	struct fs *fs;
	struct ostream *output;
	char *path;

	struct istream *pending_read_input;
	bool write_pending;
};

struct fs_lock {
	struct fs_file *file;
};

struct fs_iter {
	struct fs *fs;
};

extern const struct fs fs_class_posix;
extern const struct fs fs_class_sis;
extern const struct fs fs_class_sis_queue;

void fs_set_error(struct fs *fs, const char *fmt, ...) ATTR_FORMAT(2, 3);
void fs_set_critical(struct fs *fs, const char *fmt, ...) ATTR_FORMAT(2, 3);

void fs_set_error_async(struct fs *fs);

#endif
