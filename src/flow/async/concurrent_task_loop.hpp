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

#include "flow/async/async_fwd.hpp"
#include "flow/async/op.hpp"
#include "flow/util/sched_task_fwd.hpp"
#include "flow/util/util.hpp"

namespace flow::async
{

// Types.

/**
 * The core flow::async interface, providing an optionally multi-threaded thread pool onto which runnable `Task`s,
 * optionally arranged into concurrency-avoiding `Op`s, can be boost.asio-posted for subsequent execution.
 *
 * ### Thread safety ###
 * All methods are thread-safe for read-write on a shared Concurrent_task_loop, after its ctor returns, unless
 * otherwise specified (but read on).  This is highly significant, just as it is highly significant that boost.asio's
 * `Task_engine::post()` is similarly thread-safe.  However, it is *not* safe to call either stop() or start()
 * concurrently with itself or the other of the two, on the same Concurrent_task_loop.
 *
 * ### First, select subclass to instantiate ###
 * Whenever the user needs a pool of task-executing threads, meaning threads awaiting user-supplied work, to be
 * `post()`ed (etc.) in the boost.asio sense, they'll create a concrete subclass of this interface
 * (the choice perhaps based on configuration, e.g.).  The choice of subclass determines how tasks will be scheduled
 * internally across threads, but the user need not worry about that after construction.
 *
 * If your task loop is fundamentally single-threaded -- which is *extremely* common and *typically* does *not*
 * generalize to a multi-threaded one easily -- then instead use the adapter Single_thread_task_loop which is *not*
 * a part of Concurrent_task_loop hierarchy but *does* use the relevant parts of it internally.
 *
 * If you choose Single_thread_task_loop then it is not necessary to read further.
 *
 * ### Next, call start() ###
 * This starts the actual threads in the thread pool.  Hence subsequent `post(F)` (etc.) will cause `F()` to
 * be able to run in one of the threads.  Some advanced points:
 *   - You may post() (etc.) before start().  This will do no work but queue it up until start().
 *   - You may call stop() to synchronously, gracefully exit the threads.  Any `post()`ed (etc.) that hadn't
 *     yet run will remain queued.  Any post() (etc.) until the next start() will (again) do no work and (again)
 *     remain queued until start().
 *   - If you save the (ref-counted) util::Task_engine from task_engine() or from an async::Op via
 *     op_to_exec_ctx(), you may use it even after `*this` is destroyed.  It is then your responsibility to
 *     start thread(s) in which to actually execute its tasks.  Any queued tasks on the `Task_engine` will remain
 *     queued until then.
 *   - (This start/stop/run/post paradigm may be familiar to boost.asio (particularly `boost::asio::io_context`) users.)
 *
 * ### Next, post() tasks on it: create_op() to group task into operations ###
 * One can post() a task (in the same way one would simply `Task_engine::post()`).  If one wants to execute an async
 * op with 2+ non-concurrent tasks, they would pass the same async::Op to post() for each of the aforementioned 2+
 * `Task`s (which are simply `void` no-arg functions basically).  An async::Op can be created
 * via create_op(); or if the task must be pinned to a specific pre-made per-software-thread async::Op,
 * these are accessible via per_thread_ops().
 *
 * New applications should strive to use only create_op() and not touch the advanced-yet-legacy-ish
 * per_thread_ops() facility.  In a classic async-task-based-event-loop algorithm, it should be sufficient to
 * execute `Task`s -- sprinkling in `Op` tags when certain tasks together comprise multi-async-step ops --
 * via one of the post() overloads.  Hence simply make an `Op` via create_op() to associate `Task`s with each other,
 * and that should be enough for most async algorithms.
 *
 * Note also the optional `Synchronicity synchronicity` argument to the `post()` methods.  By default this acts
 * like regular `Task_engine::post()`, but you can also access `Task_engine::dispatch()` type of behavior;
 * you can wait for the task to complete using yet another mode.  The latter feature,
 * Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION, may be particularly helpful at initialization time, such
 * as if one needs to perform some startup tasks in the new thread(s) before continuing to general work on
 * the loop.  E.g., subclasses might, for convenience, wrap this ability in their constructors, so that the user can
 * optionally provide an initializing task to run before the ctor returns.
 *
 * Lastly Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START may be quite helpful in ensuring a certain
 * task definitely does run -- without waiting for it to actually run, but merely *begin* to run.  More specifically
 * `L.post(F, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START); L.stop();` will ensure `F()` will run at some point --
 * but not wait for its completion.  `L.post(F); L.stop();` will not ensure this at all; whether it does or not
 * is a matter of luck.  `L.post(F, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION); L.stop();` *will* ensure
 * it but also force a wait until `F()` finishes which may not be necessary and hence increases latency/responsiveness
 * in the calling thread.
 *
 * ### And/or: Use per_thread_ops() to associate `Task`s with specific threads ###
 * There are cases when the simple, conceptual approach just described (using create_op() only, if an `Op` is even
 * desired at all) is not sufficient.  Another approach is to pre-create N `Op`s, where N is the number of threads
 * in the thread pool, and instead of create_op() one can randomly choose one of those N `Op`s and `post()` onto that
 * one (an `Op` is an `Op` regardless of how it was obtained).  Informally, there are 2 known categories of use cases
 * for this, with overlap:
 *   - The legacy use case: Major daemons exist that were architected decades before flow::async was even conceived,
 *     so they understandably achieve some of the same aims (to wit, non-concurrency of tasks in an op) by working with
 *     the lower-level/less abstract notion of a thread in a pool.  Basically, if an `Op` corresponds to a worker
 *     thread specifically, then internally 2 `Task`s being assigned to that 1 `Op` would just mean executing them
 *     on that `Op`'s corresponding worker thread; threads are serial by definition, so the `Op` semantics are trivially
 *     satisfied.  So to support such legacy designs, the per-thread-pre-created in per_thread_ops() allow user to
 *     obtain either a randomly chosen or a specifically indexed 1 of N per-thread async::Op.
 *     - Example: Suppose a multi-threaded memory-caching server has a central data store and explicitly
 *       thread-local copies (periodically synchronized with the central one) thereof.  With such a setup, it's
 *       a no-brainer to throw all work on a request that was originally assigned to thread 3 (random) of N *also* to
 *       per-thread Op #3 of N, for all `Task`s comprising that request's handling:  Since all tasks operating in
 *       `Op` (therefore thread) #3 by definition execute non-concurrently, no locks are necessary when working
 *       with thread #3's thread-local object store copy.  Simply, only thread `i` of N will ever touch object store
 *       `i` of N, to the extent it can be explicitly declared thread-local (`thread_local` in C++11, or similar).
 *   - The thread-to-thread communication use case: This has come up in practice: If daemon 1 has N producer threads,
 *     and daemon 2 has N consumer threads, then one can set up N IPC queues, where thread `i` in either process
 *     accessing (writing and reading, respectively) only queue `i`.  Then -- assuming the queue itself is
 *     safe against 1 read occurring concurrently with 1 write -- no further locking is required.  Basically,
 *     thread `i` in daemon 1 deals with thread `i` in daemon 2, using a dedicated lock-free thread-`i`-access-only
 *     IPC queue.
 *
 * ### Timers have similarly streamlined API: schedule_task_from_now() ###
 * post() is absolutely core, of course.  However obviously sometimes one needs to wait *asynchronously* for some kind
 * of event and THEN execute a task on that event.  In particular, executing it simply after some specific time period
 * passes is common and has a dedicated API.  This is called *scheduling a task* in our parlance.
 *
 * If you want to schedule a task, first decide whether you need certain advanced capabilities.  This is explained in
 * the doc header for util::schedule_task_from_now().  If you decide you need advanced capabilities, then skip to the
 * next subsection below, about general boost.asio I/O objects.  Most of the time you won't, in which case read on:
 *
 * schedule_from_now() and schedule_at() in this Concurrent_task_loop interface provide all the capabilities of
 * `util::schedule[d]_task*()` API.  (Reminder: This includes canceling and short-firing the task with ease, more
 * ease than if using the full on I/O util::Timer, which is -- again -- explained below.)  Just as with post(), there
 * is 1 version of each method for single tasks; and and 1 for operating within an async::Op, meaning the timer
 * completion handler.
 *
 * ### General boost.asio objects can be fully used with Concurrent_task_loop ###
 * Finally, there's general boost.asio "I/O object" work.  An I/O object is usually a class -- within boost.asio itself
 * or a custom object -- with 1 or more *asynchronous action* methods, always named in the style `async_*()`.  To show
 * how one would do this with Concurrent_task_loop, let's do it in somewhat formal fashion:
 *
 * Suppose you have boost.asio object X, for example `boost::asio::ip::tcp::socket` (in boost.asio itself) and
 * flow::net_flow::asio::Peer_socket (a custom one), and we want to perform an `async_A()` action, which waits
 * asynchronously for some event (e.g., a successful `tcp::socket::async_receive()` or
 * `net_flow::asio::Peer_socket::async_send()`, and then executes a *completion handler* task F:
 *
 *   ~~~
 *   flow::util::Task_engine E; // boost.asio user works directly with a Task_engine E running in 1+ threads.
 *   ...
 *   X_type X(&E);
 *   // A_target represents 0 or more mutable data structures (e.g., received-data target buffer) that sync_A() would
 *   //   modify in the background.
 *   // A_settings represents 0 or more immutable/by-value args controlling behavior of the background sync_A() action.
 *   X.async_A(&A_target, A_settings, F);
 *   // Equivalent of X.sync_A(&A_target, A_settings) is now executing in background via Task_engine E!
 *   // Once done, it will act as if one called, from an unspecified thread: F(err_code, A_more_result_info).
 *   //   - Error_code err_code is the success/failure result code.  In particular, `!err_code == true` on success.
 *   //   - A_more_result_info represents 0 or more other bits of immutable/by-copy info indicating the results of
 *   //     the background action.  For example, both `tcp::socket::async_receive()` and
 *   //     `net_flow::Peer_socket::async_send()` will call `F(err_code, n)`, where `size_t n` is the # of bytes
 *   //     received or sent respectively.
 *   ~~~
 *
 * That's the setup and should be familiar to boost.asio I/O object users.  (Note that util::Timer is a (relatively
 * simple) I/O object itself; it lacks `A_settings` (one makes a call like `expires_at()` separately before the
 * actual async action) and `A_more_result_info` (as `err_code` is sufficient) in particular.  It also lacks
 * any `A_target`.  It's clearly the degenerate example of an I/O object action.)  So how to write the
 * above when working with a Concurrent_task_loop instead of `Task_engine`?
 *
 *   ~~~
 *   flow::async::Concurrent_task_loop L; // Work with generalized thread pool L, not a Task_engine E.
 *   ...
 *   // ATTN!  A Task_engine is needed by boost.asio?  Use our API -- task_engine() -- to get one.
 *   X_type X(L.task_engine());
 *   X.async_A(&A_target, A_settings, F);
 *   ~~~
 *
 * Almost everything is the same!  Just need to call that API to obtain a `Task_engine` to use.  As a result:
 *   - The background action, the equivalent of `X.sync_A()`, will be performed by some unspecified code in some
 *     unknown thread.  We don't care how with boost.asio directly, and we don't care how with Concurrent_task_loop
 *     either.
 *   - Then, for some `async_A()`s it will modify `A_target`.  This, too, will be done by an unspecified thread
 *     *with no locking guarantees*.  Hence, one must not access `A_target` from application threads.  As wit
 *     boost.asio direct use, this is typical.  For example no app code would access the target data buffer of a
 *     receive operation.
 *   - Finally, `F()` will be posted on completion via `Task_engine L.task_engine()`.  Of course we guarantee it
 *     will be in some thread in the thread pool `L`.
 *
 * Finally, then, suppose the original snippet above is modified to use a `Strand`, to guarantee non-concurrency with
 * some other boost.asio handler(s).  This would look like:
 *
 *   ~~~
 *   flow::util::Task_engine E;
 *   flow::util::Strand S(&E); // Create a Strand that guarantees non-concurrency of handlers posted onto Task_engine E.
 *   ...
 *   X_type X(&E);
 *   X.async_A(&A_target, A_settings, boost::asio::bind_executor(S, F));
 *   ...
 *   Y_type Y(&E);
 *   Y.async_B(&B_target, B_settings, boost::asio::bind_executor(S, G));
 *   // X.sync_A() and Y.sync_B() are executing in background; F() and G() will run on respective completion;
 *   // but F() and G() shall run non-concurrently by virtue of being wrapped by the same Strand: S.
 *   ~~~
 *
 * To accomplish this with a `Concurrent_task_loop L`:
 *
 *   ~~~
 *   flow::async::Concurrent_task_loop L;
 *   auto op J = L.create_op(); // ATTN! The syntax is different from Strands but the idea is identical.
 *   ...
 *   X_type X(L.task_engine());
 *   // ATTN! The syntax is again somewhat different from bind_executor(S, F), but the idea is equivalent.
 *   X.async_A(&A_target, A_settings, flow::async::asio_handler_via_op(&L, J, F));
 *   ...
 *   Y_type Y(L.task_engine());
 *   Y.async_B(&B_target, B_settings, flow::async::asio_handler_via_op(&L, J, G));
 *   // X.sync_A() and Y.sync_B() are executing in background; F and G will run on respective completion;
 *   // but F() and G() shall run non-concurrently by virtue of being wrapped by the same Op: J.
 *   ~~~
 *
 * However, now that you're working with an I/O object directly, you must be careful.  Memorizing a `Task_engine`
 * at construction has different effects depending on which concrete subclass of Concurrent_task_loop `L` is.
 * Cross_thread_task_loop in particular will assign it to whichever thread is best.  Segregated_thread_task_loop
 * will keep using the same random-ish thread chosen when `L.task_engine()` is called.  If you need particular
 * behavior, you will need to strongly consider what to do: It is no longer totally generic behavior independent
 * of the subclass, as it generally is when it comes to the post() and `schedule_*()` APIs.
 *
 * Lastly, if you are going down that road (which may be fully necessary) then consider
 * the free function template op_to_exec_ctx() which is specialized
 * for each concrete Concurrent_task_loop; it takes a loop and an async::Op as input; and returns a boost.asio
 * "execution context" which can be passed -- much like a `Task_engine` in the above example -- to I/O object
 * constructors.  See the specializations -- as of this writing near Cross_thread_task_loop (returns util::Strand) and
 * Segregated_thread_task_loop (returns util::Task_engine) at least.  Choose between the above technique
 * and op_to_exec_ctx() when working directly with a boost.asio-compatible I/O object.
 *
 * I am self-conscious at the length and seeming complexity of this formal writeup but must emphasize: This is
 * using the same patterns as boost.asio users use.  It's just a matter of mapping them to `flow::async` Flow module's
 * generalized Concurrent_task_loop and async::Op APIs.  Reminder: The benefit of this is that one uses
 * boost.asio-equivalent semantics; yet the Concurrent_task_loop concrete subclass can implement it internally in
 * various ways that are or *aren't* what a direct use of `Task_engine` would do.  However, when using
 * I/O objects -- as opposed to post() -- the genericness will be less generic.  That is sometimes necessary.
 *
 * TL;DR: Any boost.asio-style (whether from boost.asio itself or custom) I/O object is to be used as normal, but:
 * To get a `Task_engine`, use task_engine().  To get a `Strand`-like thing util::Op, use create_op().
 * To *use* the `Strand`-like thing util::Op, use asio_handler_via_op().  Alternatively use actual `Task_engine`s or
 * `Strand`s directly if necessary; see op_to_exec_ctx() specializations for that purpose.
 * Lastly, make sure you understand the exact boost.asio behavior when using task_engine() (yields util::Task_engine),
 * asio_handler_via_op() (yields a util::Strand-bound callback), and/or op_to_exec_ctx() (yields a
 * util::Task_engine, util::Strand, or something else depending on subclass, to be passed as an "execution context"
 * to I/O object ctor).
 *
 * ### Are non-async boost.asio actions supported? ###
 * boost.asio `async_*()` actions are supported by `flow::async` module.  What about synchronous and non-blocking
 * operations?  Well, sure, they're supported.  This module is just not *about* them, hence the name.
 * Just for perspective though:
 *   - A non-blocking op (achieved in boost.asio normally by calling `something.non_blocking(true);` and then
 *     `some_op(something, ...)` or `something.some_op()`) can certainly be used whenever you want, in a task or
 *     outside of it, assuming of course you're not breaking thread-safety rules on concurrent access to `something`.
 *     The only "connection" to Concurrent_task_loop is that the `something` may be associated with
 *     `*(this->task_engine())`.  That's fine.
 *   - A blocking op (achieved by calling `something.non_blocking(false);` and then same as in previous bullet)
 *     can also be used whenever.  In some ways it's even less connected to Concurrent_task_loop, as blocking ops
 *     are only tangentially related to `Task_engine` in the first place; they don't participate in the
 *     internal event loop and simply usually call some blocking OS API or similar.  However, a blocking call
 *     does *block* the thread; so if you do this inside a `*this`-posted task, then that task will block.
 *     - Informally: It it best not to mix blocking and non-blocking tasks in the same Concurrent_task_loop.
 *       The blocking ones will harm the scheduler's ability to efficiently schedule the quick (non-blocking) tasks.
 *     - Informally: However, it is entirely reasonable and practical to limit a given Concurrent_task_loop
 *       (even, or especially, a multi-thread one) to blocking tasks exclusively.
 *       - One practical example is a multi-threaded
 *         DNS host resolver that maintains many threads performing blocking `getaddrinfo()` calls, since an async
 *         DNS API is not available; and a separate async-only loop/thread kicking off result-emitting handlers to the
 *         user, with the former blocking-only loop posting result-emitting tasks onto the async-only loop/thread.
 *         (boost.asio's out-of-the-box `resolver` provides an async API but is internally single-threaded and therefore
 *         unsuitable at scale.)
 *
 * @internal
 *
 * ### Design discussion re. Concurrent_task_loop hierarchy ###
 *
 * @see Task_qing_thread, particularly the constructor Task_qing_thread::Task_qing_thread() doc header, explaining
 *      how the Concurrent_task_loop hierarchy is an encapsulation of `vector<Task_qing_thread>`, arranged in various
 *      potential ways of working with each other.  It also includes an intro to the question of how to choose
 *      Cross_thread_task_loop vs. Segregated_thread_task_loop, at a lower level.
 *
 * While it is a clean interface, realistically speaking the entire existing hierarchy is perhaps best explained
 * by immediately discussing the 2 concrete classes the API as of this writing.  (More classes may well be added, but
 * as of this writing and probably for posterity it makes sense to discuss these 2 specific ones.)  The available pool
 * types are:
 *
 * - Cross_thread_task_loop: Internally, a single shared Task_engine and N `Task_qing_thread`s working together
 *   off that `post()`-capable engine.  N can be specified directly, or it can be auto-determined based on available
 *   hardware.  The latter will also enable automatically pinning the threads in such a way as to attempt to minimize
 *   latency, namely avoiding hyper-threading or other physical core sharing by several hardware threads; this is done
 *   by making N = # of physical cores; and pinning each software thread to a distinct group of logical cores (hardware
 *   threads), so that each software thread gets its own physical core, avoiding latency-increasing "competition."
 *   An attempt is also made (achievable in Linux) to pin them in a consistent way, so that if another pool elsewhere
 *   uses the same code and config, they will arrange their same N threads in the same order.  This can help if thread i
 *   from pool 1 is producer writing to some area in memory, while thread i from pool 2 is consumer of same, reading
 *   there.  The `Cross_thread` part means that each multi-task sequence of callbacks constituting an async::Op, s/t
 *   those callbacks must not execute concurrently, may use more than 1 thread (internally, via the boost.asio
 *   util::Strand mechanism) which theoretically can improve use of thread time with asymmetrical load.  It might
 *   also negate per-thread cache locality, etc., and counter-act the effectiveness of aforementioned pinning.
 *   - `Cross_thread` means cross-thread.
 *
 * - Segregated_thread_task_loop: Internally, N `Task_qing_thread`s working together, each with its *own*
 *   `post()`-capable Task_engine queue, meaning by contrast with
 *   Cross_thread_task_loop a given `Op` always executes its tasks on the same thread.  Otherwise it works the same.
 *   Under asymmetrical load it might not use all available cross-thread time; however, it arguably also works
 *   straightforwardly "synergistically" with any attempts at per-processor-core pinning.
 *
 * The use is, of course, identical via the common API Concurrent_task_loop.
 *
 * Apologies for the conversational comment.  The internal subtleties are encapsulated and hidden from user.  Yet there
 * is considerable flexibility available.  One can think of this as a convenient wrapper around various functionality
 * typically used manually and separately from each other -- simplifying the core interface to just async::Op and
 * post() and providing automatic flexibility as to what functionality is in fact used and when as a result.
 * The functionality accessible: `Task_engine::post()`; scheduling via util::Strand; scheduling on specific thread;
 * non-concurrency guarantees of 2+ tasks in one async op; and thread-count selection and pinning based on available
 * processor architecture (hardware threads, physical cores).
 */
class Concurrent_task_loop :
  public util::Null_interface
{
public:
  // Types.

  /// Short-hand for the thread-initializer-function optional arg type to start().
  using Thread_init_func = Function<void (size_t thread_idx)>;

  // Constructors/destructor.

  /**
   * Any implementing subclass's destructor shall execute stop() -- see its doc header please -- and then clean up
   * any resources.  The behavior of stop() has subtle implications, so please be sure to understand what it does.
   *
   * It is fine if stop() has already been called and returned.
   */
  ~Concurrent_task_loop() override;

  // Methods.

  /**
   * Starts all threads in the thread pool; any queued `post()`ed (and similar) tasks may begin executing immediately;
   * and any future posted work may execute in these threads.  Calling start() after start() is discouraged and may
   * log a WARNING but is a harmless no-op.  See also stop().
   *
   * The optional `init_task_or_empty` arg is a convenience thing.  It's equivalent to
   * `post(init_task_or_empty, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION)` executed upon return.
   * `init_task_or_empty()` will run in the new thread pool; and only once it `return`s, start() will `return`.
   * Rationale: It has come up in our experience several times that one wants to execute something in the new thread(s)
   * to initialize things, synchronously, before the main work -- various async `post()`ing and other calls -- can begin
   * in earnest.  Do note that any tasks enqueued before this start() but after the last stop() or constructor
   * may run first.
   *
   * Suppose a user-supplied task posted onto a worker thread throws an uncaught exception.  This will be handled
   * the same as if that occurred directly in that thread; in other words we don't catch it in any way, not even
   * to re-throw it or manually `std::abort()` or anything of that nature.  We informally recommend you handle uncaught
   * exceptions in a program-wide SIGABRT handler or equally program-wise custom `std::terminate()` (via
   * `std::set_terminate()`).  We informally recommend that all other threads similarly let any uncaught exception
   * fall through and deal with the fallout at the global program-wide level (to avoid losing precious stack trace
   * information).
   *
   * Assuming no such uncaught exception is thrown, all threads will run until stop() or the destructor runs and
   * returns.
   *
   * ### Thread safety ###
   * As noted in the class doc header, all methods are thread-safe on a common `*this` unless noted otherwise.
   * To wit: it is not safe to call `X.start()` concurrently with `X.start()` or with `X.stop()`.
   *
   * ### Thread initialization ###
   * Each thread start() starts shall be, soon, blocked by running an event loop (or part of a multi-thread event loop);
   * meaning it will be either blocking waiting for posted tasks/active events or executing posted tasks/event handlers.
   * It is, however, sometimes required to perform setup (usually of a low-level variety) in the thread before the
   * event loop proper begins.  (The use case that triggered this feature was wanting to execute Linux
   * `setns(CLONE_NEWNET)` to affect the subsequent socket-create calls in that thread.)  If needed: pass in
   * a non-empty function as `thread_init_func_or_empty` arg; it will receive the thread index 0, 1, ... as the arg.
   * It will run first-thing in the new thread.  Subtlety: As of this writing "first-thing" means literally first-thing;
   * it will run before any of the implementing start()'s own code begins.  (This may be relaxed in the future to
   * merely "before the event loop is ready for tasks."  Then this comment shall be updated.)
   *
   * Note: start() shall *block* until *all* `thread_init_func_or_empty()` invocations (if arg not `.empty()`)
   * have completed.  This can be important, for example, if the actions they are taking require elevated privileges,
   * then this guarantee means one can drop privileges after that.  Informally: we intuitively recommend against
   * blocking in this callback, although perhaps some use case might require it.  Just be careful.
   *
   * ### Design rationale (thread initialization arg) ###
   * Consider the specific implementation of the present interface, Segregated_thread_task_loop.  Something similar
   * to this feature is possible without this start() optional arg: One can simply post() onto each of the
   * per_thread_ops(), with Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION.  It'll run as the first *task*
   * in each thread, as opposed to strictly-speaking first-*thing*, but it's close enough.  So why add this to the
   * interface?  Well, consider the other implementation, Cross_thread_task_loop.  By definition it's not possible
   * to target individual threads in that guy (per_thread_ops() exists but its `Op`s are *per*-thread, not *in*-thread;
   * they are `Strand`s, not threads).  So then some other, Cross_thread_task_loop-*only* API would be necessary to
   * get what we need.  Hence it made sense to add this as an interface-level feature.  Then these asymmetries go away
   * naturally.
   *
   * @todo Concurrent_task_loop::start() has an optional thread-initializer-function arg; it could be reasonable to
   * ass a thread-finalizer-function arg symmetrically.  As of this writing there is no use case, but it's certainly
   * conceivable.
   *
   * @param init_task_or_empty
   *        Ignored if `.empty()` (the default).  Otherwise `init_task_or_empty()` shall execute in one of the
   *        threads started by this method, delaying the method's return to the caller until `init_task_or_empty()`
   *        returns in said spawned thread.
   * @param thread_init_func_or_empty
   *        If not `.empty() == true`, `thread_init_func_or_empty(thread_idx)` shall be executed first-thing
   *        in each thread, for all `thread_idx` in [0, n_threads()).  start() will return no sooner than
   *        when each such callback has finished.
   */
  virtual void start(Task&& init_task_or_empty = Task(),
                     const Thread_init_func& thread_init_func_or_empty = Thread_init_func()) = 0;

  /**
   * Waits for any ongoing task(s)/completion handler(s) to return; then prevents any further-queued such tasks
   * from running; then gracefully stops/joins all threads in pool; and then returns.  The post-condition is that
   * the worker threads have fully and gracefully exited.
   *
   * Upon return from this method, any further `post()` or more complex async ops can safely be invoked -- but they
   * will not do any actual work, and no tasks or completion handlers will run until start().  In particular
   * task_engine() will still return a util::Task_engine, and one can still invoke `post()` and async I/O ops on it:
   * doing so won't crash, but it won't do the requested work until start(). (Recall that there are no more
   * threads in which to do this work.)  The destructor can then be invoked, at which point obviously one cannot
   * `post()` (or anything else like it) either.
   *
   * This condition is reversible via start().  In fact, `*this` starts in the stopped state, and start() is required
   * to make posted tasks actually execute.
   *
   * Lastly, calling stop() after stop() returns is a harmless no-op.  Also note the destructor shall call stop().
   *
   * ### Thread safety ###
   * As noted in the class doc header, all methods are thread-safe on a common `*this` unless noted otherwise.
   * To wit: it is not safe to call `X.stop()` concurrently with `X.stop()` or with `X.start()`.
   *
   * You may call stop() from within a task/completion handler executing within `*this` thread pool.  Of course
   * you may also do this from another thread.
   *
   * ### Rationale ###
   * This is similar to boost.asio `Task_engine::stop()`.  At a minimum it is useful, when shutting down the app
   * or module, in the situation where 2+ `Concurrent_task_loop`s routinely post work onto each other (or in at least 1
   * direction).  To safely stop all 2+ loops, one would first invoke this stop() method on each
   * Concurrent_task_loop, in any order; having done that destroy (invoke dtor on) each Concurrent_task_loop, also in
   * any order.  This way any cross-posting will safely work during the stop() phase (but do nothing on the
   * already-stopped loops); and by the time the destructor-invoking phase begins, no more cross-posting tasks can
   * possibly be executing (as their threads don't even exist by then).
   *
   * Note, however, that this is as graceful as we can generically guarantee -- in that it won't crash/lead to undefined
   * behavior on our account -- but it is up to you to ensure your algorithm is robust, in that nothing bad will happen
   * if tasks are suddenly prevented from running.  For example, if task A locks some file, while task B later unlocks
   * it, *you* are the one who must ensure you don't invoke stop() "between" task A and task B.  (E.g., invoking it
   * while A runs will let A complete; but it will very possibly prevent B from starting subsequently.)  We have no way
   * of knowing to let task B run first and only then stop the thread(s).
   *
   * Lastly, the stop() and start() mechanism is amenable to dynamically configuring thread behavior such as the
   * number of threads in the pool.
   *
   * @todo boost.asio has advanced features that might help to mark certain tasks as "must-run if already queued, even
   * if one `stop()`s"; consider providing user-friendly access to these features, perhaps in the context of the
   * existing Concurrent_task_loop::stop() API.  These features are documented as of this writing at least in the
   * `io_context` doc page.
   */
  virtual void stop() = 0;

  /**
   * How many threads does start() start?
   * @return See above.
   */
  virtual size_t n_threads() const = 0;

  /**
   * Return a new Op which can bundle together an arbitrary set of `post()`s that would result in the
   * provided task functions executing non-concurrently.  That's informal; the formal semantics of what async::Op
   * means are in async::Op doc header.  Informally: please recall that a copy (of a copy, of a copy, ...) of
   * an `Op` is an equivalent `Op`, and copying them is light-weight (at worst like copying `shared_ptr`).
   *
   * All `Op`s shall remain valid throughout the lifetime of `*this`.
   *
   * This is the more general method of obtaining an async::Op, vs. going through per_thread_ops().
   * It should be used *unless* you specifically need to access some
   * per-thread resource in the associated `Task`s.  See class doc header for more discussion on this dichotomy.
   * TL;DR: Use create_op() by default, *unless* the `Task`s you plan to execute are working on some
   * per-thread resource.
   *
   * @return See above.
   */
  virtual Op create_op() = 0;

  /**
   * Returns the optional-use, pre-created collection of per-thread async::Op objects, such that
   * the i-th `Op` therein corresponds to the i-th (of N, where N = # of threads in this pool) thread.
   *
   * All `Op`s and this `Op_list&` shall remain valid throughout the lifetime of `*this`.
   *
   * This is an advanced/legacy-ish feature.  Please see class doc header for discussion on when one should use this
   * as opposed to the simpler create_op().
   *
   * @return See above.
   */
  virtual const Op_list& per_thread_ops() = 0;

  /**
   * Cause the given `Task` (function) to execute within the thread pool as soon as possible, in the first thread
   * available, in otherwise first-come-first-served fashion.  `task` may execute concurrently with some other `Task` if
   * there are 2+ threads in `*this` pool.  Meanings of "as soon as possible" and "available" are to be determined by
   * the concrete method implementation.  That is, the interface does not promise it'll use literally the first
   * thread to be idle, but informally -- all else being equal -- that's a great goal.
   *
   * `synchronicity` controls the precise behavior of the "post" operation.  Read #Synchronicity `enum` docs carefully.
   * That said: if left defaulted, `post()` works in the `Task_engine::post()` manner: return immediately; then
   * execute either concurrently in another thread or later in the same thread.
   *
   * This is safe to call after stop(), but `task()` will not run until start() (see stop() doc header).
   * Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION and Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START modes
   * will, therefore, block infinitely in that case; so don't do that after stop().
   *
   * Reminder: This is thread-safe as explained in class doc header.
   *
   * ### Rationale note ###
   * The callback arg would normally be the last arg, by Flow coding style.  In this case it isn't, because
   * it is more valuable to make `synchronicity` optional (which it can only be if it's the last arg).
   *
   * @param task
   *        Task to execute.  `task` object itself may be `move`d and saved.
   * @param synchronicity
   *        Controls when `task()` will execute particularly in relation to when this `post()` call returns.
   */
  virtual void post(Task&& task, Synchronicity synchronicity = Synchronicity::S_ASYNC) = 0;

  /**
   * Identical to the other post() with the added constraint that no other `Task` *also* similarly posted with the
   * equivalent async::Op may execute concurrently.  See doc header for async::Op for a formal definition of
   * what this call does w/r/t async::Op.
   *
   * Reminder: This is thread-safe as explained in class doc header.
   *
   * @param op
   *        The (presumably) multi-async-step operation to which `task` belongs, such that no `Task`s associated with
   *        `op` may execute concurrently with `task`.  If `op.empty()` (a/k/a `op == Op()`, recalling that `Op()`
   *        is null/sentinel), then `assert()` trips.
   * @param task
   *        See other post().
   * @param synchronicity
   *        See other post().
   */
  virtual void post(const Op& op, Task&& task, Synchronicity synchronicity = Synchronicity::S_ASYNC) = 0;

  /**
   * Equivalent to 2-argument post() but execution is scheduled for later, after the given time period passes.
   *
   * The semantics are, in all ways in which this differs from 2-argument post(), those of
   * util::schedule_task_from_now().  This includes the meaning of the returned value and the nature of
   * util::Scheduled_task.  Also, in particular, one can perform actions like canceling, short-firing, and
   * info-access by passing the returned handle into util::scheduled_task_cancel() and others.
   *
   * @warning If n_threads() is 1, then you *must* not call any `util::scheduled_task_*()` function on the returned
   *          handle except from within `*this` loop's tasks.
   *
   * @todo Deal with the scheduled-tasks-affected-from-outside-loop corner case of the
   *       `Concurrent_task_loop::schedule_*()` APIs.  Perhaps add `bool in_loop_use_only` arg
   *       which, if `false`, will always disable the `single_threaded` optimization internally.
   *       At this time it always enables it if `n_threads() == 1` which will cause thread un-safety if
   *       the returned handle is touched from outside an in-loop task.  `void` versions of the `schedule_*()` APIs
   *       should be added which would lack this, as in that case there is no handle to misuse outside the loop.
   *
   * @param from_now
   *        See util::schedule_task_from_now().
   * @param task
   *        The task to execute within `*this` unless successfully canceled.
   *        `task` object itself may be `move`d and saved.
   * @return See util::schedule_task_from_now().
   */
  virtual util::Scheduled_task_handle schedule_from_now(const Fine_duration& from_now, Scheduled_task&& task) = 0;

  /**
   * Equivalent to 2-argument schedule_from_now() except one specifies an absolute time point instead of wait duration.
   *
   * @warning See schedule_from_now() warning.
   *
   * @param at
   *        See util::schedule_task_at().
   * @param task
   *        See schedule_from_now().
   * @return See schedule_from_now().
   */
  virtual util::Scheduled_task_handle schedule_at(const Fine_time_pt& at, Scheduled_task&& task) = 0;

  /**
   * Equivalent to 3-argument post() but execution is scheduled for later, after the given time period passes.
   *
   * The semantics are, in all ways in which this differs from 3-argument post(), those of
   * util::schedule_task_from_now().  This includes the meaning of the returned value and the nature of
   * util::Scheduled_task.
   *
   * @warning See other schedule_from_now() warning.
   *
   * @param op
   *        See 3-argument post().
   * @param from_now
   *        See util::schedule_task_from_now().
   * @param task
   *        The task to execute within `*this`, subject to `op` constraints, unless successfully canceled.
   *        `task` object itself may be `move`d and saved.
   * @return See util::schedule_task_from_now().
   */
  virtual util::Scheduled_task_handle schedule_from_now(const Op& op,
                                                        const Fine_duration& from_now, Scheduled_task&& task) = 0;

  /**
   * Equivalent to 3-argument schedule_from_now() except one specifies an absolute time point instead of wait duration.
   *
   * @warning See schedule_from_now() warning.
   *
   * @param op
   *        See 3-argument post().
   * @param at
   *        See util::schedule_task_at().
   * @param task
   *        See schedule_from_now().
   * @return See schedule_from_now().
   */
  virtual util::Scheduled_task_handle schedule_at(const Op& op,
                                                  const Fine_time_pt& at, Scheduled_task&& task) = 0;

  /**
   * Returns a pointer to *an* internal util::Task_engine (a/k/a boost.asio `io_service`) for the purpose of
   * performing a boost.asio `async_*()` action on some boost.asio I/O object in the immediate near future.
   *
   * The mechanics of using this are explained in Concurrent_task_loop doc header.  Using this in any other
   * fashion may lead to undefined behavior, while `*this` exists.
   *
   * @return A mutable util::Task_engine to use soon.  Informally, the sooner one calls the intended `async_*()` action
   *         on it, the more effective the internal load-balancing.  Formally, it is *allowed* to use it as long as
   *         `*this` exists (pre-destructor) and even beyond that, though any use beyond that point would pass the
   *         reponsibility on providing thread(s) to `run()` in becomes the user's.
   */
  virtual Task_engine_ptr task_engine() = 0;
}; // class Concurrent_task_loop

// Free functions: in *_fwd.hpp.

} // namespace flow::async

