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

#include "flow/util/stat/stat_set.hpp"
#include "flow/util/stat/stat_fwd.hpp"
#include "flow/common.hpp"
#include <vector>
#include <type_traits>
#include <limits>
#include <atomic>

namespace flow::util::stat
{

// Types.

/**
 * Stats counter: tracks a histogram that summarizes, cumulatively, the numeric outcome of an event that keeps
 * repeatedly happening.  The performance profile of this tracking is as follows: record_value() performs a small
 * number of comparisons and additions, plus an increment.  All memory allocation occurs in the ctor.
 *
 * ### Thread safety ###
 * It is thread-safe, in that all operations on a given `*this` may be invoked concurrently, excluding
 * those that could change the bucket structure (as of this writing copy-assignment).
 *
 * All reads and writes are done using `std::atomic` with `memory_order_relaxed`, meaning one can (at least
 * given Linux x86-64) expect similar performance to regular loads/stores sans `atomic<>`.  (The `++` op, for
 * x86-64 -- `x.fetch_add(1)` -- does take a few more cycles on an `atomic` than a naked `++` would; it it not
 * the identical processor instruction.  However the cycle cost difference is in the low-single-digit-ns range
 * when uncontended: not identical but close.  That pertains to record_value().  Cf.: loads (count_for_bucket())
 * are fully identical.)
 *
 * Due to the nature of stat-keeping, `memory_order_relaxed` semantics are usually considered semantically acceptable.
 *
 * ### How it works ###
 * record_value() records an event, updating the histogram data which are cumulative
 * (there is no sliding time window; it just keeps accumulating stats).  The histogram results-so-far can be viewed
 * at will via `const` access.
 *
 * Each event is assumed to result in an integer-number outcome which one passes to record_value().  For example
 * it might be a die throw; in which cases the possible outcomes are 1, 2, ..., 6.  We accept *any* outcome
 * as long as it fits into the large-range, signed type #value_t; whether there is really a constraint like
 * 1-to-6, as with a die throw, is not our worry per se.  Well, sort of; read on.
 *
 * In the ctor, a histogram-bucket array is initialized.  This determines, forever (until destruction or copy-to),
 * the outcome buckets.  A count (starting at 0 and then potentially incremented by record_value()) is
 * stored for each bucket.  Of the buckets each represents a grouping of possible outcomes.  (We discuss potential
 * bucket sizes and such and how to configure this separately below; but here we stay general.)
 *
 * There shall be N (at least 2) buckets.  All buckets are adjacent (there are no #value_t holes in-between).
 * Each bucket has width that is at least 1 (no null buckets).  Therefore each bucket can be represented as
 * a combination of the following:
 *   - Min-value: To land in the bucket, the outcome must be *at least* min-value.
 *   - Width.  This determines max-value: Min-vale plus width minus 1. To land in the bucket, the outcome must be
 *     *at most* max-value.  The next bucket, if any, begins at exactly max-value plus 1.
 *
 * This just leaves the aforementioned allowance of any input.  Dice are not a great example for this, but just
 * bear with us.  Suppose you record_value() a number under first-bucket's min-value: in this case that's 1.
 * Which bucket takes it?  Answer: such an "underflow" always goes into first-bucket.  So a "die throw" of 0
 * or -12312 goes into first-bucket.  Similarly, any value greater than or equal to X, where X is the min-value for
 * last-bucket, lands into last-bucket (even if it exceeds last-bucket's max-value).  A "die throw" of 7 or 321
 * land in last-bucket, along with the vanilla last-bucket throw 6.
 *
 * @note You might notice that this in effect makes first-bucket's min-value and last-bucket's max-value
 *       irrelevant; as long as, when invoking ctor, it possible to express everything about the buckets except
 *       those two values, then it'll all work fine.  Indeed: this is true.  Nevertheless we require these
 *       things (one way or another) to still be specified.  Reasons: First-bucket min-value provides a user-friendly
 *       "anchor point" of sorts, from which one can express various required points (e.g., first-bucket max-value,
 *       which is mandatory, can be expressed as min-value plus bucket-width minus 1).  Last-bucket max-value
 *       is less useful in that sense but provides user-friendly symmetry (and the ability to detect overflow events
 *       in the future, if we come to desire this feature).  Lastly, these values are -- again for user-friendliness
 *       reasons -- shown in the pretty-print output (`ostream<<` + to_ostream()).
 *
 * What about specific bucket structures supported by `*this`?  In fact a `*this` supports any conceivable bucket
 * structure, as long as the basic axioms are observed (to recap: at least 2, no null buckets, buckets are adjacent).
 * In other words, the N buckets can be of any widths; e.g., they can be all different.  The `vector`-taking
 * ctor form supports this maximally-flexible config.  However, there are different ctor forms for convenience
 * and performance.  We discuss these now.
 *
 * ### Bucket structure: Linear ###
 * The simplest bucket structure supported is *linear*.  In this setup, first-bucket can be of any specified
 * width (as usual, not zero); while the remaining buckets (1+ of them) shall each have the same specified
 * width.  Therefore, the scale is generally linear, with a special-case potentially-different first-bucket.
 *
 * The rationale for offering this ctor form is not just convenience in the case where one happens to desire a
 * linear scale.  It is also, internally, handled differently w/r/t *performance*.  If the bucket structure is
 * linear, then the only computationally notable operation within record_value() et al -- given `value_t` outcome,
 * find `size_t` bucket index of bucket containing that out come -- can be performed in constant time; no
 * search through the buckets is required.  record_value() can record stats inside tight loops in the middle
 * of very perf-sensitive areas of one's application; the faster it is the better.  This is therefore a significant
 * win (when applicable).
 *
 * @note This constant-time-performance gain is in effect whenever the configured bucket structure supports it;
 *       it is not necessary to use that particular linear-scale ctor form.  That is, if one uses the general
 *       (`vector`-taking) ctor form, and using that form specifies a linear-scale bucket structure, then
 *       `*this` will automatically use the faster lookup algorithm in record_value() et al.
 *
 * @note For the constant-time-performance gain to activate the requirement is as follows:
 *       Either there are exactly 2 buckets (degenerate case); or there are 3+, and all non-bookend buckets
 *       (i.e., all but first-bucket and last-bucket) have equal widths.  (The linear-scale ctor form
 *       guarantees this; though in its case last-bucket also has the same width as the non-bookend buckets.)
 *
 * We demonstrate the linear-scale ctor's knobs with the die-throw example.  `n_buckets` (ctor arg)
 * is how many buckets there are.  `bucket0_sz` is the "width" of first-bucket, and `bucket_sz` is the "width"
 * of each of the other buckets including last-bucket.  `bucket0_val0` is the min-value for first-bucket.
 *   - For example, with a die throw, you might use `n_buckets = 6` and `bucket0_sz = bucket_sz = bucket0_val0 = 1`.
 *     - The outcomes are then simply grouped as: [got 1][got 2][got 3][got 4][got 5][got 6].
 *   - Or you could do: `n_buckets = 3` and `bucket0_sz = bucket_sz = 2` and `bucket0_val0 = 1`.
 *     - The buckets are then: [got 1 or 2][got 3 or 4][got 5 or 6].
 *
 * This just leaves the aforementioned allowance of any input.  Dice are not a great example for this, but just
 * bear with us.  Suppose you record_value() a number under `bucket0_val0`.  Which bucket takes it?  Answer:
 * such an "underflow" always goes into first-bucket.  So a "die throw" of 0 or -12312 goes into first-bucket.
 * Similarly, any value greater than or equal to X, where X is the min-value for last-bucket, lands into last-bucket.
 *   - So consider `n_buckets = 3` and `bucket0_sz = bucket_sz = 2` and `bucket0_val0 = 1`.  This *really* means
 *     the following actual buckets as viewed by `*this`:
 *     - [any value <= 2][3 or 4][any value >= 5].
 *     - In reality valid dice throws always result in 1 or 2 or ... or 6, so this happens to reduce to
 *       [1 or 2][3 or 4][5 or 6].  If "broken" dice throws are possible, though, then a `*this` stands ready
 *       to count them in the two edge buckets.
 *
 * ### Bucket structure: General ###
 * Other than that, the general structure allows one to simply specify each bucket's width and min-value.
 * The general-scale ctor form takes a `vector` (sized at least 2, the # of buckets); each bucket's min-value
 * is that element (so they must be in strictly-increasing order).  The only missing bit of info is the
 * last-bucket's width (as there's no next-bucket to point at where this one ends); this is supplied as a separate
 * ctor arg alongside the `vector`.
 *
 * Example: With die-throws, the general-scale ctor call would look like so:
 * `Histogram_counter{{1, 2, 3, 4, 5, 6}, 1}`.  Six buckets, with the given min-values; and the last bucket's
 * width is 1.
 *
 * The performance of bucket-index-from-outcome lookup in record_value() et al is therefore as follows:
 *   - If 2 buckets: constant-time (degenerate case).
 *   - If 3+ buckets, and the non-bookend buckets are all of the same width: constant-time (see above).
 *   - Otherwise: logarithmic in bucket-count.  The lookup is a straightforward binary search through
 *     the min-values `vector` supplied via ctor.
 *
 * @internal
 * ### Impl notes ###
 * Reading the ground-level comments should clarify ~all questions.
 *
 * One mechanical subtlety is that we use stat::load(), stat::store(), and stat::fetch_add() as our atomic ops.
 * These are respectively simply:
 *   - load(x) is `x.load(memory_order_relaxed)`.
 *   - store(&x, y) is `x.store(y, memory_order_relaxed).
 *   - fetch_add(&x, y) is `x.fetch_add(y, memory_order_relaxed)`.
 *
 * We promised `memory_order_relaxed` perf and semantics; and those ops are available in flow::util::stat.
 */
class Histogram_counter
{
public:
  // Types.

