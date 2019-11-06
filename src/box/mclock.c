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
#include "mclock.h"

void
mclock_create(struct mclock *mclock)
{
	memset(mclock, 0, sizeof(struct mclock));
}

void
mclock_destroy(struct mclock *mclock)
{
	memset(mclock, 0, sizeof(struct mclock));
}

/*
 * Rebuild an order map if there are new
 * replica identifiers going to be added.
 */
static void
mclock_adjust_col_map(struct mclock *mclock, uint32_t id,
		      const struct vclock *vclock)
{
	/* Eveluate new matrix column identifiers. */
	uint32_t new_col_map = vclock->map & ~mclock->col_map;
	struct bit_iterator col_map_it;
	bit_iterator_init(&col_map_it, &new_col_map, sizeof(new_col_map), true);
	for (size_t col_id = bit_iterator_next(&col_map_it); col_id < SIZE_MAX;
	     col_id = bit_iterator_next(&col_map_it)) {
		/* Register new replica idnetifier. */
		mclock->col_map |= (1 << col_id);
		struct bit_iterator row_map_it;
		bit_iterator_init(&row_map_it, &mclock->row_map,
				  sizeof(mclock->row_map), true);
		/* Rebuild an order map for given column. */
		mclock->order[col_id][0] = id;
		for (size_t row_id = bit_iterator_next(&row_map_it), i = 1;
		     row_id < SIZE_MAX;
		     row_id = bit_iterator_next(&row_map_it)) {
			if (row_id != id)
				mclock->order[col_id][i++] = row_id;
		}
	}
}

/* Fetches a lsn on given column and position. */
static inline int64_t
mclock_get_pos_lsn(const struct mclock *mclock, uint32_t col_id, uint32_t pos)
{
	uint32_t row_id = mclock->order[col_id][pos];
	return vclock_get(mclock->vclock + row_id, col_id);
}

/*
 * Locate a range which contains given lsn for given column
 * identifier. Function return two values by pointes: to and from.
 * The first one contain the less position which lsn greather or
 * equal than given lsn, the second one contains the bigger position
 * which lsn less than the given one. So lsns on postion from *from
 * to *to -1 are equal with given lsn.
 * For instance if we have lsn array like {12, 10, 10, 7, 6}
 * then for lsn == 10 we will get *from == 1 and *to == 3
 * whereas for lsn == 8 the result should be: *from = 3 and *to == 3
 */
static inline void
mclock_find_range(const struct mclock *mclock, uint32_t col_id, int64_t lsn,
		  uint32_t *from, uint32_t *to)
{
	/* Logarithic search, setup initial search ranges. */
	uint32_t b = *from, e = *to;
	uint32_t b_to = *from, e_to = *to;
	/* Look for `from' position. */
	while (e - b > 1) {
		uint32_t m = (b + e) / 2;
		int64_t m_lsn = mclock_get_pos_lsn(mclock, col_id, m);
		if (m_lsn <= lsn)
			e = m;
		else
			b = m;
		/*
		 * Optimizarion: check if we could decrease
		 * the `to' search range.
		 */
		if (m_lsn < lsn)
			e_to = MIN(m, e_to);
		else
			b_to = MAX(m, b_to);
	}
	if (mclock_get_pos_lsn(mclock, col_id, b) > lsn)
		*from = e;
	else
		*from = b;
	/* Look for `to' position. */
	while (e_to - b_to > 1) {
		uint32_t m = (b_to + e_to) / 2;
		int64_t m_lsn = mclock_get_pos_lsn(mclock, col_id, m);
		if (m_lsn < lsn)
			e_to = m;
		else
			b_to = m;
	}
	*to = e_to;
}

/*
 * Update replica id vclock and reorder mclock members. */
