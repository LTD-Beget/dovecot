/* Copyright (c) 2015 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "mail-namespace.h"
#include "mail-search.h"
#include "fts-api-private.h"
#include "fts-tokenizer.h"
#include "fts-filter.h"
#include "fts-user.h"
#include "fts-search-args.h"

static void strings_deduplicate(ARRAY_TYPE(const_string) *arr)
{
	const char *const *strings;
	unsigned int i, count;

	strings = array_get(arr, &count);
	for (i = 1; i < count; ) {
		if (strcmp(strings[i-1], strings[i]) == 0) {
			array_delete(arr, i, 1);
			strings = array_get(arr, &count);
		} else {
			i++;
		}
	}
}

static struct mail_search_arg *
fts_search_arg_create_or(const struct mail_search_arg *orig_arg, pool_t pool,
			 const ARRAY_TYPE(const_string) *tokens)
{
	struct mail_search_arg *arg, *or_arg, **argp;
	const char *const *tokenp;

	/* create the OR arg first as the parent */
	or_arg = p_new(pool, struct mail_search_arg, 1);
	or_arg->type = SEARCH_OR;
	or_arg->next = orig_arg->next;

	/* now create all the child args for the OR */
	argp = &or_arg->value.subargs;
	array_foreach(tokens, tokenp) {
		arg = p_new(pool, struct mail_search_arg, 1);
		*arg = *orig_arg;
		arg->match_not = FALSE; /* we copied this to the parent SUB */
		arg->next = NULL;
		arg->value.str = p_strdup(pool, *tokenp);

		*argp = arg;
		argp = &arg->next;
	}
	return or_arg;
}

static void
fts_backend_dovecot_expand_lang_tokens(const ARRAY_TYPE(fts_user_language) *languages,
				       pool_t pool,
				       struct mail_search_arg *parent_arg,
				       const struct mail_search_arg *orig_arg,
				       const char *orig_token, const char *token)
{
	struct mail_search_arg *arg;
	struct fts_user_language *const *langp;
	ARRAY_TYPE(const_string) tokens;
	const char *token2;
	int ret;

	t_array_init(&tokens, 4);
	/* first add the word exactly as it without any tokenization */
	array_append(&tokens, &orig_token, 1);
	/* then add it tokenized, but without filtering */
	array_append(&tokens, &token, 1);

	/* add the word filtered */
	array_foreach(languages, langp) {
		token2 = t_strdup(token);
		if ((*langp)->filter != NULL)
			ret = fts_filter_filter((*langp)->filter, &token2);
		if (ret > 0) {
			token2 = t_strdup(token2);
			array_append(&tokens, &token2, 1);
		}
	}
	array_sort(&tokens, i_strcmp_p);
	strings_deduplicate(&tokens);

	arg = fts_search_arg_create_or(orig_arg, pool, &tokens);
	arg->next = parent_arg->value.subargs;
	parent_arg->value.subargs = arg;
}

static void fts_search_arg_expand(struct fts_backend *backend, pool_t pool,
				  struct mail_search_arg **argp)
{
	const ARRAY_TYPE(fts_user_language) *languages;
	struct mail_search_arg *and_arg, *orig_arg = *argp;
	const char *token, *orig_token = orig_arg->value.str;
	unsigned int orig_token_len = strlen(orig_token);
	struct fts_tokenizer *tokenizer;

	languages = fts_user_get_all_languages(backend->ns->user);
	tokenizer = fts_user_get_search_tokenizer(backend->ns->user);

	/* we want all the tokens found from the string to be found, so create
	   a parent AND and place all the filtered token alternatives under
	   it */
	and_arg = p_new(pool, struct mail_search_arg, 1);
	and_arg->type = SEARCH_SUB;
	and_arg->match_not = orig_arg->match_not;
	and_arg->next = orig_arg->next;
	*argp = and_arg;

	while (fts_tokenizer_next(tokenizer,
	                          (const void *)orig_token,
	                          orig_token_len, &token) > 0) {
		fts_backend_dovecot_expand_lang_tokens(languages, pool, and_arg,
						       orig_arg, orig_token,
						       token);
	}
	while (fts_tokenizer_next(tokenizer, NULL, 0, &token) > 0) {
		fts_backend_dovecot_expand_lang_tokens(languages, pool, and_arg,
		                                       orig_arg, orig_token,
		                                       token);
	}
}

static void
fts_search_args_expand_tree(struct fts_backend *backend, pool_t pool,
			    struct mail_search_arg **argp)
{
	for (; *argp != NULL; argp = &(*argp)->next) {
		switch ((*argp)->type) {
		case SEARCH_OR:
		case SEARCH_SUB:
		case SEARCH_INTHREAD:
			fts_search_args_expand_tree(backend, pool,
						    &(*argp)->value.subargs);
			break;
		case SEARCH_HEADER:
		case SEARCH_HEADER_ADDRESS:
		case SEARCH_HEADER_COMPRESS_LWSP:
		case SEARCH_BODY:
		case SEARCH_TEXT:
			T_BEGIN {
				fts_search_arg_expand(backend, pool, argp);
			} T_END;
			break;
		default:
			break;
		}
	}
}

int fts_search_args_expand(struct fts_backend *backend,
			   struct mail_search_args *args)
{
	fts_search_args_expand_tree(backend, args->pool, &args->args);

	/* we'll need to re-simplify the args if we changed anything */
	args->simplified = FALSE;
	mail_search_args_simplify(args);
	return 0;
}