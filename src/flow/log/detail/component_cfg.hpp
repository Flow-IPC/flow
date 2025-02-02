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

#include "flow/log/detail/log_fwd.hpp"
#include <algorithm>
#include <utility>

namespace flow::log
{

// Types.

/**
 * An internal-use dictionary for fast lookup of small `Cfg` values keyed by
 * `type_info` objects; impl = 1 `Component_payload_type_dict_by_val_...`
 * and 1 `Component_payload_type_dict_by_ptr_...`.
 *
 * The idea is that Component_payload_type_dict_by_val_via_map and other `Component_payload_type_dict_by_val_...`s
 * are fast but require that one `insert(X)` only if `&Y = &X` for all `Y == X`; while
 * the `..._by_val_...` counteparts are slower but work with any `X` types.  So an object of the present class
 * combines both: when one calls `insert(X)`, one specifies whether `type_info X` is guaranteed to live at exactly
 * that one address; or possibly not.  We then store it, and lookup() will try to leverage the stored items so as to
 * be as fast as it can be (but no faster).
 *
 * In order to maximize the chances of a well-performing lookup(), the user shall strive to (correctly!) set
 * the `type_info_ptr_is_uniq` arg to `true` when calling insert(), whenever possible.
 *
 * @tparam Dict_by_ptr_t
 *         A `Component_payload_type_dict_by_ptr_...` template instance.
 * @tparam Dict_by_var_t
 *         A `Component_payload_type_dict_by_val_...` template instance such that
 *         `Dict_by_ptr_t::Cfg` and `Dict_by_val_t::Cfg` are the same type (meaning they store the same
 *         mapped-payloads).
 */
template<typename Dict_by_ptr_t, typename Dict_by_val_t>
class Component_payload_type_dict
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Dict_by_ptr = Dict_by_ptr_t;
  /// Convenience alias for template arg.
  using Dict_by_val = Dict_by_val_t;
  /// Convenience alias for the mapped-type looked-up by a `*this`.
  using Cfg = typename Dict_by_ptr::Cfg;

  static_assert(std::is_same_v<Cfg, typename Dict_by_val::Cfg>,
                "The two dictionary types must hold the same-typed Cfg payloads.");

  // Methods.

  /**
   * Adds a mapping from the given type to a `Cfg` value, so it can be obtained by lookup().
   *
   * @param type
   *        A `typeid(...)` value.  Duplicate insert() calls are not allowed: if `this->lookup(X)`, with
   *        `X == type`, has been previously invoked, then behavior is undefined (assertion might trip).
   * @param cfg
   *        A value.
   * @param type_info_ptr_is_uniq
   *        Firstly, whenever you can set this to `true`, you should do so in order to improve performance.
   *        Secondly, doing so means you offer the following guarantee: If `this->lookup(X)` is subsequently called,
   *        and `X == type`, then `&X == &type`.  If `false` then you're not making this guarantee (note that doesn't
   *        mean you promise there *is* an `X == type` where `&X == &type` -- only that you can't say there isn't).
   *        Tips:
   *        In practice, if `this->insert(type)` and `this->lookup(X)` (with `X == type`) shall always be called
   *        *not* from either side of shared-object boundary, then `type_info_ptr_is_uniq = true` is safe.
   *        Corollary: if all relevant code is statically linked, then it is also safe.
   */
  void insert(const std::type_info& type, Cfg cfg, bool type_info_ptr_is_uniq);

  /**
   * Looks up a value previously added via insert(), or indicates no such value has yet been added.
   * Informally: we strive to optimize the performance of this method, expecting it to potentially be called
   * very frequently.
   *
   * @param type
   *        A `typeid(...)` value.  The lookup shall be successful if and only if `insert(X)` has been called
   *        previously such that `X == type`.
   * @param cfg
   *        `*cfg` shall be set to the looked-up value previously `insert()`ed; or it shall be left untouched
   *        if not found.
   * @return `true` if and only if `*cfg` was assigned (i.e., the lookup succeeded).
   */
  bool lookup(const std::type_info& type, Cfg* cfg) const;

private:
  // Data.

