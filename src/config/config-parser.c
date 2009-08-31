/* Copyright (C) 2005-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "hash.h"
#include "strescape.h"
#include "istream.h"
#include "settings-parser.h"
#include "all-settings.h"
#include "config-filter.h"
#include "config-parser.h"

#include <unistd.h>
#include <fcntl.h>
#ifdef HAVE_GLOB_H
#  include <glob.h>
#endif

#ifndef GLOB_BRACE
#  define GLOB_BRACE 0
#endif

#define IS_WHITE(c) ((c) == ' ' || (c) == '\t')

struct config_filter_stack {
	struct config_filter_stack *prev;
	struct config_filter filter;
	unsigned int pathlen;
};

struct input_stack {
	struct input_stack *prev;

	struct istream *input;
	const char *path;
	unsigned int linenum;
};

struct parser_context {
	pool_t pool;
	const char *path;

	ARRAY_DEFINE(all_parsers, struct config_filter_parser_list *);
	/* parsers matching cur_filter */
	ARRAY_TYPE(config_setting_parsers) cur_parsers;
	struct config_setting_parser_list *root_parsers;
	struct config_filter_stack *cur_filter;
	struct input_stack *cur_input;

	struct config_filter_context *filter;
};

struct config_setting_parser_list *config_setting_parsers;
struct config_filter_context *config_filter;

static const char *info_type_name_find(const struct setting_parser_info *info)
{
	unsigned int i;

	for (i = 0; info->defines[i].key != NULL; i++) {
		if (info->defines[i].offset == info->type_offset)
			return info->defines[i].key;
	}
	i_panic("setting parser: Invalid type_offset value");
	return NULL;
}

static void config_add_type(struct setting_parser_context *parser,
			    const char *line, const char *section_name)
{
	const struct setting_parser_info *info;
	const char *p;
	string_t *str;
	int ret;

	info = settings_parse_get_prev_info(parser);
	if (info->type_offset == (size_t)-1)
		return;

	str = t_str_new(256);
	p = strchr(line, '=');
	str_append_n(str, line, p-line);
	str_append_c(str, SETTINGS_SEPARATOR);
	str_append(str, p+1);
	str_append_c(str, SETTINGS_SEPARATOR);
	str_append(str, info_type_name_find(info));
	str_append_c(str, '=');
	str_append(str, section_name);

	ret = settings_parse_line(parser, str_c(str));
	i_assert(ret > 0);
}

static int
config_parsers_parse_line(struct config_setting_parser_list *parsers,
			  const char *key, const char *line,
			  const char *section_name, const char **error_r)
{
	struct config_setting_parser_list *l;
	bool found = FALSE;
	int ret;

	for (l = parsers; l->module_name != NULL; l++) {
		ret = settings_parse_line(l->parser, line);
		if (ret > 0) {
			found = TRUE;
			if (section_name != NULL)
				config_add_type(l->parser, line, section_name);
		} else if (ret < 0) {
			*error_r = settings_parser_get_error(l->parser);
			return -1;
		}
	}
	if (!found) {
		*error_r = t_strconcat("Unknown setting: ", key, NULL);
		return -1;
	}
	return 0;
}

static int
config_apply_line(struct config_setting_parser_list *const *all_parsers,
		  const char *key, const char *line, const char *section_name,
		  const char **error_r)
{
	for (; *all_parsers != NULL; all_parsers++) {
		if (config_parsers_parse_line(*all_parsers, key, line,
					      section_name, error_r) < 0)
			return -1;
	}
	*error_r = NULL;
	return 0;
}

static const char *
fix_relative_path(const char *path, struct input_stack *input)
{
	const char *p;

	if (*path == '/')
		return path;

	p = strrchr(input->path, '/');
	if (p == NULL)
		return path;

	return t_strconcat(t_strdup_until(input->path, p+1), path, NULL);
}

