/* Copyright (c) 2005-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "abspath.h"
#include "env-util.h"
#include "ostream.h"
#include "str.h"
#include "settings-parser.h"
#include "master-service.h"
#include "all-settings.h"
#include "sysinfo-get.h"
#include "config-connection.h"
#include "config-parser.h"
#include "config-request.h"

#include <stdio.h>
#include <unistd.h>

struct prefix_stack {
	unsigned int prefix_idx;
	unsigned int str_pos;
};
ARRAY_DEFINE_TYPE(prefix_stack, struct prefix_stack);

struct config_request_get_string_ctx {
	pool_t pool;
	ARRAY_TYPE(const_string) strings;
	ARRAY_TYPE(const_string) errors;
};

#define LIST_KEY_PREFIX "\001"
#define UNIQUE_KEY_SUFFIX "\xff"

static void
config_request_get_strings(const char *key, const char *value,
			   enum config_key_type type, void *context)
{
	struct config_request_get_string_ctx *ctx = context;
	const char *p;

	switch (type) {
	case CONFIG_KEY_NORMAL:
		value = p_strdup_printf(ctx->pool, "%s=%s", key, value);
		break;
	case CONFIG_KEY_LIST:
		value = p_strdup_printf(ctx->pool, LIST_KEY_PREFIX"%s=%s",
					key, value);
		break;
	case CONFIG_KEY_UNIQUE_KEY:
		p = strrchr(key, '/');
		i_assert(p != NULL);
		value = p_strdup_printf(ctx->pool, "%s/"UNIQUE_KEY_SUFFIX"%s=%s",
					t_strdup_until(key, p), p + 1, value);
		break;
	case CONFIG_KEY_ERROR:
		value = p_strdup(ctx->pool, value);
		array_append(&ctx->errors, &value, 1);
		return;
	}
	array_append(&ctx->strings, &value, 1);
}

static int config_string_cmp(const char *const *p1, const char *const *p2)
{
	const char *s1 = *p1, *s2 = *p2;
	unsigned int i = 0;

	while (s1[i] == s2[i]) {
		if (s1[i] == '\0' || s1[i] == '=')
			return 0;
		i++;
	}

	if (s1[i] == '=')
		return -1;
	if (s2[i] == '=')
		return 1;

	return s1[i] - s2[i];
}

static struct prefix_stack prefix_stack_pop(ARRAY_TYPE(prefix_stack) *stack)
{
	const struct prefix_stack *s;
	struct prefix_stack sc;
	unsigned int count;

	s = array_get(stack, &count);
	i_assert(count > 0);
	if (count == 1) {
		sc.prefix_idx = -1U;
	} else {
		sc.prefix_idx = s[count-2].prefix_idx;
	}
	sc.str_pos = s[count-1].str_pos;
	array_delete(stack, count-1, 1);
	return sc;
}

static void prefix_stack_reset_str(ARRAY_TYPE(prefix_stack) *stack)
{
	struct prefix_stack *s;

	array_foreach_modifiable(stack, s)
		s->str_pos = -1U;
}

static int config_connection_request_human(struct ostream *output,
					   const struct config_filter *filter,
					   const char *module,
					   enum config_dump_scope scope)
{
	static const char *indent_str = "               ";
	ARRAY_TYPE(const_string) prefixes_arr;
	ARRAY_TYPE(prefix_stack) prefix_stack;
	struct config_export_context *export_ctx;
	struct prefix_stack prefix;
	struct config_request_get_string_ctx ctx;
	const char *const *strings, *const *args, *p, *str, *const *prefixes;
	const char *key, *key2, *value;
	unsigned int i, j, count, len, prefix_count, skip_len;
	unsigned int indent = 0, prefix_idx = -1U;
	string_t *list_prefix;
	bool unique_key;
	int ret = 0;

	ctx.pool = pool_alloconly_create("config human strings", 10240);
	i_array_init(&ctx.strings, 256);
	i_array_init(&ctx.errors, 256);
	export_ctx = config_export_init(filter, module, scope,
					CONFIG_DUMP_FLAG_CHECK_SETTINGS |
					CONFIG_DUMP_FLAG_HIDE_LIST_DEFAULTS |
					CONFIG_DUMP_FLAG_CALLBACK_ERRORS,
					config_request_get_strings, &ctx);
	if (config_export_finish(&export_ctx) < 0)
		return -1;

	array_sort(&ctx.strings, config_string_cmp);
	strings = array_get(&ctx.strings, &count);

	p_array_init(&prefixes_arr, ctx.pool, 32);
	for (i = 0; i < count && strings[i][0] == LIST_KEY_PREFIX[0]; i++) T_BEGIN {
		p = strchr(strings[i], '=');
		i_assert(p != NULL);
		for (args = t_strsplit(p + 1, " "); *args != NULL; args++) {
			str = p_strdup_printf(ctx.pool, "%s/%s/",
					      t_strcut(strings[i]+1, '='),
					      *args);
			array_append(&prefixes_arr, &str, 1);
		}
	} T_END;
	prefixes = array_get(&prefixes_arr, &prefix_count);

	list_prefix = str_new(ctx.pool, 128);
	p_array_init(&prefix_stack, ctx.pool, 8);
	for (; i < count; i++) T_BEGIN {
		value = strchr(strings[i], '=');
		i_assert(value != NULL);

		key = t_strdup_until(strings[i], value++);
		unique_key = FALSE;

		p = strrchr(key, '/');
		if (p != NULL && p[1] == UNIQUE_KEY_SUFFIX[0]) {
			key = t_strconcat(t_strdup_until(key, p + 1),
					  p + 2, NULL);
			unique_key = TRUE;
		}

	again:
		j = 0;
		while (prefix_idx != -1U) {
			len = strlen(prefixes[prefix_idx]);
			if (strncmp(prefixes[prefix_idx], key, len) != 0) {
				prefix = prefix_stack_pop(&prefix_stack);
				indent--;
				if (prefix.str_pos != -1U)
					str_truncate(list_prefix, prefix.str_pos);
				else {
					o_stream_send(output, indent_str, indent*2);
					o_stream_send_str(output, "}\n");
				}
				prefix_idx = prefix.prefix_idx;
			} else {
				/* keep the prefix */
				j = prefix_idx + 1;
				break;
			}
		}
		for (; j < prefix_count; j++) {
			len = strlen(prefixes[j]);
			if (strncmp(prefixes[j], key, len) == 0) {
				key2 = key + (prefix_idx == -1U ? 0 :
					      strlen(prefixes[prefix_idx]));
				prefix.str_pos = !unique_key ? -1U :
					str_len(list_prefix);
				prefix_idx = j;
				prefix.prefix_idx = prefix_idx;
				array_append(&prefix_stack, &prefix, 1);

				str_append_n(list_prefix, indent_str, indent*2);
				p = strchr(key2, '/');
				if (p != NULL)
					str_append_n(list_prefix, key2, p - key2);
				else
					str_append(list_prefix, key2);
				if (unique_key && *value != '\0')
					str_printfa(list_prefix, " %s", value);
				str_append(list_prefix, " {\n");
				indent++;

				if (unique_key)
					goto end;
				else
					goto again;
			}
		}
		o_stream_send(output, str_data(list_prefix), str_len(list_prefix));
		str_truncate(list_prefix, 0);
		prefix_stack_reset_str(&prefix_stack);

		skip_len = prefix_idx == -1U ? 0 : strlen(prefixes[prefix_idx]);
		i_assert(skip_len == 0 ||
			 strncmp(prefixes[prefix_idx], strings[i], skip_len) == 0);
		o_stream_send(output, indent_str, indent*2);
		key = strings[i] + skip_len;
		if (unique_key) key++;
		value = strchr(key, '=');
		o_stream_send(output, key, value-key);
		o_stream_send_str(output, " = ");
		o_stream_send_str(output, value+1);
		o_stream_send(output, "\n", 1);
	end: ;
	} T_END;

	while (prefix_idx != -1U) {
		prefix = prefix_stack_pop(&prefix_stack);
		prefix_idx = prefix.prefix_idx;
		indent--;
		o_stream_send(output, indent_str, indent*2);
		o_stream_send_str(output, "}\n");
	}

	/* flush output before writing errors */
	o_stream_uncork(output);
	array_foreach(&ctx.errors, strings) {
		i_error("%s", *strings);
		ret = -1;
	}

	array_free(&ctx.strings);
	array_free(&ctx.errors);
	pool_unref(&ctx.pool);
	return ret;
}

