/* Copyright (C) 2005 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "buffer.h"
#include "base64.h"
#include "hash.h"
#include "str.h"
#include "str-sanitize.h"
#include "auth-request.h"
#include "auth-request-handler.h"

#include <stdlib.h>

struct auth_request_handler {
	int refcount;
	pool_t pool;
	struct hash_table *requests;

        struct auth *auth;
        unsigned int connect_uid, client_pid;

	auth_request_callback_t *callback;
	void *context;

	auth_request_callback_t *master_callback;
	void *master_context;

	unsigned int prepend_connect_uid:1;
};

static buffer_t *auth_failures_buf;
static struct timeout *to_auth_failures;

static struct auth_request_handler *
_create(struct auth *auth, int prepend_connect_uid,
	auth_request_callback_t *callback, void *context,
	auth_request_callback_t *master_callback, void *master_context)
{
	struct auth_request_handler *handler;
	pool_t pool;

	pool = pool_alloconly_create("auth request handler", 4096);

	handler = p_new(pool, struct auth_request_handler, 1);
	handler->refcount = 1;
	handler->pool = pool;
	handler->requests = hash_create(default_pool, pool, 0, NULL, NULL);
	handler->auth = auth;
	handler->callback = callback;
	handler->context = context;
	handler->master_callback = master_callback;
	handler->master_context = master_context;
	handler->prepend_connect_uid = prepend_connect_uid;
	return handler;
}

static void _set(struct auth_request_handler *handler,
		 unsigned int connect_uid, unsigned int client_pid)
{
	handler->connect_uid = connect_uid;
	handler->client_pid = client_pid;
}

static void _unref(struct auth_request_handler *handler)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	i_assert(handler->refcount > 0);
	if (--handler->refcount > 0)
		return;

	iter = hash_iterate_init(handler->requests);
	while (hash_iterate(iter, &key, &value))
		auth_request_unref(value);
	hash_iterate_deinit(iter);

	/* notify parent that we're done with all requests */
	handler->callback(NULL, handler->context);

	hash_destroy(handler->requests);
	pool_unref(handler->pool);
}

static void auth_request_handler_remove(struct auth_request_handler *handler,
					struct auth_request *request)
{
	hash_remove(handler->requests, POINTER_CAST(request->id));
	auth_request_unref(request);
}

static void _check_timeouts(struct auth_request_handler *handler)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	iter = hash_iterate_init(handler->requests);
	while (hash_iterate(iter, &key, &value)) {
		struct auth_request *request = value;

		if (request->created + AUTH_REQUEST_TIMEOUT < ioloop_time)
			auth_request_handler_remove(handler, request);
	}
	hash_iterate_deinit(iter);
}

static const char *get_client_extra_fields(struct auth_request *request)
{
	const char **fields;
	unsigned int src, dest;

	if (request->extra_fields == NULL)
		return NULL;

	/* we only wish to remove all fields prefixed with "userdb_" */
	if (strstr(request->extra_fields, "userdb_") == NULL)
		return request->extra_fields;

	fields = t_strsplit(request->extra_fields, "\t");
	for (src = dest = 0; fields[src] != NULL; src++) {
		if (strncmp(fields[src], "userdb_", 7) != 0)
			fields[dest++] = fields[src];
	}
	fields[dest] = NULL;
	return t_strarray_join(fields, "\t");
}

