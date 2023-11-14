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

#include "flow/util/detail/util_fwd.hpp"

namespace flow::util
{

// Free functions: in *_fwd.hpp.

// Template/constexpr implementations.

template<typename Rep, typename Period>
Fine_duration chrono_duration_to_fine_duration(const boost::chrono::duration<Rep, Period>& dur)
{
  using boost::chrono::duration;
  using boost::chrono::ceil;
  using User_duration = duration<Rep, Period>;

  assert(dur.count() >= 0);
  return (dur == User_duration::max()) ? Fine_duration::max() : ceil<Fine_duration>(dur);
}

template<typename Rep, typename Period>
Fine_time_pt chrono_duration_from_now_to_fine_time_pt(const boost::chrono::duration<Rep, Period>& dur)
{
  using boost::chrono::duration;
  using boost::chrono::ceil;
  using User_duration = duration<Rep, Period>;

  assert(dur.count() >= 0);
  return (dur == User_duration::max())
           ? Fine_time_pt()
           : (Fine_clock::now() + ceil<Fine_duration>(dur));
}

constexpr String_view get_last_path_segment(String_view full_path)
{
  String_view path(full_path); // This only copies the pointer and length (not the string).
  // @todo Get it from boost::filesystem or something:
#  ifdef FLOW_OS_WIN
  constexpr char SEP = '\\';
#  else
  constexpr char SEP = '/';
#  endif
  /* Here I just want to do path.rfind(SEP)... but gcc 5.4 (at least) refuses to compile it with the helpful
   * message "Sorry, not implemented <some words about how there's a loop involved>."  Hence I am doing it manually,
   * and that worked, even though there's a loop involved.
   * @todo Can probably change it back to rfind() in C++20 which allegedly will make all/most
   * APIs in <algorithm> constexpr which would trickle down to rfind().
   * Oh, also, one would think there's a backwards-searching standard function, like strrchr() but given a length
   * as input, but I couldn't find one.  If it existed it'd probably be constexpr (std::strlen() is for example).
   * Doesn't matter though. */
  const auto path_sz = path.size();
  if (path_sz != 0)
  {
    const auto path_ptr = path.data();
    for (auto path_search_ptr = path_ptr + path_sz - 1;
         path_search_ptr >= path_ptr; --path_search_ptr)
    {
      if ((*path_search_ptr) == SEP)
      {
        path.remove_prefix((path_search_ptr - path_ptr) + 1);
        break;
      }
    }
  }
  // else { Nothing to do. }

  return path;
} // get_last_path_segment()

} // namespace flow::util

// Macros.

/**
 * Helper macro, same as FLOW_UTIL_WHERE_AM_I(), but takes the source location details as arguments instead of
 * grabbing them from `__FILE__`, `__FUNCTION__`, `__LINE__`.  Arguably not useful outside of the `flow::util` module
 * itself.
 *
 * ### Perf instructions ###
 * For best perf results: for all `ARG_...` that you pass in as flow::util::String_view please follow instructions in
 * doc header of log::Msg_metadata::m_msg_src_file and log::Msg_metadata::m_msg_src_function.
 *
 * @param ARG_file
 *        Full file name, as from `__FILE__`, as a `String_view`; or a fragment inside it (e.g., just the
 *        part past the last dir separator if any); depending on which part you'd prefer ultimately printed.
 * @param ARG_function
 *        Full function name, as from `__FUNCTION__`, as a `String_view` (recommended for perf) or `const char*`.
 * @param ARG_line
 *        Line number, as from `__LINE__`.
 * @return `ostream` fragment `X` (suitable for, for example: `std::cout << X << ": Hi!"`).
 */
#define FLOW_UTIL_WHERE_AM_I_FROM_ARGS(ARG_file, ARG_function, ARG_line) \
  ARG_file << ':' << ARG_function << '(' << ARG_line << ')'

/**
 * Helper macro, same as #FLOW_UTIL_WHERE_AM_I_FROM_ARGS(), but results in a list of comma-separated, instead of
 * `<<` separated, arguments, although they are still to be passed to an `ostream` with exactly the same semantics
 * as the aforementioned macro.
 *
 * ### Perf instructions ###
 * See doc header for FLOW_UTIL_WHERE_AM_I_FROM_ARGS().
 *
 * @param ARG_file
 *        See FLOW_UTIL_WHERE_AM_I_FROM_ARGS().
 * @param ARG_function
 *        See FLOW_UTIL_WHERE_AM_I_FROM_ARGS().
 * @param ARG_line
 *        See FLOW_UTIL_WHERE_AM_I_FROM_ARGS().
 * @return Exactly the same as FLOW_UTIL_WHERE_AM_I_FROM_ARGS() but with commas instead of `<<`.
 */
#define FLOW_UTIL_WHERE_AM_I_FROM_ARGS_TO_ARGS(ARG_file, ARG_function, ARG_line) \
  ::flow::util::get_last_path_segment(ARG_file), ':', ARG_function, '(', ARG_line, ')'