int
mclock_update_vclock(struct mclock *mclock, uint32_t id, const struct vclock *vclock)
{
	uint32_t count = __builtin_popcount(mclock->row_map);
	mclock_adjust_col_map(mclock, id, vclock);
	/* Perform reordering for each column. */
	struct bit_iterator col_map_it;
	bit_iterator_init(&col_map_it, &mclock->col_map, sizeof(mclock->col_map), true);
	for (size_t col_id = bit_iterator_next(&col_map_it); col_id < SIZE_MAX;
	     col_id = bit_iterator_next(&col_map_it)) {
		int64_t new_lsn = vclock_get(vclock, col_id);
		int64_t old_lsn = vclock_get(mclock->vclock + id, col_id);
		/*
		 * Find a positions range which contains given
		 * replica id for current collumn (old lsn).
		 */
		uint32_t from = 0, to = count;
		mclock_find_range(mclock, col_id, old_lsn, &from, &to);
		assert(to > from);
		uint32_t old_pos = from, new_pos;
		while (old_pos < to) {
			uint32_t replica_id = mclock->order[col_id][old_pos];
			if (replica_id == id)
				break;
			++old_pos;
		}
		/* Replica id should be found. */
		assert(old_pos < to);
		if (new_lsn == old_lsn) {
			/*
			 * Lsn was not changed put replica id on the
			 * last position in corresponding range.
			 */
			new_pos = to - 1;
		}
		else if (new_lsn > mclock_get_pos_lsn(mclock, col_id, 0)) {
			/*
			 * New lsn is the biggest one so put on
			 * the first position in a column.
			 */
			new_pos = 0;
		}
		else if (new_lsn <= mclock_get_pos_lsn(mclock, col_id,
						       count - 1)) {
			/* The least one - the last postion. */
			new_pos = count - 1;
		}
		else {
			/* Find a range of position which contains new lsn. */
			if (new_lsn > old_lsn) {
				to = from;
				from = 0;
			} else {
				from = to;
				to = count;
			}
			mclock_find_range(mclock, col_id, new_lsn, &from, &to);
			/* Take care about positions shift - to the
			 * head or to the tail of collumn order map.
			 */
			new_pos = to - (new_lsn <= old_lsn? 1: 0);
		}
		assert(new_pos < count);
		if (old_pos == new_pos)
			continue;
		/* Shift members one step top or down. */
		if (old_pos > new_pos) {
			memmove(mclock->order[col_id] + new_pos + 1,
				mclock->order[col_id] + new_pos,
				(old_pos - new_pos) * sizeof(**mclock->order));
		} else if (old_pos < new_pos) {
			memmove(mclock->order[col_id] + old_pos,
				mclock->order[col_id] + old_pos + 1,
				(new_pos - old_pos) * sizeof(**mclock->order));
		}
		mclock->order[col_id][new_pos] = id;
	}
	vclock_copy(&mclock->vclock[id], vclock);
	return 0;
}

int
mclock_update(struct mclock *mclock, uint32_t id, const struct vclock *vclock)
{
	/*
	 * The given id is not registered and
	 * vclock is zero - nothing to do.
	 */
	if ((mclock->row_map & (1 << id)) == 0 && vclock_sum(vclock) == 0)
		return 0;
	/*
	 * The given replica id is not yet attached so
	 * put a zero vclock on the last position with
	 * corresponding replica identifier.
	 */
	if ((mclock->row_map & (1 << id)) == 0) {
		vclock_create(&mclock->vclock[id]);
		/* Put the given vclock at the last position. */
		mclock->row_map |= 1 << id;
		uint32_t count = __builtin_popcount(mclock->row_map);
		struct bit_iterator col_map_it;
		bit_iterator_init(&col_map_it, &mclock->col_map, sizeof(mclock->col_map), true);
		for (size_t col_id = bit_iterator_next(&col_map_it); col_id < SIZE_MAX;
		     col_id = bit_iterator_next(&col_map_it)) {
			mclock->order[col_id][count - 1] = id;
		}
	}
	mclock_update_vclock(mclock, id, vclock);
	/*
	 * If the given vclock is zero - remove them from the mclock.
	 * As the last updated member is put on the last position
	 * in an order map we could just truncate last member
	 * from each column order array.
	 */
	if (vclock_sum(vclock) == 0)
		mclock->row_map ^= (1 << id);

	{
		struct bit_iterator col_map_it;
		bit_iterator_init(&col_map_it, &mclock->col_map, sizeof(mclock->col_map), true);
		for (size_t col_id = bit_iterator_next(&col_map_it); col_id < SIZE_MAX;
		     col_id = bit_iterator_next(&col_map_it)) {
		}
	}
	return 0;
}

int
mclock_get(struct mclock *mclock, int32_t offset, struct vclock *vclock)
{
	int32_t count = __builtin_popcount(mclock->row_map);
	/* Check if given offset is out of mclock range. */
	if (offset >= count || offset < -count) {
		vclock_create(vclock);
		return -1;
	}
	offset = (offset + count) % count;
	vclock_create(vclock);
	/* Fetch lsn for each known replica identifier. */
	struct bit_iterator col_map_it;
	bit_iterator_init(&col_map_it, &mclock->col_map, sizeof(mclock->col_map), true);
	for (size_t col_id = bit_iterator_next(&col_map_it); col_id < SIZE_MAX;
	     col_id = bit_iterator_next(&col_map_it)) {
		int64_t lsn = mclock_get_pos_lsn(mclock, col_id, offset);
		if (lsn > 0)
			vclock_follow(vclock, col_id, lsn);
	}
	return 0;
}

int
mclock_extract_row(struct mclock *mclock, uint32_t id, struct vclock *vclock)
{
	if (mclock->row_map && (1 << id) == 0) {
		vclock_create(vclock);
		return -1;
	}
	vclock_copy(vclock, mclock->vclock + id);
	return 0;
}

int
mclock_extract_col(struct mclock *mclock, uint32_t id, struct vclock *vclock)
{
	vclock_create(vclock);
	if (mclock->col_map && (1 << id) == 0)
		return -1;

	struct bit_iterator row_map_it;
	bit_iterator_init(&row_map_it, &mclock->row_map,
			  sizeof(mclock->row_map), true);
	for (size_t row_id = bit_iterator_next(&row_map_it);
	     row_id < SIZE_MAX; row_id = bit_iterator_next(&row_map_it)) {
		int64_t lsn = vclock_get(mclock->vclock + row_id, id);
		if (lsn == 0)
			continue;
		vclock_follow(vclock, row_id, lsn);
	}

	return 0;
}