static void auth_callback(struct auth_request *request,
			  enum auth_client_result result,
			  const void *reply, size_t reply_size)
{
        struct auth_request_handler *handler = request->context;
	string_t *str;
	const char *fields;

	t_push();

	str = t_str_new(128 + MAX_BASE64_ENCODED_SIZE(reply_size));
	if (handler->prepend_connect_uid)
		str_printfa(str, "%u\t", request->connect_uid);

	switch (result) {
	case AUTH_CLIENT_RESULT_CONTINUE:
		str_printfa(str, "CONT\t%u\t", request->id);
		base64_encode(reply, reply_size, str);
                request->accept_input = TRUE;
		handler->callback(str_c(str), handler->context);
		break;
	case AUTH_CLIENT_RESULT_SUCCESS:
		str_printfa(str, "OK\t%u\tuser=%s", request->id, request->user);
		if (reply_size > 0) {
			str_append(str, "\tresp=");
			base64_encode(reply, reply_size, str);
		}
		fields = get_client_extra_fields(request);
		if (fields != NULL) {
			str_append_c(str, '\t');
			str_append(str, fields);
		}

		if (request->no_login || handler->master_callback == NULL) {
			/* this request doesn't have to wait for master
			   process to pick it up. delete it */
			auth_request_handler_remove(handler, request);
		}
		handler->callback(str_c(str), handler->context);
		break;
	case AUTH_CLIENT_RESULT_FAILURE:
		str_printfa(str, "FAIL\t%u", request->id);
		if (request->user != NULL)
			str_printfa(str, "\tuser=%s", request->user);
		if (request->internal_failure)
			str_append(str, "\ttemp");
		fields = get_client_extra_fields(request);
		if (fields != NULL) {
			str_append_c(str, '\t');
			str_append(str, fields);
		}

		if (request->delayed_failure) {
			/* we came here from flush_failures() */
			handler->callback(str_c(str), handler->context);
			break;
		}

		/* remove the request from requests-list */
		auth_request_ref(request);
		auth_request_handler_remove(handler, request);

		if (request->no_failure_delay) {
			/* passdb specifically requested not to delay the
			   reply. */
			handler->callback(str_c(str), handler->context);
			auth_request_unref(request);
		} else {
			/* failure. don't announce it immediately to avoid
			   a) timing attacks, b) flooding */
			request->delayed_failure = TRUE;
			handler->refcount++;
			buffer_append(auth_failures_buf,
				      &request, sizeof(request));
		}
		break;
	}
	/* NOTE: request may be destroyed now */

        auth_request_handler_unref(handler);

	t_pop();
}

static void auth_request_handler_auth_fail(struct auth_request_handler *handler,
					   struct auth_request *request,
					   const char *reason)
{
	string_t *reply = t_str_new(64);

	auth_request_log_info(request, request->mech->mech_name, "%s", reason);

	if (handler->prepend_connect_uid)
		str_printfa(reply, "%u\t", request->connect_uid);
	str_printfa(reply, "FAIL\t%u\treason=%s", request->id, reason);
	handler->callback(str_c(reply), handler->context);

	auth_request_handler_remove(handler, request);
}