  /// Each event outcome recorded by record_value() must fit in the range of this type.
  using value_t = int64_t;
  static_assert(std::is_integral_v<value_t> && std::is_signed_v<value_t>,
                "Histogram_counter shall support negative outcomes albeit only integer ones.");

  /// The count type.
  using count_t = uint64_t;

  // Constructors/destructor.

  /**
   * Allocates the histogram data structure (linear-scale: bucket widths [ABBB...]) and initializes each bucket's count
   * to zero.  Bucket widths [ABBB...] means that first-bucket can be of any positive length, while the remaining
   * 1+ buckets are each of the same width which may or not equal first-bucket's.
   *
   * ### Performance ###
   * This ctor form guarantees constant-time record_value().  See class doc header for a deeper dive.
   *
   * @see class doc header for overview of the arg knobs and the overall algorithm they control.  Meanwhile
   *      the docs just below are formal with few-or-no expositional niceties.
   *
   * @param n_buckets
   *        Number of buckets (at least 2) in the histogram.
   * @param bucket0_sz
   *        First-bucket shall count the following outcomes:
   *        less-than `bucket0_val0`, `bucket0_val0`, ..., `bucket0_val0 + bucket0_sz - 1`.
   *        `bucket0_sz` is at least 1.
   * @param bucket_sz
   *        Each bucket except first-bucket and last-bucket shall count outcomes `X`, ..., `X + bucket_sz - 1`,
   *        where `X` is one plus the max-value of the preceding bucket.
   *        Last-bucket shall count outcomes `X`, `X + 1`, ... ad infinitum, where `X` is one plus the max-value of
   *        the preceding (next-to-last) bucket.
   *        `bucket_sz` is at least 1.
   * @param bucket0_val0
   *        See above.  Note that any value is allowed for `bucket0_val0`.
   */
  explicit Histogram_counter(size_t n_buckets, value_t bucket0_sz, value_t bucket_sz = 1, value_t bucket0_val0 = 0);

