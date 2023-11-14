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

namespace flow::async
{

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Execution_context>
void asio_exec_ctx_post(log::Logger* logger_ptr, Execution_context* exec_ctx, Synchronicity synchronicity, Task&& task)
{
  using log::Sev;
  using boost::asio::post;
  using boost::asio::dispatch;
  using boost::promise;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_ASYNC);

  /* As noted in contract, Execution_context is either Task_engine or Strand.  Since a Strand is born of a Task_engine,
   * everything that would apply to its parent Task_engine applies to the Strand, so we can talk about exclusively
   * Task_engines in below comments to explain the reasoning.  Strand only adds an extra constraint, namely that
   * the chosen thread is not executing something from the same Strand at the time that F() eventually runs.  This is
   * typically irrelevant to the below logic; when it is relevant we will explicitly discuss the possibility that
   * *exec_ctx is a Strand after all.
   *
   * Lastly, as the contract notes, this all quite likely works with the general ExecutionContext formal concept
   * from boost.asio docs; but we have made no formal effort to ensure this in our comments/reasoning.  This may be
   * corrected as needed in the future.
   *
   * Let us now discuss the asio_exec_ctx_post(E, synchronicity, F) call, where E is a Task_engine.
   *
   * We post to some random-ish thread in E, which is run()ning in at least 1 thread, using built-in Task_engine
   * (boost.asio) functionality wherein: post(E, F) will execute F() within any thread currently executing E.run(),
   * for a given Task_engine E.
   *
   * post(E, F) is indeed used in the most common synchronicity mode, S_ASYNC, and in its
   * close cousin S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION.  If calling thread is outside pool E, then
   * F() will run sometime in the future concurrently to the calling thread; including to our code in this method.
   * If calling thread is in E's pool (we're in in a task being called by E.run() now), then F()
   * will run sometime after we return.
   *
   * However, S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION will additionally cause us to await F()'s completion before
   * we return control to caller.  If we're inside pool E, this might cause a deadlock (and *definitely* will
   * if n_threads() == 1), because we'll wait to return 'til a condition that can only become true after we return
   * (unless by luck the post() targets a different thread in the pool).  Hence we advertise undefined behavior if the
   * user makes the improper choice to use this mode from inside an E-posted task.
   *
   * [Sanity check: What if E is a Strand after all (from some Task_engine E*), and F() is currently executing from
   * within E*'s pool after all... but also E* happens to have itself been posted through some *other* Strand
   * E#?  Would that preclude the deadlock situation, since seemingly being in different Strands would prevent
   * boost.asio from posting F() onto W (current thread)?  Answer: No; just because it can't put it on the current
   * thread *now* doesn't mean it can't queue it to run on W after we return... and we're back to deadlock.  So,
   * the contract stands without any further allowances due to E's Strandiness.]
   *
   * S_ASYNC_AND_AWAIT_CONCURRENT_START is similar to ..._COMPLETION; but it returns "early": just before F()
   * executes (in such a way as to guarantee E.stop(), when E is a Task_engine, executed after we return will be unable
   * to prevent F() from running).  The same deadlock situation applies: we can't wait for F() to start, if it can only
   * start once we're done waiting.
   *
   * Lastly: S_OPPORTUNISTIC_SYNC_ELSE_ASYNC is much like S_ASYNC, except dispatch(E, F) is used instead of post(E, F).
   * If post(E, F) would have caused F() to execute in the current thread anyway as soon as we return, then F() is
   * executed synchronously, now, and *then* we will return.  (This is a performance shortcut, so that boost.asio
   * won't pointlessly exit us and immediately re-enter F().)  If, however, post(E, F) would have caused F() to execute
   * on some other thread and/or later, then dispatch(E, F) acts identically to post(E, F) after all, meaning it'll
   * execute in the background concurrently and/or later.  (In other words, the performance shortcut is not possible
   * due to other constraints.) */

