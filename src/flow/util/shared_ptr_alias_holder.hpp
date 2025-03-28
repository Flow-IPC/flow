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

namespace flow::util
{

// Classes.

/**
 * Convenience class template that endows the given subclass `T` with nested aliases `Ptr` and `Const_ptr`
 * aliased to `shared_ptr<T>` and `shared_ptr<const T>` respectively.  In addition a few syntactic-sugary `static`
 * methods are provided, in particular to perform shared pointer versions of `static_cast<T*>` and
 * `dynamic_cast<T*>`.
 *
 * ### When to use ###
 * The following assumes you are familiar with `shared_ptr` (`boost::` and `std::` are basically equivalent).
 * Mechanically speaking, if class `C` subclasses Shared_ptr_alias_holder, then, simply, you gain a shorter way
 * to write `boost::shared_ptr<C>` and `boost::shared_ptr<const C>`; plus briefer ways to cast down-class
 * polymorphic `shared_ptr`s to those types (e.g., if `C2` derives from `C`, and one has a `shared_ptr<C2>` but
 * needs a `shared_ptr<C>`).  It is prettier and more pleasant to write `C::Ptr`, `C::Const_ptr`, `C::ptr_cast(p)`,
 * etc., which may not seem like much, but in our experience it adds up over time.  (Some coders aren't familiar with
 * `{boost|std}::{static|dynamic}_pointer_cast()`, for example, so it's nice to provide equivalents "out of the box.")
 *
 * However, in addition to these mechanical niceties, subclassing Shared_ptr_alias_holder indicates the
 * intention that the inheriting class `C` is to be used with the *shared ownership pattern*: essentially,
 * that:
 *   -# `C`s are passed around by pointer, not by value;
 *   -# a `C` is generated via the factory pattern only and returned pre-wrapped in a `shared_ptr`; and
 *   -# a `C` is to be destroyed automatically once all outside *and* inside (to `C` code itself) references to that `C`
 *      have disappeared.
 *
 * It may be tempting to derive from Shared_ptr_alias_holder for every class ever: who doesn't like syntactic sugar?
 * We recommend restraint, however.  We recommend you do so if and only if `C` is indeed intended to be used per
 * the above 3 bullet points (shared ownership pattern).
 *
 * For example, net_flow::Node is just a (rather big) class,
 * intended to be used in boring ways, such as on the stack; and so it lacks any `Node::Ptr`.  However, it stores
 * (internally) tables of net_flow::Peer_socket objects, and it also returns them for public use
 * via factory methods such as `Node::connect() -> Peer_socket::Ptr`.  A net_flow::Peer_socket is never "out in the
 * wild" without a wrapping `shared_ptr`; hence it -- unlike `net_flow::Node` -- derives from Shared_ptr_alias_holder.
 * Once both the `Node` user has finishes with a given `Peer_socket`, and the `Node` has also deleted it from its
 * internal structures, the `Peer_socket` is deleted automatically.
 *
 * In particular, the *mere* fact that you want to use *a* `shared_ptr<>` with some class `C` -- and there are many
 * such situations -- is not sufficient to endow `C` with a Shared_ptr_alias_holder parent.  It is more intended for
 * when you want to use `C` with *only* `shared_ptr`s and in no other fashion.  `shared_ptr` is still useful in
 * a less expansive role, without Shared_ptr_alias_holder or the pattern the latter encourages.
 *
 * ### How to use ###
 * `T` would typically `public`ly subclass `Shared_ptr_alias_holder<shared_ptr<T>, shared_ptr<T const>>`,
 * where `shared_ptr` is either `boost::shared_ptr` or `std::shared_ptr` (or some other semantically identical
 * ref-counting pointer class template).  It's also reasonable to use `protected` or `private` inheritance.  Behavior is
 * undefined in any other use case.
 *
 * It is not recommended to have `C2` derive from us, if `C` already derives from us, and `C2` is a subclass of `C`
 * (directly or indirectly).  At that point `C2::Ptr` might refer to either alias, so compilation will likely fail
 * when trying to use it, and browbeating it into working -- even if achieved -- is unlikely to result in syntactically
 * pleasant code... which was the whole point in the first place.  So, don't do that.  If you need this stuff
 * at multiple levels of a hierarchy, then it's probably best to just define the nested `Ptr` (etc.) manually.
 * (It really isn't that hard in the first place... but reduced boiler-plate is reduced boiler-plate.)
 *
 * Also, if your `T` requires being an aggregate type -- such as if you intend to use `{}` direct member init --
 * then you can't derive from anything, including us, and will have to make a manual `Ptr` (etc.).  Note that's true
 * of other similar utilities like `boost::noncopyable`.
 *
 * ### Rationale ###
 * Why have this at all?  Answer: It was tiring to keep manually making `using Ptr = shared_ptr<X>` nested aliases.
 * Casting to `Ptr` in pretty fashion was also annoying to keep reimplementing.  Down with boiler-plate!
 *
 * Why not take a single template arg, the target type `T`?  Answer: That would've been fine, but it seemed nice to
 * be able to support not just specifically one of `"{std|boost|...}::shared_ptr"` but all of them.  Admittedly it's a
 * bit uglier when declaring its use in the `class` or `struct` declaration; but since that's a one-time thing,
 * trade-off seems worth it.
 *
 * @tparam Target_ptr
 *         Supposing the intended subclass is `T`, this must be `shared_ptr<T>`, where `shared_ptr` is a class template
 *         with `std::shared_ptr` semantics; namely at least `std::shared_ptr` itself and `boost::shared_ptr`.
 * @tparam Const_target_ptr
 *         Same as `Target_ptr` but pointing to immutable object: `shared_ptr<T const>`.  Note behavior is undefined
 *         if the `T` in `Const_target_ptr` isn't the same `T` as in `Target_ptr`; or if
 *         the `shared_ptr` template is different between the two (e.g., one uses `std::`, the other `boost::`).
 *         Update: It is now recommended to leave this at its default.  Had the authors originally known about
 *         `pointer_traits::rebind` this would never have been a parameter in the first place.  We are leaving it
 *         in for compatibility with existing code.
 *
 * @todo flow::util::Shared_ptr_alias_holder `Const_target_ptr` is deprecated and shall be always left at its default
 *       value in future code; eventually remove it entirely and hard-code the default value internally.
 *
 * @todo flow::util::Shared_ptr_alias_holder, such as it is, may well largely work for `unique_ptr` and other
 * smart/fancy pointer types; should be generalized both in name (if possible) and capabilities (if not already
 * the case).  Could be just a matter of updating docs and renaming (deprecated-path alias may be required to
 * avoid a breaking change); or not.  Needs a bit of investigation to determine such details.  (The author (ygoldfel)
 * was conisderably less advanced when this was originally made versus the time of this writing... maybe a
 * decade+?  He meant well though.)
 *
 * @todo Add example usage snippets in Shared_ptr_alias_holder doc header, illustrating the pattern.
 */
template<typename Target_ptr,
         typename Const_target_ptr> // Const_target_ptr is defaulted in _fwd.hpp.
class Shared_ptr_alias_holder
{
public:
  // Types.

