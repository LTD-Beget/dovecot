/* Copyright (C) 2002-2004 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "str.h"
#include "strescape.h"
#include "istream.h"
#include "ostream.h"
#include "istream-header-filter.h"
#include "message-parser.h"
#include "message-send.h"
#include "mail-storage.h"
#include "imap-parser.h"
#include "imap-fetch.h"

#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

struct imap_fetch_body_data {
	struct imap_fetch_body_data *next;

        struct mailbox_header_lookup_ctx *header_ctx;
	const char *section; /* NOTE: always uppercased */
	uoff_t skip, max_size; /* if you don't want max_size,
	                          set it to (uoff_t)-1 */

	const char *const *fields;
	size_t fields_count;

	unsigned int skip_set:1;
	unsigned int peek:1;
};

struct partial_cache {
	unsigned int select_counter;
	unsigned int uid;

	uoff_t physical_start;
	int cr_skipped;
	struct message_size pos;
};

static struct partial_cache partial = { 0, 0, 0, 0, { 0, 0, 0 } };

static int seek_partial(unsigned int select_counter, unsigned int uid,
			struct partial_cache *partial, struct istream *stream,
			uoff_t virtual_skip)
{
	int cr_skipped;

	if (select_counter == partial->select_counter && uid == partial->uid &&
	    stream->v_offset == partial->physical_start &&
	    virtual_skip >= partial->pos.virtual_size) {
		/* we can use the cache */
		virtual_skip -= partial->pos.virtual_size;
	} else {
		partial->select_counter = select_counter;
		partial->uid = uid;
		partial->physical_start = stream->v_offset;
		partial->cr_skipped = FALSE;
		memset(&partial->pos, 0, sizeof(partial->pos));
	}

	i_stream_seek(stream, partial->physical_start +
		      partial->pos.physical_size);
	message_skip_virtual(stream, virtual_skip, &partial->pos,
			     partial->cr_skipped, &cr_skipped);

	partial->cr_skipped = FALSE;
	return cr_skipped;
}

static uoff_t get_send_size(const struct imap_fetch_body_data *body,
			    uoff_t max_size)
{
	uoff_t size;

	if (body->skip >= max_size)
		return 0;

	size = max_size - body->skip;
	return size <= body->max_size ? size : body->max_size;
}

static string_t *get_prefix(struct imap_fetch_context *ctx,
			    const struct imap_fetch_body_data *body,
			    uoff_t size)
{
	string_t *str;

	str = t_str_new(128);
	if (ctx->first)
		ctx->first = FALSE;
	else
		str_append_c(str, ' ');

	str_printfa(str, "BODY[%s]", body->section);
	if (body->skip_set)
		str_printfa(str, "<%"PRIuUOFF_T">", body->skip);

	if (size != (uoff_t)-1)
		str_printfa(str, " {%"PRIuUOFF_T"}\r\n", size);
	else
		str_append(str, " NIL");
	return str;
}

static off_t imap_fetch_send(struct ostream *output, struct istream *input,
			     int cr_skipped, uoff_t virtual_size,
			     int add_missing_eoh, int *last_cr)
{
	const unsigned char *msg;
	size_t i, size;
	uoff_t vsize_left, sent;
	off_t ret;
	unsigned char add;
	int blocks = FALSE;

	/* go through the message data and insert CRs where needed.  */
	sent = 0; vsize_left = virtual_size;
	while (vsize_left > 0 && !blocks &&
	       i_stream_read_data(input, &msg, &size, 0) > 0) {
		add = '\0';
		for (i = 0; i < size && vsize_left > 0; i++) {
			vsize_left--;

			if (msg[i] == '\n') {
				if ((i > 0 && msg[i-1] != '\r') ||
				    (i == 0 && !cr_skipped)) {
					/* missing CR */
					add = '\r';
					break;
				}
			} else if (msg[i] == '\0') {
				add = 128;
				break;
			}
		}

		if ((ret = o_stream_send(output, msg, i)) < 0)
			return -1;
		if ((uoff_t)ret < i) {
			add = '\0';
			blocks = TRUE;
		}

		if (ret > 0)
			cr_skipped = msg[ret-1] == '\r';

		i_stream_skip(input, ret);
		sent += ret;

		if (add != '\0') {
			if ((ret = o_stream_send(output, &add, 1)) < 0)
				return -1;
			if (ret == 0)
				blocks = TRUE;
			else {
				sent++;
				cr_skipped = add == '\r';
				if (add == 128)
					i_stream_skip(input, 1);
			}
		}
	}

	if (add_missing_eoh && sent + 2 == virtual_size) {
		/* Netscape missing EOH workaround. */
		o_stream_set_max_buffer_size(output, (size_t)-1);
		if (o_stream_send(output, "\r\n", 2) < 0)
			return -1;
		sent += 2;
	}

	if ((uoff_t)sent != virtual_size && !blocks) {
		/* Input stream gave less data then we expected. Two choices
		   here: either we fill the missing data with spaces or we
		   disconnect the client.

		   We shouldn't really ever get here. One reason is if mail
		   was deleted from NFS server while we were reading it.
		   Another is some temporary disk error.

		   If we filled the missing data the client could cache it,
		   and if it was just a temporary error the message would be
		   permanently left corrupted in client's local cache. So, we
		   disconnect the client and hope that next try works. */
		o_stream_close(output);
		return -1;
	}

	*last_cr = cr_skipped;
	return sent;
}

