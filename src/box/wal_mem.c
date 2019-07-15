/*
 * Copyright 2010-2019, Tarantool AUTHORS, please see AUTHORS file.
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

#include "wal_mem.h"

#include "fiber.h"
#include "errinj.h"

enum {
	/* Initial size for rows storage. */
	WAL_MEM_BUF_INITIAL_ROW_COUNT = 4096,
	/* Initial size for data storage. */
	WAL_MEM_BUF_INITIAL_DATA_SIZE = 65536,
	/* How many rows we will place in one buffer. */
	WAL_MEM_BUF_ROWS_LIMIT = 8192,
	/* How many data we will place in one buffer. */
	WAL_MEM_BUF_DATA_LIMIT = 1 << 19,
};

void
wal_mem_create(struct wal_mem *wal_mem)
{
	int i;
	for (i = 0; i < WAL_MEM_BUF_COUNT; ++i) {
		ibuf_create(&wal_mem->buf[i].rows, &cord()->slabc,
			    WAL_MEM_BUF_INITIAL_ROW_COUNT *
			    sizeof(struct xrow_header));
		obuf_create(&wal_mem->buf[i].data, &cord()->slabc,
			    WAL_MEM_BUF_INITIAL_DATA_SIZE);
	}
	wal_mem->last_buf_index = 0;
	wal_mem->first_buf_index = 0;
}

void
wal_mem_destroy(struct wal_mem *wal_mem)
{
	int i;
	for (i = 0; i < WAL_MEM_BUF_COUNT; ++i) {
		ibuf_destroy(&wal_mem->buf[i].rows);
		obuf_destroy(&wal_mem->buf[i].data);
	}
}

/*
 * Switch to the next buffer if required and discard outdated data.
 */
static struct wal_mem_buf *
wal_mem_rotate(struct wal_mem *wal_mem)
{
	struct wal_mem_buf *mem_buf = wal_mem->buf +
				      wal_mem->last_buf_index % WAL_MEM_BUF_COUNT;
	if (ibuf_used(&mem_buf->rows) < WAL_MEM_BUF_ROWS_LIMIT *
					sizeof(struct xrow_header) &&
	    obuf_size(&mem_buf->data) < WAL_MEM_BUF_DATA_LIMIT)
		return mem_buf;
	/* Switch to the next buffer (a target to append new data). */
	++wal_mem->last_buf_index;
	mem_buf = wal_mem->buf + wal_mem->last_buf_index % WAL_MEM_BUF_COUNT;
	if (wal_mem->last_buf_index - wal_mem->first_buf_index <
	    WAL_MEM_BUF_COUNT) {
		/* The buffer is unused, nothing to do. */
		return mem_buf;
	}
	/* Discard data and adjust first buffer index. */
	ibuf_reset(&mem_buf->rows);
	obuf_reset(&mem_buf->data);
	++wal_mem->first_buf_index;
	return mem_buf;
}

void
wal_mem_svp(struct wal_mem *wal_mem, struct vclock *vclock)
{
	struct wal_mem_buf *mem_buf = wal_mem_rotate(wal_mem);
	/* Check if the current buffer is empty and setup vclock. */
	if (ibuf_used(&mem_buf->rows) == 0)
		vclock_copy(&mem_buf->vclock, vclock);
	wal_mem->tx_first_row_index = ibuf_used(&mem_buf->rows) /
				      sizeof(struct wal_mem_buf_row);
	wal_mem->tx_first_row_svp = obuf_create_svp(&mem_buf->data);
}

