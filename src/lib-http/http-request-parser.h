#ifndef HTTP_REQUEST_PARSER_H
#define HTTP_REQUEST_PARSER_H

#include "http-response.h"

struct http_request {
	const char *method;
	const char *target;

	unsigned char version_major;
	unsigned char version_minor;

	time_t date;
	struct istream *payload;

	ARRAY_TYPE(http_response_header) headers;
	ARRAY_TYPE(const_string) connection_options;

	unsigned int connection_close:1;
};

struct http_request_parser *
http_request_parser_init(struct istream *input);
void http_request_parser_deinit(struct http_request_parser **_parser);

int http_request_parse_next(struct http_request_parser *parser,
			    pool_t pool, struct http_request *request,
			    const char **error_r);

#endif
