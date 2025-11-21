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
 * An object of this class is a map that combines the lookup speed of an `unordered_map<>` and ordering and
 * iterator stability capabilities of a `list<>`.
 *
 * The API is generally that of an `unordered_map<>`.  The differences essentially all have to do with iterators.
 * This map introduces a concept of "newness," which determines the iteration order.  Moreover, *every* iterator remains
 * valid except (of course) under erasure of the underlying element.  Newness is defined as follows inductively:
 * whenever an element is inserted, it is "newest," thus it is placed at the front of the iterator order.  Furthermore,
 * the methods touch() can be used to make any element "newest" (moved to the front of the iterator order).  Iterator
 * thus formed orders elements from newest to oldest (hence newest() is begin(), past_oldest() is end()).
 *
 * Performance expectations: The best way to determine a method's time needs is to
 * imagine what it must do.  If it must perform a lookup by key, that is an `unordered_set<>` lookup resulting in an
 * (at least internal) iterator.  If it must insert an element, it is always inserted at the start of a `list`; and
 * also into an `unordered_set<>`.  If it must erase an element based on an iterator, that element is erased from a list
 * based on that iterator; and also by key from said `unordered_set<>`.  Iteration itself is iteration along a `list`.
 * But essentially, every operation is either near constant time or constant time.
 * In terms of space needs, this essentially stores the values themselves in a `list`; and also a pointer to each
 * list-held key/element in an `unordered_set<>`, which also stores a pointer or list iterator per element.
 *
 * Move semantics for both keys and mapped-values are supported (let `T` be a concrete type for a `*this` and `x`
 * a `*this`):
 *   - `x.insert(std::make_pair<Key, Mapped>(..., ...))`;
 *     - or `x.insert(T::Value_movable{..., ...})`;
 *   - `x[std::move(...)] = std::move(...)`.
 *
 * There is the standard complement of container-wide move operations: move-construction, move-assignment, and
 * `swap()` (all constant-time, excluding any implied `this->clear()` in the move-assignment).
 *
 * The iterators are, really, `list<pair<const Key, Mapped>>` iterators; and as such are not invalidated except
 * due to direct erasure of a given pointee.
 *
 * @todo Linked_hash_map and Linked_hash_set have a reasonable complement of C++1x-ish APIs including move-semantics;
 * but the API does not quite mirror the full complement of what is in existence for `unordered_*` counterparts in
 * C++17 STL/Boost -- it would be nice to add these.  This includes such things as `.emplace()` and `.try_emplace()`
 * but more fundamentally would probably involve trolling `std::unordered_*` and copying its ~full API (and likely
 * some of a decent impl too).  That said what's available already acquits itself reasonably well.  (Historically
 * this was first written before C++11 and hasn't been given the full-on C++1x overhaul but instead merely the
 * essentials thereof.)
 *
 * ### Thread safety ###
 * Same as for `unordered_map<>`.
 *
 * @internal
 * ### Impl notes ###
 * You should get much of what you need to grok this just by reading the above and possibly looking at the Data
 * section doc-headers under `private:`.  Essentially, to repeat/recap: there's the `list<pair<const Key, Mapped>>`
 * to store the actual values, in order (#m_value_list); #Iterator and #Const_iterator come directly from there.
 *
 * When lookup by #Key is needed, the `unordered_set` #m_value_iter_set comes into play.  This is arguably the
 * only real mechanical trickiness.  It actually stores `Iterator`s into #m_value_list but in such a way as to allow
 * seamless lookup from a mere `const Key&`; so `m_value_iter_set.find(key)` "magically" either finds `.end()` --
 * then the key is not in `*this` -- or the iterator into the actual key/mapped-value store in #m_value_list.
 * Using #m_value_iter_set is easy; but a bit of internal infrastructure is necessary to have it work.  Namely
 * we have support class template `Linked_hash_key<Key, Iterator>`, and the `unordered_set m_value_iter_set`
 * actually stores those guys, not raw #Key copies; so it is a wrapper around an `Iterator` *or* a #Key
 * (union-style).  #m_value_iter_set stores #Iterator wrappers, while lookup attempts use the #Key wrapper form
 * to pass into `m_value_iter_set.find()`.
 *
 * An earlier version of Linked_hash_map instead used a simple `unordered_map<Key, Iterator>` instead of the
 * `unordered_set<Linked_hash_key<Key, Iterator>>`; so a lookup by key was just that.
 * Eventually we replaced it with the more complex solution simply to avoid storing 2 copies of
 * each #Key (one in the list, one in the map); as the `Iterator` itself
 * includes a pointer to the thing containing the corresponding #Key in the first place.  So this saves memory
 * as well as various #Key copying.
 *
 * @endinternal
 *
 * @tparam Key_t
 *         Key type.  We omit formal requirements, as it is tedious and full of corner cases depending on what
 *         you plan to invoke (e.g., whether you use move-semantics for keys).  Please use common sense knowing
 *         the basic data structures involved as explained above.  That said: if #Key is of non-trivial size,
 *         it is good to have it have performant move-constructibility and move-assignability and then make use
 *         of it via move-aware APIs as suggested in the doc header above.
 * @tparam Mapped_t
 *         The 2nd (satellite) part of the #Value pair type.  Same commentary as for #Key applies here.
 * @tparam Hash_t
 *         Hasher type.  Same requirements and behavior as `boost::unordered_set<>` counterpart.  If using
 *         the default value for #Hash (`boost::hash<Key>`), and the default object is passed to ctor (`Hash{}`) (this
 *         is typical), but there is no hash-function already defined for #Key, then the easiest way to define
 *         it is: make a `size_t hash_value(Key)` free function in the same namespace as #Key.
 * @tparam Pred_t
 *         Equality-determiner type.  Same requirements and behavior as `boost::unordered_set<>` counterpart.  If using
 *         the default value for #Pred (`std::equal_to<Key>`), and the default object is passed to ctor (`Pred{}`)
 *         (this is typical), but there is no equality op defined for #Key, then the easiest way to define
 *         it is: make an operator-method or free function such that `k1 == k2` (where `k1` and `k2` are `Key`s)
 *         determines equality or lack thereof.
 */
