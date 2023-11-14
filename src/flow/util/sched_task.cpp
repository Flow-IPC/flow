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
#include "flow/util/sched_task.hpp"

namespace flow::util
{

// Implementations.

bool scheduled_task_cancel(log::Logger* logger_ptr, Scheduled_task_handle task)
{
  using boost::chrono::round;
  using boost::chrono::milliseconds;
  using boost::system::system_error;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_UTIL);

  auto& timer = task->m_timer;
  const bool single_threaded = !task->m_mutex_unless_single_threaded;

  /* OK, they want to cancel it -- but if we marked it as already [short-]fired or canceled then we just NOOP.
   * This helper checks whether we should NOOP or actually prevent m_body().  Pre-condition: Thread-safe to touch *task.
   * It's a function due to the single_threaded dichotomy just below.  Otherwise it'd just be direct inline code. */
  auto cancel_if_should = [&]() -> bool // [&] meaning this can only be called synchronously within current function.
  {
    /* This is inside cancel_if_should() because it accesses `timer.*()` potentially concurrently with another thread:
     * e.g., another scheduled_task_cancel() call. */
    FLOW_LOG_TRACE("Canceling scheduled task [" << task->m_id << "] "
                   "that was set to fire in [" << round<milliseconds>(timer.expires_from_now()) << "]; "
                   "single-threaded optimizations applicable? = [" << single_threaded << "].");

    if (task->m_fired)
    {
      FLOW_LOG_TRACE("Was going to cancel but user handler already began executing earlier.  Done.");
      return false;
    }
    // else
    if (task->m_canceled)
    {
      FLOW_LOG_TRACE("Was going to cancel but task was already canceled earlier.  Done.");
      return false;
    }

    task->m_canceled = true;
    // The body will never run now, as it checks m_canceled before executing m_body (see schedule_task_from_now()).

    /* For cleanliness, short-circuit the scheduled function to run now with Error_code `operation_aborted`.
     * We could also let it run harmlessly as scheduled, but this is less entropy-laden and a bit more
     * resource-frugal, in that *task might be destroyed earlier this way. */
    try
    {
      timer.cancel();
    }
    catch (const system_error& exc) // See similar `catch` in schedule_task_from_now().
    {
      const Error_code sys_err_code = exc.code();
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      // Do not re-throw: Even if our scheduled function still runs, it'll only log, as `(m_canceled == true)`.
    }

    return true;
  }; // cancel_if_should =

  bool canceled;
  if (single_threaded)
  {
    canceled = cancel_if_should();
  }
  else
  {
    Lock_guard<Mutex_non_recursive> lock(*task->m_mutex_unless_single_threaded);
    canceled = cancel_if_should();
  }

  // At this point cancel_if_should() already either did something or NOOPed.  So we just inform user which one.
  return canceled;
} // scheduled_task_cancel()

