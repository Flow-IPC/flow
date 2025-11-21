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

#include "flow/util/string_view.hpp"
#include "flow/common.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/unordered_set.hpp>

/// @cond
// -^- Doxygen, please ignore the following.

/* This strangeness is a work-around a Boost bug which manifests itself at least in g++ and clang++ in
 * C++11 mode, when trying to use ratio<>::num and ::den.  Without this, those two symbols, per each ratio<>
 * type directly used, each get an undefined reference linker error.  See https://svn.boost.org/trac10/ticket/13191,
 * not fixed as of this writing.  @todo: Revisit. */
#ifndef BOOST_NO_CXX11_CONSTEXPR
template <boost::intmax_t N, boost::intmax_t D>
const boost::intmax_t boost::ratio<N, D>::num;
template <boost::intmax_t N, boost::intmax_t D>
const boost::intmax_t boost::ratio<N, D>::den;
#endif

// -v- Doxygen, please stop ignoring.
/// @endcond

namespace flow::util
{

// Types.

/* (The @namespace line shouldn't be needed, but there must be some Doxygen bug causing that doc header to get
 * merged into the succeeding function's.  Who knows?  @todo Look into it more in copious spare time.
 *
 * Update: That was with Doxygen 1.8.11 through 1.8.15.  In 1.8.16, it's still true; plus another bug appeared:
 * The first sentence (up to the first period) is supposed to interpreted, with certain Doxygen config,
 * as @brief, meaning the brief summary of the namespace; this feature is used constantly, not just in this
 * spot.  Well, for some reason that Doxygen version took the @brief to be blank (shows up as blank in the
 * namespaces list page and the namespace page itself), whereas before it was fine.  Since it's not happening
 * for anything else I made it an explicit @brief here and called it a day; I guess the Doxygen parser is
 * getting doubly confused; not sure.  @todo Sigh, untangle this at some point; maybe Doxygen gets fixed;
 * file bug against Doxygen.  This is the only (documented) namespace alias in the code, as of this writing, so
 * maybe that is why it is "special.") */

/**
 * @namespace flow::util::bind_ns
 *
 * @brief Hack namespace alias to enable use of `bind` and related utilities like `cref` in the face of strange observed
 * compiler behavior.
 *
 * The code in the past used `bind` and its helpers `cref` and `ref` fairly extensively.  (Vast majority of `bind`s are
 * now replaced with superior uses of lambdas, with the upgrade to C++1x.  So it's far less extensive than in past.)
 * However, there is something odd going on with namespace resolution for
 * these guys.  At least in the clang version used as of this writing, `bind` somehow
 * works without qualification, as does `cref` (and thus probably `ref`); I really don't know why.  I'd like to use
 * the `boost::` versions, though, specifically (for stylistic reasons explained elsewhere).  More importantly, I don't
 * want to use something that mysteriously works without qualification -- is `std::` somehow being forced with some
 * phantom `using namespace` from some evil header somewhere?  I am not sure as of this writing.  In any case,
 * this allows one to just say `using util::bind_ns::bind` and then use `bind()` without qualification in that
 * block (typically function/method) without worrying that some weird unqualified guy will be used.
 *
 * Caveat 1: I don't know why, but in the aforementioned clang, `using util::bind_ns::cref` makes `cref()` become
 * ambiguous with some `std::cref`; it's quite unclear why the same complaint then doesn't happen for `bind()` itself.
 * In that case, when an error is encountered, get rid of the `using` and instead qualify it as `util::bind_ns::cref()`.
 * It sucks, but at least it's clear.
 *
 * Caveat 2: I haven't built in MS Visual Studio in quite a while (though it did use to work at one point), but back
 * then I remember some VC++-specific trouble with `bind()` resolution.  Possibly it's related to the above.
 * Anyway, use `bind_ns` as needed to get around any such compile errors.
 *
 * Note: Knowing of this phantom `bind()` that's magically available sans qualification, I also tried to use
 * unqualified `cout` and `endl` in the same context but thankfully got the expected errors in clang saying
 * these identifiers do not exist but suggesting the use of `std::cout` and `std::endl`.  So, at least it's not
 * all of `std` being forced on the code.
 *
 * @todo Investigate deeply the cause of the strange behavior with unqualified `bind()` and others described
 * above this line in the source code.
 */
namespace bind_ns = boost;

// Find doc headers near the bodies of these compound types.

template<typename Key, typename Iterator, bool IS_ITER_TO_PAIR>
class Linked_hash_key;
template<typename Hash>
class Linked_hash_key_hash;
template<typename Pred>
class Linked_hash_key_pred;

/**
 * The lookup structure used inside Linked_hash_map and Linked_hash_set.  See the former's doc header(s).
 *
 * @tparam Key
 *         See Linked_hash_map, Linked_hash_set.
 * @tparam Iterator
 *         Linked_hash_map::Iterator or Linked_hash_set::Iterator.
 * @tparam Hash
 *         See Linked_hash_map, Linked_hash_set.
 * @tparam Pred
 *         See Linked_hash_map, Linked_hash_set.
 * @tparam IS_ITER_TO_PAIR
 *         `true` for Linked_hash_map, `false` for Linked_hash_set.
 */
template<typename Key, typename Iterator, typename Hash, typename Pred, bool IS_ITER_TO_PAIR>
using Linked_hash_key_set = boost::unordered_set<Linked_hash_key<Key, Iterator, IS_ITER_TO_PAIR>,
                                                 Linked_hash_key_hash<Hash>,
                                                 Linked_hash_key_pred<Pred>>;
// Free functions.

/**
 * Helper that takes a non-negative duration of arbitrary precision/period and converts it to
 * #Fine_duration, rounding up.  Also, the input type's `max()` is translated to `Fine_duration::max()`.
 *
 * @tparam Rep
 *         See `boost::chrono::duration` documentation.
 * @tparam Period
 *         See `boost::chrono::duration` documentation.
 * @param dur
 *        A non-negative duration (or assertion trips).
 * @return See above.
 */
template<typename Rep, typename Period>
Fine_duration chrono_duration_to_fine_duration(const boost::chrono::duration<Rep, Period>& dur);

/**
 * Helper that takes a non-negative duration of arbitrary precision/period and converts it to
 * #Fine_duration, rounding up; then adds it to `Fine_clock::now()` and returns the result.
 * Also, the input type's `max()` is translated to `Fine_time_pt{}` (Epoch-valued -- i.e., zero-valued -- time point).
 *
 * @tparam Rep
 *         See `boost::chrono::duration` documentation.
 * @tparam Period
 *         See `boost::chrono::duration` documentation.
 * @param dur
 *        A non-negative duration (or assertion trips).
 * @return See above.
 */
template<typename Rep, typename Period>
Fine_time_pt chrono_duration_from_now_to_fine_time_pt(const boost::chrono::duration<Rep, Period>& dur);

/**
 * Helper for FLOW_UTIL_WHERE_AM_I() that, given a pointer/length of a string in memory containing a path, returns
 * a pointer/length into that same buffer that comprises the postfix
 * just past the last directory separator or (if none exists) to all of it.
 *
 * ### Rationale for taking and returning #String_view this way ###
 * There are 2 other alternatives.  One is essentially to take `std::string` (or #String_view) and return new
 * `std::string` (or some variation).  That is un-great, because the main use case is to use this at log call sites,
 * and creating temporary `std::string`s is best avoided; the ability to simply return a pointer into existing memory,
 * -- particularly since it is likely to come from `__FILE__` (which is in static memory) -- might represent
 * significant perf savings.  So that leaves either the alternative we chose or a variation (which, historically, used
 * to be the case) which is to take a `const char*` and return same, into that memory.  The latter is an acceptable
 * approach.  #String_view, by comparison, does involve storing an extra bit of data, namely the length stored inside
 * #String_view.  We stipulate this is negligible in terms of RAM and copying costs when compared to doing same with
 * the pointers alone... but what's the advantage?  Answer: it may be small; but in order to search for the right-most
 * dir separator character, one needs to internally find the NUL (a/k/a `strlen()`); with #String_view it is already
 * available, so the linear search is unnecessary.  Doesn't it cost cycles to set the length though? you ask.
 * Answer: Not if one constructs the `#String_view`s involved cleverly.  To wit:
 *
 * ### Instructions on how to best construct `String_view path` from `__FILE__` ###
 * For max performance, do it in the same way as recommended when setting log::Msg_metadata::m_msg_src_file.
 * Spoiler alert: Use `sizeof(__FILE__)` to obtain a compile-time length.
 *
 * ### `constexpr` ###
 * Update: This function is now `constexpr`, so `path` must be known at compile-time; hence the above instructions
 * (while still valid) apply in a different way.  The raw performance doesn't necessarily matter (since it's "executed"
 * by the compiler now).  However, following those instructions (1) will help make `constexpr`ness easier to achieve
 * (as the NUL-terminated #String_view ctor is less likely to be effectively `constexpr` depending on compiler; while
 * the pointer+length ctor is more likely so); and (2) is good for perf in case `constexpr`ness is
 * dropped in the future.  It's less fragile is the point.  Anyway, just do it!
 *
 * @param path
 *        String containing a path from a `__FILE__` macro.  See instructions above for max perf.
 * @return See above.
 */
constexpr String_view get_last_path_segment(String_view path);

/**
 * Helper for FLOW_UTIL_WHERE_AM_I(), etc., that, given values for source code file name, function, and line number,
 * returns an `std::string` equal to what FLOW_UTIL_WHERE_AM_I() would place into an `ostream`.
 *
 * For rationale of using #String_view instead of `const char*` for the string args see similar reasoning in
 * get_last_path_segment() doc header.  See also practical instructions on how to performantly obtain these args, in
 * the doc header for `ARG_file` for FLOW_UTIL_WHERE_AM_I_FROM_ARGS().
 *
 * @param file
 *        See `ARG_file` in FLOW_UTIL_WHERE_AM_I_FROM_ARGS().
 * @param function
 *        Full function name, as from `__FUNCTION__`.  See instructions above for max perf.
 * @param line
 *        Line number, as from `__LINE__`.
 * @return String `X` (suitable for, for example: `std::cout << X << ": Hi!"`).
 */
std::string get_where_am_i_str(String_view file, String_view function, unsigned int line);

} // namespace flow::util
