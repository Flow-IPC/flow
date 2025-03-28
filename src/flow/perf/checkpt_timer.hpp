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

#include "flow/perf/perf_fwd.hpp"
#include "flow/perf/clock_type_fwd.hpp"
#include "flow/log/log.hpp"
#include <boost/chrono/process_cpu_clocks.hpp>
#include <string>
#include <vector>

namespace flow::perf
{

/**
 * The central class in the `perf` Flow module, this efficiently times the user's operation, with a specified subset
 * of timing methods; and with the optional ability to time intermediate *checkpoints* within the overall operation.
 *
 * ### How to use ###
 * To use this: Construct a Checkpointing_timer; perform the operation in question; then access results via accessors;
 * including in particular the string/stream output operation that lays out results in human-friendly fashion.
 * Optionally, call checkpoint() to mark down intermediate timing results, so that the operation can be broken down
 * into its component parts (the string/stream output will include these).  This is optional, except that at least
 * 1 checkpoint is required: the last checkpoint represents the end of the measured operation.
 *
 * The actual measurements are performed by sampling a time stamp from *each* of N clocks, where N <= M, and M is the
 * number of values in perf::Clock_type `enum`.  The N clocks are specified at construction by user; thus can measure
 * just 1, in particular, as is often desired.  This is a critical and potentially subtle decision; see doc headers
 * of each perf::Clock_type member for tips.
 *
 * ### Performance ###
 * This is a performance-measuring facility, and the performance of a performance-measuring-thing is quite important,
 * especially since some of the things being timed can be very quick (single-digit microseconds potentially).
 *
 * Extreme care has been taken to keep the computational fat of all the book-keeping in the class very low
 * (and empirical testing shows this was successful).  In the cases where user cooperation is necessary to avoid undoing
 * this, the doc headers point it out.  Please read docs carefully to avoid timing-technique traps that might lead to
 * wrong conclusions and frustration.
 *
 * ### Thread safety ###
 * Checkpointing_timer itself is not thread-safe for non-`const` access to an object while any other access occurs
 * concurrently.  (Internally, there is no locking.)  However, the nature of how one uses a timer object is that
 * one does stuff to be measured, checkpoint(), more stuff, checkpoint(), ..., final checkpoint().  In other words,
 * by its very nature, it expects only sequential non-`const` calls while measuring times -- so thread safety
 * should not come up.  (However, thread safety enters the picture with Checkpointing_timer::Aggregator; see below.)
 *
 * Aggregation
 * -----------
 * `Duration` result *aggregation* is the common-sense idea that to accurately measure the typical length of operation X
 * is to repeat X many times (N times) and then view a sum-over-N-samples and/or mean-over-N-samples and/or
 * mean-over-N-samples-scaled-times-M, where M is usually some convenience constant like 1,000,000.  There are
 * two patterns we provide/suggest to accomplish aggregation.
 *
 * ### Lowest-overhead aggregation approach: Single Checkpointing_timer ###
 * Suppose you have operation X and are *not* interested in subdividing it into checkpoints; you only care about the
 * total duration(s) (according to which `Clock_type`s interest you).  Then the following technique leads to the
 * lowest possible overhead:
 *
 *   ~~~
 *   flow::perf::Checkpointing_timer sum_timer(logger, "op name", which_clocks, 1); // 1 checkpoint only.
 *   const unsigned int n_samples = 1000000;
 *   for (unsigned int sample_idx = 0; sample_idx != n_samples; ++sample_idx)
 *   {
 *     // ...Operation X being measured goes here.
 *   }
 *   sum_timer.checkpoint("total"); // Mark down the total time taken.
 *   // Log the total duration(s), one per clock type!
 *   flow::perf::Checkpointing_timer::Aggregator::log_aggregated_result_in_timer(sum_timer, n_samples, false);
 *   // And/or: Log mean duration(s), times N_SAMPLES_SCALE_CONVENIENCE, one per clock type!
 *   {
 *     // Just make a copy of the raw sum, then scale it x N_SAMPLES_SCALE_CONVENIENCE / n_samples.
 *     auto mean_scaled_timer(sum_timer);
 *     mean_scaled_timer.scale(N_SAMPLES_SCALE_CONVENIENCE, n_samples);
 *     flow::perf::Checkpointing_timer::Aggregator::log_aggregated_result_in_timer(mean_scaled_timer, n_samples,
 *                                                                                 true, N_SAMPLES_SCALE_CONVENIENCE);
 *   }
 *   ~~~
 *
 * This involves minimal overhead, as no timing calls occur at all until X is repeated a million times.
 *
 * ### Cross-product aggregation approach: Multiple `Checkpointing_timer`s + Checkpointing_timer::Aggregator ###
 * If, by contrast, you want to divide X into checkpoints and see (say) how long checkpoint 3 takes on average
 * (as well as the total as above), then use the dedicated Checkpointing_timer::Aggregator class for that purpose.
 * This approach also allows one to do stuff (possibly unrelated stuff) between the many repetitions of operation X.
 * Only X itself will be timed.
 *
 * To use `Aggregator`, construct one; then simply create a Checkpointing_timer for each repetition of X and time it
 * as normal.  Before or after each repetition, register it inside the agg using Aggregator::aggregate() which takes
 * a pointer to Checkpointing_timer.  The only requirement is that the number and respective names of the checkpoints
 * in each Checkpointing_timer are the same for all of them.
 *
 * Once enough samples (`n_samples` in earlier example) have been thus collected, Aggregator::log_aggregated_results()
 * will log every potential form (sum, mean, scaled-mean), not just for the total operation X but also
 * each checkpoint().  For example, if X consists of steps X1, X2, X3, then one would see the mean duration of X1,
 * X2, and X3 individually and their means would add up to the mean for all of X.
 *
 * This is quite powerful; and while the overhead of creating individual Checkpointing_timer objects, say, *might*
 * affect the overall performance of the algorithm involving X, it shouldn't have any direct effect on the values
 * actually being measured, since only each X invocation itself is measured; stuff in-between (slow or not) isn't.
 * However:
 *   - Checkpointing_timer::Aggregator::aggregate() isn't thread-safe for concurrent access (so if that's relevant,
 *     you'll need a mutex lock around the call).
 *   - Always ensure you understand the structure of your measurement plan as it relates to concurrency.
 *     For example, it's meaningless to measure processor-time cost of X being done repeatedly, if various incarnations
 *     of X might be performed partially concurrently (say, if X = handling 1 HTTP request in a multi-threaded server
 *     under load).  In that case processor-times will overlap and produce essentially meaningless (at the very least
 *     misleading) results.  On the other hand, real-time measurements (Clock_type::S_REAL_HI_RES) make plenty of sense
 *     in that situation and would indeed reliably measure latency.  Thread-time measurements
 *     (Clock_type::S_CPU_THREAD_TOTAL_HI_RES) can also make sense.  Just saying: do not assume all clock types
 *     make sense in the face of concurrent operations; plan mindfully.
 *
 * @see Checkpointing_timer::Aggregator.
 *
 * @internal
 *
 * Implementation/design notes
 * ---------------------------
 * On the one hand we wanted to be able to measure multiple clock types; and since at least some of them are
 * mutually complementary (Clock_type::S_CPU_USER_LO_RES and Clock_type::S_CPU_SYS_LO_RES happen to measure the
 * user + kernel components of overall processor-time) they need to be sampled simultaneously.  On the other hand
 * we know clock sampling itself can take non-trivial time; so it must be possible to specify the clocks of interest.
 *
 * Also, we wanted to be able to programmatically and/or via logs provide convenient human access to the results,
 * including optional ability to time the various parts of a multi-step operation and then see which part took how
 * long; a/k/a checkpoints.
 *
 * On the other hand it all needs to be maximally fast, adding very low overhead to the operation being measured.
 * The way I thought of this was considering the absolute bare minimum of measuring durations: one simply samples the
 * clock and records it somehow very fast (stack variable perhaps); then at the end one can take time point differences
 * and log some durations.  This is the minimum; so every computational addition to that must be considered for impact.
 * In the end, it was successful; as measured by actually raw-timing the timing facilities.  How is this accomplished?
 *
 *   - The core operation is checkpoint(), when a final or intermediate time point is sampled; so that's the thing
 *     that must be fast.
 *   - Each sample is recorded in a Duration_set which is a *statically* sized array of N `Duration` values;
 *     and each Duration (chrono.duration) is a simple nanosecond (or whatever is the unit of flow::Fine_clock, but
 *     let's say nanoseconds) count; N is the # of clocks available.
 *   - The samples are recorded in a simple `vector<>`, so basically it is an array of nanosecond (or whatever)
 *     counts, with no pointer indirection inside each element.
 *     - Moreover, the `vector<>` is preallocated to be large enough to store the max # of checkpoints.
 *       The user specifies this maximum at construction, guaranteeing (re-)allocation never occurs while stuff is
 *       being timed.  In other words checkpoint() -- when it gets down to the low level of what is actually records --
 *       places the sampled nanosecond count (for each clock) into a preallocated buffer whose address/size doesn't
 *       change, at the address that's simply `i * sizeof(<N nanosecond counts tightly packed>)`, where `i` is the
 *       count of checkpoints already recorded.
 *   - In a nod to the goal of making things human-readable, we add one more thing into the data structures explained
 *     so far.  Each checkpoint (of which there is at least 1, the final one indicating total elapsed time) therefore
 *     has a string name supplied by user in checkpoint() call.  Hence instead of storing the `Duration_set`s
 *     directly in the array, instead a Checkpoint `struct` wraps the Duration_set and name; but this doesn't add any
 *     per-sample pointer indirection, so in term of performance it only increases the above `sizeof()` to (internally)
 *     also include the `std::string` stuff (probably `char*` pointer and a couple `size_t`s).
 *     - Using move semantics, the user's `std::string` is not even copied when it is recorded; only the internal
 *       `string` buffer pointer and sizes are copied.
 *     - However, the user must form the `string` in the first place!  This is a potential gotcha albeit only when
 *       one times very short operations.  I actually performed analysis of the cost of this.  The outcome is as
 *       follows:
 *       - A nearly-constant checkpoint name (like this: `T.checkpoint(string("sample"));`) is essentially free.
 *       - A dynamically computed checkpoint name, using an improved version of `ostringstream` -
 *         `T.checkpoint(util::ostream_op_string("sample", sample_idx));` - has some cost: ~0.5 microseconds
 *         per sample (MacBook Pro 2015).  Whether this matters or not depends on the op one is measuring, but
 *         in my experience it's significant only for the most subtly-tiny ops.  In any case, the official doc header
 *         for checkpoint() makes suggestions accordingly.  (Spoiler alert: Ultimately - if necessary - one can always
 *         use a constant checkpoint name, or construct a dynamic name via ultra-fast C-style `char*` direct-writing.)
 *
 * The bottom line is, there's very low overhead added, as long as the user mindfully provides the max # of checkpoints
 * at construction; and doesn't waste time on fancily naming the checkpoints.
 *
 * @todo `const std::string&` to `String_view` in all of flow::perf where applicable.  Look into `string&&` also.
 */
class Checkpointing_timer :
  public log::Log_context
{
public:

  // Types.

  class Aggregator; // Fwd.

  /**
   * The entirety of the information recorded with each Checkpointing_timer::checkpoint() call;
   * it contains the `Duration` values (for each `Clock_type`) with the time elapsed since either
   * the preceding `T.checkpoint()` or construction.
   *
   * User doesn't usually need to access these, since the string/stream/log facilities of Checkpointing_timer and
   * Checkpointing_timer::Aggregator would already print out the info; but this provides programmatic access if desired.
   */
  struct Checkpoint
  {
    /// User-supplied name of the checkpoint.
    const std::string m_name;
    /**
     * For the `T.checkpoint()` call that generated `*this`, for each enabled `Clock_type`: Time elapsed since either
     * the preceding `T.checkpoint()` or (if it's the first `T.checkpoint()`) since `T` construction.
     * Zero is recorded for each disabled `Clock_type`.
     */
    Duration_set m_since_last;
  };

  // Constructors/destructor.

  /**
   * Constructs a new timer and immediately begins measuring time, in that the next checkpoint() call will record
   * the time passed since right now.  Please read the following docs to achieve low overhead.
   *
   * @param name_moved
   *        Nickname for the timer, for logging and such.  If you want to preserve your string, pass in a copy:
   *        `string(your_name_value)`.
   * @param which_clocks
   *        The clocks you want sampled in each checkpoint().  Please carefully read the doc headers of all
   *        perf::Clock_type `enum` members.
   * @param max_n_checkpoints
   *        The number of times you shall call checkpoint() shall not exceed this.
   *        A tight value saves RAM; but more importantly a *correct* upper bound guarantees no internal reallocation
   *        will occur in checkpoint() which is important for maintaining low overhead.
   *        `assert()` trips if one breaks this promise.
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   */
  explicit Checkpointing_timer(log::Logger* logger_ptr, std::string&& name_moved,
                               Clock_types_subset which_clocks, size_t max_n_checkpoints);

  /**
   * Constructs a timer that is identical to but entirely independent of the source object.
   * When performed outside of performance-critical sections, this is very useful for data massaging *after*
   * having completely measured something in a Checkpointing_timer.  E.g., one can create a copy and then
   * scale() it to obtain a mean, etc.
   *
   * @see Checkpointing_timer doc header for discussion of aggregation approaches.
   *
   * @param src
   *        Object to copy.
   */
  explicit Checkpointing_timer(const Checkpointing_timer& src) = default; // Yes, do allow EXPLICIT copy construction.

  /// For now at least there's no reason for move-construction.
  Checkpointing_timer(Checkpointing_timer&&) = delete;

  // Methods.

  /// Disallow overwriting.  Copy-construction exists for a specific pattern to be possible; no need for this so far.
  void operator=(const Checkpointing_timer&) = delete;

  /// No copy assignment; no move assignment either.
  void operator=(Checkpointing_timer&&) = delete;

  /**
   * Returns the bit-set containing only those `Clock_type`s enabled that measure passage of real (wall clock) time.
   *
   * ### Rationale ###
   * One application is when deciding on the constructor `which_clocks` arg (also a `Clock_types_subset`).
   * If one wants to time some operation in such a way as to make only real-time clocks make sense, and the user is
   * allowed to control which clock types to sample, then one can do:
   *
   *   ~~~
   *   // They can specify a bitmask the_clock_set_from_config; then we potentially cull that further to exclude all
   *   // but the real-time clocks, because we feel only those make sense in our timing context.
   *   const auto which_clocks = Checkpointing_timer::real_clock_types() & the_clock_set_from_config;
   *   Checkpointing_timer timer(..., which_clocks, ...);
   *   // The timing has begun....
   *   ~~~
   *
   * @see perf::Clock_type
   *
   * @return See above.
   */
  static Clock_types_subset real_clock_types();

  /**
   * Returns the bit-set containing only those `Clock_type`s enabled that measure passage of processor time by all
   * threads of the calling process combined.
   *
   * ### Rationale ###
   * See real_clock_types().
   *
   * @see perf::Clock_type
   * @return See above.
   */
  static Clock_types_subset process_cpu_clock_types();

  /**
   * Returns the bit-set containing only those `Clock_type`s enabled that measure passage of processor time by the
   * calling thread.
   *
   * ### Rationale ###
   * See real_clock_types().
   *
   * @see perf::Clock_type
   * @return See above.
   */
  static Clock_types_subset thread_cpu_clock_types(); // Each clock measures current thread's CPU cycles.

  /**
   * Records a checkpoint, which takes a sample of all enabled `Clock_type`s' clocks and records the corresponding
   * duration(s) since the last checkpoint() or (if none) since construction.
   *
   * In practice, at least one checkpoint() call is required for `*this` to be of any use at all.  The final
   * checkpoint() call's sample(s) determine the current value of since_start() which is the duration of the entire
   * operation, as measured since `*this` construction.  Intermediate calls (ones preceding the final one)
   * are optional and represent intermediate steps and how long they each took.
   *
   * since_start() depends on exactly two things: when `*this` was constructed, and when the last checkpoint() call
   * (at that time) was made.
   *
   * ### Naming vs. performance ###
   * `name_moved` will be taken via move semantics, meaning no string buffer will be copied, only the pointer(s)
   * and buffer size scalar(s) inside `std::string`.  This helps performance.  However, to *form* `name_moved`
   * you will naturally spend some processor cycles which therefore risks slowing down the measured operation and
   * polluting timing results.  To avoid this, please follow the following suggestions.
   *
   *   - Passing a constant string as follows is very cheap: `"some constant"` or `string("same")`.
   *     So if that's good enough for you, do that.
   *     - In most STL, due to the `std::string` SSO (optimization which stores a short-enough string directly inside
   *       the `string` object), if you keep the constant 15 characters or less in length, it's even cheaper;
   *       heap is not used as a result.
   *   - Constructing something via util::ostream_op_string() (and/or util::String_ostream) adds about 500 nanoseconds
   *     per checkpoint() call (e.g.: `util::ostream_op_string("some string", some_int)`) on a 2015 MacBook Pro.
   *     For many, many timing scenarios such sub-microsecond overheads are no big deal, but watch out if you're
   *     measuring something tiny-subtle.
   *     - If it is a problem, use a constant thing as in previous bullet.  If that isn't sufficient, you can
   *       fairly easily use C-style direct-`char*`-writing to do it almost as quickly as that, or at least far closer
   *       to it than to the (syntactically-pretty but slow-in-*this*-context) stream-based string formation.
   *
   * ### Logging ###
   * It will log a TRACE message.  Hence if TRACE is enabled, timing results might be polluted, as logging can
   * take non-trivial time.
   *
   * @param name_moved
   *        Nickname for the timer, for logging and such.  If you want to preserve your string, pass in a copy:
   *        `string(your_name_value)`.  See above regarding perf impact.
   * @return Reference to immutable new Checkpoint `struct`, as it sits directly inside `*this`.
   */
  const Checkpoint& checkpoint(std::string&& name_moved);

  /**
   * Returns the duration(s) (for all enabled `Clock_type`s) between construction and the last checkpoint() call.
   * Behavior is undefined if checkpoint() has not been called.
   *
   * @return See above.
   */
  Duration_set since_start() const;

  /**
   * Returns the checkpoints recorded so far.  This returns a reference and thus avoids a copy.  The reference is
   * valid until `*this` destroyed.
   *
   * @return Checkpoints so far.
   */
  const std::vector<Checkpoint>& checkpoints() const;

  /**
   * Called anytime after the last checkpoint(), this modifies the data collected so far to be as if every checkpoint
   * duration D shortened/lengthened by `mult_scale` and divided by `div_scale`.  For example, if
   * `float(mult_scale) / float(div_scale) == 0.5`, and 3 checkpoints were recorded as (6s, 4s, 12s)
   * (`since_start()` = 12s + 4s + 6s = 22s), then after the scale() call the checkpoints will reflect
   * (3s, 2s, 6s) (`since_start()` == 11s).
   *
   * In particular (and the original use case for this -- see doc header), if one recorded operation X repeated
   * sequentially 100x, with `*this` constructed just before the first iteration and `checkpoint()`ed
   * just after the 100th, then performing `scale(1, 100)` would effectively result in `*this` representing
   * the mean duration of a single X iteration.
   *
   * Formally, behavior is undefined if one calls checkpoint() after calling scale().  Informally it is likely to
   * result in nonsensical data for all checkpoints created starting with that one and for since_start().
   *
   * @param mult_scale
   *        The timer data are conceptually multiplied by this, first.
   * @param div_scale
   *        The timer data are conceptually divided by this, next.
   */
  void scale(uint64_t mult_scale, uint64_t div_scale);

  /**
   * Sample all currently enabled `Clock_type`s' clocks and return those values, each of which is a time stamp
   * relative to some Epoch value.  (The Epoch value differs by Clock_type.  Durations can be generated by subtracting
   * one time point from another which mathematically makes Epoch's meaning irrelevant.)
   * The value `Time_pt()` (internally: 0 a/k/a Epoch) is set for the disabled clocks.  In particular checkpoint() will
   * internally call this.
   *
   * ### Rationale ###
   * It is unusual to use this directly by the user, as absolute time
   * stamps aren't useful directly, while since_start(), checkpoints(),
   * the I/O facilities, etc., provide access to the various computed durations, so one needn't do the subtracting
   * manually.  Nevertheless being able to get the raw value is useful for (1) sanity-checking more advanced results;
   * and (2) to time the timer!  (An example of the latter is when I was comparing the duration of checkpoint()
   * with a constant `name_moved` vs. a dynamic-stream-created one vs. just getting the time stamp via now() and
   * not calling checkpoint() at all.)
   *
   * @return See above.
   */
  Time_pt_set now() const;

  /**
   * Based on the passed in `clock_type` argument, the current time is returned for that one particular clock type.
   *
   * @param clock_type
   *        The type of clock to use.  Please carefully read the doc headers of all
   *        perf::Clock_type `enum` members.
   * @return The current time.
   */
  static Time_pt now(Clock_type clock_type);

  /**
   * Based on the passed in `which_clocks` argument, the current time is returned for the clock types enabled by it.
   * Identical to non-`static` now() but `which_clocks` is specified as arg instead of at construction.
   *
   * @param which_clocks
   *        The types of clock to use.  Please carefully read the doc headers of all
   *        perf::Clock_type `enum` members.
   * @return The current time or times.
   */
  static Time_pt_set now(const Clock_types_subset& which_clocks);

  /**
   * Equivalent to `(*os) << (*this)`.
   *
   * @param os
   *        Pointer to `ostream` to which to write.
   */
  void output(std::ostream* os) const;

  // Data.

  /// The immutable human-readable name for this timer.  It's `const` so no need for accessor for now.
  const std::string m_name;

private:
  // Types.

  /**
   * `chrono` clock used, internally, for Clock_type::S_CPU_USER_LO_RES and Clock_type::S_CPU_SYS_LO_RES queries.
   * It can obtain a measurement of both user and kernel time spent by the process concurrently.
   * In C++20 this isn't in std.chrono yet.  Go, boost.chrono!
   *
   * @note It returns a "real" time too.  However we use other, better clocks for that.
   */
  using Cpu_split_clock = boost::chrono::process_cpu_clock;

  /// Short-hand for the combo (user/kernel) durations-since-epoch tracked by #Cpu_split_clock.
  using Cpu_split_clock_durs_since_epoch = Cpu_split_clock::times;

  // Methods.

  /**
   * Returns the current #Cpu_split_clock time, essentially as a combo `struct` containing raw (integer) counts of
   * user and kernel ticks since that clock's epoch reference point.
   *
   * ### Rationale ###
   * It's a helper for performance and accuracy: So we can access the clock only once and be able to able to get the
   * various values in the result at the same time.  now_cpu_lo_res() can get individual values from the return value
   * here.
   *
   * @return See above.
   */
  static Cpu_split_clock_durs_since_epoch now_cpu_lo_res_raw();

  /**
   * Returns individual `Time_pt` value returnable by now() various overloads: either the user time or system time
   * depending on arg.
   *
   * @param cpu_combo_now_raw
   *        Value returned by now_cpu_lo_res_raw() (or equivalent).
   * @param user_else_sys
   *        If `true` the user time is returned; else the system time.
   * @return See above.
   */
  static Time_pt now_cpu_lo_res(const Cpu_split_clock_durs_since_epoch& cpu_combo_now_raw, bool user_else_sys);

  // Data.

  /**
   * Aggregator needs access to the Checkpointing_timer innards; generally speaking, stylistically, it is fine.
   * It is an inner class to reflect that cooperation further.
   */
  friend class Aggregator;

  /**
   * Internally useful constructor that initializes an ill-formed `*this` without almost no meaningful member values,
   * specifically useful at least when creating the aggregated Checkpointing_timer from multiple "real" timers,
   * by Aggregator.  Take care to initialize all fields properly before making
   * `*this` publicly available.
   *
   * @param logger_ptr
   *        See `public` constructor.
   * @param name_moved
   *        See `public` constructor.
   * @param which_clocks
   *        See `public` constructor.
   */
  explicit Checkpointing_timer(log::Logger* logger_ptr, std::string&& name_moved, Clock_types_subset which_clocks);

  /**
   * The bit-set indexed by `size_t(Clock_type::...)` wherein that clock will be sampled in now() and checkpoint()
   * if and only if its bit is set.  All zeroes is allowed and will, accordingly, result in 0 `Duration`s and
   * `Time_pt`s in all results.  Subtlety: now() doesn't work properly until this is initialized.
   */
  const Clock_types_subset m_which_clocks;

  /**
   * The value of now() saved in `public` constructor, marking the time point when the timing began.  Doesn't change
   * after construction.  since_start() (the total duration(s)) is based on this and #m_start_when.
   *
   * Ideally this would be `const` but isn't for some minor reasons (see `public` constructor).
   */
  Time_pt_set m_start_when;

  /**
   * The value of now() recorded during the last checkpoint() call; or equals #m_start_when if checkpoint() has not
   * yet been called.  since_start() (the total duration(s)) is based on this and #m_start_when.
   *
   * Ideally this would be `const` but had to be left technically mutable due to certain `array<>` initialization
   * subtleties.
   */
  Time_pt_set m_last_checkpoint_when;

  /**
   * The checkpoints recorded so far, in order, one per checkpoint() call.  To avoid any reallocations, this is
   * reserved to contain a buffer at least `max_n_checkpoints` (see ctor) elements long.
   */
  std::vector<Checkpoint> m_checkpoints;
};

/**
 * This companion/inner class of Checkpointing_timer provides aggregation of results from many `Checkpointing_timer`s
 * each measuring some user operation being repeated many times; particularly when the operation consists of multiple
 * steps (checkpoints) of interest.
 *
 * Please read the aggregation section in Checkpointing_timer doc header before using this.
 *
 * One small technical point: To make a Checkpointing_timer X available for `Aggregator::aggregate(X)`, you must
 * construct it on the heap and provide a Checkpointing_timer_ptr accordingly:
 *
 *   ~~~
 *   Checkpointing_timer_ptr X(new Checkpointing_timer(...));
 *   ~~~
 *
 * ### Thread safety ###
 * A reiteration of the aforementioned Checkpointing_timer doc header: Non-`const` methods are generally not safe
 * if called concurrently with any other method on the same Aggregator.  In practice this should only matter for
 * calling aggregate().  If you are timing things concurrently happening in different threads you may need to add
 * a mutex lock around aggregate(); and in that case make sure it doesn't adversely affect your algorithm's overall
 * performance (or, if it does, then that is acceptable).
 */
class Checkpointing_timer::Aggregator :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Constructors/destructor.

