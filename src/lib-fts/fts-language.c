/* Copyright (c) 2014-2015 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "fts-language.h"
#include "strfuncs.h"
#include "llist.h"

#ifdef HAVE_LIBEXTTEXTCAT_TEXTCAT_H
#  include <libexttextcat/textcat.h>
#  define HAVE_TEXTCAT
#elif defined (HAVE_LIBTEXTCAT_TEXTCAT_H)
#  include <libtextcat/textcat.h>
#  define HAVE_TEXTCAT
#elif defined (HAVE_FTS_TEXTCAT)
#  include <textcat.h>
#  define HAVE_TEXTCAT
#endif

#ifndef TEXTCAT_RESULT_UNKNOWN /* old textcat.h has typos */
#  ifdef TEXTCAT_RESULT_UNKOWN
#    define TEXTCAT_RESULT_UNKNOWN TEXTCAT_RESULT_UNKOWN
#  endif
#endif

#define DETECT_STR_MAX_LEN 200

struct fts_language_list {
	pool_t pool;
	ARRAY_TYPE(fts_language) languages;
	const char *textcat_config;
	const char *textcat_datadir;
	void *textcat_handle;
	bool textcat_failed;
};

const struct fts_language fts_languages[] = {
	{ "en" },
	{ "fi" },
	{ "fr" },
	{ "de" }
};

const struct fts_language fts_language_data = {
	"data"
};

const struct fts_language *fts_language_find(const char *name)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS(fts_languages); i++) {
		if (strcmp(fts_languages[i].name, name) == 0)
			return &fts_languages[i];
	}
	return NULL;
}

struct fts_language_list *
fts_language_list_init(const char *const *settings)
{
	struct fts_language_list *lp;
	pool_t pool;
	unsigned int i;
	const char *conf = NULL;
	const char *data = NULL;

	for (i = 0; settings[i] != NULL; i += 2) {
		const char *key = settings[i], *value = settings[i+1];

		if (strcmp(key, "fts_language_config") == 0) {
			conf = value;
		}
		else if (strcmp(key, "fts_language_data") == 0) {
			data = value;
		} else {
			i_debug("Unknown setting: %s", key);
			return NULL;
		}
	}

	pool = pool_alloconly_create("fts_language_list", 128);
	lp = p_new(pool, struct fts_language_list, 1);
	lp->pool = pool;
	if (conf != NULL)
		lp->textcat_config = p_strdup(pool, conf);
	else
		lp->textcat_config = NULL;
	if (data != NULL)
		lp->textcat_datadir = p_strdup(pool, data);
	else
		lp->textcat_datadir = NULL;
	p_array_init(&lp->languages, pool, 32);
	return lp;
}

void fts_language_list_deinit(struct fts_language_list **list)
{
	struct fts_language_list *lp = *list;

	*list = NULL;
#ifdef HAVE_TEXTCAT
	if (lp->textcat_handle != NULL)
		textcat_Done(lp->textcat_handle);
#endif
	pool_unref(&lp->pool);
}

static const struct fts_language *
fts_language_list_find(struct fts_language_list *list, const char *name)
{
	const struct fts_language *const *langp;

	array_foreach(&list->languages, langp) {
		if (strcmp((*langp)->name, name) == 0)
			return *langp;
	}
	return NULL;
}

void fts_language_list_add(struct fts_language_list *list,
			   const struct fts_language *lang)
{
	i_assert(fts_language_list_find(list, lang->name) == NULL);
	array_append(&list->languages, &lang, 1);
}

bool fts_language_list_add_names(struct fts_language_list *list,
				 const char *names,
				 const char **unknown_name_r)
{
	const char *const *langs;
	const struct fts_language *lang;

	for (langs = t_strsplit_spaces(names, ", "); *langs != NULL; langs++) {
		lang = fts_language_find(*langs);
		if (lang == NULL) {
			/* unknown language */
			*unknown_name_r = *langs;
			return FALSE;
		}
		if (fts_language_list_find(list, lang->name) == NULL)
			fts_language_list_add(list, lang);
	}
	return TRUE;
}

const ARRAY_TYPE(fts_language) *
fts_language_list_get_all(struct fts_language_list *list)
{
	return &list->languages;
}

