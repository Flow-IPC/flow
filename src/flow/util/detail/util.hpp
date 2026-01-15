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
           ? Fine_time_pt{}
           : (Fine_clock::now() + ceil<Fine_duration>(dur));
}

constexpr String_view get_last_path_segment(String_view full_path)
{
  String_view path{full_path}; // This only copies the pointer and length (not the string).
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

/**
 * Helper macro: like FLOW_UTIL_WHERE_AM_I(), with a major relative strength -- its replacement is a string literal --
 * and two differences: the function name must be supplied verbatim as an arg; and the source file name
 * will contain the entire path as opposed to a massaged version with just the file-name component.
 * The key is that the function name arg is not to be a string: it is *stringified* to get the string.
 *
 * Nevertheless, in perf-sensitive scenarios, this may well be worth it.  For now we keep it in a detail/ header
 * for use internally to Flow; we did in fact need this in flow::error.
 *
 * Other than the aforementioned difference and mild formatting tweaks for cosmetics (as of this writing an added
 * space), the format of the replacement's contents is identical to that from FLOW_UTIL_WHERE_AM_I().
 *
 * ### Rationale ###
 * Why need `ARG_function`?  Why not simply use `__FUNCTION__` internally?  Answer: Despite its similar look
 * to `__FILE__`, actually `__FUNCTION__` is *not* a macro: it is an identifier (that refers to a `const char*`).
 * The preprocessor knows what file (and line) it's scanning; but it has no idea what func it's scanning;
 * that's only known at a later stage of compilation.  So long story short: `__FUNCTION__` is not replaced
 * by a string literal and thus cannot be used to compose a string literal by compile-time concatenation.
 *
 * (`constexpr` sweetness can be used for an otherwise compile-time-determined value; but we promised a literal
 * here.  `constexpr`ness is outside our scope.  Though perhaps see FLOW_UTIL_WHERE_AM_I_FROM_ARGS() for that.)
 *
 * @param ARG_function
 *        Informally this is something akin to `ARG_function` to FLOW_ERROR_EXEC_AND_THROW_ON_ERROR().
 *        Formally this can be anything; it will be stringified via `#ARG_function` and output as the function name.
 *        (Do not attempt to pass here the name of a string-typed variable; that probably won't do
 *        what you want.  E.g., if you invoke `FLOW_UTIL_WHERE_AM_I_LITERAL(func_name)`, and `func_name`
 *        is an `std::string` with a function name, then the macro replacement will be
 *        `"/some/file.cpp:func_name(332)"` --
 *        without `func_name` getting replaced by the contents of your `func_name` variable or whatever.)
 * @return A string literal containing file name (and path; from `__FILE__`), function name given, and
 *         line number (from `__LINE__`).
 *         The string literal will be in the form `"stuff" "more stuff" "more stuff"` (etc.).  There will be
 *         no surrounding parentheses (in case you want to compile-time-concatenate to even more literal segments).
 */
#define FLOW_UTIL_WHERE_AM_I_LITERAL(ARG_function) \
  FLOW_UTIL_WHERE_AM_I_LITERAL_IMPL_OUTER(ARG_function, __LINE__)

/**
 * Impl helper macro from FLOW_UTIL_WHERE_AM_I_LITERAL().  This intermediate between
 * that guy and the "true" impl macro FLOW_UTIL_WHERE_AM_I_LITERAL_IMPL_INNER() is needed in order for the
 * preprocessor to substitute `__LINE__` instead of simply stringifying it as `"__LINE__"`.
 *
 * @param ARG_function
 *        See FLOW_UTIL_WHERE_AM_I_LITERAL().
 * @param ARG_line
 *        Literally `__LINE__`.
 */
#define FLOW_UTIL_WHERE_AM_I_LITERAL_IMPL_OUTER(ARG_function, ARG_line) \
  FLOW_UTIL_WHERE_AM_I_LITERAL_IMPL_INNER(ARG_function, ARG_line)

/**
 * Impl helper macro from FLOW_UTIL_WHERE_AM_I_LITERAL().  This is the real impl; see also
 * FLOW_UTIL_WHERE_AM_I_LITERAL_IMPL_OUTER().
 *
 * @param ARG_function
 *        See FLOW_UTIL_WHERE_AM_I_LITERAL().
 * @param ARG_line
 *        The line number integer as from `__LINE__`.
 */
#define FLOW_UTIL_WHERE_AM_I_LITERAL_IMPL_INNER(ARG_function, ARG_line) \
  __FILE__ ": " #ARG_function "(" #ARG_line ")"