  /**
   * Allocates the histogram data structure (general-scale: bucket widths [ABCD...]) and initializes each bucket's
   * count to zero.  Bucket widths [ABCD...] means that each bucket's width can be any positive value, and
   * they may (but are not required to) be all different.
   *
   * @note `bucket_val0s` can often be specified as a naked initializer-list sans any mention of `std::vector`;
   *       e.g.: `Histogram_counter histo{{1, 3, 5}, 2};`.
   *
   * ### Performance ###
   * If all bucket widths, except possibly the first and last, are all equal -- or there are only 2 buckets total --
   * this ctor form guarantees constant-time record_value().  Otherwise record_value() shall be logarithmic-time in
   * the number of buckets.  See class doc header for a deeper dive.
   *
   * @see class doc header for overview of the arg knobs and the overall algorithm they control.  Meanwhile
   *      the docs just below are formal with few-or-no expositional niceties.
   *
   * @param bucket_val0s
   *        Vector (of size at least 2) indicating the min-values of each bucket in order; these must be in
   *        strictly increasing order.  The max-value of the last bucket shall be
   *        `bucket_val0s.back() + bucket_n_sz - 1`.
   *        First-bucket shall *also* count all outcomes less-than its min-value (but to_ostream()
   *        will still specially show this min-value for expositional purposes).
   *        Last-bucket shall *also* count all outcomes greater-than the indicated max-value (but to_ostream()
   *        will still specially show this max-value for expositional purposes).
   * @param bucket_n_sz
   *        Width (at least 1) of the last bucket; see formula just above.
   */
  explicit Histogram_counter(const std::vector<value_t>& bucket_val0s, value_t bucket_n_sz);

