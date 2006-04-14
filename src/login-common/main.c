/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "lib-signals.h"
#include "randgen.h"
#include "restrict-access.h"
#include "restrict-process-size.h"
#include "process-title.h"
#include "fd-close-on-exec.h"
#include "master.h"
#include "client-common.h"
#include "auth-client.h"
#include "ssl-proxy.h"
#include "login-proxy.h"

#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>

bool disable_plaintext_auth, process_per_connection, greeting_capability;
bool verbose_proctitle, verbose_ssl, verbose_auth;
const char *greeting, *log_format;
const char *const *log_format_elements;
unsigned int max_logging_users;
unsigned int login_process_uid;
struct auth_client *auth_client;

static const char *process_name;
static struct ioloop *ioloop;
static struct io *io_listen, *io_ssl_listen;
static int main_refcount;
static bool is_inetd, closing_down;

void main_ref(void)
{
	main_refcount++;
}

void main_unref(void)
{
	if (--main_refcount == 0) {
		/* nothing to do, quit */
		io_loop_stop(ioloop);
	} else if (closing_down && clients_get_count() == 0) {
		/* last login finished, close all communications
		   to master process */
		master_close();
	}
}

void main_close_listen(void)
{
	if (closing_down)
		return;

	if (io_listen != NULL) {
		io_remove(&io_listen);
		if (close(LOGIN_LISTEN_FD) < 0)
			i_fatal("close(listen) failed: %m");
	}

	if (io_ssl_listen != NULL) {
		io_remove(&io_ssl_listen);
		if (close(LOGIN_SSL_LISTEN_FD) < 0)
			i_fatal("close(ssl_listen) failed: %m");
	}

	closing_down = TRUE;
	master_notify_finished();
}

static void sig_die(int signo, void *context __attr_unused__)
{
	/* warn about being killed because of some signal, except SIGINT (^C)
	   which is too common at least while testing :) */
	if (signo != SIGINT)
		i_warning("Killed with signal %d", signo);
	io_loop_stop(ioloop);
}

static void login_accept(void *context __attr_unused__)
{
	struct ip_addr ip, local_ip;
	int fd;

	fd = net_accept(LOGIN_LISTEN_FD, &ip, NULL);
	if (fd < 0) {
		if (fd < -1)
			i_fatal("accept() failed: %m");
		return;
	}

	if (process_per_connection)
		main_close_listen();

	if (net_getsockname(fd, &local_ip, NULL) < 0)
		memset(&local_ip, 0, sizeof(local_ip));

	(void)client_create(fd, FALSE, &local_ip, &ip);
}

static void login_accept_ssl(void *context __attr_unused__)
{
	struct ip_addr ip, local_ip;
	struct client *client;
	struct ssl_proxy *proxy;
	int fd, fd_ssl;

	fd = net_accept(LOGIN_SSL_LISTEN_FD, &ip, NULL);
	if (fd < 0) {
		if (fd < -1)
			i_fatal("accept() failed: %m");
		return;
	}

	if (process_per_connection)
		main_close_listen();
	if (net_getsockname(fd, &local_ip, NULL) < 0)
		memset(&local_ip, 0, sizeof(local_ip));

	fd_ssl = ssl_proxy_new(fd, &ip, &proxy);
	if (fd_ssl == -1)
		net_disconnect(fd);
	else {
		client = client_create(fd_ssl, TRUE, &local_ip, &ip);
		client->proxy = proxy;
	}
}

static void auth_connect_notify(struct auth_client *client __attr_unused__,
				bool connected, void *context __attr_unused__)
{
	if (connected)
                clients_notify_auth_connected();
}

static void drop_privileges(void)
{
	const char *env;

	if (!is_inetd)
		i_set_failure_internal();
	else {
		/* log to syslog */
		env = getenv("SYSLOG_FACILITY");
		i_set_failure_syslog(process_name, LOG_NDELAY,
				     env == NULL ? LOG_MAIL : atoi(env));

		/* if we don't chroot, we must chdir */
		env = getenv("LOGIN_DIR");
		if (env != NULL) {
			if (chdir(env) < 0)
				i_error("chdir(%s) failed: %m", env);
		}
	}

	/* Initialize SSL proxy so it can read certificate and private
	   key file. */
	random_init();
	ssl_proxy_init();

	/* Refuse to run as root - we should never need it and it's
	   dangerous with SSL. */
	restrict_access_by_env(TRUE);

	/* make sure we can't fork() */
	restrict_process_size((unsigned int)-1, 1);
}

