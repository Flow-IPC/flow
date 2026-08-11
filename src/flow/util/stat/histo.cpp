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

#include "flow/util/stat/histo.hpp"
#include "flow/util/stat/stat_set.hpp"
#include <cassert>
#include <algorithm>

namespace flow::util::stat
{

// Implementations.

Histogram_counter::Histogram_counter(const std::vector<value_t>& bucket_val0s, value_t bucket_n_sz) :
  m_bucket_val0s(bucket_val0s),
  m_bucket_n_sz(bucket_n_sz),
  m_linear_bucket_sz_or_0(0), // Might stay this way (general case; degenerate case size() == 2); or not (see below).
  m_per_bucket_counts(bucket_val0s.size())
{
  using std::adjacent_find;
  using std::greater_equal;

  // See m_bucket_val0s doc header for the central algorithm's overview; it informs the below.

  const auto start_it = m_bucket_val0s.begin();
  const auto end_it = m_bucket_val0s.end();
  assert((m_bucket_val0s.size() > 1) && "Ctor contract violation.");
  assert((m_bucket_n_sz > 0) && "Ctor contract violation.");
  assert((adjacent_find(start_it, end_it, greater_equal<value_t>()) == end_it)
         && "Ctor contract violation: Needs to be sorted and sans repeats.");

  const auto n_buckets = size();
  if (n_buckets != 2) // I.e., n_buckets >= 3.
  {
    // Optimism!  Assume constant-time idx_of_outcome().  Grab 1st middle-width W=[1.min->2.min]; assume rest are same.
    m_linear_bucket_sz_or_0 = m_bucket_val0s[2] - m_bucket_val0s[1];

    /* Check whether assumption holds up: W=[2.min->3.min], W=[3.min->4.min], etc.
     *   - If n_buckets=3, adjacent_find() is given a 1-range, so it just returns .end().  Vacuously there is just W.
     *   - If n_buckets>4, adjacent_find() is given a >1-range, so it scans at least 1 adjacent pair; if any !=W,
     *     returns non-.end().
     *     - If it can't find such a thing -- meaning all are =W -- then returns .end(). */
    if (end_it != adjacent_find(start_it + 2, end_it,
                                [&](auto a, auto b) -> bool { return (b - a) != m_linear_bucket_sz_or_0; }))
    {
      m_linear_bucket_sz_or_0 = 0; // Optimism failed!  One of the middle bkt widths was not the same as all others.
    }
    // else { N=3; or N>3, and each middle-width after the 1st (W) equals W.  Optimism justified! }
  }
  // else if (2 buckets) { Degenerate case of idx_of_outcome(). }

  /* Before C++20 the atomic<>s are originally uninitialized... or at least there are mixed signals out there
   * about whether a given impl will do the right thing here in the context of vector<atomic<>>.
   * Let's not risk any issues. @todo Remove on moving to C++20. */
  clear();
} // Histogram_counter::Histogram_counter()

Histogram_counter::Histogram_counter(size_t n_buckets, value_t bucket0_sz, value_t bucket_sz, value_t bucket0_val0) :
  m_bucket_val0s(n_buckets), // size() set; must still set the values (min-value for each bucket).
  m_bucket_n_sz(bucket_sz),
  m_linear_bucket_sz_or_0(bucket_sz),
  m_per_bucket_counts(n_buckets)
{
  // See m_bucket_val0s doc header for the central algorithm's overview; it informs the below.

  assert((n_buckets > 1) && "Ctor contract violation.");
  assert((bucket0_sz > 0) && "Ctor contract violation.");
  assert((bucket_sz > 0) && "Ctor contract violation.");

  /* Maintain official invariant per m_linear_bucket_sz_or_0 doc header.  (It is mostly for cleanliness here;
   * as of this writing idx_of_outcome() will ignore m_linear_bucket_sz_or_0 anyway, given `size() == 2`.) */
  (n_buckets == 2)
    && (m_linear_bucket_sz_or_0 = 0);

  // Must fill out the bucket-labels (min-value for each bucket), as noted earlier.
  m_bucket_val0s[0] = bucket0_val0;
  auto next_val = bucket0_val0 + bucket0_sz;
  for (size_t idx = 1; idx != n_buckets; ++idx)
  {
    m_bucket_val0s[idx] = next_val;
    next_val += bucket_sz;
  }

  clear(); // See other ctor for brief explanation.
} // Histogram_counter::Histogram_counter()

Histogram_counter::Histogram_counter(const Histogram_counter& src)
{
  // As noted in m_per_bucket_counts doc header, the `= default` impl will not compile due to atomic<>, so we gotta:
  operator=(src);
}

Histogram_counter& Histogram_counter::operator=(const Histogram_counter& src)
{
  using std::vector;
  using std::atomic;

  // As noted in m_per_bucket_counts doc header, the `= default` impl will not compile due to atomic<>, so we gotta:

  if (&src != this)
  {
    m_bucket_val0s = src.m_bucket_val0s;
    m_bucket_n_sz = src.m_bucket_n_sz;
    m_linear_bucket_sz_or_0 = src.m_linear_bucket_sz_or_0;

    // Replace our bucket-vector with a freshly-sized one containing whatever.  Then manually copy the values.
    m_per_bucket_counts = vector<atomic<count_t>>(src.size()); // Avoid {}, might list-initialize.

    const auto n_buckets = size();
    for (size_t idx = 0; idx != n_buckets; ++idx)
    {
      store(&m_per_bucket_counts[idx], load(src.m_per_bucket_counts[idx]));
    }
  }

  return *this;
} // Histogram_counter::operator=()

Histogram_counter& Histogram_counter::operator+=(const Histogram_counter& src)
{
  assert((m_bucket_val0s == src.m_bucket_val0s) && "Dif structures; stop that."); // Vector size + values compare.
  assert((m_bucket_n_sz == src.m_bucket_n_sz) && "Dif structures; stop that.");
  assert((m_linear_bucket_sz_or_0 == src.m_linear_bucket_sz_or_0) && "Bug in ctor(s)?  This should be invariant.");

  const auto n_buckets = size();
  for (size_t idx = 0; idx != n_buckets; ++idx)
  {
    fetch_add(&m_per_bucket_counts[idx], load(src.m_per_bucket_counts[idx]));
  }

  return *this;
}

Histogram_counter& Histogram_counter::operator-=(const Histogram_counter& src)
{
  assert((m_bucket_val0s == src.m_bucket_val0s) && "Dif structures; stop that."); // Vector size + values compare.
  assert((m_bucket_n_sz == src.m_bucket_n_sz) && "Dif structures; stop that.");
  assert((m_linear_bucket_sz_or_0 == src.m_linear_bucket_sz_or_0) && "Bug in ctor(s)?  This should be invariant.");

  const auto n_buckets = size();
  for (size_t idx = 0; idx != n_buckets; ++idx)
  {
    fetch_sub(&m_per_bucket_counts[idx], load(src.m_per_bucket_counts[idx]));
  }

  return *this;
}

Histogram_counter& Histogram_counter::operator/=(count_t divisor)
{
  assert((divisor != 0) && "Divisor must not be zero.");

  const auto n_buckets = size();
  for (size_t idx = 0; idx != n_buckets; ++idx)
  {
    store(&m_per_bucket_counts[idx], load(m_per_bucket_counts[idx]) / divisor);
  }

  return *this;
}

size_t Histogram_counter::idx_of_outcome(value_t val) const
{
  static_assert(static_cast<int32_t>(0xFFFF'FFFFu) == static_cast<int32_t>(-1),
                "We require two's complement in our reasoning; C++20 guarantees it; below that, no, but in "
                  "reality yes.");

  using std::upper_bound;

  // See m_bucket_val0s doc header for this algorithm's overview; this follows that outline.

  const auto bucket0_val0 = m_bucket_val0s.front();
  const auto bucket0_sz = m_bucket_val0s[1] - bucket0_val0;
  if (size() == 2)
  {
    // Degenerate case.
    return (val < (bucket0_val0 + bucket0_sz)) ? 0 : 1;
  }
  // else if (size() > 2):

  if (m_linear_bucket_sz_or_0 == 0)
  {
    // General case: O(log n_buckets) for the binary search.
    const auto start_it = m_bucket_val0s.begin();
    const auto idx = upper_bound(start_it, m_bucket_val0s.end(), val) - start_it;
    return (idx == 0) ? 0 : (idx - 1);
  }
  // else if (m_linear_bucket_sz_or_0 != 0): Optimized case: O(1) for some comparisons and arithmetic; no loop.

  if (val <= bucket0_val0)
  {
    return 0;
    /* val below bucket0 min-value => definitely bucket 0 and eliminates worrying about annoying math below.
     * val equal to it => definitely bucket 0 as well, so while the math below would probably work too, might as well
     * just return early anyway. */
  }
  /* else if (val > bucket0_val0):
   * We need to know the guaranteed-positive difference `val - bucket0_val0` a/k/a `offset`.
   * Then we'll just determine which bucket that's in (bucket 0 begins at offset 0 exactly and is of size
   * bucket0_sz, then after that they're all of size m_linear_bucket_sz_or_0).
   *
   * Tempting to just have, say, `auto& offset = val` and calculate it in-place like `offset -= bucket0_val0`,
   * but this can lead to over/underflow trouble for very high or very low `val`.  E.g.,
   * with `offset = val = max_int` and `bucket0_val0 == -1` => offset = `min_int`, when it should be `max_int + 1`.
   *
   * So need to be more careful than that.  Call val and bucket0_val0 V and V0 respectively.  Firstly
   * the answer `offset` must be of the unsigned type with the same width as value_t; call this offset_t.
   * Then it can store the difference between any two V and V0 (with V0 < V) including, e.g.,
   * `max_int` plus one from the prev paragraph.
   *
   * As for how to compute this difference `V - V0`....  If unaccustomed to this topic one can get paranoid.
   * If `V0 >= 0`, then it's fine to simply do `V - V0` and cast to offset_t; but if `V0 < 0`, then we've got
   * various overflow worries.  It's fine, though; the answer is actually simple:
   *   - cast each of V and V0 to offset_t;
   *   - do the subtraction.  The resulting offset_t is correct.
   *
   * This works due to 2's complement and standard guarantees.  In short that is because the cast from signed to
   * unsigned, including from a negative number, is formally guaranteed to wrap-around
   * as needed to yield the correct answer mod(2^N).  We omit the proof here.  (For example
   * `(uint)MAX_INT - (uint)(-1) == X`, where `X` is uint and indeed equals MAX_INT plus one, which is indeed
   * the distance from -1 to MAX_INT.)
   *
   * (Without that, we could have used some more complex logic along the lines of: If V0 is non-negative, then
   * just subtract.  If V0 is negative, then carefully calculate `v0 - value_t.min` and `v - value_t.min`, the only
   * hard case being the latter subtraction, if `v >= 0` -- would have to do some careful casting and negation.
   * Having gotten those two values -- both non-negative now -- subtract one from the other.) */

  using offset_t = std::make_unsigned_t<value_t>; // Range [0, (value_t.max + 1) * 2).
  auto offset = static_cast<offset_t>(val) - static_cast<offset_t>(bucket0_val0);

  // `offset` = how far past first-bucket's min-value is val?  Recall also `offset >= 1`.
  const auto bucket0_sz_pos = static_cast<offset_t>(bucket0_sz); // Just to keep signedness consistent/avoid warns.
  if (offset < bucket0_sz_pos)
  {
    return 0; // val > first-bucket's min-value -and- <= first-bucket's max-value.
  }
  // else: Bucket 0 is eliminated, so now it's irrelevant, and we ~repeat versus the bucket collection 1, 2, ....

  offset -= bucket0_sz_pos;

  // `offset` now = how far past second-bucket's min-value is val?

  /* We've decremented `offset` by at least 1, so adding 1 cannot overflow.  Be careful to not go down to size_t
   * yet; idx might be gigantic at this point. */
  const auto idx = static_cast<offset_t>(1) + (offset / static_cast<offset_t>(m_linear_bucket_sz_or_0));

  const auto n_buckets = size();
  if (idx < static_cast<offset_t>(n_buckets))
  {
    return static_cast<size_t>(idx); // Safe given the `if`.
  }
  // else if (idx >= n_buckets):

  // By our rules last-bucket grabs everything belonging directly to it -- or beyond it.
  return n_buckets - 1;
} // Histogram_counter::idx_of_outcome()

size_t Histogram_counter::size() const
{
  return m_per_bucket_counts.size();
}

Histogram_counter::count_t Histogram_counter::count_for_bucket(size_t idx) const
{
  assert(idx < size());
  return load(m_per_bucket_counts[idx]);
}

void Histogram_counter::clear()
{
  for (auto& per_bucket_count : m_per_bucket_counts)
  {
    store(&per_bucket_count, 0);
  }
}

void Histogram_counter::overwrite_count_for_bucket(size_t idx, count_t new_count)
{
  assert(idx < size());
  store(&m_per_bucket_counts[idx], new_count);
}

void Histogram_counter::to_ostream(std::ostream* os_ptr) const
{
  auto& os = *os_ptr;

  /* E.g.: If n_buckets=4, bucket_sz=5, bucket0_val0=-1, bucket0_sz=3, then output might be:
   *   ..-1..1[3]..6[0]..11[97]..16..[3].
   * Additionally, any max-length sequence (excluding bucket 0) of 2+ buckets with all-0 counts should be
   * compacted into: ..{N}..x[0], where x is the normal last-val for the last 0-bucket in the sequence,
   * and {N} is the length of the 0-sequence, minus one:
   *   ..-1..1[0]..{2}..16..[0] (compacted from ..-1..1[0]..6[0]..11[0]..16..[0])
   *   ..-1..1[0]..{1}..11[0]..16..[3] (compacted from ..-1..1[0]..6[0]..11[0]..16..[3])
   *   ..-1..1[2]..{1}..11[0]..16..[3] (compacted from ..-1..1[2]..6[0]..11[0]..16..[3])
   *
   * (That example, for simplicity, was premised on a linear-scale ctor form (hence bucket_sz, bucket0_val0, etc.).
   * However the output format is the same in general, even if all the bkt widths are different from each other.
   * All that changes is the sequence of indices (above, -1, 1, 6, 11, 16: even after the first pair -- but
   * they could just be not, if one uses the different ctor and a funkier sequence of bucket min-values et al.) */

  const auto n_buckets = size();

  /* Due to the look-ahead/skip scheme below, we need to know both idx-th count and `idx+1`th too in the
   * same iteration; but dupe-load()ing of the same idx, while they can concurrently change, can lead to
   * bugs.  So have to cache it.  This starts it off. */
  value_t next_val = load(m_per_bucket_counts.front());

  size_t skipped0_ct = 0;
  for (size_t idx = 0; idx != n_buckets; ++idx)
  {
    const bool first = idx == 0;
    const bool last = (idx + 1) == n_buckets;
    const auto val = next_val;
    last || (next_val = load(m_per_bucket_counts[idx + 1]));
    const bool val0 = val == 0;

    /* If count is 0, and the next count is also 0, then skip output of this one.
     * (Suppose skipped0_ct is 0 (steady state).  Then:
     * If next-ct is 0, but next-next-ct is >0: `..{1}..0` shall be printed upon reaching next-ct.
     * If next-next-ct is 0 but next-next-next-ct is >0: `..{2}..0` shall be printed upon reaching next-next-ct.  Etc.)
     *
     * If count is 0, and the next count is not 0, show a count of how many were skipped in a row; then normal output.
     * If count > 0, then just normal output.
     *
     * Exception:
     *   - Don't skip 0 if `first`: Bkt0 normal output looks a bit different, so just always output whatever
     *     is in there for consistency at least; start the 0-skipping (if any) from `idx == 1`.
     * Tricksy:
     *   - For `last` there is no next count; time to "flush" at that point; so act as-if next count is not 0. */
    if ((!first) && (!last) && val0 && (next_val == 0))
    {
      ++skipped0_ct;
    }
    else
    {
      if (first)
      {
        os << ".." << m_bucket_val0s.front(); // Special prefix for `idx == 0` (indicates underflows + init value).
      }
      else
      {
        const bool skipped0s = skipped0_ct != 0;
        assert((val0 || (!skipped0s)) && "This-ct=0 => may or may not have skipped 1+ zeroes before us; "
                                         "this-ct>0 => must not have skipped any zeroes before us.");
        if (skipped0s)
        {
          os << "..{" << skipped0_ct << "}"; // "Flush" the skipped 0s in the form of their count.
          skipped0_ct = 0;
        }
      }

      if (last)
      {
        /* Print max-value.  In this case that is min-value + bkt-width - 1.  (There is no "next" min-value.)
         * Plus add special postfix for last bucket (indicates overflows). */
        os << ".." << (m_bucket_val0s[idx] + m_bucket_n_sz - 1) << "..";
      }
      else
      {
        // Print max-value, which is defined as *next* min-value minus 1.
        os << ".." << (m_bucket_val0s[idx + 1] - 1);
      }

      os << '[' << val << ']';
    } // else // if (skip this 0, because the next count is also 0)
  } // for (idx in [0, n_buckets))
} // Histogram_counter::to_ostream()

void reset(Histogram_counter* target_val, const Histogram_counter&)
{
  target_val->clear();
}

std::ostream& operator<<(std::ostream& os, const Histogram_counter& val)
{
  val.to_ostream(&os);
  return os;
}

} // namespace flow::util::stat
