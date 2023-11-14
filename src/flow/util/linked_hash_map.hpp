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
#include <boost/unordered_map.hpp>
#include <boost/move/unique_ptr.hpp>

namespace flow::util
{

/**
 * An object of this class is a map that combines the lookup speed of a `boost::unordered_map<>` and ordering and
 * iterator stability capabilities of an `std::list<>`.
 *
 * The API is generally that of an `unordered_map<>`.  The differences essentially all have to do with iterators.
 * This map introduces a concept of "newness," which determines the iteration order.  Moreover, *every* iterator remains
 * valid except (of course) under erasure of the underlying element.  Newness is defined as follows inductively:
 * whenever an element is inserted, it is "newest," thus it is placed at the front of the iterator order.  Furthermore,
 * the methods touch() can be used to make any element "newest" (moved to the front of the iterator order).  Iterator
 * thus formed orders elements from newest to oldest (hence newest() is begin(), past_oldest() is end()).
 *
 * Performance expectations: The best way to determine a method's time needs is to
 * imagine what it must do.  If it must perform a lookup by key, that is an `unordered_map<>` lookup resulting in an
 * (at least internal) iterator.  If it must insert an element, it is always inserted at the start of a `list`; and
 * also into an `unordered_map<>`.  If it must erase an element based on an iterator, that element is erased from a list
 * based on that iterator; and also by key from an `unordered_map<>`.  Iteration itself is iteration along a `list`.
 * But essentially, every operation is either near constant time or constant time.
 * In terms of space needs, this essentially stores the values themselves in a `list`; and also a copy of each key
 * in an `unordered_map<>`, which also stores a pointer or list iterator per element.
 *
 * ### Thread safety ###
 * Same as for `unordered_map<>`.
 *
 * @tparam Key
 *         Key type.  Same requirements and behavior as `unordered_map<>` counterpart.  Also (is it "in particular"?):
 *         `Key` must be Assignable, which is STL-speak for: If `Key x, y` are objects of this type,
 *         then `x = y;` is valid and has all the usual semantics.  (There are other requirements, but
 *         that's the "controversial" one of interest.)  In particular, `Key` cannot be of the form `T const` --
 *         more commonly written as `const T` (but recall that, say, `const char*` = `char const *` really;
 *         which is therefore fine here).
 * @tparam Mapped
 *         The 2nd (satellite) part of the `Value` pair type.  Same requirements and behavior as `unordered_map<>`
 *         counterpart.  Colloquially, in a K->V map, this is V, while formally the values stored are (K, V)
 *         pairs.
 * @tparam Hash
 *         Hasher type.  Same requirements and behavior as `unordered_map<>` counterpart.  To get a hasher
 *         object, one must be able to call: `Hash h = Hash()`.  To then hash a `Key`, one must be able to
 *         call `h(key)`.  Typically one will simply define a `size_t hash_value(Key)` function, which will be
 *         activated via the default value for this template parameter.  Defaults to `boost::hash<Key>`.
 * @tparam Pred
 *         Equality functor type.  Same requirements and behavior as `unordered_map<>` counterpart.
 *         Once a functor object `Pred e = Pred()` is obtained, `bool eq = e(a, b)` must return whether `a` equals
 *         `b`, where `a` and `b` are keys.  Typically `operator==()` will be used via the default template parameter.
 *         Defaults to `std::equal_to<Key>`.
 */
template<typename Key, typename Mapped, typename Hash, typename Pred>
class Linked_hash_map
{
public:
  // Types.

  /// Short-hand for key/mapped-value pairs stored in the structure.
  using Value = std::pair<Key const, Mapped>;

private:
  // Types.  These are here in the middle of public block due to inability to forward-declare aliases.

  /// Short-hand for doubly linked list of (`Key`, `Mapped`) pairs.
  using Value_list = std::list<Value>;

public:

  // Types (continued).

  /// Type for index into array of items, where items are all applicable objects including `Value`s and `Key`s.
  using size_type = std::size_t;
  /// Type for difference of `size_type`s.
  using difference_type = std::ptrdiff_t;

  /// Type for iterator pointing into a mutable structure of this type.
  using Iterator = typename Value_list::iterator;

  /// Type for iterator pointing into an immutable structure of this type.
  using Const_iterator = typename Value_list::const_iterator;