static int
config_dump_human(const struct config_filter *filter, const char *module,
		  enum config_dump_scope scope)
{
	struct ostream *output;
	int ret;

	output = o_stream_create_fd(STDOUT_FILENO, 0, FALSE);
	o_stream_cork(output);
	ret = config_connection_request_human(output, filter, module, scope);
	o_stream_uncork(output);
	o_stream_unref(&output);
	return ret;
}

static void config_request_putenv(const char *key, const char *value,
				  enum config_key_type type ATTR_UNUSED,
				  void *context ATTR_UNUSED)
{
	T_BEGIN {
		env_put(t_strconcat(t_str_ucase(key), "=", value, NULL));
	} T_END;
}

static const char *get_mail_location(void)
{
	struct config_module_parser *l;
	const struct setting_define *def;
	const char *const *value;
	const void *set;

	for (l = config_module_parsers; l->root != NULL; l++) {
		if (strcmp(l->root->module_name, "mail") != 0)
			continue;

		set = settings_parser_get(l->parser);
		for (def = l->root->defines; def->key != NULL; def++) {
			if (strcmp(def->key, "mail_location") == 0) {
				value = CONST_PTR_OFFSET(set, def->offset);
				return *value;
			}
		}
	}
	return "";
}

static void filter_parse_arg(struct config_filter *filter, const char *arg)
{
	if (strncmp(arg, "service=", 8) == 0)
		filter->service = arg + 8;
	else if (strncmp(arg, "protocol=", 9) == 0)
		filter->service = arg + 9;
	else if (strncmp(arg, "lhost=", 6) == 0)
		filter->local_host = arg + 6;
	else if (strncmp(arg, "rhost=", 6) == 0)
		filter->remote_host = arg + 6;
	else if (strncmp(arg, "lip=", 4) == 0) {
		if (net_parse_range(arg + 4, &filter->local_net,
				    &filter->local_bits) < 0)
			i_fatal("lip: Invalid network mask");
	} else if (strncmp(arg, "rip=", 4) == 0) {
		if (net_parse_range(arg + 4, &filter->remote_net,
				    &filter->remote_bits) < 0)
			i_fatal("rip: Invalid network mask");
	} else {
		i_fatal("Unknown filter argument: %s", arg);
	}
}