template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
class Linked_hash_map
{
public:
  // Types.

  /// Convenience alias for template arg.
  using Key = Key_t;

  /// Convenience alias for template arg.
  using Mapped = Mapped_t;

  /// Convenience alias for template arg.
  using Hash = Hash_t;

  /// Convenience alias for template arg.
  using Pred = Pred_t;

  /// Short-hand for key/mapped-value pairs stored in the structure.
  using Value = std::pair<Key const, Mapped>;

  /// Short-hand for key/mapped-value pair best-suited (perf-wise) as arg type for the moving `insert()` overload.
  using Value_movable = std::pair<Key, Mapped>;

private:
  // Types.  These are here in the middle of public block due to inability to forward-declare aliases.

  /// Short-hand for doubly linked list of (#Key, #Mapped) pairs.
  using Value_list = std::list<Value>;

public:

  // Types (continued).

  /// Expresses sizes/lengths of relevant things.
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

  /// For container compliance (hence the irregular capitalization): #Key type.
  using key_type = Key;
  /// For container compliance (hence the irregular capitalization): #Mapped type.
  using mapped_type = Mapped;
  /// For container compliance (hence the irregular capitalization): #Key/#Mapped pair type.
  using value_type = Value;
  /// For container compliance (hence the irregular capitalization): #Hash type.
  using hasher = Hash;
  /// For container compliance (hence the irregular capitalization): #Pred type.
  using key_equal = Pred;
  /// For container compliance (hence the irregular capitalization): pointer to #Key/#Mapped pair type.
  using pointer = Value*;
  /// For container compliance (hence the irregular capitalization): pointer to `const Key`/#Mapped pair type.
  using const_pointer = const Value*;
  /// For container compliance (hence the irregular capitalization): reference to #Key/#Mapped pair type.
  using reference = Value&;
  /// For container compliance (hence the irregular capitalization): reference to `const Key`/#Mapped pair type.
  using const_reference = const Value&;
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
   *        whatever `unordered_set<>` would use by default.
   * @param hasher_obj
   *        Instance of the hash function type (`hasher_obj(k) -> size_t` should be hash of `Key k`).
   * @param pred
   *        Instance of the equality function type (`pred(k1, k2)` should return `true` if and
   *        only if the `Key`s are equal by value).
   */
  Linked_hash_map(size_type n_buckets = size_type(-1),
                  const Hash& hasher_obj = Hash{},
                  const Pred& pred = Pred{});