static int fetch_stream_send(struct imap_fetch_context *ctx)
{
	off_t ret;

	o_stream_set_max_buffer_size(ctx->client->output, 4096);
	ret = imap_fetch_send(ctx->client->output, ctx->cur_input,
			      ctx->skip_cr, ctx->cur_size - ctx->cur_offset,
			      ctx->cur_append_eoh, &ctx->skip_cr);
	o_stream_set_max_buffer_size(ctx->client->output, (size_t)-1);

	if (ret < 0)
		return -1;

	ctx->cur_offset += ret;
	if (ctx->update_partial) {
		partial.cr_skipped = ctx->skip_cr != 0;
		partial.pos.physical_size =
			ctx->cur_input->v_offset - partial.physical_start;
		partial.pos.virtual_size += ret;
	}

	return ctx->cur_offset == ctx->cur_size;
}

static int fetch_stream_send_direct(struct imap_fetch_context *ctx)
{
	off_t ret;

	o_stream_set_max_buffer_size(ctx->client->output, 0);
	ret = o_stream_send_istream(ctx->client->output, ctx->cur_input);
	o_stream_set_max_buffer_size(ctx->client->output, (size_t)-1);

	if (ret < 0)
		return -1;

	ctx->cur_offset += ret;

	if (ctx->cur_append_eoh && ctx->cur_offset + 2 == ctx->cur_size) {
		/* Netscape missing EOH workaround. */
		if (o_stream_send(ctx->client->output, "\r\n", 2) < 0)
			return -1;
		ctx->cur_offset += 2;
		ctx->cur_append_eoh = FALSE;
	}

	if (ctx->cur_offset != ctx->cur_size && ret == 0 &&
	    ctx->cur_input->eof) {
		/* Input stream gave less data than expected */
		o_stream_close(ctx->client->output);
		return -1;
	}

	return ctx->cur_offset == ctx->cur_size;
}

static int fetch_stream(struct imap_fetch_context *ctx,
			const struct message_size *size)
{
	struct istream *input;

	if (size->physical_size == size->virtual_size &&
	    ctx->cur_mail->has_no_nuls) {
		/* no need to kludge with CRs, we can use sendfile() */
		input = i_stream_create_limit(default_pool, ctx->cur_input,
					      ctx->cur_input->v_offset,
					      ctx->cur_size);
		i_stream_unref(ctx->cur_input);
		ctx->cur_input = input;

		ctx->cont_handler = fetch_stream_send_direct;
	} else {
                ctx->cont_handler = fetch_stream_send;
	}

	return ctx->cont_handler(ctx);
}

