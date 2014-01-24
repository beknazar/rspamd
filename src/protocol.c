/*
 * Copyright (c) 2009-2012, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "main.h"
#include "util.h"
#include "cfg_file.h"
#include "settings.h"
#include "message.h"

/* Max line size */
#define OUTBUFSIZ BUFSIZ
/*
 * Just check if the passed message is spam or not and reply as
 * described below
 */
#define MSG_CMD_CHECK "check"
/* 
 * Check if message is spam or not, and return score plus list
 * of symbols hit
 */
#define MSG_CMD_SYMBOLS "symbols"
/*
 * Check if message is spam or not, and return score plus report
 */
#define MSG_CMD_REPORT "report"
/*
 * Check if message is spam or not, and return score plus report
 * if the message is spam
 */
#define MSG_CMD_REPORT_IFSPAM "report_ifspam"
/*
 * Ignore this message -- client opened connection then changed
 */
#define MSG_CMD_SKIP "skip"
/*
 * Return a confirmation that spamd is alive
 */
#define MSG_CMD_PING "ping"
/*
 * Process this message as described above and return modified message
 */
#define MSG_CMD_PROCESS "process"

/*
 * Learn specified statfile using message
 */
#define MSG_CMD_LEARN "learn"

/*
 * spamassassin greeting:
 */
#define SPAMC_GREETING "SPAMC"
/*
 * rspamd greeting:
 */
#define RSPAMC_GREETING "RSPAMC"
/*
 * Headers
 */
#define CONTENT_LENGTH_HEADER "Content-length"
#define HELO_HEADER "Helo"
#define FROM_HEADER "From"
#define IP_ADDR_HEADER "IP"
#define NRCPT_HEADER "Recipient-Number"
#define RCPT_HEADER "Rcpt"
#define SUBJECT_HEADER "Subject"
#define STATFILE_HEADER "Statfile"
#define QUEUE_ID_HEADER "Queue-ID"
#define ERROR_HEADER "Error"
#define USER_HEADER "User"
#define PASS_HEADER "Pass"
#define JSON_HEADER "Json"
#define HOSTNAME_HEADER "Hostname"
#define DELIVER_TO_HEADER "Deliver-To"

static GList                   *custom_commands = NULL;


/*
 * Remove <> from the fixed string and copy it to the pool
 */
static gchar *
rspamd_protocol_escape_braces (GString *in)
{
	gint                          len = 0;
	gchar                        *orig, *p;

	orig = in->str;
	while ((g_ascii_isspace (*orig) || *orig == '<') && orig - in->str < (gint)in->len) {
		orig ++;
	}

	g_string_erase (in, 0, orig - in->str);

	p = orig;
	while ((!g_ascii_isspace (*p) && *p != '>') && p - in->str < (gint)in->len) {
		p ++;
		len ++;
	}

	g_string_truncate (in, len);

	return in->str;
}