static int _auth_begin(struct auth_request_handler *handler, const char *args)
{
	struct mech_module *mech;
	struct auth_request *request;
	const char *const *list, *name, *arg, *initial_resp;
	const void *initial_resp_data;
	size_t initial_resp_len;
	unsigned int id;
	buffer_t *buf;
	int valid_client_cert;

	/* <id> <mechanism> [...] */
	list = t_strsplit(args, "\t");
	if (list[0] == NULL || list[1] == NULL) {
		i_error("BUG: Authentication client %u "
			"sent broken AUTH request", handler->client_pid);
		return FALSE;
	}

	id = (unsigned int)strtoul(list[0], NULL, 10);

	mech = mech_module_find(list[1]);
	if (mech == NULL) {
		/* unsupported mechanism */
		i_error("BUG: Authentication client %u requested unsupported "
			"authentication mechanism %s", handler->client_pid,
			str_sanitize(list[1], MAX_MECH_NAME_LEN));
		return FALSE;
	}

	request = auth_request_new(handler->auth, mech, auth_callback, handler);
	request->connect_uid = handler->connect_uid;
	request->client_pid = handler->client_pid;
	request->id = id;

	/* parse optional parameters */
	initial_resp = NULL;
	valid_client_cert = FALSE;
	for (list += 2; *list != NULL; list++) {
		arg = strchr(*list, '=');
		if (arg == NULL) {
			name = *list;
			arg = "";
		} else {
			name = t_strdup_until(*list, arg);
			arg++;
		}

		if (strcmp(name, "lip") == 0)
			(void)net_addr2ip(arg, &request->local_ip);
		else if (strcmp(name, "rip") == 0)
			(void)net_addr2ip(arg, &request->remote_ip);
		else if (strcmp(name, "service") == 0)
			request->service = p_strdup(request->pool, arg);
		else if (strcmp(name, "resp") == 0)
			initial_resp = arg;
		else if (strcmp(name, "valid-client-cert") == 0)
			valid_client_cert = TRUE;
	}

	if (request->service == NULL) {
		i_error("BUG: Authentication client %u "
			"didn't specify service in request",
			handler->client_pid);
		auth_request_unref(request);
		return FALSE;
	}

	hash_insert(handler->requests, POINTER_CAST(id), request);

	if (request->auth->ssl_require_client_cert && !valid_client_cert) {
		/* we fail without valid certificate */
                auth_request_handler_auth_fail(handler, request,
			"Client didn't present valid SSL certificate");
		return TRUE;
	}

	if (initial_resp == NULL) {
		initial_resp_data = NULL;
		initial_resp_len = 0;
	} else {
		size_t len = strlen(initial_resp);
		buf = buffer_create_dynamic(pool_datastack_create(),
					    MAX_BASE64_DECODED_SIZE(len));
		if (base64_decode(initial_resp, len, NULL, buf) < 0) {
                        auth_request_handler_auth_fail(handler, request,
				"Invalid base64 data in initial response");
			return TRUE;
		}
		initial_resp_data = buf->data;
		initial_resp_len = buf->used;
	}

	/* handler is referenced until auth_callback is called. */
	handler->refcount++;
	auth_request_initial(request, initial_resp_data, initial_resp_len);
	return TRUE;
}

static int
_auth_continue(struct auth_request_handler *handler, const char *args)
{
	struct auth_request *request;
	const char *data;
	size_t data_len;
	buffer_t *buf;
	unsigned int id;

	data = strchr(args, '\t');
	if (data++ == NULL) {
		i_error("BUG: Authentication client sent broken CONT request");
		return FALSE;
	}

	id = (unsigned int)strtoul(args, NULL, 10);

	request = hash_lookup(handler->requests, POINTER_CAST(id));
	if (request == NULL) {
		string_t *reply = t_str_new(64);

		if (handler->prepend_connect_uid)
			str_printfa(reply, "%u\t", handler->connect_uid);
		str_printfa(reply, "FAIL\t%u\treason=Timeouted", id);
		handler->callback(str_c(reply), handler->context);
		return TRUE;
	}

	/* accept input only once after mechanism has sent a CONT reply */
	if (!request->accept_input) {
		auth_request_handler_auth_fail(handler, request,
					       "Unexpected continuation");
		return TRUE;
	}
	request->accept_input = FALSE;

	data_len = strlen(data);
	buf = buffer_create_dynamic(pool_datastack_create(),
				    MAX_BASE64_DECODED_SIZE(data_len));
	if (base64_decode(data, data_len, NULL, buf) < 0) {
		auth_request_handler_auth_fail(handler, request,
			"Invalid base64 data in continued response");
		return TRUE;
	}

	/* handler is referenced until auth_callback is called. */
	handler->refcount++;
	auth_request_continue(request, buf->data, buf->used);
	return TRUE;
}

