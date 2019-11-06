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

#include "xrow_buf.h"
#include "fiber.h"

/* Xrow buffer chunk options (empirical values). */
enum {
	/* Chunk row info array capacity increment */
	XROW_BUF_CHUNK_CAPACITY_INCREMENT = 16384,
	/* Initial size for raw data storage. */
	XROW_BUF_CHUNK_INITIAL_DATA_SIZE = 65536,
	/* How many rows we will place in one buffer. */
	XROW_BUF_CHUNK_ROW_COUNT_THRESHOLD = 8192,
	/* How many data we will place in one buffer. */
	XROW_BUF_CHUNK_DATA_SIZE_THRESHOLD = 1 << 19,
};


/*
 * Save the current xrow buffer chunk state wich consists of two
 * values index and position where the next row header and raw data
 * would be placed. This state is used to track the next
 * transaction starting boundary.
 */
static inline void
xrow_buf_save_state(struct xrow_buf *xrow_buf)
{
	struct xrow_buf_chunk *chunk = xrow_buf->chunk +
		xrow_buf->last_chunk_index % XROW_BUF_CHUNK_COUNT;
	/* Save the current xrow buffer state. */
	xrow_buf->tx_first_row_index = chunk->row_count;
	xrow_buf->tx_first_row_svp = obuf_create_svp(&chunk->data);
}

void
xrow_buf_create(struct xrow_buf *xrow_buf)
{
	for (int i = 0; i < XROW_BUF_CHUNK_COUNT; ++i) {
		xrow_buf->chunk[i].row_info = NULL;
		xrow_buf->chunk[i].row_info_capacity = 0;
		xrow_buf->chunk[i].row_count = 0;
		obuf_create(&xrow_buf->chunk[i].data, &cord()->slabc,
			    XROW_BUF_CHUNK_INITIAL_DATA_SIZE);
	}
	xrow_buf->last_chunk_index = 0;
	xrow_buf->first_chunk_index = 0;
	xrow_buf_save_state(xrow_buf);
}

void
xrow_buf_destroy(struct xrow_buf *xrow_buf)
{
	for (int i = 0; i < XROW_BUF_CHUNK_COUNT; ++i) {
		if (xrow_buf->chunk[i].row_info_capacity > 0)
			slab_put(&cord()->slabc,
				 slab_from_data(xrow_buf->chunk[i].row_info));
		obuf_destroy(&xrow_buf->chunk[i].data);
	}
}

/*
 * If the current chunk data limits were reached then this function
 * swithes a xrow buffer to the next chunk. If there is no free
 * chunks in a xrow_buffer ring then the oldest one is going
 * to be truncated, after truncate it will be reused to store new data.
 */
static struct xrow_buf_chunk *
xrow_buf_rotate(struct xrow_buf *xrow_buf)
{
	struct xrow_buf_chunk *chunk = xrow_buf->chunk +
		xrow_buf->last_chunk_index % XROW_BUF_CHUNK_COUNT;
	/* Check if the current chunk could accept new data. */
	if (chunk->row_count < XROW_BUF_CHUNK_ROW_COUNT_THRESHOLD &&
	    obuf_size(&chunk->data) < XROW_BUF_CHUNK_DATA_SIZE_THRESHOLD)
		return chunk;

	/*
	 * Increase the last chunk generation and fetch
	 * corresponding chunk from the ring buffer.
	 */
	++xrow_buf->last_chunk_index;
	chunk = xrow_buf->chunk + xrow_buf->last_chunk_index %
				  XROW_BUF_CHUNK_COUNT;
	/*
	 * Check if the next chunk has data and discard
	 * the data if required.
	 */
	if (xrow_buf->last_chunk_index - xrow_buf->first_chunk_index >=
	    XROW_BUF_CHUNK_COUNT) {
		chunk->row_count = 0;
		obuf_reset(&chunk->data);
		++xrow_buf->first_chunk_index;
	}
	/*
	 * The xrow_buffer current chunk was changed so update
	 * the xrow buffer state.
	 */
	xrow_buf_save_state(xrow_buf);
	return chunk;
}

void
xrow_buf_tx_begin(struct xrow_buf *xrow_buf, const struct vclock *vclock)
{
	/*
	 * Xrow buffer fits a transaction in one chunk and does not
	 * chunk rotation while transaction is in progress. So check
	 * current chunk thresholds and rotate if required.
	 */
	struct xrow_buf_chunk *chunk = xrow_buf_rotate(xrow_buf);
	/*
	 * Check if the current chunk is empty and a vclock for
	 * the chunk should be set.
	 */
	if (chunk->row_count == 0)
		vclock_copy(&chunk->vclock, vclock);
}

