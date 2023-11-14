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
 * Concrete Concurrent_task_loop that is able to efficiently schedule `Task`s within a given `Op` to execute in
 * different threads while still properly avoiding concurrency.
 *
 * @see Concurrent_task_loop doc header for an overview of this class in the context of sibling classes (alternatives).
 *
 * By definition all you need to know to use it is in the super-interface Concurrent_task_loop doc header
 * (except constructor semantics of course).  However, in choosing this vs. a sibling type, one must at least
 * loosely understand its underlying behavior.  To summarize the behavior of this class beyond the interface guarantees:
 *   - A no-`Op` stand-alone `Task` executes simply as soon as any one thread becomes available.
 *   - An `Op`-sharing `Task` may execute in a different thread than another `Task` sharing the same `Op`.
 *     (They just won't execute *concurrently*.)  If you're familiar with boost.asio: This internally uses
 *     util::Strand to achieve that behavior.
 *   - per_thread_ops() is organized as follows: If N worker threads are created, then accordingly N strands are
 *     created internally.  Internally, `per_thread_ops()[i]` is really the i-th strand, not any thread, and hence even
 *     though it is "per-thread," actual execution may occur in other thread(s) than the i-th.  However, user logic
 *     can remain exactly as if it were truly per-thread.
 *     Why?  Answer: A thread is a thing where stuff can only execute serially
 *     within it -- but so is a strand!  And we have N of those, just like we have N threads; so it works by symmetry.
 *     So if there are N strands across N threads, then subdividing work among the N
 *     strands results in the same non-concurrency guarantees as doing so among the N threads directly.  In fact,
 *     it might be somewhat more efficient in using idle thread time across the pool.
 *     - This allows for an interesting experiment if a legacy application is written in an explicitly per-thread way,
 *       maintaining per-thread data structures for example: Suppose one tweaks it to use Concurrent_task_loop.
 *       One can convert its tasks (conceptually speaking) and data structures to refer to
 *       per_thread_ops()-returned N `Op`s, instead of the N threads directly, and further modify it to work with
 *       a Concurrent_task_loop.  Now, if the latter is a Segregated_thread_task_loop, then the resulting program
 *       will work identically (functionally speaking) to how it worked before these changes: one has perhaps made it
 *       more conceptual/readable/maintainable, but the thread arch is unchanged.  The interesting part is next:
 *       To see whether one can gain efficiency from using a strand-based approach, but
 *       without making any real design changes in this legacy application, all one has to do now is replace the
 *       Segregated_thread_task_loop construction with a Cross_thread_task_loop one.  All application logic can remain
 *       exactly identical (after initialization), yet efficiency gains might be achieved for "free" -- simply by
 *       changing the concrete type of the central Concurrent_task_loop.  One can
 *       even easily use one or the other based on config, changing between the two in the field.
 *
 * @todo Add dynamic configurability of low-level thread/core behavior of Cross_thread_task_loop,
 * Segregated_thread_task_loop, and the Concurrent_task_loop interface generally, as these parameters
 * (including `n_threads_or_zero`, `est_hw_core_sharing_helps_algo`, `est_hw_core_pinning_helps_algo`)
 * can only be set at construction time even though start() and stop() can be invoked anytime.
 * For instance a non-`virtual` `configure()` method could be added to each Concurrent_task_loop subclass,
 * potentially with different signatures.  Note, also, that the decision to have stop() exit the actual threads,
 * in addition to making some things easier to code/reason about internally, is also more amenable to these dynamic
 * changes -- messing with threading behavior dynamically is easier if one can exit and re-spawn the thread pool.
 *
 * @internal
 *
 * Regarding the dynamic-configurability to-do above: By my (ygoldfel) off-top-of-head estimation,
 * it's easier to add in Cross_thread_task_loop than in Segregated_thread_task_loop, as there is exactly 1
 * util::Task_engine per `*this` in the former, while Segregated_thread_task_loop has N of them, 1 per thread.
 * So changing the desired thread count, in particular, would be more difficult in the latter case, as one must not
 * lose any enqueued tasks through stop() and start().  It would not be unreasonable to make one more
 * dynamically-configurable than the other.
 *
 * One must also be careful about the legacy-ish mechanism per_thread_ops() in this context.
 */
class Cross_thread_task_loop :
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
  explicit Cross_thread_task_loop(log::Logger* logger_ptr, util::String_view nickname,
                                  size_t n_threads_or_zero,
                                  bool est_hw_core_sharing_helps_algo = false,
                                  bool est_hw_core_pinning_helps_algo = false,
                                  bool hw_threads_is_grouping_collated = false);

  /// See superclass destructor.
  ~Cross_thread_task_loop() override;

  // Methods.

  /**
   * Implements superclass API.  In this implementation this essentially involves a single `Task_engine::restart()`,
   * followed by starting N threads, each of which executing a long-running `Task_engine::run()`.
   *
   * ### Logging subtlety for large thread pools ###
   * If n_threads() is large -- e.g., hundreds -- this method may produce a large number of log::Sev::S_INFO level
   * log messages to log::Logger at `get_logger()`.  There may be a few lines, split among this thread and the
   * spawned thread, per each of n_threads() threads.  In addition stop() and/or the destructor will produce a similar
   * volume of INFO messages.
   *
   * You may, however, use `log::Config::this_thread_verbosity_override_auto(X)` just before start().
   * X = log::Sev::S_NONE will disable all logging; while X = log::Sev::S_WARNING will skip anything but error
   * conditions.
   *
   * This is not special and can be done anytime in any context: it affects any logging through log::Config
   * in this thread.  In addition: If indeed such an X is set at entry to start(), we will intentionally affect
   * each spawned thread as well, albeit only around startup and shutdown.  This is for your convenience.
   *
   * @param init_task_or_empty
   *        See superclass API.
   * @param thread_init_func_or_empty
   *        See superclass API.
   */
  void start(Task&& init_task_or_empty = Task(),
             const Thread_init_func& thread_init_func_or_empty = Thread_init_func()) override;

  /**
   * Implements superclass API.  In this implementation this essentially boils down to a single `Task_engine::stop()`,
   * followed by joining each thread started by start().
   *
   * ### Logging subtlety for large thread pools ###
   * A similar note from start() doc header applies here.  Again: you may set an override verbosity just ahead of
   * stop() to reduce or disable logging if desired.
   *
   * In addition, for your convenience: Any shutdown logging within each of the spawned (now shutting down) threads
   * will follow the override (if any) you had specified ahead of start().
   */
  void stop() override;

  /**
   * Implements superclass API.
   * @return See superclass API.
   */
  size_t n_threads() const override;

  /**
   * Implements superclass API.  In this implementation: async::Op has the "weight" of a `shared_ptr<>` and internally
   * corresponds to a util::Strand.
   *
   * @return See superclass API.
   */
  Op create_op() override;

  /**
   * Implements superclass API.  In this implementation: Each pre-created `Op` internally corresponds to a
   * pre-created long-lived util::Strand.
   *
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
   * See superclass API.  In this implementation: Internally there is just this `Task_engine`, and that is what this
   * returns.  The load-balancing is performed directly by boost.asio itself.  (Reminder: general
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

  /// N task-execution-capable worker threads whose lifetimes equal those of `*this`, all sharing #m_shared_task_engine.
  std::vector<Task_qing_thread_ptr> m_qing_threads;

  /// boost.asio `Task_engine` (a/k/a `io_service`) co-used by all #m_qing_threads.
  Task_engine_ptr m_shared_task_engine;

  /// See per_thread_ops().  The `boost::any` payload is always of the type async::Strand_ptr.
  const Op_list m_per_thread_strands;
}; // class Cross_thread_task_loop

// Free functions: in *_fwd.hpp.

} // namespace flow::async