// Macros.

#ifdef FLOW_DOXYGEN_ONLY // Compiler ignores; Doxygen sees.

/**
 * Macro set to `1` (else `0`) if and only if natively the pthread API allows one to set thread-to-core
 * affinity.  This API, if available, is an extension of POSIX and not always available.
 *
 * The macro conceptually belongs to the flow::async namespace,
 * hence the prefix.
 *
 * @see #FLOW_ASYNC_HW_THREAD_AFFINITY_MACH_VIA_POLICY_TAG
 */
#  define FLOW_ASYNC_HW_THREAD_AFFINITY_PTHREAD_VIA_CORE_IDX

/**
 * Macro set to `1` (else `0`) if and only if natively there is Mach kernel API that allows to set thread-to-core
 * affinity using the policy tag mechanism.  This is specific to Mac kernels (used in Darwin/Mac) and not all of
 * them.
 *
 * The macro conceptually belongs to the flow::async namespace,
 * hence the prefix.
 *
 * @see #FLOW_ASYNC_HW_THREAD_AFFINITY_PTHREAD_VIA_CORE_IDX
 */
#  define FLOW_ASYNC_HW_THREAD_AFFINITY_MACH_VIA_POLICY_TAG

#else // if !defined(FLOW_DOXYGEN_ONLY)