  /// The "fast" dictionary (wherein `insert(type, ..., true)`-originated values are stored).
  Dict_by_ptr m_dict_by_type_ptr;
  /// The "slow" dictionary (wherein `insert(type, ..., false)`-originated values are stored).
  Dict_by_val m_dict_by_type;
}; // class Component_payload_type_dict

/**
 * An internal-use dictionary for fast lookup of small `Cfg` values keyed by
 * `type_info` objects, given the guarantee that any type involved shall have 1 uniquely-addressed such object;
 * impl = map.
 *
 * Note: The API/semantics documented here are shared by all other `Component_payload_type_dict_by_ptr_*` types.
 *
 * Formally, the key requirement for using this type (and the equivalent aforementioned ones) is that for any
 * two `type_info` objects `A` and `B` that might ever be given to `this->insert()` and `this->lookup()`
 * respectively, if `&A != &B`, then `A != B` as well.  I.e., `x = typeid(...)` for one distinct type `...`
 * shall be guaranteed to yield the same `&x` within a given program invocation, for all `...` used with a `*this`.
 * In practice, this is the case *if* the relevant `typeid()` calls are
 * never invoked from translation units with a shared-object boundary between them.  (E.g., if everything relevant
 * is statically linked, then that is the case.)
 *
 * ### Rationale ###
 * This is (at least) potentially used by log::Config, internally, to map Component::payload_type() values to
 * log-config table indices.
 *
 * Note: The API/semantics documented here are also shared by all `Component_payload_type_dict_by_val_*` types,
 * except that the aforementioned `type_info` address uniqueness guarantee cannot be made for those types.  Generally
 * *our* implementation(s) tend to be faster than their implementation(s), because internally lookup by pointer
 * is faster than lookup by `std::type_index` which (due to the possibility of dynamic linking) must involve hashing
 * and/or comparison of type name strings.
 *
 * @tparam Map_to_cfg_t
 *         Internally-used map type of choice.  It must have the API common to `std::map` and `std::unordered_map`;
 *         and must have `key_type` = `std::type_info*`.  In other words, it must be:
 *         `...map<std::type_info*, Cfg_t>`: substitute your `...map` of choice (e.g., a tree or hash map from
 *         Boost or STL); and your small, copyable `Cfg_t` type of choice (e.g., `size_t`).
 */
template<typename Map_to_cfg_t>
class Component_payload_type_dict_by_ptr_via_map
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Map_to_cfg = Map_to_cfg_t;
  /// Convenience alias for the mapped-type looked-up by a `*this`.
  using Cfg = typename Map_to_cfg::mapped_type;

  // Methods.

  /**
   * Adds a mapping from the given type to a `Cfg` value, so it can be obtained by lookup().
   *
   * @param type
   *        A `typeid(...)` value.  Duplicate insert() calls are not allowed: if `this->lookup(X)`, with
   *        `X == type`, has been previously invoked, then behavior is undefined (assertion might trip).
   * @param cfg
   *        A value.
   */
  void insert(const std::type_info& type, Cfg cfg);

  /**
   * Looks up a value previously added via insert(), or indicates no such value has yet been added.
   * Informally: we strive to optimize the performance of this method, expecting it to potentially be called
   * very frequently.
   *
   * @param type
   *        A `typeid(...)` value.  The lookup shall be successful if and only if `insert(X)` has been called
   *        previously such that `X == type`.
   * @param cfg
   *        `*cfg` shall be set to the looked-up value previously `insert()`ed; or it shall be left untouched
   *        if not found.
   * @return `true` if and only if `*cfg` was assigned (i.e., the lookup succeeded).
   */
  bool lookup(const std::type_info& type, Cfg* cfg) const;

