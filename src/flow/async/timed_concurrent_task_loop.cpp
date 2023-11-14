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

#include "flow/async/timed_concurrent_task_loop.hpp"

namespace flow::async
{

// Implementations.

Timed_concurrent_task_loop::Timed_concurrent_task_loop(Concurrent_task_loop* loop, perf::Clock_type clock_type) :
  Impl(loop, clock_type,
       // Thread-safe.
       [](Time_accumulator* time_acc) -> perf::duration_rep_t { return time_acc->exchange(0); })
{
  // That's it.  The above is why we exist as a subclass and not just an alias.
}

} // namespace flow::async
