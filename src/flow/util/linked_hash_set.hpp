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

#include "flow/util/detail/linked_hash.hpp"
#include "flow/util/util_fwd.hpp"
#include <cstddef>

namespace flow::util
{

/**
 * An object of this class is a map that combines the lookup speed of an `unordered_set<>` and ordering and
 * iterator stability capabilities of a `list<>`.
 *
 * This is just like Linked_hash_map, except it only stores keys -- no mapped values.  All comments, except for
 * self-explanatory differences, from Linked_hash_map apply here.  Thus I will only speak of differences below to
 * avoid duplication of this header.  Incidentally the most visible API difference (aside from having no `Mapped`s
 * to speak of, only `Key`s) is that Linked_hash_set lacks `(*this)[]` operator; so one always uses insert() to
 * insert.
 *
 * Move semantics for keys are supported (let `x` be a `*this`):
 *   - `x.insert(std::move(a_key))`;
 *   - `x.insert(Key{...})`.
 *
 * The iterators are, really, `list<Key>` const-iterators; and as such are not invalidated except
 * due to direct erasure of a given pointee.
 *
 * @internal
 * ### Impl notes ###
 * It's very much like Linked_hash_map; just the `list` #m_value_list stores only `Key`s as opposed to
 * `pair<const Key, Mapped>`s.  See Linked_hash_map.
 * @endinternal
 *
 * @tparam Key_t
 *         Key type.  Same as for Linked_hash_map.
 * @tparam Hash_t
 *         Hasher type.  Same as for Linked_hash_map.
 * @tparam Pred_t
 *         Equality functor type.  Same as for Linked_hash_map.
 */
template<typename Key_t, typename Hash_t, typename Pred_t>
class Linked_hash_set
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Key = Key_t;

  /// Convenience alias for template arg.
  using Hash = Hash_t;

  /// Convenience alias for template arg.
  using Pred = Pred_t;

  /// Short-hand for values, which in this case are simply the keys.
  using Value = Key;

private:
  // Types. These are here in the middle of public block due to inability to forward-declare aliases.

  /// Short-hand for doubly linked list of `Key`s.
  using Value_list = std::list<Value>;

public:

  // Types (continued).

  /// Expresses sizes/lengths of relevant things.
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

  /// For container compliance (hence the irregular capitalization): #Key type.
  using key_type = Key;
  /// For container compliance (hence the irregular capitalization): #Value type.
  using value_type = Value;
  /// For container compliance (hence the irregular capitalization): #Hash type.
  using hasher = Hash;
  /// For container compliance (hence the irregular capitalization): #Pred type.
  using key_equal = Pred;
  /// For container compliance (hence the irregular capitalization): pointer to #Key type.
  using pointer = Value*;
  /// For container compliance (hence the irregular capitalization): pointer to `const Key` type.
  using const_pointer = const Value*;
  /// For container compliance (hence the irregular capitalization): reference to #Key type.
  using reference = Value&;
  /// For container compliance (hence the irregular capitalization): reference to `const Key` type.
  using const_reference = const Value&;
  /// For container compliance (hence the irregular capitalization): `Iterator` type.
  using iterator = Iterator;
  /// For container compliance (hence the irregular capitalization): `Const_iterator` type.
  using const_iterator = Const_iterator;

  // Constructors/destructor.

  /**
   * Constructs empty structure with some basic parameters.
   *
   * @param n_buckets
   *        Number of buckets for the unordered (hash) table.  Special value -1 (default) will cause us to use
   *        whatever `unordered_set<>` would use by default.
   * @param hasher_obj
   *        Instance of the hash function type (`hasher_obj(k) -> size_t` should be hash of `Key k`).
   * @param pred
   *        Instance of the equality function type (`pred(k1, k2)` should return `true` if and
   *        only if the `Key`s are equal by value).
   */
  Linked_hash_set(size_type n_buckets = size_type(-1),
                  const Hash& hasher_obj = Hash{},
                  const Pred& pred = Pred{});

