/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "safe-memset.h"
#include "auth.h"
#include "cookie.h"
#include "userinfo.h"

static void auth_plain_continue(struct cookie_data *cookie,
				struct auth_continued_request_data *request,
				const unsigned char *data,
				auth_callback_t callback, void *context)
{
	struct auth_cookie_reply_data *cookie_reply = cookie->context;
	struct auth_reply_data reply;
	const char *authid, *authenid;
	char *pass;
	size_t i, count, len;

	/* initialize reply */
	memset(&reply, 0, sizeof(reply));
	reply.id = request->id;
	reply.result = AUTH_RESULT_FAILURE;
	memcpy(reply.cookie, cookie->cookie, AUTH_COOKIE_SIZE);

	/* authorization ID \0 authentication ID \0 pass.
	   we'll ignore authorization ID for now. */
	authid = (const char *) data;
	authenid = NULL; pass = NULL;

	count = 0;
	for (i = 0; i < request->data_size; i++) {
		if (data[i] == '\0') {
			if (++count == 1)
				authenid = data + i+1;
			else {
				i++;
				len = request->data_size - i;
				pass = t_malloc(len+1);
				memcpy(pass, data + i, len);
				pass[len] = '\0';
				break;
			}
		}
	}

	if (pass != NULL) {
		if (userinfo->verify_plain(authenid, pass, cookie_reply)) {
			cookie_reply->success = TRUE;
			reply.result = AUTH_RESULT_SUCCESS;

			if (strocpy(reply.virtual_user,
				    cookie_reply->virtual_user,
				    sizeof(reply.virtual_user)) < 0)
				i_panic("virtual_user overflow");
		}

		if (*pass != '\0') {
			/* make sure it's cleared */
			safe_memset(pass, 0, strlen(pass));
		}
	}

        callback(&reply, NULL, context);

	if (!cookie_reply->success) {
		/* failed, we don't need the cookie anymore */
		cookie_remove(cookie->cookie);
	}
}

static int auth_plain_fill_reply(struct cookie_data *cookie,
				 struct auth_cookie_reply_data *reply)
{
	struct auth_cookie_reply_data *cookie_reply;

	cookie_reply = cookie->context;
	if (!cookie_reply->success)
		return FALSE;

	memcpy(reply, cookie_reply, sizeof(struct auth_cookie_reply_data));
	return TRUE;
}

static void auth_plain_free(struct cookie_data *cookie)
{
	i_free(cookie->context);
	i_free(cookie);
}

static void auth_plain_init(unsigned int login_pid,
			    struct auth_init_request_data *request,
			    auth_callback_t callback, void *context)
{
	struct cookie_data *cookie;
	struct auth_reply_data reply;

	cookie = i_new(struct cookie_data, 1);
	cookie->login_pid = login_pid;
	cookie->auth_fill_reply = auth_plain_fill_reply;
	cookie->auth_continue = auth_plain_continue;
	cookie->free = auth_plain_free;
	cookie->context = i_new(struct auth_cookie_reply_data, 1);

	cookie_add(cookie);

	/* initialize reply */
	memset(&reply, 0, sizeof(reply));
	reply.id = request->id;
	reply.result = AUTH_RESULT_CONTINUE;
	memcpy(reply.cookie, cookie->cookie, AUTH_COOKIE_SIZE);

	callback(&reply, NULL, context);
}

struct auth_module auth_plain = {
	AUTH_MECH_PLAIN,
	auth_plain_init
};
