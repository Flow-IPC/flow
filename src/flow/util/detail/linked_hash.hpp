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
#include <variant>

namespace flow::util
{

// Types.

/**
 * The internal-use key/iterator-wrapper, used as the key type in internal-use set-type #Linked_hash_key_set.
 *
 * For background please see Linked_hash_map doc header Impl section, and where that points.  Note that
 * the same information applies to Linked_hash_set as well.  (`IS_ITER_TO_PAIR` indicates which guy this is helping
 * implement.)
 *
 * That said, we very specifically just need to:
 *
 *   - store either a #Key pointer (when a `*this` is used as the arg to `Linked_hash_key_set::find()` or an
 *     #Iterator copy (ditto but to `Linked_hash_key_set::insert()`);
 *     - be implicit-constructible from either (so the `Key` or `Iterator` can be passed seamlessly into
 *       `.find()` and `.insert()` respectively);
 *   - in the iterator-storing case provide access to that iterator via iter() accessor.
 *
 * @tparam Key_t
 *         See Linked_hash_map, Linked_hash_set.
 * @tparam Iterator_t
 *         Linked_hash_map::Iterator or Linked_hash_set::Iterator.
 * @tparam IS_ITER_TO_PAIR
 *         `true` for Linked_hash_map, `false` for Linked_hash_set.
 */
template<typename Key_t, typename Iterator_t, bool IS_ITER_TO_PAIR>
class Linked_hash_key
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Key = Key_t;

  /// Convenience alias for template arg.
  using Iterator = Iterator_t;

  // Constants.

  /// Convenience alias for template arg.
  static constexpr bool S_IS_ITER_TO_PAIR = IS_ITER_TO_PAIR;

  // Constructors/destructor.

  /**
   * Constructs `*this` to contain a pointer to a #Key living outside the `*this`-using data structure
   * #Linked_hash_key_set (presumably as arg to its `.find()`).
   *
   * @param key
   *        The key to which to point.  The referred object must continue to be valid, until either
   *        `*this` is destroyed, or via assignment `*this` changes value.
   */
  Linked_hash_key(const Key& key);

  /**
   * Constructs `*this` to contain an #Iterator into the `*this`-using data structure
   * #Linked_hash_key_set (presumably as arg to its `.insert()`).
   *
   * @param it
   *        The iterator a copy of which to store.  The pointee of the iterator must not be erased or moved in
   *        an iterator-invalidating fashion, until either
   *        `*this` is destroyed, or via assignment `*this` changes value.
   */
  Linked_hash_key(const Iterator& it);

  // Methods.

  /**
   * Returns reference to immutable key to which we saved a pointer via one of our constructor forms.
   * Specifically, then, that's either simply the ctor-saved pointer (in reference form); or the address of the key
   * stored as stored within the `*this`-using data structure #Linked_hash_key_set.
   *
   * @return See above.
   */
  const Key& key() const;

  /**
   * Assuming we were as-if constructed via the ctor that takes an #Iterator (as opposed to a #Key reference),
   * returns copy of the stored iterator.  Behavior undefined if the assumption does not hold (as of this writing
   * exception is thrown).
   *
   * Informally:
   * if `this->key()` *was* found in the #Linked_hash_key_set, then this accessor may be used; else it may not.
   * That `*this`-using data structure -- a hash-set -- shall use Linked_hash_key_hash and Linked_hash_key_pred for its
   * hasher and equality functors respectively.
   *
   * @return See above.
   */
  Iterator iter() const;

private:
  // Types.

  /// Short-hand for raw pointer to an immutable #Key living outside the `*this`-using data structure.
  using Key_ptr = Key const *;

  // Data.

  /**
   * Stores the key, without copying its actual value, as either a pointer to an immutable key, or as an iterator
   * into a structure such that it contains the key.  In the latter case:
   *
   *   - #S_IS_ITER_TO_PAIR determines how to obtain the key from the iterator pointee;
   *   - the iterator is itself of value to the `Linked_hash_key_*` user (Linked_hash_set, Linked_hash_map).
   */
  std::variant<Key_ptr, Iterator> m_key_hndl;
}; // class Linked_hash_key

