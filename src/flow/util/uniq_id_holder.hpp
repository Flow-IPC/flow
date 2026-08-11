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
#include <atomic>
#include <type_traits>

namespace flow::util
{

/**
 * Each object of this class stores (at construction) and returns (on demand) a numeric ID unique from all other
 * objects of the same class ever constructed, across all time from program start to end.  To be clear, the uniqueness
 * is not just for all existing objects at a given point in time (for which `this` could simply be used instead) but
 * across all time.
 *
 * ### Copying behavior ###
 * For obvious reasons, Unique_id_holder cannot have standard copy construction and assignment semantics.
 * It was contemplated to simply make class noncopyable (forbidding copy construction).
 * However, then no derived type could use auto-generated copy construction and/or `operator=()`, making the
 * deriving usage pattern (see below) considerably less concise for such types.
 *
 * Therefore, it defines a (somewhat unusual) copy constructor: It is simply equal to the default constructor and simply
 * generates a unique ID.  The reason: a common pattern (see below) will involve type `C` deriving from
 * Unique_id_holder.  If `C` happens to use the auto-generated copy constructor implementation, Unique_id_holder's
 * copy constructor will be invoked by this implementation.  Since equal (by value) objects are still separate objects,
 * the correct behavior is for the constructed object to gain a new ID.
 *
 * The assignment operator operator=() similarly has unusual behavior: it does nothing, for similar reasons.
 *
 * ### Thread safety, performance ###
 * All operations safe for simultaneous execution on 2+ separate objects or on the same object.
 * The ID accessor is a totally trivial accessor.  Only construction requires any concurrent-access protection
 * and internally uses an `atomic` for good efficiency.
 *
 * ### Usage patterns ###
 * This can be used, in particular, to uniquely identify (over all time) objects of any type (across all types).
 * The most concise usage pattern is probably via public (or possibly private) inheritance.  Another is simply via
 * composition (storage).
 *
 *   ~~~
 *   void Widget_store::store_in_table(Widget* obj, const Unique_id_holder& obj_id_holder)
 *   {
 *     m_unordered_map[obj_id_holder.unique_id()] = obj;
 *   }
 *
 *   // Usage pattern 1: Inheritance.
 *   class Widget : ..., public Unique_id_holder // Can change to private inheritance if we always register internally.
 *   {
 *   private:
 *     // Example of an object registering itself.
 *     void some_private_method()
 *     {
 *       ...
 *       s_widget_store.store_in_table(this, *this); // `*this` is itself a Unique_id_holder.
 *       ...
 *     }
 *     static Widget_store s_widget_store;
 *   };
 *   ...
 *     // Example of outside code registering an object.
 *     Widget_store widget_store;
 *     Widget widget;
 *     widget_store::store_in_table(&widget, widget); // `widget` is itself a Unique_id_holder.
 *   ...
 *
 *   // Usage pattern 2: Composition.
 *   class Widget : ...
 *   {
 *   private:
 *     // Example of an object registering itself.
 *     void some_private_method()
 *     {
 *       ...
 *       s_widget_store.store_in_table(this, m_uniq_id_holder);
 *       ...
 *     }
 *     Unique_id_holder m_uniq_id_holder;
 *     static Widget_store s_widget_store;
 *   };
 *   ~~~
 *
 * Update: A simplified pattern is now possible: If you just want the raw unique number itself and don't even
 * want to keep Unique_id_holder objects around or refer to them, call the static create_unique_id() which returns
 * a new raw `id_t` integer.  The above 2 usage patterns can be rewritten to pass around `id_t` values.
 * The "Example of outside code registering an object" sub-case is the one exception to that, as for that
 * either `store_in_table()` must continue to take a Unique_id_holder and not a raw integer, or I suppose
 * `m_uniq_id_holder` must be made public which some would not like.
 */
class Unique_id_holder
{
public:
  // Types.

  /// Raw integer type to uniquely identify a thing.  64-bit width should make overflow extremely hard to reach.
  using id_t = uint64_t;