private:
  // Data.

  /**
   * A `...map<type_info*, Cfg>`, wherein insert() simply performs `m_dict[&type] = cfg`.
   *
   * ### Performance ###
   * If it is a tree (sorted) map, it is log-time and composed of quite-quick pointer `<` comparisons.
   * If it is a hash map, it is amortized constant-time and composed of quite-quick hashing wherein
   * pointer's value *is* its hash (no calculation required).
   */
  Map_to_cfg m_dict;
};

/**
 * Exactly equivalent to Component_payload_type_dict_by_ptr_via_map but with impl = array with optimized
 * linear search.
 *
 * @tparam Cfg_t
 *         See Component_payload_type_dict_by_ptr_via_map::Cfg.
 */
template<typename Cfg_t>
class Component_payload_type_dict_by_ptr_via_array
{
public:
  // Types.
  /// See Component_payload_type_dict_by_ptr_via_map.
  using Cfg = Cfg_t;
  // Methods.
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   */
  void insert(const std::type_info& type, Cfg cfg);
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   * @return Ditto.
   */
  bool lookup(const std::type_info& type, Cfg* cfg) const;

private:
  // Data.

  /**
   * The values `&type` passed to insert(), in the order in which they were invoked.
   * (Due to insert() contract: they are guaranteed to be distinct pointer values, no repeats.)
   *
   * ### Performance/rationale ###
   * Conceptually, since we don't sort the keys, a linear search through potentially the entire array
   * shall be required.  However, at a lower level, a number of optimizations may come into play when doing this,
   * speeding up the constant factor significantly.  See lookup() for specific details, but here consider this:
   * By placing the search keys into a contiguous buffer (as opposed to interleaving the values by using
   * `vector<pair>` or similar), we have maximized the potential for optimizations (such as a possible `REPNE SCASQ`
   * instruction).  Even if this property ends up not being used, it certainly won't hurt -- and
   * the code is hardly more complex.
   */
  std::vector<const std::type_info*> m_dict_keys;

  /**
   * The values `cfg` passed to insert(), in the order in which they were invoked.
   * (Hence `m_dict_vals.size() == m_dict_keys.size()`, with a given value sitting at the same index as its corresponding
   * key.)
   */
  std::vector<Cfg> m_dict_vals;
};

/**
 * Exactly equivalent to Component_payload_type_dict_by_ptr_via_array but with impl = sorted array susceptible to
 * binary search.
 *
 * @tparam Cfg_t
 *         See Component_payload_type_dict_by_ptr_via_map::Cfg.
 */
template<typename Cfg_t>
class Component_payload_type_dict_by_ptr_via_sorted_array
{
public:
  // Types.
  /// See Component_payload_type_dict_by_ptr_via_map.
  using Cfg = Cfg_t;
  // Methods.
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   */
  void insert(const std::type_info& type, Cfg cfg);
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   * @return Ditto.
   */
  bool lookup(const std::type_info& type, Cfg* cfg) const;

private:
  // Data.

  /**
   * The values pairs (`&type`, `cfg`) passed to insert(), in order of increasing `&type` (pointer) value.
   * (Due to insert() contract: they are guaranteed to be distinct pointer values, no repeats.)
   *
   * ### Performance/rationale ###
   * The values are sorted by key, so as to be binary-searched for log-time performance.  As for the constant
   * factor: a binary search is a higher-level operation, hence realistically lower-level perfomance is mostly
   * outside of our control.  So we can just use a vanilla setup: a `vector` with key-value `pair`s +
   * `std::lower_bound()` for binary search.
   *
   * The constant factor shall involve the quite-quick pointer `<` comparisons.  Versus
   * Component_payload_type_dict_by_ptr_via_sorted_array (array, but linear search instead of binary search),
   * we might do better for larger N, while the potentially-low-level-optimized search-through-contiguous-buffer
   * might cause "them" to do better for smaller N.
   */
  std::vector<std::pair<const std::type_info*, Cfg>> m_dict_sorted;
};

