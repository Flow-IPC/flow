/* Flow
 * Copyright 2023 Akamai Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy
 * of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in
 * writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing
 * permissions and limitations under the License. */

/// @file
#pragma once

#include "flow/log/log.hpp"
#include "flow/log/detail/log_fwd.hpp"
#include "flow/async/single_thread_task_loop.hpp"
#include "flow/util/util_fwd.hpp"
#include <boost/asio.hpp>
#include <boost/move/unique_ptr.hpp>
#include <atomic>

namespace flow::log
{

// Types.

/**
 * An implementation of Logger that logs messages to a given file-system path but never blocks any logging thread
 * for file I/O; suitable for heavy-duty file logging.  Protects against garbling due to simultaneous logging from
 * multiple threads.
 *
 * For file logging, the two out-of-the-box `Logger`s currently suitable are this Async_file_logger and
 * Simple_ostream_logger.  Vaguely speaking, Simple_ostream_logger is suitable for console (`cout`, `cerr`) output;
 * and, in a pinch outside of a heavy-duty production/server environment, for file (`ofstream`) output.  For heavy-duty
 * file logging one should use this Async_file_logger.  The primary reason is performance; this is discussed
 * in the Logger class doc header; note Async_file_logger logs asynchronously.
 * A secondary reason is additional file-logging-specific utilities -- such as
 * rotation -- are now or in the future going to be in Async_file_logger, as its purpose is heavy-duty file logging
 * specifically.
 *
 * ### Throttling ###
 * By default this feature is disabled, but one can enable/disable/configure it at will via throttling_cfg() mutator.
 * For example a viable tactic is to call it once right after construction, before any logging via `*this`; then
 * call it subsequently in case of dynamic config update.
 *
 * Throttling deals with the following potential problem which can occur under very heavy do_log() throughput
 * (lots of calls being made per unit time, probably from many threads); in practice this would typically only happen
 * if one sets effective verbosity to Sev::S_TRACE or more-verbose -- as `S_INFO`-or-less-verbose means
 * should_log() should be preventing do_log() from being called frequently.  For example one might enable TRACE
 * logging temporarily in production to see some details, causing a heavy do_log() execution rate.  (That said, if
 * your code does INFO-level logging too often, it could happen then too.)
 *
 * Internally each do_log() call pushes this *log request* into a central (per `*this`) queue of log requests;
 * a single (per `*this`) background thread is always popping this queue ASAP, writing the message to the file,
 * etc., until the queue is emptied; then it sleeps until it becomes non-empty; and so on.  If many threads are
 * pushing messages faster than this thread can get through them (congestive collapse), then more and more RAM
 * is used to store the enqueued messages.  If the throughput doesn't decrease in time, letting this central thread
 * catch up, then the RAM use might cause swapping or out-of-memory.
 *
 * Throttling is a safeguard against this.  It works as follows.  There's a limit H
 * in bytes; a limit L (strictly smaller than H) in bytes.  We start at Not-Throttling state.  If the memory use M
 * grows beyond H, we enter Throttling state.  In this state, if M goes below L, we re-enter Not-Throttling state.
 * In other words beyond a certain total memory use, we throttle; then to exit this state total memory use has
 * to go down by quite a big amount before we un-throttle.  The idea is to encourage a sawtooth `/\/\/\` pattern
 * as opposed to more like `/^^^^^^`.
 *
 * If throttling is enabled during a particular call to should_log(), and if and only if should_log() would return
 * `true` based on considerations excluding the throttling feature, then:
 *
 *   - should_log() returns `true` in Not-Throttling state;
 *   - should_log() returns `false` in Throttling state;
 *
 * Since standard `FLOW_LOG_*` macros avoid do_log() (or even the evaluation of the message) if should_log()
 * returns `false` for a given log call site, during Throttling state enqueuing of messages is blocked (letting
 * the logging-to-file thread catch up).
 *
 * M starts at 0.  Each do_log() (queued-up log request) increments M based on the memory-use estimate of the
 * message+metadata passed to do_log(); then it enqueues the log request.  Each time the background thread
 * actually writes out the queued-up message+metadata, it decrements M by the same value by which it was
 * incremented in do_log().
 *
 * This algorithm (computing M via increments and decrements; setting state to Throttling or Not-Throttling)
 * is carried out at all times.  However, should_log() consults the state (Throttling versus Not-Throttling) if
 * and only if the throttling feature is enabled at that time.  If it is not, that state is simply ignored, and
 * should_log() only makes the usual `Config` verbosity check(s); if that results in `true` then the message
 * is enqueued.  Therefore one can enable throttling at any time and count on its having immediate
 * effect based on actual memory use at that time.
 *
 * The limits H and L can also be reconfigured at any time.  Essentially throttling_cfg() mutator takes
 * 2 orthogonal sets of info: 1, whether throttling is to be possible at all (whether Throttling versus Not-Throttling
 * affects should_log() from now on); and 2, the limits H and L which control the policy about setting
 * the state (Throttling versus Not-Throttling).  (2) affects the algorithm that computes that binary state; whereas
 * (1) affects whether that binary state actually controls whether to prevent logging to save memory or not.
 *
 * If H and/or L are modified, the binary state is reinitialized: it is set to Throttling if and only if
 * memory use M at that time exceeds H; else to Not-Throttling.  The state prior to the throttling_cfg() mutator call
 * does not matter in this situation; it is overwritten.  This avoids various annoying corner cases and ambiguities
 * around config updates.
 *
 * Lastly: Initially throttling is disabled, while certain default values of H and L are assumed.  Hence the above
 * algorithm is active but has no effect, unless you call throttling_cfg() mutator to make it have effect.
 * You may use throttling_cfg() accessor and throttling_active() to get a copy of the current config values.
 *
 * ### Thread safety ###
 * As noted above, simultaneous logging from multiple threads is safe from output corruption, in that
 * simultaneous do_log() calls for the same Logger targeting the same stream will log serially to each other.
 * However, if some other code or process writes to the same file, then all bets are off -- so don't.
 *
 * See thread safety notes and to-dos regarding #m_config in Simple_ostream_logger doc header.  These apply here also.
 *
 * throttling_cfg() mutator may not be called concurrently with itself or throttling_cfg() accessor or
 * throttling_active() on the same `*this`.  It may be called concurrently with other methods on the same `*this`, most
 * notably should_log() and do_log().
 *
 * There are no other mutable data (state), so that's that.
 *
 * @todo Lacking feature: Compress-as-you-log in Async_file_logger.  So, optionally, when characters are actually
 * written out to file-system, gzip/zip/whatever them instead of writing plain text.  (This is possible at least
 * for gzip.)  Background: It is common-place to compress a log file after it has been rotated (e.g., around rotation
 * time: F.log.1.gz -> F.log.2.gz, F.log -> F.log.1 -> F.log.1.gz). It is more space-efficient (at least), however,
 * to write to F.log.gz directly already in compressed form; then rotation requires only renaming (e.g.:
 * F.log.1.gz -> F.log.2.gz, F.log.gz [already gzipped from the start] -> F.log.1.gz).
 *
 * @internal
 * Implementation
 * --------------
 * The basic implementation is straightforward enough to be gleaned from reading the code and other comments.
 *
 * ### Throttling impl: The essential algorithm ###
 *
 * What bears discussion is the implementation of the throttling feature.  Read on if you have interest in that
 * specific topic.  If so please carefully read the public section above entitled Throttling; then come back here.
 *
 * The impl of throttling writes itself based on that description, if one is allowed to use a mutex.  Then it's all
 * pretty simple: There are 3-4 functions to worry about:
 *
 *   - should_log(sev, component): First compute `m_serial_logger->should_log(sev, component)`; usually that will
 *     return `false`, so we should too.  If it returns `true`, though, then we should apply the added test of
 *     whether throttling should make us return `false` after all.  So:
 *     - Lock mutex.
 *       - If #m_throttling_active is `false` (feature disabled) then return `true`.  Else:
 *       - If `m_throttling_now` is `false` (state is Not-Throttling) then return `true`.  Else:
 *       - Return `false`.  (Throttling feature enabled, and state is currently Not-Throttling.)
 *   - `do_log(metadata, msg)`:
 *     - Compute `C = mem_cost(msg)` which inlines to essentially `msg.size()` + some compile-time constant.
 *     - Let local `bool throttling_begins = false`.
 *     - Lock mutex.
 *       - Increment `m_pending_logs_sz` by `C`.  `m_pending_logs_sz` is called M in the earlier discussion: the
 *         memory use estimate of things do_log() has enqueued but `really_log()` (see just below) has not
 *         yet dequeued and logged to file.
 *       - If and only if `m_throttling_now` is `false`, and we just made `m_pending_logs_sz` go from
 *         `< m_throttling_cfg.m_hi_limit` (a/k/a H) to `>= m_throttling_cfg.m_hi_limit`, then
 *         set `m_throttling_now` to `true`; and set `throttling_begins = true`.
 *     - Enqueue the log-request:
 *       - Capture `metadata`; a copy of `msg`; and `throttling_begins`.
 *       - `m_async_worker.post()` the lambda which invokes `really_log()` with those 3 items as args.
 *   - `really_log(metadata, msg, throttling_begins)`:
 *     - `m_serial_logger->do_log(metadata, msg)`: write-out the actual message to the file.
 *     - If `throttling_begins == true`: via `m_serial_logger->do_log()` write-out a special message
 *       indicating that state has changed from Not-Throttling to Throttling due to mem-use passing ceiling H.
 *     - Compute `C = mem_cost(msg)` (same as in `do_log()` above).
 *     - Let local `bool throttling_ends = false`.
 *     - Lock mutex.
 *       - Decrement `m_pending_logs_sz` (a/k/a M) by `C`.
 *       - If and only if `m_throttling_now` is `true`, and we just made `m_pending_logs_sz` go from
 *         `>= m_throttling_cfg.m_lo_limit` (a/k/a L) to `< m_throttling_cfg.m_lo_limit`, then
 *         set `m_throttling_now` to `false`; and set `throttling_ends = true`.
 *     - If `throttling_ends == true`: via `m_serial_logger->do_log()` write-out a special message
 *         indicating that state has changed from Throttling to Not-Throttling due to mem-use passing floor L.
 *   - `throttling_cfg(active, cfg)` mutator:
 *     - Lock mutex.
 *       - Save args into #m_throttling_active and `m_throttling_cfg` respectively.
 *       - If the latter's values (H and/or L) changed:
 *         - Assign `m_throttling_now = (m_pending_logs_sz >= m_throttling_cfg.m_hi_limit)`.
 *           This reinitializes the state machine cleanly as promised in the class doc header.
 *
 * The mutex makes everything easy.  However the resulting perf is potentially unacceptable:
 *
 *  - should_log() is called *very* frequently possibly from *many* threads.
 *  - do_log() can be called frequently (though a good should_log() policy will avoid it unless verbose
 *    logging is enabled) and also from *many* threads.
 *  - `really_log()` is called from only one thread but a number of times ~equal to do_log() call count.
 *  - throttling_cfg() mutator is called rarely (seconds apart; probably minutes or even hours in most cases).
 *
 * All this is happening with potentially heavy concurrency between them.  Any throttling-related computational
 * overhead in each of the 3 functions should be kept minimal, at least as a % of the rest of the computation
 * occuring in that function; and lock contention should be kept minimal, most importantly in should_log()
 * and secondarily (but still importantly) in do_log().  (`really_log()` is in a background thread, so it's less
 * important in these sense; but still not nothing.)
 *
 * So we strive to get rid of the mutex.  That's where it gets complicated.  Or, rather, the code is fairly simple,
 * but convincing oneself that it is correct is difficult.  To wit:
 *
 * ### Throttling impl: The algorithm modified to become lock-free ###
 *
 * The easiest critical section to reduce is in should_log() and concerns access to #m_throttling_active.
 * Suppose we make it `atomic<bool>` instead of `bool` with mutex protection.  If we store with `relaxed` ordering
 * and load with `relaxed` ordering, and do so outside any mutex-lock: throttling_cfg() mutator does the quite-rare
 * storing; should_log() does the possibly-frequent (albeit gated by `m_serial_logger->should_log()` returning `true`
 * in the first palce) loading.  The relaxed order means at worst there's a bit of a delay for some threads
 * noticing the config change; so this or that thread might throttle or not-throttle a tiny bit of time after another:
 * it's absolutely nor a problem.  However there is zero penalty to a `relaxed` load of `atomic<bool>` compared to
 * simply accessing a `bool`.  Adding a `bool` check to should_log() is not nothing, but it's close.
 *
 * The next fairly easy subject is `m_throttling_now`.  There is exactly one consumer: should_log().  Again
 * let's replace `bool m_throttling_now` with `atomic<bool> m_throttling_now`; and load it with `releaxed` ordering
 * in should_log(), outside any mutex-lock section.  There are exactly 3 assigners: do_log() and `really_log()`;
 * they assign this when M goes up past H (assign `true`) or down past L (assign `false`);
 * and throttling_cfg() mutator (assign depending on where M is compared to the new H).  So let's assume -- and
 * we'll discuss the bejesus out of it below -- we ensure the assigning algorithm is made to work properly, meaning
 * `m_throttling_now` (Throttling versus Not-Throttling state) is made to work correctly within itself.
 * Then should_log() merely needs to read `m_throttling_now`, in case it is `true`, so it should return
 * `should_log() == false`.  Once again, if `relaxed` ordering causes some threads to "see" a new value
 * a little later than others, it is perfectly fine.  (Recall also that the 3 assignment events are hardly frequent:
 * only when passing H going up, passing L going down, and possibly in throttling_cfg() mutator call.)
 *
 * We've now reduced the algorithm to:
 *
 *   - should_log(sev, component):
 *     `return m_serial_logger->should_log(sev, component) && ((!m_throttling_active) || (!m_throttling_now));`.
 *     (The latter 2 `bools` shall be accessed via `.load(relaxed)` though.)
 *   - `do_log(metadata, msg)`:
 *     Same as before; except when assigning `m_throttling_now = true`, do so with `.store(true, relaxed)`.
 *   - `really_log(metadata, msg, throttling_begins)`:
 *     Same as before; except when assigning `m_throttling_now = false`, do so with `.store(false, relaxed)`.
 *   - `throttling_cfg(active, cfg)` mutator:
 *     - Save `m_throttling_active = active;`.
 *     - Otherwise same as before.
 *
 * That's already pretty good, because the really important (for perf) place, should_log(), now is lock-free
 * with the added overhead being 2 mere checks against zero; and even then only if the core `should_log()` yielded
 * `true` -- which usually it doesnt.
 *
 * However do_log() still has pretty thick mutex-locking.  This could actually be argued away as no big deal in
 * practice (if do_log() is called so much as to make this matter, then processor usage due to verbosity is already
 * far too high to be "saved")... but let's not.  Our goal is to make it all lock-free, so let's continue.
 *
 * First, though, let's assume temporarily that throttling_cfg() mutator is never called, once H and L have been
 * initially set.  We'll first make an algorithm that'll work given that simplification; then we'll modify
 * it further to account for it.  That leaves:
 *
 *   - `do_log(metadata, msg)`:
 *     - Compute `C = mem_cost(msg)` (add a few things including `msg.size()`).
 *     - Let local `bool throttling_begins = false`.
 *     - Lock mutex.
 *       - `m_pending_logs_sz += C`.
 *       - If and only if `m_throttling_now` is `false`, and we just made `m_pending_logs_sz` go from
 *         `< m_throttling_cfg.m_hi_limit` to `>= m_throttling_cfg.m_hi_limit`, then
 *         set `m_throttling_now` to `true`; and set `throttling_begins = true`.
 *     - Enqueue the log-request:
 *       - Capture `metadata`; a copy of `msg`; and `throttling_begins`.
 *       - `m_async_worker.post()` the lambda which invokes `really_log()` with those 3 items as args.
 *   - `really_log(metadata, msg, throttling_begins)`:
 *     - `m_serial_logger->do_log(metadata, msg)`.
 *     - If `throttling_begins == true`: via `m_serial_logger->do_log()` write-out:
 *       state has changed from Not-Throttling to Throttling due to mem-use passing ceiling H.
 *     - Compute `C = mem_cost(msg)`.
 *     - Let local `bool throttling_ends = false`.
 *     - Lock mutex.
 *       - `m_pending_logs_sz -= C`.
 *       - If and only if `m_throttling_now` is `true`, and we just made `m_pending_logs_sz` go from
 *         `>= m_throttling_cfg.m_lo_limit` to `< m_throttling_cfg.m_lo_limit`, then
 *         set `m_throttling_now` to `false`; and set `throttling_ends = true`.
 *     - If `throttling_ends == true`: via `m_serial_logger->do_log()` write-out:
 *       state has changed from Throttling to Not-Throttling due to mem-use passing floor L.
 *
 * So how to get rid of the mutex?  Answer:  Just do it.  Do not have any mutex, and do not lock it.
 * Everything else stays the same; except that `m_pending_logs_sz` and `m_throttling_now` become
 * `atomic<uint64_t>` and `atomic<bool>` respectively; and:
 *
 *   - `auto new_val = X += Y; auto prev_val = new_val - Y;` becomes:
 *     `auto prev_val = X.fetch_add(Y, relaxed); auto new_val = prev_val + Y;`.
 *   - `auto new_val = X -= Y; auto prev_val = new_val + Y;` becomes:
 *     `auto prev_val = X.fetch_sub(Y, relaxed); auto new_val = prev_val - Y;`.
 *   - `m_throttling_now = F` (where F is a `bool`) becomes `m_throttling_now.store(F, relaxed)`.
 *
 * First let's see if this is the added overhead on top of "Enqueue the log-request" is low enough to be considered
 * acceptable.  (Remember that this isn't a mere question of an absolute overhead but more importantly as a % of the
 * do_log() call *and* the assembly of `msg` and loading of `*metadata` to even pass-through to do_log() in the first
 * place.)  To enumerate this overhead:
 *
 *   - mem_cost().  This adds `msg.size()` (a `size_t` memory value) to a compile-time constant, more or less.
 *   - Increment M by that number (atomic relaxed `+=` with saving a `prev_val` and resulting `new_val`).
 *   - Compare `prev_val < H` and `new_val >= H`.
 *   - Set `bool throttling_begins` to `true` or `false` accordingly.
 *   - Capture `bool throttling_begins` in addition to the existing payload (2 pointers + 1 `size_t`) of the lambda.
 *
 * So it's 3-ish increments, 2-ish integer comparisons, copying a ~handful of scalars ~1x each.  Compare this overhead
 * to the "Enqueue the log-request" step alone: creating lambda object, `function<>` object, enqueue it to boost.asio
 * task queue -- copying `msg` in the process.  Now throw in the `ostream<<` manipulation needed to assemble `msg`;
 * the time spent heap-allocating and populating Msg_metadata.  And lastly remember that the should_log() mechanism
 * (without any throttling) is normally supposed to make do_log() calls so infrequent that the processor cycle cost
 * is small in the first place.  Conclusion: Yes, this overhead is acceptable.
 *
 * What about overhead in `really_log()`?  Firstly it doesn't matter even as much as it does in do_log(); but secondly
 * it is essentially equal to the overhead in do_log(); so if the latter is OK, then so is the former.
 *
 * So the perf is good.  But is the algorithm still *correct*?  Answer: ...Almost; here is how we know this.
 * This is probably the hardest part mentally.  The part that is absolutely correct is the movement of
 * `m_pending_logs_sz`: since `fetch_add/sub()` is atomic and involves both a read of the existing value and write
 * of the incremented/decremented result, we've ensured that:
 *
 *   - the integer goes through a total ordering of increment and decrement operations (no operations are "lost" or
 *     "duplicated"); and therefore
 *   - the integer goes through a total ordering of "crossed H going up" and "crossed L going down" events,
 *     exactly alternating: "crossed H up", "crossed L down", "crossed H up", "crossed L down", ....
 *
 * So, in particular, there will be a clean sequence of log lines showing those events.  (Even if some kind of
 * freaky reordering occurs in terms of which one is printed when, it does not matter.)  Most importantly this is
 * achieved at ~zero computational cost, since we are using `relaxed` ordering for all operations: the least strict
 * one.
 *
 * What does *not* work though?  It's this: In do_log() and `really_log()` respectively there's the part that says,
 * "if we crossed threshold, then set the throttling state Boolean accordingly."  Due to the distance between H and L,
 * at least, this will work fine in practice.  However, formally, it is possible that a crossed-H-up and crossed-L-down
 * events happen very close to each other in time.  Since there is no synchronization combining the inc/dec statement
 * and the following if-crossed-threshold-then-set-Boolean into one atomic statement, it is possible formally that
 * this would happen:
 *
 *   -# do_log() increments M past H.
 *   -# `really_log()` decrements M past L.
 *   -# `really_log()` sets state to Not-Throttling.
 *   -# do_log() sets state to Throttling.
 *
 * This would be bad; `really_log()` should "win in the end" and end up in Not-Throttling.  should_log() would then
 * malfunction for potentially a long time.
 *
 * There is however a quite small modification to the algorithm which would solve this.  It's based on the same
 * principle that allowed us to make a clean ordering of how M changes values.  Instead of `bool`, make
 * `m_throttling_now` an `unsigned int` (interpreted by should_log() as a Boolean still, so no change there);
 * when do_log() crosses H going up, perform a `fetch_xor(true, relaxed)` instead of assigning `true`;
 * when `really_log()` crosses L going down, perform a `fetch_xor(true, relaxed)` instead of assigning `false`.
 * Or, in English, *flip the Boolean* in both cases.  So even in the pathological reordering situation above:
 *
 *   -# (`m_throttling_now == 0` init state.)
 *   -# do_log() increments M past H.
 *   -# `really_log()` decrements M past L.
 *   -# `really_log()` flips `m_throttling_now` to `1`.
 *   -# do_log() flips `m_throttling_now` to `0`.
 *
 * or in the non-pathological case:
 *
 *   -# (`m_throttling_now == 0` init state.)
 *   -# do_log() increments M past H.
 *   -# `really_log()` decrements M past L.
 *   -# do_log() flips `m_throttling_now` to `1`.
 *   -# `really_log()` flips `m_throttling_now` to `0`.
 *
 * Result is correct either way.  One way to think about it is that we're counting threshold crossings in either
 * direction; when the *counter* of such crossings is odd, throttling is on; when it is even throttling is off.
 * In the rare pathological case where the "wrong" thread goes first, it is corrected based on this inductive
 * algorithm by the other thread momentarily.  And if during this time should_log() lets through a message or two
 * that it shouldn't, or vice versa, then it hardly matters.  During the steady state when the system is nowhere
 * near the thresholds H or L, should_log() behaves as it should.  (This is no different from accepting the possible
 * slight lag due to `relaxed` stores and loads of `m_throttling_now`.  It is an infrequent situation that does not
 * matter for our application, where we're trying to control bulky logging en masse, without absolute scalpel-like
 * surgical precision.)
 *
 * ### Throttling impl: Dealing with changes in H and L via configuration setter ###
 *
 * That leaves one thing to deal with, which we intentionally mentioned we've been ignoring.  What if H and/or L
 * are changed via throttling_cfg() mutator?  The above discussiona assumes they simply cannot change.  If they can:
 * A potential approach is trying to deal with that directly by adding tricky reasoning to the algorithm so that
 * H or L changing is always a possibility at any time, and we try to account for it.  This, I (ygoldfel) think,
 * is a terrible idea.  Even if there's some nice functional result possible, reasoning about it and maintaining it
 * is a nightmare.  Instead we can leave the algorithm essentially be as-described... but:
 *
 * Separate the state of the algorithm -- everything except #m_throttling_active which is orthogonal and accessed
 * only by should_log() -- into a `Throttling` state `struct` with fields `m_throttling_now`, `m_pending_logs_sz`,
 * and `Throttling_cfg m_cfg` (which contains `m_hi_limit` and `m_lo_limit`).  The contents of a given `m_cfg`
 * (which we've been calling `m_throttling_cfg` until now) *never change*.  Next, have an
 * `atomic<Throttling*> m_throttling` member.  In the algorithm (should_log(), do_log(), `really_log()`) relaxed-load
 * this into a local `Throttling* const throttling` pointer; then from that point on access the bits of state
 * through `*throttling` dereference.  In a given `*throttling` the limits can never change.
 *
 * So the last bit of the puzzle is what throttling_cfg() mutator must do.  And that is: Upon detecting that the
 * config (`m_hi_limit`, `m_lo_limit`, or both) has actually changed, create a new `Throttling`; initialize
 * its state (including, as noted before, `m_throttling_now = (m_pending_logs_sz >= m_throttling_cfg.m_hi_limit)`);
 * and then atomically-replace the pointer `m_throttling` to point to this new state structure.  Whatever concurrent
 * operations might still be dealing with the previous `*m_throttling` will exit soon enough.  At worst a few
 * logged lines' memory estimates will be ignored by the system, due to this old `Throttling` getting abandoned
 * along with those last few changes to its `m_pending_logs_sz` -- but so what?  This only occurs (if ever) during
 * a config change which are not meant to be frequent events during which we must very-precisely count the stats.
 *
 * Of course we must be careful to not `delete` the old `Throttling` until it's safe.  You can see how this is handled
 * in the code itself; omitting here.
 *
 * Naturally nothing is free, so this adds a little more overhead, most importantly to should_log().  Namely that is:
 *
 *   - Atomic pointer load (relaxed-ordering).
 *   - Pointer dereference.
 *
 * We can live with that.
 */
class Async_file_logger :
  public Logger,
  protected Log_context
{
public:
  // Types.

  /**
   * Controls behavior of the throttling algorithm as described in Async_file_logger doc header Throttling section.
   * As noted there, these values affect the algorithm for computing Throttling versus Not-Throttling state but
   * *not* whether should_log() actually allows that state to have any effect.  That is controlled by a peer
   * argument to throttling_cfg().
   */
  struct Throttling_cfg
  {
    /**
     * The throttling algorithm will go from Not-Throttling to Throttling state if and only if the current memory
     * usage changes from `< m_hi_limit` to `>= m_hi_limit`.
     * Async_file_logger doc header Throttling section calls this value H.  It must be positive.
     */
    uint64_t m_hi_limit;

    /**
     * The throttling algorithm will go from Throttling to Not-Throttling state if and only if the current memory
     * usage changes from `>= m_lo_limit` to `< m_lo_limit`.
     * Async_file_logger doc header Throttling section calls this value L.  It must be `< m_hi_limit` and positive.
     */
    uint64_t m_lo_limit;
  }; // struct Throttling_cfg

  // Constructors/destructor.

  /**
   * Constructs logger to subsequently log to the given file-system path.  It will append.
   *
   * @todo Consider adding Async_file_logger constructor option to overwrite the file instead of appending.
   *
   * @param config
   *        Controls behavior of this Logger.  In particular, it affects should_log() logic (verbosity default and
   *        per-component) and output format (such as time stamp format).  See thread safety notes in class doc header.
   *        This is saved in #m_config.
   * @param log_path
   *        File-system path to which to write subsequently.  Note that no writing occurs until the first do_log() call.
   * @param backup_logger_ptr
   *        The Logger to use for `*this` to log *about* its logging operations to the actual intended file-system path;
   *        or null to not log such things anywhere.  If you do not pass in null, but ensure
   *        `backup_logger_ptr->should_log()` lets through nothing more than `Sev::S_INFO` severity messages for
   *        `Flow_log_component::S_LOG`, then you can expect a reasonable amount of useful output that will not
   *        affect performance.  Tip: null is a reasonable value.  A Simple_ostream_logger logging to `cout` and `cerr`
   *        (or only `cout`) is also a good choice, arguably better than null.
   *        Lastly, setting verbosity to `INFO` for `*backup_logger_ptr` is typically a better choice than
   *        `TRACE` in practice.
   * @param capture_rotate_signals_internally
   *        If and only if this is `true`, `*this` will detect SIGHUP (or your OS's version thereof);
   *        upon seeing such a signal, it will fire the equivalent of log_flush_and_reopen(), as needed for classic
   *        log rotation.  (The idea is: If we are writing to path F, then your outside log rotation tool will rename
   *        F -> F.1 [and F.1 -> F.2, etc.]; even as we continue writing to the underlying file after it has been
   *        renamed; then the tool sends SIGHUP; we flush/close what is really F.1; reopen at the real path F
   *        again, which will create it anew post-rotation.)  If `false` then you'd have to do it yourself if desired.
   *        If this is `true`, the user may register their own signal handler(s) (for any purpose whatsoever) using
   *        `boost::asio::signal_set`. However, behavior is undefined if the program registers signal handlers via any
   *        other API, such as `sigaction()` or `signal()`.  If you need to set up such a non-`signal_set` signal
   *        handler, AND you require rotation behavior, then (1) set this option to `false`; (2) trap SIGHUP yourself;
   *        (3) in your handlers for the latter, simply call log_flush_and_reopen().  However, if typical, common-sense
   *        behavior is what you're after -- and either don't need additional signal handling or are OK with using
   *        `signal_set` for it -- then setting this to `true` is a good option.
   */
  explicit Async_file_logger(Logger* backup_logger_ptr,
                             Config* config, const fs::path& log_path,
                             bool capture_rotate_signals_internally);

  /// Flushes out anything buffered, returns resources/closes output file(s); then returns.
  ~Async_file_logger() override;

  // Methods.

  /**
   * Implements interface method by returning `true` if the severity and component (which is allowed to be null)
   * indicate it should.  As of this writing not thread-safe against changes to `*m_config`.
   *
   * @param sev
   *        Severity of the message.
   * @param component
   *        Component of the message.  Reminder: `component.empty() == true` is allowed.
   * @return See above.
   */
  bool should_log(Sev sev, const Component& component) const override;

  /**
   * Implements interface method by returning `true`, indicating that this Logger may need the contents of
   * `*metadata` and `msg` passed to do_log() even after that method returns.
   *
   * @return See above.
   */
  bool logs_asynchronously() const override;

  /**
   * Implements interface method by asynchronously logging the message and some subset of the metadata in a fashion
   * controlled by #m_config.
   *
   * @param metadata
   *        All information to potentially log in addition to `msg`.
   * @param msg
   *        The message.
   */
  void do_log(Msg_metadata* metadata, util::String_view msg) override;

  /**
   * Causes the log at the file-system path to be flushed/closed (if needed) and
   * re-opened; this will happen as soon as possible but may occur asynchronously after this method exits, unless
   * directed otherwise via `async` argument.
   *
   * ### Uses ###
   * Flushing: `log_flush_and_reopen(false)` is a reasonable and safe way to flush anything buffered in memory to the
   * file.  Naturally, for performance, it should not be done frequently.  For example this might be useful in the
   * event of an abnormal termination (`abort()`, etc.), in the signal handler before exiting program.
   *
   * Rotation: `log_flush_and_reopen(true)` is useful for rotation purposes; however, you need not do this manually if
   * you decided to (properly) use the `capture_rotate_signals_internally == true` option in Async_file_logger
   * constructor; the procedure will occur on receiving the proper signal automatically.
   *
   * @todo `Async_file_logger::log_flush_and_reopen(true)` is great for flushing, such as in an abort-signal handler,
   * but providing just the flushing part without the reopening might be useful.  At the moment we've left it
   * this way, due to the vague feeling that closing the file upon flushing it is somehow more final and thus safer
   * (in terms of accomplishing its goal) in an abort-signal scenario.  Feelings aren't very scientific though.
   *
   * @param async
   *        If `true`, the operation will execute ASAP but asynchronously, the method exiting immediately;
   *        else it will complete fully before this method returns.
   */
  void log_flush_and_reopen(bool async = true);

  /**
   * Accessor returning a copy of the current set of throttling knobs.  Please see Async_file_logger doc header
   * Throttling section for description of their meanings in the algorithm.
   *
   * @see throttling_active() also.
   *
   * Informally, this is meant for calling just ahead of throttling_cfg() mutator in the same thread, or perhaps
   * purely informationally such as to log the values.
   *
   * ### Thread safety ###
   * It is allowed to call this anytime from any thread, except concurrently with same-named mutator.
   * The returned Throttling_cfg will never mix values from 2+ different states straddling a throttling_cfg()
   * mutator call.
   *
   * @return The current knobs controlling the behavior of the algorithm that determines
   *         Throttling versus Not-Throttling state.
   *         If throttling_cfg() mutator is never called, then the values therein will be some valid defaults.
   */
  Throttling_cfg throttling_cfg() const;

  /**
   * Whether the throttling feature is currently in effect.  That is: can the throttling computations actually
   * affect should_log() output?  (It is *not* about whether log lines are actually being rejected due to throttling
   * right now.)  Please see Async_file_logger doc header Throttling section for more info.
   *
   * If `true` should_log() will potentially consider Throttling versus Not-Throttling state; else it will ignore it.
   * If throttling_cfg() mutator is never called, then this shall be `false` (feature inactive by default).
   *
   * @see throttling_cfg() accessor also.
   *
   * Informally, this is meant for calling just ahead of throttling_cfg() mutator in the same thread, or perhaps
   * purely informationally such as to log the value.
   *
   * ### Thread safety ###
   * It is allowed to call this anytime from any thread, except concurrently with throttling_cfg() mutator.
   *
   * @return See above.
   */
  bool throttling_active() const;

  /**
   * Mutator that sets the throttling knobs.  Please see Async_file_logger doc header
   * Throttling section for description of their meanings in the algorithm including about corner cases as to
   * what happens when these values change.
   *
   * ### Thread safety ###
   * It is allowed to call this anytime from any thread except concurrently with itself or
   * same-named accessor or throttling_active() on the same `*this`.
   * That leads to undefined behavior.
   *
   * @param active
   *        Whether the feature shall be in effect (if `true` should_log() will
   *        potentially consider Throttling versus Not-Throttling state; else it will ignore it).
   * @param The new values for knobs controlling the behavior of the algorithm that determines
   *        Throttling versus Not-Throttling state.
   */
  void throttling_cfg(bool active, const Throttling_cfg& cfg);

  // Data.  (Public!)

  /**
   * Reference to the config object passed to constructor.  Note that object is mutable; see notes on thread safety.
   *
   * @internal
   * ### Rationale ###
   * This can (and is but not exclusively) exclusively stored in `m_serial_logger->m_config`; it is stored here also
   * for `public` access to the user.  It's a pointer in any case.
   */
  Config* const m_config;

private:
  // Types.

  /**
   * A coherent full state of the throttling algorithm.  This is discussed in detail in the Impl section of
   * the class doc header.  Briefly: this is in an encapsulating `struct` to be able to replace the entire state
   * via an atomic `Throttling*` store.
   */
  struct Throttling
  {
    /**
     * The immutable hi-limit and lo-limit configuration.
     * As discussed in Impl section of class doc header: if the config needs to change, then a new Throttling
     * is created.  Hence the algorithm dealing with a given Throttling state set can remain simple, knowing
     * that hi-limit and lo-limit are immutable.
     */
    const Throttling_cfg m_cfg;

    /**
     * Estimate of how much RAM is being used by storing do_log() requests' data (message itself, metadata)
     * before they've been logged to file via `really_log()` (and therefore freed).
     *
     * Each log request's cost is computed via mem_cost(): do_log() increments this by mem_cost(); then
     * a corresponding `really_log()` decrements it by that same amount.
     *
     * ### Rationale: Why signed type? ###
     * There is a corner case having to do with throttling_cfg() mutator.  See explanation therein.
     */
    std::atomic<int64_t> m_pending_logs_sz;

    /**
     * Boolean-representing integer that represents the output of the always-on throttling algorithm; namely
     * 1 if currently should_log() shall return `false` due to too-much-RAM-being-used; 0 otherwise.
     * It starts at 0; then when `m_cfg.m_hi_limit` is crossed (by #m_pending_logs_sz) going up in do_log(),
     * it becomes 1.  Next, when `m_cfg.m_lo_limit` is crossed (by #m_pending_logs_sz) going down in `really_log()`,
     * it becomes 0 again.  Rinse/repeat.
     *
     * The Impl section of class doc header discusses subtleties about setting this properly, avoiding
     * evil reordering.
     */
    std::atomic<unsigned int> m_throttling_now;
  }; // struct Throttling

  /// Short-hand for a signal set.
  using Signal_set = boost::asio::signal_set;

  // Methods.

  /**
   * SIGHUP/equivalent handler for the optional feature `capture_rotate_signals_internally` in constructor.
   * Assuming no error, executes `m_serial_logger->log_flush_and_reopen()` and then again waits for the next such
   * signal.
   *
   * @param sys_err_code
   *        The code from boost.asio.  Anything outside of `operation_aborted` is very strange.
   * @param sig_number
   *        Should be SIGHUP/equivalent, as we only wait for those signal(s).
   */
  void on_rotate_signal(const Error_code& sys_err_code, int sig_number);

  /**
   * How much do_log() with the same args shall contribute to Trottling::m_pending_logs_sz.
   * See discussion of throttling algorithm in Impl section of class doc header.  This always returns the same value
   * if given the same args.
   *
   * @param metadata
   *        See do_log().
   * @param msg
   *        See do_log().
   * @return Positive value.
   */
  static size_t mem_cost(const Msg_metadata* metadata, util::String_view msg);

  // Data.

  /**
   * The current state of throttling-based-on-pending-logs-memory-used always-on algorithm.
   * See Throttling doc header (which would also point you to the long-form discussion in Impl section of class doc
   * header).
   */
  std::atomic<Throttling*> m_throttling;

  /**
   * The objects to which has #m_throttling has pointed;
   * in steady state `m_throttling == m_throttling_states.back().get()`.
   *
   * ### Rationale ###
   * Firstly see the end of the throttling impl discussion in class doc header; then come back here.
   * If throttling_cfg() is never called, then this has `.size() == 1`, and
   * the important thing -- `m_throttling` -- will point to that one Throttling object.  If user wants to
   * change something inside Throttling_cfg via their throttling_cfg() call, a new Throttling state
   * including that new Throttling_cfg is created, added to this list, and then #m_throttling is updated to
   * point to it.
   *
   * The good thing is it allows #m_throttling to be a raw pointer which is loaded by should_log() with high perf.
   * The bad thing is we use a bit (`sizeof Throttling` + a bit more) of memory per config update.  Alternative =
   * use `boost::atomic_shared_ptr` or `std::shared_ptr` with `std::atomic_load_explicit()`
   * (in C++20 `std::atomic<shared_ptr>` specialization is the better option).
   * The good thing would be no such memory quasi-leaking; the bad thing would have been less certainty about
   * performance or loading-to-dereference it.  It's arguable; going with this at this time though.
   */
  std::vector<boost::movelib::unique_ptr<Throttling>> m_throttling_states;

  /**
   * Whether the throttling-based-on-pending-logs-memory-used feature is currently active or not.
   * As explained in detail in Throttling section in class doc header, this is queried only in should_log()
   * and only as a gate to access the results of the always-on throttling algorithm.  That algorithm, whose
   * data reside in #m_throttling, is always active; but this `m_throttling_active` flag determines whether
   * that algorithm's output `m_throttling->m_throttling_on` is used or ignored by should_log().
   *
   * It is atomic, and accessed with `relaxed` order only, due to being potentially frequently accessed in
   * the very-often-called should_log().
   */
  std::atomic<bool> m_throttling_active;

  /**
   * This is the `Logger` doing all the real log-writing work (the one stored in Log_context is the
   * logger-about-logging, `backup_logger_ptr` from ctor args).  In itself it's a fully functional Logger, but
   * its main limitation is all calls between construction and destruction must be performed non-concurrently --
   * for example from a single thread or boost.asio "strand."  The way we leverage it is we simply only make
   * log-file-related calls (especially Serial_file_logger::do_log() but also
   * Serial_file_logger::log_flush_and_reopen()) from within #m_async_worker-posted tasks (in other words from
   * the thread #m_async_worker starts at construction).
   *
   * ### Design rationale ###
   * What's the point of Serial_file_logger?  Why not just have all those data and logic (storing the `ofstream`, etc.)
   * directly in Async_file_logger?  Answer: That's how it was originally.  The cumulative amount of state and logic
   * is the same; and there's no real usefulness for Serial_file_logger as a stand-alone Logger for general users;
   * so in and of itself splitting it out only adds a bit of coding overhead but is otherwise the same.  So why do it?
   * Answer:
   *
   * The reason we split it off was the following: We wanted to sometimes log to the real file from within the
   * #m_async_worker thread; namely to mark with a pair of log messages that a file has been rotated.  It was possible
   * to do this via direct calls to a would-be `impl_do_log()` (which is do_log() but already from the worker thread),
   * but then one couldn't use the standard `FLOW_LOG_*()` statements to do it; this was both inconvenient and difficult
   * to maintain over time (we predicted).  At that point it made sense that really Async_file_logger *is*
   * a would-be Serial_file_logger (one that works from one thread) with the creation of the one thread -- and other
   * such goodies -- added on top.  With that, `FLOW_LOG_SET_LOGGER(m_serial_logger)` + `FLOW_LOG_INFO()` trivially logs
   * to the file (as long as it's done from the worker thread), accomplishing that goal; and furthermore it makes
   * logical sense in terms of overall design.  The latter fact I (ygoldfel) would say is the best justification; and
   * the desire for `*this` to log to the file, itself, was the triggering use case.
   */
  boost::movelib::unique_ptr<Serial_file_logger> m_serial_logger;

  /**
   * The thread (1-thread pool, technically) in charge of all #m_serial_logger I/O operations including writes
   * to file.  Thread starts at construction of `*this`; ends (and is synchronously joined) at
   * destruction of `*this` (by means of delegating to #m_async_worker destructor).
   */
  async::Single_thread_task_loop m_async_worker;

  /**
   * Signal set which we may or may not be using to trap SIGHUP in order to auto-fire
   * `m_serial_logger->log_flush_and_reopen()` in #m_async_worker.  `add()` is called on it at init iff [sic]
   * that feature is enabled by the user via ctor arg `capture_rotate_signals_internally`.  Otherwise this object
   * just does nothing `*this` lifetime.
   */
  Signal_set m_signal_set;
}; // class Async_file_logger

} // namespace flow::log
