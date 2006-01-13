/* Copyright (C) 2004 Timo Sirainen */

#include "common.h"
#include "lib-signals.h"
#include "hash.h"
#include "str.h"
#include "strescape.h"
#include "var-expand.h"
#include "auth-request.h"
#include "auth-cache.h"

#include <time.h>

struct cache_node {
	struct cache_node *prev, *next;
	time_t created;
	uint32_t alloc_size;
	char data[4]; /* key \0 value \0 */
};

struct auth_cache {
	struct hash_table *hash;
	struct cache_node *head, *tail;

	size_t size_left;
	unsigned int ttl_secs;

	unsigned int hit_count, miss_count;
};

char *auth_cache_parse_key(pool_t pool, const char *query)
{
	string_t *str;
	char key_seen[256];
	uint8_t key;

	memset(key_seen, 0, sizeof(key_seen));

	str = str_new(pool, 32);
	for (; *query != '\0'; query++) {
		if (*query == '%' && query[1] != '\0') {
			query++;
                        key = var_get_key(query);
			if (key != '\0' && key != '%' && !key_seen[key]) {
				if (str_len(str) != 0)
					str_append_c(str, '\t');
				str_append_c(str, '%');
				str_append_c(str, key);

				/* @UNSAFE */
                                key_seen[key] = 1;
			}
		}
	}
	return str_free_without_data(str);
}

static void
auth_cache_node_unlink(struct auth_cache *cache, struct cache_node *node)
{
	if (node->prev != NULL)
		node->prev->next = node->next;
	else {
		/* unlinking tail */
		cache->tail = node->next;
	}

	if (node->next != NULL)
		node->next->prev = node->prev;
	else {
		/* unlinking head */
		cache->head = node->prev;
	}
}

static void
auth_cache_node_link_head(struct auth_cache *cache, struct cache_node *node)
{
	node->prev = cache->head;
	node->next = NULL;

	cache->head = node;
	if (node->prev != NULL)
		node->prev->next = node;
	else
		cache->tail = node;
}

static void
auth_cache_node_destroy(struct auth_cache *cache, struct cache_node *node)
{
	auth_cache_node_unlink(cache, node);

	cache->size_left += node->alloc_size;
	hash_remove(cache->hash, node->data);
	i_free(node);
}

static void sig_auth_cache_clear(int signo __attr_unused__, void *context)
{
	struct auth_cache *cache = context;

	i_info("SIGHUP received, clearing cache");
	auth_cache_clear(cache);
}

static void sig_auth_cache_stats(int signo __attr_unused__, void *context)
{
	struct auth_cache *cache = context;
	unsigned int total_count;

	total_count = cache->hit_count + cache->miss_count;
	i_info("Authentication cache hits %u/%u (%u%%)",
	       cache->hit_count, total_count,
	       total_count == 0 ? 100 : (cache->hit_count * 100 / total_count));

	/* reset hit counter */
	cache->hit_count = cache->miss_count = 0;
}

struct auth_cache *auth_cache_new(size_t max_size, unsigned int ttl_secs)
{
	struct auth_cache *cache;

	cache = i_new(struct auth_cache, 1);
	cache->hash = hash_create(default_pool, default_pool, 0, str_hash,
				  (hash_cmp_callback_t *)strcmp);
	cache->size_left = max_size;
	cache->ttl_secs = ttl_secs;

	lib_signals_set_handler(SIGHUP, TRUE, sig_auth_cache_clear, cache);
	lib_signals_set_handler(SIGUSR2, TRUE, sig_auth_cache_stats, cache);
	return cache;
}

void auth_cache_free(struct auth_cache *cache)
{
	lib_signals_unset_handler(SIGHUP, sig_auth_cache_clear, cache);
	lib_signals_unset_handler(SIGUSR2, sig_auth_cache_stats, cache);

	auth_cache_clear(cache);
	hash_destroy(cache->hash);
	i_free(cache);
}

void auth_cache_clear(struct auth_cache *cache)
{
	while (cache->tail != NULL)
		auth_cache_node_destroy(cache, cache->tail);
	hash_clear(cache->hash, FALSE);
}

const char *auth_cache_lookup(struct auth_cache *cache,
			      const struct auth_request *request,
			      const char *key, bool *expired_r)
{
	string_t *str;
	struct cache_node *node;

	*expired_r = FALSE;

	str = t_str_new(256);
	var_expand(str, key,
		   auth_request_get_var_expand_table(request, str_escape));

	node = hash_lookup(cache->hash, str_c(str));
	if (node == NULL) {
		cache->miss_count++;
		return NULL;
	}
	cache->hit_count++;

	if (node->created < time(NULL) - (time_t)cache->ttl_secs) {
		/* TTL expired */
		*expired_r = TRUE;
	} else {
		/* move to head */
		if (node != cache->head) {
			auth_cache_node_unlink(cache, node);
			auth_cache_node_link_head(cache, node);
		}
	}

	return node->data + strlen(node->data) + 1;
}

void auth_cache_insert(struct auth_cache *cache,
		       const struct auth_request *request,
		       const char *key, const char *value)
{
	string_t *str;
        struct cache_node *node;
	size_t data_size, alloc_size, value_len = strlen(value);

	str = t_str_new(256);
	var_expand(str, key,
		   auth_request_get_var_expand_table(request, str_escape));

	data_size = str_len(str) + 1 + value_len + 1;
	alloc_size = sizeof(struct cache_node) - sizeof(node->data) + data_size;

	/* make sure we have enough space */
	while (cache->size_left < alloc_size)
		auth_cache_node_destroy(cache, cache->tail);

	node = hash_lookup(cache->hash, str_c(str));
	if (node != NULL) {
		/* key is already in cache (probably expired), remove it */
		auth_cache_node_destroy(cache, node);
	}

	/* @UNSAFE */
	node = i_malloc(alloc_size);
	node->created = time(NULL);
	node->alloc_size = alloc_size;
	memcpy(node->data, str_data(str), str_len(str));
	memcpy(node->data + str_len(str) + 1, value, value_len);

	auth_cache_node_link_head(cache, node);

	cache->size_left -= alloc_size;
	hash_insert(cache->hash, node->data, node);
}
