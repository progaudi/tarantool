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
#include "relay.h"

#include "trivia/config.h"
#include "tt_static.h"
#include "scoped_guard.h"
#include "cbus.h"
#include "cfg.h"
#include "errinj.h"
#include "fiber.h"
#include "say.h"

#include "coio.h"
#include "coio_task.h"
#include "engine.h"
#include "gc.h"
#include "iproto_constants.h"
#include "replication.h"
#include "trigger.h"
#include "vclock.h"
#include "version.h"
#include "xrow.h"
#include "xrow_io.h"
#include "xstream.h"
#include "wal.h"

/** State of a replication relay. */
struct relay {
	/** The thread in which we relay data to the replica. */
	struct cord cord;
	/** Replica connection */
	struct ev_io io;
	/** Request sync */
	uint64_t sync;
	/** Xstream argument to recovery */
	struct xstream stream;
	/** Vclock to stop playing xlogs */
	struct vclock stop_vclock;
	/** Remote replica */
	struct replica *replica;
	/** WAL memory relay. */
	struct wal_relay wal_relay;
	/** Relay diagnostics. */
	struct diag diag;
	/** Vclock recieved from replica. */
	struct vclock recv_vclock;
	/** Replicatoin slave version. */
	uint32_t version_id;
	/**
	 * Local vclock at the moment of subscribe, used to check
	 * dataset on the other side and send missing data rows if any.
	 */
	struct vclock local_vclock_at_subscribe;

	/** Relay sync state. */
	enum relay_state state;
};

struct diag*
relay_get_diag(struct relay *relay)
{
	return &relay->diag;
}

enum relay_state
relay_get_state(const struct relay *relay)
{
	return relay->state;
}

void
relay_vclock(const struct relay *relay, struct vclock *vclock)
{
	wal_relay_vclock(&relay->wal_relay, vclock);
}

double
relay_last_row_time(const struct relay *relay)
{
	return wal_relay_last_row_time(&relay->wal_relay);
}

static int
relay_send(struct relay *relay, struct xrow_header *packet);
static int
relay_send_initial_join_row(struct xstream *stream, struct xrow_header *row);

struct relay *
relay_new(struct replica *replica)
{
	struct relay *relay = (struct relay *) calloc(1, sizeof(struct relay));
	if (relay == NULL) {
		diag_set(OutOfMemory, sizeof(struct relay), "malloc",
			  "struct relay");
		return NULL;
	}
	relay->replica = replica;
	diag_create(&relay->diag);
	relay->state = RELAY_OFF;
	return relay;
}

static void
relay_start(struct relay *relay, int fd, uint64_t sync)
{
	/*
	 * Clear the diagnostics at start, in case it has the old
	 * error message which we keep around to display in
	 * box.info.replication.
	 */
	diag_clear(&relay->diag);
	coio_create(&relay->io, fd);
	relay->sync = sync;
	relay->state = RELAY_FOLLOW;
}

void
relay_cancel(struct relay *relay)
{
	/* Check that the thread is running first. */
	if (relay->cord.id != 0) {
		if (tt_pthread_cancel(relay->cord.id) == ESRCH)
			return;
		tt_pthread_join(relay->cord.id, NULL);
	}
}

/**
 * Called by a relay thread right before termination.
 */
static void
relay_exit(struct relay *relay)
{
	(void) relay;
	struct errinj *inj = errinj(ERRINJ_RELAY_EXIT_DELAY, ERRINJ_DOUBLE);
	if (inj != NULL && inj->dparam > 0)
		fiber_sleep(inj->dparam);
}

static void
relay_stop(struct relay *relay)
{
	relay->state = RELAY_STOPPED;
	/*
	 * Needed to track whether relay thread is running or not
	 * for relay_cancel(). Id is reset to a positive value
	 * upon cord_create().
	 */
	relay->cord.id = 0;
}

void
relay_delete(struct relay *relay)
{
	if (relay->state == RELAY_FOLLOW)
		relay_stop(relay);
	diag_destroy(&relay->diag);
	TRASH(relay);
	free(relay);
}

