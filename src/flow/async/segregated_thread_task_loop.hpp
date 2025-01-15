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
#include "flow/async/detail/async_fwd.hpp"
#include "flow/async/concurrent_task_loop.hpp"
#include "flow/log/log.hpp"
#include <boost/move/unique_ptr.hpp>
#include <vector>
#include <string>

namespace flow::async
{

/**
 * Concrete Concurrent_task_loop that uses the legacy pin-`Task`s-within-1-`Op`-to-1-thread method of achieving
 * required non-concurrency of `Task`s.
 *
 * @see Concurrent_task_loop doc header for an overview of this class in the context of sibling classes (alternatives).
 *
 * By definition all you need to know to use it is in the super-interface Concurrent_task_loop doc header
 * (except constructor semantics of course).  However, in choosing this vs. a sibling type, one must at least
 * loosely understand its underlying behavior.  To summarize the behavior of this class beyond the interface guarantees:
 *   - A no-`Op` stand-alone `Task` executes as soon as thread T becomes available, and T is randomly chosen.
 *   - An `Op`-sharing `Task` will *always* execute in the same thread T as the first `Task`; and that T is randomly
 *     chosen at the time of that 1st Task.  This ensures they won't execute concurrently by definition.
 *   - per_thread_ops() is organized as follows: If N worker threads are created, then, internally,
 *     `per_thread_ops()[i]` is really the i-th thread.  Very simple.
 *     - See discussion in Cross_thread_task_loop doc header about an "interesting experiment" related to us.
 *
 * @internal
 *
 * @todo In Segregated_thread_task_loop, when dealing with the
 * 2-arg `post()` (the one taking an async::Op and an async::Task) and similar
 * `schedule_*()` methods -- consider postponing the random thread selection until the last possible moment, as
 * opposed to in create_op().  Given that Segregated_thread_task_loop is legacy-ish in nature, the resulting
 * significant increase in internal complexity may or may not be worth accepting.  This to-do also makes little
 * practical difference without the nearby to-do that would bias in favor of less-loaded threads.
 *
 * @todo In Segregated_thread_task_loop, when randomly selecting a thread, considering trying to bias in favor of
 * threads with less load (perhaps measured as # of tasks enqueued in a given thread's `Task_engine`); and/or
 * round-robin thread assignment; and/or other heuristics.
 */
class Segregated_thread_task_loop :
  public Concurrent_task_loop,
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Constructors/destructor.

  /**
   * Constructs object, making it available for post() and similar work, but without starting any threads and hence
   * without the ability to perform any work; call start() to spawn threads and perform work.
   *
   * The arguments `n_threads_or_zero` and ones following it should be viewed as configuration for the next
   * start() call.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname
   *        Brief, human-readable nickname of the new thread pool, as of this writing for logging only.
   * @param n_threads_or_zero
   *        If non-zero, start() will start exactly this many worker threads; and the system will be entirely entrusted
   *        with the question of which thread is assigned to which processor core and any subsequent migration.
   *        If zero, start() attempts to optimize the thread count and thread-to-core relationship itself, per
   *        optimal_worker_thread_count_per_pool() and optimize_pinning_in_thread_pool().  The thread pool will choose
   *        the worker thread count and any thread-to-core relationships as per those methods, having made the
   *        assumptions documented for those functions.
   * @param est_hw_core_sharing_helps_algo
   *        See optimal_worker_thread_count_per_pool().
   * @param est_hw_core_pinning_helps_algo
   *        See optimize_pinning_in_thread_pool().
   * @param hw_threads_is_grouping_collated
   *        See optimize_pinning_in_thread_pool().
   */
  explicit Segregated_thread_task_loop(log::Logger* logger_ptr, util::String_view nickname,
                                       size_t n_threads_or_zero,
                                       bool est_hw_core_sharing_helps_algo = false,
                                       bool est_hw_core_pinning_helps_algo = false,
                                       bool hw_threads_is_grouping_collated = false);

  /// See superclass destructor.
  ~Segregated_thread_task_loop() override;

  // Methods.

  /**
   * Implements superclass API.  In this implementation this essentially involves repeating N times:
   * `Task_engine::restart()`; spawn thread; in it execute a long-running `Task_engine::run()`.
   *
   * ### Logging subtlety for large thread pools ###
   * See Cross_thread_task_loop::start() doc header similar section: The exact same note applies here.
   *
   * @param init_task_or_empty
   *        See superclass API.
   * @param thread_init_func_or_empty
   *        See superclass API.
   */
  void start(Task&& init_task_or_empty = Task(),
             const Thread_init_func& thread_init_func_or_empty = Thread_init_func()) override;

  /**
   * Implements superclass API.  In this implementation this essentially boils down to N `Task_engine::stop()`s,
   * followed by joining each thread started by start().
   *
   * ### Logging subtlety for large thread pools ###
   * See Cross_thread_task_loop::stop() doc header similar section: The exact same note applies here.
   */
  void stop() override;

  /**
   * Implements superclass API.
   * @return See superclass API.
   */
  size_t n_threads() const override;

  /**
   * Implements superclass API.  In this implementation: async::Op has the "weight" of a raw pointer and internally
   * corresponds to a thread.
   *
   * @return See superclass API.
   */
  Op create_op() override;

