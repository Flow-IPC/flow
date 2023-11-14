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
#include <cstddef>
#include <boost/unordered_set.hpp>
#include <boost/move/unique_ptr.hpp>

namespace flow::util
{

/**
 * An object of this class is a set that combines the lookup speed of an `unordered_set<>` and ordering and iterator
 * stability capabilities of an `std::list<>`.
 *
 * This is just like Linked_hash_map, except it only stores keys -- no mapped values.  All comments, except for
 * self-explanatory differences, from Linked_hash_map apply here.  Thus I will only speak of differences below to
 * avoid duplication of this header.
 *
 * @see class Linked_hash_map.
 *
 * @tparam Key
 *         Key type.  Same as for Linked_hash_map.
 * @tparam Hash
 *         Hasher type.  Same as for Linked_hash_map.
 * @tparam Pred
 *         Equality functor type.  Same as for Linked_hash_map.
 */
template<typename Key, typename Hash, typename Pred>
class Linked_hash_set
{
public:
  // Types.

  /// Short-hand for values, which in this case are simply the keys.
  using Value = Key;

private:
  // Types. These are here in the middle of public block due to inability to forward-declare aliases.

  /// Short-hand for doubly linked list of `Key`s.
  using Value_list = std::list<Value>;

public:

  // Types (continued).

  /// Type for index into array of items, where items are all applicable objects including `Value`s and `Key`s.
  using size_type = std::size_t;
  /// Type for difference of `size_type`s.
  using difference_type = std::ptrdiff_t;

  /// Type for iterator pointing into an immutable structure of this type.
  using Const_iterator = typename Value_list::const_iterator;

  /**
   * Type for iterator pointing into a mutable structure of this type but actually that is not possible;
   * so alias to #Const_iterator.  Note these are standard semantics (see `std::set`, etc.).
   */
  using Iterator = Const_iterator;

  /// Type for reverse iterator pointing into an immutable structure of this type.
  using Const_reverse_iterator = typename Value_list::const_reverse_iterator;

  /**
   * Type for reverse iterator pointing into a mutable structure of this type but actually that is not possible;
   * so alias to #Const_reverse_iterator.  Note these are standard semantics (see `std::set`, etc.).
   */
  using Reverse_iterator = Const_reverse_iterator;

  /// For container compliance (hence the irregular capitalization): `Key` type.
  using key_type = Key;
  /// For container compliance (hence the irregular capitalization): `Value` type.
  using value_type = Value;
  /// For container compliance (hence the irregular capitalization): `Hash` type.
  using hasher = Hash;
  /// For container compliance (hence the irregular capitalization): `Pred` type.
  using key_equal = Pred;
  /// For container compliance (hence the irregular capitalization): pointer to `Key` type.
  using pointer = Value*;
  /// For container compliance (hence the irregular capitalization): pointer to `const Key` type.
  using const_pointer = Value const *;
  /// For container compliance (hence the irregular capitalization): reference to `Key` type.
  using reference = Value&;
  /// For container compliance (hence the irregular capitalization): reference to `const Key` type.
  using const_reference = Value const &;
  /// For container compliance (hence the irregular capitalization): `Iterator` type.
  using iterator = Iterator;
  /// For container compliance (hence the irregular capitalization): `Const_iterator` type.
  using const_iterator = Const_iterator;

  // Constructors/destructor.

  /**
   * Constructs empty structure with some basic parameters.
   *
   * @param n_buckets
   *        Number of buckets for the unordered (hash) table. Special value -1 (default) will cause us to use
   *        whatever `unordered_set<>` would use by default.
   * @param hasher_instance
   *        Instance of the hash function type (`hasher_instance(Key k)` should be `size_type`d hash of key `k`).
   * @param key_equal_instance
   *        Instance of the equality function type (`key_equal_instance(Key k1, Key k2)` should return `true` if and
   *        only if `k1` equals `k2`).
   */
  explicit Linked_hash_set(size_type n_buckets = size_type(-1),
                           Hash const & hasher_instance = Hash(),
                           Pred const & key_equal_instance = Pred());

