/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "temp-string.h"
#include "rfc822-tokenize.h"
#include "message-parser.h"
#include "message-content-parser.h"
#include "imap-envelope.h"
#include "imap-bodystructure.h"

#define EMPTY_BODYSTRUCTURE \
        "(\"text\" \"plain\" (\"charset\" \"us-ascii\") NIL NIL \"7bit\" 0 0)"

typedef struct {
	Pool pool;
	TempString *str;
	char *content_type, *content_subtype;
	char *content_type_params;
	char *content_transfer_encoding;
	char *content_id;
	char *content_description;
	char *content_disposition;
	char *content_disposition_params;
	char *content_md5;
	char *content_language;

	MessagePartEnvelopeData *envelope;
} MessagePartBodyData;

static void part_write_bodystructure(MessagePart *part, TempString *str,
				     int extended);

static void parse_content_type(const Rfc822Token *tokens,
			       int count, void *user_data)
{
        MessagePartBodyData *data = user_data;
	const char *value;
	int i;

	/* find the content type separator */
	for (i = 0; i < count; i++) {
		if (tokens[i].token == '/')
			break;
	}

	value = rfc822_tokens_get_value_quoted(tokens, i, FALSE);
	data->content_type = p_strdup(data->pool, value);

	value = rfc822_tokens_get_value_quoted(tokens+i+1, count-i-1, FALSE);
	data->content_subtype = p_strdup(data->pool, value);
}

static void parse_save_params_list(const Rfc822Token *name,
				   const Rfc822Token *value, int value_count,
				   void *user_data)
{
        MessagePartBodyData *data = user_data;
	const char *str;

	if (data->str->len != 0)
		t_string_append_c(data->str, ' ');

	t_string_append_c(data->str, '"');
	t_string_append_n(data->str, name->ptr, name->len);
	t_string_append(data->str, "\" ");

        str = rfc822_tokens_get_value_quoted(value, value_count, FALSE);
	t_string_append(data->str, str);
}

static void parse_content_transfer_encoding(const Rfc822Token *tokens,
					    int count, void *user_data)
{
        MessagePartBodyData *data = user_data;
	const char *value;

	value = rfc822_tokens_get_value_quoted(tokens, count, FALSE);
	data->content_transfer_encoding = p_strdup(data->pool, value);
}

static void parse_content_disposition(const Rfc822Token *tokens,
				      int count, void *user_data)
{
        MessagePartBodyData *data = user_data;
	const char *value;

	value = rfc822_tokens_get_value_quoted(tokens, count, FALSE);
	data->content_disposition = p_strdup(data->pool, value);
}

static void parse_content_language(const Rfc822Token *tokens,
				   int count, void *user_data)
{
        MessagePartBodyData *data = user_data;
	const char *value;

	if (count <= 0)
		return;

	value = rfc822_tokens_get_value_quoted(tokens, count, FALSE);
	data->content_language = p_strdup(data->pool, value);

	/* FIXME: a,b,c -> "a" "b" "c" */
}

static void parse_header(MessagePart *part,
			 const char *name, unsigned int name_len,
			 const char *value,
			 unsigned int value_len, void *user_data)
{
	Pool pool = user_data;
	MessagePartBodyData *part_data;
	int parent_rfc822;

        parent_rfc822 = part->parent != NULL && part->parent->message_rfc822;
	if (!parent_rfc822 && (name_len <= 8 ||
			       strncasecmp(name, "Content-", 8) != 0))
		return;

	if (part->user_data == NULL) {
		/* initialize message part data */
		part->user_data = part_data =
			p_new(pool, MessagePartBodyData, 1);
		part_data->pool = pool;
	}
	part_data = part->user_data;

	t_push();

	/* fix the name to be \0-terminated */
	name = t_strndup(name, name_len);

	if (strcasecmp(name, "Content-Type") == 0) {
		part_data->str = t_string_new(256);
		(void)message_content_parse_header(t_strndup(value, value_len),
						   parse_content_type,
						   parse_save_params_list,
						   part_data);
		part_data->content_type_params =
			p_strdup(pool, part_data->str->str);
	} else if (strcasecmp(name, "Content-Transfer-Encoding") == 0) {
		(void)message_content_parse_header(t_strndup(value, value_len),
						parse_content_transfer_encoding,
						NULL, part_data);
	} else if (strcasecmp(name, "Content-ID") == 0) {
		part_data->content_id = p_strndup(pool, value, value_len);
	} else if (strcasecmp(name, "Content-Description") == 0) {
		part_data->content_description =
			p_strndup(pool, value, value_len);
	} else if (strcasecmp(name, "Content-Disposition") == 0) {
		part_data->str = t_string_new(256);
		(void)message_content_parse_header(t_strndup(value, value_len),
						   parse_content_disposition,
						   parse_save_params_list,
						   part_data);
		part_data->content_disposition_params =
			p_strdup(pool, part_data->str->str);
	} else if (strcasecmp(name, "Content-Language") == 0) {
		(void)message_content_parse_header(t_strndup(value, value_len),
						   parse_content_language, NULL,
						   part_data);
	} else if (strcasecmp(name, "Content-MD5") == 0) {
		part_data->content_md5 = p_strndup(pool, value, value_len);
	} else if (parent_rfc822) {
		/* message/rfc822, we need the envelope */
		imap_envelope_parse_header(pool, &part_data->envelope,
					   name, value, value_len);
	}
	t_pop();
}

