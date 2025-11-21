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

#include "flow/util/sched_task_fwd.hpp"
#include "flow/util/detail/sched_task_handle_state.hpp"
#include "flow/log/log.hpp"
#include "flow/error/error.hpp"

namespace flow::util
{

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Scheduled_task_handler>
Scheduled_task_handle schedule_task_from_now(log::Logger* logger_ptr,
                                             const Fine_duration& from_now, bool single_threaded,
                                             Task_engine* task_engine,
                                             Scheduled_task_handler&& task_body_moved)
{
  using boost::chrono::round;
  using boost::chrono::milliseconds;
  using boost::system::system_error;
  using boost::asio::bind_executor;
  using boost::asio::get_associated_executor;
  using boost::asio::post;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_UTIL);

  // See schedule_task_from_now() doc header for philosophical intro to what we are doing here: the context. Then read:

  const auto task_id = Unique_id_holder::create_unique_id();

  /* Shove all the state here, which is a ref-counted pointer around this simple struct.
   * Because we copy that ref-counted pointer into any lambda/closure we schedule, the state -- including the Timer
   * itself! -- will continue existing until none of *our* code needs it, at which point ref-count=0, and it is all
   * destroyed automatically.
   *
   * In addition, though, we return (in opaque fashion) a copy of this pointer in the return value of the present
   * function.  The life-time can thus be extended by the user (not that they need to worry about that internal
   * detail).  This is all trivially safe -- it exists until it doesn't need to, automatically -- but is there a
   * potential for wasted memory?  Answer: Not really; in addition to the struct being fairly light-weight, there's no
   * reason for user to keep a copy of this handle once the task has fired, or they've had to cancel it.
   *
   * ### IMPORTANT SUBTLETY REGARDING THE SAVED LAMBDA IN task->m_body (LAST ARG TO CTOR) ###
   * Why save a wrapper function around task_body_moved (could just set m_body = task_body_moved), and
   * why not just execute m_body(<true or false>) synchronously from within the async_wait() handler below or
   * the handler post()ed by scheduled_task_short_fire(), and moreover why the strange-looking
   * get_associated_executor()/bind_executor() thing (could just post() task_body)?  Answer:
   *
   * For that matter, every other API as of this writing seems to take a concrete type, a Function<>, for callbacks --
   * avoiding templates -- but we take a template-param-typed arg.  Why?  Answer(s):
   *
   * schedule_task_from_now() is, essentially, a boost.asio extension that is akin to its various async_*() methods
   * and free functions (like Timer::async_wait() itself), in that it takes a completion handler to execute async later.
   * As such there is the expectation that, if it's associated with a `strand`, it will execute from within that strand
   * (meaning non-concurrently with any other handlers also so-associated).  (More generally, a strand is an "executor",
   * and one expects it to be executed through that "executor."  See boost.asio docs, not that they're super-readable,
   * especially the link below.)
   *
   * Long story short, if they pass in X = boost::asio::bind_executor(some_strand, some_func), where some_func()
   * takes 1+ args, then to execute it on *task_engine we must do post(*task_engine, F), where F() takes no args
   * and invokes X(args...) in its { body }.  An easy mistake, and bug, is to simply call X(args...).
   * Per https://www.boost.org/doc/libs/1_77_0/doc/html/boost_asio/overview/core/strands.html
   * and https://www.boost.org/doc/libs/1_77_0/doc/html/boost_asio/reference/post.html, post(*task_engine, F) picks up
   * the association with some_strand via get_associated_executor() machinery, internally.  If we simply call
   * X(args...), then that association is ignored.  Therefore, when we post the wrapper F(), we have to associate F()
   * with some_strand.  This has 2 implications:
   *   - We have to post() the wrapper around X(), and we must re-bind that wrapper with X()'s associated executor.
   *     This is done via bind_executor(get_associated_executor(X), F).
   *   - Even more subtly, if we took task_body_moved as a Function<> (Scheduled_task) and not a template arg
   *     Scheduled_task_handler, then their X will be beautifully auto-converted into a wrapping Function<>()...
   *     thus making get_associated_executor(Function<>(X)) return the default thing, and not some_strand.
   *     Hence we must carefully take their *actual* function object of indeterminate type, and extract the
   *     associated executor from *that*, not from Function<>(X).
   *
   * The rest flows from that.  We post() a very simple wrapper (with a bit of logging) through the same executor
   * (if any; if it's the default one then it still works) as the handler object they passed in.  I (ygoldfel) have
   * written so much text only because this is an easy mistake to make, and I have made it, causing a bug: it still
   * compiles and works... but not through the strand.  Tip: To directly check whether it worked,
   * `some_strand.running_in_this_thread()` must return true inside some_func().  Beats trying to force a thread-unsafe
   * situation and try to be satisfied when it doesn't occur (which is luck-based).
   *
   * P.S. I have verified, both in theory and in practice, that
   * `post(some_task_engine, bind_executor(get_associated_executor(F), F)` still executes through some_task_engine,
   * even when F is a vanilla lambda/function and not associated with any strand.  The theoretical reason it works
   * can be found in the post() doc link above, where it explains how precisely it invokes F() (both
   * some_task_engine, itself an executor, and g_a_e(F) are involved -- it works out; details omitted here).
   * The reason one might worry is that g_a_e(F), where F is non-strandy/vanilla, yields system_executor, which -- if
   * post()ed onto *directly* -- uses some unspecified thread/pool.  But together with post(some_task_engine),
   * it does work.
   *
   * P.P.S. Why not execute the async_wait()'s completion handler below through g_a_e(task_body_moved)?  1, we have
   * our own algorithm for avoiding thread-unsafety and want to use *task_engine directly; it is less entropy-laden
   * to apply task_body_moved's associated executor only to task_body_moved itself (plus a bit of bracketing logging).
   * Whatever executor they associated task_body_moved with -- perhaps a strand, or something else -- has unknown
   * properties, and we have our own algorithm.
   * 2, then scheduled_task_short_fire() can just reuse task->m_body which already takes care of the
   * properly-executor-associated post()ing of task_body_moved.
   */
  auto task = boost::make_shared<Scheduled_task_handle_state>
                (task_id, single_threaded, task_engine, [task_id, get_logger, get_log_component,
                                                         task_engine,
                                                         task_body = std::move(task_body_moved)]
                                                          (bool short_fire) mutable
  {
    // Not safe to rely on L->R arg evaluation below; get this 1st, when we know task_body hasn't been move()d.
    const auto executor = get_associated_executor(task_body); // Usually system_executor (vanilla) or a strand.
    post(*task_engine,
         bind_executor(executor, [get_logger, get_log_component, task_id,
                                  task_body = std::move(task_body), short_fire]()
    {
      FLOW_LOG_TRACE("Scheduled task [" << task_id <<"]: Body starting; short-fired? = [" << short_fire << "].");
      task_body(short_fire); // NOTE: Might throw.
      FLOW_LOG_TRACE("Scheduled task [" << task_id <<"]: Body finished without throwing exception.");
    }));
  });
  // task_body_moved is now potentially destroyed after move().