static int fetch_data(struct imap_fetch_context *ctx,
		      const struct imap_fetch_body_data *body,
		      const struct message_size *size)
{
	string_t *str;

	ctx->cur_size = get_send_size(body, size->virtual_size);

	str = get_prefix(ctx, body, ctx->cur_size);
	if (o_stream_send(ctx->client->output,
			  str_data(str), str_len(str)) < 0)
		return -1;

	if (!ctx->update_partial) {
		message_skip_virtual(ctx->cur_input, body->skip, NULL, FALSE,
				     &ctx->skip_cr);
	} else {
		ctx->skip_cr =
			seek_partial(ctx->select_counter, ctx->cur_mail->uid,
				     &partial, ctx->cur_input, body->skip);
	}

	return fetch_stream(ctx, size);
}

static int fetch_body(struct imap_fetch_context *ctx, struct mail *mail,
		      void *context)
{
	const struct imap_fetch_body_data *body = context;
	const struct message_size *fetch_size;
	struct message_size hdr_size, body_size;

	ctx->cur_input =
		mail->get_stream(mail, &hdr_size,
				 body->section[0] == 'H' ? NULL : &body_size);
	if (ctx->cur_input == NULL)
		return -1;

	i_stream_ref(ctx->cur_input);
	ctx->update_partial = TRUE;

	switch (body->section[0]) {
	case '\0':
		/* BODY[] - fetch everything */
		message_size_add(&body_size, &hdr_size);
                fetch_size = &body_size;
		break;
	case 'H':
		/* BODY[HEADER] - fetch only header */
                fetch_size = &hdr_size;
		break;
	case 'T':
		/* BODY[TEXT] - skip header */
		i_stream_skip(ctx->cur_input, hdr_size.physical_size);
                fetch_size = &body_size;
		break;
	default:
		i_unreached();
	}

	return fetch_data(ctx, body, fetch_size);
}

static void header_filter_eoh(struct message_header_line *hdr,
			      int *matched __attr_unused__, void *context)
{
	struct imap_fetch_context *ctx = context;

	if (hdr != NULL && hdr->eoh)
		ctx->cur_have_eoh = TRUE;
}

static void header_filter_mime(struct message_header_line *hdr,
			       int *matched, void *context)
{
	struct imap_fetch_context *ctx = context;

	if (hdr == NULL)
		return;

	if (hdr->eoh) {
		ctx->cur_have_eoh = TRUE;
		return;
	}

	*matched = strncasecmp(hdr->name, "Content-", 8) == 0 ||
		strcasecmp(hdr->name, "Mime-Version") == 0;
}

static int fetch_header_partial_from(struct imap_fetch_context *ctx,
				     const struct imap_fetch_body_data *body,
				     const char *header_section)
{
	struct message_size msg_size;
	struct istream *input;

	/* MIME, HEADER.FIELDS (list), HEADER.FIELDS.NOT (list) */

	ctx->cur_have_eoh = FALSE;
	if (strncmp(header_section, "HEADER.FIELDS ", 14) == 0) {
		input = i_stream_create_header_filter(ctx->cur_input,
						      HEADER_FILTER_INCLUDE,
						      body->fields,
						      body->fields_count,
						      header_filter_eoh, ctx);
	} else if (strncmp(header_section, "HEADER.FIELDS.NOT ", 18) == 0) {
		input = i_stream_create_header_filter(ctx->cur_input,
						      HEADER_FILTER_EXCLUDE,
						      body->fields,
						      body->fields_count,
						      header_filter_eoh, ctx);
	} else if (strcmp(header_section, "MIME") == 0) {
		/* Mime-Version + Content-* fields */
		input = i_stream_create_header_filter(ctx->cur_input,
						      HEADER_FILTER_INCLUDE,
						      NULL, 0,
						      header_filter_mime, ctx);
	} else {
		i_error("BUG: Accepted invalid section from user: '%s'",
			header_section);
		return -1;
	}

	i_stream_unref(ctx->cur_input);
	ctx->cur_input = input;
	ctx->update_partial = FALSE;

	message_get_header_size(ctx->cur_input, &msg_size, NULL);
	i_stream_seek(ctx->cur_input, 0);

	if (!ctx->cur_have_eoh &&
	    (client_workarounds & WORKAROUND_NETSCAPE_EOH) != 0) {
		/* Netscape 4.x doesn't like if end of headers line is
		   missing. */
		msg_size.virtual_size += 2;
		ctx->cur_append_eoh = TRUE;
	}

	return fetch_data(ctx, body, &msg_size);
}