  /**
   * Constructs a new, empty aggregator.  After this call aggregate() to add source `Checkpointing_timer`s.
   *
   * @param name_moved
   *        Nickname for the agg, for logging and such.
   * @param max_n_samples
   *        The number of times you shall call aggregate() shall not exceed this.
   *        See similar (in spirit) `max_n_checkpoints` in Checkpointing_timer constructor.
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   */
  explicit Aggregator(log::Logger* logger_ptr, std::string&& name_moved, size_t max_n_samples);

  // Methods.

  /**
   * Register given Checkpointing_timer for aggregated results.  The best time to call this is immediately after
   * the last `timer_ptr->checkpoint()` call, as that way the call itself won't be included in the timer's duration(s).
   *
   * The second best time to call it is immediately after `*timer_ptr` is constructed; the call is quite cheap, so
   * it really shouldn't affect results much.  The third best time is somewhere between those 2 points in time; in that
   * case its (quite small, probably negligible) time cost will be charged to whatever checkpoint was "running" at the
   * time.
   *
   * ### Requirements on aggregated timers ###
   * In and of itself `*timer_ptr` can be any Checkpointing_timer with at least one checkpoint() call done.
   * In addition, behavior is undefined in log_aggregated_results() and create_aggregated_result() unless all of the
   * following hold about all `aggregate()`d timers at the time of those calls.
   *   - Their `which_clocks` values (as passed to Checkpointing_timer constructor) are all equal.
   *   - They each have the same number of checkpoints.
   *   - For each `i`: Their `i`-th Checkpointing_timer::Checkpoint::m_name values are all mutually equal.
   *
   * Since aggregation collates the many measurements of the same multi-step operation, these should all be common
   * sense; if one doesn't hold for you, there is some kind of very basic problem.
   *
   * ### Thread safety ###
   * See Aggregator doc header.  A mutex lock around this call may be desirable depending on situation.
   *
   * @param timer_ptr
   *        The timer to register.
   */
  void aggregate(Checkpointing_timer_ptr timer_ptr);