  /// Type for reverse iterator pointing into a mutable structure of this type.
  using Reverse_iterator = typename Value_list::reverse_iterator;

  /// Type for reverse iterator pointing into an immutable structure of this type.
  using Const_reverse_iterator = typename Value_list::const_reverse_iterator;

  /// For container compliance (hence the irregular capitalization): `Key` type.
  using key_type = Key;
  /// For container compliance (hence the irregular capitalization): `Mapped` type.
  using mapped_type = Mapped;
  /// For container compliance (hence the irregular capitalization): `Key`/`Mapped` pair type.
  using value_type = Value;
  /// For container compliance (hence the irregular capitalization): `Hash` type.
  using hasher = Hash;
  /// For container compliance (hence the irregular capitalization): `Pred` type.
  using key_equal = Pred;
  /// For container compliance (hence the irregular capitalization): pointer to `Key`/`Mapped` pair type.
  using pointer = Value*;
  /// For container compliance (hence the irregular capitalization): pointer to `const Key`/`Mapped` pair type.
  using const_pointer = Value const *;
  /// For container compliance (hence the irregular capitalization): reference to `Key`/`Mapped` pair type.
  using reference = Value&;
  /// For container compliance (hence the irregular capitalization): reference to `const Key`/`Mapped` pair type.
  using const_reference = Value const &;
  /// For container compliance (hence the irregular capitalization): #Iterator type.
  using iterator = Iterator;
  /// For container compliance (hence the irregular capitalization): #Const_iterator type.
  using const_iterator = Const_iterator;

  // Constructors/destructor.