void
xrow_buf_tx_rollback(struct xrow_buf *xrow_buf)
{
	struct xrow_buf_chunk *chunk = xrow_buf->chunk +
		xrow_buf->last_chunk_index % XROW_BUF_CHUNK_COUNT;
	chunk->row_count = xrow_buf->tx_first_row_index;
	obuf_rollback_to_svp(&chunk->data, &xrow_buf->tx_first_row_svp);
}

void
xrow_buf_tx_commit(struct xrow_buf *xrow_buf)
{
	/* Save the current xrow buffer state. */
	xrow_buf_save_state(xrow_buf);
}

int
xrow_buf_write(struct xrow_buf *xrow_buf, struct xrow_header **begin,
	       struct xrow_header **end, struct iovec **iovec)
{
	struct xrow_buf_chunk *chunk = xrow_buf->chunk +
		xrow_buf->last_chunk_index % XROW_BUF_CHUNK_COUNT;

	/* Save a data buffer svp to restore the buffer in case of an error. */
	struct obuf_svp data_svp = obuf_create_svp(&chunk->data);

	size_t row_count = chunk->row_count + (end - begin);
	/* Allocate space for new row information members if required. */
	if (row_count > chunk->row_info_capacity) {
		/* Round allocation up to XROW_BUF_CHUNK_CAPACITY_INCREMENT. */
		uint32_t capacity = XROW_BUF_CHUNK_CAPACITY_INCREMENT *
				    ((row_count +
				      XROW_BUF_CHUNK_CAPACITY_INCREMENT - 1) /
				     XROW_BUF_CHUNK_CAPACITY_INCREMENT);

		struct slab *row_info_slab =
			slab_get(&cord()->slabc,
				 sizeof(struct xrow_buf_row_info) * capacity);
		if (row_info_slab == NULL) {
			diag_set(OutOfMemory, capacity *
					      sizeof(struct xrow_buf_row_info),
				 "region", "row info array");
			goto error;
		}
		if (chunk->row_info_capacity > 0) {
			memcpy(slab_data(row_info_slab), chunk->row_info,
			       sizeof(struct xrow_buf_row_info) *
			       chunk->row_count);
			slab_put(&cord()->slabc,
				 slab_from_data(chunk->row_info));
		}
		chunk->row_info = slab_data(row_info_slab);
		chunk->row_info_capacity = capacity;
	}

	/* Encode rows. */
	for (struct xrow_header **row = begin; row < end; ++row) {
		/* Reserve space for raw encoded data. */
		char *data = obuf_reserve(&chunk->data, xrow_approx_len(*row));
		if (data == NULL) {
			diag_set(OutOfMemory, xrow_approx_len(*row),
				 "region", "wal memory data");
			goto error;
		}

		/*
		 * Xrow header itself is going to be encoded onto a gc
		 * memory region and the first member of a resulting
		 * iovec points to this data. Row bodies are going
		 * to be attached to the resulting iovec consequently.
		 */
		struct iovec iov[XROW_BODY_IOVMAX];
		int iov_cnt = xrow_header_encode(*row, 0, iov, 0);
		if (iov_cnt < 0)
			goto error;

		/*
		 * Now we have xrow header encoded representation
		 * so place it onto chunk data buffer starting
		 * from xrow header and then bodies.
		 */
		data = obuf_alloc(&chunk->data, iov[0].iov_len);
		memcpy(data, iov[0].iov_base, iov[0].iov_len);
		/*
		 * Initialize row info from xrow header and
		 * the row header encoded data location.
		 */
		struct xrow_buf_row_info *row_info =
			chunk->row_info + chunk->row_count + (row - begin);
		row_info->xrow = **row;
		row_info->data = data;
		row_info->size = iov[0].iov_len;

		for (int i = 1; i < iov_cnt; ++i) {
			data = obuf_alloc(&chunk->data, iov[i].iov_len);
			memcpy(data, iov[i].iov_base, iov[i].iov_len);
			/*
			 * Adjust stored row body location as we just
			 * copied it to a chunk data buffer.
			 */
			row_info->xrow.body[i - 1].iov_base = data;
			row_info->size += iov[i].iov_len;
		}
	}

