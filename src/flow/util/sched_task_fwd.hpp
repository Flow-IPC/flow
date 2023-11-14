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

#include "flow/util/util_fwd.hpp"
#include "flow/log/log_fwd.hpp"
#include <boost/shared_ptr.hpp>

namespace flow::util
{

// Types.

// Find doc headers near the bodies of these compound types.

struct Scheduled_task_handle_state;

/**
 * Black-box type that represents a handle to a scheduled task as scheduled by
 * schedule_task_at() or schedule_task_from_now() or similar, which can be (optionally) used to control the
 * scheduled task after it has been thus scheduled.  Special value `Scheduled_task_handle()` represents an invalid task
 * and can be used as a sentinel, as with a null pointer.
 *
 * Values of this type are to be passed around by value, not reference.  They are light-weight.
 * Officially, passing it by reference results in undefined
 * behavior.  Unofficially, it will probably work (just haven't thought super-hard about it to make sure) but will
 * bring no performance benefit.
 */
using Scheduled_task_handle = boost::shared_ptr<Scheduled_task_handle_state>;

/**
 * Equivalent to `Scheduled_task_handle` but refers to immutable version of a task.
 * One can safely construct a `Scheduled_task_const_handle` directly from `Scheduled_task_handle` (but not vice versa;
 * nor would that compile without some kind of frowned-upon `const_cast` or equivalent).
 */
using Scheduled_task_const_handle = boost::shared_ptr<const Scheduled_task_handle_state>;

/**
 * Short-hand for tasks that can be scheduled/fired by schedule_task_from_now() and similar.  `short_fire` shall be
 * set to `false` if the task executed based on its scheduled time; `true` if it is fired early due to
 * scheduled_task_short_fire().
 */
using Scheduled_task = Function<void (bool short_fire)>;

// Free functions.

/**
 * Schedule the given function to execute in a certain amount of time: A handy wrapper around #Timer (asio's timer
 * facility).  Compared to using #Timer, this has far simplified semantics at the cost of certain
 * less-used features of #Timer.  Recommend using this facility when sufficient; otherwise use #Timer directly.
 * The trade-offs are explained below, but first:
 *
 * ### Semantics ###
 * Conceptually this is similar to JavaScript's ubiquitous (albeit single-threaded) `setTimeout()` feature.
 * The given function shall execute as if `post()`ed onto the given `Task_engine`; unless successfully canceled by
 * `scheduled_task_cancel(X)`, where X is the (optionally used and entirely ignorable) returned handle.  Barring
 * unrelated crashes/etc. there are exactly three mutually exclusive outcomes of this function executing:
 *   - It runs at the scheduled time, with the `bool short_fire` arg to it set to `false`.
 *   - It runs before the scheduled time, with `short_fire == true`.  To trigger this, use scheduled_task_short_fire()
 *     before the scheduled time.
 *     - If this loses the race with normal firing, the short-fire function will indicate that via its return value.
 *   - It never runs.  To trigger this, use scheduled_task_cancel() before the scheduled time.
 *     - If this loses the race with normal firing, the cancel function will indicate that via its return value.
 *
 * All related functions are thread-safe w/r/t a given returned `Scheduled_task_handle`, if `single_threaded == false`.
 * In addition, for extra performance (internally, by omitting certain locking),
 * set `single_threaded = true` only if you can guarantee the following:
 *   - `*task_engine` is `run()`ning in no more than one thread throughout all work with the returned
 *     `Scheduled_task_handle` including the present function.
 *   - Any calls w/r/t the returned `Scheduled_task_handle` are also -- if at all -- called from that one thread.
 *
 * The minimum is step 1: Call schedule_task_at() or schedule_task_from_now(). Optionally, if one saves the return
 * value X from either, one can do step 2: `scheduled_task_short_fire(X)` or `scheduled_task_cancel(X)`, which
 * will succeed (return `true`) if called sufficiently early.  There is no step 3; any subsequent calls on X
 * will fail (return `false`).
 *
 * ### Simplifications over #Timer ###
 * - There is no need to, separately, create a #Timer object; set the firing time; and kick off the
 *   asynchronous wait.  One call does everything, and there is no object to maintain.  Sole exception to the latter:
 *   *if* you want to be able to cancel or short-circuit the task later on, you can optionally save the return value
 *   and later call a `scheduled_task_*()` function on it.)
 * - You need not worry about internal errors -- no need to worry about any `Error_code` passed in to your task
 *   function.
 * - Cancellation semantics are straightforward and intuitive, lacking corner cases of vanilla #Timer.  To wit:
 *   - A successful scheduled_task_cancel() call means the task will *not* execute.  (`Timer::cancel()` means it
 *     will still execute but with `operation_aborted` code.)
 *   - `Timer::cancel()` can result in ~3 behaviors: it already executed, so it does nothing; it has not yet executed
 *     but was JUST about to execute, so it will still execute with non-`operation_aborted` code; it has not yet
 *     executed, so now it will execute but with `operation_aborted`.  With this facility, it's simpler:
 *     it either succeeds (so acts per previous bullet -- does not execute); or it fails, hence it will have executed
 *     (and there is no `Error_code` to decipher).
 * - Short-firing semantics are arguably more intuitive.  With #Timer, `cancel()` actually means short-firing
 *   (firing ASAP) but with a special `Error_code`.  One can also change the expiration time to the past
 *   to short-fire in a different way.  With this facility, cancellation means it won't run; and
 *   scheduled_task_short_fire() means it will fire early; that's it.
 *
 * ### Features lost vs. #Timer ###
 * - A #Timer can be reused repeatedly.  This *might* be more performant than using this facility repeatedly, since
 *   (internally) that approach creates a #Timer each time.  Note: We have no data about the cost of initializing a new
 *   #Timer, other than the fact I peeked at the boost.asio source and saw that the construction of a #Timer isn't
 *   obviously trivial/cheap -- which does NOT mean it isn't just cheap in reality, only that that's not immediately
 *   clear.
 *   - A rule of thumb in answering the question, 'Is this facility fine to just call repeatedly, or should we reuse
 *     a `Timer` for performance?': It *might* not be fine if and only if timer firings and/or cancellations occur
 *     many times a second in a performance-sensitive environment.  E.g., if it's something fired and/or canceled
 *     every second repeatedly, it's fine; it it's packet pacing that must sensitively fire near the resolution limit
 *     of the native #Timer facility, it *might* not be fine, and it's safer to use #Timer repeatedly.
 * - A #Timer can be re-scheduled to fire at an arbitrary different time than originally set.  We provide no such
 *   facility.  (We could provide such an API at the cost of API and implementation complexity; it's a judgment call,
 *   but I feel at that point just use a #Timer.)
 *   - However, we provide one special case of this: the timer can be fired
 *     ASAP, a/k/a short-fired, via scheduled_task_short_fire().
 * - One can (incrementally) schedule 2+ tasks to fire at the scheduled time on one #Timer; this facility only takes
 *   exactly 1 task, up-front. (We could provide such an API, but again this feels like it defeats the point.)
 * - #Timer has certain informational accessors (like one that returns the scheduled firing time) that we lack.
 *   (Again, we could provide this also -- but why?)
 *
 * @todo We could eliminate schedule_task_from_now() potential limitation versus #Timer wherein each call constructs
 * (internally) a new #Timer.  A pool of `Timer`s can be internally maintained to implement this.  This may or may not
 * be worth the complexity, but if the API can remain identically simple while cleanly eliminating the one perf-related
 * reason to choose #Timer over this simpler facility, then that is a clean win from the API user's point of view.
 * By comparison, other possible improvements mentioned *complicate* the API which makes them less attractive.
 *
 * @see util::Timer doc header for native #Timer resolution limitations (which apply to quite-low `from_now` values).
 * @note Design note: This is a small, somewhat C-style set of functions -- C-style in that it returns a handle
 *       on which to potentially call more functions as opposed to just being a class with methods.  This is
 *       intentional, because #Timer already provides the stateful `class`.  The picture is slightly muddled because
 *       we DO provide some "methods" -- so why not make it a `class` after all, just a simpler one than #Timer?
 *       Answer: I believe this keeps the API simple: Step 1: Schedule it. Step 2 (optional): Short-fire or cancel it.
 *       There are no corner cases introduced as might have been via increased potential statefulness inherent with a
 *       class.  But see the following to-do.
 *
 * @todo schedule_task_from_now() and surrounding API provides an easy way to schedule a thing into the future, but
 * it is built on top of boost.asio util::Timer directly; an intermediate wrapper class around this would be quite
 * useful in its own right so that all boost.asio features including its perf-friendliness would be retained along
 * with eliminating its annoyances (around canceling-but-not-really and similar).  Then scheduled_task_from_now()
 * would be internally even simpler, while a non-annoying util::Timer would become available for more advanced use
 * cases.  echan may have such a class (in a different project) ready to adapt (called `Serial_task_timer`).
 * I believe it internally uses integer "task ID" to distinguish between scheduled tasks issued in some chronological
 * order, so that boost.asio firing a task after it has been canceled/pre-fired can be easily detected.
 *
 * @param logger_ptr
 *        Logging, if any -- including in the background -- will be done via this logger.
 * @param from_now
 *        Fire ASAP once this time period passes (0 to fire ASAP).  A negative value has the same effect as 0.
 * @param single_threaded
 *        Set to a true value if and only if, basically, `*task_engine` is single-threaded, and you promise not
 *        to call anything on the returned `Scheduled_task_handle` except from that same thread.  More formally, see
 *        above.
 * @param task_engine
 *        The `Task_engine` onto which the given task may be `post()`ed (or equivalent).
 * @param task_body_moved
 *        The task to execute within `*task_engine` unless successfully canceled.  See template param doc below also
 *        regarding `Strand` and other executor binding.
 * @return Handle to the scheduled task which can be ignored in most cases.  If you want to cancel, short-fire, etc.
 *         subsequently, save this (by value) and operate on it subsequently, e.g., with scheduled_task_cancel().
 *
 * @tparam Scheduled_task_handler
 *         Completion handler with signature compatible with `void (bool short_fired)`.
 *         This allows for standard boost.asio semantics, including associating with an executor such as a
 *         `boost::asio::strand`.  In particular you may pass in: `bind_executor(S, F)`, where
 *         `F(bool short_fire)` is the handler, and `S` is a #Strand (or other executor).
 *         Binding to a #Strand will ensure the fired or short-fired body will not execute concurrently with
 *         any other handler also bound to it.
 */
template<typename Scheduled_task_handler>
Scheduled_task_handle schedule_task_from_now(log::Logger* logger_ptr,
                                             const Fine_duration& from_now, bool single_threaded,
                                             Task_engine* task_engine,
                                             Scheduled_task_handler&& task_body_moved);

/**
 * Identical to schedule_task_from_now() except the time is specified in absolute terms.
 *
 * ### Performance note ###
 * The current implementation is such that there is no performance benefit to using
 * `schedule_task_from_now(at - Fine_clock::now(), ...)` over `schedule_task_at(at, ...)`.  Therefore, if it
 * is convenient for caller's code reuse to do the former, there is no perf downside to it, so feel free.
 *
 * @internal
 * ### Maintenance reminder ###
 * Ensure the "Performance note" is accurate w/r/t to the body of the function; as of this writing it is; keep
 * the two places in sync with each other.
 * @endinternal
 *
 * @param logger_ptr
 *        See schedule_task_from_now().
 * @param at
 *        Fire at this absolute time.  If this is in the past, it will fire ASAP.
 * @param single_threaded
 *        See schedule_task_from_now().
 * @param task_engine
 *        See schedule_task_from_now().
 * @param task_body_moved
 *        See schedule_task_from_now().
 * @return See schedule_task_from_now().
 *
 * @tparam Scheduled_task_handler
 *         See schedule_task_from_now().
 */
template<typename Scheduled_task_handler>
Scheduled_task_handle schedule_task_at(log::Logger* logger_ptr,
                                       const Fine_time_pt& at, bool single_threaded,
                                       Task_engine* task_engine,
                                       Scheduled_task_handler&& task_body_moved);

/**
 * Attempts to reschedule a previously scheduled (by schedule_task_from_now() or similar) task to fire immediately.
 * For semantics, in the context of the entire facility, see schedule_task_from_now() doc header.
 *
 * @param logger_ptr
 *        See schedule_task_from_now().
 * @param task
 *        (Copy of) the handle returned by a previous `schedule_task_*()` call.
 * @return `true` if the task will indeed have executed soon with `short_fire == true`.
 *         `false` if it has or will soon have executed with `short_file == false`; or if it has already been
 *         successfully canceled via scheduled_task_cancel().
 */
bool scheduled_task_short_fire(log::Logger* logger_ptr, Scheduled_task_handle task);

/**
 * Attempts to prevent the execution of a previously scheduled (by schedule_task_from_now() or similar) task.
 * For semantics, in the context of the entire facility, see schedule_task_from_now() doc header.
 *
 * @param logger_ptr
 *        See scheduled_task_short_fire().
 * @param task
 *        See scheduled_task_short_fire().
 * @return `true` if the task has not executed and will NEVER have executed, AND no other scheduled_task_cancel()
 *         with the same arg has succeeded before this.
 *         `false` if it has or will soon have executed because the present call occurred too late to stop it.
 *         Namely, it may have fired or short-fired already.
 */
bool scheduled_task_cancel(log::Logger* logger_ptr, Scheduled_task_handle task);

/**
 * Returns how long remains until a previously scheduled (by schedule_task_from_now() or similar) task fires;
 * or negative time if that point is in the past; or special value if the task has been canceled.
 *
 * This is based solely on what was specified when scheduling it; it may be different from when it will actually fire
 * or has fired.  However, a special value (see below) is returned, if the task has been canceled
 * (scheduled_task_cancel()).
 *
 * @param logger_ptr
 *        Logging, if any, will be done synchronously via this logger.
 * @param task
 *        (Copy of) the handle returned by a previous `schedule_task_*()` call.
 * @return Positive duration if it is set to fire in the future; negative or zero duration otherwise; or
 *         special value `Fine_duration::max()` to indicate the task has been canceled.
 *         Note a non-`max()` (probably negative) duration will be returned even if it has already fired.
 */
Fine_duration scheduled_task_fires_from_now_or_canceled(log::Logger* logger_ptr, Scheduled_task_const_handle task);

/**
 * Returns whether a previously scheduled (by schedule_task_from_now() or similar) task has already fired.
 * Note that this cannot be `true` while scheduled_task_canceled() is `false` and vice versa (but see thread safety note
 * below).
 *
 * Also note that, while thread-safe, if `!single_threaded` in the original scheduling call, then the value
 * returned might be different even if checked immediately after this function exits, in the same thread.
 * However, if `single_threaded` (and one indeed properly uses `task` from one thread only) then it is guaranteed
 * this value is consistent/correct synchronously in the caller's thread, until code in that thread actively changes it
 * (e.g., by canceling task).
 *
 * @param logger_ptr
 *        See scheduled_task_fires_from_now_or_canceled().
 * @param task
 *        See scheduled_task_fires_from_now_or_canceled().
 * @return See above.
 */
bool scheduled_task_fired(log::Logger* logger_ptr, Scheduled_task_const_handle task);

/**
 * Returns whether a previously scheduled (by schedule_task_from_now() or similar) task has been canceled.
 * Note that this cannot be `true` while scheduled_task_fired() is `false` and vice versa (but see thread safety note
 * below).
 *
 * Thread safety notes in the scheduled_task_fired() doc header apply equally here.
 *
 * @param logger_ptr
 *        See scheduled_task_fired().
 * @param task
 *        See scheduled_task_fired().
 * @return See above.
 */
bool scheduled_task_canceled(log::Logger* logger_ptr, Scheduled_task_const_handle task);

} // namespace flow::util
