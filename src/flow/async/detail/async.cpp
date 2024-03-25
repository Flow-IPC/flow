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

#include "flow/async/detail/async_fwd.hpp"
#include "flow/common.hpp"
#include <limits>
#ifdef FLOW_OS_LINUX
#  include <sched.h>
#endif

namespace flow::async
{

uint16_t cpu_idx()
{
  using std::numeric_limits;
  using ret_int_t = uint16_t;

#ifdef FLOW_OS_LINUX
  using ::sched_getcpu;

  return ret_int_t(sched_getcpu());
#else
  static_assert(false, "cpu_idx() implementation is for Linux.  "
                         "Revisit for other OS and/or architectures (x86-64, ARM64, ...).");
#endif
}

} // namespace flow::async
