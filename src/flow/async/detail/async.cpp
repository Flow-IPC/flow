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
#ifdef FLOW_OS_MAC
#  include <cpuid.h>
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
#elif defined(FLOW_OS_MAC)
  /* Adapted from: https://stackoverflow.com/questions/33745364/sched-getcpu-equivalent-for-os-x
   * Their comments omitted, since they wouldn't be to the desired level of quality anyway.  @todo Comment well. */
  uint32_t cpu_info[4];
  __cpuid_count(1, 0, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]); // It's a macro.  It loads cpu_info[]s.
  if ((cpu_info[3] & (1 << 9)) == 0)
  {
    return 0;
  }
  // else
  const auto idx = cpu_info[1] >> 24;
  assert(idx <= numeric_limits<ret_int_t>::max());
  return ret_int_t(idx);
#else
#  error "cpu_idx() implementation is for Linux or Mac only.  Mac way'd likely work in any x86, but that's untested."
#endif
}

} // namespace flow::async
