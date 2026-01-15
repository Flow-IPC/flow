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
#include "flow/async/concurrent_task_loop.hpp"
#include "flow/perf/timed_handler.hpp"
#include <atomic>

namespace flow::async
{

// Types.

/**
 * Decorator of a Concurrent_task_loop with same or greater lifetime that accumulates time elapsed in any tasks posted
 * or scheduled onto that loop.  While this class template is publicly available, most likely the user shall choose
 * one of 2 related classes:
 *   - Timed_concurrent_task_loop: This is essentially an alias of `Timed_concurrent_task_loop_impl<T>`, where `T` is
 *     an `atomic` suitable for accumulating times from tasks executing across a multi-thread pool.
 *     Use this if you have a general Concurrent_task_loop (of whatever actual non-abstract type) which may
 *     at times use multiple threads.  In other words use it if you want a Concurrent_task_loop... but timed.
 *   - Timed_single_thread_task_loop: This is a refinement (subclass) of Single_thread_task_loop.
 *     Internally it uses a `Timed_concurrent_task_loop<perf::duration_rep_t>`, since no `atomic` is needed with
 *     only 1 thread of execution.  In other words use it if you want a Single_thread_task_loop... but timed.
 *
 * @see Timed_concurrent_task_loop doc header for instructions/synopsis of timing functionality.
 *
 * @tparam Time_accumulator
 *         As of this writing this should be either perf::duration_rep_t; or `atomic<perf::duration_rep_t>`,
 *         depending on whether the `Timed_*_loop` that uses this template instance is guaranteed to be
 *         single-threaded or potentially a multi-thread pool.
 */
template<typename Time_accumulator>
class Timed_concurrent_task_loop_impl : public Concurrent_task_loop
{
public:
  // Types.

  /// Short-hand for the exchanger function taken by the ctor.
  using Exchanger_func = Function<perf::duration_rep_t (Time_accumulator*)>;

  // Constructors/destructor.

  /**
   * Constructs a time-accumulating version of existing loop `*loop`.  `*loop` must exist at least until
   * `*this` is destroyed.
   *
   * @param clock_type
   *        See timed_function().  Use a perf::Clock_type that tracks thread time, not process time, typically.
   *        That said, in single-thread loops without competing concurrent work on the same machine,
   *        more precise non-per-thread clocks may be suitable (it is a trade-off).
   * @param loop
   *        The loop object being decorated with timing functionality.
   * @param exchanger_func_moved
   *        If `A` is a `Time_accumulator`, then `exchanger_func_moved()` must return
   *        the perf::duration_rep_t currently stored in `A` and -- as atomically as possible --
   *        store `0` in `A` before returning that.  More specifically, if `Time_accumulator` is an `atomic`,
   *        then use `.exchange(0)`; if it's simply perf::duration_rep_t, then it can save the value, set it to 0,
   *        then return the saved thing (as concurrency is not a concern).
   */
  explicit Timed_concurrent_task_loop_impl(Concurrent_task_loop* loop, perf::Clock_type clock_type,
                                           Exchanger_func&& exchanger_func_moved);

  // Methods.

  /**
   * Implements superclass API.
   * @param init_task_or_empty
   *        See superclass API.
   * @param thread_init_func_or_empty
   *        See superclass API.
   */
  void start(Task&& init_task_or_empty = Task{},
             const Thread_init_func& thread_init_func_or_empty = Thread_init_func{}) override;

  /// Implements superclass API.
  void stop() override;

  /**
   * Implements superclass API.
   * @return See superclass API.
   */
  size_t n_threads() const override;

  /**
   * Implements superclass API.
   * @return See superclass API.
   */
  Op create_op() override;

  /**
   * Implements superclass API.
   * @return See superclass API.
   */
  const Op_list& per_thread_ops() override;

  /**
   * Implements superclass API.  In this implementation: `task` will execute as soon as a thread is available, because
   * no other applicable work is forthcoming.
   *
   * @param task
   *        See superclass API.
   * @param synchronicity
   *        See superclass API.
   */
  void post(Task&& task, Synchronicity synchronicity = Synchronicity::S_ASYNC) override;

  /**
   * Implements superclass API.
   *
   * In this implementation: `task` will execute as soon as a thread is available, because
   * no other applicable work is forthcoming, plus one more constraint: It will not allow `task` to execute concurrently
   * to any other `Task` also associated with `op`.  The latter is accomplished on account of
   * `Op` internally being associated with a util::Strand.
   *
   * @param op
   *        See superclass API.
   * @param task
   *        See superclass API.
   * @param synchronicity
   *        See superclass API.
   */
  void post(const Op& op, Task&& task, Synchronicity synchronicity = Synchronicity::S_ASYNC) override;

