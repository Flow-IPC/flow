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

#include "flow/async/detail/async_fwd.hpp"
#include "flow/util/util_fwd.hpp"
#include "flow/util/sched_task_fwd.hpp"
#include <boost/any.hpp>


/**
 * Flow module containing tools enabling multi-threaded event loops operating under the asynchronous-task proactor
 * pattern, by providing a streamlined API around boost.asio event loops with added advanced task- and
 * thread-scheduling features.  There is also support for single-threaded event loops.
 *
 * In simpler terms, at its core -- including when the "pool" has just one thread, which
 * is very common -- it provides a compact way of both starting thread(s) *and* posting/scheduling tasks and I/O
 * to run in such thread(s).  By default one must worry about each of those 2 concerns separately and decide how
 * exactly to hook them up; which is not rocket science, but it *is* a ton of boiler-plate, and it *is* easy to
 * make mistakes and/or omit useful startup/shutdown practices, logging, and more.  This module provides, via
 * Concurrent_task_loop and its implementations, at least that consistency/standardization.  Plus, it provides
 * certain advanced features as mentioned above.
 *
 *   - boost.asio provides the core algorithmic abilities of an optionally multi-threaded task-executing loop,
 *     particularly through classes util::Task_engine (a/k/a `boost::asio::io_context`), util::Strand
 *     (a/k/a `strand` in `boost::asio`), and util::Timer.  flow::async Flow module somewhat streamlines
 *     this API in such a way as to keep the user's focus on their conceptual async-task-driven algorithm as opposed
 *     to details of threads, handlers, cores, etc.  The async::Op opaque type is central to this streamlined API,
 *     plus the central class Concurrent_task_loop.
 *     - The bottom line is the user thinks about their algorithm in
 *       terms of tasks; while the internals of the chosen Concurrent_task_loop concrete object worry about the
 *       actual scheduling of these tasks across threads.
 *   - boost.asio doesn't really provide ways to specify how threads should be assigned to processor cores; it only
 *     controls what code is executed on which thread.  These abilities are available natively.  flow::async Flow
 *     module allows one to set certain knobs controlling this behavior, and the user can continue to only worry
 *     about their algorithm and not threading details.
 *     - The combination of the generalized async::Op mechanism and these thread-hardware-scheduling features
 *       in an integrated whole is what hopefully makes the Flow `async` module a value-add over just boost.asio,
 *       or over just boost.asio with some thread-core-affinity utility functions on the side.
 *
 * @see The central type is the interface class Concurrent_task_loop.  For single-thread async work,
 *      which is very common, see Single_thread_task_loop, a simplified adapter, similar to how `std::queue<T>`
 *      is commongly a simplified `std::deque<T>` or `list` underneath.
 * @see async::Op.
 *
 * @internal
 *
 * @todo The thread-to-core optimizations provided at this time are, at least, a good start, but more advanced logic
 * can be devised with more low-level experience and/or by using certain open-source libraries.  It's possible that
 * a more knowledgeable person would devise more or better knobs and/or require less manual specification of
 * values.  The following background reading may help devise
 * more advanced logic and/or knobs:
 * [ https://eli.thegreenplace.net/2016/c11-threads-affinity-and-hyperthreading/ |
 * https://mirrors.edge.kernel.org/pub/linux/kernel/people/paulmck/perfbook/perfbook.2016.07.31a.pdf |
 * https://lwn.net/Articles/255364/ | "hwloc" library (portable lib for detailed hardware topology info) |
 * libNUMA ].
 */