  /**
   * Straightforward copy ctor.  There is no special-behavior move ctor.
   * @param src
   *        Source object.
   */
  Histogram_counter(const Histogram_counter& src);

  // Methods.

  /**
   * Straightforward copy assignment.  There is no special-behavior move assignment.
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Histogram_counter& operator=(const Histogram_counter& src);

  /**
   * Aggregator: bucket-by-bucket, makes `*this` bucket's count incremented by `src`'s corresponding bucket's
   * count.  That is: make `this->count_for_bucket(idx) == C(idx) + S(idx)`, where `C(idx)` is
   * pre-condition `this->count_for_bucket(idx)`, and `S` is ditto for `src`.
   *
   * If `src` has a different structure than `*this`, behavior is undefined.  Different structure means
   * the following does not hold: same bucket count; for each respective bucket min-value and width are the same.
   *
   * Tip: To aggregate `N`=2+ `Histogram_counters`:
   *   - (Destructive) Take `auto& result = v[0]`; then for `idx` in [`1`, `N - 1`]: `result += v[idx]`.
   *   - (Non-destructive) Take `auto result = v[0]`; then for `idx` in [`1`, `N - 1`]: `result += v[idx]`.
   *
   * @param src
   *        One with same structure as `*this`.  `this == &src` is allowed and will incur no special handling
   *        (hence the counts shall be doubled).
   * @return `*this`.
   */
  Histogram_counter& operator+=(const Histogram_counter& src);

  /**
   * Delta: bucket-by-bucket, makes `*this` bucket's count decremented by `src`'s corresponding bucket's
   * count.  That is: make `this->count_for_bucket(idx) == C(idx) - S(idx)`, where `C(idx)` is
   * pre-condition `this->count_for_bucket(idx)`, and `S` is ditto for `src`.
   *
   * @warning There is no underflow protection; if a given `C(idx) < S(idx)`, then post-condition
   *          `C(idx)` shall potentially *increase* in apparent value, potentially to an extremely high value
   *          indeed.
   *
   * If `src` has a different structure than `*this`, behavior is undefined.  Different structure means
   * the following does not hold: same bucket count; for each respective bucket min-value and width are the same.
   *
   * @param src
   *        One with same structure as `*this`.  (`this == &src` is allowed and will incur no special handling
   *        (hence the counts shall be zeroed).  Though in that case it's better to use clear().)
   * @return `*this`.
   */
  Histogram_counter& operator-=(const Histogram_counter& src);