static struct config_setting_parser_list *
config_setting_parser_list_dup(pool_t pool,
			       const struct config_setting_parser_list *src)
{
	struct config_setting_parser_list *dest;
	unsigned int i, count;

	for (count = 0; src[count].module_name != NULL; count++) ;

	dest = p_new(pool, struct config_setting_parser_list, count + 1);
	for (i = 0; i < count; i++) {
		dest[i] = src[i];
		dest[i].parser = settings_parser_dup(src[i].parser, pool);
	}
	return dest;
}

static struct config_filter_parser_list *
config_add_new_parser(struct parser_context *ctx)
{
	struct config_filter_parser_list *parser;
	struct config_setting_parser_list *const *cur_parsers;
	unsigned int count;

	parser = p_new(ctx->pool, struct config_filter_parser_list, 1);
	parser->filter = ctx->cur_filter->filter;

	cur_parsers = array_get(&ctx->cur_parsers, &count);
	if (count == 0) {
		/* first one */
		parser->parser_list = ctx->root_parsers;
	} else {
		/* duplicate the first settings list */
		parser->parser_list =
			config_setting_parser_list_dup(ctx->pool,
						       cur_parsers[0]);
	}

	array_append(&ctx->all_parsers, &parser, 1);
	return parser;
}

static void config_add_new_filter(struct parser_context *ctx)
{
	struct config_filter_stack *filter;

	filter = p_new(ctx->pool, struct config_filter_stack, 1);
	filter->prev = ctx->cur_filter;
	filter->filter = ctx->cur_filter->filter;
	ctx->cur_filter = filter;
}

static struct config_setting_parser_list *const *
config_update_cur_parsers(struct parser_context *ctx)
{
	struct config_filter_parser_list *const *all_parsers;
	unsigned int i, count;
	bool full_found = FALSE;

	array_clear(&ctx->cur_parsers);

	all_parsers = array_get(&ctx->all_parsers, &count);
	for (i = 0; i < count; i++) {
		if (!config_filter_match(&ctx->cur_filter->filter,
					 &all_parsers[i]->filter))
			continue;

		if (config_filters_equal(&all_parsers[i]->filter,
					 &ctx->cur_filter->filter)) {
			array_insert(&ctx->cur_parsers, 0,
				     &all_parsers[i]->parser_list, 1);
			full_found = TRUE;
		} else {
			array_append(&ctx->cur_parsers,
				     &all_parsers[i]->parser_list, 1);
		}
	}
	i_assert(full_found);
	(void)array_append_space(&ctx->cur_parsers);
	return array_idx(&ctx->cur_parsers, 0);
}

static int
config_filter_parser_list_check(struct parser_context *ctx,
				struct config_filter_parser_list *parser,
				const char **error_r)
{
	struct config_setting_parser_list *l = parser->parser_list;
	const char *errormsg;

	for (; l->module_name != NULL; l++) {
		if (!settings_parser_check(l->parser, ctx->pool, &errormsg)) {
			*error_r = t_strdup_printf(
				"Error in configuration file %s: %s",
				ctx->path, errormsg);
			return -1;
		}
	}
	return 0;
}

static int
config_all_parsers_check(struct parser_context *ctx, const char **error_r)
{
	struct config_filter_parser_list *const *parsers;
	unsigned int i, count;

	parsers = array_get(&ctx->all_parsers, &count);
	for (i = 0; i < count; i++) {
		if (config_filter_parser_list_check(ctx, parsers[i],
						    error_r) < 0)
			return -1;
	}
	return 0;
}

static int
str_append_file(string_t *str, const char *key, const char *path,
		const char **error_r)
{
	unsigned char buf[1024];
	int fd;
	ssize_t ret;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		*error_r = t_strdup_printf("%s: Can't open file %s: %m",
					   key, path);
		return -1;
	}
	while ((ret = read(fd, buf, sizeof(buf))) > 0)
		str_append_n(str, buf, ret);
	if (ret < 0) {
		*error_r = t_strdup_printf("%s: read(%s) failed: %m",
					   key, path);
	}
	(void)close(fd);
	return ret < 0 ? -1 : 0;
}

