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
#include "flow/async/x_thread_task_loop.hpp"
#include "flow/async/detail/task_qing_thread.hpp"
#include "flow/async/util.hpp"
#include "flow/util/sched_task.hpp"

namespace flow::async
{

// Implementations.

Cross_thread_task_loop::Cross_thread_task_loop(log::Logger* logger_ptr, util::String_view nickname,
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

  /* Note to reader: If you grok the doc headers of superclass and Cross_thread_task_loop, the following
   * is easy to understand.  If you don't, then to make it clear I'd have to repeat all of that here, basically.
   * So please grok it first. */

  m_qing_threads((m_n_threads_or_zero == 0) // n_threads() nulls: still need to set each one after this.
                   ? optimal_worker_thread_count_per_pool(logger_ptr, m_est_hw_core_sharing_helps_algo)
                   : m_n_threads_or_zero),
  /* n_threads() is now accurate.  Create the shared Task_engine capable of smartly scheduling across N threads.
   * Attn: Give concurrency hint; 1 in particular may help avoid or eliminate locking inside boost.asio. */
  m_shared_task_engine(new util::Task_engine{static_cast<int>(n_threads())}),
  // Forever initialize our Ops_list of pre-created Ops which in our case simply store long-lived Strands.
  m_per_thread_strands(logger_ptr, n_threads(),
                       [this](size_t) -> Op
                         { return Op{Strand_ptr{new util::Strand{*m_shared_task_engine}}}; })
{
  /* Task_engine starts in !stopped() mode ready to run().  start() pre-condition is stopped() so for simplicity
   * start in the same state that our stop() would put the Task_engine into: */
  m_shared_task_engine->stop();
  // Now our start() can always do the sequence: restart() (to make it !stopped()), then run().

  FLOW_LOG_INFO("Cross_thread_task_loop [" << static_cast<const void*>(this) << "] "
                "with nickname [" << m_nickname << "] "
                "with cross-thread scheduling via single Task_engine across [" << n_threads() << "] threads-to-be: "
                "Created; can accept work.  Task_qing_thread(s) not started yet until start().");

  /* *m_shared_task_engine can now be post()ed onto and otherwise used with boost.asio; won't do anything until we
   * start threads and run() it.  start() does that.  We're done. */
}

Cross_thread_task_loop::~Cross_thread_task_loop() // Virtual.
{
  FLOW_LOG_INFO("Cross_thread_task_loop [" << this << "]: Destroying object; will stop threads/tasks unless already "
                "stopped earlier.");
  stop();

  /* m_qing_threads.clear() equivalent and m_shared_task_engine->~Task_engine() will now happen automatically.
   * Both are mere cleanup of stuff in memory. */
}

void Cross_thread_task_loop::start(Task&& init_task_or_empty,
                                   const Thread_init_func& thread_init_func_or_empty) // Virtual.
{
  using util::Thread;
  using boost::promise;
  using boost::unique_future;
  using boost::movelib::unique_ptr;
  using std::transform;
  using std::vector;

  /* Is the check thread-safe?  Yes, since Concurrent_task_loop::stop() must not be called concurrently with itself or
   * start() per our contract in doc header. */
  if (!m_shared_task_engine->stopped())
  {
    /* Unlike stop() after stop(), start() after start() doesn't seem all that benign (whereas double stop() will often
     * happen from dtor, say).  So use WARNING, not INFO. */
    FLOW_LOG_WARNING("Starting Cross_thread_task_loop [" << this << "]: Already started earlier.  Ignoring.");
    return;
  }
  // else

  const size_t n = n_threads();
  FLOW_LOG_INFO("Cross_thread_task_loop [" << this << "] with nickname [" << m_nickname << "]: "
                "Single Task_engine across [" << n << "] Task_qing_threads: Starting.");

  // boost.asio subtlety: While stopped(), run() will instantly return, unless one does this first.
  m_shared_task_engine->restart();
  // Now each Task_qing_thread can do ->run() as most of its thread body (and it won't just return).

  /* Create/start the threads.
   * Subtlety: If thread_init_func_or_empty is null, then we know Task_qing_thread ctor to put very quick-executing
   * stuff onto the new thread, before it's officially ready to participate in post()ing.  So it would be fine
   * to just wait for each ctor to finish in series (in fact that's what we used to do).  Otherwise, though,
   * thread_init_func_or_empty() could be something lengthy... it's best to let the ctors launch the thread-init
   * steps to happen asynchronously, each ctor returning potentially before its thread's init steps are finished.
   * So that's why we use the Task_qing_thread ctor mode wherein we pass in our own `promise`s and then wait afterwards
   * for them all to be satisfied, barrier-style. */

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

    m_qing_threads[idx].reset(new Task_qing_thread{get_logger(),
                                                   (n == 1) ? m_nickname : util::ostream_op_string(m_nickname, idx),
                                                   m_shared_task_engine, false, // A *shared* Task_engine.
                                                   &(thread_init_done_promises[idx]),
                                                   std::move(task_qing_thread_init_func)});
  } // for (idx in [0, n))

  // By barrier-style I mean that they all the waits must be done, before the loop exits.
  FLOW_LOG_INFO("All threads are asynchronously starting.  Awaiting their readiness barrier-style, in sequence.");
  for (size_t idx = 0; idx != n; ++idx)
  {
    thread_init_done_promises[idx].get_future().wait();
    FLOW_LOG_INFO("Thread [" << idx << "] (0-based) of [" << n << "] (1-based) is ready.");
  }

  /* Threads are running and ready for work.  Note that if `m_n_threads_or_zero == 0`, then that enabled the
   * auto-config of the thread arch based on the assumption of a processor/RAM-heavy thread pool that doesn't
   * benefit from hyper-threading.  This has already then had an effect by auto-determining `n`.
   * Now we complete the functionality in this auto-config mode by performing whatever thread-core pinning is
   * implied by the 3 constructor bool arguments, the use of which was enabled by `m_n_threads_or_zero == 0`.
   *
   * There is a sliiightly annoying corner case, which is that: not only are the threads *ready* for work, but
   * they might already be executing work right now, namely if stuff has been post()ed (or other types of work
   * posting) onto *m_shared_task_engine already.  Yet we haven't yet done the pinning stuff!  That's fine
   * though.  They've only been running for, like, microseconds; and it is certainly allowed to mess with their
   * core affinities, even when they aren't idle.  @todo It's technically possible to get rid of this minor
   * sort-of-dirtiness... have Task_qing_thread wait for some signal (probably a promise/future) to actually
   * kick off Task_engine::run()... that complexity does not seem in any way worth it at this time.  Revisit maybe. */
  if (m_n_threads_or_zero == 0)
  {
    FLOW_LOG_INFO("Thread count was auto-determined.  Further attempting thread-to-core scheduling optimization.");

    vector<Thread*> worker_threads(n); // Initialized to nulls.  Now set them to the raw `Thread*`s.
    transform(m_qing_threads.begin(), m_qing_threads.end(), worker_threads.begin(),
              [](const Task_qing_thread_ptr& qing_thread_ptr) -> Thread*
                { return qing_thread_ptr->raw_worker_thread(); });

    optimize_pinning_in_thread_pool(get_logger(), worker_threads,
                                    m_est_hw_core_sharing_helps_algo, m_est_hw_core_pinning_helps_algo,
                                    m_hw_threads_is_grouping_collated);
    // That logged details.
  }

  /* That's it; ready for work/working... but help out by running this optional init task: the key is this will wait
   * until that task completes in a spawned thread.  So we return only once that has returned.
   * Do note that anything else queued while `*this` was stopped may well run before this guy (if any). */
  if (!init_task_or_empty.empty())
  {
    post(std::move(init_task_or_empty), Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
  }

  /* Sanity check: start() isn't thread-safe against stop() and start(), as we warned in our contract.
   * But we've also promised that other API calls *are* thread-safe, including against
   * stop().  To mentally test that <see similar comment in stop()>.  TL;DR: post() and similar APIs work via
   * *m_shared_task_engine safely, even as underlying thread(s) start or stop and m_shared_task_engine->run(). */
} // Cross_thread_task_loop::start()

void Cross_thread_task_loop::stop() // Virtual.
{
  /* This is probably unnecessary, since as of this writing all below statements are no-ops if stop() was called before.
   * We still do it because:
   *   - It's cleaner: one less code path to have to reason about in terms of safety.
   *   - Less spurious/verbose logging (note Task_qing_thread would log in both its .stop() and dtor).
   *   - It's a tiny bit faster.
   *
   * Is it thread-safe?  Yes, since Concurrent_task_loop::stop() must not be called concurrently with itself per our
   * contract in doc header. */
  if (m_shared_task_engine->stopped())
  {
    FLOW_LOG_INFO("Stopping Cross_thread_task_loop [" << this << "]: Already stopped earlier.  Ignoring.");
    return;
  }
  // else

  FLOW_LOG_INFO("Cross_thread_task_loop [" << this << "] with nickname [" << m_nickname << "]: Stopping: "
                "All ongoing thread tasks will complete normally; all pending thread tasks will be belayed [sic]; "
                "each thread will be asked to gracefully terminate and be joined synchronously.  "
                "Any subsequently-queued tasks will not run until start().");

  /* Kick off all the thread exits in parallel.  To see how the following statement accomplishes this,
   * consider what happens when Task_engine::stop() does its thing.  boost.asio docs show
   * that it lets any ongoing task(s) to complete normally/return.  (That is even the case if it, and we, are called
   * from within a task in the thread pool itself.)  They further show any *more* tasks are prevented from running
   * even if already queued up.  Lastly, Task_engine::run() will exit, in any thread that invoked it.
   *
   * In our case, each thread's body is Task_engine::run(), meaning once that returns, the thread ends gracefully.
   * So that's how the following kicks off all the thread exits. */
  m_shared_task_engine->stop();
  assert(m_shared_task_engine->stopped());
  // Any pending tasks on *m_shared_task_engine now await another thread to ->run().  Then the queue will resume.

  /* That has almost satisfied the contract in our doc header.  All that's left is to return only once the thread-exit
   * we've kicked off fully completes, a/k/a we must join those threads.  Do so: */
  for (Task_qing_thread_ptr& thread_ptr_in_container : m_qing_threads)
  {
    /* By its contract, this will simply wait for the threads to exit (and it's up to us to kick that off, which we
     * did above). */
    thread_ptr_in_container->stop();

    /* Delete the Task_qing_thread.  Sanity check: Is it safe?  Yes, because in our case, nothing ever touches the
     * thread objects m_qing_threads[] themselves after start() -- all posting/etc. is done indirectly through
     * m_shared_task_engine, shared among all the threads; and that will keep working (but no-op'ing until the next
     * start() if any).
     *
     * Lastly, why not m_qing_threads.clear() instead, which is equivalent to .reset() + clearing the container of
     * pointers too, thus freeing a bit more RAM?  Answer: n_threads() *does* access m_qing_threads.size() after stop();
     * plus start() would assume the vector has the elements, albeit nulls; clear() would break that.
     * The RAM savings via clear() are minor, so we haven't bothered making it more complicated. */
    thread_ptr_in_container.reset();
  }
  // All threads joined as promised.

  /* Sanity check: stop() isn't thread-safe against stop() and start(), as we warned in our contract.
   * But we've also promised that other API calls *are* thread-safe, including against
   * stop().  Let's mentally test that by analyzing the
   * above statements against a hypothetical concurrent boost::asio::post(m_shared_task_engine, F) (and all our
   * task-posting APIs reduce to boost::asio::post() ultimately).
   *   - Task_engine::stop() is thread-safe against boost::asio::post(<same Task_engine>, F) by boost.asio docs.
   *   - Task_qing_thread::stop() merely joins the thread (waits for Task_engine::run() to return as kicked off
   *     in previous bullet point).  No prob: It's equally fine to boost::asio::post(E, F) before and after
   *     E.run() returns, as well as from within tasks invoked by E.run(), by boost.asio docs (and common sense).
   *   - ~Task_qing_thread() is safe, as m_qing_threads[] is never accessed at all after start() returns (outside of
   *     stop()), as discussed above.
   *
   * Mentally testing our thread-safety against other, non-posting, APIs is left as an exercise to the reader. */
} // Cross_thread_task_loop::stop()

size_t Cross_thread_task_loop::n_threads() const // Virtual.
{
  return m_qing_threads.size();
}

Op Cross_thread_task_loop::create_op() // Virtual.
{
  return Op{Strand_ptr{new util::Strand{*m_shared_task_engine}}};
}

const Op_list& Cross_thread_task_loop::per_thread_ops() // Virtual.
{
  return m_per_thread_strands;
}

void Cross_thread_task_loop::post(Task&& task, Synchronicity synchronicity) // Virtual.
{
  using util::Task_engine;

  FLOW_LOG_TRACE("Cross_thread_task_loop [" << this << "]: "
                 "About to post single-task sequence via boost.asio scheduler; details TRACE-logged below.");

  /* No constraint vs. any other task: simply post to some random-ish thread in our pool-spanning shared Task_engine.
   * Any details as to when to run it and whether/how long to wait for completion are forwarded to
   * the following per contract. */

  asio_exec_ctx_post<Task_engine>
    (get_logger(), m_shared_task_engine.get(), synchronicity, std::move(task));
} // Cross_thread_task_loop::post()

void Cross_thread_task_loop::post(const Op& op, Task&& task, Synchronicity synchronicity) // Virtual.
{
  using util::Strand;

  if (n_threads() == 1)
  {
    // Non-concurrency is always guaranteed with 1 thread.  An optimization (though boost.asio likely does similar).
    post(std::move(task));
    return;
  }
  // else

  auto const chosen_strand_ptr = op_to_exec_ctx<Strand_ptr>(this, op);
  FLOW_LOG_TRACE("Cross_thread_task_loop [" << this << "]: "
                 "About to post a task in multi-task sequence "
                 "via previously created strand; details TRACE-logged below.");

  /* This is much like the other post() but with the extra constraint: must not run concurrently with any other Task
   * post()ed to the Strand pointed to by `op`'s payload.  That is simply the core functionality of a Strand.
   *
   * Check out asio_exec_ctx_post() doc header, but TL;DR spoiler alert: It works equally well, because Strand has
   * the same needed post() and dispatch() abilities as the Task_engine from which it is born. */

  asio_exec_ctx_post<Strand>
    (get_logger(), chosen_strand_ptr.get(), synchronicity, std::move(task));
} // Cross_thread_task_loop::post()

util::Scheduled_task_handle Cross_thread_task_loop::schedule_from_now(const Fine_duration& from_now,
                                                                      Scheduled_task&& task) // Virtual.
{
  using util::schedule_task_from_now;

  // See 1-arg post().  Keeping comments light.

  auto const logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    FLOW_LOG_TRACE_WITHOUT_CHECKING("Cross_thread_task_loop [" << this << "]: "
                                    "About to schedule single-task sequence via boost.asio timer+scheduler.");

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
                                  m_shared_task_engine.get(), std::move(task_plus_logs));
  }
  // else