namespace flow::async
{
// Types.

// Find doc headers near the bodies of these compound types.

class Cross_thread_task_loop;
class Concurrent_task_loop;
class Op_list;
class Segregated_thread_task_loop;
class Single_thread_task_loop;
class Timed_single_thread_task_loop;
class Timed_concurrent_task_loop;
template<typename Time_accumulator>
class Timed_concurrent_task_loop_impl;

/**
 * Short-hand for a task that can be posted for execution by a Concurrent_task_loop or flow::util::Task_engine;
 * it is simply something callable via `()` with no arguments and returning nothing.
 *
 * By convention in comments we represent `Task`s with the letters F, G, H.
 */
using Task = Function<void ()>;

/**
 * An object of this opaque type represents a collection of 1 or more async::Task, past or future, such that:
 * *if* one performs `C->post(J, F)` and `C->post(K, G)` (where C is `Concurrent_task_loop*`, JK are the same
 * `async::Op&`, or one refers to a transitive copy of the other, and FG are both `Task`s), *then*
 * F and G will NOT execute concurrently.
 *
 * In addition, it is guaranteed that copying (via constructor or assignment) of async::Op is
 * has performance characteristics no worse than those of `shared_ptr`.  I.e., it is to be thought of as light-weight.
 *
 * The value `Op()` is designated as a null/sentinel value and must not be passed to Concurrent_task_loop::post()
 * or anything built on it.
 *
 * That's the formal definition.  We reiterate that copying these is cheap; and moreover two `Op`s such that
 * one is a copy (of a copy, of a copy, of a copy...) of another, then these are conceptually isomorphic: they
 * represent the same op, or collection of `Task`s that must never execute concurrently.  Finally, tip: Don't think
 * of an `Op` as a collection of 2+ `Task`s; but rather a tag or label that associates 2+ `Task`s with each other.
 * (Also, nothing prevents an async::Task being a part of 2+ `Op`s simultaneously, though informally speaking
 * it's arguably best not to make code maintainers grok such a design.)
 *
 * By convention in comments we represent `Op`s with the letters J, K, L.
 *
 * ### When to use an `Op` versus just a stand-alone `Task`? ###
 * When choosing a Concurrent_task_loop::post() (the one with `Op` vs. one without; or similar choices in more
 * advanced cases), here are some things to remember.  These can be derived independently and are only included
 * as a convenience/refresher:
 *
 *   - An `Op` prevents `Task`s from executing concurrently.  If there is exactly 1 thread in a pool, then they couldn't
 *     anyway, so an `Op` is not needed...
 *     - ...except as future-proofing, in case conceivably 1 thread might soon turn into 2+ after all.
 *   - It is quite common to follow the pattern wherein, as the very last statement to execute within a `Task`,
 *     one `post()`s (or similar) exactly 1 `Task` to asynchronously execute next.  Since it's the last statement,
 *     and `post()` and similar are explicitly thread-safe, this ensures the current and next `Task`s do not execute
 *     concurrently.  So an `Op` is not needed...
 *     - ...except as future-proofing.  It is sometimes easier to maintain, and more expressive to read, when many
 *       `Task`s are "officially" run under the banner of a single `Op`, even if some parts of the async handling of
 *       the "conceptual" operation are serial and hence don't technically require an `Op`.  Example: In a web
 *       server it is reasonable to create an `Op` for the entire request, with all `Task`s (even serially called ones)
 *       being associated with that per-request `Op`; then, simply, no locking is necessary for per-request data
 *       structure(s).  It's much easier to explain, "you don't need to lock 'em," vs. "you don't need to lock 'em,
 *       unless the logic changes in such a way as to...."
 *
 * To be fair, those tips ignore performance; they implicitly assume using `Op` pointlessly (functionally, not
 * stylistically, so) is otherwise "free."  It is *not* free; depending on internal details using `Op` might involve
 * a util::Strand and/or pinning stuff to a specific thread.  Informally, this is seldom a big deal in practice;
 * but in performance-sensitive projects one must remember there is a cost.
 *
 * @internal
 *
 * `boost::any` (storing nothing heavier-weight than a `shared_ptr` to satisfy that explicit guarantee above) is one
 * way to do this.  A rigid polymorphic hierarchy (`virtual`) is another.  Performance-wise they're similar, costing
 * essentially a single `virtual` pointer lookup per Concurrent_task_loop::post().  boost.any is much pithier, not
 * requiring a class hierarchy at all, and otherwise it's pretty much the same in terms of how they're used internally.
 * Some might say the polymorphic hierarchy is clearer, because it is explicit, but I feel comments alone, too, can
 * be just as clear, and brevity is a virtue.
 */
using Op = boost::any;

/**
 * Similar to flow::async::Task but used for scheduled-in-future tasks as opposed to to-be-run-ASAP tasks.
 * In practice it's the same thing but takes a single `bool` argument with the meaning explained
 * in util::schedule_task_from_now() doc header (spoiler alert: whether it ran as scheduled or was short-fired by
 * user, as of this writing).
 *
 * @note Whenever a comment explains how `Task`s are dealt with, one may usually assume the same extends equally to
 *       a `Scheduled_task`, merely at a different point in time.  We omit that explicit language for brevity;
 *       it is to be assumed.
 */
using Scheduled_task = util::Scheduled_task;

/**
 * Short-hand for a boost.asio completion handler: The minimal type, taking only a flow::Error_code
 * a/k/a `boost::system::error_code`.
 */
using Task_asio_err = Function<void (const Error_code&)>;

/**
 * Short-hand for a boost.asio completion handler: The type that takes a `size_t` count of things successfully
 * transferred usually.
 */
using Task_asio_err_sz = Function<void (const Error_code&, size_t)>;

/**
 * Short-hand for reference-counting pointer to a mutable util::Task_engine (a/k/a `boost::asio::io_context`).
 * This is generally how classes in the Concurrent_task_loop hierarchy refer to their internally used
 * `Task_engine`s but also in advanced cases may be communicated to their user.
 *
 * @internal
 *
 * ### Rationale ###
 * Why do that instead of using raw `Task_engine*`?  It may not be obvious as you read
 * this now, but all kinds of pain goes away due to the possibilities of who
 * happens to own an underlying `Task_engine` -- it can be shared by all threads in pool, or each can have its own
 * `Task_engine`, depending on the chosen Concurrent_task_loop subclass -- and in what order those things
 * might execute their destructors.  By simply using a `shared_ptr<>` everywhere, with small overhead we ensure
 * an underlying `Task_engine` does not get destroyed until everything that uses it is destroyed first, and
 * that could be a number of things.  By using `shared_ptr<>` we needn't break our heads worrying about executing
 * de-init pieces of code in just the right order just to avoid early-free.  (This is said from experience.)
 *
 * This is reinforced in those semi-advanced cases where a `Task_engine` is passed to user via public API.
 */
using Task_engine_ptr = boost::shared_ptr<util::Task_engine>;

/**
 * Short-hand for ref-counted pointer to util::Strand.
 *
 * @internal
 *
 * ### Rationale ###
 * We at times return new `Strand`s (Cross_thread_task_loop::create_op()), so universally use ref-counted pointers
 * to `Strand`s to not have to worry about `Strand` lifetimes too hard.
 *
 * Key fact: The type Cross_thread_task_loop loads into superclass's async::Op (`boost::any`) is `Strand_ptr`.
 * That is, a `Strand` is the mechanism used to bundle together non-concurrent tasks in Cross_thread_task_loop.
 */
using Strand_ptr = boost::shared_ptr<util::Strand>;

/**
 * Enumeration indicating the manner in which asio_exec_ctx_post(), and various boost.asio "post" operations like
 * it or based on it, are to actually execute the given task in relation to when the "posting" routine, itself,
 * returns control to its caller.  Basically it indicates whether the execution should be synchronous or asynchronous
 * and how, if it all, to wait for its completion -- or its initiation.
 * The `enum` members' meanings are the key things to understand;
 * and there's some discussion in their doc headers that might be useful as a boost.asio refresher.
 */
enum class Synchronicity
{
  /**
   * Simply post the given task to execute asynchronously in some execution context -- as soon as the context's
   * scheduler deems wise but specifically *not* inside the posting routine itself; and return as soon as possible
   * having thus posted it.  That is: work in the manner of boost.asio `post(Task_engine, F)`.
   *
   * In particular, suppose you're calling `POST(F)` in this mode, where
   * `POST()` is some posting routine controlling a thread pool P, and `F()` is the task; and suppose the scheduler
   * would deem wise to run `F()` in some thread P.W in that pool (perhaps for load-balancing reasons).  Then:
   *   - If `POST()` is being called outside of pool P, or it is being called from a sibling
   *     thread P.W' but not W itself, then `F()` will run at some point in the future (possibly even
   *     concurrently with `POST()` itself), in thread P.W.
   *     - One typical case is when some external user of P loads work onto P.
   *     - The other is if some task or completion handler already in P loads async work back
   *       onto its own pool P, but the scheduler decides it's best for it to run in a different thread than
   *       the posting code.
   *       - This is only possible with 2 or more threads in P (by no means always the case).
   *   - If `POST()` is being called from W itself, meaning the scheduler decided that the task should load
   *     on the same thread as the posting task, then `F()` will run at some point in the future strictly after the
   *     `POST()` returns.
   *     - This usually means some task or completion handler in pool P is loading async work back onto its own
   *       pool, and either that pool contains only 1 thread (so there is no other choice), or else
   *       the scheduler decided the calling thread is still the best choice for task `F()` at this time
   *       (e.g., maybe the other thread(s) are loaded with queued work).
   *
   * Either way, `POST()` will return quickly.  Then `F()` will either run concurrently or after this return -- but
   * never *in* `POST()` synchronously.
   */
  S_ASYNC,

