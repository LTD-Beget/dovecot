/* Copyright (C) 2004 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "sql-api-private.h"

#ifdef HAVE_PGSQL
#include <stdlib.h>
#include <time.h>
#include <libpq-fe.h>

struct pgsql_db {
	struct sql_db api;

	pool_t pool;
	const char *connect_string;
	PGconn *pg;

	struct io *io;
	enum io_condition io_dir;

	struct pgsql_queue *queue, **queue_tail;
	struct timeout *queue_to;

	time_t last_connect;
	unsigned int connecting:1;
	unsigned int connected:1;
	unsigned int querying:1;
};

struct pgsql_result {
	struct sql_result api;
	PGresult *pgres;

	unsigned int rownum, rows;
	unsigned int fields_count;
	const char **fields;
	const char **values;

	sql_query_callback_t *callback;
	void *context;
};

struct pgsql_queue {
	struct pgsql_queue *next;

	time_t created;
	char *query;
	struct pgsql_result *result;
};

extern struct sql_result driver_pgsql_result;

static void queue_send_next(struct pgsql_db *db);

static void driver_pgsql_close(struct pgsql_db *db)
{
	if (db->io != NULL) {
		io_remove(db->io);
		db->io = NULL;
	}
	db->io_dir = 0;

	PQfinish(db->pg);
	db->pg = NULL;

	db->connecting = FALSE;
	db->connected = FALSE;
        db->querying = FALSE;
}

static const char *last_error(struct pgsql_db *db)
{
	const char *msg;
	size_t len;

	msg = PQerrorMessage(db->pg);
	if (msg == NULL)
		return "(no error set)";

	/* Error message should contain trailing \n, we don't want it */
	len = strlen(msg);
	return len == 0 || msg[len-1] != '\n' ? msg :
		t_strndup(msg, len-1);
}

static void connect_callback(void *context)
{
	struct pgsql_db *db = context;
	enum io_condition io_dir = 0;
	int ret;

	while ((ret = PQconnectPoll(db->pg)) == PGRES_POLLING_ACTIVE)
		;

	switch (ret) {
	case PGRES_POLLING_READING:
		io_dir = IO_READ;
		break;
	case PGRES_POLLING_WRITING:
		io_dir = IO_WRITE;
		break;
	case PGRES_POLLING_OK:
		i_info("pgsql: Connected to %s", PQdb(db->pg));
		db->connecting = FALSE;
		db->connected = TRUE;
		break;
	case PGRES_POLLING_FAILED:
		i_error("pgsql: Connect failed to %s: %s",
			PQdb(db->pg), last_error(db));
		driver_pgsql_close(db);
		return;
	}

	if (db->io_dir != io_dir) {
		if (db->io != NULL)
			io_remove(db->io);
		db->io = io_dir == 0 ? NULL :
			io_add(PQsocket(db->pg), io_dir, connect_callback, db);
		db->io_dir = io_dir;
	}
}

static void driver_pgsql_connect(struct pgsql_db *db)
{
	time_t now;

	/* don't try reconnecting more than once a second */
	now = time(NULL);
	if (db->connecting || db->last_connect == now)
		return;
	db->last_connect = now;

	db->pg = PQconnectStart(db->connect_string);
	if (db->pg == NULL)
		i_fatal("pgsql: PQconnectStart() failed (out of memory)");

	if (PQstatus(db->pg) == CONNECTION_BAD) {
		i_error("pgsql: Connect failed to %s: %s",
			PQdb(db->pg), last_error(db));
		driver_pgsql_close(db);
	} else {
		/* nonblocking connecting begins. */
		db->io = io_add(PQsocket(db->pg), IO_WRITE,
				connect_callback, db);
		db->io_dir = IO_WRITE;
		db->connecting = TRUE;
	}
}

static struct sql_db *driver_pgsql_init(const char *connect_string)
{
	struct pgsql_db *db;

	db = i_new(struct pgsql_db, 1);
	db->connect_string = i_strdup(connect_string);
	db->api = driver_pgsql_db;
	db->queue_tail = &db->queue;

	(void)driver_pgsql_connect(db);
	return &db->api;
}

static void driver_pgsql_deinit(struct sql_db *_db)
{
	struct pgsql_db *db = (struct pgsql_db *)_db;

        driver_pgsql_close(db);
	i_free(db);
}

