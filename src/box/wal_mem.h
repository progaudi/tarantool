#ifndef TARANTOOL_WAL_MEM_H_INCLUDED
#define TARANTOOL_WAL_MEM_H_INCLUDED
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
#include <stdint.h>

#include "small/ibuf.h"
#include "small/obuf.h"
#include "xrow.h"
#include "vclock.h"

enum {
	/*
	 * Wal memory object contains some count of rotating data buffers.
	 * Estimated decrease in amount of stored row is about
	 * 1/(COUNT OF BUFFERS). However the bigger value makes rotation
	 * more frequent, the decrease would be smoother and size of
	 * a wal memory more stable.
	 */
	WAL_MEM_BUF_COUNT = 8,
};

/*
 * A wal memory row descriptor which contains decoded xrow header and
 * encoded data pointer and size.
 */
struct wal_mem_buf_row {
	/* Decoded xrow header. */
	struct xrow_header xrow;
	/* Pointer to the xrow encoded raw data. */
	void *data;
	/* xrow raw data size. */
	size_t size;
};

/*
 * Wal memory data buffer which contains
 *  a vclock just before the first contained row,
 *  an ibuf with row descriptors
 *  an obuf with encoded data
 */
struct wal_mem_buf {
	/* vclock just before the first row. */
	struct vclock vclock;
	/* A row descriptor array. */
	struct ibuf rows;
	/* Data storage for encoded row data. */
	struct obuf data;
};

/*
 * Wal memory contains WAL_MEM_BUF_COUNT wal memory buffers which are
 * organized in a ring. In order to track Wal memory tracks the first and
 * the last used buffers indexes (generation) and those indexes are not wrapped
 * around the ring. Each rotation increases the last buffer index and
 * each buffer discard increases the first buffer index. To evaluate effective
 * index in an wal memory array a modulo operation (or mask) should be used.
 */
struct wal_mem {
	/* An index of the first used buffer. */
	uint32_t first_buf_index;
	/* An index of the last used buffer. */
	uint32_t last_buf_index;
	/* A memory buffer array. */
	struct wal_mem_buf buf[WAL_MEM_BUF_COUNT];
	/* The first row index written in the current transaction. */
	uint32_t tx_first_row_index;
	/* The first row data svp written in the current transaction. */
	struct obuf_svp tx_first_row_svp;
};

/* Create a wal memory. */
void
wal_mem_create(struct wal_mem *wal_mem);

/* Destroy wal memory structure. */
void
wal_mem_destroy(struct wal_mem *wal_mem);

/*
 * Rotate a wal memory if required and save the current wal memory write
 * position.
 */
void
wal_mem_svp(struct wal_mem *wal_mem, struct vclock *vclock);

/* Retrieve data after last svp. */
int
wal_mem_svp_data(struct wal_mem *wal_mem, struct iovec *iovec);

/* Truncate all the data written after the last svp. */
void
wal_mem_svp_reset(struct wal_mem *wal_mem);

/* Count of rows written since the last svp. */
static inline int
wal_mem_svp_row_count(struct wal_mem *wal_mem)
{
	struct wal_mem_buf *mem_buf = wal_mem->buf +
				      wal_mem->last_buf_index % WAL_MEM_BUF_COUNT;
	return ibuf_used(&mem_buf->rows) / sizeof(struct wal_mem_buf_row) -
	       wal_mem->tx_first_row_index;
}

/*
 * Append xrow array to a wal memory. The array is placed into one
 * wal memory buffer and each row takes a continuous space in a data buffer.
 * continuously.
 * Return
 *  0 for Ok
 *  -1 in case of error
 */
int
wal_mem_write(struct wal_mem *wal_mem, struct xrow_header **begin,
	      struct xrow_header **end);

/* Wal memory cursor to track a position in a wal memory. */
struct wal_mem_cursor {
	/* Current memory buffer index. */
	uint32_t buf_index;
	/* Current row index. */
	uint32_t row_index;
};

/* Create a wal memory cursor from the wal memory current position. */
void
wal_mem_cursor_create(struct wal_mem *wal_mem,
		      struct wal_mem_cursor *wal_mem_cursor);

/*
 * Move cursor forward to the end of current buffer and build iovec which
 * points on read data. If current buffer hasn't got data to read then cursor
 * swithes to the next one if it possible.
 * Return
 * count of build iovec items,
 * 0 there are no more data to read,
 * -1 in case of error.
 */
int
wal_mem_cursor_forward(struct wal_mem *wal_mem,
		       struct wal_mem_cursor *wal_mem_cursor, struct iovec *iov);

#endif /* TARANTOOL_WAL_MEM_H_INCLUDED */