  /**
   * Same as Synchronicity::S_ASYNC but the posting routine then waits as long as necessary for the given task to
   * complete; and only then returns.  That is: work in the manner of boost.asio `post(Task_engine, F)`, but
   * wait until `F()` actually runs and returns.  This must only be used when posting from a thread *outside* the
   * target thread pool; or undefined behavior will result.
   *
   * @warning One must *not* use this mode when posting onto a thread pool from inside
   *          that thread pool: boost.asio `post()`-like function by definition won't execute a task synchronously
   *          inside itself, yet by the definition of this mode it must also wait for the task to run and complete.
   *          So if `post()` (or similar) were to decide the task belongs on the calling thread, an inifinite
   *          block (deadlock) occurs, as it will be waiting for something to happen that the wait prevents from
   *          happening.
   *
   * This mode is reminiscent of the promise/future concept and allows one to easily solve the age-old problem of
   * "how do I ask a thread/pool to do a thing and then wait for get a result?".  One might do this by manually
   * using a promise/future pair; or even mutex/condition variable pair; but by using this such boiler-plate is
   * reduced (along with fewer bugs).
   *
   * @warning Be aware that the wait for completion will block infinitely, if one were to do something that would
   *          prevent the task from ever running.  When working with boost.asio `Task_engine`s, this may occur
   *          when one `stop()`s or simply destroys the `Task_engine` (though the time period during which
   *          one would have to do this is short, assuming the task is quick).  Naturally the way to avoid this is
   *          by not stopping or destroying the execution context during a posting call in
   *          mode `S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION`.  For example, the `Task_engine::stop()` call might
   *          be placed in the same thread as the posting; then they cannot be concurrent.  If this is triggered from
   *          a SIGTERM/INT handler, one might only set or post something that will trigger the destruction in an
   *          orderly fashion at the proper time as opposed to doing it directly from the sig handler itself.
   *          This warning isn't anything that should be particularly new -- orderly shutdown is typically concerned
   *          with such logic anyway -- but it seemed worth putting in perspective of the fact this mode involves
   *          a wait for something that doesn't necessarily ever run, unless you actively make sure it does.
   *
   * @todo Much like the promise/future mechanism provides optional timed wait functionality, it might make sense
   * to provide the API ability to set an optional time limit for any wait invoked by
   * Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION or Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START.
   * Probably best to add this only once a need clearly arises though.
   */
  S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION,

