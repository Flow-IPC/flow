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
 * @todo Lacking feature: Compress-as-you-log in Async_file_logger.  So, optionally, when characters are actually
 * written out to file-system, gzip/zip/whatever them instead of writing plain text.  (This is possible at least
 * for gzip.)  Background: It is common-place to compress a log file after it has been rotated (e.g., around rotation
 * time: F.log.1.gz -> F.log.2.gz, F.log -> F.log.1 -> F.log.1.gz). It is more space-efficient (at least), however,
 * to write to F.log.gz directly already in compressed form; then rotation requires only renaming (e.g.:
 * F.log.1.gz -> F.log.2.gz, F.log.gz [already gzipped from the start] -> F.log.1.gz).
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
 * your code does INFO-level logging too often, it could happen then too.  The official *convention* is to
 * log INFO-or-more-severe messages only if this would not meaningfully affect the overall perf and responsiveness
 * of the system; but sometimes mistakes are made.)
 *
 * Internally each do_log() call pushes this *log request* into a central (per `*this`) queue of log requests;
 * a single (per `*this`) background thread is always popping this queue ASAP, writing the message to the file,
 * etc., until the queue is emptied; then it sleeps until it becomes non-empty; and so on.  If many threads are
 * pushing messages faster than this thread can get through them, then more and more RAM
 * is used to store the enqueued messages.  If the throughput doesn't decrease in time, letting this central thread
 * catch up, then the RAM use might cause swapping and eventually out-of-memory (congestive collapse).
 * To be clear, unless congestive collapse is in fact reached, the system is self-correcting, in that given
 * a respite from the log-requests (do_log() calls), the queue size *will* get down to zero, as will the corresponding
 * memory use.  However, if this does not occur early enough, congestive collapse occurs.
 *
 * Throttling is a safeguard against this.  It works as follows.  There's a limit H in bytes.  We start at
 * Not-Throttling state.  If the memory use M grows beyond H, we enter Throttling state.  In this state, we
 * reject incoming log requests -- thus letting the system deal with just logging-to-file what it has queued up
 * (in FIFO fashion, naturally) -- until no more such queued-up requests remain.  Once the queue is empty,
 * we re-enter Not-Throttling state.  In other words beyond a certain total memory use, we throttle; then to exit
 * this state total memory use has to go down to 0, before we un-throttle.  The idea is to encourage a sawtooth
 * `/\/\/\` memory-use pattern when subjected to a firehose of log requests.
 *
 * @note An earlier functional design contemplated having a limit L (smaller than H; e.g., picture L = 50% of H),
 *       so that mem-use would need to merely get down to L to re-enter Not-Throttling state.  However, upon
 *       prototyping this, it became clear this hardly improved the experience, instead making it rather confusing
 *       to understand in action.  E.g., if there's a firehose going at full-blast, and you're fighting it by turning
 *       off the fire-hose and letting water drain, but then turn it on again once the container is 50% full, then the
 *       draining will "fight" the filling again, potentially losing quite decisively.  Trying to then read resulting
 *       logs is messy and strange, depending on the 2 rates relative to each other.  By comparison, the
 *       fill-drain-fill-drain `/\/\/\` pattern is straightforward to understand; and one can cleanly point out
 *       the minima and maxima with log messages.  Plus, less configuration/tuning with little to no functional
 *       loss = a good thing.
 *
 * The particular mechanic of throttling is as follows.  If throttling is enabled during a particular call to
 * should_log(), and if and only if should_log() would return `true` based on considerations excluding the
 * throttling feature, then:
 *
 *   - should_log() returns `true` in Not-Throttling state;
 *   - should_log() returns `false` in Throttling state;
 *
 * Since standard `FLOW_LOG_*` macros avoid do_log() (or even the evaluation of the message -- which is itself
 * quite expensive, possibly quite a bit more expensive than the do_log()) if should_log()
 * returns `false` for a given log call site, during Throttling state enqueuing of messages is blocked (letting
 * the logging-to-file thread catch up).  `should_log() == false` is how we turn off the firehose.
 *
 * M starts at 0.  Each do_log() (queued-up log request) increments M based on the memory-use estimate of the
 * message+metadata passed to do_log(); then it enqueues the log request.  Each time the background thread
 * actually writes out the queued-up message+metadata, it decrements M by the same value by which it was
 * incremented in do_log(); accordingly the log-request's memory is freed.
 *
 * This algorithm (computing M via increments and decrements; setting state to Throttling or Not-Throttling)
 * is carried out at all times.  However, should_log() consults the state (Throttling versus Not-Throttling), if
 * and only if the throttling feature is enabled at that time.  If it is not, that state is simply ignored, and
 * should_log() only makes the usual Config verbosity check(s); if that results in `true` then the message
 * is enqueued.  Therefore one can enable throttling at any time and count on its having immediate
 * effect based on actual memory use at that time.
 *
 * The limit H can also be reconfigured at any time.  Essentially throttling_cfg() mutator takes
 * 2 orthogonal sets of info: 1, whether throttling is to be possible at all (whether Throttling versus Not-Throttling
 * affects should_log() from now on); and 2, the limit H which controls the policy about setting
 * the state (Throttling versus Not-Throttling).  (2) affects the algorithm that computes that binary state; whereas
 * (1) affects whether that binary state actually controls whether to prevent logging to save memory or not.
 *
 * If H is modified, the binary state is reinitialized: it is set to Throttling if and only if
 * memory use M at that time exceeds H; else to Not-Throttling.  The state prior to the throttling_cfg() mutator call
 * does not matter in this situation; it is overwritten.  This avoids various annoying corner cases and ambiguities
 * around config updates.
 *
 * Lastly: Initially throttling is disabled, while a certain default value of H is assumed.  Hence the above
 * algorithm is active but has no effect, unless you call `throttling_cfg(true, ...)` to make it have effect
 * and/or change H.  You may use throttling_cfg() accessor and throttling_active() to get a copy of the current
 * config values.
 *
 * ### Thread safety ###
 * As noted above, simultaneous logging from multiple threads is safe from output corruption, in that
 * simultaneous do_log() calls for the same Logger targeting the same stream will log serially to each other.
 * However, if some other code or process writes to the same file, then all bets are off -- so don't.
 *
 * See thread safety notes and to-dos regarding #m_config in Simple_ostream_logger doc header.  These apply here also.
 *
 * throttling_cfg() mutator does not add any thread safety restrictions: it can be called concurrently with any
 * other method, including should_log(), do_log(), same-named accessor, and throttling_active().  There is one formal
 * exception: it must not be called concurrently with itself.
 *
 * There are no other mutable data (state), so that's that.
 *
 * ### Throttling: Functional design rationale notes ###
 * The throttling feature could have been designed differently (in terms of how it should act, functionally speaking),
 * and a couple of questions tend to come up, so let's answer here.
 *   - Why have the throttling algorithm always-on, even when `!throttling_active()` -- which is default at that?
 *     Could save cycles otherwise, no?  Answer: To begin with, the counting of the memory used (M) should be accurate
 *     in case it is (e.g.) high, and one changes throttling_active() to `true`.  Still, couldn't some things be
 *     skipped -- namely perhaps determining whether state is Throttling or Not-Throttling and logging about it -- when
 *     `!throttling_active()`?  Answer: Yes, and that might be a decent change in the future, as internally it might
 *     be possible to skip some mutex work in that situation which could be a small optimization.  It is basically
 *     simpler to think about and implement the existing way.  (More notes on this in internal comments.)
 *   - Why not get rid of throttling_active() knob entirely?  E.g., Async_file_logger::Throttling_cfg::m_hi_limit (H)
 *     could just be set to a huge value to have an apparently similar effect to `!throttling_active()`.  (The knob
 *     could still exist cosmetically speaking but just have the aforementioned effect.)  Answer: I (ygoldfel) first
 *     thought similarly, while others specified otherwise; but I quickly came around to agreeing with them.  It is
 *     nice to log about crossing the threshold H even without responding to it by throttling; it could be a signal
 *     for a user to look into enabling the feature.  Granted, we could also log at every 500k increment, or
 *     something like that; but the present setup seemed like a nice balance between power and simplicity.
 *
 * All in all, these choices are defensible but not necessarily the only good ones.
 *
 * @internal
 *
 * Implementation
 * --------------
 * The basic implementation is straightforward enough to be gleaned from reading the code and other comments.
 *
 * ### Throttling impl: The essential algorithm ###
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
 *       - If #m_throttling_now is `false` (state is Not-Throttling) then return `true`.  Else:
 *       - Return `false`.  (Throttling feature enabled, and state is currently Throttling.)
 *   - `do_log(metadata, msg)`:
 *     - Compute `C = mem_cost(msg)` which inlines to essentially `msg.size()` + some compile-time constant.
 *     - Let local `bool throttling_begins = false`.
 *     - Lock mutex.
 *       - Increment `m_pending_logs_sz` by `C`.  `m_pending_logs_sz` is called M in the earlier discussion: the
 *         memory use estimate of things do_log() has enqueued but `really_log()` (see just below) has not
 *         yet dequeued and logged to file.
 *       - If #m_throttling_now is `false`, and we just made `m_pending_logs_sz` go from
 *         `< m_throttling_cfg.m_hi_limit` (a/k/a H) to `>= m_throttling_cfg.m_hi_limit`, then
 *         set #m_throttling_now to `true`; and set `throttling_begins = true`.
 *     - Enqueue the log-request:
 *       - Capture `metadata`; a copy of `msg`; and `throttling_begins`.
 *       - `m_async_worker.post()` the lambda which invokes `really_log()` with those 3 items as args.
 *   - `really_log(metadata, msg, throttling_begins)`:
 *     - `m_serial_logger->do_log(metadata, msg)`: write-out the actual message to the file.
 *     - If `throttling_begins == true`: via `m_serial_logger->do_log()` write-out a special message
 *       indicating that `msg` was the message causing state to earlier change from Not-Throttling to
 *       Throttling due to mem-use passing ceiling H at that time.
 *     - Compute `C = mem_cost(msg)` (same as in `do_log()` above).
 *     - Let local `bool throttling_ends = false`.
 *     - Lock mutex.
 *       - Decrement `m_pending_logs_sz` (a/k/a M) by `C`.
 *       - If #m_throttling_now is `true`, and we just made `m_pending_logs_sz` go down to 0, then
 *         set #m_throttling_now to `false`; and set `throttling_ends = true`.
 *     - If `throttling_ends == true`: via `m_serial_logger->do_log()` write-out a special message
 *       indicating that state has changed from Throttling to Not-Throttling due to mem-use reaching 0.
 *   - `throttling_cfg(active, cfg)` mutator:
 *     - Lock mutex.
 *       - Save args into #m_throttling_active and #m_throttling_cfg respectively.
 *       - If the latter's contained value (H) changed:
 *         - Assign `m_throttling_now = (m_pending_logs_sz >= m_throttling_cfg.m_hi_limit)`.
 *           This reinitializes the state machine cleanly as promised in the class doc header public section.
 *
 * The mutex makes everything easy.  However the resulting perf is potentially unacceptable, at least
 * because should_log() is called *very* frequently from *many* threads and has a mutex lock now.
 * We must strive to keep computational overhead in should_log() very low; and avoid extra lock contention
 * if possible, especially to the extent it would affect should_log().  Onward:
 *
 * ### Throttling impl: The algorithm modified to become lock-free in should_log() ###
 * The easiest way to reduce critical section in should_log() concerns access to #m_throttling_active.
 * Suppose we make it `atomic<bool>` instead of `bool` with mutex protection.  If we store with `relaxed` ordering
 * and load with `relaxed` ordering, and do both outside any shared mutex-lock section:
 * throttling_cfg() mutator does the quite-rare storing; should_log() does the possibly-frequent (albeit gated by
 * `m_serial_logger->should_log() == true` in the first place) loading.  The `relaxed` order means
 * at worst there's a bit of a delay for some threads noticing the config change; so this or that thread might
 * throttle or not-throttle a tiny bit of time after another: it's absolutely not a problem.  Moreover there is
 * ~zero penalty to a `relaxed` load of `atomic<bool>` compared to simply accessing a `bool`.  Adding a `bool` check
 * to should_log() is not nothing, but it's very close.
 *
 * The only now-remaining thing in the mutex-lock section of should_log() (see pseudocode above) is the
 * Boolean check of #m_throttling_now.  There is exactly one consumer of this Boolean: should_log().  Again
 * let's replace `bool m_throttling_now` with `atomic<bool> m_throttling_now`; and load it with `relaxed`
 * ordering in should_log(), outside any shared mutex-lock section.  There are exactly 3 assigners: do_log()
 * and `really_log()`; they assign this when M 1st goes up past H (assign `true`) or 1st down to 0
 * (assign `false`) respectively; and throttling_cfg() mutator (assign depending on where M is compared to
 * the new H).  So let's assume -- and we'll discuss the bejesus out of it below -- we ensure the assigning
 * algorithm among those 3 places is made to work properly, meaning #m_throttling_now (Throttling versus
 * Not-Throttling state) algorithm is made to correctly set the flag's value correctly in and of itself.  Then
 * should_log() merely needs to read #m_throttling_now and check it against `true` to return `should_log() ==
 * false` iff so.  Once again, if `relaxed` ordering causes some threads to "see" a new value a little later
 * than others, that is perfectly fine.  (We re-emphasize that the 3 mutating assignment events are hardly
 * frequent: only when passing H going up for the 1st time since being 0, reaching 0 for the 1st time since
 * being >= H, and possibly in throttling_cfg() mutator call.)
 *
 * Now the critical section in should_log() has been emptied: so no more mutex locking or unlocking needed in it.
 *
 * @note Note well!  The lock-free, low-overhead nature of should_log() as described in the preceding 3 paragraphs is
 *       **by far** the most important perf achievement of this algorithm.  Having achieved that, we've solved what's
 *       almost certainly the only perf objective that really matters.  The only other code area that could conceivably
 *       matter perf-wise is do_log() -- and it does conceivably matter but not very much in practice.
 *       Please remember: do_log() is already a heavy-weight operation; before it is
 *       even called, the `FLOW_LOG_*()` macro almost certainly invoking it must perform expensive `ostream` assembly
 *       of `msg`; then do_log() itself needs to make a copy of `msg` and create an `std::function<>` with a number of
 *       captures, and enqueue all that into a boost.asio queue (which internally involves a mutex lock/unlock).  That's
 *       why should_log() is a separate call: by being very fast and usually returning `false`, most *potential*
 *       do_log() calls -- *and* the `msg` assembly (and more) *potentially* preceding each -- never happen at all:
 *       `FLOW_LOG_...()` essentially has the form `if (should_log(...)) { ...prep msg and mdt...; do_log(mdt, msg); }`.
 *       So we *should* strive to keep added throttling-algorithm-driven overhead in do_log() low and minimize
 *       mutex-locked critical sections therein; but such striving is a nicety, whereas optimizing should_log()
 *       is a necessity.
 *
 * So: We've now reduced the algorithm to:
 *
 *   - `do_log(metadata, msg)`:
 *     - Compute `C = mem_cost(msg)` (add a few things including `msg.size()`).
 *     - Let local `bool throttling_begins = false`.
 *     - Lock mutex.
 *       - `m_pending_logs_sz += C`.
 *       - If #m_throttling_now is `false`, and we just made `m_pending_logs_sz` go from
 *         `< m_throttling_cfg.m_hi_limit` to `>= m_throttling_cfg.m_hi_limit`, then
 *         set #m_throttling_now to `true`; and set `throttling_begins = true`.
 *     - Enqueue the log-request:
 *       - Capture `metadata`; a copy of `msg`; and `throttling_begins`.
 *       - `m_async_worker.post()` the lambda which invokes `really_log()` with those 3 items as inputs.
 *   - `really_log(metadata, msg, throttling_begins)`:
 *     - `m_serial_logger->do_log(metadata, msg)`.
 *     - If `throttling_begins == true`: via `m_serial_logger->do_log()` write-out a special message
 *       indicating that `msg` was the message causing state to earlier change from Not-Throttling to
 *       Throttling due to mem-use passing ceiling H at that time.
 *     - Compute `C = mem_cost(msg)`.
 *     - Let local `bool throttling_ends = false`.
 *     - Lock mutex.
 *       - `m_pending_logs_sz -= C`.
 *       - If #m_throttling_now is `true`, and we just made `m_pending_logs_sz == 0`,
 *         then set #m_throttling_now to `false`; and set `throttling_ends = true`.
 *     - If `throttling_ends == true`: via `m_serial_logger->do_log()` write-out:
 *       state has changed from Throttling to Not-Throttling due to mem-use use reaching 0.
 *   - `throttling_cfg(active, cfg)` mutator:
 *     - Save `m_throttling_active = active`.
 *     - Lock mutex.
 *       - Save `m_throttling_cfg = cfg`.
 *       - If the latter's contained value (H) changed:
 *         - Assign `m_throttling_now = (m_pending_logs_sz >= m_throttling_cfg.m_hi_limit)`.
 *
 * That's acceptable, because the really important (for perf) place, should_log(), now is lock-free
 * with the added overhead being 2 mere checks against zero; and even then only if the core `should_log()` yielded
 * `true` -- which usually it doesn't.
 *
 * Let's reassert that the overhead and potential lock contention added on account of the throttling logic
 * in do_log() are minor.  (If so, then `really_log()` and throttling_cfg() mutator need not be scrutinized much, as
 * they matter less and much less respectively.)  We've already noted this, but let's make sure.
 *
 *   - Added cycles (assuming no lock contention): To enumerate this overhead:
 *     - mem_cost().  This adds `msg.size()` (a `size_t` memory value) to a compile-time constant, more or less.
 *     - Increment M by that number (`+=` with saving a `prev_val` and resulting `new_val`).
 *     - Check a `bool`.
 *       - Possibly compare `prev_val < H` and `new_val >= H`.
 *     - Set `bool throttling_begins` to `true` or `false` accordingly.
 *     - Add `throttling_begins` in addition to the existing payload into Log_request (which is
 *       2 pointers + 1 `size_t`) which is packaged together with the task.
 *     - Mutex lock/unlock.  (We're assuming no lock contention; so this is cheap.)
 *     - CONCLUSION: It's 5-ish increments, 2-ish integer comparisons, copying a ~handful of scalars ~1x each, and
 *       change.  Compare to the "Enqueue the log-request" step alone: create `function<>`
 *       object, enqueue it to boost.asio task queue -- copying `msg` and other parts of Log_request in the process.
 *       Now throw in the `ostream<<` manipulation needed to assemble `msg`; the time spent heap-allocating + populating
 *       Msg_metadata, such as allocating and copying thread nickname, if it's long enough.  And lastly remember
 *       that the should_log() mechanism (even *without* any throttling) is
 *       normally supposed to make do_log() calls so infrequent that the processor cycle cost is small in the
 *       first place.  Conclusion: Yes, this overhead is acceptable: it is small as a %; and in absolute terms,
 *       the latter only conceivably not being the case, if it would have been not the case anyway (and not in a
 *       well functioning system).
 *   - Lock contention: The critical section is similar in both potentially contending pieces of code
 *     (do_log() and `really_log()`); so let's take the one in do_log().  It is *tiny*:
 *     Integer add and ~3 assignments; Boolean comparison; possibly 1-2 integer comparisons; and either a short jump
 *     or 2 more Boolean assignments.
 *     - It's tiny in absolute terms.
 *     - It's tiny in % terms, as discussed earlier for do_log().  (In `really_log()` it's even more so, as it is
 *       all in one thread *and* doing synchronous file I/O.)
 *     - do_log() already has a boost.asio task queue push with mutex lock/unlock, contending against `really_log()`
 *       performing mutex lock/unlock + queue pop; plus condition-variable wait/notify.  Even under very intense
 *       practical logging scenarios, lock contention from this critical section was never observed to be a factor.
 *     - CONCLUSION: It would be very surprising if this added locking ever caused any observable contention.
 *
 * @note I (ygoldfel) heavily pursued a completely lock-free solution.  I got tantalizingly close.  It involved
 *       an added pointer indirection in should_log() (and do_log() and `really_log()`), with a pointer
 *       storing throttling state `struct`, atomically replaced by `throttling_cfg()` mutator; completely removing
 *       mutex; and turning `m_pending_logs_sz` into an `atomic`.  Unfortunately there was a very unlikely corner
 *       case that was nevertheless formally possible.  Ultimately it came down to the fact that
 *       `A(); if (...based-on-A()...) { B(); }` and `C(); if (...based-on-C()...) { D(); }` executing concurrently,
 *       with `A()` being reached before `C()`, execution order can be A-C-D-B instead of the desired A-C-B-D.  In our
 *       case this could, formally speaking, cause `m_throttling_now => true => false` to incorrectly be switched to
 *       `m_throttling_now => false => true` (if, e.g., M=H is reached and then very quickly/near-concurrently M=0 is
 *       reached); or vice versa.  Without synchronization of some kind I couldn't make it be bullet-proof.
 *       (There was also the slightly longer computation in should_log(): pointer indirection to account for
 *       config-setting no longer being mutex-protected; but in my view that addition was acceptable still.
 *       Unfortunately I couldn't make the algorithm formally correct.)
 */
class Async_file_logger :
  public Logger,
  protected Log_context
{
public:
  // Types.

  /**
   * Controls behavior of the throttling algorithm as described in Async_file_logger doc header Throttling section.
   * As noted there, value(s) therein affect the algorithm for computing Throttling versus Not-Throttling state but
   * *not* whether should_log() actually allows that state to have any effect.  That is controlled by a peer
   * argument to throttling_cfg().
   *
   * @internal
   * ### Rationale ###
   * Why the `struct` and not just expose `m_hi_limit` by itself?  Answer: there is a "note" about it in the
   * Async_file_logger class doc header.  Short answer: maintainability/future-proofing.
   */
  struct Throttling_cfg
  {
    // Data.

    /**
     * The throttling algorithm will go from Not-Throttling to Throttling state if and only if the current memory
     * usage changes from `< m_hi_limit` to `>= m_hi_limit`.
     * Async_file_logger doc header Throttling section calls this value H.  It must be positive.
     */
    uint64_t m_hi_limit;

    /**
     * Value of `Async_file_logger{...}.throttling_cfg().m_hi_limit`: default/initial value of #m_hi_limit.
     *
     * Note that this value is not meant to be some kind of universally correct choice for #m_hi_limit.
     * Users can and should change `m_hi_limit`.
     */
    static constexpr uint64_t S_HI_LIMIT_DEFAULT = 1ull * 1024 * 1024 * 1024;
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
   * indicate it should; and if so potentially applies the throttling algorithm's result as well.
   * As of this writing not thread-safe against changes to `*m_config` (but thread-safe agains throttling_cfg()
   * mutator).
   *
   * Throttling comes into play if and only if: 1, `sev` and `component` indicate
   * should_log() should return `true` in the first place; and 2, `throttling_active() == true`.  In that case
   * the throttling alogorithm's current output (Throttling versus Not-Throttling state) is consulted to determine
   * whether to return `true` or `false`.  (See Throttling section of class doc header.)
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
   * @return See above.
   */
  bool throttling_active() const;

  /**
   * Mutator that sets the throttling knobs.  Please see Async_file_logger doc header
   * Throttling section for description of their meanings in the algorithm.
   *
   * ### Thread safety ###
   * It is okay to call concurrently with any other method on the same `*this`, except it must not be called
   * concurrently with itself.
   *
   * @param active
   *        Whether the feature shall be in effect (if should_log() will
   *        potentially consider Throttling versus Not-Throttling state; else it will ignore it).
   * @param cfg
   *        The new values for knobs controlling the behavior of the algorithm that determines
   *        Throttling versus Not-Throttling state.
   */
  void throttling_cfg(bool active, const Throttling_cfg& cfg);

  // Data.  (Public!)

  /**
   * Reference to the config object passed to constructor.  Note that object is mutable; see notes on thread safety.
   *
   * @internal
   * ### Rationale ###
   * This can be (and is but not exclusively) exclusively stored in `m_serial_logger->m_config`; it is stored here also
   * for `public` access to the user.  It's a pointer in any case.
   */
  Config* const m_config;

private:
  // Types.

  /// Short-hand for #m_throttling_mutex type.
  using Mutex = flow::util::Mutex_non_recursive;

  /// Short-hand for #Mutex lock.
  using Lock_guard = flow::util::Lock_guard<Mutex>;

  /// Short-hand for a signal set.
  using Signal_set = boost::asio::signal_set;

  /**
   * In addition to the task object (function) itself, these are the data placed onto the queue of `m_async_worker`
   * tasks for a particular `do_log()` call, to be used by that task and then freed immediately upon logging of the
   * message to file.
   *
   * @see mem_cost().
   *
   * ### Rationale/details ###
   * The object is movable which is important.  It is also copyable but only because, as of this writing, C++17
   * requires captures by value to be copyable (in order to compile), even though this is *not executed at runtime*,
   * unless one actually needs to make a copy of the function object (which we avoid like the plague).
   *
   * We try hard -- harder than in most situations -- to keep the memory footprint of this thing as small as possible,
   * right down to even avoiding a `shared_ptr`, when a raw or `unique_ptr` is enough; and not storing the result
   * of mem_cost() (but rather recomputing it inside the `m_async_worker` task).  That is because of the same memory
   * use potential problem with which the throttling feature (see Async_file_logger class doc header) grapples.
   *
   * Ideally each item stored here has RAII semantics, meaning once the object is destroyed, the stuff referred-to
   * therein is destroyed.  However you'll notice this is not the case at least for #m_metadata and
   * for #m_msg_copy.  Therefore the function body (the `m_async_worker` task for this Log_request) must manually
   * `delete` these objects from the heap.  Moreover, if the lambda were to never run (e.g., if we destroyed or
   * stopped `m_async_worker` while tasks are still enqueued), those objects would get leaked.  (As of this writing
   * we always flush the queue in Async_file_logger dtor for this and another reason.)
   *
   * So why not just use `unique_ptr` then?  The reason is above: they're not copyable, and we need it to be,
   * as otherwise C++17 won't let Log_request be value-captured in lambda.  One can use `shared_ptr`; this is elegant,
   * but at this point we're specifically trying to reduce the RAM use to the bare minimum, so we avoid even
   * the tiny control block size of `shared_ptr`.  For #m_msg_copy one could have used `std::string` or util::Basic_blob
   * or `std::vector`, but they all have some extra members we do not need (size on top of capacity; `Basic_blob`
   * also has `m_start`).  (util::Basic_blob doc header as of this writing has a to-do to implement a
   * `Tight_blob` class with just the pointer and capacity, no extras; so that would have been useful.)  Even
   * with those options, that would've still left #m_metadata.  One could write little wrapper classes for both
   * the string blob #m_msg_copy and/or Msg_metadata #m_metadata, and that did work.  Simply put, however, storing the
   * rare raw pointers and then explicitly `delete`ing them in one spot is just much less boiler-plate.
   *
   * @warning Just be careful with maintenance.  Tests should indeed try to force the above leak and use sanitizers
   * (etc.) to ensure it is avoided.
   */
  struct Log_request
  {
    // Data.

    /// Pointer to array of characters comprising a copy of `msg` passed to `do_log()`.  We must `delete[]` it.
    char* m_msg_copy;

    /// Number of characters in #m_msg_copy pointee string.
    size_t m_msg_size;

    /// Pointer to array of characters comprising a copy of `msg` passed to `do_log()`.  We must `delete` it.
    Msg_metadata* m_metadata;

    /**
     * Whether this log request was such that its memory footprint (`mem_cost()`) pushed `m_pending_logs_sz` from
     * `< m_throttling_cfg.m_hi_limit` to `>= m_throttling_cfg.m_hi_limit` for the first time since it was last
     * equal to zero.
     */
    bool m_throttling_begins;
  }; // struct Log_request

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
   * How much do_log() issuing the supplied Log_request shall contribute to #m_pending_logs_sz.
   * Log_request in this case essentially just describes the `msg` and `metadata` args to do_log().
   * See discussion of throttling algorithm in Impl section of class doc header.
   *
   * @param log_request
   *        See do_log(): essentially filled-out with `msg`- and `metadata`-derived info.
   *        The value of Log_request::m_throttling_begins is ignored in the computation (it would not affect it
   *        anyway), but the other fields must be set.
   * @return Positive value.  (Informally: we've observed roughly 200 plus message size as of this writing.)
   */
  static size_t mem_cost(const Log_request& log_request);

  /**
   * Estimate of memory footprint of the given value, including memory allocated on its behalf -- but
   * excluding its shallow `sizeof`! -- in bytes.
   *
   * @param val
   *        Value.
   * @return See above.
   */
  static size_t deep_size(const Log_request& val);

  // Data.

  /**
   * Protects throttling algorithm data that require coherence among themselves:
   * #m_throttling_cfg, #m_pending_logs_sz, #m_throttling_now.  The latter is nevertheless `atomic<>`; see
   * its doc header as to why.
   *
   * ### Perf ###
   * The critical sections locked by this are extremely small, and should_log() does not have one at all.
   *
   * @see Class doc header Impl section for discussion of the throttling algorithm and locking in particular.
   */
  mutable Mutex m_throttling_mutex;

  /// See Throttling_cfg.  Protected by #m_throttling_mutex.
  Throttling_cfg m_throttling_cfg;

  /**
   * Estimate of how much RAM is being used by storing do_log() requests' data (message itself, metadata)
   * before they've been logged to file via `really_log()` (and therefore freed).  Protected by #m_throttling_mutex.
   *
   * Each log request's cost is computed via mem_cost(): do_log() increments this by mem_cost(); then
   * a corresponding `really_log()` decrements it by that same amount.
   *
   * ### Brief discussion ###
   * If one made this `atomic<size_t>`, and one needed merely the correct updating of #m_pending_logs_sz,
   * the mutex #m_throttling_mutex would not be necessary: `.fetch_add(mem_cost(...), relaxed)`
   * and `.fetch_sub(mem_cost(...), relaxed)` would have worked perfectly with no corruption or unexpected
   * reordering; each read/modify/write op is atomic, and that is sufficient.  Essentially the mutex was needed
   * only to synchronize subsequent potential assignment of #m_throttling_now.
   *
   * @see Class doc header Impl section for discussion of the throttling algorithm and locking in particular.
   */
  size_t m_pending_logs_sz;

  /**
   * Contains the output of the always-on throttling algorithm; namely
   * `true` if currently should_log() shall return `false` due to too-much-RAM-being-used; `false` otherwise.
   * It starts at `false`; when `m_throttling_cfg.m_hi_limit` is crossed (by #m_pending_logs_sz) going up
   * in do_log(), it is made equal to `true`; when reaching 0 it is made equal to `false`.
   *
   * Protected by #m_throttling_mutex.  *Additionally* it is `atomic`, so that should_log() can read it
   * without locking.  should_log() does not care about the other items protected by the mutex, and it for
   * functional purposes does not care about inter-thread volatility due to `relaxed`-order access to this
   * flag around the rare occasions when its value actually changes.
   *
   * @see Class doc header Impl section for discussion of the throttling algorithm and locking in particular.
   *
   * At least for logging purposes we do want to detect when it *changes* from `false` to `true` and vice versa;
   * this occurs only the 1st time it reaches `hi_limit` since it was last 0; and similarly the 1st time it reaches
   * 0 since it was last `>= hi_limit`.
   */
  std::atomic<bool> m_throttling_now;

  /**
   * Whether the throttling-based-on-pending-logs-memory-used feature is currently active or not. As explained
   * in detail in Throttling section in class doc header, this is queried only in should_log() and only as a
   * gate to access the results of the always-on throttling algorithm: #m_throttling_now.  That algorithm,
   * whose data reside in other `m_throttling_*` and #m_pending_logs_sz, is always active; but this
   * `m_throttling_active` flag determines whether that algorithm's output #m_throttling_now is used or
   * ignored by should_log().
   *
   * It is atomic, and accessed with `relaxed` order only, due to being potentially frequently accessed in
   * the very-often-called should_log().  Since independent of the other state, it does not need mutex protection.
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
   *
   * @todo Async_file_logger::m_serial_logger (and likely a few other similar `unique_ptr` members of other
   * classes) would be slightly nicer as an `std::optional<>` instead of `unique_ptr`.  `optional` was not
   * in STL back in the day and either did not exist or did not catch our attention back in the day.  `unique_ptr`
   * in situations like this is fine but uses the heap more.
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
