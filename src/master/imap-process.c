/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "restrict-access.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <syslog.h>
#include <grp.h>

static unsigned int imap_process_count = 0;

static int validate_uid_gid(uid_t uid, gid_t gid)
{
	if (uid == 0) {
		i_error("imap process isn't allowed for root");
		return FALSE;
	}

	if (uid != 0 && gid == 0) {
		i_error("imap process isn't allowed to be in group 0");
		return FALSE;
	}

	if (uid < (uid_t)set_first_valid_uid ||
	    (set_last_valid_uid != 0 && uid > (uid_t)set_last_valid_uid)) {
		i_error("imap process isn't allowed to use UID %ld",
			(long) uid);
		return FALSE;
	}

	if (gid < (gid_t)set_first_valid_gid ||
	    (set_last_valid_gid != 0 && gid > (gid_t)set_last_valid_gid)) {
		i_error("imap process isn't allowed to use "
			"GID %ld (UID is %ld)", (long) gid, (long) uid);
		return FALSE;
	}

	return TRUE;
}

static int validate_chroot(const char *dir)
{
	char *const *chroot_dirs;

	if (*dir == '\0')
		return TRUE;

	if (set_valid_chroot_dirs == '\0')
		return FALSE;

	chroot_dirs = t_strsplit(set_valid_chroot_dirs, ":");
	while (*chroot_dirs != NULL) {
		if (strncmp(dir, *chroot_dirs, strlen(*chroot_dirs)) == 0)
			return TRUE;
		chroot_dirs++;
	}

	return FALSE;
}

MasterReplyResult create_imap_process(int socket, IPADDR *ip, const char *user,
				      uid_t uid, gid_t gid, const char *home,
				      int chroot, const char *env[])
{
	static char *argv[] = { NULL, "-s", NULL, NULL };
	char host[MAX_IP_LEN], title[1024];
	pid_t pid;
	int i, j, err;

	if (imap_process_count == set_max_imap_processes) {
		i_error("Maximum number of imap processes exceeded");
		return MASTER_RESULT_INTERNAL_FAILURE;
	}

	if (!validate_uid_gid(uid, gid))
		return MASTER_RESULT_FAILURE;

	if (chroot && !validate_chroot(home))
		return MASTER_RESULT_FAILURE;

	pid = fork();
	if (pid < 0) {
		i_error("fork() failed: %m");
		return MASTER_RESULT_INTERNAL_FAILURE;
	}

	if (pid != 0) {
		/* master */
		imap_process_count++;
		PID_ADD_PROCESS_TYPE(pid, PROCESS_TYPE_IMAP);
		(void)close(socket);
		return MASTER_RESULT_SUCCESS;
	}

	clean_child_process();

	/* move the imap socket into stdin, stdout and stderr fds */
	for (i = 0; i < 3; i++) {
		if (dup2(socket, i) < 0) {
			err = errno;
			for (j = 0; j < i; j++)
				(void)close(j);
			(void)close(socket);
			i_fatal("imap: dup2() failed: %m");
		}
	}
	(void)close(socket);

	/* setup environment */
	while (env[0] != NULL && env[1] != NULL) {
		putenv((char *) t_strconcat(env[0], "=", env[1], NULL));
		env += 2;
	}

	putenv((char *) t_strconcat("HOME=", home, NULL));
	putenv((char *) t_strconcat("MAIL_CACHE_FIELDS=",
				    set_mail_cache_fields, NULL));
	putenv((char *) t_strconcat("MAIL_NEVER_CACHE_FIELDS=",
				    set_mail_never_cache_fields, NULL));
	putenv((char *) t_strdup_printf("MAILBOX_CHECK_INTERVAL=%u",
					set_mailbox_check_interval));

	if (set_mail_save_crlf)
		putenv("MAIL_SAVE_CRLF=1");
	if (set_maildir_copy_with_hardlinks)
		putenv("MAILDIR_COPY_WITH_HARDLINKS=1");
	if (set_maildir_check_content_changes)
		putenv("MAILDIR_CHECK_CONTENT_CHANGES=1");
	if (set_overwrite_incompatible_index)
		putenv("OVERWRITE_INCOMPATIBLE_INDEX=1");
	if (umask(set_umask) != set_umask)
		i_fatal("Invalid umask: %o", set_umask);

	if (set_verbose_proctitle && net_ip2host(ip, host) == 0) {
		i_snprintf(title, sizeof(title), "[%s %s]", user, host);
		argv[2] = title;
	}

	/* setup access environment - needs to be done after
	   clean_child_process() since it clears environment */
	restrict_access_set_env(user, uid, gid, chroot ? home : NULL);

	/* hide the path, it's ugly */
	argv[0] = strrchr(set_imap_executable, '/');
	if (argv[0] == NULL) argv[0] = set_imap_executable; else argv[0]++;

	execv(set_imap_executable, argv);
	err = errno;

	for (i = 0; i < 3; i++)
		(void)close(i);

	i_fatal("execv(%s) failed: %m", set_imap_executable);

	/* not reached */
	return 0;
}

void imap_process_destroyed(pid_t pid __attr_unused__)
{
	imap_process_count--;
}
