#ifndef TARANTOOL_XROW_BUF_H_INCLUDED
#define TARANTOOL_XROW_BUF_H_INCLUDED
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

#include "small/obuf.h"
#include "small/rlist.h"
#include "xrow.h"
#include "vclock.h"

enum {
	/*
	 * Xrow buffer contains some count of rotating data chunks.
	 * Every rotation has an estimated decrease in amount of
	 * stored rows at 1/(COUNT OF CHUNKS). However the bigger
	 * value makes rotation more frequent, the decrease would be
	 * smoother and size of a xrow buffer more stable.
	 */
	XROW_BUF_CHUNK_COUNT = 16,
};

/**
 * Xrow_info structure used to describe a row stored in a xrow
 * buffer. Xrow info contains an xrow_header structure, pointer
 * and size of the row_header encoded representation. Xrow header
 * allows to filter rows by replica_id, lsn or replication group
 * while encoded representation could be used to write xrow 
 * without any further encoding.
 */
struct xrow_buf_row_info {
	/** Stored row header. */
	struct xrow_header xrow;
	/** Pointer to row encoded raw data. */
	void *data;
	/** Row encoded raw data size. */
	size_t size;
};

/**
 * Xrow buffer data chunk structure is used to store a continuous
 * sequence of xrow headers written to a xrow buffer. Xrow buffer data
 * chunk contains a vclock of the last row just before the first row
 * stored in the chunk, count of rows, its encoded raw data, and array of
 * stored row info. Vclock is used to track stored vclock lower boundary.
 */
struct xrow_buf_chunk {
	/** Vclock just before the first row in this chunk. */
	struct vclock vclock;
	/** Count of stored rows. */
	size_t row_count;
	/** Stored rows information array. */
	struct xrow_buf_row_info *row_info;
	/** Capacity of stored rows information array. */
	size_t row_info_capacity;
	/** Data storage for encoded rows data. */
	struct obuf data;
};

/**
 * Xrow buffer enables to encode and store some continuous sequence
 * of xrows (both headers and binary encoded representation).
 * Storage organized as a range of globally indexed chunks. New rows
 * are appended to the last one chunk (the youngest one). If the last
 * chunk is already full then a new chunk will be used. Xrow_buffer
 * maintains not more than XROW_BUF_CHUNK_COUNT chunks so when the buffer
 * is already full then a first one chunk should be discarded before a
 * new one could be used. All chunks are organized in a ring which is
 * XROW_BUF_CHUNK_COUNT the size so a chunk in-ring index could be
 * evaluated from the chunk global index with the modulo operation.
 */
struct xrow_buf {
	/** Global index of the first used chunk (the oldest one). */
	size_t first_chunk_index;
	/** Global index of the last used chunk (the youngest one). */
	size_t last_chunk_index;
	/** Ring -array containing chunks . */
	struct xrow_buf_chunk chunk[XROW_BUF_CHUNK_COUNT];
	/**
	 * A xrow_buf transaction is recorded in one chunk only.
	 * When transaction is started current row count and data
	 * buffer svp from the current chunk (which is the last one)
	 * are remembered in order to be able to restore the chunk
	 * state in case of rollback.
	 */
	struct {
		/** The current transaction first row index. */
		uint32_t tx_first_row_index;
		/** The current transaction encoded data start svp. */
		struct obuf_svp tx_first_row_svp;
	};
};

/** Create a wal memory. */
void
xrow_buf_create(struct xrow_buf *xrow_buf);

/** Destroy wal memory structure. */
void
xrow_buf_destroy(struct xrow_buf *xrow_buf);

/**
 * Begin a xrow buffer transaction. This function may rotate the
 * last one data chunk and use the vclock parameter as a new chunk
 * starting vclock.
 */
void
xrow_buf_tx_begin(struct xrow_buf *xrow_buf, const struct vclock *vclock);

/** Discard all the data was written after the last transaction. */
void
xrow_buf_tx_rollback(struct xrow_buf *xrow_buf);

/** Commit a xrow buffer transaction. */
void
xrow_buf_tx_commit(struct xrow_buf *xrow_buf);

/**
 * Append an xrow array to a wal memory. The array is placed into
 * one xrow buffer data chunk and each row takes a continuous
 * space in a data buffer. Raw encoded data is placed onto
 * gc-allocated iovec array.
 *
 * @retval count of written iovec members for success
 * @retval -1 in case of error
 */
int
xrow_buf_write(struct xrow_buf *xrow_buf, struct xrow_header **begin,
	       struct xrow_header **end,
	       struct iovec **iovec);

/**
 * Xrow buffer cursor used to search a position in a buffer
 * and then fetch rows one by one from the postion toward the
 * buffer last append row.
 */
struct xrow_buf_cursor {
	/** Current chunk global index. */
	uint32_t chunk_index;
	/** Row index in the current chunk. */
	uint32_t row_index;
};

/**
 * Create a xrow buffer cursor and set it's position to
 * the first row after passed vclock value.
 *
 * @retval 0 cursor was created
 * @retval -1 if a vclock was discarded
 */
int
xrow_buf_cursor_create(struct xrow_buf *xrow_buf,
		       struct xrow_buf_cursor *xrow_buf_cursor,
		       struct vclock *vclock);

/**
 * Fetch next row from a xrow buffer cursor and return the row
 * header and encoded data pointers as well as encoded data size
 * in the corresponding parameters.
 *
 * @retval 0 in case of success
 * @retval 1 if there is no more rows in a buffer
 * @retval -1 if this cursor postion was discarded by xrow buffer
 */
int
xrow_buf_cursor_next(struct xrow_buf *xrow_buf,
		     struct xrow_buf_cursor *xrow_buf_cursor,
		     struct xrow_header **row, void **data, size_t *size);

#endif /* TARANTOOL_XROW_BUF_H_INCLUDED */