  /**
   * Constructs structure with some basic parameters, and values initialized from initializer list.
   * The values are inserted as if `insert(v)` was called for each pair `v` in `values`
   * <em>in reverse order</em>.  Since the canonical ordering places the *newest* (last inserted/touch()ed)
   * element at the *front* of the ordering, that means that forward iteration through the set (right after this
   * constructor runs) will yield values in the *same* order as in initializer list `values`.
   *
   * @param values
   *        Values with which to fill the structure after initializing it.
   *        Typically you'd provide a series of keys like this:
   *        `{ key1, key2, ... }`.  They will appear in iterated sequence in the same order as they appear
   *        in this list.
   * @param n_buckets
   *        See other constructor.
   * @param hasher_instance
   *        See other constructor.
   * @param key_equal_instance
   *        See other constructor.
   */
  explicit Linked_hash_set(std::initializer_list<Value> values,
                           size_type n_buckets = size_type(-1),
                           Hash const & hasher_instance = Hash(),
                           Pred const & key_equal_instance = Pred());

  /**
   * Constructs object that is a copy of the given source.  Equivalent to `operator=(src)`.
   *
   * @param src
   *        Source object.
   */
  Linked_hash_set(Linked_hash_set const & src);

  /**
   * Constructs object by making it equal to the given source, while the given source becomes as-if default-cted.
   *
   * @param src
   *        Source object which is emptied.
   */
  Linked_hash_set(Linked_hash_set&& src);

  // Methods.

  /**
   * Overwrites this object with a copy of the given source.  We become equal to `src` but independent of it to the max
   * extent possible (if you've got pointers stored in there, for example, the pointers are copied, not the values
   * at those pointers).  In addition, the hasher instance and equality predicate are copied from `src`.  Finally, a
   * reasonable attempt is made to also make the internal structure of the hash set to be similar to that of `src`.
   *
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Linked_hash_set& operator=(Linked_hash_set const & src);

  /**
   * Overwrites this object making it equal to the given source, while the given source becomes as-if default-cted.
   *
   * @param src
   *        Source object which is emptied (unless it *is* `*this`; then no-op).
   * @return `*this`.
   */
  Linked_hash_set& operator=(Linked_hash_set&& src);

  /**
   * Attempts to insert the given key into the set.  If the key is already present in the set,
   * does nothing.  Return value indicates various info of interest about what occurred or did not occur.
   * Key presence is determined according to the `Pred` template parameter which determines equality of 2 given keys;
   * and via the `Hash` template parameter that enables efficient hash-based lookup.
   * If inserted, the new element is considered "newest," as if by touch().  If not inserted, the existing element
   * location is not affected.
   *
   * @param key
   *        The key to attempt to insert.  This value is copied, and the copy is inserted.
   * @return A pair whose second element is `true` if and only if the insertion occurred; and whose first element
   *         is an iterator pointing to either the newly inserted element or already present one equal to
   *         `key`.
   */
  std::pair<Iterator, bool> insert(Value const & key);

  /**
   * Attempts to find the given key in the set.  Key presence is determined according to the `Pred` template
   * parameter which determines equality of 2 given keys; and via the `Hash` template parameter that enables efficient
   * hash-based lookup.  The returned iterator (if valid) cannot be used to mutate the elements stored in the map.
   *
   * As long as the key is not removed from the map, the iterator will continue to be valid.
   *
   * @note Let `r` be the returned value.  Since no `key`-associated value beyond `key` itself is stored in the
   *       structure, the fact that `*r == key` is not valuable: you already had `key` after all!  It is only useful
   *       in pin-pointing the relative location in the chronological ordering; in being used as an argument to
   *       various erasing methods; and in checking for presence of the key in the set.  For the latter, I recommend
   *       the following utility:
   * @see util::key_exists(), which uses this method to more concisely check for the presence of a key.
   * @param key
   *        Key whose equal to find.
   * @return If found, iterator to the equivalent key; else `this->const_past_oldest()`.
   */
  Const_iterator find(Key const & key) const;

