/* Copyright (C) 2004 Timo Sirainen, Alex Howansky */

#include "config.h"
#undef HAVE_CONFIG_H

#ifdef USERDB_SQL

#include "common.h"
#include "str.h"
#include "strescape.h"
#include "var-expand.h"
#include "db-sql.h"
#include "userdb.h"

#include <stdlib.h>
#include <string.h>

struct userdb_sql_request {
	struct auth_request *auth_request;
	userdb_callback_t *callback;
};

extern struct userdb_module userdb_sql;

static struct sql_connection *userdb_sql_conn;

static const char *sql_query_get_result(struct sql_result *result,
					struct auth_request *auth_request)
{
	string_t *str;
	uid_t uid, gid;
	const char *name, *value;
	unsigned int i, fields_count;

	uid = (uid_t)-1;
	gid = (gid_t)-1;

	str = t_str_new(256);
	str_append(str, auth_request->user);

	fields_count = sql_result_get_fields_count(result);
	for (i = 0; i < fields_count; i++) {
		name = sql_result_get_field_name(result, i);
		value = sql_result_get_field_value(result, i);

		if (value == NULL)
			continue;

		str_append_c(str, '\t');
		str_append(str, name);
		str_append_c(str, '=');

		/* some special handling for UID and GID. */
		if (strcmp(name, "uid") == 0) {
			uid = userdb_parse_uid(auth_request, value);
			if (uid == (uid_t)-1)
				return NULL;
			value = dec2str(uid);
		} else if (strcmp(name, "gid") == 0) {
			gid = userdb_parse_gid(auth_request, value);
			if (gid == (gid_t)-1)
				return NULL;
			value = dec2str(gid);
		}

		str_append(str, value);
	}

	if (uid == (uid_t)-1) {
		auth_request_log_error(auth_request, "sql",
			"Password query didn't return uid, or it was NULL");
	}
	if (gid == (gid_t)-1) {
		auth_request_log_error(auth_request, "sql",
			"Password query didn't return gid, or it was NULL");
	}

	return str_c(str);
}

static void sql_query_callback(struct sql_result *result, void *context)
{
	struct userdb_sql_request *sql_request = context;
	struct auth_request *auth_request = sql_request->auth_request;
	const char *user_result = NULL;
	int ret;

	ret = sql_result_next_row(result);
	if (ret < 0) {
		auth_request_log_error(auth_request, "sql",
			"User query failed: %s", sql_result_get_error(result));
	} else if (ret == 0) {
		auth_request_log_info(auth_request, "sql", "User not found");
	} else {
                user_result = sql_query_get_result(result, auth_request);
	}

	sql_request->callback(user_result, auth_request);
	i_free(sql_request);
}

static void userdb_sql_lookup(struct auth_request *auth_request,
			      userdb_callback_t *callback)
{
	struct userdb_sql_request *sql_request;
	string_t *query;

	query = t_str_new(512);
	var_expand(query, userdb_sql_conn->set.user_query,
		   auth_request_get_var_expand_table(auth_request,
						     str_escape));

	sql_request = i_new(struct userdb_sql_request, 1);
	sql_request->callback = callback;
	sql_request->auth_request = auth_request;

	auth_request_log_debug(auth_request, "sql", "%s", str_c(query));

	sql_query(userdb_sql_conn->db, str_c(query),
		  sql_query_callback, sql_request);
}

static void userdb_sql_preinit(const char *args)
{
	userdb_sql_conn = db_sql_init(args);
}

static void userdb_sql_init(const char *args __attr_unused__)
{
	enum sql_db_flags flags;

	db_sql_connect(userdb_sql_conn);

	flags = sql_get_flags(userdb_sql_conn->db);
	userdb_sql.blocking = (flags & SQL_DB_FLAG_BLOCKING) != 0;
}

static void userdb_sql_deinit(void)
{
	db_sql_unref(userdb_sql_conn);
}

struct userdb_module userdb_sql = {
	"sql",
	FALSE,

	userdb_sql_preinit,
	userdb_sql_init,
	userdb_sql_deinit,

	userdb_sql_lookup
};

#endif
