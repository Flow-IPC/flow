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
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace flow::util
{

/**
 * Allocator adaptor (useful for, e.g., `vector` that skips zero-filling) that turns a value-initialization
 * `T()` into a default-initialization; this matters for those types (including all PoDs) for which the two differ.
 *
 * ### Rationale (why) ###
 * Suppose you have `vector<uint8_t> v(10);`.  This will create a buffer and fill with at least 10 zeroes.
 * What if you want it *not* to do this?  Maybe you don't want it because performance.
 * Actually, consider an even sneakier situation that involves correctness, not just perf.  Suppose you
 * have `vector<uint8_t> v` with `.capacity() == 10` and `.size() == 0`, but you're letting some 3rd-party code
 * operate in range `[v.begin(), v.begin() + v.capacity())`, and you're using `.size()` as a store of the result
 * of the 3rd-party operation.  Say the 3rd party (e.g., a builder library) performs its op (serialization)
 * and reports that it has used exactly 7 bytes.  Now you call `v.resize(7)`.  The problem: this will fill the first
 * 7 bytes of the range with zeroes, wiping out the builder's work.  Of course you could have solved this
 * by keeping `.capacity() == .size() == 10` at all times and marking down the serialization's size in a separate
 * variable.  That's a bit sad though: you have a perfectly nice potential holder of the size, the `vector` inner
 * `m_size` (or whatever it is called), but you cannot use it due to the value-initializing behavior of `vector`.
 *
 * The link
 *   https://stackoverflow.com/questions/21028299/is-this-behavior-of-vectorresizesize-type-n-under-c11-and-boost-container/21028912#21028912
 * contains a nice solution in the form of an allocator adaptor which can be set as the `Allocator` template param
 * of a `vector` (etc.), adapting the normal allocator used; whether that's the typical heap-allocating `std::allocator`
 * or something more exotic (e.g., SHM-allocating `ipc::shm::stl::Stateless_allocator` from the sister project
 * Flow-IPC).  cppreference.com `vector::resize()` page even links to that in "Notes" about this problem!
 *
 * ### What it does (what/how) ###
 * The allocator works as follows: Suppose the STL-compliant container, having raw-allocated a buffer `sizeof(T)`
 * bytes long, at `void* p`, requests to in-place-construct a `T{}` (a/k/a `T()`) at `p`.  That is what, e.g.,
 * `vector::resize(size_t)` does: it *value-initializes* the `T` at address `p`.  Now there are two possibilities:
 *   - `T` has no user-provided default constructor (e.g., it is a PoD (plain old data-type) or an aggregate
 *     like `struct { std::string m_s; int m_i; }`): value-initialization `T t{};` *does not equal*
 *     default-initialization `T t;`.  The latter leaves pre-existing (possibly garbage) values in `t` (or the
 *     relevant parts of it).  The former first zero-initializes `t`; e.g., if `T` is `int` then it zeroes `t`.
 *   - `T` has a user-provided default constructor: value-initialization `T t{}` *does* equal default-initialization
 *     `T t`.  They will both default-construct `T` via `T::T()`.
 *
 * The allocator adaptor changes the *former* bullet point to act like the *latter* bullet point;
 * while making no changes for the latter `T`s.  For the former `T`s:
 * The container will ask to do `T t{}` (a/k/a `T t = T()`), but what will happen in reality is `T t;` instead.
 * Or, in plainer English, it's gonna make it so that `int`s and `float`s and simple `struct`s and stuff
 * don't get auto-zeroed by containers when default-creating them on one's behalf.
 *
 * ### What about Blob? ###
 * Blob specifically exists -- among a few other reasons -- to avoid the default-zeroing that a `vector<uint8_t>`
 * would do.  You should use it instead of `vector<uint8_t>`; you'll get quite a few goodies as a result.
 * Moreover Blob is the heap-allocating case of Basic_blob, which plays well with custom (e.g., SHM-allocating)
 * allocators.  Therefore Default_init_allocator is not needed for byte buffers; it remains potentially useful for
 * containers of other types.
 *
 * ### Source ###
 * I (ygoldfel) took it from the above link(s), made stylistic edits, and added documentation.
 *
 * @tparam T
 *         The type managed by this instance of the allocator.
 *         See `Allocator` concept (in standard or cppreference.com).
 * @tparam Allocator
 *         The `Allocator` being adapted.  Usually one uses the heap-allocating `std::allocator` which is the default
 *         arg for STL-compliant containers usually.  However it may well be something more advanced
 *         such as Flow-IPC's SHM-allocating `ipc::shm::stl::Stateless_allocator` or alias
 *         `ipc::shm::classic::Pool_arena::Allocator`.
 */
template <typename T, typename Allocator>
class Default_init_allocator : public Allocator
{
public:
  // Types.

  /**
   * Satisfies `Allocator` concept optional requirement for rebinding to another type `U`: the result is
   * a Default_init_allocator adapting the adaptee allocator rebound to `U`.  Without this `std::allocator_traits`
   * would use the adaptee's own `rebind` (if any; e.g., `std::allocator` in C++17), silently dropping the
   * adaptor; and containers rebind routinely (e.g., `vector` to its own element type; `list` to its node type).
   *
   * @tparam U
   *         See `Allocator` concept docs.
   */
  template<typename U>
  struct rebind
  {
    // Types.

    /// See `Allocator` concept docs.
    using other = Default_init_allocator<U, typename std::allocator_traits<Allocator>::template rebind_alloc<U>>;
  };

  // Constructors/destructor.

  /// Inherit adaptee allocator's constructors.
  using Allocator::Allocator;

  // Methods.

  /**
   * Satisfies `Allocator` concept optional requirement for in-place construction: specialized for 0-args,
   * i.e., value-initialization; replaces value-initialization with default-initialization.
   * This specialization is the reason Default_init_allocator exists at all.
   *
   * @tparam U
   *         Type being constructed.  See `Allocator` concept docs.
   * @param ptr
   *        Address at which to in-place-construct the `U`.
   */
  template<typename U>
  void construct(U* ptr) noexcept(std::is_nothrow_default_constructible_v<U>);

  /**
   * Satisfies `Allocator` concept optional requirement for in-place construction: non-specialized version
   * invoked for 1+ args.  This behaves identically to the adaptee allocator.
   *
   * @tparam U
   *         Type being constructed.  See `Allocator` concept docs.
   * @tparam Args
   *         Constructor args (1+ of them).
   * @param ptr
   *        Address at which to in-place-construct the `U`.
   * @param args
   *        See `Args...`.
   */
  template<typename U, typename... Args>
  void construct(U* ptr, Args&&... args);
}; // class Default_init_allocator

// Template implementations.

template<typename T, typename Allocator>
template <typename U>
void Default_init_allocator<T, Allocator>::construct(U* ptr) noexcept(std::is_nothrow_default_constructible_v<U>)
{
  // If U's default-init = value-init, this changes no behavior.  Otherwise this performs default-init (~no-op).
  ::new(static_cast<void*>(ptr)) U;
}

template<typename T, typename Allocator>
template <typename U, typename... Args>
void Default_init_allocator<T, Allocator>::construct(U* ptr, Args&&... args)
{
  // Just do the normal thing.
  std::allocator_traits<Allocator>::construct(static_cast<Allocator&>(*this),
                                              ptr, std::forward<Args>(args)...);
}

} // namespace flow::util
