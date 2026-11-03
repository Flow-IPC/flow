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
#include <type_traits>
#include <boost/array.hpp>

namespace flow::util::stat
{

// Types.

/**
 * Dead-simple collection (essentially a statically-sized array) of up-to-`N` `Stat_set`s, for use
 * as a lightning-fast way to choose a category and update the `Stat_set` relevant to it.
 *
 * So, for example, stats_default() and stats_mutable_default() (a/k/a `stats[_mutable]<0>()`) resolves to,
 * basically, at compile-time, `(Stat_set*)this + 0`; whereas `stats[_mutable]_at(idx_for_a_particular_category)`
 * resolves to `(Stat_set*)this + idx_for_a_particular_category` at run-time.  Either way, it is just about
 * as quick as a quasi-associative lookup can be.  We've consciously avoided anything dynamic like a hash-map,
 * or even a `vector` (which, while quick for a read-only lookup, could involve reallocations and associated
 * difficulties).  Dead-simple.
 *
 * It really *is* an `array<Stat_set, S_N>` -- initialized to hold #S_N default-constructed `Stat_set`s --
 * albeit with syntactic sugar for convenient/expressive access.
 *
 * @note As a consequence, it is copy-ctible and copy-assignable if and only if the aforementioned `array`
 *       type is.  To wit: `Stat_set` is <=> a `Stat_set_list<Stat_set, N>` is.  This simple semantic is
 *       why this class template lacks constructors (as does `array` generally).
 *
 * Hence:
 *   - `Stat_set_list<Stat_set, N> stats;` // Constructs N things like this: Stat_set{}.
 *   - `auto stats2 = stats;` // Constructs N more like this: Stat_set{stats[idx]}.
 *
 * @see Global_stats which can provide global-singleton access to a Stat_set_list.
 *
 * @tparam Stat_set_t
 *         #Stat_set is a `Stat_set` of your choice, subject to the patterns and requirements
 *         documented in the namespace util::stat doc header.  An additional requirement in this context is
 *         it must be default-constructible: `Stat_set{}`.  (It would not be impossible to loosen this
 *         requirement, but it brings certain conveniences, so to the extent the requirement can survive the
 *         user's needs over time, it should.)
 * @tparam N
 *         The number of `Stat_set`s `*this` can hold.  Informally speaking, this does not mean all #S_N
 *         slots *must* be actively used.  A suggested pattern is: some module may want to, in the future,
 *         hold 2+ `Stat_set`s depending on the category; but in the meantime it can use `S_N == 1` and
 *         `stats_[mutable_]default()` for everything.  Adding more fine-grained stat-keeping later, then, could
 *         involve few to no breaking API changes in this module: perhaps some alias would change from
 *         `using Stats = flow::util::Stat_set_list<My_cool_stats, 1>` to
 *         `using Stats = flow::util::Stat_set_list<My_cool_stats, MAX_COOL_STAT_CATEGORY>`, while
 *         an accessor would remain `Stats& stats()`.
 */
template<typename Stat_set_t, size_t N>
class Stat_set_list
{
public:
  // Types.

  /// The template parameter `Stat_set_t`.
  using Stat_set = Stat_set_t;

  static_assert(std::is_default_constructible_v<Stat_set>,
                "Stat_set must be default-constructible (T{} required).");

  // Constants.

  /// The template parameter `N`.
  static constexpr size_t S_N = N;

  static_assert(S_N != 0, "Have to store at least 1 Stat_set.");

  // Methods.

  /**
   * Read-only access to the 0th `Stat_set`: for stat consumption.
   *
   * This slot always exists, as `S_N >= 1`, so this will always compile and execute safely.
   *
   * @return See above.
   */
  const Stat_set& stats_default() const;

  /**
   * Mutable access to the 0th `Stat_set`: for stat updates.  Otherwise identical to stats_default(); see that.
   * @return See above.
   */
  Stat_set& stats_mutable_default();

  /**
   * Read-only access to the compile-time-specified-slot-indexed `Stat_set`: for stat consumption.
   *
   * Will not compile, if #S_N is too low.
   *
   * Rationale: This is not appreciably faster than stats_at() (which takes an `idx` at run-time), but using
   * it where possible can enhance safety by causing compile errors instead of letting a range-check fail at
   * run-time.
   *
   * @tparam IDX
   *         The slot to access.  See also stats_default().
   * @return See above.
   */
  template<size_t IDX>
  auto stats() const -> const Stat_set&; // @todo See definition for reason for the odd signature form (Doxygen).

  /**
   * Mutable access to the compile-time-specified-slot-indexed `Stat_set`: for stat updates.
   * Otherwise identical to stats(); see that.
   *
   * @tparam IDX
   *         See above.
   * @return See above.
   */
  template<size_t IDX>
  auto stats_mutable() -> Stat_set&; // @todo See definition for reason for the odd signature form (Doxygen).

  /**
   * Read-only access to the run-time-specified-slot-indexed `Stat_set`: for stat consumption.
   *
   * If #S_N is too low, behavior is undefined (assertion may trip).
   *
   * @param idx
   *        The slot to access.
   * @return See above.
   */
  const Stat_set& stats_at(size_t idx) const;

