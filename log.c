/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

static FILE	*log_file;
static int	 log_level;

/* Substrings loaded from TMUX_LOG_DROP (comma-separated) at first log call. */
static char	**log_drop_subs;
static size_t	  log_drop_n;
static int	  log_drop_initialized;

/* Substrings loaded from TMUX_LOG_KEEP (comma-separated) at first log call. */
static char	**log_keep_subs;
static size_t	  log_keep_n;
static int	  log_keep_initialized;

/* Parse TMUX_LOG_DROP once; entries are comma-separated substrings. */
static void
log_init_drops(void)
{
	const char	*env;
	char		*copy, *p, *tok;
	size_t		 cap = 0;

	env = getenv("TMUX_LOG_DROP");
	if (env == NULL || *env == '\0')
		return;
	if ((copy = strdup(env)) == NULL)
		return;
	p = copy;
	while ((tok = strsep(&p, ",")) != NULL) {
		if (*tok == '\0')
			continue;
		if (log_drop_n == cap) {
			cap = (cap == 0) ? 16 : cap * 2;
			log_drop_subs = xreallocarray(log_drop_subs, cap,
			    sizeof *log_drop_subs);
		}
		log_drop_subs[log_drop_n++] = strdup(tok);
	}
	free(copy);
}

/* Run log_init_drops() exactly once, regardless of caller. */
static void
log_ensure_drops(void)
{
	if (log_drop_initialized)
		return;
	log_drop_initialized = 1;
	log_init_drops();
}

/* Return 1 if buf matches any entry in the drop list. */
static int
log_should_drop(const char *buf)
{
	size_t	i;

	log_ensure_drops();
	for (i = 0; i < log_drop_n; i++) {
		if (strstr(buf, log_drop_subs[i]) != NULL)
			return (1);
	}
	return (0);
}

/*
 * Fast-path drop check against __func__ only — avoids the vsnprintf cost
 * incurred by log_should_drop, which has to operate on the rendered buffer
 * because some patterns ("peer 0x", "file ", "wcwidth(", ...) only appear in
 * the message body. Drop entries whose leading identifier is followed by
 * '\0', ':' or ' ' represent function-name prefixes; for those we can match
 * directly on __func__ and short-circuit before any formatting work happens.
 * Entries that continue with other characters (e.g. "wcwidth(", "peer 0x")
 * fall through and are handled by the slow path. Drop always takes precedence
 * over keep, so a positive fast-path drop is unconditionally correct even
 * when TMUX_LOG_KEEP is in effect.
 */
static int
log_func_should_drop(const char *func)
{
	size_t		 i, n;
	const char	*sub;
	char		 tail;
	char		 ident[64];

	log_ensure_drops();
	if (log_drop_n == 0 || func == NULL)
		return (0);
	for (i = 0; i < log_drop_n; i++) {
		sub = log_drop_subs[i];
		/* Length of the leading [A-Za-z0-9_] identifier in sub. */
		for (n = 0; sub[n] != '\0'; n++) {
			if (!isalnum((unsigned char)sub[n]) && sub[n] != '_')
				break;
		}
		if (n == 0)
			continue;
		tail = sub[n];
		/*
		 * Fast path only handles entries shaped like a function name
		 * followed by an end-of-name terminator. ':' is the standard
		 * "%s: ..." log_debug prefix; ' ' covers the few callers that
		 * emit "<func> ..." without a colon (e.g. status_redraw);
		 * '\0' covers bare-prefix entries like "format_loop" that
		 * intentionally match a family of related functions via
		 * strstr.
		 */
		if (tail != '\0' && tail != ':' && tail != ' ')
			continue;
		if (strstr(func, sub) != NULL) {
			/* Full substring (including the terminator) is in
			 * func — unlikely for ':' / ' ' tails but harmless. */
			return (1);
		}
		if (tail != '\0' && n < sizeof ident) {
			/*
			 * The full sub (with trailing ':' or ' ') doesn't
			 * occur in func because __func__ never carries those
			 * separators. Retry matching just the identifier
			 * prefix against func — strstr so bare prefixes like
			 * "cmd_find_" match cmd_find_pane.
			 */
			memcpy(ident, sub, n);
			ident[n] = '\0';
			if (strstr(func, ident) != NULL)
				return (1);
		}
	}
	return (0);
}

/* Parse TMUX_LOG_KEEP once; entries are comma-separated substrings. */
static void
log_init_keeps(void)
{
	const char	*env;
	char		*copy, *p, *tok;
	size_t		 cap = 0;

	env = getenv("TMUX_LOG_KEEP");
	if (env == NULL || *env == '\0')
		return;
	if ((copy = strdup(env)) == NULL)
		return;
	p = copy;
	while ((tok = strsep(&p, ",")) != NULL) {
		if (*tok == '\0')
			continue;
		if (log_keep_n == cap) {
			cap = (cap == 0) ? 16 : cap * 2;
			log_keep_subs = xreallocarray(log_keep_subs, cap,
			    sizeof *log_keep_subs);
		}
		log_keep_subs[log_keep_n++] = strdup(tok);
	}
	free(copy);
}

