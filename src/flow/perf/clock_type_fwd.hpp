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

#include "flow/common.hpp"
#include <bitset>
#include <ostream>

namespace flow::perf
{
// Types.

/**
 * Short-hand for a high-precision boost.chrono point in time, formally equivalent to flow::Fine_time_pt.
 * The alias exists 1/2 for brevity, 1/2 to declare the standardly-used time point type in flow::perf Flow module.
 */
using Time_pt = Fine_time_pt;

/**
 * Short-hand for a high-precision boost.chrono duration, formally equivalent to flow::Fine_duration.
 * The alias exists 1/2 for brevity, 1/2 to declare the standardly-used duration type in flow::perf Flow module.
 */
using Duration = Fine_duration;

/**
 * The raw type used in #Duration to store its clock ticks.  It is likely `int64_t`, but try not to rely on that
 * directly.
 *
 * ### Rationale ###
 * Useful, e.g., in `atomic<duration_rep_t>`, when one wants to perform high-performance operations like
 * `+=` and `fetch_add()` on `atomic<>`s: these do not exist for `chrono::duration`, because the latter is not an
 * integral type.
 */
using duration_rep_t = Duration::rep;

/**
 * Clock types supported by flow::perf module facilities, perf::Checkpointing_timer in particular.
 * These are used, among other things, as array/`vector` indices and therefore numerically equal 0, 1, ....
 * Clock_type::S_END_SENTINEL is an invalid clock type whose numerical value equals the number of clock types
 * available.
 *
 * @todo Consider adding a system-calendar-clock (a/k/a POSIX time) type to perf::Clock_type.  It would be a cousin
 * of Clock_type::S_REAL_HI_RES.  It would certainly be inferior in terms of resolution/monotonicity/etc., and one would
 * think `S_REAL_HI_RES` would always be preferable.  Nevertheless it would be interesting to "officially" see its
 * characteristics including in particular (1) resolution and (2) its own perf cost especially vs. `S_REAL_HI_RES` which
 * we know is quite fast itself.  This may also help a certain to-do listed as of this writing in the doc header
 * of flow::log FLOW_LOG_WITHOUT_CHECKING() (the main worker bee of the log system, the one that generates each log
 * time stamp).
 */
enum class Clock_type : size_t
{
  /**
   * Measures real time (not processor time), using the highest-resolution system clock available that guarantees
   * steady, monotonic time passage with no discontinuities.  In POSIX `"::clock_gettime(CLOCK_MONOTONIC)"` is the
   * underlying native mechanism as of this writing.
   *
   * ### Obervations, informal suggestions ###
   * Of all clocks observed so far, it has the best resolution
   * and also is the cheapest computationally itself.  However, it measures real time, so (for example) another
   * thread or process pegging the processor concurrently can affect the time being measured.  That, in particular,
   * is not necessarily a problem in test rigs; but even so it cannot measure (for example) how costly one thread
   * is over another; nor does it separate idle time from computing time from I/O time from....
   *
   * Due to the high resolution and low computational cost, one should strive to use this clock whenever possible;
   * but it is not always possible (as additional info may be required, as outlined just above).
   */
  S_REAL_HI_RES = 0,

  /**
   * Along with `S_CPU_SYS_LO_RES`, measures processor time (user-level) spent by the current *process*;
   * this is the lower-resolution timer facility as in the `time` POSIX command.  In POSIX `"::times()"` is the
   * underlying native mechanism as of this writing.
   *
   * ### Obervations, informal suggestions ###
   * `S_CPU_TOTAL_HI_RES` is the superior processor-time clock; we have observed it to be equally computationally
   * expensive but delivering higher-resolution results.  However, it doesn't provide the classic user-kernel
   * split the way `S_CPU_SYS_LO_RES` and `S_CPU_USER_LO_RES` do together.
   *
   * See discussion on `S_CPU_TOTAL_HI_RES` regarding when/if to use process-time clocks.
   */
  S_CPU_USER_LO_RES,

  /// Counterpart of `S_CPU_USER_LO_RES`, measuring processor time (kernel-level) spent by the current *process*.
  S_CPU_SYS_LO_RES,