const struct fts_language *
fts_language_list_get_first(struct fts_language_list *list)
{
	const struct fts_language *const *langp;

	langp = array_idx(&list->languages, 0);
	return *langp;
}

#ifdef HAVE_TEXTCAT
static bool fts_language_match_lists(struct fts_language_list *list,
                                     candidate_t *candp, int candp_len,
                                     const struct fts_language **lang_r)
{
	const char *name;

	for (int i = 0; i < candp_len; i++) {
		/* name is <lang>-<optional country or characterset>-<encoding> 
		   eg, fi--utf8 or pt-PT-utf8 */
		name = t_strcut(candp[i].name, '-');
		if ((*lang_r = fts_language_list_find(list, name)) != NULL)
			return TRUE;
	}
	return FALSE;
}
#endif

#ifdef HAVE_TEXTCAT
static int fts_language_textcat_init(struct fts_language_list *list)
{
	const char *config_path;
	const char *data_dir;

	if (list->textcat_handle != NULL)
		return 0;

	if (list->textcat_failed)
		return -1;
	    
	config_path = list->textcat_config != NULL ? list->textcat_config :
		DATADIR"/libexttextcat/fpdb.conf";
	data_dir = list->textcat_datadir != NULL ? list->textcat_datadir :
		DATADIR"/libexttextcat/";
	list->textcat_handle = special_textcat_Init(config_path, data_dir);
	if (list->textcat_handle == NULL) {
		i_error("special_textcat_Init(%s, %s) failed",
			config_path, data_dir);
		list->textcat_failed = TRUE;
		return -1;
	}
	/* The textcat minimum document size could be set here. It
	   currently defaults to 3. UTF8 is enabled by default. */
	return 0;
}
#endif

static enum fts_language_result
fts_language_detect_textcat(struct fts_language_list *list ATTR_UNUSED,
			    const unsigned char *text ATTR_UNUSED,
			    size_t size ATTR_UNUSED,
			    const struct fts_language **lang_r ATTR_UNUSED)
{
#ifdef HAVE_TEXTCAT
	candidate_t *candp; /* textcat candidate result array pointer */
	int cnt;
	bool match = FALSE;

	if (fts_language_textcat_init(list) < 0)
		return FTS_LANGUAGE_RESULT_ERROR;

	candp = textcat_GetClassifyFullOutput(list->textcat_handle);
	if (candp == NULL)
		i_fatal_status(FATAL_OUTOFMEM, "textcat_GetCLassifyFullOutput failed: malloc() returned NULL");
	cnt = textcat_ClassifyFull(list->textcat_handle, (const void *)text,
				   I_MIN(size, DETECT_STR_MAX_LEN), candp);
	if (cnt > 0) {
		T_BEGIN {
			match = fts_language_match_lists(list, candp, cnt, lang_r);
		} T_END;
		textcat_ReleaseClassifyFullOutput(list->textcat_handle, candp);
		if (match)
			return FTS_LANGUAGE_RESULT_OK;
		else
			return FTS_LANGUAGE_RESULT_UNKNOWN;
	} else {
		textcat_ReleaseClassifyFullOutput(list->textcat_handle, candp);
		switch (cnt) {
		case TEXTCAT_RESULT_SHORT:
			i_assert(size < DETECT_STR_MAX_LEN);
			return FTS_LANGUAGE_RESULT_SHORT;
		case TEXTCAT_RESULT_UNKNOWN:
			return FTS_LANGUAGE_RESULT_UNKNOWN;
		default:
			i_unreached();
		}
	}
#else
	return FTS_LANGUAGE_RESULT_UNKNOWN;
#endif
}

enum fts_language_result
fts_language_detect(struct fts_language_list *list,
		    const unsigned char *text ATTR_UNUSED,
		    size_t size ATTR_UNUSED,
                    const struct fts_language **lang_r)
{
	i_assert(array_count(&list->languages) > 0);

	/* if there's only a single wanted language, return it always. */
	if (array_count(&list->languages) == 1) {
		const struct fts_language *const *langp =
			array_idx(&list->languages, 0);
		*lang_r = *langp;
		return FTS_LANGUAGE_RESULT_OK;
	}
	return fts_language_detect_textcat(list, text, size, lang_r);
}
