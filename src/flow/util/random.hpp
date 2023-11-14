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

#include "flow/util/util_fwd.hpp"
#include <boost/random.hpp>
#include <limits>

namespace flow::util
{

/**
 * Base class for Rnd_gen_uniform_range and Rnd_gen_uniform_range_mt for various aliases and similar, so
 * template arguments need not be involved.
 */
class Rnd_gen_uniform_range_base
{
public:
  // Types.

  /**
   * The random generator engine; it is public for the reason explained in Usability section of the
   * Rnd_gen_uniform_range doc header.
   */
  using Random_generator = boost::random::mt19937;
};

/**
 * Simple, non-thread-safe uniform-range random number generator.  This merely wraps the perfectly nice, advanced
 * random number generation facilities for this common, simple use case.
 *
 * ### Thread safety ###
 * The random number generation itself is not safe for concurrent invocation on the same `this`.
 * Use Rnd_gen_uniform_range_mt if this is not acceptable.
 *
 * ### Usability, performance discussion ###
 * This class is hopefully appealingly simple; but please note that if you are using N=2+ instances of it for N
 * different uniform ranges (let alone other distributions), you could instead use boost.random directly: namely
 * institute a single RNG and then generate values against various distributions from that shared RNG.  In particular,
 * this would require only one seed (arguably increases code simplicity in that respect); and might be faster
 * since there is one raw RNG engine instead of N (as would be the case if one used N `Rnd_gen_uniform_range`s).
 *
 * If one chooses to do as suggested in the previous paragraph, Rnd_gen_uniform_range_base::Random_generator is a good
 * choice for the RNG itself (but of course one is free to use another one, if this one's mathematical/perf properties
 * are not acceptable).
 *
 * @tparam range_t
 *         Type of values generated; must be suitable for `boost::random::uniform_int_distribution`.
 *
 * @todo Rnd_gen_uniform_range and Rnd_gen_uniform_range_mt, on reflection, are very similar to
 * `boost::random::random_number_generator`, and should be written to conform to it, so as to be usable in
 * `std::random_shuffle()`, and also to gain its appealing extra features without losing any of the already-available
 * simplicity.  Note, however, that `std::random_shuffle()` is deprecated (and gone in C++17) in favor of
 * `std::shuffle()`, which is available from C++11 on, and which works directly with a formal URNG
 * such as Rnd_gen_uniform_range_base::Random_generator.  So this to-do is less about that and more about gaining
 * those other features while being suitable for `random_shuffle()`, which some code does still use out there.
 *
 * @internal
 *
 * Regarding the above to-do: Here is how `boost::random::random_number_generator` differs from this class and
 * the implications:
 *   - It accepts a URNG as a template arg.  We should do that too while defaulting to
 *     Rnd_gen_uniform_range_base::Random_generator.
 *   - It accepts a URNG reference as a ctor arg.  We should do that too while also providing a ctor that causes us to
 *     create our own, like now; in the latter case we would pass the ctor `seed` arg into it.  In the former case
 *     it should invoke `URNG.seed()` to set the seed while requiring that URNG is a
 *     boost.random PseudoRandomNumberGenerator (so that it can take `.seed()`).  Do note that taking a seed this way
 *     is an added feature we have.
 *   - Its `operator()` accepts N and returns a value in [0, N - 1], whereas ours takes no args and returns
 *     something in the range [min, max] given in ctor.  We must (in order to conform to what
 *     `std::random_shuffle()` expects) provide what they provide; but we should also feature two more `operator()`
 *     overloads:
 *     - No-args: Same as now.
 *     - Two args min, max: Return from [min, max].
 *   - It is interesting that its implementation as of Boost 1.76.0 (at least) creates a new equivalent of
 *     #m_seq in *each* `operator()` call.  Is that slower than our impl?  However, since no one is screaming about
 *     its slowness, maybe we should conclude the way they do it is fine; and it does have an advantage:
 *     one can implement the non-zero-arg `operator()` trivially.  Perhaps we should do what they do for the
 *     non-zero-arg overloads; while doing what we do for the zero-arg one.
 *
 * Lastly we also provide Rnd_gen_uniform_range_mt, which is a feature they lack, and we can trivially extend it
 * to cover all of the above suggested changes.
 */
template<typename range_t>
class Rnd_gen_uniform_range :
  public Rnd_gen_uniform_range_base,
  private boost::noncopyable
{
public:
  // Constuctors/destructor.

  /**
   * Begins the random number generation sequence.  Current time is used as the seed.
   *
   * @param min
   *        Generated numbers will be in range `[min, max]`.
   * @param max
   *        See `min`.
   */
  Rnd_gen_uniform_range(range_t min = 0, range_t max = std::numeric_limits<range_t>::max());

  /**
   * Begins the random number generation sequence.  Seed is to be given as an argument.
   *
   * @param min
   *        Generated numbers will be in range `[min, max]`.
   * @param max
   *        See `min`.
   * @param seed
   *        Seed.  Supply the same value to yield the same sequence repeatedly.
   */
  Rnd_gen_uniform_range(uint32_t seed, range_t min, range_t max);

  // Methods.

  /**
   * Returns the next pseudo-random number in `[min, max]` provided in constructor.
   * Not thread-safe for a given `this`.
   *
   * @return Ditto.
   */
  range_t operator()();

private:
  // Data.

  /// Random number generator engine.
  Random_generator m_gen;
  /// Uniform-distribution sequence in `[min, max]` backed by #m_gen.
  boost::random::uniform_int_distribution<range_t> m_seq;
}; // class Rnd_gen_uniform_range

/**
 * Identical to Rnd_gen_uniform_range but safe for concurrent RNG given a single object.
 *
 * @tparam range_t
 *         Same as for Rnd_gen_uniform_range.
 */
template<typename range_t>
class Rnd_gen_uniform_range_mt :
  private Rnd_gen_uniform_range<range_t>
{
public:
  // Constructors/destructor.

  // Delegate the exact constructors from Rnd_gen_uniform_range.
  using Rnd_gen_uniform_range<range_t>::Rnd_gen_uniform_range;

  // Methods.

  /**
   * Returns the next pseudo-random number in `[min, max]` provided in constructor.
   * Thread-safe for a given `this`.
   *
   * @return Ditto.
   */
  range_t operator()();

private:
  // Data.

  /// Mutex that protects against concurrency-based corruption.
  flow::util::Mutex_non_recursive m_mutex;
};

// Template implementations.

template<typename range_t>
Rnd_gen_uniform_range<range_t>::Rnd_gen_uniform_range(uint32_t seed, range_t min, range_t max) :
  m_gen(static_cast<typename decltype(m_gen)::result_type>(seed)),
  m_seq(min, max)
{
  // Nothing else.
}

template<typename range_t>
range_t Rnd_gen_uniform_range<range_t>::operator()()
{
  return m_seq(m_gen);
}

template<typename range_t>
Rnd_gen_uniform_range<range_t>::Rnd_gen_uniform_range(range_t min, range_t max) :
  Rnd_gen_uniform_range(static_cast<unsigned int>(flow::util::time_since_posix_epoch().count()), min, max)
{
  // Nothing else.
}

template<typename range_t>
range_t Rnd_gen_uniform_range_mt<range_t>::operator()()
{
  Lock_guard<decltype(m_mutex)> lock(m_mutex);
  return static_cast<Rnd_gen_uniform_range<range_t>&>(*this)();
}

} // namespace flow::util
