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

#include "flow/util/stat/stat_fwd.hpp"
#include "flow/util/string_view.hpp"
#include "flow/util/util_fwd.hpp"
#include "flow/cfg/cfg_fwd.hpp"
#include <ostream>
#include <atomic>
#include <cassert>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Macros.

/**
 * Declares an individual `Stat_set`-stat-member to be tracked in `flow::util::stat::stats_*()` APIs,
 * except for stats with Stat_type::S_HI_WMARK.  Intended for use inside a `declare_stats()` function body.
 *
 * @see FLOW_UTIL_STAT_DECLARE_HI_WMARK() which is used to declare `HI_WMARK`-type stats.
 *
 * ### Advanced info ###
 * This is of interest, we suspect, only if you want to perhaps write your own `stats_*()`-like function for
 * some custom needs; for example see the doc header for stats_sum() with some relevant discussion.
 *
 * The macro expands to a call to `visitor(...)` (as passed-to `declare_stats()`) for the given data member.
 * `visitor()` shall expect the following args with the following requirements and semantics.  In each case
 * `T` is the type of the `Stat_set` member being processed.
 *   -# `const T* src_val`: The member of an immutable source `const Stat_set* src_stats` -- `src_stats->ARG_m_stat` --
 *      or null if such is not necessary for the `stats_*()` op that's invoking `declare_stats()`, in which case
 *      `src_stats` shall be null to indicate this.
 *   -# `T* target_val`: The member of a mutable target `Stat_set* target_stats` -- `target_stats->ARG_m_stat` --
 *      or null if such is not necessary for the `stats_*()` op that's invoking `declare_stats()`, in which case
 *      `target_stats` shall be null to indicate this.
 *   -# `nullptr`: Ignored here.  See FLOW_UTIL_STAT_DECLARE_HI_WMARK().
 *   -# `auto stat_type_tag`: The type (accumulator, gauge, etc.) of the member encoded in such a way as to be able
 *      determing said type at compile-time via `if constexpr(...stat_type_tag...)`.
 *      It may be ignored if unneeded by the op that's invoking `declare_stats()`.  With this macro this can
 *      be any type except `HI_WMARK`.
 *   -# `String_view name`: The printable name of the member.  It may be ignored if unneeded
 *      by the op that's invoking `declare_stats()`.
 *
 * When writing the `visitor()` lambda passed to a `stats_*()` op, start with the signature implied by
 * that list; then for readability consider replacing each ignored arg with `auto&&` (to avoid clutter
 * and unused-arg warnings or `[[maybe_unused]]`).
 *
 * @param ARG_m_stat
 *        The data member to declare, written exactly as the member identifier (e.g., `m_msg_count` or
 *        `m_rcv.m_n_transmitted`).
 * @param ARG_stat_type
 *        The `X` in `"Stat_type::S_X"` indicating the kind of stat the data member tracks.  Note the `Stat_type::S_`
 *        fragment is to be omitted.
 */