  /**
   * Constructs structure with some basic parameters, and values initialized from initializer list.
   * The values are inserted as if `insert(v)` was called for each element `v` in `values`
   * **in reverse order**.  Since the canonical ordering places the *newest* (last inserted/`touch()`ed)
   * element at the *front* of the ordering, that means that forward iteration through the set (right after this
   * constructor runs) will yield values in the *same* order as in initializer list `values`.
   *
   * @param values
   *        Values with which to fill the structure after initializing it.
   *        Typically you'd provide a series of keys like this:
   *        `{ key1, key2, ... }`.  They will appear in iterated sequence in the same order as
   *        they appear in this list.
   * @param n_buckets
   *        See other constructor.
   * @param hasher_obj
   *        See other constructor.
   * @param pred
   *        See other constructor.
   */
  explicit Linked_hash_set(std::initializer_list<Value> values,
                           size_type n_buckets = size_type(-1),
                           const Hash& hasher_obj = Hash{},
                           const Pred& pred = Pred{});

  /**
   * Constructs object that is a copy of the given source.  Equivalent to default-ction followed by `operator=(src)`.
   *
   * @param src
   *        Source object.
   */
  Linked_hash_set(const Linked_hash_set& src);

  /**
   * Constructs object by making it equal to the given source, while the given source becomes as-if default-cted.
   * Equivalent to default-ction followed by `operator=(std::move(src))`.
   *
   * This is a constant-time operation.
   *
   * @param src
   *        Source object which is emptied.
   */
  Linked_hash_set(Linked_hash_set&& src);

  // Methods.

  /**
   * Overwrites this object with a copy of the given source.  We become equal to `src` but independent of it to a
   * common-sense extent.  In addition, the hasher instance and equality predicate are copied from `src`.  Finally, a
   * reasonable attempt is made to also make the internal structure of the hash map to be similar to that of `src.
   *
   * @param src
   *        Source object.  No-op if `this == &src`.
   * @return `*this`.
   */
  Linked_hash_set& operator=(const Linked_hash_set& src);

  /**
   * Overwrites this object making it identical to the given source, while the given source becomes as-if default-cted.
   *
   * This is a constant-time operation, plus whatever is the cost of `this->clear()` (linear in pre-op `.size()`).
   *
   * @param src
   *        Source object which is emptied; except no-op if `this == &src`.
   * @return `*this`.
   */
  Linked_hash_set& operator=(Linked_hash_set&& src);

  /**
   * Swaps the contents of this structure and `other`.  This is a constant-time operation, as internal
   * representations are swapped instead of any copy-assignment.
   *
   * @see The `swap()` free function.
   *      It is generally best (equivalent but covers more generic cases) to use the ADL-enabled `swap(a, b)`
   *      pattern instead of this member function.  That is: `using std::swap; ...; swap(a, b);`.
   *      (Details are outside our scope here; but in short ADL will cause the right thing to happen.)
   *
   * @param other
   *        The other structure.
   */
  void swap(Linked_hash_set& other);

  /**
   * Attempts to insert (copying it) the given keyinto the map; if key
   * already in `*this` makes no change.  See also the overload which can avoid a copy and destructively move
   * the key instead.
   *
   * Return value indicates various info of interest about what occurred or did not occur.
   * If inserted, the new element is considered "newest," as if by touch().  If not inserted, the existing element
   * location is not affected (use touch() upon consulting the return value, if this is desirable).
   *
   * @param key
   *        The key to attempt to insert.  A copy of this value is placed in `*this`.
   * @return A pair whose second element is true if and only if the insertion occurred; and whose first element
   *         is an iterator pointing to either the newly inserted element or already present one with a key equal to
   *         `key`.
   */
  std::pair<Iterator, bool> insert(const Key& key);

  /**
   * Identical to the other overload, except that (if key not already present in `*this`) the key
   * is moved, not copied, into `*this`.
   *
   * @param key
   *        The key to attempt to insert (it is moved-from, if insertion occurs).
   * @return See other overload.
   */
  std::pair<Iterator, bool> insert(Key&& key);

  /**
   * Attempts to find value at the given key in the map.  Key presence is determined identically to how it would be
   * done in an `unordered_set<Key_t, Hash_t, Pred_t>`, with the particular #Hash and #Pred instances given to ctor
   * (typically their default-cted instances, typically occupying no memory).
   *
   * The returned iterator (if valid) *cannot* be used to mutate the key inside the map.
   *
   * @param key
   *        Key whose equal to find.
   * @return If found, iterator to the key/mapped-value pair with the equivalent key; else `this->end()`.
   */
  Const_iterator find(const Key& key) const;

  /**
   * Returns the number of times a key equal to the given one is present (as-if via find()) in the map: either 1 or 0.
   *
   * @param key
   *        Key whose equal to find.
   * @return 0 or 1.
   */
  size_type count(const Key& key) const;