  /**
   * Returns the number of times a key is equivalent to the given one is present in the hash: either 1 or 0.
   *
   * @param key
   *        Key whose equal to find.
   * @return 0 or 1.
   */
  size_type count(Key const & key) const;

  /**
   * Returns reference to immutable front ("newest") element in the structure; formally equivalent to
   * `*(this->const_newest())`.
   *
   * OK to call when empty(); but behavior undefined if you attempt to access the result in any way if either empty()
   * when this was called; or if `!empty()` at that time, but the underlying element is erased at time of access.
   * If not `empty()` when this was called, then resulting reference continues to be valid as long as the underlying
   * element is not erased; however, in the future the reference (while referring to the same element) might not refer
   * to front ("newest") element any longer.  (Informally, most uses would only call const_front() when `!empty()`, and
   * would access it immediately and but once.  However, I'm listing the corner cases above.)
   *
   * @return Reference to immutable `Key` (a/k/a `Value`) directly inside data structure; or to undefined location if
   *         currently empty().
   */
  Value const & const_front() const;

  /**
   * Returns reference to immutable back ("oldest") element in the structure; formally equivalent to
   * `*(--this->const_past_oldest())`.
   *
   * All other comments for const_front() apply analogously.
   *
   * @return Reference to immutable `Key` (a/k/a `Value`) directly inside data structure; or to undefined location if
   *         currently empty().
   */
  Value const & const_back() const;

  /**
   * Given a valid iterator into the structure, makes the pointed to element "newest" by moving it from wherever it
   * is to be first in the iteration order.  Behavior undefined if iterator invalid.
   *
   * The iterator continues to be valid.
   *
   * @param it
   *        Iterator to an element of the structure.
   */
  void touch(Const_iterator const & it);

  /**
   * Given a key into the structure, makes the corresponding element "newest" by moving it from wherever it
   * is to be first in the iteration order; or does nothing if no such key.  Return value indicates various info of
   * interest about what occurred or did not occur.
   *
   * @param key
   *        Key whose equal to find.
   * @return `true` if the key was found (even if it was already "newest"); false if not found.
   */
  bool touch(Key const & key);

  /**
   * Erases the element pointed to by the given valid iterator.  Behavior undefined if it is not valid.  `it` becomes
   * invalid.
   *
   * @param it
   *        Iterator of element to erase.
   * @return Iterator one position past (i.e., "older") than `it`, before `*it` was removed.
   */
  Iterator erase(Const_iterator const & it);

  /**
   * Erases all elements in the range [`it_newest`, `it_past_oldest`).  Behavior undefined if given iterator is invalid.
   * `it_newest` becomes invalid.
   *
   * @param it_newest
   *        Iterator of first ("newest") element to erase.
   * @param it_past_oldest
   *        Iterator of one past last ("oldest") element to erase.
   * @return `it_past_oldest` copy.
   */
  Iterator erase(Const_iterator const & it_newest, Const_iterator const & it_past_oldest);

  /**
   * Erases the element with the given key, if it exists.  Return value indicates various info of interest about what
   * occurred or did not occur.
   *
   * @param key
   *        Key such that its equal's (if found) element will be erased.
   * @return Number of elements erased (0 or 1).
   */
  size_type erase(Key const & key);

  /// Queue-style pop (erase) of the front -- a/k/a newest -- element.  Behavior undefined if empty().
  void pop_front();

  /// Queue-style pop (erase) of the back -- a/k/a oldest -- element.  Behavior undefined if empty().
  void pop_back();

  /// Makes it so that `size() == 0`.
  void clear();

  /**
   * Swaps the contents of this structure and `other`.  This is a constant-time operation.
   *
   * @param other
   *        The other structure.
   */
  void swap(Linked_hash_set& other);