  // @todo Maybe provide operator<<(ostream, Task_engine/Strand) so we can output *exec_ctx too.
  FLOW_LOG_TRACE("About to post task via boost.asio execution context (Task_engine or Strand as of this writing) "
                 "[@" << exec_ctx << "]; might sync-execute in this thread? = "
                 "[" << (synchronicity == Synchronicity::S_OPPORTUNISTIC_SYNC_ELSE_ASYNC) << "]; "
                 "will we ensure concurrent completion before continuing? = "
                 "[" << (synchronicity == Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION) << "]; "
                 "will we ensure concurrent initiation before continuing? = "
                 "[" << (synchronicity == Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START) << "].");

  Task actual_task(std::move(task));

  /* If the current log level suggests we want TRACE logging then book-end their task with some log statements.
   * For perf: entirely avoid this wrapping if almost certainly no logging would occur anyway. */
  if (logger_ptr && logger_ptr->should_log(Sev::S_TRACE, get_log_component()))
  {
    actual_task = [get_logger, get_log_component, wrapped_task = std::move(actual_task)]()
    {
      FLOW_LOG_TRACE("Task starting: current processor logical core index [" << cpu_idx() << "].");
      wrapped_task();
      FLOW_LOG_TRACE("Task ended: current processor logical core index [" << cpu_idx() << "].");
    };
  }

  /* Now we want to (1) post `actual_task` onto *m_shared_task_engine, so that it runs at some point; and (2)
   * either wait or not wait for it to complete concurrently.  The details are discussed above and
   * in Synchronicity member doc headers. */

  if (synchronicity == Synchronicity::S_ASYNC)
  {
    post(*exec_ctx, std::move(actual_task));
    // That's it; give control back to caller.  actual_task() has not run here (but may run concurrently).
  }
  else if (synchronicity == Synchronicity::S_OPPORTUNISTIC_SYNC_ELSE_ASYNC)
  {
    dispatch(*exec_ctx, std::move(actual_task));
    // That's it; give control back to caller.  actual_task() may have just run -- or else it's just like post().
  }
  else
  {
    const bool assure_finish_else_start = synchronicity == Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION;
    assert((assure_finish_else_start || (synchronicity == Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START))
           && "asio_exec_ctx_post() failed to handle one of the Synchronicity enum values.");

    /* In this interesting mode, by contract, we are *not* in W now.  (@todo Consider finding a way to assert() this.)
     * As noted above we still post(E, F) -- but instead of returning await its completion, or initiation; then return.
     * A future/promise pair accomplishes the latter easily.  (Historical note: I (ygoldfel) used to do the
     * future/promise thing manually in many places.  It's boiler-plate, short but a little hairy, so it's nice to
     * give them this feature instead.  Also some might not know about promise/future and try to use
     * mutex/cond_var, which is always a nightmare of potential bugs.) */

    FLOW_LOG_TRACE("From outside thread pool, we post task to execute concurrently; and await its "
                   "[" << (assure_finish_else_start ? "completion" : "initiation") << "].  "
                   "A deadlock here implies bug, namely that we are *not* outside thread pool now after all.");

    promise<void> wait_done_promise;

    if (assure_finish_else_start)
    {
      post(*exec_ctx, [&]() // We're gonna block until the task completes, so just keep all context by &reference.
      {
        actual_task();
        wait_done_promise.set_value();
      });
    }
    else
    {
      post(*exec_ctx, [&wait_done_promise, // Safe to capture: .get_future().wait() returns after .set_value().
                       actual_task = std::move(actual_task)] // Gotta cap it; else `actual_task` hosed while executing.
                        ()
      {
        wait_done_promise.set_value(); // We've started!  Nothing (other than a thread killing) can prevent:
        actual_task();
      });
    }

    wait_done_promise.get_future().wait();
    FLOW_LOG_TRACE("Wait for concurrent execution complete.  Done.");

    /* That's it; give control back to caller.  actual_task() *did* run and finish but not in here; or at least
     * it will have definitely finished eventually, even if caller tries E.stop() (if applicable) after we return. */
  } // if (sync == S_ASYNC_AND_AWAIT_CONCURRENT_{COMPLETION|START})
} // asio_exec_ctx_post()

} // namespace flow::async