static int fetch_body_header_partial(struct imap_fetch_context *ctx,
				     struct mail *mail, void *context)
{
	const struct imap_fetch_body_data *body = context;

	ctx->cur_input = mail->get_stream(mail, NULL, NULL);
	if (ctx->cur_input == NULL)
		return -1;

	i_stream_ref(ctx->cur_input);
	ctx->update_partial = FALSE;

	return fetch_header_partial_from(ctx, body, body->section);
}

static int fetch_body_header_fields(struct imap_fetch_context *ctx,
				    struct mail *mail, void *context)
{
	const struct imap_fetch_body_data *body = context;
	struct message_size size;

	ctx->cur_input = mail->get_headers(mail, body->header_ctx);
	if (ctx->cur_input == NULL)
		return -1;

	i_stream_ref(ctx->cur_input);
	ctx->update_partial = FALSE;

	message_get_body_size(ctx->cur_input, &size, NULL);
	i_stream_seek(ctx->cur_input, 0);

	/* FIXME: We'll just always add the end of headers line now.
	   ideally mail-storage would have a way to tell us if it exists. */
	size.virtual_size += 2;
	ctx->cur_append_eoh = TRUE;

	return fetch_data(ctx, body, &size);
}

/* Find message_part for section (eg. 1.3.4) */
static int part_find(struct mail *mail, const struct imap_fetch_body_data *body,
		     const struct message_part **part_r, const char **section)
{
	const struct message_part *part;
	const char *path;
	unsigned int num;

	part = mail->get_parts(mail);
	if (part == NULL)
		return -1;

	path = body->section;
	while (*path >= '0' && *path <= '9' && part != NULL) {
		/* get part number */
		num = 0;
		while (*path != '\0' && *path != '.') {
			if (*path < '0' || *path > '9')
				return FALSE;
			num = num*10 + (*path - '0');
			path++;
		}

		if (*path == '.')
			path++;

		if (part->flags & MESSAGE_PART_FLAG_MULTIPART) {
			/* find the part */
			part = part->children;
			for (; num > 1 && part != NULL; num--)
				part = part->next;
		} else {
			/* only 1 allowed with non-multipart messages */
			if (num != 1)
				part = NULL;
		}

		if (part != NULL &&
		    (part->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) &&
		    ((*path >= '0' && *path <= '9') ||
		     strncmp(path, "HEADER", 6) == 0)) {
			/* if remainder of path is a number or "HEADER",
			   skip the message/rfc822 part */
			part = part->children;
		}
	}

	*part_r = part;
	*section = path;
	return 0;
}

static int fetch_body_mime(struct imap_fetch_context *ctx, struct mail *mail,
			   void *context)
{
	const struct imap_fetch_body_data *body = context;
	const struct message_part *part;
	const char *section;

	if (part_find(mail, body, &part, &section) < 0)
		return -1;

	if (part == NULL) {
		/* part doesn't exist */
		string_t *str = get_prefix(ctx, body, (uoff_t)-1);
		if (o_stream_send(ctx->client->output,
				  str_data(str), str_len(str)) < 0)
			return -1;
		return 1;
	}

	ctx->cur_input = mail->get_stream(mail, NULL, NULL);
	if (ctx->cur_input == NULL)
		return -1;

	i_stream_ref(ctx->cur_input);
	ctx->update_partial = TRUE;

	if (*section == '\0' || strcmp(section, "TEXT") == 0) {
		i_stream_seek(ctx->cur_input, part->physical_pos +
			      part->header_size.physical_size);
		return fetch_data(ctx, body, &part->body_size);
	}

	if (strcmp(section, "HEADER") == 0) {
		/* all headers */
		return fetch_data(ctx, body, &part->header_size);
	}

	if (strncmp(section, "HEADER", 6) == 0 ||
	    strcmp(section, "MIME") == 0) {
		i_stream_seek(ctx->cur_input, part->physical_pos);
		return fetch_header_partial_from(ctx, body, section);
	}

	i_error("BUG: Accepted invalid section from user: '%s'", body->section);
	return 1;
}