  /**
   * Implements superclass API.
   * @param from_now
   *        See superclass API.
   * @param task
   *        See superclass API.
   * @return See superclass API.
   */
  util::Scheduled_task_handle schedule_from_now(const Fine_duration& from_now, Scheduled_task&& task) override;

  /**
   * Implements superclass API.
   * @param at
   *        See superclass API.
   * @param task
   *        See superclass API.
   * @return See superclass API.
   */
  util::Scheduled_task_handle schedule_at(const Fine_time_pt& at, Scheduled_task&& task) override;

  /**
   * Implements superclass API.
   * @param op
   *        See superclass API.
   * @param from_now
   *        See superclass API.
   * @param task
   *        See superclass API.
   * @return See superclass API.
   */
  util::Scheduled_task_handle schedule_from_now(const Op& op,
                                                const Fine_duration& from_now, Scheduled_task&& task) override;

  /**
   * See superclass API.
   * @param op
   *        See superclass API.
   * @param at
   *        See superclass API.
   * @param task
   *        See superclass API.
   * @return See superclass API.
   */
  util::Scheduled_task_handle schedule_at(const Op& op,
                                          const Fine_time_pt& at, Scheduled_task&& task) override;

  /**
   * See superclass API.
   * @return See superclass API.
   */
  Task_engine_ptr task_engine() override;

  /**
   * Returns the accumulated time spent on tasks this thread pool handles since its last invocation; and reset the
   * accumulated time.
   *
   * @see Constructor which controls the behavior of the timing.
   *
   * @return See above.
   */
  perf::Duration accumulated_time();

  /**
   * Given a boost.asio *completion handler* `handler` for a boost.asio `async_*()` action on some boost.asio I/O
   * object to be initiated in the immediate near future, returns a wrapped handler with the same signature
   * to be passed as the handler arg to that `async_*()` action, so that `handler()` will be properly timed.
   *
   * The mechanics of using this are explained in the class doc header.  Using this in any other
   * fashion leads to undefined behavior.
   *
   * @tparam Handler
   *         See asio_handler_via_op(): same here.
   * @param handler_moved
   *        Completion handler for the boost.asio `async_*()` operation to be initiated soon.
   *        It may be `move`d and saved.
   * @return A completion handler that will act as `handler()` but with the addition of timing as
   *         returned by accumulated_time().
   */
  template<typename Handler>
  auto asio_handler_timed(Handler&& handler_moved);

private:
  // Data.

  /// See constructor.
  const flow::perf::Clock_type m_clock_type;

  /// See constructor.
  Concurrent_task_loop* const m_loop;

  /// See constructor.
  const Exchanger_func m_exchanger_func;

  /// Accumulates time ticks, of clock type #m_clock_type, spent in tasks posted onto #m_loop.
  Time_accumulator m_time_accumulator;
}; // class Timed_concurrent_task_loop_impl

/**
 * Decorates a general -- potentially multi-threaded -- Concurrent_task_loop of any kind but with timing capabilities.
 *
 * Using one of these is identical to using the underlying Concurrent_task_loop; except that after constructing it
 * it must be passed to this class's ctor.  From that point on, `*this` is identical to that underlying loop object,
 * except that all relevant handlers posted onto it will be timed.  One can then access the result (and reset the
 * accumulator to zero) simply by calling accumulated_time().
 *
 * The following tasks shall be timed, with results accessible via accumulated_time():
 *   - Anything posted via post().
 *   - Anything scheduled via `schedule_task_*()`.
 *   - Anything invoked directly through a boost.asio `async_*()` API, as long as:
 *     - either it is wrapped via asio_handler_via_op() (to execute through an async::Op);
 *     - or it is wrapped via asio_handler_timed() (otherwise).
 *
 * @warning Unfortunately, any "naked" handler passed to a boost.asio async procedure will not be timed;
 *          so don't forget to use a wrapper (presumably asio_handler_timed()).
 *          The key point: 3/4 posting techniques listed above are identical in use to general use of
 *          Concurrent_task_loop as explained in that class's doc header.  The exception is the need to
 *          use asio_handler_timed() when dealing with "naked" boost.asio handlers (the last bullet point above).
 */
class Timed_concurrent_task_loop : public Timed_concurrent_task_loop_impl<std::atomic<perf::duration_rep_t>>
{
public:
  // Constructors/destructor.

  /**
   * Constructs a time-accumulating version of existing loop `*loop`.  `*loop` must exist at least until
   * `*this` is destroyed.
   *
   * @param clock_type
   *        See Timed_concurrent_task_loop_impl ctor.
   * @param loop
   *        See Timed_concurrent_task_loop_impl ctor.
   */
  explicit Timed_concurrent_task_loop(Concurrent_task_loop* loop,
                                      perf::Clock_type clock_type = perf::Clock_type::S_CPU_THREAD_TOTAL_HI_RES);

private:
  // Types.