// Now the actual definitions compiler sees (Doxygen ignores).

#  ifdef FLOW_OS_MAC
#    define FLOW_ASYNC_HW_THREAD_AFFINITY_PTHREAD_VIA_CORE_IDX 0
#    define FLOW_ASYNC_HW_THREAD_AFFINITY_MACH_VIA_POLICY_TAG 1
#  elif defined(FLOW_OS_LINUX)
#    define FLOW_ASYNC_HW_THREAD_AFFINITY_PTHREAD_VIA_CORE_IDX 1
#    define FLOW_ASYNC_HW_THREAD_AFFINITY_MACH_VIA_POLICY_TAG 0
#  else
static_assert(false, "We only know how to deal with thread-core affinities in Darwin/Mac and Linux.");
#  endif

#endif // elif !defined(FLOW_DOXYGEN_ONLY)

namespace flow::async
{
// Template implementations.

template<typename Handler>
auto asio_handler_via_op(Concurrent_task_loop* loop, const Op& op, Handler&& handler)
{
  /* To understand this -- though it is rather basic boiler-plate if one is familiar with lambdas and boost.asio --
   * in context, read the relevant section of Concurrent_task_loop doc header.  */
  return [loop, op, handler = std::move(handler)](auto... params) mutable
  {
    loop->post(op,
               [handler = std::move(handler), // Avoid copying handler again.  Move it instead.
                params...]()
    {
      handler(params...);
    }, Synchronicity::S_OPPORTUNISTIC_SYNC_ELSE_ASYNC);
    /* Attn: That last argument means if we're already in the proper thread, then run `handler(args...);`
     * synchronously right now.  This is a perf win -- particularly when *loop is really a Segregated_thread_task_loop,
     * when this win will be in effect every single time -- but is it safe?  Yes: The entirety of the intended task
     * *is* `handler(args...)`, and the present { body } has already been post()ed in its entirety and is
     * executing, hence we're adding nothing unsafe by executing the handler synchronously now. */
  };

  /* Subtleties regarding `params...` and `auto`:
   * Firstly, if not familiar with this crazy particular use of `auto` (on `params`), it's something like this
   * (cppreference.com, in its big page on lambdas, explains this is called a "generic lambda"):
   * The params... pack expands to the arg types based on what Handler really is, in that instantiation of the
   * asio_handler_via_op() template; meaning if Handler is, say, Function<void (int a, int b)>, then
   * params... = int, int -- because of the invocation of `handler()` inside the *body* of the returned lambda.
   * Hence the returned object is a function object that takes the same types of arguments, magically.
   * Secondly, and this is where it gets subtle, notice what we do with params... within the returned lambda:
   * we *capture* params..., so that we can post() a no-arg handler onto *loop; and within the body of that no-arg
   * handler we will finally (when boost.asio actually executes the post()ed thing at some point) invoke the handler,
   * passing it `params...`.  The subtlety is that each actual arg in the params... pack is *copied* when thus
   * captured.  Ideally, for performance, we'd write something like `params = std::move(params)...`, but this is not
   * supported (at least in C++17); only capturing a pack by copy is supported -- using a pack in an init-capture
   * ([blah = blah]) isn't supported.  To that end a standards edit was proposed:
   *   http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2017/p0780r0.html
   * Apparently C++20 accepted this in slightly modified (syntactically) form, and indeed on coliru.com one can write
   * `...params = std::move(params)` and build it with --std=c++20.  In the meantime (C++14, C++17) one *could* get the
   * desired behavior by using a strained tuple<> construction (see the above link which suggests it as a work-around).
   * However, I (ygoldfel) did not do it; I am just copying it.  Reason: Well, it's much easier to read.
   * Perf-wise, however, the reason is: boost.asio out-of-the-box handler types all take little scalars (size_t,
   * Error_code) which are fine to copy (std::move() would just copy them anyway).  That said, boost.asio can be used
   * with custom types; and indeed for example flow::net_flow::asio::Server_socket::async_accept() works with handlers
   * that take a shared_ptr<> arg: copying that would increment a ref-count/later decrement it... whereas a move()
   * would do neither.  So it's not quite free in all cases -- close enough though in my opinion.
   *
   * @todo As of this writing we've recently moved to C++17 (months after this code was written originally);
   * which gains std::apply().  The tuple<>+apply() trick isn't *so* hard to read/write.  Maybe do so.
   * @todo With C++20 -- if that ever happens -- can just write it so nicely: [...params = std::move(params)];
   * best of all worlds. */

  // ---

  /* Lastly let's deal with the possibility that *loop is a Timed_concurrent_task_loop decorator of an actual
   * Concurrent_task_loop.  The good news is we've already ensured `handler()` itself gets timed: we L->post()ed,
   * and L->post() does the right thing by definition when L is indeed Timed_....  However the loop->post() thingie
   * we executed to make that happen -- that won't be timed, if we simply return posting_handler; after all
   * they'll just do async_<something>(..., posting_handler), and that'll be that.  One *could* try to
   * conditionally (via dynamic_cast<> RTTI check) wrap the above in asio_handler_timed() if *loop is
   * really a Timed_concurrent_task_loop; otherwise just return the above.  However then `auto` can fail to compile,
   * as the returned lambda function object type could be A in one case but B in another, for the same `Handler`;
   * and we're screwed.  So for now it's a @todo:
   *
   * @todo Enable the timing of the post()ing code above, not just handler() itself.  How?  Suppose the RTTI
   * check idea were workable (thought experiment).  Then:
   * This kind of use of RTTI to conditionally do or not do something immediately sets off alarms; the point
   * of interfaces is to do everything conditional through virtual.  But the whole point of
   * Timed_concurrent_task_loop's design is to *not* modify the core Concurrent_task_loop API but only decorate
   * its behavior.  To really solve this, I (ygoldfel) believe that a good-anyway rewrite of the
   * Concurrent_task_loop hierarchy is in order.  It really should have been a set of concept-implementing
   * classes (where Concurrent_task_loop would cease to exist as a class and be merely a duck-typing concept).
   * This would be a breaking change, but it would resolve a few issues.  1, asio_handler_via_op() could be coded
   * separately for each type of _loop, which would make it possible to not even add this intermediate post()
   * but instead simply do the right thing for each actual type of _loop: Cross_thread_task_loop would
   * fish out the Strand from the Op and bind that executor to `handler`; Segregated_thread_task_loop would
   * be a no-op (as the user must select the Op-appropriate Task_engine in the first place).  The perf
   * implications of such an approach would be pleasant.  And 2,
   * Timed_concurrent_task_loop's overload of asio_handler_via_op() would simply forward to the contained
   * Concurrent_task_loop's asio_handler_via_op() -- and then wrap the result in asio_handler_timed() and return
   * that.  Nice and clean.  Obviously then the way one uses the whole hierarchy would change and be a breaking
   * change -- but even that wouldn't be such a big deal; a virtual hierarchy could be built on top of these
   * concept-satisfying classes, if needed (and I doubt it's needed all that frequently in practice). */
} // asio_handler_via_op()

} // namespace flow::async
