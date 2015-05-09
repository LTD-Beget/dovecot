#ifndef FTS_USER_H
#define FTS_USER_H

struct fts_user_language {
	const struct fts_language *lang;
	struct fts_filter *filter;
};
ARRAY_DEFINE_TYPE(fts_user_language, struct fts_user_language *);

struct fts_user_language *
fts_user_language_find(struct mail_user *user,
                       const struct fts_language *lang);
struct fts_tokenizer *fts_user_get_index_tokenizer(struct mail_user *user);
struct fts_tokenizer *fts_user_get_search_tokenizer(struct mail_user *user);
struct fts_language_list *fts_user_get_language_list(struct mail_user *user);
const ARRAY_TYPE(fts_user_language) *
fts_user_get_all_languages(struct mail_user *user);

int fts_mail_user_init(struct mail_user *user, const char **error_r);
void fts_mail_user_deinit(struct mail_user *user);

#endif