  // Constructors/destructor.

  /// Thread-safely construct an ID whose value is different from any other object of this class, past or future.
  explicit Unique_id_holder();

  /**
   * This copy constructor is identical in behavior to Unique_id_holder(), the default ctor.  Unique_id_holder doc
   * header explains this behavior and why copy construction is not disallowed entirely.
   */
  explicit Unique_id_holder(const Unique_id_holder&);

  // Methods.

  /**
   * Raw unique ID identifying this object as well as any object of a derived type.
   *
   * @return Numeric ID.  Can be used as a key for most classic associative containers without extra code.
   */
  id_t unique_id() const;

  /**
   * This assignment operator is a `const` no-op.  Unique_id_holder doc
   * header explains this behavior, and why assignment is not disallowed entirely.
   *
   * @return `*this`.
   */
  const Unique_id_holder& operator=(const Unique_id_holder&) const;

  /**
   * Short-hand for `Unique_id_holder().unique_id()`; useful when all you want is the unique integer itself.
   *
   * @return See unique_id().
   */
  static id_t create_unique_id();

private:

  // Data.

  /// The ID.  Note its `const`ness alone forbids a classic assignment operator.
  const id_t m_id;
}; // class Unique_id_holder

/**
 * An integer-like, type-safe wrapper for the *thread-token* concept -- see this_thread_unique_token(), which
 * explains the whole situation and returns one of these; we are merely the type-safe type usable for it.
 * A default-constructed Thread_token is "none": it equals no thread's token, ever -- so a data member of this
 * type needs no explicit not-yet-set sentinel value or initializer.
 *
 * @see this_thread_unique_token() that returns a unique (across all threads in this process) Thread_token.
 *
 * It converts to #id_t implicitly, so comparisons and the like work as for an integer; while the reverse
 * requires a deliberate `Thread_token{...}` (type-safety being the whole point).  `ostream<<`
 * prints it in fixed-width hex (see that operator's doc header): a token is an identity, not a quantity.
 * `Thread_token`s are usable as associate keys, whether ordered or unordered.
 *
 * @internal
 *
 * ### Impl notes ###
 * Stylistically there is some inconsistency between Unique_id_holder and Thread_token; for example
 * Unique_id_holder::create_unique_id() is a `static` member that returns raw values alongside a default-ctor
 * that creates a Unique_id_holder -- while Thread_token default-ctor returns a sentinel, and free function
 * this_thread_unique_token() returns a Thread_token.  The two are slightly related too.  The inconsistency
 * is neither criminal nor ideal; and the reason for the inconsistency is that Unique_id_holder was written so
 * long ago; the author (ygoldfel) has developed their style since then.  That said Unique_id_holder is fine
 * too; it probably could be simplified a bit, but it works fine.  We are not marking a to-do here, as it would
 * create unnecessary breaking-code situations, but this all was worth writing down for posterity.
 */
struct Thread_token
{
  // Types.

  /// Short-hand for the underlying integer type.
  using id_t = Unique_id_holder::id_t;

  // Data.

  /// The underlying integer value; `0` <=> "none."  Typically it is better to assign to `*this` rather than to #m_val.
  id_t m_val = 0;

  // Methods.

  /**
   * Returns #m_val; so comparisons and the like work as for an integer.
   * @return See above.
   */
  constexpr operator id_t() const;
}; // struct Thread_token
static_assert(std::is_trivially_copyable_v<Thread_token> && std::is_trivially_destructible_v<Thread_token>,
              "Thread_token must remain trivially copyable (at least to store it in raw memory as, say, a prefix) and "
                "destructible (at least so that any C++ code -- even in a thread_local dtor during TL-deinit "
                "at thread end -- can safely access a thread_local Thread_token).");

// Free functions: in *_fwd.hpp.

// constexpr implementations.

constexpr Thread_token::operator id_t() const
{
  return m_val;
}


} // namespace flow::util