static gboolean
rspamd_protocol_handle_url (struct worker_task *task, struct rspamd_http_message *msg)
{
	GList                          *cur;
	struct custom_command          *cmd;
	const gchar *p;

	if (msg->url == NULL || msg->url->len == 0) {
		task->last_error = "command is absent";
		task->error_code = 400;
		return FALSE;
	}

	if (msg->url->str[0] == '/') {
		p = &msg->url->str[1];
	}
	else {
		p = msg->url->str;
	}

	switch (*p) {
	case 'c':
	case 'C':
		/* check */
		if (g_ascii_strcasecmp (p + 1, MSG_CMD_CHECK + 1) == 0) {
			task->cmd = CMD_CHECK;
		}
		else {
			goto err;
		}
		break;
	case 's':
	case 'S':
		/* symbols, skip */
		if (g_ascii_strcasecmp (p + 1, MSG_CMD_SYMBOLS + 1) == 0) {
			task->cmd = CMD_SYMBOLS;
		}
		else if (g_ascii_strcasecmp (p + 1, MSG_CMD_SKIP + 1) == 0) {
			task->cmd = CMD_SKIP;
		}
		else {
			goto err;
		}
		break;
	case 'p':
	case 'P':
		/* ping, process */
		if (g_ascii_strcasecmp (p + 1, MSG_CMD_PING + 1) == 0) {
			task->cmd = CMD_PING;
		}
		else if (g_ascii_strcasecmp (p + 1, MSG_CMD_PROCESS + 1) == 0) {
			task->cmd = CMD_PROCESS;
		}
		else {
			goto err;
		}
		break;
	case 'r':
	case 'R':
		/* report, report_ifspam */
		if (g_ascii_strcasecmp (p + 1, MSG_CMD_REPORT + 1) == 0) {
			task->cmd = CMD_REPORT;
		}
		else if (g_ascii_strcasecmp (p + 1, MSG_CMD_REPORT_IFSPAM + 1) == 0) {
			task->cmd = CMD_REPORT_IFSPAM;
		}
		else {
			goto err;
		}
		break;
	default:
		cur = custom_commands;
		while (cur) {
			cmd = cur->data;
			if (g_ascii_strcasecmp (p, cmd->name) == 0) {
				task->cmd = CMD_OTHER;
				task->custom_cmd = cmd;
				break;
			}
			cur = g_list_next (cur);
		}

		if (cur == NULL) {
			goto err;
		}
		break;
	}

	return TRUE;

err:
	debug_task ("bad command: %s", p);
	task->last_error = "invalid command";
	task->error_code = 400;
	return FALSE;
}

