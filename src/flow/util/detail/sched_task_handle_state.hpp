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
#include "flow/util/util_fwd.hpp"
#include "flow/util/uniq_id_holder.hpp"
#include <boost/move/unique_ptr.hpp>
#include <boost/utility.hpp>

namespace flow::util
{

// Types.

/**
 * Internal-use data store used by `schedule[d]_task_*()`, each object containing the state
 * relevant to a single call to either schedule_task_from_now() or schedule_task_at().  The user accesses these
 * via the opaque type util::Scheduled_task_handle which is just `shared_ptr<>` around this `struct`.
 *
 * Internally, it's (1) storage for the underlying util::Timer itself, the task body, etc.; (2) a simple state machine
 * with 3 possible states: `!(m_canceled || m_fired)` (initial); `m_canceled && (!m_fired)` (final state 1);
 * `m_fired && (!m_canceled)` (final state 2).  Obviously the state machine can only move from the initial state to
 * exactly one of the final states.  Less obviously these members must be synchronized in any multi-threaded situation.
 */
struct Scheduled_task_handle_state :
  private boost::noncopyable
{
  /// Unique ID for logging.
  const Unique_id_holder::id_t m_id;

  /**
   * The function to execute when (and if) the timer fires, or short-fires.  Note that successful cancellation will
   * prevent this from being executed.  Reminder that the function takes one argument, `bool short_fire`, explained
   * elsewhere.  This will execute either 0 times or 1 time.
   */
  const Scheduled_task m_body;

  /**
   * Pointer to the task-executing engine (`Task_engine`) onto which to post tasks, including (if applicable) #m_body.
   */
  Task_engine* const m_task_engine;

  /**
   * Mutex protecting #m_fired, #m_canceled, and #m_timer from the moment `m_timer.async_wait()` is called.
   * Those members are safe to touch before that, as no other threads have knowledge of `this` until then.
   * However, in `single_threaded` mode this pointer is left null, and no mutex is necessary or used (perf measure).
   */
  mutable boost::movelib::unique_ptr<Mutex_non_recursive> m_mutex_unless_single_threaded;

  /**
   * The underlying timer object -- paired with #m_task_engine -- used to actually wait in the background and
   * ultimately execute (if applicable) a wrapper around #m_body.
   */
  Timer m_timer;

  /**
   * Flag indicating whether or not the following is true: A wrapper task around #m_body is about to fire very soon
   * or has already been fired.  This starts `false` and, at most, can change once to `true`; in the latter case
   * #m_canceled is then guaranteed to remain `false`.
   *
   * It (and #m_canceled) is used as a barrier: once #m_body is on track to be immediately executed, it cannot be
   * canceled or rescheduled; hence attempts to do so are ignored once either flag reaches `true`.
   */
  bool m_fired;

  /**
   * Flag indicating whether or not the following is true: From this point on, #m_body will absolutely not be executed.
   * This starts `false` and, at most, can change once to `true`; in the latter case
   * #m_fired is then guaranteed to remain `false`.
   *
   * It (and #m_fired) is used as a barrier: once #m_body is marked as canceled, it cannot be
   * re-canceled or rescheduled; hence attempts to do so are ignored once either flag reaches `true`.
   */
  bool m_canceled;

  /**
   * Constructs it to the initial state, wherein it has neither fired nor been canceled.
   *
   * @param id
   *        See #m_id.
   * @param single_threaded
   *        See schedule_task_from_now().
   * @param task_engine
   *        See schedule_task_from_now().
   * @param body_moved
   *        See schedule_task_from_now().
   */
  Scheduled_task_handle_state(Unique_id_holder::id_t id,
                              bool single_threaded, Task_engine* task_engine, Scheduled_task&& body_moved);
}; // struct Scheduled_task_handle_state

} // namespace flow::util
