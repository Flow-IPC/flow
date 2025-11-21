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
#include "flow/async/segregated_thread_task_loop.hpp"
#include "flow/async/detail/task_qing_thread.hpp"
#include "flow/async/util.hpp"
#include "flow/util/sched_task.hpp"

namespace flow::async
{

// Implementations.

Segregated_thread_task_loop::Segregated_thread_task_loop(log::Logger* logger_ptr, util::String_view nickname,
                                                         size_t n_threads_or_zero,
                                                         bool est_hw_core_sharing_helps_algo,
                                                         bool est_hw_core_pinning_helps_algo,
                                                         bool hw_threads_is_grouping_collated) :
  log::Log_context(logger_ptr, Flow_log_component::S_ASYNC),

  m_nickname(nickname),
  m_n_threads_or_zero(n_threads_or_zero),
  m_est_hw_core_sharing_helps_algo(est_hw_core_sharing_helps_algo),
  m_est_hw_core_pinning_helps_algo(est_hw_core_pinning_helps_algo),
  m_hw_threads_is_grouping_collated(hw_threads_is_grouping_collated),

  /* Note to reader: If you grok the doc headers of superclass and Segregated_thread_task_loop, the following
   * is easy to understand.  If you don't, then to make it clear I'd have to repeat all of that here, basically.
   * So please grok it first. */