	/* Return an iovec which points to the encoded data. */
	int iov_cnt = 1 + obuf_iovcnt(&chunk->data) - data_svp.pos;
	*iovec = region_alloc(&fiber()->gc, sizeof(struct iovec) * iov_cnt);
	if (*iovec == NULL) {
		diag_set(OutOfMemory, sizeof(struct iovec) * iov_cnt,
			 "region", "xrow_buf iovec");
		goto error;
	}
	memcpy(*iovec, chunk->data.iov + data_svp.pos,
	       sizeof(struct iovec) * iov_cnt);
	/* Adjust first iovec member to data starting location. */
	(*iovec)[0].iov_base += data_svp.iov_len;
	(*iovec)[0].iov_len -= data_svp.iov_len;

	/* Update chunk row count. */
	chunk->row_count = row_count;
	return iov_cnt;

error:
	/* Restore data buffer state. */
	obuf_rollback_to_svp(&chunk->data, &data_svp);
	return -1;
}

/*
 * Returns an index of the first row after given vclock
 * in a chunk.
 */
static int
xrow_buf_chunk_locate_vclock(struct xrow_buf_chunk *chunk,
			     struct vclock *vclock)
{
	for (uint32_t row_index = 0; row_index < chunk->row_count;
	     ++row_index) {
		struct xrow_header *row = &chunk->row_info[row_index].xrow;
		if (vclock_get(vclock, row->replica_id) < row->lsn)
			return row_index;
	}
	/*
	 * We did not find any row with vclock not less than
	 * given one so return an index just after the last one.
	 */
	return chunk->row_count;
}

int
xrow_buf_cursor_create(struct xrow_buf *xrow_buf,
		       struct xrow_buf_cursor *xrow_buf_cursor,
		       struct vclock *vclock)
{
	/* Check if a buffer has requested data. */
	struct xrow_buf_chunk *chunk =
			xrow_buf->chunk + xrow_buf->first_chunk_index %
					  XROW_BUF_CHUNK_COUNT;
	int rc = vclock_compare(&chunk->vclock, vclock);
	if (rc > 0 || rc == VCLOCK_ORDER_UNDEFINED) {
		/* The requested data was discarded. */
		return -1;
	}
	uint32_t index = xrow_buf->first_chunk_index;
	while (index < xrow_buf->last_chunk_index) {
		chunk = xrow_buf->chunk + (index + 1) % XROW_BUF_CHUNK_COUNT;
		int rc = vclock_compare(&chunk->vclock, vclock);
		if (rc > 0 || rc == VCLOCK_ORDER_UNDEFINED) {
			/* Next chunk has younger rows than requested vclock. */
			break;
		}
		++index;
	}
	chunk = xrow_buf->chunk + (index) % XROW_BUF_CHUNK_COUNT;
	xrow_buf_cursor->chunk_index = index;
	xrow_buf_cursor->row_index = xrow_buf_chunk_locate_vclock(chunk, vclock);
	return 0;
}

int
xrow_buf_cursor_next(struct xrow_buf *xrow_buf,
		     struct xrow_buf_cursor *xrow_buf_cursor,
		     struct xrow_header **row, void **data, size_t *size)
{
	if (xrow_buf->first_chunk_index > xrow_buf_cursor->chunk_index) {
		/* A cursor current chunk was discarded by a buffer. */
		return -1;
	}

	struct xrow_buf_chunk *chunk;
next_chunk:
	chunk = xrow_buf->chunk + xrow_buf_cursor->chunk_index %
				  XROW_BUF_CHUNK_COUNT;
	size_t chunk_row_count = chunk->row_count;
	if (chunk_row_count == xrow_buf_cursor->row_index) {
		/*
		 * No more rows in a buffer but there are two
		 * possibilities:
		 *  1. A cursor current chunk is the last one and there is
		 * no more rows in the cursor.
		 *  2. There is a chunk after the current one
		 * so we can switch to it.
		 * */
		if (xrow_buf->last_chunk_index ==
		    xrow_buf_cursor->chunk_index) {
			/*
			 * The current chunk is the last one -
			 * no more rows in a buffer.
			 */
			return 1;
		}
		/* Switch to the next chunk. */
		xrow_buf_cursor->row_index = 0;
		++xrow_buf_cursor->chunk_index;
		goto next_chunk;
	}
	/* Return row header and data pointers and data size. */
	struct xrow_buf_row_info *row_info = chunk->row_info +
					     xrow_buf_cursor->row_index;
	*row = &row_info->xrow;
	*data = row_info->data;
	*size = row_info->size;
	++xrow_buf_cursor->row_index;
	return 0;
}
