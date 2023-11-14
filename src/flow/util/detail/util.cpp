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
#include "flow/util/detail/util.hpp"
#include "flow/util/util.hpp"

namespace flow::util
{

// Implementations.

std::string get_where_am_i_str(String_view file, String_view function, unsigned int line)
{
  /* @todo There's a to-do in the one invoker of this as of this writing, namely FLOW_UTIL_WHERE_AM_I_STR(), to
   * make that thing into a compile-time expression; which really reduces to making the present function constexpr.
   * For example, FLOW_UTIL_WHERE_AM_I_FROM_ARGS() it compile-time, and it basically only took
   * making get_last_path_segment() constexpr (which wasn't trivial but doable).  One problem here is that macro
   * only needs to evaluate to an ostream <<fragment<<, while we must actually create a string, which involves an
   * extra step.  That may still be doable, if we can keep [concatenation of 3 string vars and a few "literals"; +
   * conversion of a number (`line`) to string] to constexpr-supported syntax.  With a methodical approach it may be
   * possible to build such a thing from the bottom up.  Is itoa() `constexpr`able?  Is string concatenation in
   * some form?  Perhaps. */

  using std::string;

  string out; // Write directly into this string.
  ostream_op_to_string(&out, FLOW_UTIL_WHERE_AM_I_FROM_ARGS_TO_ARGS(file, function, line));

  return out;
}

} // namespace flow::util
