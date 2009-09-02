#ifndef CONFIG_FILTER_H
#define CONFIG_FILTER_H

#include "network.h"

struct config_filter {
	const char *service;
	struct ip_addr local_net, remote_net;
	unsigned int local_bits, remote_bits;
};

struct config_filter_parser_list {
	struct config_filter filter;
	/* NULL-terminated array of parsers */
	struct config_module_parser *parsers;
};

struct config_filter_context *config_filter_init(pool_t pool);
void config_filter_deinit(struct config_filter_context **ctx);

/* Replace filter's parsers with given parser list. */
void config_filter_add_all(struct config_filter_context *ctx,
			   struct config_filter_parser_list *const *parsers);

/* Find the filter that best matches what we have. */
const struct config_filter_parser_list *
config_filter_find(struct config_filter_context *ctx,
		   const struct config_filter *filter);

/* Returns TRUE if filter matches mask. */
bool config_filter_match(const struct config_filter *mask,
			 const struct config_filter *filter);
/* Returns TRUE if two filters are fully equal. */
bool config_filters_equal(const struct config_filter *f1,
			  const struct config_filter *f2);

#endif