static int fetch_body_header_fields_check(const char *section)
{
	if (*section++ != '(')
		return FALSE;

	while (*section != '\0' && *section != ')') {
		if (*section == '(')
			return FALSE;
		section++;
	}

	if (*section++ != ')')
		return FALSE;

	if (*section != '\0')
		return FALSE;
	return TRUE;
}

static int fetch_body_header_fields_init(struct imap_fetch_context *ctx,
					 struct imap_fetch_body_data *body,
					 const char *section)
{
	const char *const *arr;

	if (!fetch_body_header_fields_check(section))
		return FALSE;

	if ((ctx->fetch_data & (MAIL_FETCH_STREAM_HEADER |
				MAIL_FETCH_STREAM_BODY)) != 0) {
		/* we'll need to open the file anyway, don't try to get the
		   headers from cache. */
		imap_fetch_add_handler(ctx, fetch_body_header_partial, body);
		return TRUE;
	}

	t_push();

	for (arr = body->fields; *arr != NULL; arr++) {
		char *hdr = p_strdup(ctx->client->cmd_pool, *arr);
		buffer_append(ctx->all_headers_buf, &hdr, sizeof(hdr));
	}

	body->header_ctx = mailbox_header_lookup_init(ctx->box, body->fields);
	imap_fetch_add_handler(ctx, fetch_body_header_fields, body);
	t_pop();
	return TRUE;
}

static int fetch_body_section_name_init(struct imap_fetch_context *ctx,
					struct imap_fetch_body_data *body)
{
	const char *section = body->section;

	if (*section == '\0') {
		ctx->fetch_data |= MAIL_FETCH_STREAM_HEADER |
			MAIL_FETCH_STREAM_BODY;
		imap_fetch_add_handler(ctx, fetch_body, body);
		return TRUE;
	}

	if (strcmp(section, "TEXT") == 0) {
		ctx->fetch_data |= MAIL_FETCH_STREAM_BODY;
		imap_fetch_add_handler(ctx, fetch_body, body);
		return TRUE;
	}

	if (strncmp(section, "HEADER", 6) == 0) {
		/* exact header matches could be cached */
		if (section[6] == '\0') {
			ctx->fetch_data |= MAIL_FETCH_STREAM_HEADER;
			imap_fetch_add_handler(ctx, fetch_body, body);
			return TRUE;
		}

		if (strncmp(section, "HEADER.FIELDS ", 14) == 0 &&
		    fetch_body_header_fields_init(ctx, body, section+14))
			return TRUE;

		if (strncmp(section, "HEADER.FIELDS.NOT ", 18) == 0 &&
		    fetch_body_header_fields_check(section+18)) {
			imap_fetch_add_handler(ctx, fetch_body_header_partial,
					       body);
			return TRUE;
		}
	} else if (*section >= '0' && *section <= '9') {
		ctx->fetch_data |= MAIL_FETCH_STREAM_BODY |
			MAIL_FETCH_MESSAGE_PARTS;

		while ((*section >= '0' && *section <= '9') ||
		       *section == '.') section++;

		if (*section == '\0' ||
		    strcmp(section, "MIME") == 0 ||
		    strcmp(section, "TEXT") == 0 ||
		    strcmp(section, "HEADER") == 0 ||
		    (strncmp(section, "HEADER.FIELDS ", 14) == 0 &&
		     fetch_body_header_fields_check(section+14)) ||
		    (strncmp(section, "HEADER.FIELDS.NOT ", 18) == 0 &&
		     fetch_body_header_fields_check(section+18))) {
			imap_fetch_add_handler(ctx, fetch_body_mime, body);
			return TRUE;
		}
	}

	client_send_command_error(ctx->client,
		"Invalid BODY[..] parameter: Unknown or broken section");
	return FALSE;
}

/* Parse next digits in string into integer. Returns FALSE if the integer
   becomes too big and wraps. */
static int read_uoff_t(const char **p, uoff_t *value)
{
	uoff_t prev;

	*value = 0;
	while (**p >= '0' && **p <= '9') {
		prev = *value;
		*value = *value * 10 + (**p - '0');

		if (*value < prev)
			return FALSE;

		(*p)++;
	}

	return TRUE;
}