  /**
   * Constructs structure with some basic parameters, and values initialized from initializer list.
   * The values are inserted as if `insert(v)` was called for each pair `v` in `values`
   * **in reverse order**.  Since the canonical ordering places the *newest* (last inserted/`touch()`ed)
   * element at the *front* of the ordering, that means that forward iteration through the set (right after this
   * constructor runs) will yield values in the *same* order as in initializer list `values`.
   *
   * @param values
   *        Values with which to fill the structure after initializing it.
   *        Typically you'd provide a series of key/value pairs like this:
   *        `{ { key1, value1 }, { key2, value2 }, ... }`.  They will appear in iterated sequence in the same order as
   *        they appear in this list.
   * @param n_buckets
   *        See other constructor.
   * @param hasher_obj
   *        See other constructor.
   * @param pred
   *        See other constructor.
   */
  explicit Linked_hash_map(std::initializer_list<Value> values,
                           size_type n_buckets = size_type(-1),
                           const Hash& hasher_obj = Hash{},
                           const Pred& pred = Pred{});

  /**
   * Constructs object that is a copy of the given source.  Equivalent to default-ction followed by `operator=(src)`.
   *
   * @param src
   *        Source object.
   */
  Linked_hash_map(const Linked_hash_map& src);

  /**
   * Constructs object by making it equal to the given source, while the given source becomes as-if default-cted.
   * Equivalent to default-ction followed by `operator=(std::move(src))`.
   *
   * This is a constant-time operation.
   *
   * @param src
   *        Source object which is emptied.
   */
  Linked_hash_map(Linked_hash_map&& src);

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
  Linked_hash_map& operator=(const Linked_hash_map& src);

  /**
   * Overwrites this object making it identical to the given source, while the given source becomes as-if default-cted.
   *
   * This is a constant-time operation, plus whatever is the cost of `this->clear()` (linear in pre-op `.size()`).
   *
   * @param src
   *        Source object which is emptied; except no-op if `this == &src`.
   * @return `*this`.
   */
  Linked_hash_map& operator=(Linked_hash_map&& src);

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
  void swap(Linked_hash_map& other);

  /**
   * Attempts to insert (copying both key and mapped-value) the given key/mapped-value pair into the map; if key
   * already in `*this` makes no change.  See also the overload which can avoid a copy and destructively move
   * the key and mapped-value instead.
   *
   * Return value indicates various info of interest about what occurred or did not occur.
   * If inserted, the new element is considered "newest," as if by touch().  If not inserted, the existing element
   * location is not affected (use touch() upon consulting the return value, if this is desirable).
   *
   * @param key_and_mapped
   *        The key/mapped-value pair to attempt to insert.  A copy of this value is placed in `*this`.
   * @return A pair whose second element is true if and only if the insertion occurred; and whose first element
   *         is an iterator pointing to either the newly inserted element or already present one with a key equal to
   *         `key_and_mapped.first`.
   */
  std::pair<Iterator, bool> insert(const Value& key_and_mapped);

  /**
   * Identical to the other overload, except that (if key not already present in `*this`) the key and mapped-value
   * are moved, not copied, into `*this`.
   *
   * @note `key_and_mapped` pointee must be of type #Value_movable, a/k/a `pair<Key, Mapped>` -- not
   *       #Value, a/k/a `pair<const Key, Mapped>` -- otherwise the other insert() overload may get invoked,
   *       and copying may occur contrary to your intention.  E.g., use `std::make_pair<Key, Mapped>()` or
   *       `"decltype(*this)::Value_movable{}"`.
   *       (For a move to occur, the source-object can't be `const`; so that's why.)
   * @note You can often also use `x[std::move(key)] = std::move(value)`, particularly if you know `key` isn't in
   *       there, or you are OK with replacing the value if it is.  In those cases it's probably more convenient,
   *       no pairs or `Value_movable`s to worry oneself.
   *
   * @param key_and_mapped
   *        The key/mapped-value pair to attempt to insert (both key and mapped-value are moved-from, if insertion
   *        occurs).
   * @return See other overload.
   */
  std::pair<Iterator, bool> insert(Value_movable&& key_and_mapped);

