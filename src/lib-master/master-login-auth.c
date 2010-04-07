/* Copyright (c) 2009-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "network.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "llist.h"
#include "hex-binary.h"
#include "hash.h"
#include "str.h"
#include "master-auth.h"
#include "master-login-auth.h"

#include <stdlib.h>

#define AUTH_MAX_INBUF_SIZE 8192
#define AUTH_REQUEST_TIMEOUT_SECS (2*60)

struct master_login_auth_request {
	struct master_login_auth_request *prev, *next;

	time_t create_stamp;
	master_login_auth_request_callback_t *callback;
	void *context;
};

struct master_login_auth {
	pool_t pool;
	const char *auth_socket_path;
	int refcount;

	int fd;
	struct io *io;
	struct istream *input;
	struct ostream *output;
	struct timeout *to;

	unsigned int id_counter;
	struct hash_table *requests;
	/* linked list of requests, ordered by create_stamp */
	struct master_login_auth_request *request_head, *request_tail;

	unsigned int version_received:1;
};

static void master_login_auth_set_timeout(struct master_login_auth *auth);

struct master_login_auth *master_login_auth_init(const char *auth_socket_path)
{
	struct master_login_auth *auth;
	pool_t pool;

	pool = pool_alloconly_create("master login auth", 1024);
	auth = p_new(pool, struct master_login_auth, 1);
	auth->pool = pool;
	auth->auth_socket_path = p_strdup(pool, auth_socket_path);
	auth->refcount = 1;
	auth->fd = -1;
	auth->requests = hash_table_create(default_pool, pool, 0, NULL, NULL);
	return auth;
}

void master_login_auth_disconnect(struct master_login_auth *auth)
{
	struct master_login_auth_request *request;

	while (auth->request_head != NULL) {
		request = auth->request_head;
		DLLIST2_REMOVE(&auth->request_head,
			       &auth->request_tail, request);

		request->callback(NULL, MASTER_AUTH_ERRMSG_INTERNAL_FAILURE,
				  request->context);
		i_free(request);
	}
	hash_table_clear(auth->requests, FALSE);

	if (auth->to != NULL)
		timeout_remove(&auth->to);
	if (auth->io != NULL)
		io_remove(&auth->io);
	if (auth->fd != -1) {
		i_stream_destroy(&auth->input);
		o_stream_destroy(&auth->output);

		net_disconnect(auth->fd);
		auth->fd = -1;
	}
	auth->version_received = FALSE;
}

static void master_login_auth_unref(struct master_login_auth **_auth)
{
	struct master_login_auth *auth = *_auth;

	*_auth = NULL;

	i_assert(auth->refcount > 0);
	if (--auth->refcount > 0)
		return;

	hash_table_destroy(&auth->requests);
	pool_unref(&auth->pool);
}

void master_login_auth_deinit(struct master_login_auth **_auth)
{
	struct master_login_auth *auth = *_auth;

	*_auth = NULL;

	master_login_auth_disconnect(auth);
	master_login_auth_unref(&auth);
}

static unsigned int auth_get_next_timeout_secs(struct master_login_auth *auth)
{
	time_t expires;

	expires = auth->request_head->create_stamp + AUTH_REQUEST_TIMEOUT_SECS;
	return expires <= ioloop_time ? 0 : expires - ioloop_time;
}

static void master_login_auth_timeout(struct master_login_auth *auth)
{
	struct master_login_auth_request *request;

	while (auth->request_head != NULL &&
	       auth_get_next_timeout_secs(auth) == 0) {
		request = auth->request_head;
		DLLIST2_REMOVE(&auth->request_head,
			       &auth->request_tail, request);

		i_error("Auth server request timed out after %u secs",
			(unsigned int)(ioloop_time - request->create_stamp));
		request->callback(NULL, MASTER_AUTH_ERRMSG_INTERNAL_FAILURE,
				  request->context);
		i_free(request);
	}
	timeout_remove(&auth->to);
	master_login_auth_set_timeout(auth);
}

static void master_login_auth_set_timeout(struct master_login_auth *auth)
{
	i_assert(auth->to == NULL);

	if (auth->request_head != NULL) {
		auth->to = timeout_add(auth_get_next_timeout_secs(auth) * 1000,
				       master_login_auth_timeout, auth);
	}
}

static struct master_login_auth_request *
master_login_auth_lookup_request(struct master_login_auth *auth,
				 unsigned int id)
{
	struct master_login_auth_request *request;
	bool update_timeout;

	request = hash_table_lookup(auth->requests, POINTER_CAST(id));
	if (request == NULL) {
		i_error("Auth server sent reply with unknown ID %u", id);
		return NULL;
	}
	update_timeout = request->prev == NULL;

	hash_table_remove(auth->requests, POINTER_CAST(id));
	DLLIST2_REMOVE(&auth->request_head, &auth->request_tail, request);

	if (update_timeout) {
		timeout_remove(&auth->to);
		master_login_auth_set_timeout(auth);
	}
	return request;
}

static bool
master_login_auth_input_user(struct master_login_auth *auth, const char *args)
{
	struct master_login_auth_request *request;
	const char *const *list;
	unsigned int id;

	/* <id> <userid> [..] */

	list = t_strsplit(args, "\t");
	if (list[0] == NULL || list[1] == NULL ||
	    str_to_uint(list[0], &id) < 0) {
		i_error("Auth server sent corrupted USER line");
		return FALSE;
	}

	request = master_login_auth_lookup_request(auth, id);
	if (request != NULL) {
		request->callback(list + 1, NULL, request->context);
		i_free(request);
	}
	return TRUE;
}