  return schedule_task_from_now(get_logger(), from_now, n_threads() == 1,
                                m_shared_task_engine.get(), std::move(task));
}

util::Scheduled_task_handle Cross_thread_task_loop::schedule_from_now(const Op& op,
                                                                      const Fine_duration& from_now,
                                                                      Scheduled_task&& task) // Virtual.
{
  using util::Strand;
  using util::schedule_task_from_now;
  using boost::asio::bind_executor;

  // See 2-arg post().  Keeping comments light.

  if (n_threads() == 1)
  {
    return schedule_from_now(from_now, std::move(task));
  }
  // else

  assert(op.type() == typeid(Strand_ptr));
  auto const chosen_strand_ptr = op_to_exec_ctx<Strand_ptr>(this, op);

  auto const logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    FLOW_LOG_TRACE_WITHOUT_CHECKING("Cross_thread_task_loop [" << this << "]: "
                                    "About to schedule single-task sequence via boost.asio timer+scheduler "
                                    "via previously created strand [" << chosen_strand_ptr << "].");

    Scheduled_task task_plus_logs = [this, task = std::move(task)]
                                    (bool short_fired)
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Scheduled task starting: "
                                      "current processor logical core index [" << cpu_idx() << "].");
      task(short_fired);
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Scheduled task ended: "
                                      "current processor logical core index [" << cpu_idx() << "].");
    };

    return schedule_task_from_now(get_logger(), from_now, n_threads() == 1, m_shared_task_engine.get(),
                                  bind_executor(*chosen_strand_ptr, task_plus_logs)); // <-- ATTN!  Go through Strand.
  }
  // else

  return schedule_task_from_now(get_logger(), from_now, n_threads() == 1, m_shared_task_engine.get(),
                                bind_executor(*chosen_strand_ptr, task)); // <-- ATTN!  Go through Strand.
}