  /**
   * Given a valid iterator into the structure, makes the pointed-to element "newest" by moving it from wherever it
   * is to be first in the iteration order.  Behavior undefined if iterator invalid.
   *
   * The iterator continues to be valid.
   *
   * @param it
   *        Iterator to an element of the structure.
   */
  void touch(const Const_iterator& it);

  /**
   * Given a key into the structure, makes the corresponding element "newest" by moving it from wherever it
   * is to be first in the iteration order; or does nothing if no such key.  `find(key)` equivalent is performed
   * first.  Return value indicates whether it was found.
   *
   * @param key
   *        Key whose equal to find.
   * @return `true` if the key was found (even if it was already "newest"); `false` if not found.
   */
  bool touch(const Key& key);

  /**
   * Erases the element pointed to by the given valid iterator.  Behavior undefined if it is not valid.  `it` becomes
   * invalid.
   *
   * @param it
   *        Iterator of element to erase.
   * @return Iterator one position past (i.e., "older") than `it`, before `*it` was removed.
   */
  Const_iterator erase(const Const_iterator& it);

  /**
   * Erases all elements in the range [`it_newest`, `it_past_oldest`).  Behavior undefined if a given iterator is
   * invalid, or if the range is invalid.  Corner case: an empty range is allowed; then this no-ops.  Unless no-op,
   * `it_newest` becomes invalid.
   *
   * @param it_newest
   *        Iterator of first ("newest") element to erase.
   * @param it_past_oldest
   *        Iterator of one past last ("oldest") element to erase.
   * @return `it_past_oldest` copy.
   */
  Const_iterator erase(const Const_iterator& it_newest, const Const_iterator& it_past_oldest);

  /**
   * Erases the element with the given key, if it exists.  `find(key)` equivalent is performed
   * first.  Return value indicates whether it existed.
   *
   * @param key
   *        Key such that its equal's (if found) element will be erased.
   * @return Number of elements erased (0 or 1).
   */
  size_type erase(const Key& key);

  /// Makes it so that `size() == 0`.
  void clear();

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
   * Returns true if and only if container is empty.  Same performance as of `unordered_set<>`.
   * @return Ditto.
   */
  bool empty() const;

  /**
   * Returns number of elements stored.  Same performance as of `unordered_set<>.`
   * @return Ditto.
   */
  size_type size() const;

  /**
   * Returns max number of elements that can be stored.  Same performance as of `unordered_set<>` + `list<>`.
   * @return Ditto.
   */
  size_type max_size() const;

private:

  // Data.

  /// Analogous to Linked_hash_map::m_value_list; but simpler in that it just stores `Key`s, not pairs of (stuff).
  Value_list m_value_list;