  /**
   * Scaler: bucket-by-bucket, makes `this->count_for_bucket(idx)` equal to `v / divisor`, where `v` is its
   * pre-condition value.
   *
   * Tip: Can compute a mean histogram as follows: operator+=() `N - 1` times, then `*this /= N`.
   *
   * @param divisor
   *        Non-zero divisor.  If zero undefined behavior results (assertion may trip).
   * @return `*this`.
   */
  Histogram_counter& operator/=(count_t divisor);

  /**
   * Record event outcome.  See class doc header and ctor doc header for details.
   *
   * @tparam T
   *         `val` shall be `static_cast` from `T` to #value_t.
   * @param val
   *        The outcome.
   */
  template<typename T>
  void record_value(T val);

  /**
   * Convenience wrapper for record_value() that records a duration in the units specified as the mandatory
   * first template parameter.
   *
   * If necessary to lose precision, the duration is rounded to the nearest
   * desired unit.
   *
   * @todo It is possible to make util::stat::Histogram_counter a template parameterized on the type of values
   * stored, which would in particular allow `chrono::duration`s or `Fine_duration`s to be stored instead
   * of signed large integers.
   *
   * @internal
   * @note Internal impl note: For the preceding to-do impl might involve some specializing or otherwise wrangling the
   *       arithmetic in key private helper idx_of_outcome().
   * @endinternal
   *
   * @note If different rounding behavior is desired, such as floor or ceiling, then please use record_value()
   *       directly along with the appropriate rounding function: `.record_value(ceil<seconds>(val).count())`.
   *
   * @tparam Recorded_duration
   *         `boost::chrono` concrete duration type specifying the units in which the histogram tracks durations.
   *         E.g.: `chrono::milliseconds`, `chrono::minutes`.
   * @tparam Duration
   *         Any `boost::chrono` duration type, typically auto-detected by the compiler.
   * @param val
   *        The outcome duration.
   */
  template<typename Recorded_duration, typename Duration>
  void record_period(Duration val);

  /**
   * Returns the number of buckets in the histogram, as supplied to ctor as `n_buckets`.  Naming is consistent with
   * standard container `size()` (the bucket structure being the structure of `*this`); not to be confused with
   * the count-per-bucket via count_for_bucket().
   *
   * @return Number of buckets in `*this`.
   */
  size_t size() const;

  /**
   * Returns the event count accumulated so far in the bucket with the given index.
   * The semantics of how buckets are ordered and what they track are explained in the class doc header; or more
   * formally in the ctor doc header.
   *
   * @param idx
   *        Bucket index.  Behavior undefined (assert may trip) unless this is
   *        in [`0`, size()), where size() equals `n_buckets` supplied in ctor.
   * @return The count so far.  Note it cannot overflow due to the gargantuan max-value of #count_t.
   */
  count_t count_for_bucket(size_t idx) const;

  /**
   * Returns `count_for_bucket(idx)`, where `idx` is the index of the bucket into which `val` would fall, if one were
   * to call `record_value(val)`.
   *
   * This method may be convenient, but please remember: This doesn't count how many times `val` outcome has
   * occurred alone; it also includes the range of outcomes in its bucket; depending on the knobs set in ctor
   * other outcomes may have been possible.
   *
   * @tparam T
   *         See record_value().
   * @param val
   *        See record_value().
   * @return See count_for_bucket().
   */
  template<typename T>
  count_t count_for_bucket_containing_outcome(T val) const;

  /// Returns `*this` to the state as-if just-cted with the current bucket structure.  Uses stat::store() x N.
  void clear();

  /**
   * Set a bucket-value so that `count_for_bucket(idx) == new_count`.
   *
   * ### Rationale ###
   * If counting the usual `record_*()` techniques, the use of overwrite_count_for_bucket() is arguably unlikely.
   * The precipitating use case is when the counts come wholesale from another stats data-source, so that
   * a Histogram_counter is used as a mere container of counts, perhaps for pretty-printing and/or
   * with stats_since_reset_state().
   *
   * Corollary: Be cautious if concurrent access -- particularly record_value() et al -- is possible.
   * Uses stat::store().
   *
   * @param idx
   *        See record_value().
   * @param new_count
   *        See count_for_bucket(): namely what it returns.
   */
  void overwrite_count_for_bucket(size_t idx, count_t new_count);