void
relay_initial_join(int fd, uint64_t sync, struct vclock *vclock)
{
	struct relay *relay = relay_new(NULL);
	if (relay == NULL)
		diag_raise();

	relay_start(relay, fd, sync);
	auto relay_guard = make_scoped_guard([=] {
		relay_stop(relay);
		relay_delete(relay);
	});

	/* Freeze a read view in engines. */
	struct engine_join_ctx ctx;
	engine_prepare_join_xc(&ctx);
	auto join_guard = make_scoped_guard([&] {
		engine_complete_join(&ctx);
	});

	/*
	 * Sync WAL to make sure that all changes visible from
	 * the frozen read view are successfully committed.
	 */
	if (wal_sync() != 0)
		diag_raise();

	vclock_copy(vclock, &replicaset.vclock);

	/* Respond to the JOIN request with the current vclock. */
	struct xrow_header row;
	xrow_encode_vclock_xc(&row, vclock);
	row.sync = sync;
	if (coio_write_xrow(&relay->io, &row) < 0)
		diag_raise();

	xstream_create(&relay->stream, relay_send_initial_join_row);
	/* Send read view to the replica. */
	engine_join_xc(&ctx, &relay->stream);
}

/*
 * Filter callback function used by wal relay in order to
 * transform all local rows into a NOPs.
 */
static ssize_t
relay_final_join_filter(struct wal_relay *wal_relay, struct xrow_header **row)
{
	(void) wal_relay;
	ssize_t rc = WAL_RELAY_FILTER_PASS;
	struct errinj *inj = errinj(ERRINJ_RELAY_BREAK_LSN,
				    ERRINJ_INT);
	if (inj != NULL && (*row)->lsn == inj->iparam) {
		struct xrow_header *filtered_row = (struct xrow_header *)
			region_alloc(&fiber()->gc, sizeof(*filtered_row));
		if (filtered_row == NULL) {
			diag_set(OutOfMemory, sizeof(struct xrow_header),
				 "region", "struct xrow_header");
			return WAL_RELAY_FILTER_ERR;
		}
		*filtered_row = **row;
		filtered_row->lsn = inj->iparam - 1;
		say_warn("injected broken lsn: %lld",
			 (long long) filtered_row->lsn);
		*row = filtered_row;
		rc = WAL_RELAY_FILTER_ROW;
	}
	/*
	 * Transform replica local requests to IPROTO_NOP so as to
	 * promote vclock on the replica without actually modifying
	 * any data.
	 */
	if ((*row)->group_id == GROUP_LOCAL) {
		struct xrow_header *filtered_row = (struct xrow_header *)
			region_alloc(&fiber()->gc, sizeof(*filtered_row));
		if (filtered_row == NULL) {
			diag_set(OutOfMemory, sizeof(struct xrow_header),
				 "region", "struct xrow_header");
			return WAL_RELAY_FILTER_ERR;
		}
		*filtered_row = **row;
		filtered_row->type = IPROTO_NOP;
		filtered_row->group_id = GROUP_DEFAULT;
		filtered_row->bodycnt = 0;
		*row = filtered_row;
		rc = WAL_RELAY_FILTER_ROW;
	}
	return rc;
}

void
relay_final_join(int fd, uint64_t sync, struct vclock *start_vclock,
		 struct vclock *stop_vclock)
{
	struct relay *relay = relay_new(NULL);
	if (relay == NULL)
		diag_raise();

	relay_start(relay, fd, sync);
	auto relay_guard = make_scoped_guard([=] {
		relay_stop(relay);
		relay_delete(relay);
	});

	vclock_copy(&relay->stop_vclock, stop_vclock);

	if (wal_relay(&relay->wal_relay, start_vclock, stop_vclock,
		      relay_final_join_filter, fd, relay->replica) != 0)
		diag_raise();

	ERROR_INJECT(ERRINJ_RELAY_FINAL_JOIN,
		     tnt_raise(ClientError, ER_INJECTION, "relay final join"));

	ERROR_INJECT(ERRINJ_RELAY_FINAL_SLEEP, {
		while (vclock_compare(stop_vclock, &replicaset.vclock) == 0)
			fiber_sleep(0.001);
	});
}

static void
relay_set_error(struct relay *relay, struct error *e)
{
	/* Don't override existing error. */
	if (diag_is_empty(&relay->diag))
		diag_add_error(&relay->diag, e);
}

/*
 * Filter callback function used while subscribe phase.
 */