  m_qing_threads((m_n_threads_or_zero == 0) // n_threads() nulls: still need to set each one after this.
                   ? optimal_worker_thread_count_per_pool(logger_ptr, m_est_hw_core_sharing_helps_algo)
                   : m_n_threads_or_zero),
  // n_threads() is now accurate.
  m_task_engines(n_threads()) // n_threads() nulls: still need to set each one after this.
{
  using util::Task_engine;

  /* m_per_thread_task_engines will store the `n` Task_engines.  These will be cted now and destroyed in dtor;
   * they will survive any stop()s or start()s (which stop and start all the threads in thread pool respectively).
   * For each Task_engine E, post(E, F) (etc.) is safe while E.stopped() is false, or true, or even across that value
   * changing, by boost.asio docs guaranteeing this tread safety. */
  for (Task_engine_ptr& task_engine_ptr_in_container : m_task_engines)
  {
    // Attn: The concurrency-hint=1 may avoid or all most locking in boost.asio.  Exactly 1 thread in the Task_engine.
    task_engine_ptr_in_container.reset(new Task_engine{1});

    /* Task_engine starts in !stopped() mode ready to run().  start() pre-condition is stopped() so for simplicity
     * start in the same state that our stop() would put the Task_engine into: */
    task_engine_ptr_in_container->stop();
    // Now our start() can always do the sequence: restart() (to make it !stopped()), then run().
  }

  // Initialize our Ops_list of pre-created Ops which in our case simply store all `n` `Task_engine_ptr`s.
  const size_t n = n_threads();
  m_per_thread_ops.reset(new Op_list{get_logger(), n,
                                     [this](size_t idx) -> Op
                                       { return Op{static_cast<Task_engine_ptr>(m_task_engines[idx])}; }});
  /* (The static_cast<> is probably unnecessary but makes the compiler check our type logic for us.  That's quite
   * helpful in this rare situation where we're essentially using a dynamically typed variable in C++ [boost::any].
   * There is 0 perf cost to it by the way.) */

  FLOW_LOG_INFO("Segregated_thread_task_loop [" << static_cast<const void*>(this) << "] "
                "with nickname [" << m_nickname << "] "
                "with segregated-thread scheduling via individual Task_engines "
                "across [" << n << "] threads-to-be: "
                "Created; can accept work.  Task_qing_thread(s) not started yet until start().");

  /* Each Task_engine can now be post()ed onto and otherwise used with boost.asio; won't do anything until we
   * start threads and run() it.  start() does that.  We're done. */
} // Segregated_thread_task_loop::Segregated_thread_task_loop()

Segregated_thread_task_loop::~Segregated_thread_task_loop() // Virtual.
{
  FLOW_LOG_INFO("Segregated_thread_task_loop [" << this << "]: Destroying object; will stop threads/tasks unless "
                "already stopped earlier.");
  stop();

  /* m_qing_threads.clear(), m_task_engines.clear() equivalents will now happen automatically.  That is mere cleanup of
   * stuff in memory that may include the destruction of (already stop()ped!) Task_engines. */
}

void Segregated_thread_task_loop::start(Task&& init_task_or_empty,
                                        const Thread_init_func& thread_init_func_or_empty) // Virtual.
{
  using util::Thread;
  using util::Task_engine;
  using boost::promise;
  using std::transform;
  using std::vector;

  /* Is the check thread-safe?  Yes, since Concurrent_task_loop::stop() must not be called concurrently with itself or
   * start() per our contract in doc header. */
  if (!m_task_engines.front()->stopped()) // They're all started/stopped together, so check random guy #1.
  {
    FLOW_LOG_INFO("Starting Segregated_thread_task_loop [" << this << "]: Already started earlier.  Ignoring.");
    return;
  }
  // else

  const size_t n = n_threads();
  FLOW_LOG_INFO("Segregated_thread_task_loop [" << this << "] with nickname [" << m_nickname << "]: "
                "1-to-1 Task_engines across [" << n << "] Task_qing_threads: Starting.");

  /* Create/start the threads.
   * See same logic in Cross_thread_task_loop, including comments.  It's too brief to worry about code reuse here.
   * Note, though, that we could just this->post(op, thread_init_func..., S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION)
   * for each `op` in per_thread_ops().  We promised to run thread_init_func...() *first-thing* in its thread,
   * though, so let's keep to the letter of our contract.  Also, this way we can do it in parallel instead of
   * serially. */

  vector<promise<void>> thread_init_done_promises{n};
  for (size_t idx = 0; idx != n; ++idx)
  {
    Task task_qing_thread_init_func;
    if (!thread_init_func_or_empty.empty())
    {
      task_qing_thread_init_func = [idx, &thread_init_func_or_empty]()
      {
        thread_init_func_or_empty(idx);
      };
    }
    else
    {
      assert(task_qing_thread_init_func.empty()); // Just leave it.
    }

    const Task_engine_ptr& task_engine = m_task_engines[idx];

    // boost.asio subtlety: While stopped(), run() will instantly return, unless one does this first.
    task_engine->restart();
    // Now its Task_qing_thread can do ->run() as most of its thread body (and it won't just return).

    // Create/start the thread.
    m_qing_threads[idx].reset(new Task_qing_thread{get_logger(),
                                                   (n == 1) ? m_nickname : util::ostream_op_string(m_nickname, idx),
                                                   task_engine, true, // Its *own* 1-1 Task_engine.
                                                   &(thread_init_done_promises[idx]),
                                                   std::move(task_qing_thread_init_func)});
  } // for (idx in [0, n))
  FLOW_LOG_INFO("All threads are asynchronously starting.  Awaiting their readiness barrier-style, in sequence.");
  for (size_t idx = 0; idx != n; ++idx)
  {
    thread_init_done_promises[idx].get_future().wait();
    FLOW_LOG_INFO("Thread [" << idx << "] (0-based) of [" << n << "] (1-based) is ready.");
  }

  // Threads are running and ready for work.

  // See same logic in Cross_thread_task_loop, including comments.  It's too brief to worry about code reuse here.
  if (m_n_threads_or_zero == 0)
  {
    FLOW_LOG_INFO("Thread count was auto-determined.  Further attempting thread-to-core scheduling optimization.");

    vector<Thread*> worker_threads{n}; // Initialized to nulls.  Now set them to the raw `Thread*`s.
    transform(m_qing_threads.begin(), m_qing_threads.end(), worker_threads.begin(),
              [](const Task_qing_thread_ptr& qing_thread_ptr) -> Thread*
                { return qing_thread_ptr->raw_worker_thread(); });
    optimize_pinning_in_thread_pool(get_logger(), worker_threads,
                                    m_est_hw_core_sharing_helps_algo, m_est_hw_core_pinning_helps_algo,
                                    m_hw_threads_is_grouping_collated);
  }

  /* That's it; ready for work... but help out by running this optional init task: the key is this will wait
   * until that task completes in a spawned thread.  So we return only once that has returned. */
  if (!init_task_or_empty.empty())
  {
    post(std::move(init_task_or_empty), Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
  }

  /* Sanity check: start() isn't thread-safe against stop() and start(), as we warned in our contract.
   * But we've also promised that other API calls *are* thread-safe, including against
   * stop().  To mentally test that <see similar comment in stop()>.  TL;DR: post() and similar APIs work via
   * m_task_engines safely, even as underlying thread(s) start or stop and m_task_engines[]->run(). */
} // Segregated_thread_task_loop::start()

void Segregated_thread_task_loop::stop() // Virtual.
{
  // This is probably unnecessary, but... <see similar comment in Cross_thread_task_loop::stop()>.
  if (m_task_engines.front()->stopped()) // They're all started/stopped together, so check random guy #1.
  {
    FLOW_LOG_INFO("Stopping Segregated_thread_task_loop [" << this << "]: Already stopped earlier.  Ignoring.");
    return;
  }
  // else

  FLOW_LOG_INFO("Stopping Segregated_thread_task_loop [" << this << "]: All ongoing tasks will complete normally; "
                "all pending thread tasks will be belayed [sic]; "
                "each thread will be asked to gracefully terminate and be joined synchronously.  "
                "Any subsequently-queued tasks will not run until start().");

#ifndef NDEBUG
  size_t idx = 0;
#endif
  for (Task_qing_thread_ptr& thread_ptr_in_container : m_qing_threads)
  {
    /* Consider E = thread_ptr_in_container->task_engine().  The following statement has the following effects,
     * in order:
     *   - Any currently executing post()ed task on E completes normally (presumably quickly, if well-behaved).
     *     (There may well be no such task running at this time.  Thread might be idle.)
     *   - Any subsequent queued tasks on E are prevented from running, even if they otherwise would have run
     *     after the aforementioned final task (if any).  (They stay queued on )
     *   - E->run() therefore returns, so `*thread` exits.
     *   - `*thread_ptr_in_container`'s thread is joined, meaning the following blocks until that thread exits
     *     gracefully (i.e., all of the above completes, particularly E->run() returns).
     *   - `*thread_ptr_in_container` is destroyed (dtor runs).  This decrements ref-count of the E shared_ptr<>,
     *     but we have E in m_task_engines[] always, so it continues to live.
     *
     * *E continues to exist for now.  In particular, if a not-yet-stopped thread's
     * currently executing task (if any) tries to post(*E, F), that call will safely execute but do
     * nothing (F() will not run, because `E->stopped() == true` now).
     *
     * @warning Without m_task_engines storing the shared_ptr<> E also, the above would not be safe, because:
     *          A race could occur wherein Task_engine E1 is destroyed, while not-yet-stopped Task_engine E2 tries to
     *          post(*E2, F) (crash/undefined behavior).  In that case we'd need to first do
     *          a round of ->stop()s; and only then auto-destruct m_qing_threads[] and their underlying E's. */
    assert(thread_ptr_in_container->task_engine() == m_task_engines[idx]);
    thread_ptr_in_container.reset();
    assert(m_task_engines[idx]->stopped());
    // Any pending tasks on that Task_engine now await another thread to ->run().  Then the queue will resume.

#ifndef NDEBUG
    ++idx;
#endif
  } // for (thread_ptr_in_container : m_qing_threads)

  /* Now every thread in the thread pool has exited.  Therefore by definition no post()ed tasks on `*this` will
   * execute until start(), and none is executing now.  In particular, they won't try to, themselves, post() more
   * work onto `*this`, because "they" cannot actually run until start().
   *
   * As promised in our contract, however, user can still post() (and other async work) from outside the tread pool;
   * it will work, but (again) no task/completion handler will actually execute.  There are no threads left in which it
   * would.
   *
   * Don't nullify each m_task_engines[]; post() (and others) will thus (haplessly but safely) post tasks onto
   * m_task_engines[], as we promised in our contract. */

  /* Sanity check: stop() isn't thread-safe against stop(), as we warned in our contract.  But we've also promised
   * that other API calls *are* thread-safe, including against stop().  For brevity I leave that proof as an exercise
   * to reader.  That said see Cross_thread_task_loop::stop() similarly placed comment which justifies its
   * somewhat-more-complex situation.  Having proven that, the same logic applies here, except we don't even have the
   * complication of a shared Task_engine among threads.  Check the logic inside Task_qing_thread::stop(). */
} // Segregated_thread_task_loop::stop()

size_t Segregated_thread_task_loop::n_threads() const // Virtual.
{
  return m_qing_threads.size();
}

Op Segregated_thread_task_loop::create_op() // Virtual.
{
  return per_thread_ops().random_op(); // It makes a copy (which is just a Task_engine_ptr copy ultimately).
}

const Op_list& Segregated_thread_task_loop::per_thread_ops() // Virtual.
{
  return *m_per_thread_ops; // It's fine until destructor runs.
}

void Segregated_thread_task_loop::post(Task&& task, Synchronicity synchronicity) // Virtual.
{
  /* Each thread has its own single-run()-executing Task_engine, so it's left to us to choose a random one.
   * Theoretically this is less efficient than in cross-thread sibling which might choose a thread that is more
   * free to do work rather than just being entirely random about the decision.  @todo It's conceivable some
   * querying as to each thread's state or load could be used to make a more informed choice.  It might also be
   * wortwhile to make that choice here in post() instead of at create_op() time.  Class doc header as of this writing
   * includes these formal to-dos.  See also task_engine() where this to-do applies. */

  auto const chosen_thread_idx = per_thread_ops().random_idx();
  auto const chosen_task_engine = m_task_engines[chosen_thread_idx];
  // Could use random_op() for slightly shorter code, but the above is potentially a bit faster, bypassing boost.any.

  FLOW_LOG_TRACE("Segregated_thread_task_loop [" << this << "]: About to post single-task sequence "
                 "on randomly selected thread [" << chosen_thread_idx << "]: "
                 "Task_engine [" << chosen_task_engine << "].");

  post_impl(chosen_task_engine, synchronicity, std::move(task));
}

void Segregated_thread_task_loop::post(const Op& op, Task&& task, Synchronicity synchronicity) // Virtual.
{
  using util::Task_engine;

  /* Since both create_op() and per_thread_ops()[i] just return an Op containing a pointer
   * to one of n_threads() threads, we simply execute on that thread, thus guaranteeing all other post()ed
   * tasks by definition of "thread" cannot run concurrently with this `task`. */

  auto const chosen_task_engine = op_to_exec_ctx<Task_engine_ptr>(this, op);

  FLOW_LOG_TRACE("Segregated_thread_task_loop [" << this << "]: About to post a task in multi-task sequence "
                 "on previously chosen thread: "
                 "Task_engine [" << chosen_task_engine << "].");

  post_impl(chosen_task_engine, synchronicity, std::move(task));
}

util::Scheduled_task_handle Segregated_thread_task_loop::schedule_from_now(const Fine_duration& from_now,
                                                                           Scheduled_task&& task) // Virtual.
{
  // See 1-arg post().  Keeping comments light.

  auto const chosen_thread_idx = per_thread_ops().random_idx();
  auto const chosen_task_engine = m_task_engines[chosen_thread_idx];

  FLOW_LOG_TRACE("Segregated_thread_task_loop [" << this << "]: "
                 "About to boost.asio-timer-schedule single-task sequence "
                 "on randomly selected thread [" << chosen_thread_idx << "]: "
                 "Task_engine [" << chosen_task_engine << "].");

  return schedule_from_now_impl(chosen_task_engine, from_now, std::move(task));
}

util::Scheduled_task_handle Segregated_thread_task_loop::schedule_from_now(const Op& op,
                                                                           const Fine_duration& from_now,
                                                                           Scheduled_task&& task) // Virtual.
{
  using util::Task_engine;

  // See 2-arg post().  Keeping comments light.

  auto const chosen_task_engine = op_to_exec_ctx<Task_engine_ptr>(this, op);

  FLOW_LOG_TRACE("Segregated_thread_task_loop [" << this << "]: "
                 "About to boost.asio-timer-schedule a task in multi-task sequence on previously chosen thread: "
                 "Task_engine [" << chosen_task_engine << "].");

  return schedule_from_now_impl(chosen_task_engine, from_now, std::move(task));
}

util::Scheduled_task_handle Segregated_thread_task_loop::schedule_at(const Fine_time_pt& at,
                                                                     Scheduled_task&& task) // Virtual.
{
  // Similar comment to Cross_thread_task_loop::schedule_at().
  return schedule_from_now(at - Fine_clock::now(), std::move(task));
}

util::Scheduled_task_handle Segregated_thread_task_loop::schedule_at(const Op& op,
                                                                     const Fine_time_pt& at,
                                                                     Scheduled_task&& task) // Virtual.
{
  // Same comment as in other schedule_at().
  return schedule_from_now(op, at - Fine_clock::now(), std::move(task));
}

void Segregated_thread_task_loop::post_impl(const Task_engine_ptr& chosen_task_engine,
                                            Synchronicity synchronicity, Task&& task)
{
  using util::Task_engine;

  /* The "hard" part was choosing chosen_qing_thread; now we can post to that thread's segregated Task_engine.
   * Any details as to when to run it and whether/how long to wait for completion are forwarded to
   * the following per contract. */

  asio_exec_ctx_post<Task_engine>
    (get_logger(), chosen_task_engine.get(), synchronicity, std::move(task));
}

util::Scheduled_task_handle
  Segregated_thread_task_loop::schedule_from_now_impl(const Task_engine_ptr& chosen_task_engine,
                                                      const Fine_duration& from_now,
                                                      Scheduled_task&& task)
{
  using util::schedule_task_from_now;

  // See post_impl().  Keeping comments light.

  auto const logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    Scheduled_task task_plus_logs = [this, task = std::move(task)]
                                    (bool short_fired)
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Scheduled task starting: "
                                      "current processor logical core index [" << cpu_idx() << "].");
      task(short_fired);
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Scheduled task ended: "
                                      "current processor logical core index [" << cpu_idx() << "].");
    };

    return schedule_task_from_now(get_logger(), from_now, n_threads() == 1,
                                  chosen_task_engine.get(), std::move(task_plus_logs));
  }
  // else

  return schedule_task_from_now(get_logger(), from_now, n_threads() == 1,
                                chosen_task_engine.get(), std::move(task));
}

Task_engine_ptr Segregated_thread_task_loop::task_engine() // Virtual.
{
  // See 1-arg post().  Keeping comments light.  Note the @todo about load-balancing applies here as well.

  auto const chosen_thread_idx = per_thread_ops().random_idx();
  auto const chosen_task_engine = m_task_engines[chosen_thread_idx];

  FLOW_LOG_TRACE("Segregated_thread_task_loop [" << this << "]: About to return boost.asio Task_engine "
                 "assigned to randomly selected thread [" << chosen_thread_idx << "]: "
                 "Task_engine [" << chosen_task_engine << "].");

  return chosen_task_engine;
}

template<>
Task_engine_ptr op_to_exec_ctx<Task_engine_ptr>([[maybe_unused]] Concurrent_task_loop* loop, const Op& op)
{
  using boost::any_cast;

  assert(op.type() == typeid(Task_engine_ptr));
  // @todo It'd be also nice to assert(<this Task_engine_ptr is one of m_task_engines[]>).

  return any_cast<Task_engine_ptr>(op);
}

} // namespace flow::async