  /// Short-hand for ref-counted pointer to mutable values of type `Target_type::element_type` (a-la `T*`).
  using Ptr = Target_ptr;
  /// Short-hand for ref-counted pointer to immutable values of type `Target_type::element_type` (a-la `T const *`).
  using Const_ptr = Const_target_ptr;

  // Methods.

  /**
   * Provides syntactic-sugary way to perform a `static_pointer_cast<>` from a compatible smart pointer type `From_ptr`,
   * typically `From_ptr::element_type` being in the same class hierarchy as `Target_ptr::element_type`.
   *
   * @tparam From_ptr
   *         See type of arg to `std::static_pointer_cast<>()` or `boost::static_pointer_cast<>()`.
   * @param ptr_to_cast
   *        The smart pointer to cast.
   * @return Result of the cast.
   */
  template<typename From_ptr>
  static Ptr ptr_cast(const From_ptr& ptr_to_cast);

  /**
   * Identical to ptr_cast() but adds `const`-ness (immutability) to the pointed-to type.  So if ptr_cast()
   * casts from `A*` to `B*`, this casts from `A*` to `B const *` (a/k/a `const B*`).
   *
   * @tparam From_ptr
   *         See ptr_cast().
   * @param ptr_to_cast
   *        The smart pointer to cast.
   * @return Result of the cast.
   */
  template<typename From_ptr>
  static Const_ptr const_ptr_cast(const From_ptr& ptr_to_cast);