  /**
   * Measures processor time (user- and kernel-level total) spent by the current *process*;
   * this is the higher-resolution process-time facility.  In POSIX `"::clock_gettime(CLOCK_PROCESS_CPUTIME_ID)"` is the
   * underlying native mechanism as of this writing.
   *
   * ### Obervations, informal suggestions ###
   * Firstly see `S_REAL_HI_RES` and ensure that one isn't sufficient for your needs, as it is much more accurate and
   * computationally cheaper by 1-2 orders of magnitude based on our observations in Mac and Linux runs.
   *
   * Processor time actually measures processor *cycles* being spent to make computations.  (I/O ops and idle time
   * are not counted.)  Every cycle spent by any processor core is either charged to this process or another process;
   * if the former then it's counted; otherwise it isn't.  Next, the cycle count is multiplied by its standard
   * constant time duration (which is based directly on the clock frequency, the GHz thing).  That is the result.
   * Multiple threads acting concurrently would all count if present, so remember that.  Further, it is apparently
   * not straightforward what the system will charge to process A vs. process B.  For this reason, processor-time
   * results of very short operations (on the order of, like, a few system calls, say) are notoriously inconsistent:
   * *you should strive to measure longer operations, or operation repeated many times in a row*.  This stands in
   * stark contrast to `S_REAL_HI_RES` which -- concurrent processor pegging aside (can usually be avoided in test
   * rigs) -- tends to be both accurate and consistent.  In addition, the get-time call itself can be relatively
   * expensive and can affect the overall efficiency of an algorithm even as one tries to measure its different parts
   * (again: avoid such problems by measuring longer things).
   *
   * See also `S_CPU_THREAD_TOTAL_HI_RES` if one desires per-thread timing as opposed to per-process.
   */
  S_CPU_TOTAL_HI_RES,

  /**
   * Similar to `S_CPU_TOTAL_HI_RES` but applied to the calling thread as opposed to entire process.
   * In POSIX `"::clock_gettime(CLOCK_THREAD_CPUTIME_ID)"` is the underlying native mechanism as of this writing.
   *
   * ### Obervations, informal suggestions ###
   * See `S_CPU_TOTAL_HI_RES`, as those comments apply equally here but on the finer-grained level of a thread as
   * opposed to (potentially multi-threaded) process.  Please note that formally behavior is undefined if one
   * tries to form a duration from two thread-time points A and B via subtraction, if A and B were obtained from
   * different threads.  Informally, the domains of the two respective threads are completely unrelated, so a
   * difference obtained from them is meaningless and can easily be, say, negative, even though time has advanced.
   */
  S_CPU_THREAD_TOTAL_HI_RES,

