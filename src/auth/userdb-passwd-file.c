/* Copyright (C) 2002-2003 Timo Sirainen */

#include "common.h"

#ifdef USERDB_PASSWD_FILE

#include "str.h"
#include "auth-cache.h"
#include "var-expand.h"
#include "userdb.h"
#include "db-passwd-file.h"

#define PASSWD_FILE_CACHE_KEY "%u"

struct passwd_file_userdb_module {
        struct userdb_module module;

	struct auth *auth;
	struct db_passwd_file *pwf;
};

static void passwd_file_lookup(struct auth_request *auth_request,
			       userdb_callback_t *callback)
{
	struct userdb_module *_module = auth_request->userdb->userdb;
	struct passwd_file_userdb_module *module =
		(struct passwd_file_userdb_module *)_module;
	struct auth_stream_reply *reply;
	struct passwd_user *pu;
	const char *key, *value;
	char **p;

	pu = db_passwd_file_lookup(module->pwf, auth_request);
	if (pu == NULL) {
		callback(USERDB_RESULT_USER_UNKNOWN, NULL, auth_request);
		return;
	}

	reply = auth_stream_reply_init(auth_request);
	auth_stream_reply_add(reply, NULL, auth_request->user);
	auth_stream_reply_add(reply, "uid", dec2str(pu->uid));
	auth_stream_reply_add(reply, "gid", dec2str(pu->gid));

	if (pu->home != NULL)
		auth_stream_reply_add(reply, "home", pu->home);

	if (pu->extra_fields != NULL) {
		t_push();
		for (p = pu->extra_fields; *p != NULL; p++) {
			if (strncmp(*p, "userdb_", 7) != 0)
				continue;

			key = *p + 7;
			value = strchr(key, '=');
			if (value != NULL)
				key = t_strdup_until(key, value++);
			auth_stream_reply_add(reply, key, value);
		}
		t_pop();
	}

	callback(USERDB_RESULT_OK, reply, auth_request);
}

static struct userdb_module *
passwd_file_preinit(struct auth_userdb *auth_userdb, const char *args)
{
	struct passwd_file_userdb_module *module;

	module = p_new(auth_userdb->auth->pool,
		       struct passwd_file_userdb_module, 1);
	module->auth = auth_userdb->auth;
	module->pwf =
		db_passwd_file_init(args, TRUE, module->auth->verbose_debug);

	if (!module->pwf->vars)
		module->module.cache_key = PASSWD_FILE_CACHE_KEY;
	else {
		module->module.cache_key =
			auth_cache_parse_key(auth_userdb->auth->pool,
					     t_strconcat(PASSWD_FILE_CACHE_KEY,
						         module->pwf->path,
							 NULL));
	}
	return &module->module;
}

static void passwd_file_init(struct userdb_module *_module,
			     const char *args __attr_unused__)
{
	struct passwd_file_userdb_module *module =
		(struct passwd_file_userdb_module *)_module;

	db_passwd_file_parse(module->pwf);
}

static void passwd_file_deinit(struct userdb_module *_module)
{
	struct passwd_file_userdb_module *module =
		(struct passwd_file_userdb_module *)_module;

	db_passwd_file_unref(&module->pwf);
}

struct userdb_module_interface userdb_passwd_file = {
	"passwd-file",

	passwd_file_preinit,
	passwd_file_init,
	passwd_file_deinit,

	passwd_file_lookup
};

#endif