/**
 * An internal-use dictionary for fast lookup of small `Cfg` values keyed by
 * `type_info` objects, given NO guarantee that any type involved shall have 1 uniquely-addressed such object;
 * impl = map.
 *
 * Note: The API/semantics documented here are shared by all other `Component_payload_type_dict_by_val_*` types.
 *
 * Please see Component_payload_type_dict_by_ptr_via_map.  Our API/semantics documented here are the same,
 * except that one may not use `Component_payload_type_dict_by_ptr_*`, when the `&typeid(...)` uniqueness
 * (for a given distinct `...`) guarantee cannot be made.  So in that case one would use
 * a `Component_payload_type_dict_by_val_*` (which, informally, will tend to be slower for lookup()).
 *
 * @tparam Map_to_cfg_t
 *         Internally-used map type of choice.  It must have the API common to `std::map` and `std::unordered_map`;
 *         and must have `key_type` = `std::type_index`.  In other words, it must be:
 *         `...map<std::type_index, Cfg_t>`: substitute your `...map` of choice (e.g., a tree or hash map from
 *         Boost or STL); and your small, copyable `Cfg_t` type of choice (e.g., `size_t`).
 */
template<typename Map_to_cfg_t>
class Component_payload_type_dict_by_val_via_map
{
public:
  // Types.
  /// See Component_payload_type_dict_by_ptr_via_map.
  using Map_to_cfg = Map_to_cfg_t;
  /// See Component_payload_type_dict_by_ptr_via_map.
  using Cfg = typename Map_to_cfg::mapped_type;
  // Methods.
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   */
  void insert(const std::type_info& type, Cfg cfg);
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   * @return Ditto.
   */
  bool lookup(const std::type_info& type, Cfg* cfg) const;
private:
  // Data.

  /**
   * A `...map<type_index, Cfg>`, wherein insert() simply performs `m_dict[type_index(type)] = cfg`.
   *
   * ### Performance ###
   * The performance here is not great, actually, which is why ideally one would use
   * a Component_payload_type_dict_by_val_via_map (or similar) instead of a `*this`; but if `type_info*`
   * uniqueness cannot be guaranteed, then those classes cannot be used, and one must make do with a `*this`
   * or similar.  Specifically:
   *
   * If it is a tree (sorted) map, it is log-time -- but the constant factor is (while technically a black box
   * inside `std::type_index`) almost certainly involving lexicographic less-than string comparisons, where
   * the string is more or less `type_info::name()`.  In practice that usually contains at least the fully qualified
   * name of each type being `typeid()`ed.  Still... it is potentially not *too* terrible, as the first character
   * to differ will end a string-compare, and this should typically come pretty early-on.
   * 
   * If it is a hash map, it is amortized constant-time -- but the constant factor is (while technically a black box
   * inside `std::type_info::hash_code()`) almost certainly composed of *very slow*
   * hashing of the aforementioned `type_info::name()`.  The constant time sounds nice, but for low N
   * the cost of constantly hashing `type_info` names can be very high in practice.
   */
  Map_to_cfg m_dict;
};

/**
 * Exactly equivalent to Component_payload_type_dict_by_val_via_map but with impl = array with linear search
 * (likely by string equality).
 *
 * @tparam Cfg_t
 *         See Component_payload_type_dict_by_ptr_via_map::Cfg.
 */
template<typename Cfg_t>
class Component_payload_type_dict_by_val_via_array
{
public:
  // Types.
  /// See Component_payload_type_dict_by_ptr_via_map.
  using Cfg = Cfg_t;
  // Methods.
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   */
  void insert(const std::type_info& type, Cfg cfg);
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   * @return Ditto.
   */
  bool lookup(const std::type_info& type, Cfg* cfg) const;

private:
  // Data.