/**
 * The internal-use `Hash` functor wrapper, used as the hasher type in internal-use set-type #Linked_hash_key_set.
 *
 * Impl note: We store the `Hash` via private inheritance, making use of Empty Base-class Optimization (EBO) if
 * possible; namely when `Hash` is an empty type (which is typical); in which case this shall waste no space.
 *
 * @tparam Hash
 *         See Linked_hash_map, Linked_hash_set.
 */
template<typename Hash>
class Linked_hash_key_hash :
  private Hash
{
public:
  // Constructors/destructor.

  /**
   * Saves a (typically data-free) copy of a given Linked_hash_map or Linked_hash_set hasher object,
   * as passed to that type's ctor.
   *
   * @param hasher
   *        See above.
   */
  Linked_hash_key_hash(const Hash& hasher = Hash{});

  // Methods.

  /**
   * Returns hash of `val.key()`, where `val` is a Linked_hash_key instance, using the saved hasher object.
   *
   * @tparam Linked_hash_key_t
   *         A concrete Linked_hash_key type: the key-type of #Linked_hash_key_set being used.
   * @param val
   *        Value to hash.
   * @return See above.
   */
  template<typename Linked_hash_key_t>
  size_t operator()(const Linked_hash_key_t& val) const;
}; // class Linked_hash_key_hash

/**
 * The internal-use `Pred` functor wrapper, used as the key-equality-determiner type in internal-use set-type
 * #Linked_hash_key_set.
 *
 * Impl note: We store the `Pred` via private inheritance, making use of Empty Base-class Optimization (EBO) if
 * possible; namely when `Pred` is an empty type (which is typical); in which case this shall waste no space.
 *
 * @tparam Pred
 *         See Linked_hash_map, Linked_hash_set.
 */
template<typename Pred>
class Linked_hash_key_pred :
  private Pred
{
public:
  // Constructors/destructor.

  /**
   * Saves a (typically data-free) copy of a given Linked_hash_map or Linked_hash_set equality-determiner object,
   * as passed to that type's ctor.
   *
   * @param pred
   *        See above.
   */
  Linked_hash_key_pred(const Pred& pred = Pred{});

  // Methods

  /**
   * Returns `true` if and only if `lhs.key()` and `rhs.key()` (where `lhs` and `rhs` are Linked_hash_key instances)
   * are equal, using the saved equality-determiner object.
   *
   * @tparam Linked_hash_key_t
   *         A concrete Linked_hash_key type.
   * @param lhs
   *        Value to compare.
   * @param rhs
   *        Value to compare.
   * @return See above.
   */
  template<typename Linked_hash_key_t>
  bool operator()(const Linked_hash_key_t& lhs, const Linked_hash_key_t& rhs) const;
}; // class Linked_hash_key_pred

// Template implementations.

// Linked_hash_key implementations.

template<typename Key_t, typename Iterator_t, bool IS_ITER_TO_PAIR>
Linked_hash_key<Key_t, Iterator_t, IS_ITER_TO_PAIR>::Linked_hash_key(const Key& key) :
  m_key_hndl(&key) // Store a Key* in the union.
{
  // Yep.
}

template<typename Key_t, typename Iterator_t, bool IS_ITER_TO_PAIR>
Linked_hash_key<Key_t, Iterator_t, IS_ITER_TO_PAIR>::Linked_hash_key(const Iterator& it) :
  m_key_hndl(it) // Store an Iterator in the union.
{
  // Yep.
}

template<typename Key_t, typename Iterator_t, bool IS_ITER_TO_PAIR>
const Key_t&
  Linked_hash_key<Key_t, Iterator_t, IS_ITER_TO_PAIR>::key() const
{
  using std::holds_alternative;
  using std::get;

  if (holds_alternative<Key_ptr>(m_key_hndl))
  {
    return *(get<Key_ptr>(m_key_hndl));
  }
  // else

  if constexpr(S_IS_ITER_TO_PAIR)
  {
    return get<Iterator>(m_key_hndl)->first; // Iterator into list of `const Key`s.
  }
  else
  {
    return *(get<Iterator>(m_key_hndl)); // Iterator into list of pair<const Key, Mapped>s.
  }
}

template<typename Key_t, typename Iterator_t, bool IS_ITER_TO_PAIR>
Iterator_t
  Linked_hash_key<Key_t, Iterator_t, IS_ITER_TO_PAIR>::iter() const
{
  return std::get<Iterator>(m_key_hndl);
  // ^-- Throws if !holds_alternative<Iterator>(m_key_hndl); we advertised undefined behavior; so that's fine.
}