  /**
   * Mutable access to the run-time-specified-slot-indexed `Stat_set`: for stat updates.
   * Otherwise identical to stats_at(); see that.
   *
   * @param idx
   *        See above.
   * @return See above.
   */
  Stat_set& stats_mutable_at(size_t idx);

  /// Purely as syntactic sugar: executes `stats_reset(&s, {})` for each `Stat_set s` of #S_N stored in `*this`.
  void reset();

private:
  /// Data.

  /// What `*this` is.
  boost::array<Stat_set, S_N> m_sets;
}; // class Stat_set_list

/**
 * A straightforward global/singleton holder of a single `Stat_set_list<Stat_set, S_N>`; the template parameter
 * #Tag is used to arrange for an independent such `Stat_set_list` per distinct #Tag type.
 *
 * The `static` accessor get() is the API:
 *   - get() shall thread-safely construct the `Stat_set_list` the first time it is invoked (even before/after
 *     `main()`).
 *   - get() shall subsequently return ref to that same `Stat_set_list`.
 *   - `Global_stats<Tag, Stat_set, S_N>::get()` shall always return the same address.
 *   - Each distinct, compile-time value of the tuple (#Tag, #Stat_set, #S_N) shall yield
 *     a distinct address returned by get().
 *
 * Performance: get() is almost identical in performance to simply returning the value of a `static`/global
 * pointer variable.  In practice it is 1-2 cycles costlier; this is negligible in 99.9999999999% of situations.
 *
 * @note One can, instead, simply maintain their own `Stat_set_list<My_cool_stats, NUM_CATS> s_stat_sets`, or
 *       their own on-demand `static` getter.  Global_stats provides an alternative way of organizing global
 *       access to such a thing, with segregation by #Tag and on-demand creation; that's all.  Your call!
 *
 * @tparam Tag_t
 *         #Tag can be any type and is used exclusively (and exclusively at compile-time) to disambiguate
 *         1+ `Stat_set_list`s with the same #Stat_set and #S_N.
 * @tparam Stat_set_t
 *         See Stat_set_list.
 * @tparam N
 *         See Stat_set_list.
 */
template<typename Tag_t, typename Stat_set_t, size_t N>
class Global_stats
{
public:
  // Types.

  /// The template parameter `Tag_t`.
  using Tag = Tag_t;

  /// See Stat_set_list.
  using Stat_set = Stat_set_t;

  // Constants.

  /// See Stat_set_list.
  static constexpr size_t S_N = N;

  // Methods.

  /**
   * Thread-safely default-constructs or accesses the one `Stat_set_list<Stat_set, S_N>` for #Tag.
   * @return See above.
   */
  static Stat_set_list<Stat_set, N>& get();

private:
  // Constructors/destructor.

  /// Forbid instantiation, for it is pointless.
  Global_stats() = delete;
}; // class Global_stats

// Template implementations.

template<typename Stat_set_t, size_t N>
template<size_t IDX>
auto Stat_set_list<Stat_set_t, N>::stats() const -> const Stat_set_t&
// Doxygen 1.9.4 gets confused here otherwise; the `->` form is a work-around for that.  @todo Revisit with later ver.
{
  // Not that pretty, but it is just to avoid copy/pasting the static_assert().
  return const_cast<Stat_set_list*>(this)->stats_mutable<IDX>();
}

template<typename Stat_set_t, size_t N>
template<size_t IDX>
auto Stat_set_list<Stat_set_t, N>::stats_mutable() -> Stat_set_t& // @todo See above.
{
  static_assert(IDX < S_N, "Compile-time Stat_set index exceeds compile-time capacity.");
  return m_sets[IDX];
}

template<typename Stat_set_t, size_t N>
const Stat_set_t& Stat_set_list<Stat_set_t, N>::stats_at(size_t idx) const
{
  // Not that pretty, but it is just to avoid copy/pasting the assert().
  return const_cast<Stat_set_list*>(this)->stats_mutable_at(idx);
}

template<typename Stat_set_t, size_t N>
Stat_set_t& Stat_set_list<Stat_set_t, N>::stats_mutable_at(size_t idx)
{
  assert((idx < S_N) && "Run-time Stat_set index exceeds compile-time capacity.  "
                          "Can use stats() for compile-time safety, if `idx` is known at compile-time.");
  return m_sets[idx];
}

template<typename Stat_set_t, size_t N>
const Stat_set_t& Stat_set_list<Stat_set_t, N>::stats_default() const
{
  return stats<0>();
}

template<typename Stat_set_t, size_t N>
Stat_set_t& Stat_set_list<Stat_set_t, N>::stats_mutable_default()
{
  return stats_mutable<0>();
}

template<typename Stat_set_t, size_t N>
void Stat_set_list<Stat_set_t, N>::reset()
{
  for (auto& stats : m_sets)
  {
    stats_reset(&stats, {});
  }
}

template<typename Tag_t, typename Stat_set_t, size_t N>
Stat_set_list<Stat_set_t, N>& Global_stats<Tag_t, Stat_set_t, N>::get() // Static.
{
  /* Construct once per <Tag, Stat_set, N> tuple per process.
   * Compared to just having a static s_singleton data member and returning it here:
   *   - avoids static-init-ordering hell (will be constructed on-demand);
   *   - (after the initial construct) is 1-2 cycles slower (negligible). */
  static Stat_set_list<Stat_set, S_N> s_singleton;
  return s_singleton;
}

} // namespace flow::util::stat
