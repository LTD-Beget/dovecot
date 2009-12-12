/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "env-util.h"
#include "fdpass.h"
#include "restrict-access.h"
#include "str.h"
#include "strescape.h"
#include "settings-parser.h"
#include "mail-storage-service.h"
#include "master-interface.h"
#include "master-service.h"
#include "master-service-settings.h"

#include <stdlib.h>
#include <unistd.h>

#define ENV_USERDB_KEYS "USERDB_KEYS"
#define SCRIPT_COMM_FD 3

static char **exec_args;
static bool drop_privileges = FALSE;

static void client_connected(const struct master_service_connection *conn)
{
	string_t *instr, *keys;
	const char **args, *key, *value, *error;
	struct mail_storage_service_ctx *service_ctx;
	struct mail_storage_service_input input;
	struct mail_storage_service_user *user;
	char buf[1024];
	unsigned int i;
	int fd = -1;
	ssize_t ret;

	instr = t_str_new(1024);
	ret = fd_read(conn->fd, buf, sizeof(buf), &fd);
	while (ret > 0) {
		str_append_n(instr, buf, ret);
		if (buf[ret-1] == '\n') {
			str_truncate(instr, str_len(instr)-1);
			break;
		}

		ret = read(conn->fd, buf, sizeof(buf));
	}
	if (ret <= 0) {
		if (ret < 0)
			i_fatal("read() failed: %m");
		else
			i_fatal("read() failed: disconnected");
		(void)close(conn->fd);
		return;
	}
	if (fd == -1)
		i_fatal("client fd not received");

	/* put everything to environment */
	env_clean();
	keys = t_str_new(256);
	args = t_strsplit(str_c(instr), "\t");

	if (str_array_length(args) < 3)
		i_fatal("Missing input fields");

	i = 0;
	memset(&input, 0, sizeof(input));
	input.module = input.service = "script";
	(void)net_addr2ip(args[i++], &input.local_ip);
	(void)net_addr2ip(args[i++], &input.remote_ip);
	input.username = args[i++];
	input.userdb_fields = args + i;

	env_put(t_strconcat("LOCAL_IP=", net_ip2addr(&input.local_ip), NULL));
	env_put(t_strconcat("IP=", net_ip2addr(&input.remote_ip), NULL));
	env_put(t_strconcat("USER=", input.username, NULL));

	for (; args[i] != '\0'; i++) {
		args[i] = str_tabunescape(t_strdup_noconst(args[i]));
		value = strchr(args[i], '=');
		if (value != NULL) {
			key = t_str_ucase(t_strdup_until(args[i], value));
			env_put(t_strconcat(key, value, NULL));
			str_printfa(keys, "%s ", key);
		}
	}
	env_put(t_strconcat(ENV_USERDB_KEYS"=", str_c(keys), NULL));

	master_service_init_log(master_service,
		t_strdup_printf("script(%s): ", input.username));

	service_ctx = mail_storage_service_init(master_service, NULL, 0);
	if (mail_storage_service_lookup(service_ctx, &input, &user, &error) < 0)
		i_fatal("%s", error);
	mail_storage_service_restrict_setenv(service_ctx, user);

	if (drop_privileges)
		restrict_access_by_env(getenv("HOME"), TRUE);

	if (dup2(fd, STDIN_FILENO) < 0)
		i_fatal("dup2() failed: %m");
	if (dup2(fd, STDOUT_FILENO) < 0)
		i_fatal("dup2() failed: %m");
	if (conn->fd != SCRIPT_COMM_FD) {
		if (dup2(conn->fd, SCRIPT_COMM_FD) < 0)
			i_fatal("dup2() failed: %m");
	}

	(void)execvp(exec_args[0], exec_args);
	i_fatal("execvp(%s) failed: %m", exec_args[0]);
}

static void script_execute_finish(void)
{
	const char *keys_str, *username, *const *keys, *value;
	string_t *reply = t_str_new(512);
	ssize_t ret;

	keys_str = getenv(ENV_USERDB_KEYS);
	if (keys_str == NULL)
		i_fatal(ENV_USERDB_KEYS" environment missing");

	username = getenv("USER");
	if (username == NULL)
		i_fatal("USER environment missing");
	str_append(reply, username);

	for (keys = t_strsplit_spaces(keys_str, " "); *keys != NULL; keys++) {
		value = getenv(t_str_ucase(*keys));
		if (value != NULL) {
			str_append_c(reply, '\t');
			str_tabescape_write(reply,
					    t_strconcat(t_str_lcase(*keys), "=",
							value, NULL));
		}
	}
	str_append_c(reply, '\n');

	ret = fd_send(SCRIPT_COMM_FD, STDOUT_FILENO,
		      str_data(reply), str_len(reply));
	if (ret < 0)
		i_fatal("fd_send() failed: %m");
	else if (ret != (ssize_t)str_len(reply))
		i_fatal("fd_send() sent partial output");
}

int main(int argc, char *argv[])
{
	enum master_service_flags flags = 0;
	const char *path;
	int i, c;

	if (getenv(MASTER_UID_ENV) == NULL)
		flags |= MASTER_SERVICE_FLAG_STANDALONE;

	master_service = master_service_init("script", flags,
					     &argc, &argv, "d");
	while ((c = master_getopt(master_service)) > 0) {
		switch (c) {
		case 'd':
			drop_privileges = TRUE;
			break;
		default:
			return FATAL_DEFAULT;
		}
	}
	argc -= optind;
	argv += optind;

	master_service_init_log(master_service, "script: ");
	master_service_init_finish(master_service);
	master_service_set_service_count(master_service, 1);

	if ((flags & MASTER_SERVICE_FLAG_STANDALONE) != 0)
		script_execute_finish();
	else {
		if (argv[0] == NULL)
			i_fatal("Missing script path");
		exec_args = i_new(char *, argc + 2);
		for (i = 0; i < argc; i++)
			exec_args[i] = argv[i];
		exec_args[i] = PKG_LIBEXECDIR"/script";
		exec_args[i+1] = NULL;

		if (exec_args[0][0] != '/') {
			path = t_strconcat(PKG_LIBEXECDIR"/",
					   exec_args[0], NULL);
			exec_args[0] = t_strdup_noconst(path);
		}

		master_service_run(master_service, client_connected);
	}
	master_service_deinit(&master_service);
        return 0;
}
