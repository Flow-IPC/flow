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
#include "flow/async/detail/task_qing_thread.hpp"
#include "flow/log/config.hpp"
#include <boost/asio.hpp>
#include <boost/move/make_unique.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <string>
#include <exception>
#include <cstdlib>

namespace flow::async
{

// Static initializations.

const int Task_qing_thread::S_BAD_EXIT = 1;

// Implementations.

Task_qing_thread::Task_qing_thread(flow::log::Logger* logger_ptr, util::String_view nickname_view,
                                   const Task_engine_ptr& task_engine_ptr, bool own_task_engine,
                                   boost::promise<void>* done_promise_else_block,
                                   Task&& init_func_or_empty) :
  flow::log::Log_context(logger_ptr, Flow_log_component::S_ASYNC),
  m_task_engine(task_engine_ptr), // shared_ptr<> copy.
  m_own_task_engine(own_task_engine)
{
  using boost::promise;
  using boost::asio::post;
  using boost::asio::make_work_guard;
  using boost::movelib::unique_ptr;
  using boost::movelib::make_unique;
  using std::exception;
  using std::string;
  using std::exit;
  using util::Thread;
  using util::Task_engine;
  using log::Logger;
  using log::beautify_chrono_logger_this_thread;
  using Task_engine_work = boost::asio::executor_work_guard<Task_engine::executor_type>;
  using Log_config = log::Config;

  assert(m_task_engine);
  string nickname(nickname_view); // We need an std::string below anyway, so copy this now.

  // Some programs start tons of threads.  Let's be stingy with INFO messages.

  FLOW_LOG_INFO("Task_qing_thread [" << static_cast<const void*>(this) << "] with nickname [" << nickname << "] "
                "will " << (m_own_task_engine ? "have own Task_engine, probably with concurrency-hint=1"
                                              : "share Task_engine") << ": [@" << m_task_engine << "].");

  /* We'll satisfy this `promise` inside the thread we are about to spawn -- as soon as it is up and run()ing which
   * means post()s (etc.) will actually execute from that point on instead of continuing to get queued up.
   * Then in the current thread we'll briefly await for the `promise` to be satisfied, then return from this ctor;
   * or if they've passed in their own done_promise (done_promise_else_block not null), then they'll need to
   * do the satisfaction-awaiting themselves.
   *
   * The trial-balloon post()ed task is isn't necessary; post() is formally known to work
   * across run()ning threads starting and stopping; even with
   * other tasks pre-queued on the Task_engine (which our doc header contract explicitly says is allowed).  It's
   * a nice trial balloon, and if there's something wrong, it'll block the constructor and make it really obvious;
   * this has proven to be helpful for debugging/testing in my experience (ygoldfel).
   *
   * Update: done_promise has now been extended by a new API arg init_func_or_empty, to indicate in particular
   * that we've executed init_func_or_empty().  So, while waiting for the subsequent post()ed trial-balloon task
   * to complete is arguably at best a nicety, nevertheless the fact that can only occur *after* init_func_or_empty()
   * returns satisfies a *necessity* (to wit: that we don't satisfy done_promise until init_func_or_empty() returns). */
  unique_ptr<promise<void>> our_done_promise_or_null;
  promise<void>& done_promise
    = done_promise_else_block ? *done_promise_else_block
                              : *(our_done_promise_or_null = make_unique<promise<void>>());

  /* As promised in our doc header apply any current verbosity override to the new thread.  After all, there's no
   * opportunity for them to set the override, since there *is* no thread until we start it just now.
   * For now memorize it; then immediately apply it in the young thread.  (If there is no override, that's just
   * `sev_override == Sev::S_END_SENTINEL`; we need not even track it as a special case.) */
  const auto sev_override = *(Log_config::this_thread_verbosity_override());

  m_worker_thread.reset(new Thread([this, // Valid throughout thread { body }.
                                    sev_override,
                                    nickname = std::move(nickname), // Valid throughout thread { body }.
                                    init_func_or_empty = std::move(init_func_or_empty),
                                    &done_promise]() // Valid until post({ body }) .set_value()s it.
  {
    auto const logger_ptr = get_logger();

    // Before anything (at all) can log, begin the section where logging is limited to sev_override.
    {
      const auto sev_override_auto = Log_config::this_thread_verbosity_override_auto(sev_override);

      // Now do the pre-loop work.

      Logger::this_thread_set_logged_nickname(nickname, logger_ptr); // This INFO-logs a nice message.

      // Use standard beautified formatting for chrono durations/etc. output (and conversely input).
      beautify_chrono_logger_this_thread(get_logger());

      FLOW_LOG_TRACE("Worker thread starting.");

      if (!init_func_or_empty.empty())
      {
        FLOW_LOG_TRACE("Thread initializer callback executing synchronously.");
        init_func_or_empty();
        // This is INFO, as it may be important to separate between post()ed tasks and the initial setup.
        FLOW_LOG_INFO("Thread initializer callback execution finished.  Continuing event loop setup in this thread.");
      }

      // Pre-queue this before run().
      post(*m_task_engine, [this,
                            &done_promise]() // done_promise is (by promise/future's nature) alive until .set_value().
      {
        if (m_own_task_engine)
        {
          FLOW_LOG_TRACE("First task from Task_qing_thread [" << this << "] executing "
                         "in that thread since not sharing Task_engine object with more threads.");
        }
        else
        {
          FLOW_LOG_TRACE("First task from Task_qing_thread [" << this << "] executing.  "
                         "May or may not be in that thread since sharing Task_engine object with more threads.");
        }

        // Safe to say stuff can be post()ed, possibly to us, as we were post()ed and are executing indeed.
        done_promise.set_value();
        FLOW_LOG_TRACE("First task complete.");
      });
      /* If not sharing Task_engine, above will run soon from this new thread, from within the following ->run().
       * Otherwise it might run from any thread in pool of threads sharing Task_engine; possibly right now concurrently.
       * Even then, though, subsequent post()s (etc.) might land into this new thread as soon as the following call
       * starts running in earnest. */
    } // const auto sev_override_auto = // Restore logging to normal (how it normally is at thread start).

    // Avoid loop, thread exiting when no pending tasks remain.
    Task_engine_work avoid_task_engine_stop(make_work_guard(*m_task_engine));

    // Block -- wait for tasks to be posted on this thread's (possibly shared with other threads) Task_engine.
    m_task_engine->run();

    // Lastly, after everything -- as promised -- re-apply the same Sev override as during startup, for any last msgs.
    {
      const auto sev_override_auto = Log_config::this_thread_verbosity_override_auto(sev_override);

      // Now do the post-loop work.

      /* m_task_engine->run() must have exited to have reached here.  The only reason this can happen, assuming no
       * misuse of thread API(s?) by the user, is if stop() (or dtor which runs stop()) ran.  Or it threw exception. */
      FLOW_LOG_INFO("Event loop finished: Task_qing_thread [" << this << "] presumably being stopped!  "
                    "Thread exit imminent."); // This is good to keep as INFO.
    } // const auto sev_override_auto =

    /* Let's discuss handling of various terrible things that can occur inside ->run() which is essentially the thread
     * body, even though boost.asio is deciding when to run the various pieces of code in that thread body.
     *
     * Firstly conditions like SEGV (seg fault -- bad ptr dereference, etc.), FPE (division by zero, etc.),
     * BUS (like seg fault but with code), etc., may occur and raise a signal via system functionality.  That is none
     * of our concern; to the extent that we could trap such things and do something smart it would apply to threads
     * not handled by us; so it's not within our purview.
     *
     * Secondly an assert() could fail (or, equivalently but much less likely, [std]::abort() could be called directly).
     * abort() usually raises SIGABRT and, as such, anything we could do about this -- as in the preceding paragraph --
     * would apply to all threads; so again not in our purview.
     *
     * Lastly an exception could be thrown.  If caught (within a given task loaded onto *m_task_engine) then that's no
     * different from handling an error return of a C-style function and wouldn't reach us and is handled within the
     * task; none of our concern or anyone's concern; it's handled.
     *
     * That leaves one last condition: An exception was thrown within a task loaded onto *m_task_engine and *not*
     * caught within that task.  boost.asio shall *not* catch it; hence ->run() above will be emitting an exception.
     * We can do one of 2 things.  We can catch it; or not.  If we do catch it there's no way to continue the ->run()
     * in any fashion that would lead to anything but undefined behavior.  We could ->run() again but what happens then
     * is utterly unpredictable; it's trying to pretend an uncaught exception isn't exceptional.  The next least
     * exceptional behavior for us would be to log something about the exception and then exit the thread the same
     * was we do above when ->run() returns gracefully (meaning event loop finished gracefully).  However, then the
     * rest of the program would think maybe the thread exited gracefully -- but it didn't -- which again is pretending
     * an uncaught exception isn't exceptional.  (@todo It's conceivable to make this an optional behavior specified
     * by the *this creator.  However even then it would absolutely not be a correct default behavior; so we must
     * in this discussion decide what *is* correct *default* behavior.)
     *
     * We've now whittled down to these possible courses of action in the uncaught-exception-in-a-task scenario.
     *   - Catch it here; then:
     *     -1- Inform the rest of the program in some reasonable way and let them deal with it; e.g., execute some
     *         callback function passed to this ctor, wherein they can do what they want before the thread exits.  Or:
     *     -2- Exit the entire program.
     *   -3- Do not catch it here; let it fall through.
     *
     * Let's for now not do -1-; not because it's a bad idea (it's a decent idea optionally), but because it doesn't
     * provide a good default behavior that will do the right thing without the user having to worry about it.
     *
     * -2- can be done, basically, either via exit(<non-zero>) or via abort().  exit(<non-zero>) is not good as a
     * default behavior: Printing the exception's .what() would be decent, but there's no guarantee this would be
     * flushed to any actual log (e.g., log file) due to the sudden exit() not through main().  In general this wrests
     * control of the exit() code from the program writer and arbitrarily gives it to Flow.  abort(), however, is
     * fine!  After all, an uncaught exception in a thread not managed by us indeed calls abort() through
     * std::terminate() by default (std::terminate() can also be overridden program-wide).  Other than bypassing
     * std::terminate(), then, it's the same behavior as is generally expected by default.  To not bypass
     * std::terminate() we could instead rethrow the exception.
     *
     * -3-, however, is even more direct.  If we let it fall through, std::terminate() -- the default behavior -- would
     * simply be in effect without our inserting ourselves into the situation.  If that's all we want (and we've now
     * made a good case for it) then doing nothing is less entropy then doing something that results in the same thing.
     *
     * So -3- is strictly better due to lessened entropy alone -- all else being equal.  Is there anything about -2-
     * that would make "all else" not equal?  Yes: we could log .what() first before re-throwing/aborting.
     * This, however, would come at a major cost: By 1st catching the exception we'd lose the stack trace information:
     * any generated core would show the stack of the location here in the catch-location, not the original
     * throw-location.  Between printing the exception message and getting the full stack trace in a potential core,
     * the latter wins in practice by far.  At least, by default, we shouldn't be taking that possibility away.
     *
     * So that's why we do nothing.
     *
     * Last thought: Is there something one can do to *both* get the stack trace in a core file *and* print the
     * exception .what()?  (For that matter, with boost.backtrace and similar, it should be possible to print a stack
     * trace to the logs as well.)  The answer is yes, though it's not on us to do it.  One should do such work either
     * in std::terminate() (by using std::set_terminate()) or, arguably even better, in a global SIGABRT handler.
     * I am only mentioning it here as opportunistic advice -- again, it's not in our purview, as shown above. */
  })); // Thread body.
  // `nickname`, `init_task_or_empty` may now be hosed.

  if (done_promise_else_block)
  {
    FLOW_LOG_TRACE("Thread started -- confirmation that it is up shall be signalled to caller through a `promise`.");
  }
  else
  {
    FLOW_LOG_TRACE("Thread started -- awaiting confirmation it is up.");
    done_promise.get_future().wait();
    FLOW_LOG_TRACE("Confirmed thread started -- Task_qing_thread [" << this << "] ready.");
  }
} // Task_qing_thread::Task_qing_thread()

Task_qing_thread::~Task_qing_thread()
{
  FLOW_LOG_TRACE("Task_qing_thread [" << this << "]: Destroying object.");

  stop();

  // Last thing logged on our behalf -- INFO.
  FLOW_LOG_INFO("Task_qing_thread [" << this << "]: Thread was stopped/joined just now or earlier; "
                "destructor return imminent; Task_engine destruction *may* therefore also be imminent.");
}

void Task_qing_thread::stop()
{
  /* Subtlety: stop() is to be a harmless no-op if called twice.  We therefore perform an explicit check for it
   * and return if detected.  This isn't strictly necessary; Task_engine::stop() and Thread::join() do work if called
   * again after calling them once, and they just no-op, as desired.  We do it because:
   *   - It's cleaner: one less code path to have to reason about in terms of safety.
   *   - Less spurious/verbose logging.
   *   - It's a tiny bit faster.
   *
   * This isn't a mere corner case; the destructor calls us even if stop() was called explicitly earlier. */
  const bool redundant = !m_worker_thread->joinable(); // A/k/a "not-a-thread" <=> "not joinable."

  if (redundant)
  {
    FLOW_LOG_TRACE("Task_qing_thread [" << this << "]: Not waiting for worker thread "
                  "[T" << m_worker_thread->get_id() << "] to finish, as it was already stop()ped earlier.");
  }
  else
  {
    FLOW_LOG_INFO("Task_qing_thread [" << this << "]: Waiting for worker thread "
                  "[T" << m_worker_thread->get_id() << "] to finish, as it was not stop()ped earlier.");
  }
  // Ensure we understand properly the relationship between Task_engine::stopped() and this thread's existence.
  assert((!m_own_task_engine) // In this case, as promised, it's up to them to stop m_task_engine at some point.
         || (m_task_engine->stopped() == redundant)); // && m_own_task_engine)
  if (redundant)
  {
    return;
  }
  // else OK; this is the first stop() call.  Do as we promised.

  if (m_own_task_engine)
  {
    m_task_engine->stop(); // Trigger our owned Task_engine to execute no futher tasks and hence exit run().
  }
  // else { Caller will have to do something similar before the following statement is able to return. }

  m_worker_thread->join();
  // Mirror the top of this method.
  assert(!m_worker_thread->joinable());
  assert((!m_own_task_engine) || m_task_engine->stopped());

  FLOW_LOG_TRACE("Task_qing_thread [" << this << "]: Thread stopped/joined; stop() return imminent.");
} // Task_qing_thread::stop()

Task_engine_ptr Task_qing_thread::task_engine()
{
  return m_task_engine;
}

util::Thread* Task_qing_thread::raw_worker_thread()
{
  return m_worker_thread.get(); // Valid until destructor.
}

} // namespace flow::async
