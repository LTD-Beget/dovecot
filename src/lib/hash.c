/*
    Copyright (c) 2003 Timo Sirainen

    Permission is hereby granted, free of charge, to any person obtaining
    a copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/* @UNSAFE: whole file */

#include "lib.h"
#include "hash.h"
#include "primes.h"

#include <ctype.h>

#define HASH_TABLE_MIN_SIZE 109

struct hash_node {
	struct hash_node *next;
	void *key;
	void *value;
};

struct hash_table {
	pool_t table_pool, node_pool;

	int frozen;
	size_t initial_size, nodes_count, removed_count;

	size_t size;
	struct hash_node *nodes;
	struct hash_node *free_nodes;

	hash_callback_t hash_cb;
	hash_cmp_callback_t key_compare_cb;
};

static int hash_resize(struct hash_table *table, int grow);

static int foreach_stop;

static int direct_cmp(const void *p1, const void *p2)
{
	return p1 == p2 ? 0 : 1;
}

static unsigned int direct_hash(const void *p)
{
	/* NOTE: may truncate the value, but that doesn't matter. */
	return POINTER_CAST_TO(p, unsigned int);
}

struct hash_table *
hash_create(pool_t table_pool, pool_t node_pool, size_t initial_size,
	    hash_callback_t hash_cb, hash_cmp_callback_t key_compare_cb)
{
	struct hash_table *table;

	initial_size = 0;

	table = p_new(table_pool, struct hash_table, 1);
        table->table_pool = table_pool;
	table->node_pool = node_pool;
	table->initial_size =
		I_MAX(primes_closest(initial_size), HASH_TABLE_MIN_SIZE);

	table->hash_cb = hash_cb != NULL ? hash_cb : direct_hash;
	table->key_compare_cb = key_compare_cb == NULL ?
		direct_cmp : key_compare_cb;

	table->size = table->initial_size;
	table->nodes = p_new(table_pool, struct hash_node, table->size);
	return table;
}

static void free_node(struct hash_table *table, struct hash_node *node)
{
	if (!table->node_pool->alloconly_pool)
		p_free(table->node_pool, node);
	else {
		node->next = table->free_nodes;
		table->free_nodes = node;
	}
}

static void destroy_node_list(struct hash_table *table, struct hash_node *node)
{
	struct hash_node *next;

	while (node != NULL) {
		next = node->next;
		p_free(table->node_pool, node);
		node = next;
	}
}

static void hash_destroy_nodes(struct hash_table *table)
{
	size_t i;

	for (i = 0; i < table->size; i++) {
		if (table->nodes[i].next != NULL)
			destroy_node_list(table, table->nodes[i].next);
	}
}

void hash_destroy(struct hash_table *table)
{
	if (!table->node_pool->alloconly_pool) {
		hash_destroy_nodes(table);
		destroy_node_list(table, table->free_nodes);
	}

	p_free(table->table_pool, table->nodes);
	p_free(table->table_pool, table);
}

void hash_clear(struct hash_table *table, int free_nodes)
{
	if (!table->node_pool->alloconly_pool)
		hash_destroy_nodes(table);

	if (free_nodes) {
		if (!table->node_pool->alloconly_pool)
			destroy_node_list(table, table->free_nodes);
                table->free_nodes = NULL;
	}

	memset(table->nodes, 0, sizeof(struct hash_node) * table->size);

	table->nodes_count = 0;
	table->removed_count = 0;
}

static struct hash_node *
hash_lookup_node(struct hash_table *table, const void *key, unsigned int hash)
{
	struct hash_node *node;

	node = &table->nodes[hash % table->size];

	do {
		if (node->key != NULL) {
			if (table->key_compare_cb(node->key, key) == 0)
				return node;
		}
		node = node->next;
	} while (node != NULL);

	return NULL;
}

void *hash_lookup(struct hash_table *table, const void *key)
{
	struct hash_node *node;

	node = hash_lookup_node(table, key, table->hash_cb(key));
	return node != NULL ? node->value : NULL;
}

int hash_lookup_full(struct hash_table *table, const void *lookup_key,
		     void **orig_key, void **value)
{
	struct hash_node *node;

	node = hash_lookup_node(table, lookup_key,
				table->hash_cb(lookup_key));
	if (node == NULL)
		return FALSE;

	if (orig_key != NULL)
		*orig_key = node->key;
	if (value != NULL)
		*value = node->value;
	return TRUE;
}

static struct hash_node *
hash_insert_node(struct hash_table *table, void *key, void *value,
		 int check_existing)
{
	struct hash_node *node, *prev;
	unsigned int hash;

	i_assert(key != NULL);

	hash = table->hash_cb(key);

	if (check_existing && table->removed_count > 0) {
		/* there may be holes, have to check everything */
		node = hash_lookup_node(table, key, hash);
		if (node != NULL) {
			node->value = value;
			return node;
		}

                check_existing = FALSE;
	}

	/* a) primary node */
	node = &table->nodes[hash % table->size];
	if (node->key == NULL) {
		table->nodes_count++;

		node->key = key;
		node->value = value;
		return node;
	}

	if (check_existing) {
		if (table->key_compare_cb(node->key, key) == 0) {
			node->value = value;
			return node;
		}
	}

	/* b) collisions list */
	prev = node; node = node->next;
	while (node != NULL) {
		if (node->key == NULL)
			break;

		if (check_existing) {
			if (table->key_compare_cb(node->key, key) == 0) {
				node->value = value;
				return node;
			}
		}

		prev = node;
		node = node->next;
	}

	if (node == NULL) {
		if (table->frozen == 0 && hash_resize(table, TRUE)) {
			/* resized table, try again */
			return hash_insert_node(table, key, value, FALSE);
		}

		if (table->free_nodes == NULL)
			node = p_new(table->node_pool, struct hash_node, 1);
		else {
			node = table->free_nodes;
			table->free_nodes = node->next;
			node->next = NULL;
		}
		prev->next = node;
	}

	node->key = key;
	node->value = value;;

	table->nodes_count++;
	return node;
}