  /**
   * Analogous to Linked_hash_map::m_value_iter_set; just configured to generate a simpler `.iter()` off each element
   * by supplying `false` instead of `true` for the last template arg.
   */
  Linked_hash_key_set<Key, Iterator, Hash, Pred, false> m_value_iter_set;
}; // class Linked_hash_set

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Key_t, typename Hash_t, typename Pred_t>
Linked_hash_set<Key_t, Hash_t, Pred_t>::Linked_hash_set(size_type n_buckets,
                                                        const Hash& hasher_obj,
                                                        const Pred& pred) :
  /* @todo Using detail:: like this is technically uncool, but so far all alternatives look worse.
   * We blame the somewhat annoying ctor API for unordered_*. */
  m_value_iter_set((n_buckets == size_type(-1))
                     ? boost::unordered::detail::default_bucket_count
                     : n_buckets,
                   hasher_obj, pred)
{
  // That's all.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
Linked_hash_set<Key_t, Hash_t, Pred_t>::Linked_hash_set(std::initializer_list<Value> values,
                                                        size_type n_buckets,
                                                        const Hash& hasher_obj,
                                                        const Pred& pred) :
  // Their initializer_list is meant for a set of keys, but it is perfect for our list of keys.
  m_value_list(values),
  m_value_iter_set((n_buckets == size_type(-1))
                     ? boost::unordered::detail::default_bucket_count // See @todo above.
                     : n_buckets,
                   hasher_obj, pred)
{
  // Now link each key in the quick-lookup table to its stored location in the ordering.
  const auto value_list_end_it = m_value_list.cend();
  for (auto value_list_it = m_value_list.cbegin(); value_list_it != value_list_end_it; ++value_list_it)
  {
    // Note that value_list_it contains both the iterator (lookup result) and the lookup key (iterator pointee).
    m_value_iter_set.insert(value_list_it);
  }
}

template<typename Key_t, typename Hash_t, typename Pred_t>
Linked_hash_set<Key_t, Hash_t, Pred_t>::Linked_hash_set(const Linked_hash_set& src)
  // An empty m_value_iter_set is constructed here but immediately replaced within the {body}.
{
  operator=(src);
}

template<typename Key_t, typename Hash_t, typename Pred_t>
Linked_hash_set<Key_t, Hash_t, Pred_t>::Linked_hash_set(Linked_hash_set&& src)
  // An empty m_value_iter_set is constructed here but immediately replaced within the {body}.
{
  operator=(std::move(src));
}

template<typename Key_t, typename Hash_t, typename Pred_t>
Linked_hash_set<Key_t, Hash_t, Pred_t>&
  Linked_hash_set<Key_t, Hash_t, Pred_t>::operator=(const Linked_hash_set& src)
{
  /* See Linked_hash_map equivalent method, to which this is analogous.  Keeping comments here light.
   * Though we don't have to do the reinterpret_cast<> thing; can just assign the list to src's counterpart;
   * in our case Value is just Key -- no const-ness involved. */

  using Value_iter_set = decltype(m_value_iter_set);

  if (&src == this)
  {
    return *this;
  }
  // else

  m_value_list = src.m_value_list;

  const auto& src_value_iter_set = src.m_value_iter_set;
  m_value_iter_set = Value_iter_set{src_value_iter_set.bucket_count(),
                                    src_value_iter_set.hash_function(),
                                    src_value_iter_set.key_eq()};

  const auto value_list_end_it = m_value_list.cend();
  for (auto value_list_it = m_value_list.cbegin(); value_list_it != value_list_end_it; ++value_list_it)
  {
    m_value_iter_set.insert(value_list_it);
  }

  return *this;
} // Linked_hash_set::operator=()

template<typename Key_t, typename Hash_t, typename Pred_t>
Linked_hash_set<Key_t, Hash_t, Pred_t>&
  Linked_hash_set<Key_t, Hash_t, Pred_t>::operator=(Linked_hash_set&& src)
{
  if (&src != this)
  {
    clear();
    swap(src);
  }
  return *this;
}

template<typename Key_t, typename Hash_t, typename Pred_t>
void Linked_hash_set<Key_t, Hash_t, Pred_t>::swap(Linked_hash_set& other)
{
  using std::swap;

  swap(m_value_iter_set, other.m_value_iter_set); // unordered_set<> exchange; constant-time for sure at least.
  swap(m_value_list, other.m_value_list); // list<> exchange (probably ~= head+tail pointer pairs exchanged).
  // Per cppreference.com `list<>::iterator`s (inside the `_maps`s) remain valid after list<>s swapped.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
std::pair<typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Iterator, bool>
  Linked_hash_set<Key_t, Hash_t, Pred_t>::insert(const Key& key)
{
  using std::pair;

  const auto set_it = m_value_iter_set.find(key);
  if (set_it != m_value_iter_set.end())
  {
    return pair<Iterator, bool>{set_it->iter(), false}; // *set_it is Linked_hash_key.
  }
  // else

  /* Insert it at the front: as advertised, new element is "touched," meaning it is made "newest," so goes at start.
   * Note that "it" = a copy of key; this invokes Key copy ctor, as emplace_front() forwards to it. */
  m_value_list.emplace_front(key);

  /* Iterator to the new element is therefore iterator to start of list of `Key`s.
   * And make sure we can look it up in the future quickly (such as what is done above).
   * Linked_hash_key_set m_value_iter_set achieves these aims black-boxily. */
  const auto list_it = m_value_list.cbegin();
  m_value_iter_set.insert(list_it);
  return pair<Iterator, bool>{list_it, true};
}

template<typename Key_t, typename Hash_t, typename Pred_t>
std::pair<typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Iterator, bool>
  Linked_hash_set<Key_t, Hash_t, Pred_t>::insert(Key&& key)
{
  using std::pair;

  // Same as other insert() but construct value in-place inside the list<> as-if: Key k2{move(k)}.

  const auto set_it = m_value_iter_set.find(key);
  if (set_it != m_value_iter_set.end())
  {
    return pair<Iterator, bool>{set_it->iter(), false}; // *set_it is Linked_hash_key.
  }
  // else

  m_value_list.emplace_front(std::move(key)); // <-- The difference.

  const auto list_it = m_value_list.cbegin();
  m_value_iter_set.insert(list_it);
  return pair<Iterator, bool>{list_it, true};
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::find(const Key& key) const
{
  const auto set_it = m_value_iter_set.find(key);
  return (set_it == m_value_iter_set.cend()) ? m_value_list.cend() : set_it->iter();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::size_type
  Linked_hash_set<Key_t, Hash_t, Pred_t>::count(const Key& key) const
{
  return m_value_iter_set.count(key);
}

template<typename Key_t, typename Hash_t, typename Pred_t>
void Linked_hash_set<Key_t, Hash_t, Pred_t>::touch(const Const_iterator& it)
{
  m_value_list.splice(m_value_list.begin(), m_value_list, it);
}

template<typename Key_t, typename Hash_t, typename Pred_t>
bool Linked_hash_set<Key_t, Hash_t, Pred_t>::touch(const Key& key)
{
  const auto it = find(key);
  if (it == end())
  {
    return false;
  }
  // else

  touch(it);
  return true;
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::erase(const Const_iterator& it)
{
  m_value_iter_set.erase(*it);
  return m_value_list.erase(it);
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::erase(const Const_iterator& it_newest, const Const_iterator& it_past_oldest)
{
  for (auto it = it_newest; it != it_past_oldest; ++it)
  {
    m_value_iter_set.erase(*it);
  }

  return m_value_list.erase(it_newest, it_past_oldest);
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::size_type
  Linked_hash_set<Key_t, Hash_t, Pred_t>::erase(const Key& key)
{
  const auto set_it = m_value_iter_set.find(key);
  if (set_it == m_value_iter_set.end())
  {
    return 0;
  }
  // else

  const auto list_it = set_it->iter();
  m_value_iter_set.erase(set_it);
  m_value_list.erase(list_it);

  return 1;
}

template<typename Key_t, typename Hash_t, typename Pred_t>
void Linked_hash_set<Key_t, Hash_t, Pred_t>::clear()
{
  m_value_iter_set.clear();
  m_value_list.clear();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::newest() const
{
  return m_value_list.cbegin();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::const_newest() const
{
  return newest(); // For us Iterator = Const_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::begin() const
{
  return newest();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::cbegin() const
{
  return begin(); // For us Iterator = Const_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::past_oldest() const
{
  return m_value_list.cend();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::const_past_oldest() const
{
  return past_oldest(); // For us Iterator = Const_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::end() const
{
  return past_oldest();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::cend() const
{
  return end(); // For us Iterator = Const_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Reverse_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::oldest() const
{
  return m_value_list.crbegin();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_reverse_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::const_oldest() const
{
  return oldest(); // For us Iterator = Const_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Reverse_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::rbegin() const
{
  return oldest();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_reverse_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::crbegin() const
{
  return rbegin(); // For us Reverse_iterator = Const_reverse_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Reverse_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::past_newest() const
{
  return m_value_list.crend(); // For us Reverse_iterator = Const_reverse_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_reverse_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::const_past_newest() const
{
  return past_newest(); // For us Reverse_iterator = Const_reverse_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Reverse_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::rend() const
{
  return past_newest();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::Const_reverse_iterator
  Linked_hash_set<Key_t, Hash_t, Pred_t>::crend() const
{
  return m_value_list.rend(); // For us Reverse_iterator = Const_reverse_iterator.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::size_type
  Linked_hash_set<Key_t, Hash_t, Pred_t>::size() const
{
  return m_value_iter_set.size(); // I'm skeptical/terrified of list::size()'s time complexity.
}

template<typename Key_t, typename Hash_t, typename Pred_t>
bool Linked_hash_set<Key_t, Hash_t, Pred_t>::empty() const
{
  return m_value_list.empty();
}

template<typename Key_t, typename Hash_t, typename Pred_t>
typename Linked_hash_set<Key_t, Hash_t, Pred_t>::size_type
  Linked_hash_set<Key_t, Hash_t, Pred_t>::max_size() const
{
  return std::min(m_value_iter_set.max_size(), m_value_list.max_size());
}

template<typename Key_t, typename Hash_t, typename Pred_t>
void swap(Linked_hash_set<Key_t, Hash_t, Pred_t>& val1, Linked_hash_set<Key_t, Hash_t, Pred_t>& val2)
{
  val1.swap(val2);
}

} // namespace flow::util