static gboolean
rspamd_protocol_handle_headers (struct worker_task *task, struct rspamd_http_message *msg)
{
	gchar                           *headern, *err, *tmp;
	gboolean                         res = TRUE;
	struct rspamd_http_header      *h;

	LL_FOREACH (msg->headers, h) {
		headern = h->name->str;

		switch (headern[0]) {
		case 'd':
		case 'D':
			if (g_ascii_strcasecmp (headern, DELIVER_TO_HEADER) == 0) {
				task->deliver_to = rspamd_protocol_escape_braces (h->value);
				debug_task ("read deliver-to header, value: %s", task->deliver_to);
			}
			else {
				debug_task ("wrong header: %s", headern);
				res = FALSE;
			}
			break;
		case 'h':
		case 'H':
			if (g_ascii_strcasecmp (headern, HELO_HEADER) == 0) {
				task->helo = h->value->str;
				debug_task ("read helo header, value: %s", task->helo);
			}
			else if (g_ascii_strcasecmp (headern, HOSTNAME_HEADER) == 0) {
				task->hostname = h->value->str;
				debug_task ("read hostname header, value: %s", task->hostname);
			}
			else {
				debug_task ("wrong header: %s", headern);
				res = FALSE;
			}
			break;
		case 'f':
		case 'F':
			if (g_ascii_strcasecmp (headern, FROM_HEADER) == 0) {
				task->from = rspamd_protocol_escape_braces (h->value);
				debug_task ("read from header, value: %s", task->from);
			}
			else {
				debug_task ("wrong header: %s", headern);
				res = FALSE;
			}
			break;
		case 'j':
		case 'J':
			if (g_ascii_strcasecmp (headern, JSON_HEADER) == 0) {
				task->is_json = parse_flag (h->value->str);
			}
			else {
				debug_task ("wrong header: %s", headern);
				res = FALSE;
			}
			break;
		case 'q':
		case 'Q':
			if (g_ascii_strcasecmp (headern, QUEUE_ID_HEADER) == 0) {
				task->queue_id = h->value->str;
				debug_task ("read queue_id header, value: %s", task->queue_id);
			}
			else {
				debug_task ("wrong header: %s", headern);
				res = FALSE;
			}
			break;
		case 'r':
		case 'R':
			if (g_ascii_strcasecmp (headern, RCPT_HEADER) == 0) {
				tmp = rspamd_protocol_escape_braces (h->value);
				task->rcpt = g_list_prepend (task->rcpt, tmp);
				debug_task ("read rcpt header, value: %s", tmp);
			}
			else if (g_ascii_strcasecmp (headern, NRCPT_HEADER) == 0) {
				task->nrcpt = strtoul (h->value->str, &err, 10);
				debug_task ("read rcpt header, value: %d", (gint)task->nrcpt);
			}
			else {
				msg_info ("wrong header: %s", headern);
				res = FALSE;
			}
			break;
		case 'i':
		case 'I':
			if (g_ascii_strcasecmp (headern, IP_ADDR_HEADER) == 0) {
				tmp = h->value->str;
#ifdef HAVE_INET_PTON
				if (g_ascii_strncasecmp (tmp, "IPv6:", 5) == 0) {
					if (inet_pton (AF_INET6, tmp + 6, &task->from_addr.d.in6) == 1) {
						task->from_addr.ipv6 = TRUE;
					}
					else {
						msg_err ("bad ip header: '%s'", tmp);
						return FALSE;
					}
					task->from_addr.has_addr = TRUE;
				}
				else {
					if (inet_pton (AF_INET, tmp, &task->from_addr.d.in4) != 1) {
						/* Try ipv6 */
						if (inet_pton (AF_INET6, tmp, &task->from_addr.d.in6) == 1) {
							task->from_addr.ipv6 = TRUE;
						}
						else {
							msg_err ("bad ip header: '%s'", tmp);
							return FALSE;
						}
					}
					else {
						task->from_addr.ipv6 = FALSE;
					}
					task->from_addr.has_addr = TRUE;
				}
#else
				if (!inet_aton (tmp, &task->from_addr)) {
					msg_err ("bad ip header: '%s'", tmp);
					return FALSE;
				}
#endif
				debug_task ("read IP header, value: %s", tmp);
			}
			else {
				debug_task ("wrong header: %s", headern);
				res = FALSE;
			}
			break;
		case 'p':
		case 'P':
			if (g_ascii_strcasecmp (headern, PASS_HEADER) == 0) {
				if (h->value->len == sizeof ("all") - 1 &&
						g_ascii_strcasecmp (h->value->str, "all") == 0) {
					task->pass_all_filters = TRUE;
					debug_task ("pass all filters");
				}
			}
			else {
				res = FALSE;
			}
			break;
		case 's':
		case 'S':
			if (g_ascii_strcasecmp (headern, SUBJECT_HEADER) == 0) {
				task->subject = h->value->str;
			}
			else {
				res = FALSE;
			}
			break;
		case 'u':
		case 'U':
			if (g_ascii_strcasecmp (headern, USER_HEADER) == 0) {
				task->user = h->value->str;
			}
			else {
				res = FALSE;
			}
			break;
		default:
			debug_task ("wrong header: %s", headern);
			res = FALSE;
			break;
		}
	}

	if (!res && task->cfg->strict_protocol_headers) {
		msg_err ("deny processing of a request with incorrect or unknown headers");
		task->last_error = "invalid header";
		task->error_code = 400;
		return FALSE;
	}

	return TRUE;
}

gboolean
rspamd_protocol_handle_request (struct worker_task *task,
		struct rspamd_http_message *msg)
{
	if (rspamd_protocol_handle_url (task, msg)) {
		return rspamd_protocol_handle_headers (task, msg);
	}

	return FALSE;
}

static void
write_hashes_to_log (struct worker_task *task, GString *logbuf)
{
	GList                          *cur;
	struct mime_text_part          *text_part;
	
	cur = task->text_parts;

	while (cur) {
		text_part = cur->data;
		if (text_part->fuzzy) {
			if (cur->next != NULL) {
				rspamd_printf_gstring (logbuf, " part: %Xd,", text_part->fuzzy->h);
			}
			else {
				rspamd_printf_gstring (logbuf, " part: %Xd", text_part->fuzzy->h);
			}
		}
		cur = g_list_next (cur);
	}
}


/* Structure for writing tree data */
struct tree_cb_data {
	ucl_object_t *top;
	struct worker_task *task;
};

/*
 * Callback for writing urls
 */