  /**
   * Constructs empty structure with some basic parameters.
   *
   * @param n_buckets
   *        Number of buckets for the unordered (hash) table.  Special value -1 (default) will cause us to use
   *        whatever `unordered_map<>` would use by default.
   * @param hasher_instance
   *        Instance of the hash function type (`hasher_instance(Key k)` should be `size_type`d hash of key `k`).
   * @param key_equal_instance
   *        Instance of the equality function type (`key_equal_instance(Key k1, Key k2)` should return `true` if and
   *        only if `k1` equals `k2`).
   */
  explicit Linked_hash_map(size_type n_buckets = size_type(-1),
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
   *        Typically you'd provide a series of key/value pairs like this:
   *        `{{ key1, value1 }, { key2, value2 }, ...}`.  They will appear in iterated sequence in the same order as
   *        they appear in this list.
   * @param n_buckets
   *        See other constructor.
   * @param hasher_instance
   *        See other constructor.
   * @param key_equal_instance
   *        See other constructor.
   */
  explicit Linked_hash_map(std::initializer_list<Value> values,
                           size_type n_buckets = size_type(-1),
                           Hash const & hasher_instance = Hash(),
                           Pred const & key_equal_instance = Pred());

  /**
   * Constructs object that is a copy of the given source.  Equivalent to `operator=(src)`.
   *
   * @param src
   *        Source object.
   */
  Linked_hash_map(Linked_hash_map const & src);

  /**
   * Constructs object by making it equal to the given source, while the given source becomes as-if default-cted.
   *
   * @param src
   *        Source object which is emptied.
   */
  Linked_hash_map(Linked_hash_map&& src);

  // Methods.

  /**
   * Overwrites this object with a copy of the given source.  We become equal to `src` but independent of it to the max
   * extent possible (if you've got pointers stored in there, for example, the pointers are copied, not the values
   * at those pointers).  In addition, the hasher instance and equality predicate are copied from `src`.  Finally, a
   * reasonable attempt is made to also make the internal structure of the hash map to be similar to that of `src.
   *
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Linked_hash_map& operator=(Linked_hash_map const & src);

  /**
   * Overwrites this object making it equal to the given source, while the given source becomes as-if default-cted.
   *
   * @param src
   *        Source object which is emptied (unless it *is* `*this`; then no-op).
   * @return `*this`.
   */
  Linked_hash_map& operator=(Linked_hash_map&& src);

  /**
   * Attempts to insert the given key/mapped-value pair into the map.  If the key is already present in the map,
   * does nothing.  Return value indicates various info of interest about what occurred or did not occur.
   * Key presence is determined according to the `Pred` template parameter which determines equality of 2 given keys;
   * and via the `Hash` template parameter that enables efficient hash-based lookup.
   * If inserted, the new element is considered "newest," as if by touch().  If not inserted, the existing element
   * location is not affected.
   *
   * @param key_and_mapped
   *        The key/mapped-value pair to attempt to insert.  This value is copied, and the copy is inserted.
   * @return A pair whose second element is true if and only if the insertion occurred; and whose first element
   *         is an iterator pointing to either the newly inserted element or already present one with a key equal to
   *         `key_and_mapped.first`.
   */
  std::pair<Iterator, bool> insert(Value const & key_and_mapped);

  /**
   * Attempts to find value at the given key in the map.  Key presence is determined according to the `Pred` template
   * parameter which determines equality of 2 given keys; and via the `Hash` template parameter that enables efficient
   * hash-based lookup.  The returned iterator (if valid) can be used to mutate the elements inside the map.
   *
   * As long as the value is not removed from the map, the reference will continue to be valid.
   *
   * Any subsequent writes to the referred to (by returned iterator) area of memory will NOT have the effect of touch().
   * If you need it, call touch() yourself.
   *
   * @param key
   *        Key whose equal to find.
   * @return If found, iterator to the key/mapped-value pair with the equivalent key; else `this->end()`.
   */
  Iterator find(Key const & key);

  /**
   * Attempts to find value at the given key in the map.  Key presence is determined according to the `Pred` template
   * parameter which determines equality of 2 given keys; and via the `Hash` template parameter that enables efficient
   * hash-based lookup.  The returned iterator (if valid) cannot be used to mutate the elements inside the map.
   *
   * As long as the value is not removed from the map, the iterator will continue to be valid.
   *
   * @param key
   *        Key whose equal to find.
   * @return If found, iterator to the key/mapped-value pair with the equivalent key; else `this->const_past_oldest()`.
   */
  Const_iterator find(Key const & key) const;

  /**
   * Returns the number of times a key is equivalent to the given one is present in the map: either 1 or 0.
   *
   * @param key
   *        Key whose equal to find.
   * @return 0 or 1.
   */
  size_type count(Key const & key) const;

  /**
   * Equivalent to `insert(Value(key, Mapped())).first->second` (but avoids unnecessarily invoking `Mapped()`/generally
   * strives for better performance).  Less formally, it either finds the value at the given key, or if not found
   * inserts one with a default-constructed value; then returns reference to the in-structure stored `Mapped` value
   * which can be used to to read and/or modify that value directly.
   *
   * Note that if `Mapped& x` is returned, then although `x` is mutable, in actuality `x.first` is `const`; so only
   * `x.second` is truly mutable.  You must not write to the key (such as via a `const_cast<>`); doing so will result
   * in undefined behavior.
   *
   * If inserted, the new element is considered "newest," as if by touch().  If not inserted, the existing element
   * location is not affected.
   *
   * As long as the value is not removed from the map, the reference will continue to be valid.
   *
   * Any subsequent writes to the referred to area of memory will NOT have the effect of touch().  If you need it,
   * call touch() yourself.
   *
   * @param key
   *        Key whose equal to find or insert if not found.
   * @return Reference to mutable Mapped value directly inside the data structure.
   */
  Mapped& operator[](Key const & key);

  /**
   * Returns reference to mutable front ("newest") element in the structure; formally equivalent to
   * `*(this->newest())`.
   *
   * OK to call when empty(); but behavior undefined if you attempt to access the result in any way if either empty()
   * when this was called; or if `!empty()` at that time, but the underlying element is erased at time of access.
   * If not empty() when this was called, then resulting reference continues to be valid as long as the underlying
   * element is not erased; however, in the future the reference (while referring to the same element) might not refer
   * to front ("newest") element any longer.  (Informally, most uses would only call front() when `!empty()`, and would
   * access it immediately and but once.  However, I'm listing the corner cases above.)
   *
   * Note that if `Mapped& x` is returned, then although `x` is mutable, in actuality `x.first` is const; so only
   * `x.second` is truly mutable.  You must not write to the key (such as via a `const_cast<>`); doing so will result
   * in undefined behavior.
   *
   * @return Reference to mutable value directly inside the data structure; or to undefined location if
   *         currently empty().  Note that only the `Mapped` part of `Value` is mutable.
   */
  Value& front();

  /**
   * Returns reference to mutable back ("oldest") element in the structure; formally equivalent to
   * `*(--this->past_oldest())`.
   *
   * All other comments for front() apply analogously.
   *
   * @return Reference to mutable `Mapped` value directly inside the data structure; or to undefined location if
   *         currently empty().
   */
  Value& back();

  /**
   * Returns reference to immutable front ("newest") element in the structure; formally equivalent to
   * `*(this->const_newest())`.
   *
   * OK to call when empty(); but behavior undefined if you attempt to access the result in any way if either empty()
   * when this was called; or if `!empty()` at that time, but the underlying element is erased at time of access.
   * If not empty() when this was called, then resulting reference continues to be valid as long as the underlying
   * element is not erased; however, in the future the reference (while referring to the same element) may not refer
   * to front ("newest") element any longer.  (Informally, most uses would only call front() when `!empty()`, and would
   * access it immediately and but once.  However, I'm listing the corner cases above.)
   *
   * @return Reference to immutable `Mapped` value directly inside the data structure; or to undefined location if
   *         currently empty().
   */
  Value const & const_front() const;

  /**
   * Returns reference to immutable back ("oldest") element in the structure; formally equivalent
   * to `*(--this->const_past_oldest())`.
   *
   * All other comments for const_front() apply analogously.
   *
   * @return Reference to immutable `Mapped` value directly inside the data structure; or to undefined location if
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
   * @return `true` if the key was found (even if it was already "newest"); `false` if not found.
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
   * Erases all elements in the range [`it_newest`, `it_past_oldest`).  Behavior undefined if a given iterator is
   * invalid.  `it_newest` becomes invalid.
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

  /// Makes it so that `size() == 0`.
  void clear();

  /**
   * Swaps the contents of this structure and `other`.  This is a constant-time operation.
   *
   * @param other
   *        The other structure.
   */
  void swap(Linked_hash_map& other);

  /**
   * Synonym of newest().
   * @return See newest().
   */
  Iterator begin();

  /**
   * Returns first, a/k/a "newest," element's iterator.
   * @return Ditto.
   */
  Iterator newest();

  /**
   * Synonym of past_oldest().  Exists as standard container method.
   * @return See past_oldest().
   */
  Iterator end();

  /**
   * Returns one past last, a/k/a "oldest," element's iterator.
   * @return Ditto.
   */
  Iterator past_oldest();

  /**
   * Synonym of const_newest().  Exists as standard container method (hence the odd formatting).
   * @return See const_newest().
   */
  Const_iterator cbegin() const;

  /**
   * Synonym of cbegin().  Exists to satisfy the C++11 rangy stuff (which makes `for(:)` -- and other magic -- work).
   * @return See cbegin().
   */
  Const_iterator begin() const;

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
   * Synonym of cend().  Exists to satisfy the C++11 rangy stuff (which makes `for(:)` -- and other magic -- work).
   * @return See cend().
   */
  Const_iterator end() const;

  /**
   * Returns one past last, a/k/a "oldest," element's iterator (to immutable element).
   * @return Ditto.
   */
  Const_iterator const_past_oldest() const;

  /**
   * Synonym of oldest().
   * @return See oldest().
   */
  Reverse_iterator rbegin();

  /**
   * Returns first, a/k/a "oldest," element's reverse iterator.
   * @return Ditto.
   */
  Reverse_iterator oldest();

  /**
   * Synonym of past_newest().  Exists as standard container method.
   * @return See past_newest().
   */
  Reverse_iterator rend();

  /**
   * Returns one past last, a/k/a "newest," element's reverse iterator.
   * @return Ditto.
   */
  Reverse_iterator past_newest();

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
   * Returns true if and only if container is empty.  Same performance as of `unordered_map<>`.
   * @return Ditto.
   */
  bool empty() const;

  /**
   * Returns number of elements stored.  Same performance as of `unordered_map<>.`
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
   * Helper that modifies #m_value_list and #m_keys_into_list_map so that `key_and_mapped`'s copy is inserted into
   * the structure.  Pre-condition is that `key_and_mapped.first` is not in the structure (else behavior undefined).
   *
   * @param key_and_mapped
   *        Same as in insert().
   * @return Same as in `insert().first`.
   */
  Iterator insert_impl(Value const & key_and_mapped);

  // Types.

  /// Short-hand for iterator into doubly linked list of (`Key`, `Mapped`) pairs.
  using Value_list_iter = Iterator;

  /// Short-hand for `const` iterator into doubly linked list of (`Key`, `Mapped`) pairs.
  using Value_list_const_iter = Const_iterator;

  /// Short-hand for a hash map that maps `Key` to iterator into doubly linked list of (`Key`, `Mapped`) pairs.
  using Key_to_value_iter_map = boost::unordered_map<Key, Value_list_iter, Hash, Pred>;

  // Data.

  /**
   * The actual values -- which, as in `unordered_map<K, M>`, are instances of `Value` = `pair<Key, Mapped>` --
   * are stored in here, in the order in which user would iterate over them.  If `Value v` is in this list, then no
   * `Value v1 == v` can be elsewhere in the list.  The order is semantically defined to be from "newest" to "oldest."
   * Therefore, any newly inserted value goes at the START of the list.  Similarly, any "touched" value is moved to
   * the START of the list (see touch() and other methods that are documented as "touching" the referenced key).
   * This ordering is what a normal `unordered_map<K, M>` would not supply (it's in the name!) but that we advertise.
   *
   * Since #m_keys_into_list_map stores keys, why store the keys here duplicately?  Answer: that way we can expose
   * iterators into #m_value_list directly to the user; so that they can take an iterator `I` and directly access
   * the key and mapped value via `I->first` and `I->second`, respectively -- as is expected of any map container.
   * This does, however, come at some memory cost.
   *
   * @todo It is probably possible to cut down on the memory cost of storing, for each element, a copy of the `Key`
   * in #m_value_list (in addition to the mandatory one in the lookup table #m_keys_into_list_map).  Perhaps the key
   * copy would be replaced by an iterator back into #m_value_list.  A custom iterator class would be necessary
   * to properly dereference this (this is non-trivial given that `operator*()` would have to return a reference
   * to a pair which is no longer stored anywhere in this hypothetical design).  Moreover, iterators exposed to the
   * user would become invalid the same way an `unordered_map<>` iterator does due to seemingly unrelated changes.
   * Finally, the memory savings would not even exist for `Key` types roughly the size of a pointer.  All in all,
   * not a slam-dunk....
   *
   * ### Performance ###
   * Moving a value from anywhere to either end of the list is a constant-time operation
   * (assuming the source location's iterator is known).  Hence touch() is constant-time.  Moreover, touch()
   * does NOT involve a copy of a `Value` (it only involves assigning, internally, a few linked list pointers).
   * Also note that insertion is similarly constant-time (but does, necessarily, require a `Value` copy as for
   * any container).  Finally, erasure is also constant-time.  These are the only operations needed.
   */
  Value_list m_value_list;

  /**
   * Maps each `Key K` that is in #m_value_list to an iterator into #m_value_list (note the iterator points to
   * a `Value` instance, which itself contains a copy of `K` but also the `Mapped` value, in which the user likely
   * has keen interest).  This supplies the one capability #m_value_list alone cannot: near-constant-time lookup
   * of a `Value` or a `Mapped` by `Key` (a linear search would be necessary).
   *
   * The `unique_ptr<>` wrapper remains constant after setting it to non-null.  Why have it at all?  Because in at least
   * one constructor we are unable to determine all the constructor arguments by the time the constructor body
   * executes, and we don't want to construct the map until then.
   *
   * ### Performance ###
   * Anything they'll need to do to this map carries the same performance cost as if they used a
   * straight `unordered_map<>`, so by definition it is acceptable.  The only operation this does not provide is
   * iteration and insertion in the proper order, and that's done through #m_value_list instead.
   */
  boost::movelib::unique_ptr<Key_to_value_iter_map> m_keys_into_list_map;
}; // class Linked_hash_map

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Key, typename Mapped, typename Hash, typename Pred>
Linked_hash_map<Key, Mapped, Hash, Pred>::Linked_hash_map(size_type n_buckets,
                                                          hasher const & hasher_instance,
                                                          key_equal const & key_equal_instance) :
  Linked_hash_map({}, n_buckets, hasher_instance, key_equal_instance)
{
  // Nothing.
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
Linked_hash_map<Key, Mapped, Hash, Pred>::Linked_hash_map(std::initializer_list<Value> values,
                                                          size_type n_buckets,
                                                          hasher const & hasher_instance,
                                                          key_equal const & key_equal_instance) :
  // Their initializer_list is meant for a dictionary, but it is perfect for our list of pairs!
  m_value_list(values)
{
  using boost::unordered_map;

  /* Guess the default size, if they specified the default, from a dummy unrelated-type map.  Probably
   * that'll be correct.  Even use our template argument values, just in case that matters. */
  if (n_buckets == size_type(-1))
  {
    unordered_map<Key, Mapped, Hash, Pred> dummy;
    n_buckets = dummy.bucket_count();
  }

  // We use a unique_ptr<> because of the above: we couldn't immediately initialize this map.
  m_keys_into_list_map.reset(new Key_to_value_iter_map(n_buckets, hasher_instance, key_equal_instance));

  // Now link each key in the quick-lookup table to its stored location in the ordering.
  for (Value_list_iter value_list_it = m_value_list.begin(); value_list_it != m_value_list.end();
       ++value_list_it)
  {
    // Note this sets (at key K) the value: iterator to pair<K, V>.
    (*m_keys_into_list_map)[value_list_it->first] = value_list_it;
  }
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
Linked_hash_map<Key, Mapped, Hash, Pred>::Linked_hash_map(Linked_hash_map const & src) :
  m_keys_into_list_map(new Key_to_value_iter_map()) // Dummy: all this is quickly replaced.
{
  operator=(src);
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
Linked_hash_map<Key, Mapped, Hash, Pred>::Linked_hash_map(Linked_hash_map&& src) :
  m_keys_into_list_map(new Key_to_value_iter_map()) // Dummy: all this is quickly replaced.
{
  operator=(std::move(src));
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
Linked_hash_map<Key, Mapped, Hash, Pred>&
  Linked_hash_map<Key, Mapped, Hash, Pred>::operator=(Linked_hash_map const & src)
{
  if (&src == this)
  {
    return *this;
  }
  // else

  /* Values are values -- copy them over.  Recall these are (const Key, Mapped) pairs.
   *
   * Why not just: `m_value_list = src.m_value_list;`?  Answer: It fails to build, at least with this Darwin-supplied
   * clang++, due to an interesting subtlety.  Recall that the list stores (const Key, Mapped) pairs: note
   * the `const`.  Say you have iterator `it` into m_value_list; then `*it = <...>;` will not compile, as
   * *it is partially const and cannot be assigned to (it is not "Assignable," in STL-speak).  Yet the STL
   * implementation in question will indeed perform that operation.  But why?  To make the new m_value_list,
   * just create node after node, each one storing a copy of the source node's corresponding (const Key, Mapped)
   * (a/k/a Value!) pair, right?  That would only require that Value (and therefore `const Key`, which is
   * half of it) be CopyConstructible (again, in STL-speak, which basically matches English here).  Yes, but this
   * implementation, for performance, instead keeps m_value_list's existing node structure intact, only *assigning*
   * over (via operator=(), not via copy constructor!) the stored value inside each node; only "resorting" to
   * making new nodes and copy-constructing things once it has run out of m_value_list's own nodes.  Clever but
   * messes us over.
   *
   * Note that this can't be worked around by merely performing an m_value_list.clear() before the list assignment;
   * the code is still the code (even if it would not run in actuality due to the pre-clearing) and wouldn't
   * compile.  However, by writing it as an insert(), it will absolutely force it to make a bunch of copies of the
   * nodes, ensuring it compiles and works fine:
   *   m_value_list.clear();
   *   m_value_list.insert(m_value_list.end(), src.m_value_list.begin(), src.m_value_list.end());
   *
   * That's fine.  However, it does actually bypass a nice performance trick!  We would rather not make that
   * concession.  Therefore, let's temporarily pretend m_value_list and src.m_value_list store non-const Keys.
   * Then we can assing.  Note that, even though it's N lines of code, reinterpret_cast<> generates no machine
   * code: it just makes the code-that-would-have-been-generated-anyway look at the memory in a different way
   * (in this case, as storing Keys that can be overwritten instead of read-only ones).  Finally, note that
   * we specifically require that the template parameter `typename Key` be Assignable; that is the piece of the
   * puzzle that GUARANTEES this reinterpret_cast<> (in general not a safe operation) is indeed safe/correct. */
  {
    using std::pair;
    using std::list;

    using Mutable_key_value = pair<Key, Mapped>;
    using Mutable_key_value_list = list<Mutable_key_value>;

    Mutable_key_value_list* const dst_list_ptr
      = reinterpret_cast<Mutable_key_value_list*>(&m_value_list);
    const Mutable_key_value_list* const src_list_ptr
      = reinterpret_cast<const Mutable_key_value_list*>(&src.m_value_list);

    *dst_list_ptr = *src_list_ptr;
  }

  /* However, the iterators in any hash map would point to the wrong list! Build up that map from scratch.
   *
   * ...Actually, not quite.  To attempt to keep the same structure as the source map, first copy it; then
   * overwrite the values.  Not sure how perfectly it works but seems worth a shot, as a regular unordered_map<>
   * promises to copy over things like the load factor of the source object, not to mention the hasher and
   * equality predicate, and we advertise the same. */
  *m_keys_into_list_map = *src.m_keys_into_list_map;

  // So now replace the keys in the ready map.
  for (Value_list_iter value_list_it = m_value_list.begin(); value_list_it != m_value_list.end();
       ++value_list_it)
  {
    // Note this sets (at key K) the value: iterator to pair<K, V>.
    (*m_keys_into_list_map)[value_list_it->first] = value_list_it;
  }

  return *this;
} // Linked_hash_map::operator=()

template<typename Key, typename Mapped, typename Hash, typename Pred>
Linked_hash_map<Key, Mapped, Hash, Pred>&
  Linked_hash_map<Key, Mapped, Hash, Pred>::operator=(Linked_hash_map&& src)
{
  if (&src != this)
  {
    clear();
    swap(std::move(src));
  }
  return *this;
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
std::pair<typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator, bool>
  Linked_hash_map<Key, Mapped, Hash, Pred>::insert(Value const & key_and_mapped)
{
  using std::pair;

  Key const & key = key_and_mapped.first;

  // Check if this key is already in the map of iterators and therefore overall map.
  typename Key_to_value_iter_map::iterator const map_it = m_keys_into_list_map->find(key);
  if (map_it != m_keys_into_list_map->end())
  {
    // Yes.  *map_it is pair<Key, Iterator>.  So return 2nd half of that as the iterator to already existing element!
    return pair<Iterator, bool>(map_it->second, false);
  }
  // else Nope.

  return pair<Iterator, bool>(insert_impl(key_and_mapped), true);
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::insert_impl(Value const & key_and_mapped)
{
  /* Insert it at the front: as advertised, new element is "touched," meaning it is made "newest," so goes at start.
   * Note that "it" = a copy of key_and_mapped. */
  m_value_list.push_front(key_and_mapped);

  // Iterator to the new element is therefore iterator to start of list of <Key, Mapped> pairs.
  Iterator const new_elem_it = m_value_list.begin();
  // And make sure we can look it up in the future quickly (such as what is done above).
  (*m_keys_into_list_map)[key_and_mapped.first] = new_elem_it;

  return new_elem_it;
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::find(Key const & key)
{
  typename Key_to_value_iter_map::iterator const map_it = m_keys_into_list_map->find(key);
  return (map_it == m_keys_into_list_map->end()) ? m_value_list.end() : map_it->second;
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::find(Key const & key) const
{
  typename Key_to_value_iter_map::const_iterator const map_it = m_keys_into_list_map->find(key);
  return (map_it == m_keys_into_list_map->cend()) ? m_value_list.cend() : map_it->second;
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::size_type
  Linked_hash_map<Key, Mapped, Hash, Pred>::count(Key const & key) const
{
  return m_keys_into_list_map->count(key);
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
Mapped& Linked_hash_map<Key, Mapped, Hash, Pred>::operator[](Key const & key)
{
  using std::pair;

  // Check if this key is already in the map of iterators and therefore overall map.
  typename Key_to_value_iter_map::iterator const map_it = m_keys_into_list_map->find(key);
  if (map_it != m_keys_into_list_map->end())
  {
    // Yes.  *map_it is pair<Key, Iterator>.  *(that Iterator) is pair<Key, Value>.  Return 2nd half of latter.
    return map_it->second->second;
  }
  // else Nope.

  return insert_impl(Value(key, Mapped()))->second;
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Value&
  Linked_hash_map<Key, Mapped, Hash, Pred>::front()
{
  // No assert(): we promised not to crash even if empty().  They just can't access it subsequently if so.
  return *(newest());
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Value&
  Linked_hash_map<Key, Mapped, Hash, Pred>::back()
{
  // No assert(): we promised not to crash even if empty().  They just can't access it subsequently if so.
  return *(--past_oldest());
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Value const &
  Linked_hash_map<Key, Mapped, Hash, Pred>::const_front() const
{
  // No assert(): we promised not to crash even if empty().  They just can't access it subsequently if so.
  return *(const_newest());
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Value const &
  Linked_hash_map<Key, Mapped, Hash, Pred>::const_back() const
{
  // No assert(): we promised not to crash even if empty().  They just can't access it subsequently if so.
  return *(--const_past_oldest());
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
void Linked_hash_map<Key, Mapped, Hash, Pred>::touch(Const_iterator const & it)
{
  m_value_list.splice(m_value_list.begin(), m_value_list, it);
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
bool Linked_hash_map<Key, Mapped, Hash, Pred>::touch(Key const & key)
{
  const Const_iterator it = find(key);
  if (it == end())
  {
    return false;
  }
  // else

  touch(it);
  return true;
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::erase(Const_iterator const & it)
{
  m_keys_into_list_map->erase(m_keys_into_list_map->find(it->first));
  return m_value_list.erase(it);
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::erase(Const_iterator const & it_newest,
                                                  Const_iterator const & it_past_oldest)
{
  for (Value_list_const_iter it = it_newest; it != it_past_oldest; ++it)
  {
    m_keys_into_list_map->erase(it->first);
  }

  return m_value_list.erase(it_newest, it_past_oldest);
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::size_type
  Linked_hash_map<Key, Mapped, Hash, Pred>::erase(Key const & key)
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

template<typename Key, typename Mapped, typename Hash, typename Pred>
void Linked_hash_map<Key, Mapped, Hash, Pred>::clear()
{
  m_keys_into_list_map->clear();
  m_value_list.clear();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
void Linked_hash_map<Key, Mapped, Hash, Pred>::swap(Linked_hash_map& other)
{
  using std::swap;

  swap(m_keys_into_list_map, other.m_keys_into_list_map); // unique_ptr<>s exchanged (= raw pointers exchanged).
  swap(m_value_list, other.m_value_list); // list<> exchange (probably = head+tail pointer pairs exchanged).
  // Per cppreference.com `list<>::iterator`s (inside the `_maps`s) remain valid after list<>s swapped.
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::newest()
{
  return m_value_list.begin();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::begin()
{
  return newest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::past_oldest()
{
  return m_value_list.end();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::end()
{
  return past_oldest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::const_newest() const
{
  return m_value_list.cbegin();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::cbegin() const
{
  return const_newest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::begin() const
{
  return const_newest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::const_past_oldest() const
{
  return m_value_list.cend();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::cend() const
{
  return const_past_oldest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::end() const
{
  return const_past_oldest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Reverse_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::oldest()
{
  return m_value_list.rbegin();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Reverse_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::rbegin()
{
  return oldest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Reverse_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::past_newest()
{
  return m_value_list.rend();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Reverse_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::rend()
{
  return past_newest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_reverse_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::const_oldest() const
{
  return m_value_list.crbegin();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_reverse_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::crbegin() const
{
  return const_oldest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_reverse_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::const_past_newest() const
{
  return m_value_list.crend();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::Const_reverse_iterator
  Linked_hash_map<Key, Mapped, Hash, Pred>::crend() const
{
  return const_past_newest();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::size_type
  Linked_hash_map<Key, Mapped, Hash, Pred>::size() const
{
  return m_keys_into_list_map->size(); // I'm skeptical/terrified of list::size()'s time complexity.
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
bool Linked_hash_map<Key, Mapped, Hash, Pred>::empty() const
{
  return m_keys_into_list_map->empty();
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
typename Linked_hash_map<Key, Mapped, Hash, Pred>::size_type
  Linked_hash_map<Key, Mapped, Hash, Pred>::max_size() const
{
  return std::min(m_keys_into_list_map->max_size(), m_value_list.max_size());
}

template<typename Key, typename Mapped, typename Hash, typename Pred>
void swap(Linked_hash_map<Key, Mapped, Hash, Pred>& val1, Linked_hash_map<Key, Mapped, Hash, Pred>& val2)
{
  val1.swap(val2);
}

} // namespace flow::util