static void check_wrong_config(const char *config_path)
{
	const char *prev_path;

	if (t_readlink(PKG_RUNDIR"/"PACKAGE".conf", &prev_path) < 0)
		return;

	if (strcmp(prev_path, config_path) != 0) {
		i_warning("Dovecot was last started using %s, "
			  "but this config is %s", prev_path, config_path);
	}
}

int main(int argc, char *argv[])
{
	enum config_dump_scope scope = CONFIG_DUMP_SCOPE_ALL;
	const char *orig_config_path, *config_path, *module = "";
	struct config_filter filter;
	const char *error;
	char **exec_args = NULL;
	int c, ret, ret2;
	bool config_path_specified;

	memset(&filter, 0, sizeof(filter));
	master_service = master_service_init("config",
					     MASTER_SERVICE_FLAG_STANDALONE,
					     &argc, &argv, "af:m:nNe");
	orig_config_path = master_service_get_config_path(master_service);

	i_set_failure_prefix("doveconf: ");
	while ((c = master_getopt(master_service)) > 0) {
		if (c == 'e')
			break;
		switch (c) {
		case 'a':
			break;
		case 'f':
			filter_parse_arg(&filter, optarg);
			break;
		case 'm':
			module = optarg;
			break;
		case 'n':
			scope = CONFIG_DUMP_SCOPE_CHANGED;
			break;
		case 'N':
			scope = CONFIG_DUMP_SCOPE_SET;
			break;
		default:
			return FATAL_DEFAULT;
		}
	}
	config_path = master_service_get_config_path(master_service);
	/* use strcmp() instead of !=, because dovecot -n always gives us
	   -c parameter */
	config_path_specified = strcmp(config_path, orig_config_path) != 0;

	if (argv[optind] != NULL) {
		if (c != 'e')
			i_fatal("Unknown argument: %s", argv[optind]);
		exec_args = &argv[optind];
	} else {
		/* print the config file path before parsing it, so in case
		   of errors it's still shown */
		printf("# "VERSION": %s\n", config_path);
		fflush(stdout);
	}
	master_service_init_finish(master_service);
	config_parse_load_modules();

	if ((ret = config_parse_file(config_path, FALSE, &error)) == 0 &&
	    access(EXAMPLE_CONFIG_DIR, X_OK) == 0) {
		i_fatal("%s (copy example configs from "EXAMPLE_CONFIG_DIR"/)",
			error);
	}

	if (ret <= 0 && (exec_args != NULL || ret == -2))
		i_fatal("%s", error);

	if (exec_args == NULL) {
		const char *info;

		info = sysinfo_get(get_mail_location());
		if (*info != '\0')
			printf("# %s\n", info);
		if (!config_path_specified)
			check_wrong_config(config_path);
		fflush(stdout);
		ret2 = config_dump_human(&filter, module, scope);

		if (ret < 0) {
			/* delayed error */
			i_fatal("%s", error);
		}
		if (ret2 < 0)
			i_fatal("Errors in configuration");
	} else {
		struct config_export_context *ctx;

		env_put("DOVECONF_ENV=1");
		ctx = config_export_init(&filter, module,
					 CONFIG_DUMP_SCOPE_SET,
					 CONFIG_DUMP_FLAG_CHECK_SETTINGS,
					 config_request_putenv, NULL);
		if (config_export_finish(&ctx) < 0)
			i_fatal("Invalid configuration");
		execvp(exec_args[0], exec_args);
		i_fatal("execvp(%s) failed: %m", exec_args[0]);
	}
	config_filter_deinit(&config_filter);
	master_service_deinit(&master_service);
        return 0;
}
