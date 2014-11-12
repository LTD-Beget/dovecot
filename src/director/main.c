/* Copyright (c) 2010-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "restrict-access.h"
#include "master-interface.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "auth-connection.h"
#include "doveadm-connection.h"
#include "login-connection.h"
#include "notify-connection.h"
#include "director.h"
#include "director-host.h"
#include "director-connection.h"
#include "director-request.h"
#include "mail-host.h"

#include <stdio.h>
#include <unistd.h>

#define AUTH_SOCKET_PATH "auth-login"
#define AUTH_USERDB_SOCKET_PATH "auth-userdb"

static struct director *director;
static struct notify_connection *notify_conn;

static int director_client_connected(int fd, const struct ip_addr *ip)
{
	struct director_host *host;

	host = director_host_lookup_ip(director, ip);
	if (host == NULL || host->removed) {
		i_warning("Connection from %s: Server not listed in "
			  "director_servers, dropping", net_ip2addr(ip));
		return -1;
	}

	(void)director_connection_init_in(director, fd, ip);
	return 0;
}

static void client_connected(struct master_service_connection *conn)
{
	struct auth_connection *auth;
	const char *socket_path;
	struct ip_addr ip;
	unsigned int local_port, len;
	bool userdb;

	if (conn->fifo) {
		if (notify_conn != NULL) {
			i_error("Received another proxy-notify connection");
			return;
		}
		master_service_client_connection_accept(conn);
		notify_conn = notify_connection_init(director, conn->fd);
		return;
	}

	if (net_getpeername(conn->fd, &ip, NULL) == 0 &&
	    net_getsockname(conn->fd, NULL, &local_port) == 0 &&
	    (IPADDR_IS_V4(&ip) || IPADDR_IS_V6(&ip))) {
		/* TCP/IP connection */
		if (local_port == director->set->director_doveadm_port) {
			master_service_client_connection_accept(conn);
			(void)doveadm_connection_init(director, conn->fd);
		} else {
			if (director_client_connected(conn->fd, &ip) == 0)
				master_service_client_connection_accept(conn);
		}
		return;
	}

	len = strlen(conn->name);
	if (len > 6 && strcmp(conn->name + len - 6, "-admin") == 0) {
		/* doveadm connection */
		master_service_client_connection_accept(conn);
		(void)doveadm_connection_init(director, conn->fd);
		return;
	}

	/* a) userdb connection, probably for lmtp proxy
	   b) login connection
	   Both of them are handled exactly the same, except for which
	   auth socket they connect to. */
	userdb = len > 7 && strcmp(conn->name + len - 7, "-userdb") == 0;
	socket_path = userdb ? AUTH_USERDB_SOCKET_PATH : AUTH_SOCKET_PATH;
	auth = auth_connection_init(socket_path);
	if (auth_connection_connect(auth) == 0) {
		master_service_client_connection_accept(conn);
		(void)login_connection_init(director, conn->fd, auth, userdb);
	} else {
		auth_connection_deinit(&auth);
	}
}

static unsigned int
find_inet_listener_port(struct ip_addr *ip_r,
			const struct director_settings *set)
{
	unsigned int i, socket_count, port;

	socket_count = master_service_get_socket_count(master_service);
	for (i = 0; i < socket_count; i++) {
		int fd = MASTER_LISTEN_FD_FIRST + i;

		if (net_getsockname(fd, ip_r, &port) == 0 && port > 0 &&
		    port != set->director_doveadm_port)
			return port;
	}
	return 0;
}

static void director_state_changed(struct director *dir)
{
	struct director_request *const *requestp;
	ARRAY(struct director_request *) new_requests;
	bool ret;

	if (!dir->ring_synced || !mail_hosts_have_usable(dir->mail_hosts))
		return;

	/* if there are any pending client requests, finish them now */
	t_array_init(&new_requests, 8);
	array_foreach(&dir->pending_requests, requestp) {
		ret = director_request_continue(*requestp);
		if (!ret) {
			/* a) request for a user being killed
			   b) user is weak */
			array_append(&new_requests, requestp, 1);
		}
	}
	array_clear(&dir->pending_requests);
	array_append_array(&dir->pending_requests, &new_requests);

	if (dir->to_request != NULL && array_count(&new_requests) == 0)
		timeout_remove(&dir->to_request);
}

static void main_preinit(void)
{
	const struct director_settings *set;
	struct ip_addr listen_ip;
	unsigned int listen_port;

	set = master_service_settings_get_others(master_service)[0];

	listen_port = find_inet_listener_port(&listen_ip, set);
	if (listen_port == 0 && *set->director_servers != '\0') {
		i_fatal("No inet_listeners defined for director service "
			"(for standalone keep director_servers empty)");
	}

	director = director_init(set, &listen_ip, listen_port,
				 director_state_changed);
	director_host_add_from_string(director, set->director_servers);
	director_find_self(director);
	if (mail_hosts_parse_and_add(director->mail_hosts,
				     set->director_mail_servers) < 0)
		i_fatal("Invalid value for director_mail_servers setting");
	director->orig_config_hosts = mail_hosts_dup(director->mail_hosts);

	restrict_access_by_env(NULL, FALSE);
	restrict_access_allow_coredumps(TRUE);
}

static void main_deinit(void)
{
	if (notify_conn != NULL)
		notify_connection_deinit(&notify_conn);
	director_deinit(&director);
	doveadm_connections_deinit();
	login_connections_deinit();
	auth_connections_deinit();
}

int main(int argc, char *argv[])
{
	const struct setting_parser_info *set_roots[] = {
		&director_setting_parser_info,
		NULL
	};
	const enum master_service_flags service_flags =
		MASTER_SERVICE_FLAG_NO_IDLE_DIE |
		MASTER_SERVICE_FLAG_UPDATE_PROCTITLE;
	unsigned int test_port = 0;
	const char *error;
	bool debug = FALSE;
	int c;

	master_service = master_service_init("director", service_flags,
					     &argc, &argv, "Dt:");
	while ((c = master_getopt(master_service)) > 0) {
		switch (c) {
		case 'D':
			debug = TRUE;
			break;
		case 't':
			if (str_to_uint(optarg, &test_port) < 0)
				i_fatal("-t: Not a number: %s", optarg);
			break;
		default:
			return FATAL_DEFAULT;
		}
	}
	if (master_service_settings_read_simple(master_service, set_roots,
						&error) < 0)
		i_fatal("Error reading configuration: %s", error);

	master_service_init_log(master_service, "director: ");

	main_preinit();
	director->test_port = test_port;
	director_debug = debug;
	director_connect(director);

	if (director->test_port != 0) {
		/* we're testing, possibly writing to same log file.
		   make it clear which director we are. */
		master_service_init_log(master_service,
			t_strdup_printf("director(%s): ",
					net_ip2addr(&director->self_ip)));
	}
	master_service_init_finish(master_service);

	master_service_run(master_service, client_connected);
	main_deinit();

	master_service_deinit(&master_service);
        return 0;
}
