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
#include <type_traits>

namespace flow::util
{
// Types.

/**
 * A `thread_local` object of type #Obj, in effect, but with death-aware access: this_thread_obj_or_null()
 * returns the calling thread's instance -- or null, once that instance has been destroyed during the thread's
 * `thread_local` deinit.  Hence -- unlike a plain `thread_local Obj` at namespace/class/function scope -- it is
 * safe to access from *any* code in the thread at *any* time: including from `thread_local` destructors during
 * deinit (close to thread exit); and from `boost::thread_specific_ptr`-style cleanup (see Thread_local_ptr,
 * Thread_local_state_registry), which -- depending on the thread type (to wit: non-`boost::thread` threads) --
 * can run after deinit.
 *
 * @note It is only useful for #Obj types wherein `Obj` is *not trivially destructible*.  If it is trivially
 *       destructible, then a simple `thread_local Obj` would already be usable at all times from its thread
 *       (including during deinit).
 *
 * To put it in terms of the problem being solved: a plain non-trivially-destructible `thread_local` dies at
 * some hard-to-predict point during deinit; code running near thread death (usually in destructor(s) of
 * thread-local object(s)) cannot know whether the object is still valid; and a wrong guess means undefined
 * behavior.  (It might also work with an `std::thread`-thread but then later explode when someone uses a
 * `boost::thread` or otherwise; or vice versa; or who knows?)  This class provides the missing knowledge, in the
 * form of the null return.
 *
 * One does not instantiate a Thread_local_obj_deinit_safe itself; it is a static-access facility, singleton-like.
 * The identity of the underlying per-thread object is the pair <#Obj, #Tag> (one object per thread per such
 * combo).  Use #Tag to obtain distinct storage, when the same #Obj type is needed for 2+ independent purposes.
 *
 * @note That semantic should not be too onerous, as a naked `thread_local` object can only be `static`
 *       or global in the first place.  So, essentially, instead of identifying a `thread_local` by name, one
 *       identifies a Thread_local_obj_deinit_safe by the aforementioned `Obj`+`Tag` pair.
 *
 * ### How to use ###
 * The semantics:
 *   - The first this_thread_obj_or_null() call in a thread lazily default-constructs that thread's #Obj.
 *     This matches what likely happens in practice for all `thread_local`s (for function-local ones: that is
 *     formally what happens, period).
 *   - Subsequent calls in that thread return the same pointer.  This matches `thread_local` behavior too.
 *   - Once the object is destroyed -- during that thread's `thread_local` deinit, in reverse construction
 *     order versus other `thread_local`s, as usual -- calls return null; and shall keep doing so
 *     (no resurrection).  A `thread_local` access would instead be undefined behavior.
 *     - Therefore: check for null; on null degrade/skip/fall-back to some (presumably slower or lesser)
 *       alternative as appropriate for your use-case.
 *
 * ### Corner case: the *first* this_thread_obj_or_null() occurring near thread death ###
 * Suppose the first-ever this_thread_obj_or_null() call in a given thread occurs when that thread is already
 * dying.  Then:
 *   - If it occurs during `thread_local` deinit (so from some other `thread_local`'s dtor): the #Obj is
 *     constructed normally; and the ongoing deinit pass shall destroy it too, in due course (at least in the
 *     Itanium-ABI/glibc regime -- e.g., Linux -- where destructions registered mid-pass are processed by that
 *     same pass).  So: the full normal lifecycle, compressed into the deinit phase; callers before its
 *     destruction get the object; after it -- null, as usual.
 *   - If it occurs even later -- e.g., in `thread_specific_ptr`-style cleanup of a non-`boost::thread`
 *     thread, which runs after deinit: the #Obj is constructed normally and is fully usable for the rest of
 *     the teardown; however its dtor, though registered, shall never run (the deinit pass is over).
 *     Consequence: the #Obj -- heap parts and all -- *leaks*.  We consider this acceptable: it is rare, at
 *     worst once per dying thread, and the alternative would be having no object at all; but be aware.
 *
 * (The preceding is anchored in how mainstream implementations behave -- the standard is largely silent on
 * construction-during-teardown.  On our target platforms *as of this writing* it holds; a `static_assert()`
 * will warn about looking into this in ports to other platforms.)
 *
 * @internal
 *
 * ### How it works / why it is formally OK ###
 * Internally, next to the #Obj -- which presumably needs a destructor, or you would not bother with any of
 * this -- lives a separate `thread_local bool`.  The #Obj's destruction flips it.  Reading the `bool` at any
 * subsequent time is formally fine: it is trivially destructible and constant-initialized, so its lifetime
 * never ends before the thread's very existence does.  Two subtleties are essential:
 *   - The `bool` is a separate complete object -- *not* a member of some wrapper around #Obj: accessing a
 *     member of a destroyed object is formally undefined behavior, storage-persistence notwithstanding.
 *   - There is no destruction-order dependency of any kind: the `bool` has no destructor, so no order of
 *     `thread_local` deinit can invalidate it.  Incidentally... that is why there's no infinite recursion,
 *     as we try to ourselves use `Thread_local_obj_deinit_safe<bool>`.  (Joking.  It's true though.)
 *
 * @endinternal
 *
 * @tparam Obj_t
 *         The per-thread object type.  Must be default-constructible; and must *not* be trivially
 *         destructible: otherwise a plain `thread_local Obj_t` is already safe
 *         in the above sense, and `*this` class adds nothing (cf. this_thread_unique_token() internals which
 *         use that plain-and-already-safe technique).
 * @tparam Tag_t
 *         Any type; used purely at compile time for disambiguation: distinct #Tag => distinct per-thread
 *         object, given equal #Obj.  Suggested conventions: the class in which the use site lives; or an
 *         ad-hoc `struct X_tag;` declared just for this.  Defaults to `void`.
 */
template<typename Obj_t, typename Tag_t>
class Thread_local_obj_deinit_safe
{
public:
  // Types.