  /**
   * Final, invalid clock type; its numerical value equals the number of clocks currently supported.
   *
   * @internal
   *
   * If you add or remove clock types, other places (in addition to this `enum`) to change are self-explanatory;
   * plus update Checkpointing_timer::real_clock_types() and nearby functions.
   */
  S_END_SENTINEL
}; // enum class Clock_type

/**
 * Short-hand for a bit-set of N bits which represents the presence or absence of each of the N possible clock types
 * in perf::Clock_type.  This is what we use to represent such things, as it is more compact and (we suspect) faster
 * in typical operations, especially "is clock type T enabled?".
 *
 * If C is a `Clock_types_subset`, and T is a `Clock_type`, then bit C[size_t(T)] is `true` if and only if
 * T is in C.
 *
 * Potential gotcha: bit-sets are indexed right-to-left (LSB-to-MSB); so if the 0th (in `enum`) clock type
 * is enabled and others are disabled, then a print-out of such a Clock_types_subset would be 0...0001, not 1000...0.
 * So watch out when reading logs.
 */
using Clock_types_subset = std::bitset<size_t(Clock_type::S_END_SENTINEL)>;

/* Normally Duration_set and Time_pt_set would be forward-declared here in _fwd.hpp and defined in a .hpp; instead
 * it is simply defined here in _fwd.hpp, and there is no non-_fwd.hpp.  This is in accordance with the list of
 * known reasonable exceptions to the _fwd.hpp pattern; namely since these are glorified aliases for
 * simple array<>s, only made as actual `struct`s to be able to provide a small free-function API around them. */

/**
 * Convenience wrapper around an `array<Duration, N>`, which stores a duration for each of the N possible
 * clock types in perf::Clock_type.  It is a completely tight wrapper, not even adding zeroing of the array values
 * on construction.  It only adds convenience operations (possibly as free functions), namely some common math
 * (such as scaling all N values by some numeric factor) and stream I/O.
 *
 * ### Design rationale ###
 * Performance is absolutely key, so this is to be identical to the underlying (and publicly accessible)
 * `array` in terms of data storage and operations.  I wrote this `struct` to avoid overloading things like
 * `operator-(a, b)` and `operator<<(ostream&, x)`, when the underlying type of `a`, `b, `x` is
 * a standard `array`: we don't want to overload such operations on a general type, even if it's only in our
 * namespace.  Historically, the first cut simply made non-operator-overload free function versions of such things
 * operating directly on the `array`s.
 */
struct Duration_set
{
  /// The underlying data, directly readable and writable by user.  NOT ZERO-INITIALIZED ON CONSTRUCTION (or ever).
  std::array<Duration, size_t(Clock_type::S_END_SENTINEL)> m_values;
};

/**
 * Convenience wrapper around an `array<Time_pt, N>`, which stores a time point for each of the N possible
 * clock types in perf::Clock_type.  All discussion on Duration_set applies here equally.
 */
struct Time_pt_set
{
  /// The underlying data, directly readable and writable by user.  NOT ZERO-INITIALIZED ON CONSTRUCTION (or ever).
  std::array<Time_pt, size_t(Clock_type::S_END_SENTINEL)> m_values;
};

// Free functions.

/**
 * Prints string representation of the given clock type `enum` value to the given `ostream`.
 *
 * @param os
 *        Stream to which to write.
 * @param clock_type
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, Clock_type clock_type);

/**
 * Prints string representation of the given Duration_set value to the given `ostream`.
 *
 * @relatesalso Duration_set
 *
 * @param os
 *        Stream to which to write.
 * @param duration_set
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Duration_set& duration_set);

/**
 * Advances each `Duration` in the target Duration_set by the given respective addend `Duration`s (negative Duration
 * causes advancing backwards).
 *
 * @relatesalso Duration_set
 *
 * @param target
 *        The set of `Duration`s each of which may be modified.
 * @param to_add
 *        The set of `Duration`s each of which is added to a `target` `Duration`.
 * @return Reference to mutable `target` to enable standard `+=` semantics.
 */
Duration_set& operator+=(Duration_set& target, const Duration_set& to_add);

/**
 * Scales each `Duration` in the target Duration_set by the given numerical constant.
 *
 * @note If you plan to use division as well, always first multiply, then divide, to avoid rounding errors
 *       (assuming overflow is not a possibility).
 *
 * @todo Maybe allow `operator*=(Duration_set)` by a potentially negative number; same for division.
 *
 * @relatesalso Duration_set
 *
 * @param target
 *        The set of `Duration`s each of which may be modified.
 * @param mult_scale
 *        Constant by which to multiply each `target` `Duration`.
 * @return Reference to mutable `target` to enable standard `*=` semantics.
 */
Duration_set& operator*=(Duration_set& target, uint64_t mult_scale);

/**
 * Divides each `Duration` in the target Duration_set by the given numerical constant.
 *
 * @note If you plan to user multiplication as well, always first multiply, then divide, to avoid rounding errors
 *       (and assuming overflow is not a possibility).
 *
 * @relatesalso Duration_set
 *
 * @param target
 *        The set of `Duration`s each of which may be modified.
 * @param div_scale
 *        Constant by which to divide each `target` `Duration`.
 * @return Reference to mutable `target` to enable standard `/=` semantics.
 */
Duration_set& operator/=(Duration_set& target, uint64_t div_scale);

/**
 * Returns a Duration_set representing the time that passed since `from` to `to` (negative if `to` happened earlier),
 * for each `Clock_type` stored.
 *
 * @relatesalso Time_pt_set
 *
 * @param to
 *        The minuend set of time points.
 * @param from
 *        The subtrahend set of time points.
 * @return See above.
 */
Duration_set operator-(const Time_pt_set& to, const Time_pt_set& from);

/**
 * Advances each `Time_pt` in the target Time_pt_set by the given respective addend `Duration`s (negative `Duration`
 * causes advancing backwards).
 *
 * @param target
 *        The set of `Time_pt`s each of which may be modified.
 * @param to_add
 *        The set of `Duration`s each of which is added to a `target` `Time_pt`.
 * @return Reference to mutable `target` to enable standard `+=` semantics.
 */
Time_pt_set& operator+=(Time_pt_set& target, const Duration_set& to_add);

} // namespace flow::perf