  /**
   * Either finds the #Mapped value at the given key, or if not found inserts one with a default-constructed
   * `Mapped{}`; then returns reference to the #Mapped.  That ref can be used to read and/or modify that value
   * directly.  See also the overload which can avoid a copy and destructively move the key instead.
   *
   * If inserted, the new element is considered "newest," as if by touch().  If not inserted, the existing element
   * location is not affected.
   *
   * So it is ~equivalent to
   *
   *   - (`key` is in map) `return this->find(key)->second`; or
   *   - (otherwise) `return this->insert(key, Mapped{}).first->second`.
   *
   * As long as the value is not removed from the map, the reference will continue to be valid.
   *
   * Any subsequent writes to the referred-to area of memory will NOT have the effect of touch().  If you need it
   * call touch() yourself.
   *
   * @param key
   *        Key whose equal to find or insert if not found.  A copy of this value is placed in `*this`.
   * @return Reference to mutable #Mapped value directly inside the data structure.
   */
  Mapped& operator[](const Key& key);

  /**
   * Identical to the other overload, except that (if key not already present in `*this`) the key
   * is moved, not copied, into `*this`.
   *
   * @param key
   *        The key to attempt to insert (key is moved-from, if insertion occurs).
   * @return See other overload.
   */
  Mapped& operator[](Key&& key);

  /**
   * Attempts to find value at the given key in the map.  Key presence is determined identically to how it would be
   * done in an `unordered_set<Key, Hash, Pred>`, with the particular #Hash and #Pred instances given to ctor
   * (typically their default-cted instances, typically occupying no memory).
   *
   * The returned iterator (if valid) can be used to mutate the element inside the map; though only the #Mapped
   * is mutable; the `const Key` is immutable.
   *
   * Any subsequent writes to the referred-to (by returned iterator) area of memory will NOT have the effect of touch().
   * If you need it call touch() yourself.
   *
   * @param key
   *        Key whose equal to find.
   * @return If found, iterator to the key/mapped-value pair with the equivalent key; else `this->end()`.
   */
  Iterator find(const Key& key);

  /**
   * Identical to the other overload but in a `const` context: the returned iterator is to immutable memory.
   *
   * @param key
   *        Key whose equal to find.
   * @return If found, iterator to the key/mapped-value pair with the equivalent key; else `this->cend()`.
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
  Iterator erase(const Const_iterator& it);

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
  Iterator erase(const Const_iterator& it_newest, const Const_iterator& it_past_oldest);

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
  Iterator begin();

  /**
   * Returns first, a/k/a "newest," element's iterator; or past_oldest() if empty().
   * @return Ditto.
   */
  Iterator newest();

  /**
   * Synonym of past_oldest().  Exists as standard container method.
   * @return See past_oldest().
   */
  Iterator end();

  /**
   * Returns special iterator indicating the position just past the iteration order; if not empty() this is
   * one past last, a/k/a "oldest," element in the iteration order.
   *
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
   * Same as newest() but operating on immutable `*this`.
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
   * Same as past_oldest() but operating on immutable `*this`.
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
   * Returns special reverse iterator indicating the position just past the reverse-iteration order; if not empty()
   * this is one past last, a/k/a "newest," element in the reverse-iteration order.
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

  // Methods.

  /**
   * Helper that modifies #m_value_list and #m_value_iter_set so that `key_and_mapped`'s copy is inserted into
   * the structure.  Pre-condition is that `key_and_mapped.first` is not in the structure (else behavior undefined).
   *
   * @param key_and_mapped
   *        Same as in insert().
   * @return Same as in `insert().first`.
   */
  Iterator insert_impl(const Value& key_and_mapped);

  /**
   * Simimlar to insert_impl(), except `key_and_mapped` components are `move()`d into `*this` instead of being copied.
   *
   * @param key_and_mapped
   *        Same as in insert().
   * @return Same as in `insert().first`.
   */
  Iterator insert_impl_mv(Value_movable&& key_and_mapped);

  // Data.