static gboolean
urls_protocol_cb (gpointer key, gpointer value, gpointer ud)
{
	struct tree_cb_data             *cb = ud;
	struct uri                      *url = value;
	ucl_object_t                     *obj;

	obj = ucl_object_fromlstring (url->host, url->hostlen);
	DL_APPEND (cb->top->value.av, obj);

	if (cb->task->cfg->log_urls) {
		msg_info ("<%s> URL: %s - %s: %s", cb->task->message_id, cb->task->user ?
				cb->task->user : (cb->task->from ? cb->task->from : "unknown"),
				inet_ntoa (cb->task->client_addr), struri (url));
	}

	return FALSE;
}

static ucl_object_t *
rspamd_urls_tree_ucl (GTree *input, struct worker_task *task)
{
	struct tree_cb_data             cb;
	ucl_object_t                    *obj;

	obj = ucl_object_typed_new (UCL_ARRAY);
	cb.top = obj;
	cb.task = task;

	g_tree_foreach (input, urls_protocol_cb, &cb);

	return obj;
}

static gboolean
emails_protocol_cb (gpointer key, gpointer value, gpointer ud)
{
	struct tree_cb_data             *cb = ud;
	struct uri                      *url = value;
	ucl_object_t                     *obj;

	obj = ucl_object_fromlstring (url->user, url->userlen + url->hostlen + 1);
	DL_APPEND (cb->top->value.av, obj);

	return FALSE;
}

static ucl_object_t *
rspamd_emails_tree_ucl (GTree *input, struct worker_task *task)
{
	struct tree_cb_data             cb;
	ucl_object_t                    *obj;

	obj = ucl_object_typed_new (UCL_ARRAY);
	cb.top = obj;
	cb.task = task;

	g_tree_foreach (input, emails_protocol_cb, &cb);

	return obj;
}


/* Write new subject */
static const gchar *
make_rewritten_subject (struct metric *metric, struct worker_task *task)
{
	static gchar                    subj_buf[1024];
	gchar                          *p = subj_buf, *end, *c, *res;
	const gchar                    *s;

	end = p + sizeof(subj_buf);
	c = metric->subject;
	s = g_mime_message_get_subject (task->message);

	while (p < end) {
		if (*c == '\0') {
			*p = '\0';
			break;
		}
		else if (*c == '%' && *(c + 1) == 's') {
			p += rspamd_strlcpy (p, (s != NULL) ? s : "", end - p);
			c += 2;
		}
		else {
			*p = *c ++;
		}
		p ++;
	}
	res = g_mime_utils_header_encode_text (subj_buf);

	memory_pool_add_destructor (task->task_pool, (pool_destruct_func)g_free, res);

	return res;
}

static ucl_object_t *
rspamd_str_list_ucl (GList *str_list)
{
	ucl_object_t                    *top = NULL, *obj;
	GList                           *cur;

	top = ucl_object_typed_new (UCL_ARRAY);
	cur = str_list;
	while (cur) {
		obj = ucl_object_fromstring (cur->data);
		DL_APPEND (top->value.av, obj);
		cur = g_list_next (cur);
	}

	return top;
}

static ucl_object_t *
rspamd_metric_symbol_ucl (struct worker_task *task, struct metric *m,
		struct symbol *sym, GString *logbuf)
{
	ucl_object_t                    *obj = NULL;
	const gchar                     *description = NULL;

	rspamd_printf_gstring (logbuf, "%s,", sym->name);
	description = g_hash_table_lookup (m->descriptions, sym->name);

	obj = ucl_object_insert_key (obj, ucl_object_fromstring (sym->name), "name", 0, false);
	obj = ucl_object_insert_key (obj, ucl_object_fromdouble (sym->score), "score", 0, false);
	if (description) {
		obj = ucl_object_insert_key (obj, ucl_object_fromstring (description), "description", 0, false);
	}
	if (sym->options != NULL) {
		obj = ucl_object_insert_key (obj, rspamd_str_list_ucl (sym->options), "options", 0, false);
	}

	return obj;
}