  /**
   * Equivalent to ptr_cast() but a `dynamic_pointer_cast` instead of static.
   *
   * @tparam From_ptr
   *         See ptr_cast().
   * @param ptr_to_cast
   *        The smart pointer to cast.
   * @return Result of the cast.  Recall this may be an empty (null) pointer.
   */
  template<typename From_ptr>
  static Ptr dynamic_ptr_cast(const From_ptr& ptr_to_cast);

  /**
   * Identical to const_ptr_cast() but a `dynamic_pointer_cast` instead of static.
   *
   * @tparam From_ptr
   *         See ptr_cast().
   * @param ptr_to_cast
   *        The smart pointer to cast.
   * @return Result of the cast.  Recall this may be an empty (null) pointer.
   */
  template<typename From_ptr>
  static Const_ptr dynamic_const_ptr_cast(const From_ptr& ptr_to_cast);
}; // class Shared_ptr_alias_holder

// Template implementations.

template<typename Target_ptr, typename Const_target_ptr>
template<typename From_ptr>
typename Shared_ptr_alias_holder<Target_ptr, Const_target_ptr>::Ptr
  Shared_ptr_alias_holder<Target_ptr, Const_target_ptr>::ptr_cast(const From_ptr& ptr_to_cast) // Static.
{
  // This was taken, conceptually, from the `{static|dynamic|...}_pointer_cast` page of cppreference.com.
  auto const raw_ptr_post_cast = static_cast<typename Target_ptr::element_type*>(ptr_to_cast.get());
  return Target_ptr(ptr_to_cast, raw_ptr_post_cast);
}

template<typename Target_ptr, typename Const_target_ptr>
template<typename From_ptr>
typename Shared_ptr_alias_holder<Target_ptr, Const_target_ptr>::Const_ptr
  Shared_ptr_alias_holder<Target_ptr, Const_target_ptr>::const_ptr_cast(const From_ptr& ptr_to_cast) // Static.
{
  // This was taken, conceptually, from the `{static|dynamic|...}_pointer_cast` page of cppreference.com.
  auto const raw_ptr_post_cast = static_cast<typename Target_ptr::element_type const *>(ptr_to_cast.get());
  return Const_target_ptr(ptr_to_cast, raw_ptr_post_cast);
}

template<typename Target_ptr, typename Const_target_ptr>
template<typename From_ptr>
typename Shared_ptr_alias_holder<Target_ptr, Const_target_ptr>::Ptr
  Shared_ptr_alias_holder<Target_ptr, Const_target_ptr>::dynamic_ptr_cast(const From_ptr& ptr_to_cast) // Static.
{
  // This was taken, conceptually, from the `{static|dynamic|...}_pointer_cast` page of cppreference.com.
  auto const raw_ptr_post_cast = dynamic_cast<typename Target_ptr::element_type*>(ptr_to_cast.get());
  return raw_ptr_post_cast ? Target_ptr(ptr_to_cast, raw_ptr_post_cast)
                           : Target_ptr();
}

template<typename Target_ptr, typename Const_target_ptr>
template<typename From_ptr>
typename Shared_ptr_alias_holder<Target_ptr, Const_target_ptr>::Const_ptr
  Shared_ptr_alias_holder<Target_ptr, Const_target_ptr>::dynamic_const_ptr_cast(const From_ptr& ptr_to_cast) // Static.
{
  // This was taken, conceptually, from the `{static|dynamic|...}_pointer_cast` page of cppreference.com.
  auto const raw_ptr_post_cast = dynamic_cast<typename Target_ptr::element_type const *>(ptr_to_cast.get());
  return raw_ptr_post_cast ? Const_target_ptr(ptr_to_cast, raw_ptr_post_cast)
                           : Const_target_ptr();
}

} // namespace flow::util