static int body_section_build(struct imap_fetch_context *ctx,
			      struct imap_fetch_body_data *body,
			      const char *prefix,
			      const struct imap_arg_list *list)
{
	string_t *str;
	const char **arr;
	size_t i;

	str = str_new(ctx->client->cmd_pool, 128);
	str_append(str, prefix);
	str_append(str, " (");

	/* @UNSAFE: NULL-terminated list of headers */
	arr = p_new(ctx->client->cmd_pool, const char *, list->size + 1);

	for (i = 0; i < list->size; i++) {
		if (list->args[i].type != IMAP_ARG_ATOM &&
		    list->args[i].type != IMAP_ARG_STRING) {
			client_send_command_error(ctx->client,
				"Invalid BODY[..] parameter: "
				"Header list contains non-strings");
			return FALSE;
		}

		if (i != 0)
			str_append_c(str, ' ');
		arr[i] = str_ucase(IMAP_ARG_STR(&list->args[i]));

		if (list->args[i].type == IMAP_ARG_ATOM)
			str_append(str, arr[i]);
		else {
			str_append_c(str, '"');
			str_append(str, str_escape(arr[i]));
			str_append_c(str, '"');
		}
	}
	str_append_c(str, ')');

	qsort(arr, list->size, sizeof(*arr), strcasecmp_p);
	body->fields = arr;
	body->fields_count = list->size;
	body->section = str_c(str);
	return TRUE;
}
  
int fetch_body_section_init(struct imap_fetch_context *ctx, const char *name,
			    struct imap_arg **args)
{
	struct imap_fetch_body_data *body;
	const char *partial;
	const char *p = name + 4;

	body = p_new(ctx->client->cmd_pool, struct imap_fetch_body_data, 1);
	body->max_size = (uoff_t)-1;

	if (strncmp(p, ".PEEK", 5) == 0) {
		body->peek = TRUE;
		p += 5;
	} else {
		ctx->flags_update_seen = TRUE;
	}

	if (*p != '[') {
		client_send_command_error(ctx->client,
			"Invalid BODY[..] parameter: Missing '['");
		return FALSE;
	}

	if ((*args)[0].type == IMAP_ARG_LIST) {
		/* BODY[HEADER.FIELDS.. (headers list)] */
		if ((*args)[1].type != IMAP_ARG_ATOM ||
		    IMAP_ARG_STR(&(*args)[1])[0] != ']') {
			client_send_command_error(ctx->client,
				"Invalid BODY[..] parameter: Missing ']'");
			return FALSE;
		}
		if (!body_section_build(ctx, body, p+1,
					IMAP_ARG_LIST(&(*args)[0])))
			return FALSE;
		p = IMAP_ARG_STR(&(*args)[1]);
		*args += 2;
	} else {
		/* no headers list */
		body->section = p+1;
		p = strchr(body->section, ']');
		if (p == NULL) {
			client_send_command_error(ctx->client,
				"Invalid BODY[..] parameter: Missing ']'");
			return FALSE;
		}
		body->section = p_strdup_until(ctx->client->cmd_pool,
					       body->section, p);
	}

	if (*++p == '<') {
		/* <start.end> */
		partial = p;
		p++;
		body->skip_set = TRUE;

		if (!read_uoff_t(&p, &body->skip) || body->skip > OFF_T_MAX) {
			/* wrapped */
			client_send_command_error(ctx->client,
				"Invalid BODY[..] parameter: "
				"Too big partial start");
			return FALSE;
		}

		if (*p == '.') {
			p++;
			if (!read_uoff_t(&p, &body->max_size) ||
			    body->max_size > OFF_T_MAX) {
				/* wrapped */
				client_send_command_error(ctx->client,
					"Invalid BODY[..] parameter: "
					"Too big partial end");
				return FALSE;
			}
		}

		if (*p != '>') {
			client_send_command_error(ctx->client,
				t_strdup_printf("Invalid BODY[..] parameter: "
						"Missing '>' in '%s'",
						partial));
			return FALSE;
		}
	}

	return fetch_body_section_name_init(ctx, body);
}

static int fetch_rfc822_size(struct imap_fetch_context *ctx, struct mail *mail,
			     void *context __attr_unused__)
{
	uoff_t size;

	size = mail->get_virtual_size(mail);
	if (size == (uoff_t)-1)
		return -1;

	str_printfa(ctx->cur_str, "RFC822.SIZE %"PRIuUOFF_T" ", size);
	return 1;
}

