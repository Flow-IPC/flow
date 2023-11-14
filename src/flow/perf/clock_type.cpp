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
#include "flow/perf/clock_type_fwd.hpp"
#include "flow/util/util.hpp"
#include "flow/util/string_ostream.hpp"

namespace flow::perf
{

Duration_set operator-(const Time_pt_set& to, const Time_pt_set& from)
{
  Duration_set result;
  // Could use some algorithm probably but speed is important, and I just don't care to risk it here for some syn-sugar.
  for (size_t clock_type_idx = 0; clock_type_idx != result.m_values.size(); ++clock_type_idx)
  {
    result.m_values[clock_type_idx]
      = to.m_values[clock_type_idx] - from.m_values[clock_type_idx]; // Can be negative; allowed.
  }
  return result;
}

Duration_set& operator+=(Duration_set& target, const Duration_set& to_add)
{
  // Same note as above re. algorithms.
  for (size_t clock_type_idx = 0; clock_type_idx != target.m_values.size(); ++clock_type_idx)
  {
    target.m_values[clock_type_idx] += to_add.m_values[clock_type_idx]; // All can be negative technically.
  }
  return target;
}

Time_pt_set& operator+=(Time_pt_set& target, const Duration_set& to_add)
{
  // Same note as above re. algorithms.
  for (size_t clock_type_idx = 0; clock_type_idx != target.m_values.size(); ++clock_type_idx)
  {
    target.m_values[clock_type_idx] += to_add.m_values[clock_type_idx]; // All can be negative technically.
  }
  return target;
}

Duration_set& operator*=(Duration_set& target, uint64_t mult_scale)
{
  // Same note as above re. algorithms.
  for (size_t clock_type_idx = 0; clock_type_idx != target.m_values.size(); ++clock_type_idx)
  {
    target.m_values[clock_type_idx] *= mult_scale;
  }
  return target;
}

Duration_set& operator/=(Duration_set& target, uint64_t div_scale)
{
  // Same note as above re. algorithms.
  for (size_t clock_type_idx = 0; clock_type_idx != target.m_values.size(); ++clock_type_idx)
  {
    target.m_values[clock_type_idx] /= div_scale;
  }
  return target;
}

std::ostream& operator<<(std::ostream& os, Clock_type clock_type)
{
  switch (clock_type)
  {
    case Clock_type::S_REAL_HI_RES: return os << "real_hi";
    case Clock_type::S_CPU_USER_LO_RES: return os << "cpu_usr_lo";
    case Clock_type::S_CPU_SYS_LO_RES: return os << "cpu_sys_lo";
    case Clock_type::S_CPU_TOTAL_HI_RES: return os << "cpu_total_hi";
    case Clock_type::S_CPU_THREAD_TOTAL_HI_RES: return os << "cpu_thread_total_hi";
    case Clock_type::S_END_SENTINEL: assert(false && "Should never print sentinel.");
  }
  assert(false && "Should be unreachable; compiler should warn if incomplete switch.");
  return os;
}

std::ostream& operator<<(std::ostream& os, const Duration_set& duration_set)
{
  using std::flush;
  using flow::util::ostream_op_to_string;
  using flow::util::String_ostream;
  using boost::chrono::microseconds;
  using boost::chrono::nanoseconds;
  using boost::chrono::round;

  // @todo Skip things not in m_which_clocks.

  for (size_t clock_type_idx = 0; clock_type_idx != duration_set.m_values.size(); ++clock_type_idx)
  {
    const auto& dur = duration_set.m_values[clock_type_idx];
    os << Clock_type(clock_type_idx) << ": ";
    if (dur.count() == 0)
    {
      os << '0'; // No units needed.
    }
    else
    {
      os << round<nanoseconds>(dur) << "=~" << round<microseconds>(dur); // Prints units too.
    }
    if ((clock_type_idx + 1) != duration_set.m_values.size())
    {
      os << " | ";
    }
  }
  return os << flush;
}

} // namespace flow::perf