  /**
   * Prints string representation to the given `ostream`.
   *
   * @param os
   *        Stream to which to write.
   */
  void to_ostream(std::ostream* os) const;

private:
  // Methods.

  /**
   * Which index into #m_per_bucket_counts points to the appropriate bucket for `record_value(val)`?
   *
   * @param val
   *        See record_value().
   * @return Index into #m_per_bucket_counts.
   */
  size_t idx_of_outcome(value_t val) const;

  // Data.

  /**
   * General bucket structure (except the last bucket's width): in strictly increasing order, each bucket's
   * min-value.  Not `const` to allow for copy-assignment at least.
   *
   * ### Use 1: Output ###
   * There are two uses.  First and least, it plus #m_bucket_n_sz are used in to_ostream() for user-friendly output,
   * labeling the range of each bucket.
   *
   * ### Use 2: idx_of_outcome() ###
   * More importantly it is used to implement idx_of_outcome(), used by record_value() et al to determine which
   * bucket's count to increment, given an outcome belonging to one of said buckets.  That is the central
   * and only potentially time-consuming of the fast-path ops in a `*this`.  The following cases are to be
   * considered in order for a good combo of performance (top priority) and simplicity.
   *   -# **If `size() == 2`**
   *      Only `m_bucket_val0s[1]` matters: Outcomes less-than this min-value are in bucket 0;
   *      else in bucket 1.  Various corner cases can be ignored if we handle this particular degenerate case
   *      separately.
   *      - Time complexity: constant.
   *   -# **If #m_linear_bucket_sz_or_0 is in effect (`!= 0`)**
   *      This means all 1+ buckets strictly-between 1st and last are of the same width (vacuously so if `size() == 3`);
   *      namely `m_linear_bucket_sz_or_0`.  Therefore an optimized algorithm for idx_of_outcome() is in effect; no search
   *      through buckets is required.  Instead: if below `F = m_bucket_val0s[1]` => index 0; otherwise subtract `F`,
   *      divide by `m_linear_bucket_sz_or_0`, and add 1.  The result, clipped against the ceiling `size() - 1`, is
   *      the index sought.
   *      - Time complexity: constant.
   *   -# **General case**
   *      No particular optimization applies.  Perform a binary-search through #m_bucket_val0s, clip against
   *      the range [`0`, `size()`), and voila.
   *      - Time complexity: logarithmic in size().
   *
   * ### Impl rationale: Why no `flat_map` instead of 2 `vector`s? ###
   * We keep two equally-sized `vector`s: #m_bucket_val0s (the "labels": the bucket structure) and #m_per_bucket_counts
   * (the counts in those respective buckets).  `flat_map<value_t, atomic<count_t>>` would achieve essentially the
   * same thing; in terms of time complexity of all our ops definitely the case.  So should we use that instead?
   *
   * Firstly: yes, it would be quite defensible to do that.  In terms of internal code quality, it would be more
   * compact; things like `flat_map::upper_bound()` => iterator => deref directly in map => increment `.second`
   * would read as tighter than the alternative -- a bit nicer.  And the perf difference below, while real, is
   * still pretty small.
   *
   * So why go with 2 `vector`s?  Answer: basically it's a perf-maxing thing.  record_value() et al can be and are
   * used in extremely perf-sensitive tight loops, so potentially every processor cycle counts.  The 2-`vector`
   * setup has optimal cache-locality (#m_per_bucket_counts is what changes and is all in one area; #m_bucket_val0s
   * is elsewhere and does not change after construction or copy-to).  So by doing this we do guarantee the best
   * perf achievable (in context).
   *
   * That said: Who says `flat_map` does not do it the same anyway?  And, actually, it *sort of* does.  In fact:
   * `std::flat_map` is specified by the standard as an adapter that defaults to two internal containers
   * `vector<Key>`, `vector<Mapped>` -- *exactly* what we have.  However, as of this writing we are constrained
   * by C++17; so we cannot use `std::flat_map`.  What about `boost::container::flat_map` which originated `flat_map`
   * in the first place, though?  Answer: Interestingly, as of mid-2026, `boost::container::flat_map` is
   * actually somewhat different from what the standard mandates for `std::flat_map`.  It is not an adapter --
   * its internal representation is hard-coded -- and looking inside there shows it deals in `pair<Key, Mapped>`;
   * so most likely not keeping the keys and mapped-values segregated.
   */
  std::vector<value_t> m_bucket_val0s;