/* Commit a wal memory transaction and build an iovec with encoded data. */
int
wal_mem_svp_data(struct wal_mem *wal_mem, struct iovec *iovec)
{
	struct wal_mem_buf *mem_buf = wal_mem->buf +
				      wal_mem->last_buf_index % WAL_MEM_BUF_COUNT;
	if (wal_mem->tx_first_row_svp.used == obuf_size(&mem_buf->data))
		return 0;

	int iov_cnt = 1 + obuf_iovcnt(&mem_buf->data) -
		      wal_mem->tx_first_row_svp.pos;
	memcpy(iovec, mem_buf->data.iov + wal_mem->tx_first_row_svp.pos,
	       sizeof(struct iovec) * iov_cnt);
	iovec[0].iov_base += wal_mem->tx_first_row_svp.iov_len;
	iovec[0].iov_len -= wal_mem->tx_first_row_svp.iov_len;
	return iov_cnt;
}

/* Truncate all the data written in the current transaction. */
void
wal_mem_svp_reset(struct wal_mem *wal_mem)
{
	struct wal_mem_buf *mem_buf = wal_mem->buf +
				      wal_mem->last_buf_index % WAL_MEM_BUF_COUNT;
	mem_buf->rows.wpos = mem_buf->rows.rpos + wal_mem->tx_first_row_index *
						  sizeof(struct wal_mem_buf_row);
	obuf_rollback_to_svp(&mem_buf->data, &wal_mem->tx_first_row_svp);
}

int
wal_mem_write(struct wal_mem *wal_mem, struct xrow_header **begin,
	      struct xrow_header **end)
{
	struct wal_mem_buf *mem_buf = wal_mem->buf +
				      wal_mem->last_buf_index % WAL_MEM_BUF_COUNT;

	/* Save rollback values. */
	size_t old_rows_size = ibuf_used(&mem_buf->rows);
	struct obuf_svp data_svp = obuf_create_svp(&mem_buf->data);

	/* Allocate space for row descriptors. */
	struct wal_mem_buf_row *mem_row =
		(struct wal_mem_buf_row *)
		ibuf_alloc(&mem_buf->rows, (end - begin) *
					   sizeof(struct wal_mem_buf_row));
	if (mem_row == NULL) {
		diag_set(OutOfMemory, (end - begin) *
				      sizeof(struct wal_mem_buf_row),
			 "region", "wal memory rows");
		goto error;
	}
	/* Append rows. */
	struct xrow_header **row;
	for (row = begin; row < end; ++row, ++mem_row) {
		struct errinj *inj = errinj(ERRINJ_WAL_BREAK_LSN, ERRINJ_INT);
		if (inj != NULL && inj->iparam == (*row)->lsn) {
			(*row)->lsn = inj->iparam - 1;
			say_warn("injected broken lsn: %lld",
				 (long long) (*row)->lsn);
		}

		/* Reserve space. */
		char *data = obuf_reserve(&mem_buf->data, xrow_approx_len(*row));
		if (data == NULL) {
			diag_set(OutOfMemory, xrow_approx_len(*row),
				 "region", "wal memory data");
			goto error;
		}

		struct iovec iov[XROW_BODY_IOVMAX];
		/*
		 * xrow_header_encode allocates fiber gc space only for row
		 * header.
		 */
		int iov_cnt = xrow_header_encode(*row, 0, iov, 0);
		if (iov_cnt < 0)
			goto error;
		/* Copy row header. */
		data = obuf_alloc(&mem_buf->data, iov[0].iov_len);
		memcpy(data, iov[0].iov_base, iov[0].iov_len);
		/* Initialize row descriptor. */
		mem_row->xrow = **row;
		mem_row->data = data;
		mem_row->size = iov[0].iov_len;
		/* Write bodies and patch location. */
		int i;
		for (i = 1; i < iov_cnt; ++i) {
			/* Append xrow bodies and patch xrow pointers. */
			data = obuf_alloc(&mem_buf->data, iov[i].iov_len);
			memcpy(data, iov[i].iov_base, iov[i].iov_len);
			mem_row->xrow.body[i - 1].iov_base = data;
			mem_row->size += iov[i].iov_len;
		}
	}
	return 0;

error:
	/* Restore buffer state. */
	mem_buf->rows.wpos = mem_buf->rows.rpos + old_rows_size;
	obuf_rollback_to_svp(&mem_buf->data, &data_svp);
	return -1;
}

