/* Copyright (c) 2005-2008 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "hostpid.h"
#include "istream.h"
#include "str.h"
#include "str-sanitize.h"
#include "var-expand.h"
#include "message-date.h"
#include "message-size.h"
#include "duplicate.h"
#include "istream-header-filter.h"
#include "smtp-client.h"
#include "deliver.h"
#include "mail-send.h"

#include <stdlib.h>
#include <sys/wait.h>

int global_outgoing_count = 0;

static const struct var_expand_table *
get_var_expand_table(struct mail *mail, const char *reason,
		     const char *recipient)
{
	static struct var_expand_table static_tab[] = {
		{ 'n', NULL },
		{ 'r', NULL },
		{ 's', NULL },
		{ 't', NULL },
		{ '\0', NULL }
	};
	struct var_expand_table *tab;
	const char *subject;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	tab[0].value = "\r\n";
	tab[1].value = reason;
	if (mail_get_first_header(mail, "Subject", &subject) <= 0)
		subject = "";
	tab[2].value = str_sanitize(subject, 80);
	tab[3].value = recipient;

	return tab;
}

int mail_send_rejection(struct mail *mail, const char *recipient,
			const char *reason)
{
    struct istream *input;
    struct smtp_client *smtp_client;
    FILE *f;
    struct message_size hdr_size;
    const char *return_addr, *str;
    const unsigned char *data;
    const char *msgid, *orig_msgid, *boundary;
    string_t *human_reason;
    size_t size;
    int ret;

    if (mail_get_first_header(mail, "Message-ID", &orig_msgid) < 0)
	    orig_msgid = NULL;
    return_addr = deliver_get_return_address(mail);
    if (return_addr == NULL) {
	    i_info("msgid=%s: Return-Path missing, rejection reason: %s",
		   orig_msgid == NULL ? "" : str_sanitize(orig_msgid, 80),
		   str_sanitize(reason, 512));
	    return 0;
    }

    if (getenv("DEBUG") != NULL) {
	    i_info("Sending a rejection to %s: %s", recipient,
		   str_sanitize(reason, 512));
    }

    smtp_client = smtp_client_open(return_addr, NULL, &f);

    msgid = deliver_get_new_message_id();
    boundary = t_strdup_printf("%s/%s", my_pid, deliver_set->hostname);

    fprintf(f, "Message-ID: %s\r\n", msgid);
    fprintf(f, "Date: %s\r\n", message_date_create(ioloop_time));
    fprintf(f, "From: Mail Delivery Subsystem <%s>\r\n",
	    deliver_set->postmaster_address);
    fprintf(f, "To: <%s>\r\n", return_addr);
    fprintf(f, "MIME-Version: 1.0\r\n");
    fprintf(f, "Content-Type: "
	    "multipart/report; report-type=disposition-notification;\r\n"
	    "\tboundary=\"%s\"\r\n", boundary);
    fprintf(f, "Subject: Automatically rejected mail\r\n");
    fprintf(f, "Auto-Submitted: auto-replied (rejected)\r\n");
    fprintf(f, "Precedence: bulk\r\n");
    fprintf(f, "\r\nThis is a MIME-encapsulated message\r\n\r\n");

    /* human readable status report */
    fprintf(f, "--%s\r\n", boundary);
    fprintf(f, "Content-Type: text/plain; charset=utf-8\r\n");
    fprintf(f, "Content-Disposition: inline\r\n");
    fprintf(f, "Content-Transfer-Encoding: 8bit\r\n\r\n");

    human_reason = t_str_new(256);
    var_expand(human_reason, deliver_set->rejection_reason,
	       get_var_expand_table(mail, reason, recipient));
    fprintf(f, "%s\r\n", str_c(human_reason));

    /* MDN status report */
    fprintf(f, "--%s\r\n"
	    "Content-Type: message/disposition-notification\r\n\r\n",
	    boundary);
    fprintf(f, "Reporting-UA: %s; Dovecot Mail Delivery Agent\r\n",
	    deliver_set->hostname);
    if (mail_get_first_header(mail, "Original-Recipient", &str) > 0)
	    fprintf(f, "Original-Recipient: rfc822; %s\r\n", str);
    fprintf(f, "Final-Recipient: rfc822; %s\r\n", recipient);

    if (orig_msgid != NULL)
	fprintf(f, "Original-Message-ID: %s\r\n", orig_msgid);
    fprintf(f, "Disposition: "
	    "automatic-action/MDN-sent-automatically; deleted\r\n");
    fprintf(f, "\r\n");

    /* original message's headers */
    fprintf(f, "--%s\r\nContent-Type: message/rfc822\r\n\r\n", boundary);

    if (mail_get_stream(mail, &hdr_size, NULL, &input) == 0) {
	    /* Note: If you add more headers, they need to be sorted.
	       We'll drop Content-Type because we're not including the message
	       body, and having a multipart Content-Type may confuse some
	       MIME parsers when they don't see the message boundaries. */
	    static const char *const exclude_headers[] = {
		    "Content-Type"
	    };

	    input = i_stream_create_header_filter(input,
	    		HEADER_FILTER_EXCLUDE | HEADER_FILTER_NO_CR |
			HEADER_FILTER_HIDE_BODY, exclude_headers,
			N_ELEMENTS(exclude_headers),
			null_header_filter_callback, NULL);

	    while ((ret = i_stream_read_data(input, &data, &size, 0)) > 0) {
		    if (fwrite(data, size, 1, f) == 0)
			    break;
		    i_stream_skip(input, size);
	    }
	    i_stream_unref(&input);

	    i_assert(ret != 0);
    }

    fprintf(f, "\r\n\r\n--%s--\r\n", boundary);
    return smtp_client_close(smtp_client);
}

int mail_send_forward(struct mail *mail, const char *forwardto)
{
    static const char *hide_headers[] = {
        "Return-Path"
    };
    struct istream *input;
    struct smtp_client *smtp_client;
    FILE *f;
    const unsigned char *data;
    const char *return_path;
    size_t size;
    int ret;

    if (mail_get_stream(mail, NULL, NULL, &input) < 0)
	    return -1;

    if (mail_get_first_header(mail, "Return-Path", &return_path) <= 0)
	    return_path = "";

    if (getenv("DEBUG") != NULL) {
	    i_info("Sending a forward to <%s> with return path <%s>",
		   forwardto, return_path);
    }

    smtp_client = smtp_client_open(forwardto, return_path, &f);

    input = i_stream_create_header_filter(input, HEADER_FILTER_EXCLUDE |
                                          HEADER_FILTER_NO_CR, hide_headers,
                                          N_ELEMENTS(hide_headers),
					  null_header_filter_callback, NULL);

    while ((ret = i_stream_read_data(input, &data, &size, 0)) > 0) {
	    if (fwrite(data, size, 1, f) == 0)
		    break;
	    i_stream_skip(input, size);
    }
    i_stream_unref(&input);

    return smtp_client_close(smtp_client);
}

