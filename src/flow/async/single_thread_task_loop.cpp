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
#include "flow/async/single_thread_task_loop.hpp"

namespace flow::async
{

// Single_thread_task_loop implementations.

Single_thread_task_loop::Single_thread_task_loop(log::Logger* logger_ptr, util::String_view nickname) :
  log::Log_context(logger_ptr, Flow_log_component::S_ASYNC),
  m_underlying_loop(get_logger(), nickname, 1) // Attn: 1 thread.  Other (optional) args become irrelevant.
{
  FLOW_LOG_INFO("Single_thread_task_loop [" << static_cast<const void*>(this) << "] "
                "with nickname [" << nickname << "], backed by a 1-thread Cross_thread_task_loop: "
                "Started.  Above Cross_thread_task_loop messages, if any, contain details.");
}

Single_thread_task_loop::~Single_thread_task_loop() // Virtual.
{
  FLOW_LOG_INFO("Single_thread_task_loop [" << this << "]: Destroying object: see subsequent Cross_thread_task_loop "
                "messages if any.");
}

void Single_thread_task_loop::start(Task&& init_task_or_empty)
{
  // There's an optional 2nd arg we omit.
  m_underlying_loop.start(std::move(init_task_or_empty), [this](size_t)
  {
    // This occurs ~first thing in the thread (before init_task_or_empty() if any).
    m_started_thread_id_or_none = util::this_thread::get_id();
  });
}

bool Single_thread_task_loop::in_thread() const
{
  return util::this_thread::get_id() == m_started_thread_id_or_none; // Corner case: false before start().
}

void Single_thread_task_loop::stop()
{
  m_underlying_loop.stop();
}

void Single_thread_task_loop::post(Task&& task, Synchronicity synchronicity)
{
  m_underlying_loop.post(std::move(task), synchronicity);
}

util::Scheduled_task_handle Single_thread_task_loop::schedule_from_now(const Fine_duration& from_now,
                                                                       Scheduled_task&& task)
{
  return m_underlying_loop.schedule_from_now(from_now, std::move(task));
}

util::Scheduled_task_handle Single_thread_task_loop::schedule_at(const Fine_time_pt& at,
                                                                 Scheduled_task&& task)
{
  return m_underlying_loop.schedule_at(at, std::move(task));
}

Task_engine_ptr Single_thread_task_loop::task_engine()
{
  return m_underlying_loop.task_engine();
}

Concurrent_task_loop* Single_thread_task_loop::underlying_loop()
{
  return static_cast<Concurrent_task_loop*>(&m_underlying_loop);
}

// Timed_single_thread_task_loop implementations.

Timed_single_thread_task_loop::Timed_single_thread_task_loop(log::Logger* logger_ptr, util::String_view nickname,
                                                             perf::Clock_type clock_type) :
  Single_thread_task_loop(logger_ptr, nickname),
  m_timed_loop(underlying_loop(), clock_type,
               // Not thread-safe: but we have only one thread.
               [](perf::duration_rep_t* time_acc) -> perf::duration_rep_t
                 { const auto ret = *time_acc; *time_acc = 0; return ret; })
{
  // That's it.
}

void Timed_single_thread_task_loop::post(Task&& task, Synchronicity synchronicity)
{
  m_timed_loop.post(std::move(task), synchronicity);
}

util::Scheduled_task_handle Timed_single_thread_task_loop::schedule_from_now
                              (const Fine_duration& from_now, Scheduled_task&& task)
{
  return m_timed_loop.schedule_from_now(from_now, std::move(task));
}

util::Scheduled_task_handle Timed_single_thread_task_loop::schedule_at(const Fine_time_pt& at, Scheduled_task&& task)
{
  return m_timed_loop.schedule_at(at, std::move(task));
}

perf::Duration Timed_single_thread_task_loop::accumulated_time()
{
  return m_timed_loop.accumulated_time();
}

} // namespace flow::async
