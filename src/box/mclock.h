#ifndef INCLUDES_TARANTOOL_MCLOCK_H
#define INCLUDES_TARANTOOL_MCLOCK_H
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
#include <stdlib.h>

#include "vclock.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/**
 * Matrix clock structure contains vclocks identified
 * by replica identifiers int it rows and maintains
 * the column order for each known replica identifier.
 */
struct mclock {
	/** Map of attached replica vclocks. */
	unsigned int row_map;
	/** Map of known replica identifies. */
	unsigned int col_map;
	/**
	 * Contained vclock array addressed by
	 * corresponding replica identifier.
	 */
	struct vclock vclock[VCLOCK_MAX];
	/**
	 * Per column ordered map. Each row describes
	 * an ordered array of attached replica identifiers
	 * where the most bigger lsn is on the first position.
	 * In case of sequence of the equal lsns the latest is
	 * on the last position in the sequence.
	 * For instance if we have vclock like:
	 *  1: {1: 10, 2: 12, 3: 0}
	 *  2: {1: 10, 2: 14, 3: 1} -- updated after the first one
	 *  3: {1: 0, 2: 8, 3: 4}
	 * The order array will look like:
	 *  {{1, 2, 3}, {2, 1, 3}, {3, 2, 1}}
	 */
	uint8_t order[VCLOCK_MAX][VCLOCK_MAX];
};

/** Create a mclock structure. */
void
mclock_create(struct mclock *mclock);

/** Release allocated resources. */
void
mclock_destroy(struct mclock *mclock);

/**
 * Update a vclock identified by replica id and
 * reorders mclock members.
 */
int
mclock_update(struct mclock *mclock, uint32_t id, const struct vclock *vclock);

/**
 * Build a vclock which is less or equal than offset + 1
 * (or count + offset + 1 if offset < 0) mclock contained
 * vclock members.
 * For instance if we have vclock like:
 *  1: {1: 10, 2: 12, 3: 0}
 *  2: {1: 10, 2: 14, 3: 1} -- updated after the first one
 *  3: {1: 0, 2: 8, 3: 4}
 * Then mclock_get(1) builds {1: 10, 2: 14, 3: 4}
 * whereas  mclock_get(2) build {1: 10, 2: 12, 3: 1}
 */
int
mclock_get(struct mclock *mclock, int32_t offset, struct vclock *vclock);

/**
 * Extract a row from a matric clock (an instance vclock).
 */
int
mclock_extract_row(struct mclock *mclock, uint32_t id, struct vclock *vclock);

/**
 * Extract a column from a matrix clock. Such collumn descibes
 * the corresponding instance state accros a cluster.
 */
int
mclock_extract_col(struct mclock *mclock, uint32_t id,  struct vclock *vclock);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_MCLOCK_H */
