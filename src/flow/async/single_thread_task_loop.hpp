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
#include "flow/async/x_thread_task_loop.hpp"
#include "flow/async/timed_concurrent_task_loop.hpp"
#include "flow/perf/clock_type_fwd.hpp"
#include "flow/util/util.hpp"

namespace flow::async
{

/**
 * A `Concurrent_task_loop`-related adapter-style class that represents a single-thread task loop; essentially
 * it is pleasant syntactic sugar around a Concurrent_task_loop impl that cuts out concepts that become irrelevant
 * with only one thread involved.
 *
 * ### Thread safety ###
 * All methods are thread-safe for read-write on a shared Single_thread_task_loop, after its ctor returns, unless
 * otherwise specified (but read on).  This is highly significant, just as it is highly significant that boost.asio's
 * `post(Task_engine&)` is similarly thread-safe.  However, it is *not* safe to call either stop() or start()
 * concurrently with itself or the other of the two, on the same Single_thread_task_loop.
 *
 * ### Rationale ###
 * A single-thread task/event loop is very common; arguably more common than multi-threaded.  While
 * the Concurrent_task_loop hierarchy's value is things like async::Op (strand/etc.) support, which are multi-threaded
 * by definition, it also provides significant value via setup/teardown of a thread in a consistent way and other
 * features that aren't related to concurrency.  The author (ygoldfel) therefore kept wanting to use
 * Concurrent_task_loop hierarchy for single-thread loops; and immediately realized in particular that the 2 choices
 * of impl -- Cross_thread_task_loop and Segregated_thread_task_loop -- become almost bit-wise-identical to each other
 * internally when only 1 thread is involved.  Therefore the choice of which one to use felt silly.  Moreover
 * both of those classes have internal optimizations when `n_threads_or_zero == 1`.
 *
 * So then the author decided to make this class to eliminate having to make this silly choice, when all the needs
 * are already met regardless of the choice made.  It is similar to how `std::queue<T>` is a limited `deque<T>` or
 * `list<T>`; though arguably even more straightforward, because in our case it makes no difference which underlying
 * class is used (with `queue<T>` it does).  In any case, that's why there's no template parameter for the
 * underlying impl class in our case (could change in the future).
 *
 * @internal
 * ### Implementation notes / rationale ###
 * In the past Single_thread_task_loop `private`ly inherited from Cross_thread_task_loop.  Now it simple stores one
 * as a member (composition).   In both cases it's a HAS-A relationship, so the diffence is a question of syntactic
 * sugar and not conceptual.  The reason I (ygoldfel) originally used `private` inheritance was purely for
 * brevity: one could, e.g., write `using Task_loop_impl::stop;` instead of writing a trivial body that just
 * forwarded to that super-method.  The reason I switched to composition was Timed_single_thread_task_loop appeared,
 * which made it desirable to make methods like post() `virtual` -- meaning to *start* a `virtual` hierarchy
 * of (some) similarly-named methods, so that Single_thread_task_loop::post() would be `virtual`, and
 * Timed_single_thread_task_loop::post() would `override` it.  This is when trouble started: due to inheriting
 * from a Concurrent_task_loop with a `virtual` method also named `post()`,
 * `static_cast<Concurrent_task_loop*>(this)->post()` -- necessary in Timed_single_thread_task_loop for certain
 * internal reasons -- `virtual` resolution followed the chain down through the `private` inheritance and ended up
 * invoking Single_thread_task_loop::post() and hence Timed_single_thread_task_loop::post() again, leading to
 * infinite recursion.  In other words, making Single_thread_task_loop::post() `virtual` *continued* the `virtual`
 * chain of `post()` methods, instead of starting a new chain as planned, even though the inheritance is `private`.
 * At that point it seemed better -- if a little less pithy -- to give up on that and just use composition.
 */
class Single_thread_task_loop :
  public log::Log_context, // We log a bit.
  public util::Null_interface // We have some virtual things.
{
public:
  // Constructors/destructor.

  /**
   * Constructs object, making it available for post() and similar work, but without starting any thread and hence
   * without the ability to perform any work; call start() to spawn 1 thread and perform work.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname
   *        Brief, human-readable nickname of the new loop/thread, as of this writing for logging only.
   */
  explicit Single_thread_task_loop(log::Logger* logger_ptr, util::String_view nickname);

  /**
   * Executes stop() -- see its doc header please -- and then cleans up any resources.
   * The behavior of stop() has subtle implications, so please be sure to understand what it does.
   *
   * It is fine if stop() has already been called and returned.
   */
  ~Single_thread_task_loop() override;

  // Methods.

  /**
   * Starts the 1 thread in the thread pool; any queued `post()`ed (and similar) tasks may begin executing immediately;
   * and any future posted work may execute in this thread.  Calling start() after start() is discouraged and may
   * log a WARNING but is a harmless no-op.  See also stop().
   *
   * The optional `init_task_or_empty` arg is a convenience thing.  It's equivalent to
   * `post(init_task_or_empty, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION)` executed upon return.
   * `init_task_or_empty()` will run in the new thread; and only once it `return`s, start() will `return`.
   * Rationale: It has come up in our experience several times that one wants to execute something in the new thread
   * to initialize things, synchronously, before the main work -- various async `post()`ing and other calls -- can begin
   * in earnest.  Do note that any tasks enqueued before this start() but after the last stop() or constructor
   * may run first.
   *
   * The worker thread started will exit upon any uncaught exception by one
   * of the user-supplied tasks posted onto it subsequently.  If this occurs, the handler will `exit()` the entire
   * program with a non-zero code after logging the exception message.  (It is informally recommended that all other
   * threads in the application do the same.)
   *
   * Assuming no such uncaught exception is thrown, all threads will run until stop() or the destructor runs and
   * returns.
   *
   * ### Thread safety ###
   * All methods are thread-safe on a common `*this` unless noted otherwise.
   * To wit: it is not safe to call `X.start()` concurrently with `X.start()` or with `X.stop()`.
   *
   * ### Implementation ###
   * In this implementation start() essentially involves a single `Task_engine::restart()`,
   * followed by starting 1 thread, that thread executing a long-running `Task_engine::run()`.
   *
   * ### Rationale for omitting counterpart to Concurrent_task_loop::start() `thread_init_func_or_empty` arg ###
   * `init_task_or_empty` is sufficient, as there is only one thread.  The `thread_init_func_or_empty` guarantee
   * that it run *first-thing* in the thread is too subtle to worry about in practice; `init_task_or_empty` is fine.
   *
   * @param init_task_or_empty
   *        Ignored if `.empty()` (the default).  Otherwise `init_task_or_empty()` shall execute in the
   *        thread started by this method, delaying the method's return to the caller until `init_task_or_empty()`
   *        returns in said spawned thread.
   */
  void start(Task&& init_task_or_empty = Task{});

  /**
   * Waits for the ongoing task/completion handler -- if one is running -- to return; then prevents any further-queued
   * such tasks from running; then gracefully stops/joins the worker thread; and then returns.
   * The post-condition is that the worker thread has fully and gracefully exited.
   *
   * Upon return from this method, any further `post()` or more complex async ops can safely be invoked -- but they
   * will not do any actual work, and no tasks or completion handlers will run until start().  In particular
   * task_engine() will still return a util::Task_engine, and one can still invoke `post()` and async I/O ops on it:
   * doing so won't crash, but it won't do the requested work until start(). (Recall that there is no more
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
   * You may call stop() from within a task/completion handler executing within `*this` thread.  Of course
   * you may also do this from another thread.
   *
   * ### Rationale ###
   * See Concurrent_thread_task_loop::stop() doc header's Rationale section.  It also lists some useful tip(s) as of
   * this writing.
   */
  void stop();

  /**
   * Cause the given `Task` (function) to execute within the worker thread as soon as the thread is free of other queued
   * tasks/completion handlers, in first-come-first-served fashion.  `task` may not execute concurrently with some other
   * `Task`.
   *
   * `synchronicity` controls the precise behavior of the "post" operation.  Read #Synchronicity `enum` docs carefully.
   * That said: if left defaulted, `post()` works in the `post(Task_engine&)` manner: return immediately; then
   * execute either concurrently in another thread (if called *not* from within another in-thread task) or later in the
   * same thread (otherwise).
   *
   * This is safe to call after stop(), but `task()` will not run until start() (see stop() doc header).
   * Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION and Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START modes
   * will, therefore, block infinitely in that case; so don't do that after stop().
   *
   * Reminder: This is thread-safe as explained in class doc header.
   *
   * ### Rationale notes ###
   * The callback arg would normally be the last arg, by Flow coding style.  In this case it isn't, because
   * it is more valuable to make `synchronicity` optional (which it can only be if it's the last arg).
   *
   * Also, `using` would be nice for brevity, but we don't want to expose the other (irrelevant) `post()` overload
   * of the `private` superclass.
   *
   * @param task
   *        Task to execute.  `task` object itself may be `move`d and saved.
   * @param synchronicity
   *        Controls when `task()` will execute particularly in relation to when this `post()` call returns.
   */
  virtual void post(Task&& task, Synchronicity synchronicity = Synchronicity::S_ASYNC);

  /**
   * Equivalent to post() but execution is scheduled for later, after the given time period passes.
   *
   * The semantics are, in all ways in which this differs from post(), those of
   * util::schedule_task_from_now().  This includes the meaning of the returned value and the nature of
   * util::Scheduled_task.  Also, in particular, one can perform actions like canceling, short-firing, and
   * info-access by passing the returned handle into util::scheduled_task_cancel() and others.
   *
   * @warning You *must* not call any `util::scheduled_task_*()` function on the returned
   *          handle except from within `*this` loop's tasks.
   *
   * @todo Deal with the scheduled-tasks-affected-from-outside-loop corner case of the
   *       `Single_thread_task_loop::schedule_*()` APIs.  Perhaps add `bool in_loop_use_only` arg
   *       which, if `false`, will always disable the `single_threaded` optimization internally.
   *       At this time it always enables it which will cause thread un-safety if
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
  virtual util::Scheduled_task_handle schedule_from_now(const Fine_duration& from_now, Scheduled_task&& task);

  /**
   * Equivalent to schedule_from_now() except one specifies an absolute time point instead of wait duration.
   *
   * @warning See schedule_from_now() warning.
   *
   * @param at
   *        See util::schedule_task_at().
   * @param task
   *        See schedule_from_now().
   * @return See schedule_from_now().
   */
  virtual util::Scheduled_task_handle schedule_at(const Fine_time_pt& at, Scheduled_task&& task);

  /**
   * Returns a pointer to *the* internal util::Task_engine (a/k/a boost.asio `io_context`) for the purpose of
   * performing a boost.asio `async_*()` action on some boost.asio I/O object in the immediate near future.
   *
   * @return A mutable util::Task_engine to use soon.  It is *allowed* to use it as long as
   *         `*this` exists (pre-destructor) and even beyond that, though any use beyond that point would pass the
   *         reponsibility on providing thread(s) to `run()` in becomes the user's.
   */
  Task_engine_ptr task_engine();

  /**
   * Returns `true` if and only if the thread executing this call is the thread started by start().
   * Returns `false` otherwise, including if start() has not yet been called.
   *
   * This may be useful if a task may be executed from an outside thread or posted through `*this` loop.
   * E.g., if `true` it can synchronously execute some other task; if `false` `post()` it
   * with Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START or Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION
   * to ensure it not stoppable (by stop() or dtor) or even fully finishes synchronously, respectively.
   * Using either of those `Synchronicity` values while executing while in_thread() returns `true` assures
   * a thread hang instead.
   *
   * ### Thread safety ###
   * For a given `*this` safe to execute concurrently with any method except start().  It is also safe to execute
   * in the optional init-task given to start().
   *
   * @return See above.
   */
  bool in_thread() const;

protected:
  // Methods.

  /**
   * Returns the underlying work-horse Concurrent_task_loop.
   * @return See above.
   */
  Concurrent_task_loop* underlying_loop();

private:
  // Types.

  /// The class an instance of which we hold (in HAS-A fashion, not IS-A fashion).
  using Task_loop_impl = Cross_thread_task_loop;

  // Data.

  /// See underlying_loop().
  Task_loop_impl m_underlying_loop;

  /// Before start() it is default-cted (not-a-thread); from start() on it's the started thread's ID.
  util::Thread_id m_started_thread_id_or_none;
}; // class Single_thread_task_loop

/**
 * Identical to Single_thread_task_loop, but all tasks posted through it are automatically timed, with the result
 * accessible via accumulated_time().
 *
 * Using a Timed_single_thread_task_loop is identical to using Single_thread_task_loop; except -- as with
 * a Timed_concurrent_task_loop vs. plain Concurrent_task_loop -- any "naked" handler `F` passed directly to
 * a boost.asio `async_...(..., F)` operation should be wrapped by asio_handler_timed(), so that it too is timed.
 * Hence it would become: `async_...(..., L->asio_handler_timed(F))`.
 *
 * To access timing results use added API accumulated_time().
 */
class Timed_single_thread_task_loop : public Single_thread_task_loop
{
public:
  // Constructors/destructor.