  /**
   * The actual values -- which, as in `unordered_map<K, M>`, are instances of #Value = `pair<const Key, Mapped>` --
   * are stored in here, in the order in which user would iterate over them.  If `Value v` is in this list, then no
   * `Value v1 == v` can be elsewhere in the list.  The order is semantically defined to be from "newest" to "oldest."
   * Therefore, any newly inserted value goes at the *start* of the list.  Similarly, any "touched" value is moved to
   * the *start* of the list (see touch()).
   *
   * This ordering is what a normal `unordered_map<K, M>` would not supply (it's in the name!) but that we advertise.
   *
   * ### Design ###
   * This is very much the central structure in a `*this`; its iterator type *is* our exposed #Iterator.
   * Straight-up, #m_value_list supplies every single required operation (or at least ones on top of which any
   * required ops could be implemented).  There is exactly one exception to this: `find(const Key&)`.  It too
   * could be implemented with #m_value_list alone, but a linear search (linear-time worst- and average-case)
   * would be necessary (unacceptable).  Because of that we have #m_value_iter_set.  See that guy's doc header.
   *
   * ### Performance ###
   * Moving a value from anywhere to either end of the list is a constant-time operation
   * (assuming the source location's iterator is known).  Hence touch() is constant-time.  Moreover, touch()
   * does *not* involve a copy of a #Value (it only involves assigning, internally, a few linked list pointers).
   * Also note that insertion is similarly constant-time.  Finally, erasure is also constant-time.  These are the
   * basic operations needed.
   */
  Value_list m_value_list;