static int settings_add_include(struct parser_context *ctx, const char *path,
				bool ignore_errors, const char **error_r)
{
	struct input_stack *tmp, *new_input;
	int fd;

	for (tmp = ctx->cur_input; tmp != NULL; tmp = tmp->prev) {
		if (strcmp(tmp->path, path) == 0)
			break;
	}
	if (tmp != NULL) {
		*error_r = t_strdup_printf("Recursive include file: %s", path);
		return -1;
	}

	if ((fd = open(path, O_RDONLY)) == -1) {
		if (ignore_errors)
			return 0;

		*error_r = t_strdup_printf("Couldn't open include file %s: %m",
					   path);
		return -1;
	}

	new_input = t_new(struct input_stack, 1);
	new_input->prev = ctx->cur_input;
	new_input->path = t_strdup(path);
	new_input->input = i_stream_create_fd(fd, 2048, TRUE);
	i_stream_set_return_partial_line(new_input->input, TRUE);
	ctx->cur_input = new_input;
	return 0;
}

static int
settings_include(struct parser_context *ctx, const char *pattern,
		 bool ignore_errors, const char **error_r)
{
#ifdef HAVE_GLOB
	glob_t globbers;
	unsigned int i;

	switch (glob(pattern, GLOB_BRACE, NULL, &globbers)) {
	case 0:
		break;
	case GLOB_NOSPACE:
		*error_r = "glob() failed: Not enough memory";
		return -1;
	case GLOB_ABORTED:
		*error_r = "glob() failed: Read error";
		return -1;
	case GLOB_NOMATCH:
		if (ignore_errors)
			return 0;
		*error_r = "No matches";
		return -1;
	default:
		*error_r = "glob() failed: Unknown error";
		return -1;
	}

	/* iterate throuth the different files matching the globbing */
	for (i = 0; i < globbers.gl_pathc; i++) {
		if (settings_add_include(ctx, globbers.gl_pathv[i],
					 ignore_errors, error_r) < 0)
			return -1;
	}
	globfree(&globbers);
	return 0;
#else
	return settings_add_include(ctx, pattern, ignore_errors, error_r);
#endif
}

enum config_line_type {
	CONFIG_LINE_TYPE_SKIP,
	CONFIG_LINE_TYPE_ERROR,
	CONFIG_LINE_TYPE_KEYVALUE,
	CONFIG_LINE_TYPE_SECTION_BEGIN,
	CONFIG_LINE_TYPE_SECTION_END,
	CONFIG_LINE_TYPE_INCLUDE,
	CONFIG_LINE_TYPE_INCLUDE_TRY
};