  /**
   * Same as Synchronicity::S_ASYNC but the posting routine then waits as long as necessary for the given task to
   * *just* about to begin executing concurrently (so that any subsequent `Task_engine::stop()` shall be unable to
   * prevent it from executing and eventually finishing) -- and only then returns.
   *
   * This is most similar to `S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION` but may improve responsiveness of the
   * calling thread, if what one needs to achieve is a guarantee that `F()` *will definitely* execute and complete,
   * but does *not* need to wait for this to happen.  So it's a weapon against a "dangling" `post()` that might
   * be followed immediately by `Task_engine::stop()` -- while not blocking until the posted thing finishes.
   *
   * @warning One must *not* use this mode when posting onto a thread pool from inside
   *          that thread pool; as with `S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION` that may hang the thread.
   */
  S_ASYNC_AND_AWAIT_CONCURRENT_START,

  /**
   * Execute the given task synchronously, if the scheduler determines that the calling thread is in its thread pool
   * *and* is the best thread for the task; otherwise act identically to Synchronicity::S_ASYNC.  That is: work in the
   * manner of boost.asio `dispatch(Task_engine, F)`.  This can be useful for performance, since when the opportunity
   * presents itself this way avoids exiting a task only to immediately enter the posted task, when one could just
   * synchronously execute one after the other.
   *
   * @warning Do *not* presume to know when a given scheduler will actually decide it will invoke the given task
   *          synchronously, unless documentation very clearly explains such rules.  Just because it can does not
   *          mean it will.  For example, boost.asio `post()` says that the task "might" run synchronously when this is
   *          possible; not that it "will."  Assumptions about when it might in fact do so can lead to subtle and
   *          difficult-to-reproduce bugs.  (Example of broken assumptions: Suppose it's a 1-thread pool, and one
   *          posts F from task G.  Surely it must run F synchronously -- there's no other thread!  But what if some
   *          other task or completion handler was already queued up to run before F was?  That's not even the point
   *          though; the scheduler is still free to not do it, say because of some spurious lock-related logic that
   *          is there for some obscure performance reason.)
   *
   * @warning If you choose to use this mode (or `dispatch()`-like routines in general), it is almost never a good idea
   *          to do so from anywhere except just before returning from a task (or from outside the thread pool).
   *          If called from the middle of a task, you now cannot be sure if A happens before B or B happens before A.
   *          Usually that makes things complicated unnecessarily.
   */
  S_OPPORTUNISTIC_SYNC_ELSE_ASYNC
}; // enum class Synchronicity

// Free functions.

/**
 * An extension of boost.asio's `post()` and `dispatch()` free function templates, this free function template
 * allows the user to more easily select the synchronicity behavior as the given task is posted onto the
 * given execution context (util::Task_engine or util::Strand at least).  It also adds TRACE logging including
 * that book-ending the task's execution (aiding debugging, etc.).  The `synchronicity` argument controls the
 * specific way in which `task` is posted onto `*exec_ctx`; see #Synchronicity doc header.
 *
 * This call causes `task` to execute in a thread controlled by `*exec_ctx`.  The latter, at this time, must be
 * either util::Task_engine or util::Strand (which itself is born of a `Task_engine`).  It is likely that it will
 * work equally well for other entities satisfying the boost.asio `ExecutionContext` concept (see boost.asio docs), but
 * this is untested and not thought through formally, so officially such uses cause undefined behavior as of this
 * writing.
 *
 * Semantics can be found in #Synchronicity doc headers which are required reading before using this function.
 * However, briefly and informally, the utility of this function is as follows:
 *   - `post()` works on a `Task_engine` or `Strand` already and equals
 *     mode Synchronicity::S_ASYNC; with this function you'll get some added debug logging as well.
 *   - `dispatch()` and Synchronicity::S_OPPORTUNISTIC_SYNC_ELSE_ASYNC are similarly related: same thing
 *     but more logging.
 *   - Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION equals a `post()` with a promise/future pair,
 *     wherein the caller performs `unique_future.wait()` after the `post()`, while the task always sets
 *     `promise.set_value()` just before returning (and, again, more logging).
 *   - Lastly, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START is similar, but the `promise.set_value()`
 *     executes just *before* executing `task()`.  Hence any tasks queued before `task()` will first execute
 *     (same as previous bullet); and *then* asio_exec_ctx_post() will return, just as `task()` begins executing
 *     as opposed to wait for its completion.
 *     This is useful to prevent `E.stop()` after our return (where `E` is the `Task_engine` that either is
 *     or produced `*exec_ctx`) will be too late to prevent `task()` from executing and completing.
 *
 * In all cases, one gets more logging and arguably a bit of syntactic sugar, but
 * `S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION` and `S_ASYNC_AND_AWAIT_CONCURRENT_START` in particular eliminate
 * quite a bit of tedious and hairy code and explanations.
 *
 * Lastly, if `*exec_ctx` is currently not running, then the semantics described in #Synchronicity doc header
 * still apply but are deferred until it does run.  In particular in mode
 * Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION and Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START
 * this function shall not return until `*exec_ctx` at least
 * does begin running in at least 1 thread; while the other two modes reduce to `post()`, which returns immediately,
 * leaving `task()` to run once `*exec_ctx` starts.
 *
 * @tparam Execution_context
 *         util::Task_engine or util::Strand.  See note above regarding other possibilities.
 * @param logger_ptr
 *        Logger to use in this function.
 * @param exec_ctx
 *        The execution context controlling the thread pool onto which to load the `task` for ASAP execution.
 * @param synchronicity
 *        Controls the precise behavior.  See above.
 * @param task
 *        The task -- taking no arguments and returning no value -- to load onto `*exec_ctx`.
 */
template<typename Execution_context>
void asio_exec_ctx_post(log::Logger* logger_ptr, Execution_context* exec_ctx, Synchronicity synchronicity, Task&& task);

/**
 * Assuming a planned thread pool will be receiving ~symmetrical load, and its UX-affecting (in particular, per-op
 * latency-affecting) operations are largely between processor and RAM: Returns the # of threads to store in that pool
 * for efficient performance.
 *
 * @see After the threads are created, use optimize_pinning_in_thread_pool() to complete
 * the work that targets this good performance.
 *
 * This will be used, by doc header contract, by all (as of this writing) Concurrent_task_loop subclasses if
 * so specified via `n_threads_or_zero == 0`.  So in that context one needn't call this directly.  However, it may be
 * useful directly when one is operating a thread pool but without a Concurrent_task_loop.
 *
 * @param logger_ptr
 *        Logger to use in this function.
 * @param est_hw_core_sharing_helps_algo
 *        Set this to `true` if you estimate the intended use for this thread pool is such that 2+ identically
 *        loaded pool threads sharing 1 physical core would handle the load (in total over those 2+ threads) better than
 *        just 1 thread using that same core would.  Set it to `false` otherwise.
 *        Note that, generally, this should be assumed `false`, unless there is significant cache locality between
 *        those 2+ threads, meaning they tend to work on the same cacheably-small area in memory at ~the same time.
 *        For example, parallel matrix multiplication algorithms can thus benefit and would set it to `true`; but
 *        that is the not the case by default; one would have to prove it, or design the algorithm with that in mind.
 * @return The number of threads mandated for the thread pool in question.
 */
unsigned int optimal_worker_thread_count_per_pool(flow::log::Logger* logger_ptr,
                                                  bool est_hw_core_sharing_helps_algo);

/**
 * Assuming the same situation as documented for optimal_worker_thread_count_per_pool(), and that indeed
 * the pool now contains that number of running threads: Attempts to optimize thread-core-pinning behavior in that
 * pool for efficient performance.
 *
 * @see optimal_worker_thread_count_per_pool() first.  The two functions work together (one before, the other after
 *      spawning the threads).  Behavior is undefined if the two aren't used in coherent fashion, meaning one
 *      passed different values for same-named args.
 *
 * @note There is a to-do, as of this writing, to allow one to query system to auto-determine
 *       `hw_threads_is_grouping_collated` if desired.  See `namespace` flow::async doc header.
 *
 * @todo For the Darwin/Mac platform only: There is likely a bug in optimize_pinning_in_thread_pool() regarding
 *       certain low-level pinning calls, the effect of which is that this function is probably effectively a no-op for
 *       now in Macs.  The bug is explained inside the body of the function.
 *
 * @param logger_ptr
 *        Logger to use in this function.
 * @param threads_in_pool
 *        These raw threads, which must number `optimal_worker_thread_count_per_pool(same-relevant-args)`,
 *        comprise the pool in question.  They must be already spawned (e.g., have had some caller's code execute OK).
 * @param est_hw_core_sharing_helps_algo
 *        See optimal_worker_thread_count_per_pool().
 * @param est_hw_core_pinning_helps_algo
 *        Set this to `true` if you have reason to believe that pinning each of the pool's threads to N (N >= 1)
 *        logical cores would improve performance in some way.  Set it to `false` otherwise.
 *        As of this writing I don't know why it would be `true` specifically; but it can be researched; and I know
 *        in practice some applications do (in fact) do it, so it's not necessarily worthless, at least.
 * @param hw_threads_is_grouping_collated
 *        When the number of physical cores does not equal # of logical cores (hardware threads) -- otherwise
 *        this arg is ignored -- this determines the pattern in which each set of
 *        2+ core-sharing hardware threads is arranged vs. the other sets.  When `false`, it's like ABCDABCD, meaning
 *        logical cores 0,4 share core, 1,5 share different core, 2,6 yet another, etc.  When `true`, it's like
 *        AABBCCDD instead.  It seems `true` is either rare or non-existent, but I do not know for sure.
 */
void optimize_pinning_in_thread_pool(flow::log::Logger* logger_ptr,
                                     const std::vector<util::Thread*>& threads_in_pool,
                                     bool est_hw_core_sharing_helps_algo,
                                     bool est_hw_core_pinning_helps_algo,
                                     bool hw_threads_is_grouping_collated);

/**
 * Given a boost.asio *completion handler* `handler` for a boost.asio `async_*()` action on some boost.asio I/O
 * object to be initiated in the immediate near future, returns a wrapped handler with the same signature
 * to be passed as the handler arg to that `async_*()` action, so that `handler()` will execute non-concurrently
 * with other tasks in `Op op`.  This is analogous to boost.asio's `bind_executor(Strand, Handler)` (which
 * replaces boost.asio's now-deprecated `util::Strand::wrap(Handler)`).
 *
 * The mechanics of using this are explained in Concurrent_task_loop doc header.  Using this in any other
 * fashion leads to undefined behavior.
 *
 * @tparam Handler
 *         boost.asio handlers are, essentially, all `void`-returning but can take various arg sets.
 *         E.g., util::Timer (a/k/a `boost::asio::basic_waitable_timer`) expects a handler that takes only an
 *         `Error_code`; while `boost::asio::ip:tcp::socket::read_some()` expects one to take bytes-received `size_t`
 *         and an `Error_code`.  This template supports all handlers via `auto` magic.
 * @param loop
 *        Active loop that spawned `Op op`.
 * @param op
 *        See 3-arg Concurrent_task_loop::post().
 * @param handler
 *        Completion handler for the boost.asio `async_*()` operation to be initiated soon.
 *        It may be `move`d and saved.
 * @return A completion handler that will act as `handler()` but also satisfying the constraints of
 *         `Op op`.
 */
template<typename Handler>
auto asio_handler_via_op(Concurrent_task_loop* loop, const Op& op, Handler&& handler);

/**
 * Template specialization model for operation that obtains the underlying execution context, such as a
 * util::Task_engine or util::Strand, stored in an async::Op generated by the given Concurrent_task_loop.
 * Each subclass (impl) of Concurrent_task_loop shall provide a specialization of this template with
 * `Exec_ctx_ptr` template param being the appropriate boost.asio-compatible execution context type for that
 * loop type's `Op create_op()`.
 *
 * The mechanics of using this are explained in Concurrent_task_loop doc header.  Beyond that please see the particular
 * specialization's doc header.
 *
 * @relatesalso Concurrent_task_loop
 * @tparam Exec_ctx_ptr
 *         A pointer type (raw or smart) pointing to an execution context type satisfying
 *         boost.asio's "execution context" concept.  As of this writing the known values would be
 *         pointers to util::Task_engine and util::Strand, but really it depends on the particular
 *         subclass of Concurrent_task_loop for the `*loop` arg.  See its doc header near
 *         the particular Concurrent_task_loop subclass.
 * @param loop
 *        Loop object that, one way or another, generated and returned `op`.
 * @param op
 *        async::Op from `*loop` from which to extract the execution context object on which you'd like to perform
 *        custom boost.asio work.
 * @return Pointer to a mutable execution context object.
 */
template<typename Exec_ctx_ptr>
Exec_ctx_ptr op_to_exec_ctx(Concurrent_task_loop* loop, const Op& op);

/**
 * Template specialization for operation that obtains the underlying execution context, in this case
 * a util::Task_engine, stored in an async::Op generated by the given Segregated_thread_task_loop.
 * While `*loop` is running, the Task_engine is running in exactly 1 thread.
 *
 * Note Concurrent_task_loop::task_engine() is spiritually related to this function; but while that one gives one
 * a random thread's util::Task_engine, this one returns the specific thread's assigned to a multi-step async op `op`.
 *
 * @see Concurrent_task_loop doc header for discussion.
 * @relatesalso Segregated_thread_task_loop
 *
 * @param loop
 *        Loop object that, one way or another, generated and returned `op`.
 *        Behavior is undefined if the concrete pointed-to type is not Segregated_thread_task_loop.
 *        (assertion may trip).
 * @param op
 *        async::Op from `*loop` from which to extract the execution context on which you'd like to perform
 *        custom boost.asio work.  Behavior is undefined if it is not from `*loop` (assertion may trip).
 * @return Pointer to a mutable util::Task_engine `E` used by `*loop`.
 */
template<>
Task_engine_ptr op_to_exec_ctx<Task_engine_ptr>(Concurrent_task_loop* loop, const Op& op);

/**
 * Template specialization for operation that obtains the underlying execution context, in this case
 * a util::Strand, stored in an async::Op generated by the given Cross_thread_task_loop.
 *
 * boost.asio tip: The returned util::Strand may be useful not only as an argument to `bind_executor()`
 * (formerly `Strand::wrap()`, now deprecated) but can also be passed in lieu of a util::Task_engine into
 * boost.asio-enabled I/O object constructors (util::Timer, `boost::asio::ip::tcp::socket`, etc.).  The latter use
 * uses the `Strand` as an "execution context."
 *
 * Note Concurrent_task_loop::task_engine() is spiritually related to this function; but while that one gives one
 * a util::Task_engine, which corresponds to the entire thread pool, this one returns an execution context specifically
 * assigned to a multi-step async op `op`.
 *
 * @see Concurrent_task_loop doc header for discussion.
 * @relatesalso Cross_thread_task_loop
 *
 * @param loop
 *        Loop object that, one way or another, generated and returned `op`.
 *        Behavior is undefined if the concrete pointed-to type is not Cross_thread_task_loop.
 *        (assertion may trip).
 * @param op
 *        async::Op from `*loop` from which to extract the execution context on which you'd like to perform
 *        custom boost.asio work.  Behavior is undefined if it is not from `*loop` (assertion may trip).
 * @return Pointer to a mutable util::Strand created from util::Task_engine `E` such that
 *         `loop->task_engine() == &E`.
 */
template<>
Strand_ptr op_to_exec_ctx<Strand_ptr>(Concurrent_task_loop* loop, const Op& op);

} // namespace flow::async