  // To be clear: If we want user's handler executed (below, or when short-firing) -- just call task->m_body(...).

  auto& timer = task->m_timer;

  FLOW_LOG_TRACE("Scheduling task [" << task->m_id << "] "
                 "to fire in [" << round<milliseconds>(from_now) << "]; "
                 "task engine [" << static_cast<void*>(task_engine) << "]; "
                 "single-threaded optimizations applicable? = [" << single_threaded << "].");

  timer.expires_after(from_now);

  /* We've prepared it all -- now kick off the state machine by scheduling an actual thing to run at the chosen time.
   * From this point on, unless single_threaded, the mutex must be used when accessing m_timer, m_canceled, m_ired.
   *
   * We don't schedule user-supplied body itself but rather a wrapper lambda around that body.  This allows us to
   * avoid various corner cases and annoyances of Timer -- as explained at length in the
   * present function's doc header. */
  timer.async_wait([get_logger, get_log_component, task](const Error_code& sys_err_code)
  {
    const bool single_threaded = !task->m_mutex_unless_single_threaded;

    /* operation_aborted only occurs due to m_timer.cancel().  Under our semantics, the only way the latter
     * happens is if WE INTERNALLY do that, and the only reason we do that is if user did
     * scheduled_task_cancel() or scheduled_task_short_fire().
     *   - cancel case: They've set m_canceled = true, so below code will detect it and return without m_body() call.
     *   - short-fire case: They've set m_fired = true and posted m_body() call manually, so below code will similarly
     *     detect it and return without m_body() call.
     *
     * Hence, operation_aborted is indeed possible, but we use m_fired and m_canceled to clearly record why it
     * happened.  So, we simply allow operation_aborted but subsequently ignore whether it happened.  (The most we
     * could do is a sanity-check that if operation_aborted then m_fired or m_canceled must be true.) */

#ifndef NDEBUG // The aforementioned sanity check.
    if (sys_err_code == boost::asio::error::operation_aborted)
    {
      if (single_threaded)
      {
        assert(task->m_fired != task->m_canceled); // I.e., exactly one of them must be true.
      }
      else
      {
        Lock_guard<Mutex_non_recursive> lock{*task->m_mutex_unless_single_threaded};
        assert(task->m_fired != task->m_canceled);
      }
    }
#endif

    if (sys_err_code && (sys_err_code != boost::asio::error::operation_aborted)) // As noted, ignore _aborted case now.
    {
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      FLOW_LOG_WARNING("Timer system error; just logged; totally unexpected; pretending it fired normally.");
      /* Again, this is totally unexpected.  Could throw here, but pretending it fired seems less explosive and
       * entropy-laden and might work out somehow.  Regardless, this is no-man's-land and a best effort. */
    }

    /* OK, it fired legitimately -- but if we marked it as already short-fired or canceled then we just NOOP.
     * This helper checks whether we should NOOP or actually run m_body().  Pre-condition: Thread-safe to touch *task.
     * It's a function due to the single_threaded dichotomy just below.  Otherwise it'd just be direct inline code. */
    auto should_fire = [&]() -> bool // [&] meaning this can only be called synchronously within current function.
    {
      if (task->m_fired)
      {
        FLOW_LOG_TRACE("Scheduled task [" << task->m_id << "] native timer fired (could be due to cancel() call), "
                       "but user handler already began executing earlier.  Done.");
        return false;
      }
      // else
      if (task->m_canceled)
      {
        FLOW_LOG_TRACE("Scheduled task [" << task->m_id << "] native timer fired (could be due to cancel() call), "
                       "but task was already canceled earlier.  Done.");
        return false;
      }
      // else

      FLOW_LOG_TRACE("Scheduled task [" << task->m_id << "] native timer fired (could be due to cancel() call), "
                     "and nothing has fired or canceled task already.  Proceeding with task (through post()).");

      task->m_fired = true;
      return true;
    }; // should_fire =

    bool noop;
    if (single_threaded) // Single-threaded => pre-condition is trivially true.  Else guarantee the pre-condition.
    {
      noop = !should_fire();
    }
    else
    {
      Lock_guard<Mutex_non_recursive> lock{*task->m_mutex_unless_single_threaded};
      noop = !should_fire();
    }

    if (noop)
    {
      return;
    }
    // else

    (task->m_body)(false); // false <=> Regular firing; did not short-circuit.
    FLOW_LOG_TRACE("Scheduled task [" << task->m_id <<"]: Body-post()ing function finished.");
  }); // async_wait()

  return task; // If they save a copy of this (which is optional) they can call scheduled_task_cancel(), etc., on it.
} // schedule_task_from_now()

template<typename Scheduled_task_handler>
Scheduled_task_handle schedule_task_at(log::Logger* logger_ptr,
                                       const Fine_time_pt& at, bool single_threaded,
                                       Task_engine* task_engine,
                                       Scheduled_task_handler&& task_body_moved)
{
  /* The core object, Timer m_timer, has expires_at() and expires_after() available.  As a black box, perhaps it'd
   * be best for us to call those respectively from schedule_task_at() and schedule_task_from_now().  However,
   * certain boost.asio docs suggest that ultimately Timer is built around the "fire T period after now" routine,
   * so it would ultimately just subtract Fine_clock::now() anyway even in expires_at().  So, for simpler code, we just
   * do that here and no longer worry about it subsequently.
   *
   * This is the reason for the `### Performance note ###` in our doc header. */
  return schedule_task_from_now<Scheduled_task_handler>(logger_ptr,
                                                        at - Fine_clock::now(),
                                                        single_threaded, task_engine, std::move(task_body_moved));
}

} // namespace flow::util