static enum config_line_type
config_parse_line(char *line, string_t *full_line, const char **key_r,
		  const char **section_r, const char **value_r)
{
	const char *key;
	unsigned int len;
	char *p;

	*key_r = NULL;
	*value_r = NULL;
	*section_r = NULL;

	/* @UNSAFE: line is modified */

	/* skip whitespace */
	while (IS_WHITE(*line))
		line++;

	/* ignore comments or empty lines */
	if (*line == '#' || *line == '\0')
		return CONFIG_LINE_TYPE_SKIP;

	/* strip away comments. pretty kludgy way really.. */
	for (p = line; *p != '\0'; p++) {
		if (*p == '\'' || *p == '"') {
			char quote = *p;
			for (p++; *p != quote && *p != '\0'; p++) {
				if (*p == '\\' && p[1] != '\0')
					p++;
			}
			if (*p == '\0')
				break;
		} else if (*p == '#') {
			*p = '\0';
			break;
		}
	}

	/* remove whitespace from end of line */
	len = strlen(line);
	while (IS_WHITE(line[len-1]))
		len--;
	line[len] = '\0';

	if (len > 0 && line[len-1] == '\\') {
		/* continues in next line */
		line[len-1] = '\0';
		str_append(full_line, line);
		return CONFIG_LINE_TYPE_SKIP;
	}
	if (str_len(full_line) > 0) {
		str_append(full_line, line);
		line = str_c_modifiable(full_line);
	}

	/* a) key = value
	   b) section_type [section_name] {
	   c) } */
	key = line;
	while (!IS_WHITE(*line) && *line != '\0' && *line != '=')
		line++;
	if (IS_WHITE(*line)) {
		*line++ = '\0';
		while (IS_WHITE(*line)) line++;
	}
	*key_r = key;
	*value_r = line;

	if (strcmp(key, "!include") == 0)
		return CONFIG_LINE_TYPE_INCLUDE;
	if (strcmp(key, "!include_try") == 0)
		return CONFIG_LINE_TYPE_INCLUDE_TRY;

	if (*line == '=') {
		/* a) */
		*line++ = '\0';
		while (IS_WHITE(*line)) line++;

		len = strlen(line);
		if (len > 0 &&
		    ((*line == '"' && line[len-1] == '"') ||
		     (*line == '\'' && line[len-1] == '\''))) {
			line[len-1] = '\0';
			line = str_unescape(line+1);
		}
		*value_r = line;
		return CONFIG_LINE_TYPE_KEYVALUE;
	}

	if (strcmp(key, "}") == 0 && *line == '\0')
		return CONFIG_LINE_TYPE_SECTION_END;

	/* b) + errors */
	line[-1] = '\0';

	if (*line == '{')
		*section_r = "";
	else {
		/* get section name */
		*section_r = line;
		while (!IS_WHITE(*line) && *line != '\0')
			line++;

		if (*line != '\0') {
			*line++ = '\0';
			while (IS_WHITE(*line))
				line++;
		}
		if (*line != '{') {
			*value_r = "Expecting '='";
			return CONFIG_LINE_TYPE_ERROR;
		}
		*value_r = line;
	}
	return CONFIG_LINE_TYPE_SECTION_BEGIN;
}