  /// Convenience alias to our thread-safe tick accumulator type.
  using Time_accumulator = std::atomic<perf::duration_rep_t>;

  /// Convenience alias to our superclass.
  using Impl = Timed_concurrent_task_loop_impl<Time_accumulator>;
}; // class Timed_concurrent_task_loop

// Template implementations.

template<typename Time_accumulator>
Timed_concurrent_task_loop_impl<Time_accumulator>::Timed_concurrent_task_loop_impl
  (Concurrent_task_loop* loop, perf::Clock_type clock_type, Exchanger_func&& exchanger_func_moved) :
  m_clock_type(clock_type),
  m_loop(loop),
  m_exchanger_func(std::move(exchanger_func_moved)),
  m_time_accumulator(0)
{
  // That's it.
}

template<typename Time_accumulator>
void Timed_concurrent_task_loop_impl<Time_accumulator>::start
       (Task&& init_task_or_empty, const Thread_init_func& thread_init_func_or_empty)
{
  m_loop->start(std::move(init_task_or_empty), thread_init_func_or_empty);
}

template<typename Time_accumulator>
void Timed_concurrent_task_loop_impl<Time_accumulator>::stop()
{
  m_loop->stop();
}

template<typename Time_accumulator>
size_t Timed_concurrent_task_loop_impl<Time_accumulator>::n_threads() const
{
  return m_loop->n_threads();
}

template<typename Time_accumulator>
Op Timed_concurrent_task_loop_impl<Time_accumulator>::create_op()
{
  return m_loop->create_op();
}

template<typename Time_accumulator>
const Op_list& Timed_concurrent_task_loop_impl<Time_accumulator>::per_thread_ops()
{
  return m_loop->per_thread_ops();
}

template<typename Time_accumulator>
void Timed_concurrent_task_loop_impl<Time_accumulator>::post(Task&& task, Synchronicity synchronicity)
{
  m_loop->post(perf::timed_function(m_clock_type, &m_time_accumulator, std::move(task)),
               synchronicity);
}

template<typename Time_accumulator>
void Timed_concurrent_task_loop_impl<Time_accumulator>::post(const Op& op, Task&& task, Synchronicity synchronicity)
{
  m_loop->post(op,
               perf::timed_function(m_clock_type, &m_time_accumulator, std::move(task)),
               synchronicity);
}

template<typename Time_accumulator>
util::Scheduled_task_handle Timed_concurrent_task_loop_impl<Time_accumulator>::schedule_from_now
                              (const Fine_duration& from_now, Scheduled_task&& task)
{
  return m_loop->schedule_from_now(from_now,
                                   perf::timed_function(m_clock_type, &m_time_accumulator, std::move(task)));
}

template<typename Time_accumulator>
util::Scheduled_task_handle Timed_concurrent_task_loop_impl<Time_accumulator>::schedule_at
                              (const Fine_time_pt& at, Scheduled_task&& task)
{
  return m_loop->schedule_at(at,
                             perf::timed_function(m_clock_type, &m_time_accumulator, std::move(task)));
}

template<typename Time_accumulator>
util::Scheduled_task_handle Timed_concurrent_task_loop_impl<Time_accumulator>::schedule_from_now
                              (const Op& op, const Fine_duration& from_now, Scheduled_task&& task)
{
  return m_loop->schedule_from_now(op, from_now,
                                   perf::timed_function(m_clock_type, &m_time_accumulator, std::move(task)));
}

template<typename Time_accumulator>
util::Scheduled_task_handle Timed_concurrent_task_loop_impl<Time_accumulator>::schedule_at
                              (const Op& op, const Fine_time_pt& at, Scheduled_task&& task)
{
  return m_loop->schedule_at(op, at,
                             perf::timed_function(m_clock_type, &m_time_accumulator, std::move(task)));
}

template<typename Time_accumulator>
Task_engine_ptr Timed_concurrent_task_loop_impl<Time_accumulator>::task_engine()
{
  return m_loop->task_engine();
}

template<typename Time_accumulator>
perf::Duration Timed_concurrent_task_loop_impl<Time_accumulator>::accumulated_time()
{
  return perf::Duration{m_exchanger_func(&m_time_accumulator)};
}

template<typename Time_accumulator>
template<typename Handler>
auto Timed_concurrent_task_loop_impl<Time_accumulator>::asio_handler_timed(Handler&& handler_moved)
{
  return perf::timed_handler(m_clock_type, &m_time_accumulator, std::move(handler_moved));
  // Note that timed_handler() specifically promises to not "lose" any executor (e.g., strand) bound to `handler_moved`.
}

} // namespace flow::async
