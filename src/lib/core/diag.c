/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "diag.h"
#include "fiber.h"

/* Must be set by the library user */
struct error_factory *error_factory = NULL;

struct error *
error_prev(struct error *e)
{
	assert(e != NULL);
	return e->next;
}

int
error_set_prev(struct error *e, struct error *prev)
{
	/*
	 * Make sure that adding error won't result in cycles.
	 * Don't bother with sophisticated cycle-detection
	 * algorithms, simple iteration is OK since as a rule
	 * list contains a dozen errors at maximum.
	 */
	struct error *tmp = prev;
	while (tmp != NULL) {
		if (tmp == e)
			return -1;
		tmp = tmp->next;
	}
	/*
	 * At once error can be reason for only one error.
	 * So unlink previous 'prev' node.
	 *
	 * +--------+ NEXT +--------+
	 * |    e   | ---> |old prev|
	 * +--------+      +--------+
	 *     ^               |
	 *     |      PREV     |
	 *     +-------X-------+
	 *
	 */
	if (e->next != NULL)
		e->next->prev = NULL;
	/* Set new 'prev' node. */
	e->next = prev;
	/*
	 * Unlink new 'prev' node from its old stack.
	 * nil can be also passed as an argument.
	 */
	if (prev != NULL) {
		error_unlink_tail(prev);
		prev->prev = e;
		error_ref(prev);
	}
	return 0;
}

void
error_create(struct error *e,
	     error_f destroy, error_f raise, error_f log,
	     const struct type_info *type, const char *file, unsigned line)
{
	e->destroy = destroy;
	e->raise = raise;
	e->log = log;
	e->type = type;
	e->refs = 0;
	e->saved_errno = 0;
	if (file != NULL) {
		snprintf(e->file, sizeof(e->file), "%s", file);
		e->line = line;
	} else {
		e->file[0] = '\0';
		e->line = 0;
	}
	e->errmsg[0] = '\0';
	e->prev = NULL;
	e->next = NULL;
}

struct diag *
diag_get()
{
	return &fiber()->diag;
}

void
error_format_msg(struct error *e, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	error_vformat_msg(e, format, ap);
	va_end(ap);
}

void
error_vformat_msg(struct error *e, const char *format, va_list ap)
{
	vsnprintf(e->errmsg, sizeof(e->errmsg), format, ap);
}