static int fetch_rfc822(struct imap_fetch_context *ctx, struct mail *mail,
			void *context __attr_unused__)
{
	struct message_size hdr_size, body_size;
	const char *str;

	ctx->cur_input = mail->get_stream(mail, &hdr_size, &body_size);
	if (ctx->cur_input == NULL)
		return -1;

	i_stream_ref(ctx->cur_input);
	ctx->update_partial = FALSE;

	message_size_add(&body_size, &hdr_size);

	if (ctx->cur_offset == 0) {
		str = t_strdup_printf(" RFC822 {%"PRIuUOFF_T"}\r\n",
				      body_size.virtual_size);
		if (ctx->first) {
			str++; ctx->first = FALSE;
		}
		if (o_stream_send_str(ctx->client->output, str) < 0)
			return -1;
	}

        ctx->cur_size = body_size.virtual_size;
	return fetch_stream(ctx, &body_size);
}

static int fetch_rfc822_header(struct imap_fetch_context *ctx,
			       struct mail *mail, void *context __attr_unused__)
{
	struct message_size hdr_size;
	const char *str;

	ctx->cur_input = mail->get_stream(mail, &hdr_size, NULL);
	if (ctx->cur_input == NULL)
		return -1;

	i_stream_ref(ctx->cur_input);
	ctx->update_partial = FALSE;

	str = t_strdup_printf(" RFC822.HEADER {%"PRIuUOFF_T"}\r\n",
			      hdr_size.virtual_size);
	if (ctx->first) {
		str++; ctx->first = FALSE;
	}
	if (o_stream_send_str(ctx->client->output, str) < 0)
		return -1;

        ctx->cur_size = hdr_size.virtual_size;
	return fetch_stream(ctx, &hdr_size);
}

static int fetch_rfc822_text(struct imap_fetch_context *ctx, struct mail *mail,
			     void *context __attr_unused__)
{
	struct message_size hdr_size, body_size;
	const char *str;

	ctx->cur_input = mail->get_stream(mail, &hdr_size, &body_size);
	if (ctx->cur_input == NULL)
		return -1;

	i_stream_ref(ctx->cur_input);
	ctx->update_partial = FALSE;

	str = t_strdup_printf(" RFC822.TEXT {%"PRIuUOFF_T"}\r\n",
			      body_size.virtual_size);
	if (ctx->first) {
		str++; ctx->first = FALSE;
	}
	if (o_stream_send_str(ctx->client->output, str) < 0)
		return -1;

	i_stream_seek(ctx->cur_input, hdr_size.physical_size);
        ctx->cur_size = body_size.virtual_size;
	return fetch_stream(ctx, &body_size);
}

int fetch_rfc822_init(struct imap_fetch_context *ctx, const char *name,
		      struct imap_arg **args __attr_unused__)
{
	if (name[6] == '\0') {
		ctx->fetch_data |= MAIL_FETCH_STREAM_HEADER |
			MAIL_FETCH_STREAM_BODY;
		ctx->flags_update_seen = TRUE;
		imap_fetch_add_handler(ctx, fetch_rfc822, NULL);
		return TRUE;
	}

	if (strcmp(name+6, ".SIZE") == 0) {
		ctx->fetch_data |= MAIL_FETCH_VIRTUAL_SIZE;
		imap_fetch_add_handler(ctx, fetch_rfc822_size, NULL);
		return TRUE;
	}
	if (strcmp(name+6, ".HEADER") == 0) {
		ctx->fetch_data |= MAIL_FETCH_STREAM_HEADER;
		imap_fetch_add_handler(ctx, fetch_rfc822_header, NULL);
		return TRUE;
	}
	if (strcmp(name+6, ".TEXT") == 0) {
		ctx->fetch_data |= MAIL_FETCH_STREAM_BODY;
		ctx->flags_update_seen = TRUE;
		imap_fetch_add_handler(ctx, fetch_rfc822_text, NULL);
		return TRUE;
	}

	client_send_command_error(ctx->client, t_strconcat(
		"Unknown parameter ", name, NULL));
	return FALSE;
}