  /// Short-hand for template parameter type: the per-thread object type.
  using Obj = Obj_t;
  /// Short-hand for template parameter type: the disambiguating tag type.
  using Tag = Tag_t;

  static_assert(!std::is_trivially_destructible_v<Obj_t>,
                "There is no point in using Thread_local_obj_deinit_safe with a trivially-destructible Obj: "
                  "a plain `thread_local Obj` is already accessible at all times from its thread, deinit "
                  "included (cf. this_thread_unique_token() internals which use that technique).");

  // Constructors/destructor.

  /// No instances: `*this` class is a static-access facility.
  Thread_local_obj_deinit_safe() = delete;

  // Methods.

  /**
   * Returns pointer to the calling thread's #Obj, lazily default-constructing it if this is the first call
   * in that thread; or null if it has already been destroyed (which occurs during the thread's `thread_local`
   * deinit).  Once null, always null (in that thread).
   *
   * @return See above.
   */
  static Obj* this_thread_obj_or_null();

private:
  // Types.

  /// Holds the actual per-thread #Obj; its destruction records itself in the adjacent dead-flag.
  struct Guarded_obj
  {
    // Constructors/destructor.

    /**
     * Constructs #m_obj via its default ctor.
     *
     * @param dead_flag
     *        Pointer to the flag to flip at destruction.
     */
    explicit Guarded_obj(bool* dead_flag);

    /// Flips the dead-flag; then #m_obj dies (in that order: members are destroyed after the dtor body).
    ~Guarded_obj();

    // Data.

    /// The payload.
    Obj m_obj;

    /// See ctor.
    bool* const m_dead_flag;
  }; // struct Guarded_obj
}; // class Thread_local_obj_deinit_safe

// Template implementations.

template<typename Obj_t, typename Tag_t>
Thread_local_obj_deinit_safe<Obj_t, Tag_t>::Guarded_obj::Guarded_obj(bool* dead_flag) :
  m_dead_flag(dead_flag)
{
  // Yep.
}

template<typename Obj_t, typename Tag_t>
Thread_local_obj_deinit_safe<Obj_t, Tag_t>::Guarded_obj::~Guarded_obj()
{
  *m_dead_flag = true;
}

template<typename Obj_t, typename Tag_t>
typename Thread_local_obj_deinit_safe<Obj_t, Tag_t>::Obj*
  Thread_local_obj_deinit_safe<Obj_t, Tag_t>::this_thread_obj_or_null()
{
#ifndef FLOW_OS_LINUX
static_assert(false,
              "All or most of this should work in other OS, with which we have not tested, but upon "
                "porting to such an OS be sure you've handled any corner cases mentioned throughout "
                "the comments in this impl, starting with class doc header.  At least one corner case "
                "concerns behavior if this_thread_obj_or_null() is called for the first time near thread exit.");
#endif

  /* s_this_thread_obj_dead: constant-initialized + trivially destructible => its lifetime spans the entire
   * thread lifetime: readable before s_this_thread_obj exists and after it is gone -- the whole point.
   * (See class doc header for the formal-correctness discussion.) */
  thread_local bool s_this_thread_obj_dead = false;
  if (s_this_thread_obj_dead)
  {
    /* Do not even pass through s_this_thread_obj's definition below: formally, control passing through the
     * definition of a destroyed block-scope object (static or thread storage duration) is undefined behavior
     * (per standard).  It might still be okay in practice but zero reason to risk it.  ("It" would be
     * something like doing `return s_this_thread_obj_dead ? nullptr : &s_this_thread_obj.m_obj` after
     * the `thread_local` seen below (the 2nd one overall).) */
    return nullptr;
  }
  // else:

  thread_local Guarded_obj s_this_thread_obj{&s_this_thread_obj_dead};
  return &s_this_thread_obj.m_obj;
}

} // namespace flow::util