  /// Act as if aggregate() has never been called since construction.
  void clear();

  /**
   * Called after the final Checkpointing_timer has finished (final `checkpoint()` call) and been `aggregate()`d,
   * this `INFO`-logs human-friendly readout of the aggregated results so far.  This will include all 3 types of
   * aggregation: sum; mean; and mean-scaled-by-convenience-constant; or some specified subset thereof.
   *
   * ### Performance ###
   * It is possible to call it between `aggregate()`s, perhaps to output incremental results.  It may be slow and should
   * not be called within any perf-critical section and definitely not in any measured section.  The most natural
   * time to call it is after all data collection has finished.
   *
   * Tip: you can pass null as the logger to the ctor of each Checkpointing_timer and even the Aggregator
   * and only use non-null `alternate_logger_ptr_or_null`.  Then the cost of should-log checks will be zero
   * all over except here (where presumably it does not matter).
   *
   * @param alternate_logger_ptr_or_null
   *        If null, logs are sent to get_logger() as normal; otherwise it is sent to the supplied log::Logger instead.
   *        Since logging this output is a first-class facility within Checkpointing_timer::Aggregator -- unlike
   *        vast majority of Flow logging which is a secondary (though important in practice) nicety -- it seemed
   *        sensible to provide extra control of this particular bit of logging.
   *        Also note that any (probably TRACE) logging outside of the timer-data output will still go to
   *        get_logger().
   * @param show_sum
   *        Whether to show the SUM aggregated output type.
   * @param show_mean
   *        Whether to show the MEAN aggregated output type.
   * @param mean_scale_or_zero
   *        If zero do not show SCALED-MEAN aggregated output; otherwise show it, with this value as the
   *        scale-constant.  (E.g., one could show the average latency per 100,000 requests which might be
   *        more conveniently large than a per-request mean.)
   */
  void log_aggregated_results(log::Logger* alternate_logger_ptr_or_null,
                              bool show_sum, bool show_mean, uint64_t mean_scale_or_zero) const;