void hash_insert(struct hash_table *table, void *key, void *value)
{
	struct hash_node *node;

	node = hash_insert_node(table, key, value, TRUE);
	node->key = key;
}

void hash_update(struct hash_table *table, void *key, void *value)
{
	(void)hash_insert_node(table, key, value, TRUE);
}

static void hash_compress(struct hash_table *table, struct hash_node *node)
{
	struct hash_node *next;

	/* remove deleted nodes from the list */
	while (node->next != NULL) {
		next = node->next;

		if (next->key == NULL) {
			node->next = next->next;
			free_node(table, next);
		} else {
			node = next;
		}
	}

	/* update root */
	if (node->key == NULL && node->next != NULL) {
		next = node->next;
		memcpy(node, next, sizeof(*node));
		free_node(table, next);
	}
}

static void hash_compress_removed(struct hash_table *table)
{
	size_t i;

	for (i = 0; i < table->size; i++)
		hash_compress(table, &table->nodes[i]);

        table->removed_count = 0;
}

void hash_remove(struct hash_table *table, const void *key)
{
	struct hash_node *node;
	unsigned int hash;

	hash = table->hash_cb(key);

	node = hash_lookup_node(table, key, hash);
	if (node == NULL)
		i_panic("key not found from hash");

	node->key = NULL;
	table->nodes_count--;

	if (table->frozen != 0)
		table->removed_count++;
	else if (!hash_resize(table, FALSE))
		hash_compress(table, &table->nodes[hash % table->size]);
}

size_t hash_size(struct hash_table *table)
{
	return table->nodes_count;
}

void hash_foreach(struct hash_table *table, hash_foreach_callback_t callback,
		  void *context)
{
	struct hash_node *node;
	size_t i;

	hash_freeze(table);

	foreach_stop = FALSE;

	for (i = 0; i < table->size; i++) {
		node = &table->nodes[i];

		do {
			if (node->key != NULL) {
				callback(node->key, node->value, context);
				if (foreach_stop) {
					table->frozen--;
					return;
				}
			}
			node = node->next;
		} while (node != NULL);
	}

	hash_thaw(table);
}

void hash_foreach_stop(void)
{
        foreach_stop = TRUE;
}

void hash_freeze(struct hash_table *table)
{
	table->frozen++;
}

void hash_thaw(struct hash_table *table)
{
	i_assert(table->frozen > 0);

	if (--table->frozen > 0)
		return;

	if (table->removed_count > 0) {
		if (!hash_resize(table, FALSE))
			hash_compress_removed(table);
	}
}

static int hash_resize(struct hash_table *table, int grow)
{
	struct hash_node *old_nodes, *node, *next;
	size_t next_size, old_size, i;
	float nodes_per_list;

        nodes_per_list = (float) table->nodes_count / (float) table->size;
	if (nodes_per_list > 0.3 && nodes_per_list < 2.0)
		return FALSE;

	next_size = I_MAX(primes_closest(table->nodes_count+1),
			  table->initial_size);
	if (next_size == table->size)
		return FALSE;

	if (grow && table->size >= next_size)
		return FALSE;

	/* recreate primary table */
	old_size = table->size;
	old_nodes = table->nodes;

	table->size = I_MAX(next_size, HASH_TABLE_MIN_SIZE);
	table->nodes = p_new(table->table_pool, struct hash_node, table->size);

	table->nodes_count = 0;
	table->removed_count = 0;

	table->frozen++;

	/* move the data */
	for (i = 0; i < old_size; i++) {
		node = &old_nodes[i];
		if (node->key != NULL)
			hash_insert_node(table, node->key, node->value, FALSE);

		for (node = node->next; node != NULL; node = next) {
			next = node->next;

			if (node->key != NULL) {
				hash_insert_node(table, node->key,
						 node->value, FALSE);
			}
			free_node(table, node);
		}
	}

	table->frozen--;

	p_free(table->table_pool, old_nodes);
	return TRUE;
}

/* a char* hash function from ASU -- from glib */
unsigned int str_hash(const void *p)
{
        const unsigned char *s = p;
	unsigned int g, h = 0;

	while (*s != '\0') {
		h = (h << 4) + *s;
		if ((g = h & 0xf0000000UL)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}
		s++;
	}

	return h;
}

/* a char* hash function from ASU -- from glib */
unsigned int strcase_hash(const void *p)
{
        const unsigned char *s = p;
	unsigned int g, h = 0;

	while (*s != '\0') {
		h = (h << 4) + i_toupper(*s);
		if ((g = h & 0xf0000000UL)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}
		s++;
	}

	return h;
}