static ssize_t
relay_subscribe_filter(struct wal_relay *wal_relay, struct xrow_header **row)
{
	if ((*row)->type != IPROTO_OK) {
		assert(iproto_type_is_dml((*row)->type));
		/*
		 * Because of asynchronous replication both master
		 * and replica may have different transaction
		 * order in their logs. As we start relaying
		 * transactions from the first unknow one there
		 * could be some other already known by replica
		 * and there is no point to send them.
		 */
		if (vclock_get(&wal_relay->vclock, (*row)->replica_id) >=
		    (*row)->lsn)
			return WAL_RELAY_FILTER_SKIP;
	}
	ssize_t rc = WAL_RELAY_FILTER_PASS;

	struct errinj *inj = errinj(ERRINJ_RELAY_BREAK_LSN,
				    ERRINJ_INT);
	if (inj != NULL && (*row)->lsn == inj->iparam) {
		struct xrow_header *filtered_row = (struct xrow_header *)
			region_alloc(&fiber()->gc, sizeof(*filtered_row));
		if (filtered_row == NULL) {
			diag_set(OutOfMemory, sizeof(struct xrow_header),
				 "region", "struct xrow_header");
			return WAL_RELAY_FILTER_ERR;
		}
		*filtered_row = **row;
		filtered_row->lsn = inj->iparam - 1;
		say_warn("injected broken lsn: %lld",
			 (long long) filtered_row->lsn);
		*row = filtered_row;
		rc = WAL_RELAY_FILTER_ROW;
	}

	/*
	 * Transform replica local requests to IPROTO_NOP so as to
	 * promote vclock on the replica without actually modifying
	 * any data.
	 */
	if ((*row)->group_id == GROUP_LOCAL) {
		struct xrow_header *filtered_row = (struct xrow_header *)
			region_alloc(&fiber()->gc, sizeof(*filtered_row));
		if (filtered_row == NULL) {
			diag_set(OutOfMemory, sizeof(struct xrow_header),
				 "region", "struct xrow_header");
			return WAL_RELAY_FILTER_ERR;
		}
		*filtered_row = **row;
		filtered_row->type = IPROTO_NOP;
		filtered_row->group_id = GROUP_DEFAULT;
		filtered_row->bodycnt = 0;
		*row = filtered_row;
		rc = WAL_RELAY_FILTER_ROW;
	}
	/*
	 * We're feeding a WAL, thus responding to FINAL JOIN or SUBSCRIBE
	 * request. If this is FINAL JOIN (i.e. relay->replica is NULL),
	 * we must relay all rows, even those originating from the replica
	 * itself (there may be such rows if this is rebootstrap). If this
	 * SUBSCRIBE, only send a row if it is not from the same replica
	 * (i.e. don't send replica's own rows back) or if this row is
	 * missing on the other side (i.e. in case of sudden power-loss,
	 * data was not written to WAL, so remote master can't recover
	 * it). In the latter case packet's LSN is less than or equal to
	 * local master's LSN at the moment it received 'SUBSCRIBE' request.
	 */
	struct relay *relay = container_of(wal_relay, struct relay, wal_relay);
	if (wal_relay->replica == NULL ||
	    (*row)->replica_id != wal_relay->replica->id ||
	    (*row)->lsn <= vclock_get(&relay->local_vclock_at_subscribe,
				      (*row)->replica_id)) {
		return rc;
	}
	return WAL_RELAY_FILTER_SKIP;
}

/** Replication acceptor fiber handler. */
void
relay_subscribe(struct replica *replica, int fd, uint64_t sync,
		struct vclock *replica_clock, uint32_t replica_version_id)
{
	assert(replica->id != REPLICA_ID_NIL);
	struct relay *relay = replica->relay;
	assert(relay->state != RELAY_FOLLOW);

	relay_start(relay, fd, sync);
	auto relay_guard = make_scoped_guard([=] {
		relay_stop(relay);
		replica_on_relay_stop(replica);
	});

	vclock_copy(&relay->local_vclock_at_subscribe, &replicaset.vclock);
	relay->version_id = replica_version_id;

	if (wal_relay(&relay->wal_relay, replica_clock, NULL,
		      relay_subscribe_filter, fd, relay->replica) != 0)
		relay_set_error(relay, diag_last_error(&fiber()->diag));
	relay_exit(relay);
	diag_raise();
}

static int
relay_send(struct relay *relay, struct xrow_header *packet)
{
	ERROR_INJECT_YIELD(ERRINJ_RELAY_SEND_DELAY);

	packet->sync = relay->sync;
	if (coio_write_xrow(&relay->io, packet) < 0)
		return -1;
	fiber_gc();

	struct errinj *inj = errinj(ERRINJ_RELAY_TIMEOUT, ERRINJ_DOUBLE);
	if (inj != NULL && inj->dparam > 0)
		fiber_sleep(inj->dparam);
	return 0;
}

static int
relay_send_initial_join_row(struct xstream *stream, struct xrow_header *row)
{
	struct relay *relay = container_of(stream, struct relay, stream);
	/*
	 * Ignore replica local requests as we don't need to promote
	 * vclock while sending a snapshot.
	 */
	if (row->group_id != GROUP_LOCAL)
		return relay_send(relay, row);
	return 0;
}
