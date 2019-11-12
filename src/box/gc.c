/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
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
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "gc.h"

#include <trivia/util.h>

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define RB_COMPACT 1
#include <small/rb.h>
#include <small/rlist.h>
#include <tarantool_ev.h>

#include "diag.h"
#include "errcode.h"
#include "fiber.h"
#include "fiber_cond.h"
#include "latch.h"
#include "say.h"
#include "vclock.h"
#include "cbus.h"
#include "engine.h"		/* engine_collect_garbage() */
#include "wal.h"		/* wal_collect_garbage() */
#include "checkpoint_schedule.h"

struct gc_state gc;

static int
gc_cleanup_fiber_f(va_list);
static int
gc_checkpoint_fiber_f(va_list);

/** Free a checkpoint object. */
static void
gc_checkpoint_delete(struct gc_checkpoint *checkpoint)
{
	TRASH(checkpoint);
	free(checkpoint);
}

void
gc_init(void)
{
	/* Don't delete any files until recovery is complete. */
	gc.min_checkpoint_count = INT_MAX;

	vclock_create(&gc.vclock);
	rlist_create(&gc.checkpoints);
	fiber_cond_create(&gc.cleanup_cond);
	checkpoint_schedule_cfg(&gc.checkpoint_schedule, 0, 0);
	engine_collect_garbage(&gc.vclock);

	gc.cleanup_fiber = fiber_new("gc", gc_cleanup_fiber_f);
	if (gc.cleanup_fiber == NULL)
		panic("failed to start garbage collection fiber");

	gc.checkpoint_fiber = fiber_new("checkpoint_daemon",
					gc_checkpoint_fiber_f);
	if (gc.checkpoint_fiber == NULL)
		panic("failed to start checkpoint daemon fiber");

	fiber_start(gc.cleanup_fiber);
	fiber_start(gc.checkpoint_fiber);
}

void
gc_free(void)
{
	/*
	 * Can't clear the WAL watcher as the event loop isn't
	 * running when this function is called.
	 */

	/* Free checkpoints. */
	struct gc_checkpoint *checkpoint, *next_checkpoint;
	rlist_foreach_entry_safe(checkpoint, &gc.checkpoints, in_checkpoints,
				 next_checkpoint) {
		gc_checkpoint_delete(checkpoint);
	}
}

/**
 * Invoke garbage collection in order to remove files left
 * from old checkpoints. The number of checkpoints saved by
 * this function is specified by box.cfg.checkpoint_count.
 */
static void
gc_run_cleanup(void)
{
	bool run_engine_gc = false;

	/*
	 * Find the oldest checkpoint that must be preserved.
	 * We have to preserve @min_checkpoint_count oldest
	 * checkpoints, plus we can't remove checkpoints that
	 * are still in use.
	 */
	struct gc_checkpoint *checkpoint = NULL, *tmp;
	rlist_foreach_entry_safe(checkpoint, &gc.checkpoints,
				 in_checkpoints, tmp) {
		if (checkpoint->is_join_readview)
			continue;
		if (gc.checkpoint_count <= gc.min_checkpoint_count)
			break;
		if (!rlist_empty(&checkpoint->refs))
			break; /* checkpoint is in use */
		rlist_del_entry(checkpoint, in_checkpoints);
		gc_checkpoint_delete(checkpoint);
		gc.checkpoint_count--;
		run_engine_gc = true;
	}

	/* At least one checkpoint must always be available. */
	assert(checkpoint != NULL);

	if (!run_engine_gc)
		return; /* nothing to do */

	/*
	 * Run garbage collection.
	 *
	 * It may occur that we proceed to deletion of WAL files
	 * and other engine files after having failed to delete
	 * a memtx snap file. If this happens, the corresponding
	 * checkpoint won't be removed from box.info.gc(), because
	 * we use snap files to build the checkpoint list, but
	 * it won't be possible to back it up or recover from it.
	 * This is OK as unlink() shouldn't normally fail. Besides
	 * we never remove the last checkpoint and the following
	 * WALs so this may only affect backup checkpoints.
	 */
	engine_collect_garbage(&checkpoint->vclock);
	checkpoint = rlist_first_entry(&gc.checkpoints,
					struct gc_checkpoint, in_checkpoints);
	wal_set_gc_first_vclock(&checkpoint->vclock);
}

static int
gc_cleanup_fiber_f(va_list ap)
{
	(void)ap;
	while (!fiber_is_cancelled()) {
		int delta = gc.cleanup_scheduled - gc.cleanup_completed;
		if (delta == 0) {
			/* No pending garbage collection. */
			fiber_sleep(TIMEOUT_INFINITY);
			continue;
		}
		assert(delta > 0);
		gc_run_cleanup();
		gc.cleanup_completed += delta;
		fiber_cond_signal(&gc.cleanup_cond);
	}
	return 0;
}