static void append_user_reply(string_t *str, const struct user_data *user)
{
	const char *p;

	str_printfa(str, "%s\tuid=%s\tgid=%s", user->virtual_user,
		    dec2str(user->uid), dec2str(user->gid));

	if (user->system_user != NULL)
		str_printfa(str, "\tsystem_user=%s", user->system_user);
	if (user->mail != NULL)
		str_printfa(str, "\tmail=%s", user->mail);

	p = user->home != NULL ? strstr(user->home, "/./") : NULL;
	if (p == NULL) {
		if (user->home != NULL)
			str_printfa(str, "\thome=%s", user->home);
	} else {
		/* wu-ftpd like <chroot>/./<home> */
		str_printfa(str, "\thome=%s\tchroot=%s",
			    p + 3, t_strdup_until(user->home, p));
	}
}

static void userdb_callback(const struct user_data *user, void *context)
{
        struct auth_request *request = context;
        struct auth_request_handler *handler = request->context;
	string_t *reply;

	if (user != NULL) {
		auth_request_log_debug(request, "userdb",
				       "uid=%s gid=%s home=%s mail=%s",
				       dec2str(user->uid), dec2str(user->gid),
				       user->home != NULL ? user->home : "",
				       user->mail != NULL ? user->mail : "");
	}

	reply = t_str_new(256);
	if (handler->prepend_connect_uid)
		str_printfa(reply, "%u\t", request->connect_uid);
	if (user == NULL)
		str_printfa(reply, "NOTFOUND\t%u", request->id);
	else {
		str_printfa(reply, "USER\t%u\t", request->id);
		append_user_reply(reply, user);
	}
	handler->master_callback(str_c(reply), handler->master_context);

	auth_request_unref(request);
        auth_request_handler_unref(handler);
}

static void _master_request(struct auth_request_handler *handler,
			    unsigned int id, unsigned int client_id)
{
	struct auth_request *request;
	string_t *reply;

	reply = t_str_new(64);
	if (handler->prepend_connect_uid)
		str_printfa(reply, "%u\t", handler->connect_uid);

	request = hash_lookup(handler->requests, POINTER_CAST(client_id));
	if (request == NULL) {
		i_error("Master request %u.%u not found",
			handler->client_pid, client_id);
		str_printfa(reply, "NOTFOUND\t%u", id);
		handler->master_callback(str_c(reply), handler->master_context);
		return;
	}

	auth_request_ref(request);
	auth_request_handler_remove(handler, request);

	if (!request->successful) {
		i_error("Master requested unfinished authentication request "
			"%u.%u", handler->client_pid, client_id);
		str_printfa(reply, "NOTFOUND\t%u", id);
		handler->master_callback(str_c(reply), handler->master_context);
	} else {
		/* the request isn't being referenced anywhere anymore,
		   so we can do a bit of kludging.. replace the request's
		   old client_id with master's id. */
		request->id = id;
		request->context = handler;

		/* handler is referenced until userdb_callback is called. */
		handler->refcount++;
		auth_request_lookup_user(request, userdb_callback, request);
	}
}

static void _flush_failures(void)
{
	struct auth_request **auth_request;
	size_t i, size;

	auth_request = buffer_get_modifyable_data(auth_failures_buf, &size);
	size /= sizeof(*auth_request);

	for (i = 0; i < size; i++) {
		auth_request[i]->callback(auth_request[i],
					  AUTH_CLIENT_RESULT_FAILURE, NULL, 0);
		auth_request_unref(auth_request[i]);
	}
	buffer_set_used_size(auth_failures_buf, 0);
}

static void auth_failure_timeout(void *context __attr_unused__)
{
	_flush_failures();
}

static void _init(void)
{
	auth_failures_buf = buffer_create_dynamic(default_pool, 1024);
        to_auth_failures = timeout_add(2000, auth_failure_timeout, NULL);
}

static void _deinit(void)
{
	buffer_free(auth_failures_buf);
	timeout_remove(to_auth_failures);
}

struct auth_request_handler_api auth_request_handler_default = {
	_create,
	_unref,
	_set,
	_check_timeouts,
	_auth_begin,
	_auth_continue,
	_master_request,
	_flush_failures,
	_init,
	_deinit
};