  /**
   * Implements superclass API.  In this implementation: Each pre-created `Op` internally corresponds (1-to-1 overall)
   * to one of the worker threads spawned at construction.
   *
   * @return See superclass API.
   */
  const Op_list& per_thread_ops() override;

  /**
   * Implements superclass API.  In this implementation: `task` will execute as soon as thread T is available, because
   * no other applicable work is forthcoming; and T is randomly chosen by the implementation.
   *
   * @param task
   *        See superclass API.
   * @param synchronicity
   *        See superclass API.
   */
  void post(Task&& task, Synchronicity synchronicity = Synchronicity::S_ASYNC) override;

  /**
   * Implements superclass API.  In this implementation: identical to 1-arg post() for the *first* async::Task to be
   * associated with `op` (randomly choose T, then basically 1-arg-post() equivalent); for subsequent such `Task`s
   * act similarly but simply keep that same T.
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
   * Implements superclass API.  See also 1-arg post().
   * @param from_now
   *        See superclass API.
   * @param task
   *        See superclass API.
   * @return See superclass API.
   */
  util::Scheduled_task_handle schedule_from_now(const Fine_duration& from_now, Scheduled_task&& task) override;

  /**
   * Implements superclass API.  See also 1-arg post().
   * @param at
   *        See superclass API.
   * @param task
   *        See superclass API.
   * @return See superclass API.
   */
  util::Scheduled_task_handle schedule_at(const Fine_time_pt& at, Scheduled_task&& task) override;

  /**
   * Implements superclass API.  See also 2-arg post().
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
   * See superclass API.  See also 2-arg post().
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
   * See superclass API.  In this implementation: As with create_op(), this selects a thread from the pool randomly,
   * and each thread has a 1-1 dedicated internal `Task_engine`.  The load-balancing is hence performed, by us,
   * at the time of this call -- not later when one does the `async_*()` action.  (Reminder: general
   * `Concurrent_task_loop`-using code would not rely on this implementation information.)  (Reminder: be very sure
   * you understand what will happen if/when you pass this into a boost.asio-compliant I/O object's constructor or
   * similar.  See the warning on this topic in Concurrent_task_loop doc header.)
   *
   * @return See superclass API.
   */
  Task_engine_ptr task_engine() override;

private:
  // Types.

  /// Short-hand for smart pointer to Task_qing_thread.
  using Task_qing_thread_ptr = boost::movelib::unique_ptr<Task_qing_thread>;

  // Methods.

  /**
   * Helper performing the core `Task_engine::post()` (or similar) call on behalf of the various `post()` overloads.
   *
   * @param chosen_task_engine
   *        The value `m_qing_threads[idx].task_engine().` for some valid `idx`: the engine for the thread selected
   *        to execute `task`.
   * @param synchronicity
   *        See any post().
   * @param task
   *        See any post().
   */
  void post_impl(const Task_engine_ptr& chosen_task_engine, Synchronicity synchronicity, Task&& task);

  /**
   * Helper performing the core util::schedule_task_from_now() call on behalf of the various
   * `schedule_from_now()` overloads.
   *
   * @param chosen_task_engine
   *        See post_impl().
   * @param from_now
   *        See any schedule_from_now().
   * @param task
   *        See any schedule_from_now().
   * @return See any schedule_from_now().
   */
  util::Scheduled_task_handle schedule_from_now_impl(const Task_engine_ptr& chosen_task_engine,
                                                     const Fine_duration& from_now,
                                                     Scheduled_task&& task);

  // Data.

  /// See constructor.
  const std::string m_nickname;
  /// See constructor.  @warning Not safe to use this in lieu of n_threads() when this is in fact zero.
  const size_t m_n_threads_or_zero;
  /// See constructor.
  const bool m_est_hw_core_sharing_helps_algo;
  /// See constructor.
  const bool m_est_hw_core_pinning_helps_algo;
  /// See constructor.
  const bool m_hw_threads_is_grouping_collated;

  /**
   * N task-execution-capable worker threads whose lifetimes equal those of `*this`, each with its own
   * util::Task_engine.  Due to the latter fact, util::Strand would not be useful to implement async::Op
   * semantics; as every `Task_engine` involved `run()`s within one thread only.
   *
   * @warning Outside of ctor, this datum must not be accessed except in start() and stop(), as that would break
   *          the thread-safety guarantee wherein post() (and similar) are safe to call concurrently with
   *          start() or stop().  All such work shall be done through #m_task_engines exclusively.
   */
  std::vector<Task_qing_thread_ptr> m_qing_threads;

  /**
   * boost.asio `Task_engine`s (a/k/a `io_context`s) used by each respective element in #m_qing_threads.
   * It is critical that this not be modified after constructor returns, for the same thread safety reason mentioned
   * in the warning in #m_qing_threads doc header.
   */
  std::vector<Task_engine_ptr> m_task_engines;

  /**
   * See per_thread_ops().  The `boost::any` payload is always of the type `Task_engine_ptr`.
   * The latter not being something else, namely Task_qing_thread* or similar, is due to the same thread safety
   * reason mentioned in the warning in #m_qing_threads doc header.
   */
  boost::movelib::unique_ptr<const Op_list> m_per_thread_ops;
}; // class Segregated_thread_task_loop

// Free functions: in *_fwd.hpp.

} // namespace flow::async