int config_parse_file(const char *path, bool expand_files,
		      const char **error_r)
{
	enum settings_parser_flags parser_flags =
                SETTINGS_PARSER_FLAG_IGNORE_UNKNOWN_KEYS;
	struct input_stack root;
	struct config_setting_parser_list *const *parsers;
	struct parser_context ctx;
	unsigned int pathlen = 0;
	unsigned int i, count, counter = 0, cur_counter;
	const char *errormsg, *key, *value, *section;
	string_t *str, *full_line;
	enum config_line_type type;
	char *line;
	int fd, ret = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		*error_r = t_strdup_printf("open(%s) failed: %m", path);
		return 0;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.pool = pool_alloconly_create("config file parser", 1024*64);
	ctx.path = path;

	for (count = 0; all_roots[count].module_name != NULL; count++) ;
	ctx.root_parsers =
		p_new(ctx.pool, struct config_setting_parser_list, count+1);
	for (i = 0; i < count; i++) {
		ctx.root_parsers[i].module_name = all_roots[i].module_name;
		ctx.root_parsers[i].root = all_roots[i].root;
		ctx.root_parsers[i].parser =
			settings_parser_init(ctx.pool, all_roots[i].root,
					     parser_flags);
	}

	t_array_init(&ctx.cur_parsers, 128);
	p_array_init(&ctx.all_parsers, ctx.pool, 128);
	ctx.cur_filter = p_new(ctx.pool, struct config_filter_stack, 1);
	config_add_new_parser(&ctx);
	parsers = config_update_cur_parsers(&ctx);

	memset(&root, 0, sizeof(root));
	root.path = path;
	ctx.cur_input = &root;

	str = t_str_new(256);
	full_line = t_str_new(512);
	errormsg = NULL;
	ctx.cur_input->input = i_stream_create_fd(fd, (size_t)-1, TRUE);
	i_stream_set_return_partial_line(ctx.cur_input->input, TRUE);
prevfile:
	while ((line = i_stream_read_next_line(ctx.cur_input->input)) != NULL) {
		ctx.cur_input->linenum++;
		type = config_parse_line(line, full_line,
					 &key, &section, &value);
		switch (type) {
		case CONFIG_LINE_TYPE_SKIP:
			break;
		case CONFIG_LINE_TYPE_ERROR:
			errormsg = value;
			break;
		case CONFIG_LINE_TYPE_KEYVALUE:
			str_truncate(str, pathlen);
			str_append(str, key);
			str_append_c(str, '=');

			if (*value != '<' || !expand_files)
				str_append(str, value);
			else if (str_append_file(str, key, value+1, &errormsg) < 0) {
				/* file reading failed */
				break;
			}
			(void)config_apply_line(parsers, key, str_c(str), NULL, &errormsg);
			break;
		case CONFIG_LINE_TYPE_SECTION_BEGIN:
			config_add_new_filter(&ctx);
			ctx.cur_filter->pathlen = pathlen;
			if (strcmp(key, "protocol") == 0) {
				ctx.cur_filter->filter.service =
					p_strdup(ctx.pool, section);
				config_add_new_parser(&ctx);
				parsers = config_update_cur_parsers(&ctx);
			} else if (strcmp(key, "local_ip") == 0) {
				if (net_parse_range(section, &ctx.cur_filter->filter.local_net,
						    &ctx.cur_filter->filter.local_bits) < 0)
					errormsg = "Invalid network mask";
				config_add_new_parser(&ctx);
				parsers = config_update_cur_parsers(&ctx);
			} else if (strcmp(key, "remote_ip") == 0) {
				if (net_parse_range(section, &ctx.cur_filter->filter.remote_net,
						    &ctx.cur_filter->filter.remote_bits) < 0)
					errormsg = "Invalid network mask";
				config_add_new_parser(&ctx);
				parsers = config_update_cur_parsers(&ctx);
			} else {
				str_truncate(str, pathlen);
				str_append(str, key);
				pathlen = str_len(str);
				cur_counter = counter++;

				str_append_c(str, '=');
				str_printfa(str, "%u", cur_counter);

				if (config_apply_line(parsers, key, str_c(str), section, &errormsg) < 0)
					break;

				str_truncate(str, pathlen);
				str_append_c(str, SETTINGS_SEPARATOR);
				str_printfa(str, "%u", cur_counter);
				str_append_c(str, SETTINGS_SEPARATOR);
				pathlen = str_len(str);
			}
			break;
		case CONFIG_LINE_TYPE_SECTION_END:
			if (ctx.cur_filter->prev == NULL)
				errormsg = "Unexpected '}'";
			else {
				pathlen = ctx.cur_filter->pathlen;
				ctx.cur_filter = ctx.cur_filter->prev;
				parsers = config_update_cur_parsers(&ctx);
			}
			break;
		case CONFIG_LINE_TYPE_INCLUDE:
		case CONFIG_LINE_TYPE_INCLUDE_TRY:
			(void)settings_include(&ctx, fix_relative_path(value, ctx.cur_input),
					       type == CONFIG_LINE_TYPE_INCLUDE_TRY,
					       &errormsg);
			break;
		}

		if (errormsg != NULL) {
			*error_r = t_strdup_printf(
				"Error in configuration file %s line %d: %s",
				ctx.cur_input->path, ctx.cur_input->linenum,
				errormsg);
			ret = -1;
			break;
		}
		str_truncate(full_line, 0);
	}

	i_stream_destroy(&ctx.cur_input->input);
	ctx.cur_input = ctx.cur_input->prev;
	if (line == NULL && ctx.cur_input != NULL)
		goto prevfile;

	if (ret == 0) {
		if (config_all_parsers_check(&ctx, error_r) < 0)
			ret = -1;
	}
	if (ret < 0) {
		pool_unref(&ctx.pool);
		return -1;
	}

	if (config_filter != NULL)
		config_filter_deinit(&config_filter);
	config_setting_parsers = ctx.root_parsers;

	(void)array_append_space(&ctx.all_parsers);
	config_filter = config_filter_init(ctx.pool);
	config_filter_add_all(config_filter, array_idx(&ctx.all_parsers, 0));
	return 1;
}