static ucl_object_t *
rspamd_metric_result_ucl (struct worker_task *task, struct metric_result *mres, GString *logbuf)
{
	GHashTableIter                   hiter;
	struct symbol                  *sym;
	struct metric                  *m;
	gboolean                         is_spam;
	enum rspamd_metric_action        action = METRIC_ACTION_NOACTION;
	ucl_object_t                    *obj = NULL, *sobj;
	gdouble                          required_score;
	gpointer                         h, v;
	const gchar                     *subject;
	gchar                            action_char;

	m = mres->metric;

	/* XXX: handle settings */
	required_score = m->actions[METRIC_ACTION_REJECT].score;
	is_spam = (mres->score >= required_score);
	action = check_metric_action (mres->score, required_score, m);
	if (task->is_skipped) {
		action_char = 'S';
	}
	else if (is_spam) {
		action_char = 'T';
	}
	else {
		action_char = 'F';
	}
	rspamd_printf_gstring (logbuf, "(%s: %c (%s): [%.2f/%.2f] [",
			m->name, action_char,
			str_action_metric (action),
			mres->score, required_score);

	obj = ucl_object_insert_key (obj, ucl_object_frombool (is_spam), "is_spam", 0, false);
	obj = ucl_object_insert_key (obj, ucl_object_frombool (task->is_skipped), "is_skipped", 0, false);
	obj = ucl_object_insert_key (obj, ucl_object_fromdouble (mres->score), "score", 0, false);
	obj = ucl_object_insert_key (obj, ucl_object_fromdouble (required_score), "required_score", 0, false);
	obj = ucl_object_insert_key (obj, ucl_object_fromstring (str_action_metric (action)),
			"action", 0, false);

	if (action == METRIC_ACTION_REWRITE_SUBJECT) {
		subject = make_rewritten_subject (m, task);
		obj = ucl_object_insert_key (obj, ucl_object_fromstring (subject),
					"subject", 0, false);
	}
	/* Now handle symbols */
	g_hash_table_iter_init (&hiter, mres->symbols);
	while (g_hash_table_iter_next (&hiter, &h, &v)) {
		sym = (struct symbol *)v;
		sobj = rspamd_metric_symbol_ucl (task, m, sym, logbuf);
		obj = ucl_object_insert_key (obj, sobj, h, 0, false);
	}

	/* Cut the trailing comma if needed */
	if (logbuf->str[logbuf->len - 1] == ',') {
		logbuf->len --;
	}

#ifdef HAVE_CLOCK_GETTIME
	rspamd_printf_gstring (logbuf, "]), len: %z, time: %s, dns req: %d,",
			task->msg->len, calculate_check_time (&task->tv, &task->ts,
			task->cfg->clock_res, &task->scan_milliseconds), task->dns_requests);
#else
	rspamd_printf_gstring (logbuf, "]), len: %z, time: %s, dns req: %d,",
			task->msg->len,
			calculate_check_time (&task->tv, task->cfg->clock_res, &task->scan_milliseconds),
			task->dns_requests);
#endif

	return obj;
}

/*
 * GString ucl emitting functions
 */
static int
rspamd_gstring_append_character (unsigned char c, size_t len, void *ud)
{
	GString *buf = ud;

	if (len == 1) {
		g_string_append_c (buf, c);
	}
	else {
		if (buf->allocated_len - buf->len <= len) {
			g_string_set_size (buf, buf->len + len + 1);
		}
		memset (&buf->str[buf->len], c, len);
		buf->len += len;
		buf->str[buf->len] = '\0';
	}

	return 0;
}

static int
rspamd_gstring_append_len (const unsigned char *str, size_t len, void *ud)
{
	GString *buf = ud;

	g_string_append_len (buf, str, len);

	return 0;
}

static int
rspamd_gstring_append_int (int64_t val, void *ud)
{
	GString *buf = ud;

	rspamd_printf_gstring (buf, "%L", (intmax_t)val);
	return 0;
}

static int
rspamd_gstring_append_double (double val, void *ud)
{
	GString *buf = ud;
	const double delta = 0.0000001;

	if (val == (double)(int)val) {
		rspamd_printf_gstring (buf, "%.1f", val);
	}
	else if (fabs (val - (double)(int)val) < delta) {
		/* Write at maximum precision */
		rspamd_printf_gstring (buf, "%.*g", DBL_DIG, val);
	}
	else {
		rspamd_printf_gstring (buf, "%f", val);
	}

	return 0;
}

