/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "hostpid.h"
#include "ostream.h"
#include "write-full.h"
#include "mbox-index.h"
#include "mbox-lock.h"
#include "mbox-storage.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <netdb.h>

static char my_hostdomain[256] = "";

static int write_error(struct mail_storage *storage, const char *mbox_path)
{
	if (errno == ENOSPC)
		mail_storage_set_error(storage, "Not enough disk space");
	else {
		mail_storage_set_critical(storage,
			"Error writing to mbox file %s: %m", mbox_path);
	}

	return FALSE;
}

static int mbox_seek_to_end(struct mail_storage *storage, int fd,
			    const char *mbox_path, off_t *pos)
{
	struct stat st;
	char ch;

	if (fstat(fd, &st) < 0) {
		mail_storage_set_critical(storage,
			"fstat() failed for mbox file %s: %m", mbox_path);
		return FALSE;
	}

	*pos = st.st_size;
	if (st.st_size == 0)
		return TRUE;

	if (lseek(fd, st.st_size-1, SEEK_SET) < 0) {
		mail_storage_set_critical(storage,
			"lseek() failed for mbox file %s: %m", mbox_path);
		return FALSE;
	}

	if (read(fd, &ch, 1) != 1) {
		mail_storage_set_critical(storage,
			"read() failed for mbox file %s: %m", mbox_path);
		return FALSE;
	}

	if (ch != '\n') {
		if (write_full(fd, "\n", 1) < 0)
			return write_error(storage, mbox_path);
		*pos += 1;
	}

	return TRUE;
}

static int mbox_append_lf(struct mail_storage *storage, struct ostream *output,
			  const char *mbox_path)
{
	if (o_stream_send(output, "\n", 1) < 0)
		return write_error(storage, mbox_path);

	return TRUE;
}

static int write_from_line(struct mail_storage *storage, struct ostream *output,
			   const char *mbox_path, time_t internal_date)
{
	const char *sender, *line, *name;

	if (*my_hostdomain == '\0') {
		struct hostent *hent;

		hostpid_init();
		hent = gethostbyname(my_hostname);

		name = hent != NULL ? hent->h_name : NULL;
		if (name == NULL) {
			/* failed, use just the hostname */
			name = my_hostname;
		}

		strocpy(my_hostdomain, name, sizeof(my_hostdomain));
	}

	sender = t_strconcat(storage->user, "@", my_hostdomain, NULL);

	/* save in local timezone, no matter what it was given with */
	line = mbox_from_create(sender, internal_date);

	if (o_stream_send_str(output, line) < 0)
		return write_error(storage, mbox_path);

	return TRUE;
}

static int write_flags(struct mail_storage *storage, struct ostream *output,
		       const char *mbox_path,
		       const struct mail_full_flags *full_flags)
{
	enum mail_flags flags = full_flags->flags;
	const char *str;
	unsigned int field;
	unsigned int i;

	if (flags == 0)
		return TRUE;

	if (flags & MAIL_SEEN) {
		if (o_stream_send_str(output, "Status: R\n") < 0)
			return write_error(storage, mbox_path);
	}

	if (flags & (MAIL_ANSWERED|MAIL_DRAFT|MAIL_FLAGGED|MAIL_DELETED)) {
		str = t_strconcat("X-Status: ",
				  (flags & MAIL_ANSWERED) ? "A" : "",
				  (flags & MAIL_DRAFT) ? "D" : "",
				  (flags & MAIL_FLAGGED) ? "F" : "",
				  (flags & MAIL_DELETED) ? "T" : "",
				  "\n", NULL);

		if (o_stream_send_str(output, str) < 0)
			return write_error(storage, mbox_path);
	}

	if (flags & MAIL_CUSTOM_FLAGS_MASK) {
		if (o_stream_send_str(output, "X-Keywords:") < 0)
			return write_error(storage, mbox_path);

		field = 1 << MAIL_CUSTOM_FLAG_1_BIT;
		for (i = 0; i < full_flags->custom_flags_count; i++) {
			const char *custom_flag = full_flags->custom_flags[i];

			if ((flags & field) && custom_flag != NULL) {
				if (o_stream_send(output, " ", 1) < 0)
					return write_error(storage, mbox_path);

				if (o_stream_send_str(output, custom_flag) < 0)
					return write_error(storage, mbox_path);
			}

                        field <<= 1;
		}

		if (o_stream_send(output, "\n", 1) < 0)
			return write_error(storage, mbox_path);
	}

	return TRUE;
}

int mbox_storage_save(struct mailbox *box, const struct mail_full_flags *flags,
		      time_t internal_date,
		      int timezone_offset __attr_unused__,
		      struct istream *data, uoff_t data_size)
{
	struct index_mailbox *ibox = (struct index_mailbox *) box;
	struct mail_index *index;
	enum mail_flags real_flags;
	const char *mbox_path;
	struct ostream *output;
	int failed;
	off_t pos;

	if (box->readonly) {
		mail_storage_set_error(box->storage, "Mailbox is read-only");
		return FALSE;
	}

	/* we don't need the real flag positions, easier to keep using our own.
	   they need to be checked/added though. */
	real_flags = flags->flags;
	if (!index_mailbox_fix_custom_flags(ibox, &real_flags,
					    flags->custom_flags,
					    flags->custom_flags_count))
		return FALSE;

	if (!index_storage_sync_and_lock(ibox, FALSE, MAIL_LOCK_EXCLUSIVE))
		return FALSE;

	index = ibox->index;
	mbox_path = index->mailbox_path;
	if (!mbox_seek_to_end(box->storage, index->mbox_fd, mbox_path, &pos))
		failed = TRUE;
	else {
		failed = FALSE;

		t_push();
		output = o_stream_create_file(index->mbox_fd,
					      data_stack_pool, 4096,
					      0, FALSE);
		o_stream_set_blocking(output, 60000, NULL, NULL);

		if (!write_from_line(box->storage, output, mbox_path,
				     internal_date) ||
		    !write_flags(box->storage, output, mbox_path, flags) ||
		    !index_storage_save(box->storage, mbox_path,
					data, output, data_size) ||
		    !mbox_append_lf(box->storage, output, mbox_path)) {
			/* failed, truncate file back to original size.
			   output stream needs to be flushed before truncating
			   so unref() won't write anything. */
			o_stream_flush(output);
			(void)ftruncate(index->mbox_fd, pos);
			failed = TRUE;
		}
		o_stream_unref(output);
		t_pop();
	}

	/* kludgy.. for copying inside same mailbox. */
	if (!ibox->delay_save_unlocking) {
		if (!index_storage_lock(ibox, MAIL_LOCK_UNLOCK))
			return FALSE;
	}

	return !failed;
}