  /**
   * Constructs object, similarly to Single_thread_task_loop ctor; but with timing capabilities
   * a-la Timed_concurrent_task_loop ctor.
   *
   * @param logger_ptr
   *        See Single_thread_task_loop ctor.
   * @param nickname
   *        See Single_thread_task_loop ctor.
   * @param clock_type
   *        See Timed_concurrent_task_loop_impl ctor.
   */
  explicit Timed_single_thread_task_loop(log::Logger* logger_ptr, util::String_view nickname,
                                         perf::Clock_type clock_type = perf::Clock_type::S_CPU_THREAD_TOTAL_HI_RES);

  // Methods.

  /**
   * Implements superclass method but with the addition of timing of `task`, accessible through
   * accumulated_time().
   *
   * @see Single_thread_task_loop::post() doc header for the essential documentation outside of timing.
   *
   * @param task
   *        See superclass method.
   * @param synchronicity
   *        See superclass method.
   */
  void post(Task&& task, Synchronicity synchronicity = Synchronicity::S_ASYNC) override;

  /**
   * Implements superclass method but with the addition of timing of `task`, accessible through
   * accumulated_time().
   *
   * @see Single_thread_task_loop::schedule_from_now() doc header for the essential documentation outside of timing.
   *
   * @param from_now
   *        See superclass method.
   * @param task
   *        See superclass method.
   * @return See superclass method.
   */
  util::Scheduled_task_handle schedule_from_now(const Fine_duration& from_now, Scheduled_task&& task) override;