  /**
   * Data structure that allows the amortized-constant-time (as in `unordered_set`) implementation of
   * `this->find(key)`, where `key` is `const Key&`.  Namely, then, given a #Key, it gets us an #Iterator
   * into #m_value_list -- the central data store -- or a null iterator if not-found.
   *
   * ### Design ###
   * (There is quick intro in Impl section of the class doc header.)  Ignoring various technicalities and C++isms,
   * ultimately it stores `Iterator`s while supporting the **find** operation that
   *
   *   - takes a `const Key& key`; and
   *   - yields the #Iterator `it` stored therein (if any) such that
   *     - it->second *equals by value* (via #Hash and #Pred) the `key`.
   *
   * This find-op must #Hash the `key`; and then perform a (series of) #Pred comparisons between
   * `key` and the `Key`s stored at `Iterator`s within that hash-bucket.
   *
   * *How* this is accomplished is encapsulated inside #Linked_hash_key_set, Linked_hash_key, Linked_hash_key_hash,
   * and Linked_hash_key_pred helper (internally-used only) types.  This is abstracted away; the bottom line is
   * `m_value_iter_set.find(key)->iter()` yields the proper #Iterator (assuming `.find()` didn't yield `.end()`).
   *
   * Similarly `m_value_iter_set.insert(iter)` -- where `iter` is an #Iterator into #m_value_list -- just works.
   *
   * ### Performance ###
   * Anything they'll need to do to this set (namely `.find()` and `.insert()`) carries the same performance cost as
   * if they used a straight `unordered_map<>`, so by definition it is acceptable.
   */
  Linked_hash_key_set<Key, Iterator, Hash, Pred, true> m_value_iter_set;
}; // class Linked_hash_map

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Linked_hash_map(size_type n_buckets,
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

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Linked_hash_map(std::initializer_list<Value> values,
                                                                  size_type n_buckets,
                                                                  const Hash& hasher_obj,
                                                                  const Pred& pred) :
  // Their initializer_list is meant for a dictionary, but it is perfect for our list of pairs!
  m_value_list(values),
  m_value_iter_set((n_buckets == size_type(-1))
                     ? boost::unordered::detail::default_bucket_count // See @todo above.
                     : n_buckets,
                   hasher_obj, pred)
{
  // Now link each key in the quick-lookup table to its stored location in the ordering.
  const auto value_list_end_it = m_value_list.end();
  for (auto value_list_it = m_value_list.begin(); value_list_it != value_list_end_it; ++value_list_it)
  {
    // Note that value_list_it contains both the iterator (lookup result) and the lookup key (iterator pointee).
    m_value_iter_set.insert(value_list_it);
  }
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Linked_hash_map(const Linked_hash_map& src)
  // An empty m_value_iter_set is constructed here but immediately replaced within the {body}.
{
  operator=(src);
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Linked_hash_map(Linked_hash_map&& src)
  // An empty m_value_iter_set is constructed here but immediately replaced within the {body}.
{
  operator=(std::move(src));
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>&
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::operator=(const Linked_hash_map& src)
{
  using Value_iter_set = decltype(m_value_iter_set);

  if (&src == this)
  {
    return *this;
  }
  // else

  /* Values are values -- copy them over.  Recall these are (const Key, Mapped) pairs.
   *
   * Why not just: `m_value_list = src.m_value_list;`?  Answer: It fails to build, at least with a
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
   * Then we can assign.  Note that, even though it's N lines of code, reinterpret_cast<> generates no machine
   * code: it just makes the code-that-would-have-been-generated-anyway look at the memory in a different way
   * (in this case, as storing Keys that can be overwritten instead of read-only ones).  Finally, note that
   * we specifically require that the template parameter `typename Key` be Assignable; that is the piece of the
   * puzzle that GUARANTEES this reinterpret_cast<> (in general not a safe operation) is indeed safe/correct. */
  {
    using Mutable_key_value_list = std::list<Value_movable>;

    *(reinterpret_cast<Mutable_key_value_list*>(&m_value_list))
      = *(reinterpret_cast<const Mutable_key_value_list*>(&src.m_value_list));
  }

  /* However, the iterators in any hash set would point into the wrong list!  Build up that set from scratch.
   * Do try to keep the same structure, as we advertise. */
  const auto& src_value_iter_set = src.m_value_iter_set;
  m_value_iter_set = Value_iter_set{src_value_iter_set.bucket_count(),
                                    src_value_iter_set.hash_function(),
                                    src_value_iter_set.key_eq()};

  const auto value_list_end_it = m_value_list.end();
  for (auto value_list_it = m_value_list.begin(); value_list_it != value_list_end_it; ++value_list_it)
  {
    // Note that value_list_it contains both the iterator (lookup result) and the lookup key (iterator pointee).
    m_value_iter_set.insert(value_list_it);
  }

  return *this;
} // Linked_hash_map::operator=()

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>&
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::operator=(Linked_hash_map&& src)
{
  if (&src != this)
  {
    clear();
    swap(src);
  }
  return *this;
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
void Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::swap(Linked_hash_map& other)
{
  using std::swap;

  swap(m_value_iter_set, other.m_value_iter_set); // unordered_set<> exchange; constant-time for sure at least.
  swap(m_value_list, other.m_value_list); // list<> exchange (probably ~= head+tail pointer pairs exchanged).
  // Per cppreference.com `list<>::iterator`s (inside the `_maps`s) remain valid after list<>s swapped.
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
std::pair<typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator, bool>
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::insert(const Value& key_and_mapped)
{
  using std::pair;

  const auto set_it = m_value_iter_set.find(key_and_mapped.first);
  return (set_it == m_value_iter_set.end())
           ? pair<Iterator, bool>{insert_impl(key_and_mapped), // Key and Mapped copy occurs here.
                                  true}
           : pair<Iterator, bool>{set_it->iter(), false}; // *set_it is Linked_hash_key.
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
std::pair<typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator, bool>
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::insert(Value_movable&& key_and_mapped)
{
  using std::pair;

  const auto set_it = m_value_iter_set.find(key_and_mapped.first);
  return (set_it == m_value_iter_set.end())
           ? pair<Iterator, bool>{insert_impl_mv(std::move(key_and_mapped)), // <-- The difference from other overload.
                                  true}
           : pair<Iterator, bool>{set_it->iter(), false}; // *set_it is Linked_hash_key.
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
Mapped_t& Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::operator[](const Key& key)
{
  const auto set_it = m_value_iter_set.find(key);
  return ((set_it == m_value_iter_set.end())
            ? insert_impl_mv // Returns Iterator.  *(Iterator) is pair<const Key, Mapped>.
                (Value_movable{Key{key}, Mapped{}}) // Have to copy `key`, but empty temporary Mapped is moved.
            : set_it->iter()) // *set_it is Linked_hash_key.  *(that.iter()) is pair<const Key, Mapped>.
         ->second;
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
Mapped_t& Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::operator[](Key&& key)
{
  const auto set_it = m_value_iter_set.find(key);
  return ((set_it == m_value_iter_set.end())
              // v-- The difference from other overload.
            ? insert_impl_mv // Returns Iterator.  *(Iterator) is pair<const Key, Mapped>.
                (Value_movable{std::move(key), Mapped{}})
            : set_it->iter()) // *set_it is Linked_hash_key.  *(that.iter()) is pair<const Key, Mapped>.
         ->second;
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::insert_impl(const Value& key_and_mapped)
{
  /* Insert it at the front: as advertised, new element is "touched," meaning it is made "newest," so goes at start.
   * Note that "it" = a copy of key_and_mapped; this invokes pair<> copy ctor, as emplace_front() forwards to it. */
  m_value_list.emplace_front(key_and_mapped);

  /* Iterator to the new element is therefore iterator to start of list of <Key, Mapped> pairs.
   * And make sure we can look it up in the future quickly (such as what is done first in insert()).
   * Linked_hash_key_set m_value_iter_set achieves these aims black-boxily. */
  const auto list_it = m_value_list.begin();
  m_value_iter_set.insert(list_it);
  return list_it;
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::insert_impl_mv(Value_movable&& key_and_mapped)
{
  /* Same as insert_impl() but construct value in-place inside the list<> as-if:
   *   pair<const Key, Mapped> p{move(k_a_m.first), move(k_a_m.second)}. */
  m_value_list.emplace_front(std::move(key_and_mapped));

  const auto list_it = m_value_list.begin();
  m_value_iter_set.insert(list_it);
  return list_it;
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::find(const Key& key)
{
  const auto set_it = m_value_iter_set.find(key);
  return (set_it == m_value_iter_set.end()) ? m_value_list.end() : set_it->iter();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::find(const Key& key) const
{
  const auto set_it = m_value_iter_set.find(key);
  return (set_it == m_value_iter_set.cend()) ? m_value_list.cend() : set_it->iter();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::size_type
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::count(const Key& key) const
{
  return m_value_iter_set.count(key);
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
void Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::touch(const Const_iterator& it)
{
  m_value_list.splice(m_value_list.begin(), m_value_list, it);
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
bool Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::touch(const Key& key)
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

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::erase(const Const_iterator& it)
{
  m_value_iter_set.erase(it->first);
  // (^-- Subtlety: .erase(it) won't build due to Const_ness of `it`.)

  return m_value_list.erase(it);
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::erase(const Const_iterator& it_newest,
                                                  const Const_iterator& it_past_oldest)
{
  for (auto it = it_newest; it != it_past_oldest; ++it)
  {
    m_value_iter_set.erase(it->first);
  }

  return m_value_list.erase(it_newest, it_past_oldest);
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::size_type
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::erase(const Key& key)
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

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
void Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::clear()
{
  m_value_iter_set.clear();
  m_value_list.clear();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::newest()
{
  return m_value_list.begin();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::begin()
{
  return newest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::past_oldest()
{
  return m_value_list.end();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::end()
{
  return past_oldest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::const_newest() const
{
  return m_value_list.cbegin();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::cbegin() const
{
  return const_newest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::begin() const
{
  return const_newest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::const_past_oldest() const
{
  return m_value_list.cend();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::cend() const
{
  return const_past_oldest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::end() const
{
  return const_past_oldest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Reverse_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::oldest()
{
  return m_value_list.rbegin();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Reverse_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::rbegin()
{
  return oldest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Reverse_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::past_newest()
{
  return m_value_list.rend();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Reverse_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::rend()
{
  return past_newest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_reverse_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::const_oldest() const
{
  return m_value_list.crbegin();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_reverse_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::crbegin() const
{
  return const_oldest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_reverse_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::const_past_newest() const
{
  return m_value_list.crend();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::Const_reverse_iterator
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::crend() const
{
  return const_past_newest();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::size_type
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::size() const
{
  return m_value_iter_set.size(); // I'm skeptical/terrified of list::size()'s time complexity.
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
bool Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::empty() const
{
  return m_value_list.empty();
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
typename Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::size_type
  Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>::max_size() const
{
  return std::min(m_value_iter_set.max_size(), m_value_list.max_size());
}

template<typename Key_t, typename Mapped_t, typename Hash_t, typename Pred_t>
void swap(Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>& val1,
          Linked_hash_map<Key_t, Mapped_t, Hash_t, Pred_t>& val2)
{
  val1.swap(val2);
}

} // namespace flow::util