  /**
   * The values pairs (`&type`, `cfg`) passed to insert(), in the order in which they were invoked.
   * (Due to insert() contract: they are guaranteed to be distinct values, no repeats whether as pointers
   * or their dereferenced objects.)
   *
   * ### Performance/rationale ###
   * Conceptually, since we don't sort the keys, a linear search through potentially the entire array
   * shall be required.  The constant factor, however -- unlike in `..._by_ptr_...` impls -- cannot
   * involve simply searching for a given `type_info*` pointer value: `this->insert(A)` and `this->lookup(B)`
   * can be called with `A == B` and yet `&A != &B`.  So we have no choice but to have the linear-search
   * perform (almost certainly) string `==` on potentially each value (namely on `type_info::name()` most likely),
   * and lower-level perfomance is mostly
   * outside of our control.  So we can just use a vanilla setup: a `vector` with key-value `pair`s +
   * `std::find_if()` for binary search, with the predicate repeatedly checking `*element.first == type` for
   * each `element` in #m_dict.
   *
   * For larger N this is bound to lose to the other `..._by_val_...` alternatives, but for very-low N
   * (e.g., 1 or 2) it's hard to say.
   */
  std::vector<std::pair<const std::type_info*, Cfg>> m_dict;
};

/**
 * Exactly equivalent to Component_payload_type_dict_by_val_via_array but with impl = sorted array susceptible to
 * binary search (likely with string lexicographic less-than comparisons).
 *
 * @tparam Cfg_t
 *         See Component_payload_type_dict_by_ptr_via_map::Cfg.
 */
template<typename Cfg_t>
class Component_payload_type_dict_by_val_via_sorted_array
{
public:
  // Types.
  /// See Component_payload_type_dict_by_ptr_via_map.
  using Cfg = Cfg_t;
  // Methods.
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   */
  void insert(const std::type_info& type, Cfg cfg);
  /**
   * See Component_payload_type_dict_by_ptr_via_map.
   * @param type
   *        Ditto.
   * @param cfg
   *        Ditto.
   * @return Ditto.
   */
  bool lookup(const std::type_info& type, Cfg* cfg) const;

private:
  // Data.

  /**
   * The values pairs (`&type`, `cfg`) passed to insert(), in order of increasing `type` value,
   * where `type_info::before()` determines ordering.
   * (Due to insert() contract: they are guaranteed to be distinct values, no repeats whether as pointers
   * or their dereferenced objects.)
   *
   * ### Performance/rationale ###
   * The way we work is just like the by-ptr counterpart (Component_payload_type_dict_by_ptr_via_sorted_array),
   * except that the constant factor of the binary search shall use `type_info::before()` as the ordering
   * operation repeatedly invoked.  Internally this is almost certainly string lexicographic-compare of
   * `type_info::name()` values which usually contain at least the fully-qualified name of the type
   * being `typeid()`d.
   *
   * For larger N this should improve on our unsorted-array peer, while comparisons against our tree-map (amortized
   * log-time) and hash-map (amortized constant-time but with extra-awful string hashing involved) are harder
   * to characterize without empirical testing.
   */
  std::vector<std::pair<const std::type_info*, Cfg>> m_dict_sorted;
};

// Template implementations.

template<typename Map_to_cfg_t>
void Component_payload_type_dict_by_ptr_via_map<Map_to_cfg_t>::insert(const std::type_info& type, Cfg cfg)
{
#ifndef NDEBUG
  const auto result =
#endif
  m_dict.try_emplace(&type, cfg);
  assert(result.second && "By contract duplicate insertion is disallowed.");
}

template<typename Map_to_cfg_t>
bool Component_payload_type_dict_by_ptr_via_map<Map_to_cfg_t>::lookup(const std::type_info& type, Cfg* cfg) const
{
  const auto it = m_dict.find(&type); // Perf: Likely tree-search via pointer<, or hash-search via hash(ptr)=ptr.  Good.
  if (it == m_dict.end())
  {
    return false;
  }
  // else
  *cfg = it->second;
  return true;
}