  /**
   * Bucket structure: an addendum to #m_bucket_val0s, this is the positive width of the last of size() buckets.
   *
   * ### Use ###
   * It is used with #m_bucket_val0s in to_ostream() for user-friendly output.
   *
   * Note that it is *not* used in idx_of_outcome(), as any outcome not-below min-value of the last bucket
   * is dumped into the last bucket.  Again, though, it is nice for printing `*this`.
   */
  value_t m_bucket_n_sz;

  /**
   * If `size() == 2`, or the non-bookend buckets are not all of the same width, this is zero; else
   * the (positive) width of each non-bookend bucket.  In the latter case idx_of_outcome() may use the
   * optimized non-degenerate constant-time algorithm that sidesteps the need to binary-search #m_bucket_val0s.
   *
   * @see #m_bucket_val0s doc header for an outline of the idx_of_outcome() algorithm, including how it
   *      aims to prominently use #m_linear_bucket_sz_or_0.
   *
   * ### Rationale ###
   * Why keep this, given that (assuming it is not zero) it can be quickly computed
   * as `(m_bucket_val0s[2] - m_bucket_val0s[1])`?  Answer: For one thing, it acts as a flag to cache whether
   * the non-bookend buckets are indeed all equal (walking #m_bucket_val0s each time would be linear-time and
   * destroy the whole point of the intended optimization).  Opportunistically, then, we might as well cache
   * the aforementioned value to avoid the need for recomputing it; we've got to store the flag in any case so
   * might as well cut down on repeated computations.
   */
  value_t m_linear_bucket_sz_or_0;

  /**
   * The data.  Elements start at 0 and are incremented indefinitely; #count_t is too wide for overflow to be ever
   * reached.  `.size() >= 2`.
   *
   * Storing a `vector<atomic<>>` can be dodgy, in that it can be tough to even get it to compile, but in our case:
   *   - (Ignoring copy-assignment) It is constructed at its forever-`.capacity()` (and `.size()`) all the way
   *     until it is destroyed.  No problems there.
   *   - Default copy-assignment would not compile; therefore our copy-assignment operator performs the required
   *     re-construction at the new capacity/size (explicitly); and then performs the required atomic-load-and-store
   *     ops (explicitly).
   *     - Copy-construction reuses this (again: default copy-assignment would not compile).
   */
  std::vector<std::atomic<count_t>> m_per_bucket_counts;
}; // class Histogram_counter

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename T>
void Histogram_counter::record_value(T val)
{
  auto& count = m_per_bucket_counts[idx_of_outcome(static_cast<value_t>(val))];

#if 1
  fetch_add(&count, 1);

  static_assert(sizeof(count_t) >= 8,
                "We must use 64+-bit counters, so as to not need to contend with overflow.");
#else // Pre-atomic<> code is here for posterity.  With atomic<> the overflow case is just a pain, so we do the above.
  if constexpr(sizeof(count_t) <= 4)
  {
    if (count != std::numeric_limits<count_t>::max()) // We promised to stop just before overflow.
    {
      ++count;
    }
  }
  else
  {
    ++count; // In practice with 64+ bits in a count, it'll never overflow, so don't even worry about that.
  }
#endif
}

template<typename Recorded_duration, typename Duration>
void Histogram_counter::record_period(Duration val)
{
  record_value(boost::chrono::round<Recorded_duration>(val).count());
}

template<typename T>
Histogram_counter::count_t Histogram_counter::count_for_bucket_containing_outcome(T val) const
{
  return load(m_per_bucket_counts[idx_of_outcome(static_cast<value_t>(val))]);
}

} // namespace flow::util::stat
