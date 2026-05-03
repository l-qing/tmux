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

/* Return 1 if buf matches any entry in the drop list. */
static int
log_should_drop(const char *buf)
{
	static int	initialized;
	size_t		i;

	if (!initialized) {
		initialized = 1;
		log_init_drops();
	}
	for (i = 0; i < log_drop_n; i++) {
		if (strstr(buf, log_drop_subs[i]) != NULL)
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

/* Log a debug message. */
void
log_debug(const char *msg, ...)
{
	va_list	ap, ap2;
	char	buf[1024];

	if (log_file == NULL)
		return;

	va_start(ap, msg);

	/*
	 * Check drop list before the expensive vasprintf/stravis path.
	 * log_should_drop initialises from TMUX_LOG_DROP on first call;
	 * it returns 0 immediately when the list is empty.
	 */
	va_copy(ap2, ap);
	vsnprintf(buf, sizeof buf, msg, ap2);
	va_end(ap2);
	if (log_should_drop(buf)) {
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