/**
 * Trigger asynchronous garbage collection.
 */
static void
gc_schedule_cleanup(void)
{
	/*
	 * Do not wake up the background fiber if it's executing
	 * the garbage collection procedure right now, because
	 * it may be waiting for a cbus message, which doesn't
	 * tolerate spurious wakeups. Just increment the counter
	 * then - it will rerun garbage collection as soon as
	 * the current round completes.
	 */
	if (gc.cleanup_scheduled++ == gc.cleanup_completed)
		fiber_wakeup(gc.cleanup_fiber);
}

/**
 * Wait for background garbage collection scheduled prior
 * to this point to complete.
 */
static void
gc_wait_cleanup(void)
{
	unsigned scheduled = gc.cleanup_scheduled;
	while (gc.cleanup_completed < scheduled)
		fiber_cond_wait(&gc.cleanup_cond);
}

void
gc_advance(const struct vclock *vclock)
{
	/*
	 * Bring the garbage collector up to date with the oldest
	 * wal xlog file.
	 */
	vclock_copy(&gc.vclock, vclock);
}

void
gc_set_min_checkpoint_count(int min_checkpoint_count)
{
	gc.min_checkpoint_count = min_checkpoint_count;
}

void
gc_set_checkpoint_interval(double interval)
{
	/*
	 * Reconfigure the schedule and wake up the checkpoint
	 * daemon so that it can readjust.
	 *
	 * Note, we must not wake up the checkpoint daemon fiber
	 * if it's waiting for checkpointing to complete, because
	 * checkpointing code doesn't tolerate spurious wakeups.
	 */
	checkpoint_schedule_cfg(&gc.checkpoint_schedule,
				ev_monotonic_now(loop()), interval);
	if (!gc.checkpoint_is_in_progress)
		fiber_wakeup(gc.checkpoint_fiber);
}

void
gc_add_checkpoint(const struct vclock *vclock)
{
	struct gc_checkpoint *last_checkpoint = gc_last_checkpoint();
	while (last_checkpoint != NULL && last_checkpoint->is_join_readview) {
		last_checkpoint = rlist_prev_entry(last_checkpoint,
						   in_checkpoints);
	}
	if (last_checkpoint != NULL &&
	    vclock_sum(&last_checkpoint->vclock) == vclock_sum(vclock)) {
		/*
		 * box.snapshot() doesn't create a new checkpoint
		 * if no rows has been written since the last one.
		 * Rerun the garbage collector in this case, just
		 * in case box.cfg.checkpoint_count has changed.
		 */
		gc_schedule_cleanup();
		return;
	}
	assert(last_checkpoint == NULL ||
	       vclock_sum(&last_checkpoint->vclock) < vclock_sum(vclock));

	struct gc_checkpoint *checkpoint = calloc(1, sizeof(*checkpoint));
	/*
	 * This function is called after a checkpoint is written
	 * to disk so it can't fail.
	 */
	if (checkpoint == NULL)
		panic("out of memory");

	if (rlist_empty(&gc.checkpoints))
		wal_set_gc_first_vclock(vclock);
	rlist_create(&checkpoint->refs);
	vclock_copy(&checkpoint->vclock, vclock);
	rlist_add_tail_entry(&gc.checkpoints, checkpoint, in_checkpoints);
	gc.checkpoint_count++;

	gc_schedule_cleanup();
}

void
gc_add_join_readview(const struct vclock *vclock)
{
	struct gc_checkpoint *checkpoint = calloc(1, sizeof(*checkpoint));
	/*
	 * It is not a fatal error if we could not prevent subsequent
	 * from clearance.
	 */
	if (checkpoint == NULL) {
		say_error("GC: could not add a join readview");
		return;
	}
	if (rlist_empty(&gc.checkpoints))
		wal_set_gc_first_vclock(vclock);
	checkpoint->is_join_readview = true;
	rlist_create(&checkpoint->refs);
	vclock_copy(&checkpoint->vclock, vclock);
	rlist_add_tail_entry(&gc.checkpoints, checkpoint, in_checkpoints);
}

void
gc_del_join_readview(const struct vclock *vclock)
{
	struct gc_checkpoint *checkpoint;
	rlist_foreach_entry(checkpoint, &gc.checkpoints, in_checkpoints) {
		if (!checkpoint->is_join_readview ||
		    vclock_compare(&checkpoint->vclock, vclock) != 0)
			continue;
		rlist_del(&checkpoint->in_checkpoints);
		free(checkpoint);
		checkpoint = rlist_first_entry(&gc.checkpoints,
					       struct gc_checkpoint,
					       in_checkpoints);
		wal_set_gc_first_vclock(&checkpoint->vclock);
		return;
	}
	/* A join readview was not found. */
	say_error("GC: could del a join readview");
}