static void main_init(void)
{
	const char *value;

	lib_signals_init();
        lib_signals_set_handler(SIGINT, TRUE, sig_die, NULL);
        lib_signals_set_handler(SIGTERM, TRUE, sig_die, NULL);
        lib_signals_set_handler(SIGPIPE, FALSE, NULL, NULL);

	disable_plaintext_auth = getenv("DISABLE_PLAINTEXT_AUTH") != NULL;
	process_per_connection = getenv("PROCESS_PER_CONNECTION") != NULL;
	verbose_proctitle = getenv("VERBOSE_PROCTITLE") != NULL;
        verbose_ssl = getenv("VERBOSE_SSL") != NULL;
        verbose_auth = getenv("VERBOSE_AUTH") != NULL;

	value = getenv("MAX_LOGGING_USERS");
	max_logging_users = value == NULL ? 0 : strtoul(value, NULL, 10);

	greeting = getenv("GREETING");
	if (greeting == NULL)
		greeting = PACKAGE" ready.";
	greeting_capability = getenv("GREETING_CAPABILITY") != NULL;

	value = getenv("LOG_FORMAT_ELEMENTS");
	if (value == NULL)
		value = "user=<%u> method=%m rip=%r lip=%l %c : %$";
	log_format_elements = t_strsplit(value, " ");

	log_format = getenv("LOG_FORMAT");
	if (log_format == NULL)
		log_format = "%$: %s";

	value = getenv("PROCESS_UID");
	if (value == NULL)
		i_fatal("BUG: PROCESS_UID environment not given");
        login_process_uid = strtoul(value, NULL, 10);
	if (login_process_uid == 0)
		i_fatal("BUG: PROCESS_UID environment is 0");

	/* capability default is set in imap/pop3-login */
	value = getenv("CAPABILITY_STRING");
	if (value != NULL && *value != '\0')
		capability_string = value;

        closing_down = FALSE;
	main_refcount = 0;

	auth_client = auth_client_new(login_process_uid);
        auth_client_set_connect_notify(auth_client, auth_connect_notify, NULL);
	clients_init();

	io_listen = io_ssl_listen = NULL;

	if (!is_inetd) {
		if (net_getsockname(LOGIN_LISTEN_FD, NULL, NULL) == 0) {
			io_listen = io_add(LOGIN_LISTEN_FD, IO_READ,
					   login_accept, NULL);
		}

		if (net_getsockname(LOGIN_SSL_LISTEN_FD, NULL, NULL) == 0) {
			if (!ssl_initialized) {
				/* this shouldn't happen, master should have
				   disabled the ssl socket.. */
				i_fatal("BUG: SSL initialization parameters "
					"not given while they should have "
					"been");
			}

			io_ssl_listen = io_add(LOGIN_SSL_LISTEN_FD, IO_READ,
					       login_accept_ssl, NULL);
		}

		/* initialize master last - it sends the "we're ok"
		   notification */
		master_init(LOGIN_MASTER_SOCKET_FD, TRUE);
	}
}

static void main_deinit(void)
{
	if (io_listen != NULL) io_remove(&io_listen);
	if (io_ssl_listen != NULL) io_remove(&io_ssl_listen);

	ssl_proxy_deinit();
	login_proxy_deinit();

	auth_client_free(&auth_client);
	clients_deinit();
	master_deinit();

	lib_signals_deinit();
	closelog();
}

int main(int argc __attr_unused__, char *argv[], char *envp[])
{
	const char *name, *group_name;
	struct ip_addr ip, local_ip;
	unsigned int local_port;
	struct ssl_proxy *proxy = NULL;
	struct client *client;
	int i, fd = -1, master_fd = -1;
	bool ssl = FALSE;

	is_inetd = getenv("DOVECOT_MASTER") == NULL;

#ifdef DEBUG
	if (!is_inetd && getenv("GDB") == NULL)
		fd_debug_verify_leaks(4, 1024);
#endif
	/* NOTE: we start rooted, so keep the code minimal until
	   restrict_access_by_env() is called */
	lib_init();

	if (is_inetd) {
		/* running from inetd. create master process before
		   dropping privileges. */
		process_name = strrchr(argv[0], '/');
		process_name = process_name == NULL ? argv[0] : process_name+1;
		group_name = t_strcut(process_name, '-');

		for (i = 1; i < argc; i++) {
			if (strncmp(argv[i], "--group=", 8) == 0) {
				group_name = argv[1]+8;
				break;
			}
		}

		master_fd = master_connect(group_name);
	}

	name = strrchr(argv[0], '/');
	drop_privileges();

	process_title_init(argv, envp);
	ioloop = io_loop_create(system_pool);
	main_init();

	if (is_inetd) {
		if (net_getpeername(1, &ip, NULL) < 0) {
			i_fatal("%s can be started only through dovecot "
				"master process, inetd or equilevant", argv[0]);
		}
		if (net_getsockname(1, &local_ip, &local_port) < 0) {
			memset(&local_ip, 0, sizeof(local_ip));
			local_port = 0;
		}

		fd = 1;
		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--ssl") == 0)
				ssl = TRUE;
			else if (strncmp(argv[i], "--group=", 8) != 0)
				i_fatal("Unknown parameter: %s", argv[i]);
		}

		/* hardcoded imaps and pop3s ports to be SSL by default */
		if (local_port == 993 || local_port == 995 || ssl) {
			ssl = TRUE;
			fd = ssl_proxy_new(fd, &ip, &proxy);
			if (fd == -1)
				return 1;
		}

		master_init(master_fd, FALSE);
		closing_down = TRUE;

		if (fd != -1) {
			client = client_create(fd, ssl, &local_ip, &ip);
			client->proxy = proxy;
		}
	}

	io_loop_run(ioloop);
	main_deinit();

	io_loop_destroy(&ioloop);
	lib_deinit();

        return 0;
}
