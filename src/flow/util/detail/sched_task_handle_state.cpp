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

#include "flow/util/detail/sched_task_handle_state.hpp"

namespace flow::util
{

// Implementations.

Scheduled_task_handle_state::Scheduled_task_handle_state(Unique_id_holder::id_t id,
                                                         bool single_threaded, Task_engine* task_engine,
                                                         Scheduled_task&& body_moved) :
  m_id(id),
  m_body(std::move(body_moved)),
  m_task_engine(task_engine),
  m_mutex_unless_single_threaded(single_threaded ? static_cast<Mutex_non_recursive*>(nullptr)
                                                 : (new Mutex_non_recursive)),
  m_timer(*m_task_engine),
  m_fired(false),
  m_canceled(false)
{
  // Nothing else.
}

} // namespace flow::util