  /**
   * Synonym of newest().
   * @return See newest().
   */
  Iterator begin() const;

  /**
   * Returns first, a/k/a "newest," element's iterator (to immutable element, due to nature of this type).
   * @return Ditto.
   */
  Iterator newest() const;

  /**
   * Synonym of past_oldest().  Exists as standard container method.
   * @return See past_oldest().
   */
  Iterator end() const;

  /**
   * Returns one past last, a/k/a "oldest," element's iterator (to immutable element, due to nature of this type).
   * @return Ditto.
   */
  Iterator past_oldest() const;

  /**
   * Synonym of const_newest().  Exists as standard container method (hence the odd formatting).
   * @return See const_newest().
   */
  Const_iterator cbegin() const;

  /**
   * Returns first, a/k/a "newest," element's iterator (to immutable element).
   * @return Ditto.
   */
  Const_iterator const_newest() const;

  /**
   * Synonym of const_past_oldest().  Exists as standard container method (hence the odd formatting).
   * @return See const_past_oldest().
   */
  Const_iterator cend() const;

  /**
   * Returns one past last, a/k/a "oldest," element's iterator (to immutable element).
   * @return Ditto.
   */
  Const_iterator const_past_oldest() const;

  /**
   * Synonym of oldest().
   * @return See oldest().
   */
  Reverse_iterator rbegin() const;

  /**
   * Returns first, a/k/a "oldest," element's reverse iterator (to immutable element, due to nature of this type).
   * @return Ditto.
   */
  Reverse_iterator oldest() const;

  /**
   * Synonym of past_newest().  Exists as standard container method.
   * @return See past_newest().
   */
  Reverse_iterator rend() const;

  /**
   * Returns one past last, a/k/a "newest," element's reverse iterator (to immutable element, due
   * to nature of this type).
   * @return Ditto.
   */
  Reverse_iterator past_newest() const;

  /**
   * Synonym of const_oldest().
   * @return See const_oldest().
   */
  Const_reverse_iterator crbegin() const;

  /**
   * Returns first, a/k/a "oldest," element's reverse iterator (to immutable element).
   * @return Ditto.
   */
  Const_reverse_iterator const_oldest() const;

  /**
   * Synonym of const_past_newest().  Exists as standard container method.
   * @return See const_past_newest().
   */
  Const_reverse_iterator crend() const;

  /**
   * Returns one past last, a/k/a "newest," element's reverse iterator (to immutable element).
   * @return Ditto.
   */
  Const_reverse_iterator const_past_newest() const;

  /**
   * Returns `true` if and only if container is empty.  Same performance as of `unordered_map<>`.
   * @return Ditto.
   */
  bool empty() const;

  /**
   * Returns number of elements stored.  Same performance as of `unordered_map<>`.
   * @return Ditto.
   */
  size_type size() const;

  /**
   * Returns max number of elements that can be stored.  Same performance as of `unordered_map<>` + `list<>`.
   * @return Ditto.
   */
  size_type max_size() const;

private:

  // Methods.

  /**
   * Helper that modifies #m_value_list and #m_keys_into_list_map so that `key`'s copy is inserted into
   * the structure.  Pre-condition is that `key` is not in the structure (else behavior undefined).
   *
   * @param key
   *        Same as in insert().
   * @return Same as in `insert().first`.
   */
  Iterator insert_impl(Value const & key);

  // Types.

  /// Short-hand for iterator into doubly linked list of `Key` elements.
  using Value_list_iter = Iterator;

  /// Short-hand for const iterator into doubly linked list of `Key` elements.
  using Value_list_const_iter = Const_iterator;

  /// Short-hand for a hash map that maps `Key` to iterator into doubly linked list of `Key` elements.
  using Key_to_value_iter_map = boost::unordered_map<Key, Value_list_iter, Hash, Pred>;

  // Data.