static void consume_results(void *context)
{
	struct pgsql_db *db = context;

	do {
		if (!PQconsumeInput(db->pg)) {
			db->connected = FALSE;
			break;

		}
		if (PQisBusy(db->pg))
			return;

	} while (PQgetResult(db->pg) != NULL);

	io_remove(db->io);
	db->io = NULL;

	db->querying = FALSE;
	if (db->queue != NULL && db->connected)
		queue_send_next(db);
}

static void result_finish(struct pgsql_result *result)
{
	struct pgsql_db *db = (struct pgsql_db *)result->api.db;

	if (result->callback != NULL)
		result->callback(&result->api, result->context);
	if (result->pgres != NULL) {
		PQclear(result->pgres);

		/* we'll have to read the rest of the results as well */
		i_assert(db->io == NULL);
		db->io = io_add(PQsocket(db->pg), IO_READ,
				consume_results, db);
		consume_results(db);
	} else {
		db->querying = FALSE;
	}

	i_free(result->fields);
	i_free(result->values);
	i_free(result);

	if (db->queue != NULL && !db->querying && db->connected)
		queue_send_next(db);
}

static void get_result(void *context)
{
        struct pgsql_result *result = context;
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;

	if (!PQconsumeInput(db->pg)) {
		db->connected = FALSE;
		result_finish(result);
		return;
	}

	if (PQisBusy(db->pg)) {
		if (db->io == NULL) {
 			db->io = io_add(PQsocket(db->pg), IO_READ,
					get_result, result);
		}
		return;
	}

	if (db->io != NULL) {
		io_remove(db->io);
		db->io = NULL;
	}

	result->pgres = PQgetResult(db->pg);
	result_finish(result);
}

static void flush_callback(void *context)
{
	struct pgsql_result *result = context;
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;
	int ret;

	ret = PQflush(db->pg);
	if (ret > 0)
		return;

	io_remove(db->io);
        db->io = NULL;

	if (ret < 0) {
		db->connected = FALSE;
		result_finish(result);
	} else {
		/* all flushed */
		get_result(result);
	}
}

static void send_query(struct pgsql_result *result, const char *query)
{
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;
	int ret;

	i_assert(db->io == NULL);
	i_assert(!db->querying);
	i_assert(db->connected);

	if (!PQsendQuery(db->pg, query)) {
		db->connected = FALSE;
		result_finish(result);
		return;
	}

	ret = PQflush(db->pg);
	if (ret < 0) {
		db->connected = FALSE;
		result_finish(result);
		return;
	}

	db->querying = TRUE;
	if (ret > 0) {
		/* write blocks */
		db->io = io_add(PQsocket(db->pg), IO_WRITE,
				flush_callback, result);
	} else {
		get_result(result);
	}
}

static void queue_send_next(struct pgsql_db *db)
{
	struct pgsql_queue *queue;

	queue = db->queue;
	db->queue = queue->next;

	send_query(queue->result, queue->query);

	i_free(queue->query);
	i_free(queue);
}

static void queue_timeout(void *context)
{
	struct pgsql_db *db = context;

	if (db->querying)
		return;

	if (!db->connected) {
		driver_pgsql_connect(db);
		return;
	}

	if (db->queue != NULL)
		queue_send_next(db);

	if (db->queue == NULL) {
		timeout_remove(db->queue_to);
                db->queue_to = NULL;
	}
}

static void
driver_pgsql_queue_query(struct pgsql_result *result, const char *query)
{
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;
	struct pgsql_queue *queue;

	queue = i_new(struct pgsql_queue, 1);
	queue->created = time(NULL);
	queue->query = i_strdup(query);
	queue->result = result;

	*db->queue_tail = queue;

	if (db->queue_to == NULL)
		db->queue_to = timeout_add(5000, queue_timeout, db);
}

static void do_query(struct pgsql_result *result, const char *query)
{
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;

	if (db->querying) {
		/* only one query at a time */
		driver_pgsql_queue_query(result, query);
		return;
	}

	if (!db->connected) {
		/* try connecting again */
		driver_pgsql_connect(db);
		driver_pgsql_queue_query(result, query);
		return;
	}

	if (db->queue == NULL)
		send_query(result, query);
	else {
		/* there's already queries queued, send them first */
		driver_pgsql_queue_query(result, query);
		queue_send_next(db);
	}
}

static void exec_callback(struct sql_result *result,
			  void *context __attr_unused__)
{
        struct pgsql_db *db = (struct pgsql_db *)result->db;

	i_error("pgsql: sql_exec() failed: %s", last_error(db));
}