bool scheduled_task_short_fire(log::Logger* logger_ptr, Scheduled_task_handle task)
{
  using boost::asio::post;
  using boost::chrono::round;
  using boost::chrono::milliseconds;
  using boost::system::system_error;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_UTIL);

  /* This function is very similar to scheduled_task_cancel() but different enough to copy/paste some comments
   * for clarity at the expense of some brevity. */

  auto& timer = task->m_timer;
  const bool single_threaded = !task->m_mutex_unless_single_threaded;

  /* OK, they want to fire it early -- but if we marked it as already [short-]fired or canceled then we just NOOP.
   * This helper checks whether we should NOOP or actually prevent scheduled m_body() and run m_body now.
   * Pre-condition: Thread-safe to touch *task.
   * It's a function due to the single_threaded dichotomy just below.  Otherwise it'd just be direct inline code. */
  auto fire_if_should = [&]()
  {
    FLOW_LOG_TRACE("Short-firing scheduled task [" << task->m_id << "] "
                   "that was set to fire in [" << round<milliseconds>(timer.expires_from_now()) << "]; "
                   "single-threaded optimizations applicable? = [" << single_threaded << "].");

    if (task->m_fired)
    {
      FLOW_LOG_TRACE("Was going to short-fire but user handler already began executing earlier.  Done.");
      return false;
    }
    // else
    if (task->m_canceled)
    {
      FLOW_LOG_TRACE("Was going to short-fire but task was already canceled earlier.  Done.");
      return false;
    }

    task->m_fired = true;
    // Body won't run (as scheduled) now, as it checks m_fired before executing m_body (see schedule_task_from_now()).

    /* For cleanliness, short-circuit the scheduled function to run now with Error_code `operation_aborted`.
     * We will post() the body directly, below.  We could have instead done it through the operation_aborted
     * path, and that would've also been fine; prefer this by a tiny bit because it's more tightly under our control --
     * e.g., no need to worry about even the possibility of the officially-possible situation wherein cancel() fails to
     * succeed, because it happened to *just* fire around the same time.  Also there's a pleasing similarity between
     * this logic and that in scheduled_task_cancel() which, in some sense, reduces entropy also. */
    try
    {
      timer.cancel();
    }
    catch (const system_error& exc) // See similar `catch` in scheduled_task_cancel().
    {
      const Error_code sys_err_code = exc.code();
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
    }

    return true;
  }; // fire_if_should =

  bool noop;
  if (single_threaded)
  {
    noop = !fire_if_should();
  }
  else
  {
    Lock_guard<Mutex_non_recursive> lock(*task->m_mutex_unless_single_threaded);
    noop = !fire_if_should();
  }

  if (noop)
  {
    return false;
  }
  // else

  /* Cool -- we've made sure m_body() won't be called as scheduled.  Since we are to short-fire -- not merely cancel --
   * it falls to us to early-execute m_body() (as rationalized above).
   * If we'd used Timer::cancel(), Timer would do this for us instead. */
  post(*task->m_task_engine, [get_logger, get_log_component, task]() // Capture copy of `log*` so logging just works.
  {
    const bool single_threaded = !task->m_mutex_unless_single_threaded;

    FLOW_LOG_TRACE("Short-executing recently-short-fired scheduled task [" << task->m_id << "]; "
                   "single-threaded optimizations applicable? = [" << single_threaded << "].");

#ifndef NDEBUG // Sanity-check how we got here.  Other than this, all our wrapper adds is some logging.
    if (single_threaded)
    {
      assert(task->m_fired && (!task->m_canceled));
    }
    else
    {
      Lock_guard<Mutex_non_recursive> lock(*task->m_mutex_unless_single_threaded);
      assert(task->m_fired && (!task->m_canceled));
    }
#endif

    (task->m_body)(true); // true <=> NOT regular firing; we short-circuited.
    FLOW_LOG_TRACE("Short-fired scheduled task [" << task->m_id << "]: Body-post()ing function finished.");
  }); // post()

  return true;
} // scheduled_task_short_fire()

Fine_duration scheduled_task_fires_from_now_or_canceled(log::Logger* logger_ptr, Scheduled_task_const_handle task)
{
  using boost::chrono::milliseconds;
  using boost::chrono::round;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_UTIL);

  const auto do_it = [&]() -> Fine_duration
  {
    if (task->m_canceled)
    {
      FLOW_LOG_TRACE("Scheduled task [" << task->m_id <<"]: Returning special value for fires-from-now because "
                     "task canceled.");
      return Fine_duration::max();
    }
    // else
    const auto& from_now = task->m_timer.expires_from_now();
    FLOW_LOG_TRACE("Scheduled task [" << task->m_id <<"]: Returning fires-from-now value "
                   "[" << round<milliseconds>(from_now) << "].");

    return from_now;
  };

  const auto& mtx = task->m_mutex_unless_single_threaded;
  if (mtx)
  {
    Lock_guard<Mutex_non_recursive> lock(*mtx);
    return do_it();
  }
  // else
  return do_it();

  /* Note: I haven't added a similar function that would return an absolute time point.  The real argument for it
   * is consistency, but there are some nuanced ambiguities about how/whether to return a consistent value each time;
   * or just use now(); etc.  Not worth getting into it here; but ultimately it should be trivial for the caller
   * to just use now() themselves; or save the value at scheduling time; or.... */
}

bool scheduled_task_fired(log::Logger* logger_ptr, Scheduled_task_const_handle task)
{
  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_UTIL);

  const auto do_it = [&]() -> bool
  {
    const bool is_it = task->m_fired;
    FLOW_LOG_TRACE("Scheduled task [" << task->m_id <<"]: Returning fired? = [" << is_it << "].");
    return is_it;
  };

  const auto& mtx = task->m_mutex_unless_single_threaded;
  if (mtx)
  {
    Lock_guard<Mutex_non_recursive> lock(*mtx);
    return do_it();
  }
  // else
  return do_it();
}

bool scheduled_task_canceled(log::Logger* logger_ptr, Scheduled_task_const_handle task)
{
  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_UTIL);

  const auto do_it = [&]() -> bool
  {
    const bool is_it = task->m_canceled;
    FLOW_LOG_TRACE("Scheduled task [" << task->m_id <<"]: Returning canceled? = [" << is_it << "].");
    return is_it;
  };

  const auto& mtx = task->m_mutex_unless_single_threaded;
  if (mtx)
  {
    Lock_guard<Mutex_non_recursive> lock(*mtx);
    return do_it();
  }
  // else
  return do_it();
}

} // namespace flow::util
