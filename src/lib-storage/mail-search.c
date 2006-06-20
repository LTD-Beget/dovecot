/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "mail-search.h"

void mail_search_args_reset(struct mail_search_arg *args, bool full_reset)
{
	while (args != NULL) {
		if (args->type == SEARCH_OR || args->type == SEARCH_SUB)
			mail_search_args_reset(args->value.subargs, full_reset);

		if (!args->match_always)
			args->result = -1;
		else {
			if (!full_reset)
				args->result = 1;
			else {
				args->match_always = FALSE;
				args->result = -1;
			}
		}

		args = args->next;
	}
}

static void search_arg_foreach(struct mail_search_arg *arg,
			       mail_search_foreach_callback_t callback,
			       void *context)
{
	struct mail_search_arg *subarg;

	if (arg->result != -1)
		return;

	if (arg->type == SEARCH_SUB) {
		/* sublist of conditions */
		i_assert(arg->value.subargs != NULL);

		arg->result = 1;
		subarg = arg->value.subargs;
		while (subarg != NULL) {
			if (subarg->result == -1)
				search_arg_foreach(subarg, callback, context);

			if (subarg->result == -1)
				arg->result = -1;
			else if (subarg->result == 0) {
				/* didn't match */
				arg->result = 0;
				break;
			}

			subarg = subarg->next;
		}
		if (arg->not && arg->result != -1)
			arg->result = !arg->result;
	} else if (arg->type == SEARCH_OR) {
		/* OR-list of conditions */
		i_assert(arg->value.subargs != NULL);

		subarg = arg->value.subargs;
		arg->result = 0;
		while (subarg != NULL) {
			if (subarg->result == -1)
				search_arg_foreach(subarg, callback, context);

			if (subarg->result == -1)
				arg->result = -1;
			else if (subarg->result > 0) {
				/* matched */
				arg->result = 1;
				break;
			}

			subarg = subarg->next;
		}
		if (arg->not && arg->result != -1)
			arg->result = !arg->result;
	} else {
		/* just a single condition */
		callback(arg, context);
	}
}

int mail_search_args_foreach(struct mail_search_arg *args,
			     mail_search_foreach_callback_t callback,
			     void *context)
{
	int result;

	result = 1;
	for (; args != NULL; args = args->next) {
		search_arg_foreach(args, callback, context);

		if (args->result == 0) {
			/* didn't match */
			return 0;
		}

		if (args->result == -1)
			result = -1;
	}

	return result;
}

static void
search_arg_analyze(struct mail_search_arg *arg, buffer_t *headers,
		   bool *have_body, bool *have_text)
{
	static const char *date_hdr = "Date";
	struct mail_search_arg *subarg;

	if (arg->result != -1)
		return;

	switch (arg->type) {
	case SEARCH_OR:
	case SEARCH_SUB:
		subarg = arg->value.subargs;
		while (subarg != NULL) {
			if (subarg->result == -1) {
				search_arg_analyze(subarg, headers,
						   have_body, have_text);
			}

			subarg = subarg->next;
		}
		break;
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE:
		buffer_append(headers, &date_hdr, sizeof(const char *));
		break;
	case SEARCH_HEADER:
	case SEARCH_HEADER_ADDRESS:
		buffer_append(headers, &arg->hdr_field_name,
			      sizeof(const char *));
		break;
	case SEARCH_BODY:
		*have_body = TRUE;
		break;
	case SEARCH_TEXT:
		*have_text = TRUE;
		*have_body = TRUE;
		break;
	default:
		break;
	}
}

const char *const *
mail_search_args_analyze(struct mail_search_arg *args,
			 bool *have_headers, bool *have_body)
{
	const char *null = NULL;
	buffer_t *headers;
	bool have_text;

	*have_headers = *have_body = have_text = FALSE;

	headers = buffer_create_dynamic(pool_datastack_create(), 128);
	for (; args != NULL; args = args->next)
		search_arg_analyze(args, headers, have_body, &have_text);

	*have_headers = have_text || headers->used != 0;

	if (headers->used == 0 || have_text)
		return NULL;

	buffer_append(headers, &null, sizeof(const char *));
	return buffer_get_data(headers, NULL);
}