#define FLOW_UTIL_STAT_DECLARE(ARG_m_stat, ARG_stat_type) \
  FLOW_UTIL_SEMICOLON_SAFE \
    ( \
      using Stat_type = ::flow::util::stat::Stat_type; \
      using Member_ptr_type = decltype(&target_stats->ARG_m_stat); \
      constexpr auto FLOW_UTIL_STAT_DECLARE_stat_type = Stat_type::S_##ARG_stat_type; \
      static_assert(FLOW_UTIL_STAT_DECLARE_stat_type != Stat_type::S_HI_WMARK, \
                    "To declare HI_WMARK stat please use FLOW_UTIL_STAT_DECLARE_HI_WMARK()."); \
      visitor(src_stats ? &src_stats->ARG_m_stat : nullptr, \
              target_stats ? &target_stats->ARG_m_stat : nullptr, \
              static_cast<Member_ptr_type>(nullptr), \
              std::integral_constant<Stat_type, FLOW_UTIL_STAT_DECLARE_stat_type>{}, \
              ::flow::util::String_view \
                {name_prefix + ::flow::cfg::value_set_member_id_to_opt_name(#ARG_m_stat, '_')}); \
    )

/**
 * Declares an individual `Stat_set`-stat-member of Stat_type::S_HI_WMARK to be tracked
 * in `flow::util::stat::stats_*()` APIs.  Intended for use inside a `declare_stats()` function body.
 *
 * @see FLOW_UTIL_STAT_DECLARE() which is used to declare non-`HI_WMARK`-type stats.
 *
 * ### Conventions (recommended) ###
 * Convention (order): FLOW_UTIL_STAT_DECLARE() for `ARG_m_gauged_stat` should immediately precede
 * this FLOW_UTIL_STAT_DECLARE_HI_WMARK() for `ARG_m_stat`.
 *
 * Convention (naming): Name the member `<gauged_thing>_hi_wmark`; e.g., `m_n_open_pools` is tracked
 * by `m_n_open_pools_hi_wmark`.
 *
 * ### Advanced info ###
 * This is of interest, we suspect, only if you want to perhaps write your own `stats_*()`-like function for
 * some custom needs; for example see the doc header for stats_sum() with some relevant discussion.
 *
 * Expands to a call to `visitor(...)` for the given data member.
 * `visitor()` shall expect the following args with the following requirements and semantics.  In each case
 * `T` is the type of the `Stat_set` member being processed.
 *   -# `const T* src_val`: See FLOW_UTIL_STAT_DECLARE().
 *   -# `T* target_val`: See FLOW_UTIL_STAT_DECLARE().
 *   -# `const T* hi_wmark_gauged_target_val`: Member of a mutable target `Stat_set* target_stats` --
 *      `target_stats->ARG_m_gauged_stat` -- or null if such is not necessary for the `stats_*()` op
 *      that's invoking `declare_stats()`, in which case `target_stats` shall be null to indicate this.
 *   -# `auto stat_type_tag`: See FLOW_UTIL_STAT_DECLARE().
 *   -# `String_view name`: See FLOW_UTIL_STAT_DECLARE().
 *
 * When writing the `visitor()` lambda... (same as FLOW_UTIL_STAT_DECLARE() doc header says).
 *
 * @param ARG_m_stat
 *        The `HI_WMARK`-type data member to declare, written exactly as the member identifier (e.g., `m_msg_count` or
 *        `m_rcv.m_n_transmitted`).
 * @param ARG_m_gauged_stat
 *        The `GAUGE`-type data member that is being tracked by the high-water-mark `ARG_m_stat`.
 */
#define FLOW_UTIL_STAT_DECLARE_HI_WMARK(ARG_m_stat, ARG_m_gauged_stat) \
  FLOW_UTIL_SEMICOLON_SAFE \
    ( \
      using Stat_type = ::flow::util::stat::Stat_type; \
      static_assert(std::is_same_v<decltype(&target_stats->ARG_m_stat), \
                                   decltype(&target_stats->ARG_m_gauged_stat)>, \
                    "The HI_WMARK stat must be the same C++ type as the GAUGEd stat."); \
      visitor(src_stats ? &src_stats->ARG_m_stat : nullptr, \
              target_stats ? &target_stats->ARG_m_stat : nullptr, \
              target_stats ? &target_stats->ARG_m_gauged_stat : nullptr, \
              std::integral_constant<Stat_type, Stat_type::S_HI_WMARK>{}, \
              ::flow::util::String_view \
                {name_prefix + ::flow::cfg::value_set_member_id_to_opt_name(#ARG_m_stat, '_')}); \
    )

namespace flow::util::stat
{

// Types.

/**
 * Thin proxy wrapping a `const` reference to a `Stat_set`, enabling `ostream << stat::print(my_stats)` syntax.
 * Obtain one via print().
 *
 * @see print()
 *
 * @tparam Stat_set
 *         See stats_to_ostream() for requirements.
 */
template<typename Stat_set>
struct Stat_set_printable
{
  // Data.

  /// The referenced stat set.
  const Stat_set& m_stats;
};

// Template implementations.

template<typename T>
const T& load(const T& val)
{
  return val;
}

template<typename T>
T load(const std::atomic<T>& val)
{
  return val.load(std::memory_order_relaxed);
}

template<typename T>
void store(T* target_val, const boost::type_identity_t<T>& val)
{
  *target_val = val;
}

template<typename T>
void store(std::atomic<T>* target_val, boost::type_identity_t<T> val)
{
  target_val->store(val, std::memory_order_relaxed);
}

template<typename T>
void fetch_add(T* target_val, // Yes, intentionally void.  See doc header.
               const boost::type_identity_t<T>& addend_val)
{
  *target_val += addend_val;
}

template<typename T>
T fetch_add(std::atomic<T>* target_val, boost::type_identity_t<T> addend_val)
{
  return target_val->fetch_add(addend_val, std::memory_order_relaxed);
}

template<typename T>
void fetch_sub(T* target_val, // Yes, intentionally void.  See doc header.
               const boost::type_identity_t<T>& subtrahend_val)
{
  *target_val -= subtrahend_val;
}

template<typename T>
T fetch_sub(std::atomic<T>* target_val, boost::type_identity_t<T> subtrahend_val)
{
  return target_val->fetch_sub(subtrahend_val, std::memory_order_relaxed);
}

template<typename T>
T exchange(std::atomic<T>* target_val, boost::type_identity_t<T> val)
{
  return target_val->exchange(val, std::memory_order_relaxed);
}

template<typename T>
void update_hi_wmark(T* target_val, const boost::type_identity_t<T>& val)
{
  if (*target_val < val)
  {
    *target_val = val;
  }
}

template<typename T>
void update_hi_wmark(std::atomic<T>* target_val, boost::type_identity_t<T> val)
{
  T cur = load(*target_val);
  while ((cur < val) // Over time this is probably false immediately, and we therefore typically no-op.
         && (!target_val->compare_exchange_weak(cur, val, std::memory_order_relaxed, std::memory_order_relaxed)))
  {
    // CAS failed: another thread bumped *target_val; `cur` is now its new value -- re-check `cur < val`.
  }
}

template<typename T>
void reset(T* target_val, const T& fresh_val)
{
  store(target_val, load(fresh_val)); // Exactly as advertised.
}

template<typename Stat_set>
void stats_to_ostream(std::ostream& os, const Stat_set& stats)
{
  bool first = true;
  declare_stats("", &stats, nullptr,
                [&](const auto* val_ptr, auto&&, auto&&, auto&&, String_view name)
  {
    const auto& val = *val_ptr;

    if (!first)
    {
      os << ' ';
    }
    first = false;

    // In particular load(val) efficiently loads/returns copy of atomic<> val; simply returns regular val by value.
    os << name << "=[" << load(val) << ']';
  }); // declare_stats()
  // ADL finds the overload of declare_stats() matching Stat_set.
}

template<typename Stat_set>
Stat_set_printable<Stat_set> print(const Stat_set& stats)
{
  return { stats };
}

template<typename Stat_set, typename... Ctor_args>
std::vector<std::string> stats_field_names(Ctor_args&&... ctor_args)
{
  using std::vector;
  using std::string;

  vector<string> names;
  const Stat_set dummy{std::forward<Ctor_args>(ctor_args)...}; // Typically -- not always -- just {}.
  declare_stats("", &dummy, nullptr,
                [&](auto&&, auto&&, auto&&, auto&&, String_view name)
  {
    names.emplace_back(name.data(), name.size());
  }); // declare_stats()

  return names;
}

template<typename Stat_set>
std::ostream& operator<<(std::ostream& os, const Stat_set_printable<Stat_set>& printable)
{
  stats_to_ostream(os, printable.m_stats);
  return os;
}

template<typename Stat_set>
void stats_reset(Stat_set* target_stats, const Stat_set& fresh_stats)
{
  declare_stats("", &fresh_stats, target_stats,
                [&]([[maybe_unused]] const auto* fresh_val_ptr, [[maybe_unused]] auto* target_val,
                    [[maybe_unused]] const auto* hi_wmark_gauged_target_val_or_null, auto stat_type_tag, auto&&)
  {
    constexpr auto stat_type = decltype(stat_type_tag)::value;

    if constexpr(stat_type == Stat_type::S_ACCUMULATOR)
    {
      /* By ACCUMULATOR doc header rules:
       * "Make it so that `target_stats->m_v` is as-if one just performed:
       *   `store(&target_stats->m_v, load(fresh_stats.m_v));`."
       * If further explains that (1) reset() does this; and (2) user may override it by defining an
       * ADL-findable compatible reset().  (E.g., Histogram_counter does this; namely it does target_val->clear().) */
      reset(target_val, *fresh_val_ptr);
    }
    // else if (GAUGE) // By GAUGE doc header rules: no-op.  The measured state is the measured state.
    else if constexpr(stat_type == Stat_type::S_HI_WMARK)
    {
      /* By HI_WMARK doc header rules: copy the GAUGEd stat.  New measurement period => current = highest.
       * (The gauged value in *target_stats is sound to read regardless of declare-order: a reset is a
       * no-op for GAUGEs, so no pass of ours modifies it.) */
      store(target_val, load(*hi_wmark_gauged_target_val_or_null));
    }
  }); // declare_stats()
} // stats_reset()

template<typename Stat_set>
void stats_assign(Stat_set* target_stats, const Stat_set& src_stats)
{
  declare_stats("", &src_stats, target_stats,
                [&](const auto* src_val_ptr, auto* target_val, auto&&, auto&&, auto&&)
  {
    store(target_val, load(*src_val_ptr));
    /* Sanity-check for the case where T=Histogram_counter: This reduces to `*target_val = *src_val_ptr`,
     * which for each per-bucket counter reduces to store(&target_counter, load(&src_counter)). */
  });
} // stats_assign()

template<typename Stat_set>
void stats_aggregate_one(Stat_set* target_stats, const Stat_set& src_stats)
{
  declare_stats("", &src_stats, target_stats,
                [&](const auto* src_val_ptr, auto* target_val, auto&&, auto stat_type_tag, auto&&)
  {
    constexpr auto stat_type = decltype(stat_type_tag)::value;
    const auto& src_val = *src_val_ptr;

    if constexpr((stat_type == Stat_type::S_ACCUMULATOR) || (stat_type == Stat_type::S_GAUGE))
    {
      // By these two's doc header rules, `a += b` is how to aggregate b into a.
      fetch_add(target_val, load(src_val)); // Type must be `atomic<T_numeric>` or have a reasonable +=.
    }
    else if constexpr(stat_type == Stat_type::S_HI_WMARK)
    {
      // By HI_WMARK doc header rules: essentially take the higher of the two high-water mark values.
      update_hi_wmark(target_val, load(src_val)); // Type must be `atomic<T_numeric>` or have a reasonable <.
    }
  }); // declare_stats()
} // stats_aggregate_one()

template<typename Stat_set, typename It>
void stats_aggregate(Stat_set* target_stats, const It& src_stats_begin, const It& src_stats_end)
{
  assert((src_stats_begin != src_stats_end) && "Must aggregate at least 1 thing.");

  stats_assign(target_stats, *src_stats_begin);
  auto src_stats_it = src_stats_begin;

  size_t n = 1;
  for (++src_stats_it; src_stats_it != src_stats_end; ++src_stats_it)
  {
    stats_aggregate_one(target_stats, *src_stats_it);
    ++n;
  }

  declare_stats("", nullptr, target_stats,
                [&](auto&&, [[maybe_unused]] auto* target_val, auto&&, auto stat_type_tag, auto&&)
  {
    if constexpr(decltype(stat_type_tag)::value == Stat_type::S_GAUGE)
    {
      auto scaled_val = load(*target_val);
      scaled_val /= n; // Type must be `atomic<T_numeric>` or have a reasonable /=.

      store(target_val, scaled_val);
    }
    // else { ACCUMULATORs are already summed; HI_WMARKs = max has been found.  Nothing to do. }
  }); // declare_stats()
} // stats_aggregate()

template<typename Stat_set, typename It>
void stats_sum(Stat_set* target_stats, const It& src_stats_begin, const It& src_stats_end)
{
  assert((src_stats_begin != src_stats_end) && "Must sum at least 1 thing.");

  stats_assign(target_stats, *src_stats_begin);
  auto src_stats_it = src_stats_begin;

  for (++src_stats_it; src_stats_it != src_stats_end; ++src_stats_it)
  {
    declare_stats("", &*src_stats_it, target_stats,
                  [&](const auto* src_val_ptr, auto* target_val, auto&&, auto&&, auto&&)
    {
      fetch_add(target_val, load(*src_val_ptr)); // Type must be `atomic<T_numeric>` or have a reasonable +=.
    });
  }
} // stats_sum()

template<typename Stat_set, typename It>
void stats_aggregate_shards(Stat_set* target_stats, const It& src_stats_begin, const It& src_stats_end,
                            const Stat_set* fresh_stats_from_0_shards)
{
  const bool empty_range = src_stats_begin == src_stats_end;
  assert(((!empty_range) || fresh_stats_from_0_shards) && "Must provide a fresh_stats if no input-shards.");

  /* The algorithm consists of two parts:
   *   -I- Deal with ACC[UMULATOR]s and GAUGEs.
   *   -II- Deal with HWMs (HI_WMARKs).
   * Discussing -II- first, as it's simplest arguably.  "Deal with HWMs" simply means: set each HWM to its
   * correct value in *target_stats.  To wit:
   * Set HWM to max(HWM already in *target_stats, the current GAUGEd value).
   * (By definition of HWM: max seen since last reset; by induction that is the higher of last-recorded HWM
   * and current GAUGEd value.)
   *
   * Easy enough; but note it assumes that by the end of -I- *target_stats contains current GAUGE values.
   * Indeed it will.
   *
   * As for -I-: Let's methodically divide it up.  We've got ACCs and GAUGEs.
   *
   * ACCs: Consider ACC m_x.  target_stats->m_x shall be the aggregation (basically sum) of S.m_x, over
   * all S in the given range.
   *   - Degenerate case: empty_range: There are no shards over which to sum, so as usual leverage
   *     fresh_stats (as in stats_reset()) as the source of zeroes, so to speak.  (Here fresh_stats is
   *     *fresh_stats_from_0_shards; and as assert()ed above that is a valid dereference.)
   * GAUGEs: Consider GAUGE m_x -- the answer is, same as for ACC m_x: sum it or use fresh_stats.
   *   - Note this will finalize target_stats->m_x as required for II. */

  // Let's do -I-: deal with ACCUMULATORs and GAUGEs.  This part depends on whether empty_range is true.
  if (empty_range)
  {
    // Degenerate case: just grab 'em from *fresh_stats_from_0_shards.
    declare_stats("", fresh_stats_from_0_shards, target_stats,
                  [&]([[maybe_unused]] const auto* fresh_val_ptr, [[maybe_unused]] auto* target_val,
                      auto&&, auto stat_type_tag, auto&&)
    {
      constexpr auto stat_type = decltype(stat_type_tag)::value;
      if constexpr((stat_type == Stat_type::S_ACCUMULATOR) || (stat_type == Stat_type::S_GAUGE))
      {
        reset(target_val, *fresh_val_ptr);
      }
    });
  } // if (empty_range)
  else // if (!empty_range)
  {
    /* Non-degenerate case: run through the range.
     *
     * First, as in stats_aggregate(), copy first src_ thing in range onto *target_stats (then we'll sum the
     * others onto it, similarly) -- but don't touch HI_WMARKs if any.  See below as to why. */
    auto src_stats_it = src_stats_begin;
    declare_stats("", &*src_stats_it, target_stats,
                  [&]([[maybe_unused]] const auto* src_val_ptr, [[maybe_unused]] auto* target_val, auto&&,
                      auto stat_type_tag, auto&&)
    {
      constexpr auto stat_type = decltype(stat_type_tag)::value;
      if constexpr((stat_type == Stat_type::S_ACCUMULATOR) || (stat_type == Stat_type::S_GAUGE))
      {
        store(target_val, load(*src_val_ptr));
      }
      /* else if (HI_WMARK)
       * { Leave *target_stats alone w/r/t this field!  It contains an in-value *and* will store an out-value.
       *   We'll do the update when ready below.  Until then we can't lose the in-value. } */
    }); // declare_stats()

    /* Next, also as in stats_aggregate(), sum the src_ guys past the one already in *target_stats onto
     * the latter (keeping comments light; see stats_aggregate_one()).
     * However, as advertised, HI_WMARK is to be treated specially.  See below as to how.
     *
     * As advertised -- it may be important for the reason stated in the doc header @note to this effect --
     * we only walk this range exactly once (step 1 just above, steps 2, ..., N just below). */
    for (++src_stats_it; src_stats_it != src_stats_end; ++src_stats_it)
    {
      declare_stats("", &*src_stats_it, target_stats,
                    [&]([[maybe_unused]] const auto* src_val_ptr, [[maybe_unused]] auto* target_val, auto&&,
                        auto stat_type_tag, auto&&)
      {
        constexpr auto stat_type = decltype(stat_type_tag)::value;
        if constexpr((stat_type == Stat_type::S_ACCUMULATOR) || (stat_type == Stat_type::S_GAUGE))
        {
          fetch_add(target_val, load(*src_val_ptr));
        }
        /* else if (HI_WMARK):
         * { Tempting to `update_hi_wmark(target_val, ...)` but that's wrong: Need to loop
         *   through the entire range [..., src_stats_end) to get the GAUGEd value fully summed, first,
         *   and only then is it safe to finally (possibly) overwrite the HI_WMARK. } */
      }); // declare_stats()
    } // for (src_stats_it in [src_stats_begin + 1, src_stats_end))
  } // else // if (!empty_range)

  /* Let's do -II-: Now we can finish up the HI_WMARKs.
   * As advertised: ensure for each HI_WMARK it is = highest of the GAUGEd value's states sampled across
   * stats_aggregate_shards() calls like ours, including ours. */
  declare_stats("", nullptr, target_stats,
                [&](auto&&, [[maybe_unused]] auto* target_val,
                    [[maybe_unused]] const auto* hi_wmark_gauged_target_val_or_null, auto stat_type_tag, auto&&)
  {
    if constexpr(decltype(stat_type_tag)::value == Stat_type::S_HI_WMARK)
    {
      update_hi_wmark(target_val, load(*hi_wmark_gauged_target_val_or_null));
    }
  });
} // stats_aggregate_shards()

template<typename Stat_set, typename It>
void stats_reset_shard_aggregate(Stat_set* target_stats, const It& src_stats_begin, const It& src_stats_end,
                                 const Stat_set& fresh_stats_from_0_shards)
{
  const bool empty_range = src_stats_begin == src_stats_end;

  /* As in stats_aggregate_shards() the algorithm consists of two parts:
   *   -I- Deal with ACC[UMULATOR]s and GAUGEs.
   *   -II- Deal with HWMs (HI_WMARKs).
   * Discussing -II- first, as it's simplest arguably.  "Deal with HWMs" simply means: set each HWM to its
   * correct value in *target_stats.  To wit:
   * Set HWM = the current GAUGEd value.  (By definition of HWM: max seen since last reset... which is now.)
   *
   * Easy enough; but note it assumes that by the end of -I- *target_stats contains current GAUGE values.
   * Indeed it will.
   *
   * As for -I-: Let's methodically divide it up.  We've got ACCs and GAUGEs.
   *
   * ACCs: Consider ACC m_x.
   *   - target_stats->m_x: Does not matter.  *target_stats is not being stat-consumed; its whole purpose is
   *     to ultimately end up with the correct HWMs, so that the next stat-consumption inductively can compute same.
   *     So: No-op for target_stats->m_x.
   *   - For each shard S in range: reset(&S.m_x, <from fresh_stats_from_0_shards>).  *target_stats is not being
   *     stat-consumed now, but when it does occur the ACC m_x will have been reset (and possibly further
   *     incremented again as normal).
   *     - Degenerate case: empty_range: Nothing to do.  No shards to reset -- shards that appear before
   *       the next stat-consumption will be initially fresh.
   * GAUGEs: Consider GAUGE m_x.
   *   - For each shard S in range: No-op w/r/t S.m_x.  By definition of GAUGE, a reset has no effect on it.
   *     - Degenerate case: empty_range: Can't get more degenerate than no-op.  Still no-op.
   *   - target_stats->m_x: In and of itself, does not matter: Next stat-consumption will just overwrite it.
   *     However (!): remember the prerequisite for -II- above: target_stats->m_x must be computed as-if
   *     being stat-consumed (so that -II- can then assign the corresponding HWM member, if any, to this value).
   *     (There's a bit of an inefficiency, in that this is a waste for a GAUGE without an HWM.  @todo Maybe revisit.)
   *     - Non-degenerate case: See stats_aggregate_shards().  Namely what it does is: Sum it.
   *     - Degenerate case: empty_range: See <ditto>.  Namely what it does is: Grab it from
   *       fresh_stats_from_0_shards. */

  if (empty_range)
  {
    // See above for reference.  What that reduces to is simply reset each GAUGE with help of fresh_stats_from_0_shards.
    declare_stats("", &fresh_stats_from_0_shards, target_stats,
                  [&]([[maybe_unused]] const auto* fresh_val_ptr, [[maybe_unused]] auto* target_val, auto&&,
                      auto stat_type_tag, auto&&)
    {
      if constexpr(decltype(stat_type_tag)::value == Stat_type::S_GAUGE)
      {
        reset(target_val, *fresh_val_ptr);
      }
    });
  } // if (empty_range)
  else // if (!empty_range)
  {
    /* See above for reference.  Essentially it's the non-degenerate branch of -I- in stats_aggregate_shards()
     * but limited to GAUGEs only; while for ACCs we reset() each one.  Keeping comments light: refer to
     * stats_aggregate_shards() if needed; but note how ACCs are handled differently.
     *
     * Key thing: As promised: a single walk through the range. */

    auto src_stats_it = src_stats_begin;

    const auto reset_shard_accs = [&]() // Little helper.
    {
      declare_stats("", &fresh_stats_from_0_shards, &*src_stats_it,
                    [&]([[maybe_unused]] const auto* fresh_val_ptr, [[maybe_unused]] auto* target_shard_val, auto&&,
                        auto stat_type_tag, auto&&)
      {
        if constexpr(decltype(stat_type_tag)::value == Stat_type::S_ACCUMULATOR)
        {
          reset(target_shard_val, *fresh_val_ptr);
        }
      });
    };

    // Plop shard 1 onto *target_stats (again: GAUGEs only).
    declare_stats("", &*src_stats_it, target_stats,
                  [&]([[maybe_unused]] const auto* src_val_ptr, [[maybe_unused]] auto* target_val, auto&&,
                      auto stat_type_tag, auto&&)
    {
      if constexpr(decltype(stat_type_tag)::value == Stat_type::S_GAUGE)
      {
        store(target_val, load(*src_val_ptr));
      }
    });
    reset_shard_accs(); // ...and reset shard 1's ACCs.

    // Sum shards 2, 3, ... onto *target_stats (again: GAUGEs only).
    for (++src_stats_it; src_stats_it != src_stats_end; ++src_stats_it)
    {
      declare_stats("", &*src_stats_it, target_stats,
                    [&]([[maybe_unused]] const auto* src_val_ptr, [[maybe_unused]] auto* target_val, auto&&,
                        auto stat_type_tag, auto&&)
      {
        if constexpr(decltype(stat_type_tag)::value == Stat_type::S_GAUGE)
        {
          fetch_add(target_val, load(*src_val_ptr));
        }
      });
      reset_shard_accs(); // ...and reset shard N's ACCs.
    } // for (src_stats_it in [src_stats_begin + 1, src_stats_end))
  } // else // if (!empty_range)

  // Let's do -II-: Now we can finish up the HI_WMARKs.
  declare_stats("", nullptr, target_stats,
                [&](auto&&, [[maybe_unused]] auto* target_val,
                    [[maybe_unused]] const auto* hi_wmark_gauged_target_val_or_null, auto stat_type_tag, auto&&)
  {
    if constexpr(decltype(stat_type_tag)::value == Stat_type::S_HI_WMARK)
    {
      store(target_val, load(*hi_wmark_gauged_target_val_or_null));
    }
  });
} // stats_reset_shard_aggregate()

template<typename Stat_set>
void stats_since_reset_state(Stat_set* target_stats, Stat_set* reset_state)
{
  declare_stats("", reset_state, target_stats,
                [&]([[maybe_unused]] const auto* reset_val_ptr, [[maybe_unused]] auto* target_val,
                    [[maybe_unused]] const auto* hi_wmark_gauged_target_val_or_null, auto stat_type_tag, auto&&)
  {
    constexpr auto stat_type = decltype(stat_type_tag)::value;
    const auto& reset_val = *reset_val_ptr;

    if constexpr(stat_type == Stat_type::S_ACCUMULATOR)
    {
      // Accumulator: The relative value = current raw value minus reference raw value.
      fetch_sub(target_val, reset_val); // Note that for non-`atomic`s this means it must have `-=`.
      /* Sanity-check for the case where T=Histogram_counter: This reduces to `*target_val -= reset_val`,
       * which for each per-bucket counter reduces to fetch_sub(&target_counter, load(&reset_counter)). */
    }
    // else if (GAUGE) // By GAUGE rules: no-op.  The measured state is the measured state.
    else if constexpr(stat_type == Stat_type::S_HI_WMARK)
    {
      /* As for the HI_WMARK: Per doc header: *target_val is currently garbage and should be set either to
       * highest GAUGE's value seen since reset; which by induction is either the HI_WMARK recorded in
       * reset_val already, or the new just-observed GAUGE's value -- if that is higher still.  To keep the
       * induction going for next time, in the latter case also update reset_val.
       *
       * That last sentence... we were so very close to being able to simply do `store(reset_val_ptr, new_gauge)`
       * below, in the case where `new_gauge < cur_hwm` is false (so, turn the ?: into `if/else` and add that
       * store() to else{}).  Unfortunately *reset_val_ptr is a `const T&`, not a `T&`, so ya can't store() in it;
       * const-violation.  Truth be told, it is defeatable with a const_cast<>, and that is maybe defensible --
       * we'd be abusing declare_stats()'s official signature, but it's essentially our own signature -- but I
       * (ygoldfel) just can't make myself do it.  So let's do a final separate pass to update these HI_WMARKs
       * in *reset_state.  It's slower (very unlikely that matters in this context) but cleaner.  (The alternative
       * is to expand Visitor signature to cover another op-configuration (0-2 outputs instead of 0-1 of them)
       * which is probably decent but subjectively getting overly hairy for what one pass can officially do.) */
      const auto new_gauge = load(*hi_wmark_gauged_target_val_or_null);
      const auto cur_hwm = load(reset_val);

      store(target_val, (new_gauge < cur_hwm) ? cur_hwm
                                              : new_gauge); // (<-- aforementioned would-be else{}.)
      /* ^-- Instead of < and store()/load()ing could also use something update_hi_wmark()-y; but expressiveness
       * is probably better this way.  (Either way `<` is required.)  Also the above+below is multi-step;
       * we don't even wish to contemplate how it behaves under concurrent modification and therefore advertise
       * as resulting in undefined-behavior in such scenarios. */
    } // else if (HI_WMARK)
  }); // declare_stats()

  // OK, so since we decided not to abuse the system by updating *reset_state HIGH_WMARKs above, do it in separate pass.
  declare_stats("", target_stats, reset_state,
                [&]([[maybe_unused]] const auto* target_val, [[maybe_unused]] auto* reset_val,
                    auto&&, auto stat_type_tag, auto&&)
  {
    if constexpr(decltype(stat_type_tag)::value == Stat_type::S_HI_WMARK)
    {
      update_hi_wmark(reset_val, load(*target_val));
    }
  });
} // stats_since_reset_state()

template<typename Stat_set>
void stats_mark_reset_state(Stat_set* target_stats)
{
  /* Reminder: Regardless of the name (it has to do with stats_since_marked_state()) we are to simply update
   * all HI_WMARKs to their source GAUGEs' current values; and ignore all else. */

  declare_stats("", nullptr, target_stats,
                [&](auto&&, [[maybe_unused]] auto* target_val,
                    [[maybe_unused]] const auto* hi_wmark_gauged_target_val_or_null, auto stat_type_tag, auto&&)
  {
    if constexpr(decltype(stat_type_tag)::value == Stat_type::S_HI_WMARK)
    {
      store(target_val, load(*hi_wmark_gauged_target_val_or_null));
    }
  });
}

} // namespace flow::util::stat