  /// See Linked_hash_map::m_value_list.  Essentially all of that applies here.
  Value_list m_value_list;

  /// See Linked_hash_map::m_keys_into_list_map.  Essentially all of that applies here.
  boost::movelib::unique_ptr<Key_to_value_iter_map> m_keys_into_list_map;
}; // class Linked_hash_set

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Key, typename Hash, typename Pred>
Linked_hash_set<Key, Hash, Pred>::Linked_hash_set(size_type n_buckets,
                                                  hasher const & hasher_instance,
                                                  key_equal const & key_equal_instance) :
  Linked_hash_set({}, n_buckets, hasher_instance, key_equal_instance)
{
  // Nothing.
}

template<typename Key, typename Hash, typename Pred>
Linked_hash_set<Key, Hash, Pred>::Linked_hash_set(std::initializer_list<Value> values,
                                                  size_type n_buckets,
                                                  hasher const & hasher_instance,
                                                  key_equal const & key_equal_instance) :
  // Their initializer_list is meant for a set, but it is perfect for our list of keys.
  m_value_list(values)
{
  using boost::unordered_set;

  /* Guess the default size, if they specified the default, from a dummy unrelated-type set.  Probably
   * that'll be correct.  Even use our template argument values, just in case that matters. */
  if (n_buckets == size_type(-1))
  {
    unordered_set<Key, Hash, Pred> dummy;
    n_buckets = dummy.bucket_count();
  }

  // We use a unique_ptr<> because of the above: we couldn't immediately initialize this map.
  m_keys_into_list_map.reset(new Key_to_value_iter_map(n_buckets, hasher_instance, key_equal_instance));

  // Now link each key in the quick-lookup table to its stored location in the ordering.
  for (Value_list_iter value_list_it = m_value_list.begin(); value_list_it != m_value_list.end();
       ++value_list_it)
  {
    // Note this sets (at key K) the value: iterator to K.
    (*m_keys_into_list_map)[*value_list_it] = value_list_it;
  }
}

template<typename Key, typename Hash, typename Pred>
Linked_hash_set<Key, Hash, Pred>::Linked_hash_set(Linked_hash_set const & src) :
  m_keys_into_list_map(new Key_to_value_iter_map()) // Dummy: all this is quickly replaced.
{
  operator=(src);
}

template<typename Key, typename Hash, typename Pred>
Linked_hash_set<Key, Hash, Pred>::Linked_hash_set(Linked_hash_set&& src) :
  m_keys_into_list_map(new Key_to_value_iter_map()) // Dummy: all this is quickly replaced.
{
  operator=(std::move(src));
}

template<typename Key, typename Hash, typename Pred>
Linked_hash_set<Key, Hash, Pred>&
  Linked_hash_set<Key, Hash, Pred>::operator=(Linked_hash_set const & src)
{
  // See Linked_hash_map equivalent method, to which this is analogous. Keeping comments here light.

  if (&src == this)
  {
    return *this;
  }
  // else

  {
    using std::pair;
    using std::list;

    using Mutable_key_list = list<Key>;

    Mutable_key_list* const dst_list_ptr
      = reinterpret_cast<Mutable_key_list*>(&m_value_list);
    const Mutable_key_list* const src_list_ptr
      = reinterpret_cast<const Mutable_key_list*>(&src.m_value_list);

    *dst_list_ptr = *src_list_ptr;
  }

  *m_keys_into_list_map = *src.m_keys_into_list_map;

  // So now replace the keys in the ready map.
  for (Value_list_iter value_list_it = m_value_list.begin(); value_list_it != m_value_list.end();
       ++value_list_it)
  {
    // Note this sets (at key K) the value: iterator to K.
    (*m_keys_into_list_map)[*value_list_it] = value_list_it;
  }

  return *this;
} // Linked_hash_set::operator=()

template<typename Key, typename Hash, typename Pred>
Linked_hash_set<Key, Hash, Pred>&
  Linked_hash_set<Key, Hash, Pred>::operator=(Linked_hash_set&& src)
{
  if (&src != this)
  {
    clear();
    swap(src);
  }
  return *this;
}

template<typename Key, typename Hash, typename Pred>
std::pair<typename Linked_hash_set<Key, Hash, Pred>::Iterator, bool>
  Linked_hash_set<Key, Hash, Pred>::insert(Value const & key)
{
  // See Linked_hash_map equivalent method, to which this is analogous. Keeping comments here light.

  using std::pair;

  typename Key_to_value_iter_map::iterator const map_it = m_keys_into_list_map->find(key);
  if (map_it != m_keys_into_list_map->end())
  {
    return pair<Iterator, bool>(map_it->second, false);
  }
  return pair<Iterator, bool>(insert_impl(key), true);
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Iterator
  Linked_hash_set<Key, Hash, Pred>::insert_impl(Value const & key)
{
  // See Linked_hash_map equivalent method, to which this is analogous. Keeping comments here light.

  m_value_list.push_front(key);
  Iterator const new_elem_it = m_value_list.begin();
  (*m_keys_into_list_map)[key] = new_elem_it;

  return new_elem_it;
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_iterator
  Linked_hash_set<Key, Hash, Pred>::find(Key const & key) const
{
  typename Key_to_value_iter_map::const_iterator const map_it = m_keys_into_list_map->find(key);
  return (map_it == m_keys_into_list_map->cend()) ? m_value_list.cend() : map_it->second;
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::size_type
  Linked_hash_set<Key, Hash, Pred>::count(Key const & key) const
{
  return m_keys_into_list_map->count(key);
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Value const &
  Linked_hash_set<Key, Hash, Pred>::const_front() const
{
  // No assert(): we promised not to crash even if empty().  They just can't access it subsequently if so.
  return *(const_newest());
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Value const &
  Linked_hash_set<Key, Hash, Pred>::const_back() const
{
  // No assert(): we promised not to crash even if empty().  They just can't access it subsequently if so.
  return *(--const_past_oldest());
}

template<typename Key, typename Hash, typename Pred>
void Linked_hash_set<Key, Hash, Pred>::touch(Const_iterator const & it)
{
  m_value_list.splice(m_value_list.begin(), m_value_list, it);
}

template<typename Key, typename Hash, typename Pred>
bool Linked_hash_set<Key, Hash, Pred>::touch(Key const & key)
{
  const Iterator it = find(key);
  if (it == end())
  {
    return false;
  }
  // else

  touch(it);
  return true;
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Iterator
  Linked_hash_set<Key, Hash, Pred>::erase(Const_iterator const & it)
{
  m_keys_into_list_map->erase(m_keys_into_list_map->find(*it));
  return m_value_list.erase(it);
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Iterator
  Linked_hash_set<Key, Hash, Pred>::erase(Const_iterator const & it_newest,
                                          Const_iterator const & it_past_oldest)
{
  for (Value_list_const_iter it = it_newest; it != it_past_oldest; ++it)
  {
    m_keys_into_list_map->erase(it->first);
  }

  return m_value_list.erase(it_newest, it_past_oldest);
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::size_type
  Linked_hash_set<Key, Hash, Pred>::erase(Key const & key)
{
  typename Key_to_value_iter_map::iterator const map_it = m_keys_into_list_map->find(key);
  if (map_it == m_keys_into_list_map->end())
  {
    return 0;
  }
  // else

  m_value_list.erase(map_it->second);
  m_keys_into_list_map->erase(map_it);

  return 1;
}

template<typename Key, typename Hash, typename Pred>
void Linked_hash_set<Key, Hash, Pred>::pop_front()
{
  assert(!empty());
  erase(const_newest());
}

template<typename Key, typename Hash, typename Pred>
void Linked_hash_set<Key, Hash, Pred>::pop_back()
{
  assert(!empty());
  erase(--const_past_oldest());
}

template<typename Key, typename Hash, typename Pred>
void Linked_hash_set<Key, Hash, Pred>::clear()
{
  m_keys_into_list_map->clear();
  m_value_list.clear();
}

template<typename Key, typename Hash, typename Pred>
void Linked_hash_set<Key, Hash, Pred>::swap(Linked_hash_set& other)
{
  using std::swap;

  swap(m_keys_into_list_map, other.m_keys_into_list_map); // unique_ptr<>s exchanged (= raw pointers exchanged).
  swap(m_value_list, other.m_value_list); // list<> exchange (probably = head+tail pointer pairs exchanged).
  // Per cppreference.com `list<>::iterator`s (inside the `_maps`s) remain valid after list<>s swapped.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Iterator
  Linked_hash_set<Key, Hash, Pred>::newest() const
{
  return m_value_list.cbegin();
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_iterator
  Linked_hash_set<Key, Hash, Pred>::const_newest() const
{
  return newest(); // For us Iterator = Const_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Iterator
  Linked_hash_set<Key, Hash, Pred>::begin() const
{
  return newest();
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_iterator
  Linked_hash_set<Key, Hash, Pred>::cbegin() const
{
  return begin(); // For us Iterator = Const_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Iterator
  Linked_hash_set<Key, Hash, Pred>::past_oldest() const
{
  return m_value_list.cend();
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_iterator
  Linked_hash_set<Key, Hash, Pred>::const_past_oldest() const
{
  return past_oldest(); // For us Iterator = Const_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Iterator
  Linked_hash_set<Key, Hash, Pred>::end() const
{
  return past_oldest();
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_iterator
  Linked_hash_set<Key, Hash, Pred>::cend() const
{
  return end(); // For us Iterator = Const_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Reverse_iterator
  Linked_hash_set<Key, Hash, Pred>::oldest() const
{
  return m_value_list.crbegin();
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_reverse_iterator
  Linked_hash_set<Key, Hash, Pred>::const_oldest() const
{
  return oldest(); // For us Iterator = Const_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Reverse_iterator
  Linked_hash_set<Key, Hash, Pred>::rbegin() const
{
  return oldest();
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_reverse_iterator
  Linked_hash_set<Key, Hash, Pred>::crbegin() const
{
  return rbegin(); // For us Reverse_iterator = Const_reverse_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Reverse_iterator
  Linked_hash_set<Key, Hash, Pred>::past_newest() const
{
  return m_value_list.crend(); // For us Reverse_iterator = Const_reverse_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_reverse_iterator
  Linked_hash_set<Key, Hash, Pred>::const_past_newest() const
{
  return past_newest(); // For us Reverse_iterator = Const_reverse_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Reverse_iterator
  Linked_hash_set<Key, Hash, Pred>::rend() const
{
  return past_newest();
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::Const_reverse_iterator
  Linked_hash_set<Key, Hash, Pred>::crend() const
{
  return m_value_list.rend(); // For us Reverse_iterator = Const_reverse_iterator.
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::size_type
  Linked_hash_set<Key, Hash, Pred>::size() const
{
  return m_keys_into_list_map->size(); // I'm skeptical/terrified of list::size()'s time complexity.
}

template<typename Key, typename Hash, typename Pred>
bool Linked_hash_set<Key, Hash, Pred>::empty() const
{
  return m_keys_into_list_map->empty();
}

template<typename Key, typename Hash, typename Pred>
typename Linked_hash_set<Key, Hash, Pred>::size_type
  Linked_hash_set<Key, Hash, Pred>::max_size() const
{
  return std::min(m_keys_into_list_map->max_size(), m_value_list.max_size());
}

template<typename Key, typename Hash, typename Pred>
void swap(Linked_hash_set<Key, Hash, Pred>& val1, Linked_hash_set<Key, Hash, Pred>& val2)
{
  val1.swap(val2);
}

} // namespace flow::util

