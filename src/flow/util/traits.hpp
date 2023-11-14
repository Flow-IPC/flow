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
#include <boost/unordered/unordered_set.hpp>

namespace flow::util
{

/* Normally Container_traits would have all been forward-declared in a _fwd.hpp and defined here; instead it is
 * defined here, and that's it.  This is in accordance with the list of known reasonable exceptions to
 * the _fwd.hpp pattern; namely since Container_traits is a bunch of sizeof=0 types storing constants only. */

/**
 * Properties of various container types.  For example,
 * `"Container_traits<std::vector>::S_CHANGE_INVALIDATES_ITERATORS == true"`, while
 * `"Container_traits<std::list>::S_CHANGE_INVALIDATES_ITERATORS == false"`.
 * The template defines the interface, then the specializations for various container types define
 * the implementations.
 *
 * @tparam Container
 *         The container type being described.
 */
template<typename Container>
class Container_traits
{
public:
  // Constants.

  /**
   * If false, a change (erasure, addition of an element) in the `Container` will invalidate NO
   * iterator to that `Container`, previously obtained, except for potentially any iterator to the
   * element being erased (if applicable).
   */
  static constexpr bool S_CHANGE_INVALIDATES_ITERATORS = false;
  /// `true` if and only if iterating over the elements of a `Container` yields them in sorted order.
  static constexpr bool S_SORTED = false;
  /// `true` if and only if finding an element of `Container` by key takes at most constant amortized time.
  static constexpr bool S_CONSTANT_TIME_SEARCH = false;
  /// `true` if and only if adding an element to a `Container` (anywhere) takes at most constant amortized time.
  static constexpr bool S_CONSTANT_TIME_INSERT = false;
private:
  // Constructors/destructor.

  /// Forbid all instantion.
  Container_traits() = delete;
}; // class Container_traits<>

/**
 * Traits of `std::set`.
 *
 * @tparam T
 *         Type of set element.
 */
template<typename T>
class Container_traits<std::set<T>>
{
public:
  // Constants.

  /// See Container_traits.
  static constexpr bool S_CHANGE_INVALIDATES_ITERATORS = false;
  /// See Container_traits.
  static constexpr bool S_SORTED = true;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_SEARCH = false;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_INSERT = false;
private:
  // Constructors/destructor.

  /// Forbid all instantion.
  Container_traits() = delete;
}; // class Container_traits<std::set>

/**
 * Traits of `std::map`.
 *
 * @tparam K
 *         Type of map key element.
 * @tparam V
 *         Type of map value element.
 */
template<typename K, typename V>
class Container_traits<std::map<K, V>>
{
public:
  // Constants.

  /// See Container_traits.
  static constexpr bool S_CHANGE_INVALIDATES_ITERATORS = false;
  /// See Container_traits.
  static constexpr bool S_SORTED = true;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_SEARCH = false;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_INSERT = false;
private:
  // Constructors/destructor.

  /// Forbid all instantion.
  Container_traits() = delete;
}; // class Container_traits<std::map>

/**
 * Traits of `boost::unordered_set`.
 *
 * @tparam T
 *         Type of set element.
 */
template<typename T>
class Container_traits<boost::unordered_set<T>>
{
public:
  // Constants.

  /// See Container_traits.
  static constexpr bool S_CHANGE_INVALIDATES_ITERATORS = true;
  /// See Container_traits.
  static constexpr bool S_SORTED = false;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_SEARCH = true;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_INSERT = true;
private:
  // Constructors/destructor.

  /// Forbid all instantion.
  Container_traits() = delete;
}; // class Container_traits<boost::unordered_set>

/**
 * Traits of flow::util::Linked_hash_map.
 *
 * @tparam K
 *         Type of map key element.
 * @tparam V
 *         Type of map value element.
 */
template<typename K, typename V>
class Container_traits<util::Linked_hash_map<K, V>>
{
public:
  // Constants.

  /// See Container_traits.
  static constexpr bool S_CHANGE_INVALIDATES_ITERATORS = false;
  /// See Container_traits.
  static constexpr bool S_SORTED = false;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_SEARCH = true;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_INSERT = true;
private:
  // Constructors/destructor.

  /// Forbid all instantion.
  Container_traits() = delete;
}; // class Container_traits<util::Linked_hash_map>

/**
 * Traits of flow::util::Linked_hash_set.
 *
 * @tparam T
 *         Type of set element.
 * @tparam Hash
 *         Hasher.
 * @tparam Pred
 *         Equality predicate.
 */
template<typename T, typename Hash, typename Pred>
class Container_traits<util::Linked_hash_set<T, Hash, Pred>>
{
public:
  // Constants.

  /// See Container_traits.
  static constexpr bool S_CHANGE_INVALIDATES_ITERATORS = false;
  /// See Container_traits.
  static constexpr bool S_SORTED = false;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_SEARCH = true;
  /// See Container_traits.
  static constexpr bool S_CONSTANT_TIME_INSERT = true;
private:
  // Constructors/destructor.

  /// Forbid all instantion.
  Container_traits() = delete;
}; // class Container_traits<util::Linked_hash_set>

} // namespace flow::util