static void driver_pgsql_exec(struct sql_db *db, const char *query)
{
	struct pgsql_result *result;

	result = i_new(struct pgsql_result, 1);
	result->api = driver_pgsql_result;
	result->api.db = db;
	result->callback = exec_callback;

	do_query(result, query);
}

static void driver_pgsql_query(struct sql_db *db, const char *query,
			       sql_query_callback_t *callback, void *context)
{
	struct pgsql_result *result;

	result = i_new(struct pgsql_result, 1);
	result->api = driver_pgsql_result;
	result->api.db = db;
	result->callback = callback;
	result->context = context;

	do_query(result, query);
}

static int driver_pgsql_result_next_row(struct sql_result *_result)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	struct pgsql_db *db = (struct pgsql_db *)_result->db;

	if (result->rows != 0) {
		/* second time we're here */
		return ++result->rownum < result->rows;
	}

	if (result->pgres == NULL)
		return -1;

	switch (PQresultStatus(result->pgres)) {
	case PGRES_COMMAND_OK:
		/* no rows returned */
		return 0;
	case PGRES_TUPLES_OK:
		result->rows = PQntuples(result->pgres);
		return result->rows > 0;
	case PGRES_EMPTY_QUERY:
	case PGRES_NONFATAL_ERROR:
		/* nonfatal error */
		return -1;
	default:
		/* treat as fatal error */
		db->connected = FALSE;
		return -1;
	}
}

static void driver_pgsql_result_fetch_fields(struct pgsql_result *result)
{
	unsigned int i;

	if (result->fields != NULL)
		return;

	/* @UNSAFE */
	result->fields_count = PQnfields(result->pgres);
	result->fields = i_new(const char *, result->fields_count);
	for (i = 0; i < result->fields_count; i++)
		result->fields[i] = PQfname(result->pgres, i);
}

static unsigned int
driver_pgsql_result_get_fields_count(struct sql_result *_result)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;

        driver_pgsql_result_fetch_fields(result);
	return result->fields_count;
}

static const char *
driver_pgsql_result_get_field_name(struct sql_result *_result, unsigned int idx)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;

	driver_pgsql_result_fetch_fields(result);
	i_assert(idx < result->fields_count);
	return result->fields[idx];
}

static int driver_pgsql_result_find_field(struct sql_result *_result,
					  const char *field_name)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	unsigned int i;

	driver_pgsql_result_fetch_fields(result);
	for (i = 0; i < result->fields_count; i++) {
		if (strcmp(result->fields[i], field_name) == 0)
			return i;
	}
	return -1;
}

static const char *
driver_pgsql_result_get_field_value(struct sql_result *_result,
				    unsigned int idx)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;

	if (PQgetisnull(result->pgres, result->rownum, idx))
		return NULL;

	return PQgetvalue(result->pgres, result->rownum, idx);
}

static const char *
driver_pgsql_result_find_field_value(struct sql_result *result,
				     const char *field_name)
{
	int idx;

	idx = driver_pgsql_result_find_field(result, field_name);
	if (idx < 0)
		return NULL;
	return driver_pgsql_result_get_field_value(result, idx);
}

static const char *const *
driver_pgsql_result_get_values(struct sql_result *_result)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	unsigned int i;

	if (result->values == NULL) {
		driver_pgsql_result_fetch_fields(result);
		result->values = i_new(const char *, result->fields_count);
	}

	/* @UNSAFE */
	for (i = 0; i < result->fields_count; i++) {
		result->values[i] =
                        driver_pgsql_result_get_field_value(_result, i);
	}

	return result->values;
}

static const char *driver_pgsql_result_get_error(struct sql_result *_result)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	const char *msg;
	size_t len;

	msg = PQresultErrorMessage(result->pgres);
	if (msg == NULL)
		return "(no error set)";

	/* Error message should contain trailing \n, we don't want it */
	len = strlen(msg);
	return len == 0 || msg[len-1] != '\n' ? msg :
		t_strndup(msg, len-1);
}

struct sql_db driver_pgsql_db = {
	driver_pgsql_init,
	driver_pgsql_deinit,
	driver_pgsql_exec,
	driver_pgsql_query
};

struct sql_result driver_pgsql_result = {
	NULL,

	driver_pgsql_result_next_row,
	driver_pgsql_result_get_fields_count,
	driver_pgsql_result_get_field_name,
	driver_pgsql_result_find_field,
	driver_pgsql_result_get_field_value,
	driver_pgsql_result_find_field_value,
	driver_pgsql_result_get_values,
	driver_pgsql_result_get_error
};

#endif