  /**
   * Provides programmatic access to the aggregation result that would be logged by log_aggregated_results() -- in
   * the form of a returned new Checkpointing_timer that stores those numbers.
   *
   * log_aggregated_results() basically calls this once for each type of aggregated data it wants to see.
   * Accordingly, if you desire programmatic access to type X of aggregated results, call this and specify the
   * type X of interest, as args.  log_aggregated_results() doc header's perf notes apply here similarly.
   *
   * You can also use `info_log_result` to immediately log the results, as log_aggregated_results() would for that
   * aggregation type.  Conceivably you could then ignore the return value, depending on your goals.
   *
   * @param alternate_logger_ptr_or_null
   *        Same meaning as for log_aggregated_results().  However here this is only relevant if `info_log_result`;
   *        otherwise ignored (null is fine).
   * @param info_log_result
   *        If and only if `true`, INFO messages are logged with human-readable representation of
   *        the returned data, similarly to (partial) log_aggregated_results() output.
   *        Actually it simply calls log_aggregated_result_in_timer() on the returned Checkpointing_timer.
   * @param mean_scale_or_zero
   *        If zero then return SUM aggregated output; otherwise MEAN or SCALED-MEAN aggregated output, with this
   *        value as the scale-constant (the value 1 => an un-scaled MEAN output).
   *        (E.g., one could show the average latency per 100,000 requests which might be
   *        more conveniently large than a per-request mean.)
   * @return Ref-counted pointer to a new Checkpointing_timer that stores the aggregated results.
   *         This timer will have the same `which_clocks`, and its `checkpoints()` will have the same length and
   *         in-order names, as each of the source timers; its name and get_logger() will come from Aggregator `name`
   *         (ctor arg) and get_logger() respectively.
   */
  Checkpointing_timer_ptr create_aggregated_result(log::Logger* alternate_logger_ptr_or_null, bool info_log_result,
                                                   uint64_t mean_scale_or_zero = 1) const;