static int
gc_do_checkpoint(void)
{
	int rc;
	struct wal_checkpoint checkpoint;

	assert(!gc.checkpoint_is_in_progress);
	gc.checkpoint_is_in_progress = true;

	/*
	 * Rotate WAL and call engine callbacks to create a checkpoint
	 * on disk for each registered engine.
	 */
	rc = engine_begin_checkpoint();
	if (rc != 0)
		goto out;
	rc = wal_begin_checkpoint(&checkpoint);
	if (rc != 0)
		goto out;
	rc = engine_commit_checkpoint(&checkpoint.vclock);
	if (rc != 0)
		goto out;
	wal_commit_checkpoint(&checkpoint);

	/*
	 * Finally, track the newly created checkpoint in the garbage
	 * collector state.
	 */
	gc_add_checkpoint(&checkpoint.vclock);
out:
	if (rc != 0)
		engine_abort_checkpoint();

	gc.checkpoint_is_in_progress = false;
	return rc;
}

int
gc_checkpoint(void)
{
	if (gc.checkpoint_is_in_progress) {
		diag_set(ClientError, ER_CHECKPOINT_IN_PROGRESS);
		return -1;
	}

	/*
	 * Since a user invoked a snapshot manually, this may be
	 * because he may be not happy with the current randomized
	 * schedule. Randomize the schedule again and wake up the
	 * checkpoint daemon so that it * can readjust.
	 * It is also a good idea to randomize the interval, since
	 * otherwise many instances running on the same host will
	 * no longer run their checkpoints randomly after
	 * a sweeping box.snapshot() (gh-4432).
	 */
	checkpoint_schedule_cfg(&gc.checkpoint_schedule,
				ev_monotonic_now(loop()),
				gc.checkpoint_schedule.interval);
	fiber_wakeup(gc.checkpoint_fiber);

	if (gc_do_checkpoint() != 0)
		return -1;

	/*
	 * Wait for background garbage collection that might
	 * have been triggered by this checkpoint to complete.
	 * Strictly speaking, it isn't necessary, but it
	 * simplifies testing as it guarantees that by the
	 * time box.snapshot() returns, all outdated checkpoint
	 * files have been removed.
	 */
	gc_wait_cleanup();
	return 0;
}

void
gc_trigger_checkpoint(void)
{
	if (gc.checkpoint_is_in_progress || gc.checkpoint_is_pending)
		return;

	gc.checkpoint_is_pending = true;
	checkpoint_schedule_reset(&gc.checkpoint_schedule,
				  ev_monotonic_now(loop()));
	fiber_wakeup(gc.checkpoint_fiber);
}

static int
gc_checkpoint_fiber_f(va_list ap)
{
	(void)ap;

	/*
	 * Make the fiber non-cancellable so as not to bother
	 * about spurious wakeups.
	 */
	fiber_set_cancellable(false);

	struct checkpoint_schedule *sched = &gc.checkpoint_schedule;
	while (!fiber_is_cancelled()) {
		double timeout = checkpoint_schedule_timeout(sched,
					ev_monotonic_now(loop()));
		if (timeout > 0) {
			char buf[128];
			struct tm tm;
			time_t time = (time_t)(ev_now(loop()) + timeout);
			localtime_r(&time, &tm);
			strftime(buf, sizeof(buf), "%c", &tm);
			say_info("scheduled next checkpoint for %s", buf);
		} else {
			/* Periodic checkpointing is disabled. */
			timeout = TIMEOUT_INFINITY;
		}
		if (!fiber_yield_timeout(timeout) &&
		    !gc.checkpoint_is_pending) {
			/*
			 * The checkpoint schedule has changed.
			 * Reschedule the next checkpoint.
			 */
			continue;
		}
		/* Time to make the next scheduled checkpoint. */
		gc.checkpoint_is_pending = false;
		if (gc.checkpoint_is_in_progress) {
			/*
			 * Another fiber is making a checkpoint.
			 * Skip this one.
			 */
			continue;
		}
		if (gc_do_checkpoint() != 0)
			diag_log();
	}
	return 0;
}

void
gc_ref_checkpoint(struct gc_checkpoint *checkpoint,
		  struct gc_checkpoint_ref *ref, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vsnprintf(ref->name, GC_NAME_MAX, format, ap);
	va_end(ap);

	rlist_add_tail_entry(&checkpoint->refs, ref, in_refs);
}

void
gc_unref_checkpoint(struct gc_checkpoint_ref *ref)
{
	rlist_del_entry(ref, in_refs);
	gc_schedule_cleanup();
}