static bool
master_login_auth_input_notfound(struct master_login_auth *auth,
				 const char *args)
{
	struct master_login_auth_request *request;
	unsigned int id;

	if (str_to_uint(args, &id) < 0) {
		i_error("Auth server sent corrupted NOTFOUND line");
		return FALSE;
	}

	request = master_login_auth_lookup_request(auth, id);
	if (request != NULL) {
		i_error("Authenticated user not found from userdb");
		request->callback(NULL, MASTER_AUTH_ERRMSG_INTERNAL_FAILURE,
				  request->context);
		i_free(request);
	}
	return TRUE;
}

static bool
master_login_auth_input_fail(struct master_login_auth *auth,
			     const char *args_line)
{
	struct master_login_auth_request *request;
 	const char *const *args, *error = NULL;
	unsigned int i, id;

	args = t_strsplit(args_line, "\t");
	if (args[0] == NULL || str_to_uint(args[0], &id) < 0) {
		i_error("Auth server sent broken FAIL line");
		return FALSE;
	}
	for (i = 1; args[i] != NULL; i++) {
		if (strncmp(args[i], "reason=", 7) == 0)
			error = args[i] + 7;
	}

	request = master_login_auth_lookup_request(auth, id);
	if (request != NULL) {
		if (error != NULL)
			i_error("Internal auth failure");
		request->callback(NULL, error != NULL ? error :
				  MASTER_AUTH_ERRMSG_INTERNAL_FAILURE,
				  request->context);
		i_free(request);
	}
	return TRUE;
}

static void master_login_auth_input(struct master_login_auth *auth)
{
	const char *line;
	bool ret;

	switch (i_stream_read(auth->input)) {
	case 0:
		return;
	case -1:
		/* disconnected */
		master_login_auth_disconnect(auth);
		return;
	case -2:
		/* buffer full */
		i_error("Auth server sent us too long line");
		master_login_auth_disconnect(auth);
		return;
	}

	if (!auth->version_received) {
		line = i_stream_next_line(auth->input);
		if (line == NULL)
			return;

		/* make sure the major version matches */
		if (strncmp(line, "VERSION\t", 8) != 0 ||
		    !str_uint_equals(t_strcut(line + 8, '\t'),
				     AUTH_MASTER_PROTOCOL_MAJOR_VERSION)) {
			i_error("Authentication server not compatible with "
				"master process (mixed old and new binaries?)");
			master_login_auth_disconnect(auth);
			return;
		}
		auth->version_received = TRUE;
	}

	auth->refcount++;
	while ((line = i_stream_next_line(auth->input)) != NULL) {
		if (strncmp(line, "USER\t", 5) == 0)
			ret = master_login_auth_input_user(auth, line + 5);
		else if (strncmp(line, "NOTFOUND\t", 9) == 0)
			ret = master_login_auth_input_notfound(auth, line + 9);
		else if (strncmp(line, "FAIL\t", 5) == 0)
			ret = master_login_auth_input_fail(auth, line + 5);
		else
			ret = TRUE;

		if (!ret || auth->input == NULL) {
			master_login_auth_disconnect(auth);
			break;
		}
	}
	master_login_auth_unref(&auth);
}

static int
master_login_auth_connect(struct master_login_auth *auth)
{
	int fd;

	i_assert(auth->fd == -1);

	fd = net_connect_unix_with_retries(auth->auth_socket_path, 1000);
	if (fd == -1) {
		i_error("net_connect_unix(%s) failed: %m",
			auth->auth_socket_path);
		return -1;
	}
	auth->fd = fd;
	auth->input = i_stream_create_fd(fd, AUTH_MAX_INBUF_SIZE, FALSE);
	auth->output = o_stream_create_fd(fd, (size_t)-1, FALSE);
	auth->io = io_add(fd, IO_READ, master_login_auth_input, auth);
	return 0;
}

void master_login_auth_request(struct master_login_auth *auth,
			       const struct master_auth_request *req,
			       master_login_auth_request_callback_t *callback,
			       void *context)
{
	struct master_login_auth_request *login_req;
	unsigned int id;
	string_t *str;

	str = t_str_new(128);
	if (auth->fd == -1) {
		if (master_login_auth_connect(auth) < 0) {
			callback(NULL, MASTER_AUTH_ERRMSG_INTERNAL_FAILURE,
				 context);
			return;
		}
		str_printfa(str, "VERSION\t%u\t%u\n",
			    AUTH_MASTER_PROTOCOL_MAJOR_VERSION,
			    AUTH_MASTER_PROTOCOL_MINOR_VERSION);
	}

	id = ++auth->id_counter;
	if (id == 0)
		id++;

	str_printfa(str, "REQUEST\t%u\t%u\t%u\t", id,
		    req->client_pid, req->auth_id);
	binary_to_hex_append(str, req->cookie, sizeof(req->cookie));
	str_append_c(str, '\n');
	o_stream_send(auth->output, str_data(str), str_len(str));

	login_req = i_new(struct master_login_auth_request, 1);
	login_req->create_stamp = ioloop_time;
	login_req->callback = callback;
	login_req->context = context;
	hash_table_insert(auth->requests, POINTER_CAST(id), login_req);
	DLLIST2_APPEND(&auth->request_head, &auth->request_tail, login_req);

	if (auth->to == NULL)
		master_login_auth_set_timeout(auth);
}

unsigned int master_login_auth_request_count(struct master_login_auth *auth)
{
	return hash_table_count(auth->requests);
}