  /**
   * Given an aggregated-result Checkpointing_timer (as if returned by create_aggregated_result()) logs the SUM or MEAN
   * or SCALED-MEAN stored in it as INFO messages.
   *
   * Publicly, it exists for one use case (as of this writing), namely to log a "manually" compiled aggregated-result
   * Checkpointing_timer.  This *pattern* is explained in the Checkpointing_timer doc header in the aggregation section.
   *
   * Naturally, it is also used internally by create_aggregated_result() and log_aggregated_results(), hence why they
   * all produce similar output.
   *
   * @param logger_ptr
   *        Same meaning as in log_aggregated_results(), except null is not a valid value (behavior is undefined).
   * @param agg_timer
   *        Aggregated-result Checkpointing_timer, as if returned by create_aggregated_result().
   *        Behavior is undefined unless it was, in fact, returned by that method (or is a copy of such);
   *        or unless it was compiled as described in the aforementioned pattern, documented in Checkpointing_timer
   *        doc header.
   * @param n_samples
   *        For appropriate logging, this must contain the # of samples (repetitions of the measured operation)
   *        collected to yield `agg_timer`.
   * @param mean_scale_or_zero
   *        For appropriate logging, this must contain the type of aggregated numbers that are stored in `agg_timer`.
   *        See create_aggregated_result().
   */
  static void log_aggregated_result_in_timer(log::Logger* logger_ptr,
                                             const Checkpointing_timer& agg_timer, unsigned int n_samples,
                                             uint64_t mean_scale_or_zero = 1);

  // Data.

  /// The immutable human-readable name for this timer.  It's `const` so no need for accessor for now.
  const std::string m_name;

private:
  // Data.

  /// The ref-counted pointers to source timers passed to aggregate() in that order.
  std::vector<Checkpointing_timer_ptr> m_timers;
}; // class Checkpointing_timer::Aggregator

// Free functions: in *_fwd.hpp.

// However the following refer to inner class and hence must be declared here and not _fwd.hpp.

/**
 * Prints string representation of the given `Checkpoint` to the given `ostream`.  See
 * Checkpointing_timer::checkpoint() and Checkpointing_timer::checkpoints().
 *
 * @relatesalso Checkpointing_timer::Checkpoint
 *
 * @param os
 *        Stream to which to write.
 * @param checkpoint
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Checkpointing_timer::Checkpoint& checkpoint);

} // namespace flow::perf