static void
write_check_reply (struct rspamd_http_message *msg, struct worker_task *task)
{
	GString                         *logbuf;
	struct metric_result           *metric_res;
	GHashTableIter                   hiter;
	gpointer                         h, v;
	ucl_object_t                    *top = NULL, *obj;
	struct ucl_emitter_functions func = {
		.ucl_emitter_append_character = rspamd_gstring_append_character,
		.ucl_emitter_append_len = rspamd_gstring_append_len,
		.ucl_emitter_append_int = rspamd_gstring_append_int,
		.ucl_emitter_append_double = rspamd_gstring_append_double
	};

	/* Output the first line - check status */
	logbuf = g_string_sized_new (BUFSIZ);
	rspamd_printf_gstring (logbuf, "id: <%s>, qid: <%s>, ", task->message_id, task->queue_id);

	if (task->user) {
		rspamd_printf_gstring (logbuf, "user: %s, ", task->user);
	}

	rspamd_roll_history_update (task->worker->srv->history, task);
	g_hash_table_iter_init (&hiter, task->results);

	/* Convert results to an ucl object */
	while (g_hash_table_iter_next (&hiter, &h, &v)) {
		metric_res = (struct metric_result *)v;
		obj = rspamd_metric_result_ucl (task, metric_res, logbuf);
		top = ucl_object_insert_key (top, obj, h, 0, false);
	}

	if (task->messages != NULL) {
		top = ucl_object_insert_key (top, rspamd_str_list_ucl (task->messages), "messages", 0, false);
	}
	if (g_tree_nnodes (task->urls) > 0) {
		top = ucl_object_insert_key (top, rspamd_urls_tree_ucl (task->urls, task), "urls", 0, false);
	}
	if (g_tree_nnodes (task->emails) > 0) {
		top = ucl_object_insert_key (top, rspamd_emails_tree_ucl (task->emails, task), "emails", 0, false);
	}
	
	top = ucl_object_insert_key (top, ucl_object_fromstring (task->message_id), "message-id", 0, false);

	write_hashes_to_log (task, logbuf);
	msg_info ("%v", logbuf);
	g_string_free (logbuf, TRUE);

	msg->body = g_string_sized_new (BUFSIZ);
	func.ud = msg->body;
	ucl_object_emit_full (top, UCL_EMIT_JSON_COMPACT, &func);
	ucl_object_unref (top);

	/* Increase counters */
	task->worker->srv->stat->messages_scanned++;
}

void
rspamd_protocol_write_reply (struct worker_task *task)
{
	struct rspamd_http_message    *msg;
	const gchar                   *ctype = "text/plain";

	msg = rspamd_http_new_message (HTTP_RESPONSE);
	msg->date = time (NULL);

	task->state = CLOSING_CONNECTION;

	debug_task ("writing reply to client");
	if (task->error_code != 0) {
		msg->code = task->error_code;
		msg->body = g_string_new (task->last_error);
	}
	else {
		switch (task->cmd) {
		case CMD_REPORT_IFSPAM:
		case CMD_REPORT:
		case CMD_CHECK:
		case CMD_SYMBOLS:
		case CMD_PROCESS:
		case CMD_SKIP:
			ctype = "application/json";
			write_check_reply (msg, task);
			break;
		case CMD_PING:
			msg->body = g_string_new ("pong");
			break;
		case CMD_OTHER:
			msg_err ("BROKEN");
			break;
		}
	}

	rspamd_http_connection_reset (task->http_conn);
	rspamd_http_connection_write_message (task->http_conn, msg, NULL,
					ctype, task, task->sock, &task->tv, task->ev_base);
}

void
register_protocol_command (const gchar *name, protocol_reply_func func)
{
	struct custom_command          *cmd;

	cmd = g_malloc (sizeof (struct custom_command));
	cmd->name = name;
	cmd->func = func;

	custom_commands = g_list_prepend (custom_commands, cmd);
}