/* Run log_init_keeps() exactly once, regardless of caller. */
static void
log_ensure_keeps(void)
{
	if (log_keep_initialized)
		return;
	log_keep_initialized = 1;
	log_init_keeps();
}

/*
 * Return 1 if buf is allowed past the keep allowlist. When TMUX_LOG_KEEP is
 * unset, everything passes (existing behaviour). When set, only buffers that
 * match at least one substring pass — the rest are dropped before the drop
 * blacklist runs.
 */
static int
log_should_keep(const char *buf)
{
	size_t	i;

	log_ensure_keeps();
	if (log_keep_n == 0)
		return (1);
	for (i = 0; i < log_keep_n; i++) {
		if (strstr(buf, log_keep_subs[i]) != NULL)
			return (1);
	}
	return (0);
}

/* Log callback for libevent. */
static void
log_event_cb(__unused int severity, const char *msg)
{
	log_debug("%s", msg);
}

/* Increment log level. */
void
log_add_level(void)
{
	log_level++;
}

/* Get log level. */
int
log_get_level(void)
{
	return (log_level);
}

/* Open logging to file. */
void
log_open(const char *name)
{
	char	*path;

	if (log_level == 0)
		return;
	log_close();

	xasprintf(&path, "tmux-%s-%ld.log", name, (long)getpid());
	log_file = fopen(path, "a");
	free(path);
	if (log_file == NULL)
		return;

	setvbuf(log_file, NULL, _IOLBF, 0);
	event_set_log_callback(log_event_cb);
}

/* Toggle logging. */
void
log_toggle(const char *name)
{
	if (log_level == 0) {
		log_level = 1;
		log_open(name);
		log_debug("log opened");
	} else {
		log_debug("log closed");
		log_level = 0;
		log_close();
	}
}

/* Close logging. */
void
log_close(void)
{
	if (log_file != NULL)
		fclose(log_file);
	log_file = NULL;

	event_set_log_callback(NULL);
}

/* Write a log message. */
static void printflike(1, 0)
log_vwrite(const char *msg, va_list ap, const char *prefix)
{
	char		*s, *out;
	struct timeval	 tv;

	if (log_file == NULL)
		return;

	if (vasprintf(&s, msg, ap) == -1)
		return;
	if (stravis(&out, s, VIS_OCTAL|VIS_CSTYLE|VIS_TAB|VIS_NL) == -1) {
		free(s);
		return;
	}
	free(s);

	gettimeofday(&tv, NULL);
	if (fprintf(log_file, "%lld.%06d %s%s\n", (long long)tv.tv_sec,
	    (int)tv.tv_usec, prefix, out) != -1)
		fflush(log_file);
	free(out);
}

/*
 * Log a debug message.
 *
 * Invoked as the macro log_debug(...) from tmux.h, which expands to
 * log_debug_func(__func__, ...). The __func__ string lets us short-circuit
 * the dispatcher with log_func_should_drop before paying for any vsnprintf
 * formatting — the dominant cost on hot paths under -v. Entries that the
 * fast path cannot resolve (because the substring is in the rendered body
 * rather than the function name) fall through to the original vsnprintf +
 * buffer-based check.
 */
void
log_debug_func(const char *func, const char *msg, ...)
{
	va_list	ap, ap2;
	char	buf[1024];

	if (log_file == NULL)
		return;

	/* Fast path: __func__-based drop. No formatting performed. */
	if (log_func_should_drop(func))
		return;

	va_start(ap, msg);

	/*
	 * Slow path: format once into buf so the keep allowlist and the
	 * non-function-name drop patterns (e.g. "peer 0x", "file ",
	 * "wcwidth(") can be evaluated against the rendered message.
	 */
	va_copy(ap2, ap);
	vsnprintf(buf, sizeof buf, msg, ap2);
	va_end(ap2);
	/* Allowlist runs first, then blacklist; both are no-op when unset. */
	if (!log_should_keep(buf) || log_should_drop(buf)) {
		va_end(ap);
		return;
	}

	log_vwrite(msg, ap, "");
	va_end(ap);
}

/* Log a critical error with error string and die. */
__dead void
fatal(const char *msg, ...)
{
	char	 tmp[256];
	va_list	 ap;

	if (snprintf(tmp, sizeof tmp, "fatal: %s: ", strerror(errno)) < 0)
		exit(1);

	va_start(ap, msg);
	log_vwrite(msg, ap, tmp);
	va_end(ap);

	exit(1);
}

/* Log a critical error and die. */
__dead void
fatalx(const char *msg, ...)
{
	va_list	 ap;

	va_start(ap, msg);
	log_vwrite(msg, ap, "fatal: ");
	va_end(ap);

	exit(1);
}