template<typename Map_to_cfg_t>
void Component_payload_type_dict_by_val_via_map<Map_to_cfg_t>::insert(const std::type_info& type, Cfg cfg)
{
  const std::type_index key{type};

#ifndef NDEBUG
  const auto result =
#endif
  m_dict.try_emplace(key, cfg);
  assert(result.second && "By contract duplicate insertion is disallowed.");
}

template<typename Map_to_cfg_t>
bool Component_payload_type_dict_by_val_via_map<Map_to_cfg_t>::lookup(const std::type_info& type, Cfg* cfg) const
{
  const std::type_index key{type};
  const auto it = m_dict.find(key);
  // ^-- Perf: Likely tree-search via string<, or hash-search via hash(string), where string = type_info::name().  Yuck.
  if (it == m_dict.end())
  {
    return false;
  }
  // else
  *cfg = it->second;
  return true;
}

template<typename Cfg_t>
void Component_payload_type_dict_by_ptr_via_array<Cfg_t>::insert(const std::type_info& type, Cfg cfg)
{
  m_dict_keys.emplace_back(&type);
  m_dict_vals.emplace_back(cfg);
}

template<typename Cfg_t>
bool Component_payload_type_dict_by_ptr_via_array<Cfg_t>::lookup(const std::type_info& type, Cfg* cfg) const
{
  const auto it_begin = m_dict_keys.begin(); // (Reminder: all these `it`erators are just pointers really.)
  const auto it_end = m_dict_keys.end();

  const auto it = std::find(it_begin, it_end, &type);
  /* ^-- Perf notes: First please read doc header on m_dict_keys, then come back here.  As noted there:
   * we've arranged the search values -- pointers (essentially void*s, as far as the specific algorithm involved
   * cares) -- contiugously in a buffer and are trying to find one simply bitwise-equal to &type.  This is
   * rather prone to optimization.  Now, std::find() is of course correct and pithy, but is it fast?
   * Answer: I (ygoldfel) have looked into reasonably deeply, and the answer is yes, it's pretty good with at
   * least GCC STL with Linux-64 -- both empirically (~15-30% faster than a simple loop) and in theory. (Re.
   * the latter -- the "theory": The STL code involves an internally-used find_if()-like helper that is
   * manually loop-unrolled for the random-accessor-iterator case like ours; and the generated assembly
   * with -O3 auto-inling performs 4 64-bit CMPs per loop iteration; the processor might parallelize
   * these as well.  So it's pretty good.)  Can it be better?  Answer: I do not know per se; theoretically
   * some kind of `REPNE SCASQ` -- etc. etc. -- could possibly squeeze more cycles out of it, though
   * nothing like that was getting generated automatically even if I tried incrasing m_dict_keys.size() larger
   * larger.  Perhaps such things can be manually inlined here.
   * @todo Look into all that, if in practice it is deemed worthwhile. */

  if (it == it_end)
  {
    return false;
  }
  // else
  *cfg = m_dict_vals[it - it_begin];
  return true;
}

template<typename Cfg_t>
void Component_payload_type_dict_by_val_via_array<Cfg_t>::insert(const std::type_info& type, Cfg cfg)
{
  m_dict.emplace_back(&type, cfg);
}

template<typename Cfg_t>
bool Component_payload_type_dict_by_val_via_array<Cfg_t>::lookup(const std::type_info& type, Cfg* cfg) const
{
  const auto it_end = m_dict.end();

  const auto it = std::find_if(m_dict.begin(), it_end,
                               [&type](const auto& key_and_val) -> bool { return *key_and_val.first == type; });
  /* (^-- Impl note: FWIW the std::find() impl of GCC-10 STL is pretty similar to that, using a find_if() equivalent
   * and a similar functor, just expressed as an explicit class instead of a pithy lambda... but capturing `type`
   * similarly.  I (ygoldfel) have seen std::find() reduce to quite tight assembly code, so we're probably doing
   * fine too, perf-wise no worse than having a manual loop instead of find_if() -- and prettier code-wise.) */

  if (it == it_end)
  {
    return false;
  }
  // else
  *cfg = it->second;
  return true;
}