util::Scheduled_task_handle Cross_thread_task_loop::schedule_at(const Fine_time_pt& at,
                                                                Scheduled_task&& task) // Virtual.
{
  /* This is straightforward, clearly, but note that -- moreover -- there is no perf downside to doing this
   * instead of copy-pasting schedule_from_now()'s body while using util::schedule_task_at() directly.  That's
   * according to the `### Performance ###` note in schedule_task_at()'s doc header.  So there's no perf penalty for
   * keeping this code trivial. */
  return schedule_from_now(at - Fine_clock::now(), std::move(task));
}

util::Scheduled_task_handle Cross_thread_task_loop::schedule_at(const Op& op,
                                                                const Fine_time_pt& at,
                                                                Scheduled_task&& task) // Virtual.
{
  // Same comment as in other schedule_at().
  return schedule_from_now(op, at - Fine_clock::now(), std::move(task));
}

Task_engine_ptr Cross_thread_task_loop::task_engine() // Virtual.
{
  return m_shared_task_engine;
}

template<>
Strand_ptr op_to_exec_ctx<Strand_ptr>([[maybe_unused]] Concurrent_task_loop* loop, const Op& op)
{
  using boost::any_cast;

  assert(op.type() == typeid(Strand_ptr));
  auto const strand_ptr = any_cast<Strand_ptr>(op);
  assert(&(strand_ptr->context()) == loop->task_engine().get());

  return strand_ptr;
}

} // namespace flow::async