static void part_parse_headers(MessagePart *part, const char *msg,
			       size_t size, Pool pool)
{
	while (part != NULL) {
		/* note that we want to parse the header of all
		   the message parts, multiparts too. */
		message_parse_header(part, msg + part->pos.physical_pos,
				     part->header_size.physical_size,
				     NULL, parse_header, pool);

		if (part->children != NULL)
			part_parse_headers(part->children, msg, size, pool);

		part = part->next;
	}
}

static void part_write_body_multipart(MessagePart *part, TempString *str,
				      int extended)
{
	MessagePartBodyData *data = part->user_data;

	if (part->children != NULL)
		part_write_bodystructure(part->children, str, extended);
	else {
		/* no parts in multipart message,
		   that's not allowed. write a single
		   0-length text/plain structure */
		t_string_append(str, EMPTY_BODYSTRUCTURE);
	}

	t_string_append_c(str, ' ');
	t_string_append(str, data->content_subtype);

	if (!extended)
		return;

	/* BODYSTRUCTURE data */
	t_string_append_c(str, ' ');
	if (data->content_type_params == NULL)
		t_string_append(str, "NIL");
	else {
		t_string_append_c(str, '(');
		t_string_append(str, data->content_type_params);
		t_string_append_c(str, ')');
	}

	t_string_append_c(str, ' ');
	if (data->content_disposition == NULL)
		t_string_append(str, "NIL");
	else {
		t_string_append_c(str, '(');
		t_string_append(str, data->content_disposition);
		if (data->content_disposition_params != NULL) {
			t_string_append(str, " (");
			t_string_append(str, data->content_disposition_params);
			t_string_append_c(str, ')');
		}
		t_string_append_c(str, ')');
	}

	t_string_append_c(str, ' ');
	if (data->content_language == NULL)
		t_string_append(str, "NIL");
	else {
		t_string_append_c(str, '(');
		t_string_append(str, data->content_language);
		t_string_append_c(str, ')');
	}
}

static void part_write_body(MessagePart *part, TempString *str, int extended)
{
	MessagePartBodyData *data = part->user_data;

	if (data == NULL) {
		/* there was no content headers, use an empty structure */
		data = t_new(MessagePartBodyData, 1);
	}

	/* "content type" "subtype" */
	t_string_append(str, NVL(data->content_type, "\"text\""));
	t_string_append_c(str, ' ');
	t_string_append(str, NVL(data->content_subtype, "\"plain\""));

	/* ("content type param key" "value" ...) */
	t_string_append_c(str, ' ');
	if (data->content_type_params == NULL)
		t_string_append(str, "NIL");
	else {
		t_string_append_c(str, '(');
		t_string_append(str, data->content_type_params);
		t_string_append_c(str, ')');
	}

	t_string_printfa(str, " %s %s %s %lu",
			 NVL(data->content_id, "NIL"),
			 NVL(data->content_description, "NIL"),
			 NVL(data->content_transfer_encoding, "\"8bit\""),
			 (unsigned long) part->body_size.virtual_size);

	if (part->text) {
		/* text/.. contains line count */
		t_string_printfa(str, " %u", part->body_size.lines);
	} else if (part->message_rfc822) {
		/* message/rfc822 contains envelope + body + line count */
		MessagePartBodyData *child_data;

		i_assert(part->children != NULL);
		i_assert(part->children->next == NULL);

                child_data = part->children->user_data;

		t_string_append_c(str, ' ');
		if (child_data != NULL && child_data->envelope != NULL) {
			imap_envelope_write_part_data(child_data->envelope,
						      str);
		} else {
			/* buggy message */
			t_string_append(str, "NIL");
		}
		t_string_append_c(str, ' ');
		part_write_bodystructure(part->children, str, extended);
		t_string_printfa(str, " %u", part->body_size.lines);
	}

	if (!extended)
		return;

	/* BODYSTRUCTURE data */

	/* "md5" ("content disposition" ("disposition" "params"))
	   ("body" "language" "params") */
	t_string_append_c(str, ' ');
	t_string_append(str, NVL(data->content_md5, "NIL"));

	t_string_append_c(str, ' ');
	if (data->content_disposition == NULL)
		t_string_append(str, "NIL");
	else {
		t_string_append_c(str, '(');
		t_string_append(str, data->content_disposition);
		t_string_append_c(str, ')');

		if (data->content_disposition_params != NULL) {
			t_string_append(str, " (");
			t_string_append(str, data->content_disposition_params);
			t_string_append_c(str, ')');
		}
	}

	t_string_append_c(str, ' ');
	if (data->content_language == NULL)
		t_string_append(str, "NIL");
	else {
		t_string_append_c(str, '(');
		t_string_append(str, data->content_language);
		t_string_append_c(str, ')');
	}
}

static void part_write_bodystructure(MessagePart *part, TempString *str,
				     int extended)
{
	while (part != NULL) {
		t_string_append_c(str, '(');
		if (part->multipart)
			part_write_body_multipart(part, str, extended);
		else
			part_write_body(part, str, extended);

		part = part->next;
		t_string_append_c(str, ')');
	}
}

static const char *part_get_bodystructure(MessagePart *part, int extended)
{
	TempString *str;

	str = t_string_new(2048);
	part_write_bodystructure(part, str, extended);
	return str->str;
}

const char *imap_part_get_bodystructure(Pool pool, MessagePart **part,
					const char *msg, size_t size,
					int extended)
{
	if (*part == NULL)
		*part = message_parse(pool, msg, size, parse_header, pool);
	else
		part_parse_headers(*part, msg, size, pool);

	return part_get_bodystructure(*part, extended);
}