template<typename Cfg_t>
void Component_payload_type_dict_by_ptr_via_sorted_array<Cfg_t>::insert(const std::type_info& type, Cfg cfg)
{
  /* m_dict_sorted is sorted, so find the insertion-point via binary-search, and insert there to keep it sorted.
   * This is linear-time, but we care-not about insert() performance: only lookup(). */
  const auto it = std::lower_bound(m_dict_sorted.begin(), m_dict_sorted.end(), &type,
                                   [](const auto& key_and_val, const std::type_info* key) -> bool
                                     { return key_and_val.first < key; });
  m_dict_sorted.emplace(it, &type, cfg);
}

template<typename Cfg_t>
bool Component_payload_type_dict_by_ptr_via_sorted_array<Cfg_t>::lookup(const std::type_info& type, Cfg* cfg) const
{
  const auto it_end = m_dict_sorted.end();

  /* m_dict_sorted is sorted, so find where `type` pointer is among existing ones, or else where it *would* be
   * if it isn't among existing ones. */
  const auto it = std::lower_bound(m_dict_sorted.begin(), it_end, &type,
                                   [](const auto& key_and_val, const std::type_info* key) -> bool
                                     { return key_and_val.first < key; });
  // ^-- Perf: You can see the rank-and-file op is a pointer < comparison which is pretty quick.

  if ((it == it_end) || (it->first != &type))
  {
    return false;
  }
  // else
  *cfg = it->second;
  return true;
}

template<typename Cfg_t>
void Component_payload_type_dict_by_val_via_sorted_array<Cfg_t>::insert(const std::type_info& type, Cfg cfg)
{
  const auto it = std::lower_bound(m_dict_sorted.begin(), m_dict_sorted.end(), &type,
                                   [](const auto& key_and_val, const std::type_info* key) -> bool
                                     { return key_and_val.first->before(*key); });
  m_dict_sorted.emplace(it, &type, cfg);
}

template<typename Cfg_t>
bool Component_payload_type_dict_by_val_via_sorted_array<Cfg_t>::lookup(const std::type_info& type, Cfg* cfg) const
{
  const auto it_end = m_dict_sorted.end();

  /* m_dict_sorted is sorted according to type_info::before() order, so find where `type` is in that order among
   * existing ones, or else where it *would* be if it isn't among existing ones. */
  const auto it = std::lower_bound(m_dict_sorted.begin(), it_end, &type,
                                   [](const auto& key_and_val, const std::type_info* key) -> bool
                                     { return key_and_val.first->before(*key); });
  /* ^-- Perf: The rank-and-file op is type_info::before() which is likely a type_info::name() str-compare, while
   * type_info::name() = typically compiler-generated static char[] that contains at least the fully-qualified
   * type name. */

  if ((it == it_end) || (*it->first != type))
  {
    return false;
  }
  // else
  *cfg = it->second;
  return true;
}

template<typename Dict_by_ptr_t, typename Dict_by_val_t>
void Component_payload_type_dict<Dict_by_ptr_t, Dict_by_val_t>::insert(const std::type_info& type, Cfg cfg,
                                                                       bool type_info_ptr_is_uniq)
{
  if (type_info_ptr_is_uniq)
  {
    m_dict_by_type_ptr.insert(type, cfg);
  }
  else
  {
    m_dict_by_type.insert(type, cfg);
  }
}

template<typename Dict_by_ptr_t, typename Dict_by_val_t>
bool Component_payload_type_dict<Dict_by_ptr_t, Dict_by_val_t>::lookup(const std::type_info& type, Cfg* cfg) const
{
  return m_dict_by_type_ptr.lookup(type, cfg) || m_dict_by_type.lookup(type, cfg);
}

} // namespace flow::log