  /**
   * Implements superclass method but with the addition of timing of `task`, accessible through
   * accumulated_time().
   *
   * @see Single_thread_task_loop::schedule_at() doc header for the essential documentation outside of timing.
   *
   * @param at
   *        See superclass method.
   * @param task
   *        See superclass method.
   * @return See superclass method.
   */
  util::Scheduled_task_handle schedule_at(const Fine_time_pt& at, Scheduled_task&& task) override;

  /**
   * See Timed_concurrent_task_loop_impl::accumulated_time().
   * @return See above.
   */
  perf::Duration accumulated_time();

  /**
   * See Timed_concurrent_task_loop_impl::asio_handler_timed().
   *
   * @tparam Handler
   *         See Timed_concurrent_task_loop_impl::asio_handler_timed().
   * @param handler_moved
   *        See Timed_concurrent_task_loop_impl::asio_handler_timed().
   * @return See Timed_concurrent_task_loop_impl::asio_handler_timed().
   */
  template<typename Handler>
  auto asio_handler_timed(Handler&& handler_moved);

private:
  // Data.

  /**
   * The task-timing decorator of our superclass Single_thread_task_loop's `protected` superclass.
   * As required, its lifetime (being a base class) is >= that of this decorating #m_timed_loop.
   */
  Timed_concurrent_task_loop_impl<perf::duration_rep_t> m_timed_loop;
}; // class Timed_single_thread_task_loop

// Template implementations.

template<typename Handler>
auto Timed_single_thread_task_loop::asio_handler_timed(Handler&& handler_moved)
{
  return m_timed_loop.asio_handler_timed(std::move(handler_moved));
}

} // namespace flow::async