// Linked_hash_key_hash implementations.

template<typename Hash>
Linked_hash_key_hash<Hash>::Linked_hash_key_hash(const Hash& hasher) :
  Hash(hasher) // Store `hasher` copy in our super-class, making use of Empty Base-class Optimization (EBO) if possible.
{
  /* For context: A regular unordered_set<Key, Hash, ...> would store the `Hash hasher` copy inside itself.
   * In our case it is unordered_set<Linked_hash_key, Linked_hash_key_hash<Hash>, ...> instead, so a *this is
   * instead stored; and we store the original `Hash hasher` inside us (and nothing else).  So it's the exact same
   * thing in terms of what actually ends up in memory and likely in terms of processor cycles spent.
   *
   * Why do we even exist?  Answer: Just for the extra little code in operator()(). */
}

template<typename Hash>
template<typename Linked_hash_key_t>
size_t Linked_hash_key_hash<Hash>::operator()(const Linked_hash_key_t& val) const
{
  return this->Hash::operator()(val.key()); // The piddly .key() call is the reason this class exists.
}

// Linked_hash_key_pred implementations.

template<typename Pred>
Linked_hash_key_pred<Pred>::Linked_hash_key_pred(const Pred& pred) :
  Pred(pred) // Store `pred` copy in our super-class, making use of Empty Base-class Optimization (EBO) if possible.
{
  /* For context: A regular unordered_set<Key, ..., Pred> would store the `Pred pred` copy inside itself.
   * In our case it is unordered_set<Linked_hash_key, ..., Linked_hash_key_pred<PRed>> instead, so a *this is
   * instead stored; and we store the original `Pred pred` inside us (and nothing else).  So it's the exact same
   * thing in terms of what actually ends up in memory and likely in terms of processor cycles spent.
   *
   * Why do we even exist?  Answer: Just for the extra little code in operator()(). */
}

template<typename Pred>
template<typename Linked_hash_key_t>
bool Linked_hash_key_pred<Pred>::operator()(const Linked_hash_key_t& lhs, const Linked_hash_key_t& rhs) const
{
  return this->Pred::operator()(lhs.key(), rhs.key()); // The piddly .key() calls are the reason this class exists.

  /* @todo Arguably this could be sped-up in the case where lhs and rhs both hold `Iterator`s
   * (holds_alternative<Iterator>([lr]hs.m_key_hndl)); in which case one would return `lhs.iter() == rhs.iter()`.
   * This would require formally that lhs and rhs are guaranteed to point into the same Linked_hash_{set|map}, and
   * the formal doc for this operator()() would specify that it's not merely comparison of .key() values by Pred.
   *
   * "Arguably" above refers to how that might be too much trouble "just" to avoid a by-value key comparison in
   * some cases (some cases as of this writing = Linked_hash_*::erase(<iterator-typed args>).  Then again that does
   * make sense, if one breaks down the actual use cases in Linked_hash_set/map:
   *   - lhs and rhs are both iterator-storing <=> those "some cases," namely where lookup is by user-provided iterator.
   *   - lhs is iterator-storing; rhs is key-ptr-storing <=> the other cases, namely where lookup is by user-provided
   *     key.
   *     - The reverse <=> Shouldn't happen; the "haystack" Linked_hash_key_set is on the left.
   *   - lhs and rhs are both key-ptr-storing <=> Shouldn't happen; lookup is only needed when there's a haystack
   *     involved.
   *
   * So we could write it that way -- perhaps assert(false)ing on the "shouldn't happen" cases -- and arguably it
   * would be (1) conceptually tighter (serving the exact known purpose) and (2) probably faster (avoids key comparison
   * at least sometimes).  On the other hand internal documentation would be more complex, and the code here would
   * be longer/more complex (probably requiring among other things either `friend`ship or additional accessor(s) on
   * Linked_hash_key.
   *
   * Oh, and also, I (ygoldfel) am not 100% certain about the following, but another difficulty might arise when
   * Iterator and Const_iterator are not the same type....  Bottom line, probably best not jump into this, unless
   * the perf gain is really determined to be worthwhile for someone in practice. */
}

} // namespace flow::util
