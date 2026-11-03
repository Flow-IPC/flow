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

#include "flow/util/uniq_id_holder.hpp"
#include <boost/core/noncopyable.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cassert>
#include <cstddef>
#include <optional>

namespace flow::util
{
// Types.

/**
 * Internal-use helper for Thread_local_ptr which encapsulates that guy's `thread_local`ly-accessed TLS payloads.
 * It is unlikely it is useful, as of this writing, in other contexts.  Its shape is fairly generic, actually,
 * except the reset_release() + reset_set() semantics are more specialized to an implementing Thread_local_ptr.
 *
 * Please see the impl section of Thread_local_ptr doc header.  It is required background.
 *
 * Do note that a given `Thread_local_ptr_cache`'s thread-local storage applies to all `Thread_local_ptr<T>`s
 * in the thread, regardless of `T`.  That is the (at least partial) reason it is separate from Thread_local_ptr
 * as opposed to just inlined therein.  (It is also arguably cleaner.)
 *
 * Impl notes
 * ----------
 * As noted above, the impl background is in Thread_local_ptr doc header; what we do, algorithmically speaking,
 * is described there and -- perhaps aside from `reset()` division of labor into our two `reset_*()` -- is
 * in and of itself simple.  The way Thread_local_ptr combines a `*this` with `thread_specific_ptr` is not trivial;
 * but again that is none of our business here in `*this`.
 *
 * However, due to the extremely short operations involved, there are some very-tactical perf-relevant items
 * to explain.
 *
 * ### Choice of map ###
 * We actually have not benchmarked various possibilities here; but generally `unordered_flat_map` has a great
 * reputation as a default map type of choice.  So that is what we use.  As noted elsewhere an auto-incrementing
 * 64-bit integer, used as key, has good hashing properties.
 *
 * ### Inlining our `get()` and `thread_local`s ###
 * The Flow coding guide discourages explicit inlining, partially for style reasons and partially based on the
 * idea that compiler's max-optimization mode (`-O3` for gcc, say) will auto-inline things quite well; with LTO
 * picking up the slack across translation units: in this case specifically a translation unit (call it U.cpp)
 * that `include`s this header thread_lcl_ptr_impl.hpp (by way of header-only thread_lcl_ptr.hpp) versus our
 * thread_lcl_ptr_impl.cpp.
 *
 * That said, our task here is specifically perf-oriented -- we would not exist otherwise; the conceit is
 * we fully mirror Boost's contract but do it faster for `get()` -- *and* the op involved is extremely quick
 * to begin with in Boost's impl (e.g., 10 nanosec per `get()` on a quick server machine in 2026).  The Flow
 * coding guide specifically cites such a scenario as the (an) exception to the avoid-explicit-inlining rule.
 *
 * In this particular case, in fact, we found through benchmarking that it really matters.  The fast-path
 * operation we're optimizing is get() (which essentially provides Thread_local_ptr::get(), which is what we timed).
 * Without `inline thread_local`s and `inline get()` -- placing both definitions in our .cpp instead -- we
 * observed (using flow::perf for timing-with-repetition):
 *   - TL-map contains 1 `Thread_local_ptr_cache`s: `thread_specific_ptr` => 7 ns; `Thread_local_ptr` => 11 ns.
 *   - TL-map contains 16 `Thread_local_ptr_cache`s: `thread_specific_ptr` => 11 ns; `Thread_local_ptr` => 11 ns.
 *   - (Increasing object count => Boost's latency grows; ours stays ~flat.)
 *
 * So we do indeed do better... but only once there are a few `Thread_local_ptr<T>`s around for the thread.
 * Not bad but not great; given all the complexity -- and the fact that our `reset()` (while not, usually, along the
 * fast-path) is *slower* than Boost's -- we should really just always be better.  After all, why not -- all we
 * do is access 2 `thread_local`s and perform an allegedly-fast `unordered_flat_map` lookup: we *should* be fast.
 *
 * Turns out, the cross-translation-unit call latency -- in this environment at least -- accounts for over 50%
 * of the overall `get()` latency.  So, with `inline thread_local` x 2 and `inline get()`, we get the promised
 * and desired result wherein we are just faster:
 *   - TL-map contains 1 `Thread_local_ptr_cache`s: `thread_specific_ptr` => 7 ns; `Thread_local_ptr` => 5 ns.
 *   - TL-map contains 16 `Thread_local_ptr_cache`s: `thread_specific_ptr` => 11 ns; `Thread_local_ptr` => 5 ns.
 *   - (Increasing object count => Boost's latency grows; ours stays ~flat.)
 *
 * So that is why we explicitly `inline` the `thread_local`s and get().  (The above results are with Linux/gcc `-O3`.)
 *
 * ### What about release() and `reset_*()`? ###
 * Though they are assumed to not be fast-path, we `inline` them too -- why not?  They *could* be called frequently
 * in some scenarios, and there's no reason to treat them differently.  (Ctor/dtor as of this writing are left alone.)
 *
 * In the above environment Thread_local_ptr::reset() and `thread_specific_ptr::reset()` result were as follows
 * FYI.  This is with the `inline`s as currently implemented.
 *   - `thread_specific_ptr` => 23 ns.
 *   - `Thread_local_ptr` => 41 ns.
 *
 * We are within an order of magnitude -- roughly 2x -- of Boost's thing; this makes sense, as we *do* invoke
 * Boost's thing and then *also* do our own, evidently (and sense-makingly) somewhat-cheaper map-insert.  Again
 * though, the presumption is usually `reset()` is infrequent (in the ubquituous lazy-init pattern: 1x per thread
 * per `*this`), while `get()` is potentially very frequent.
 */
class Thread_local_ptr_cache :
  private boost::noncopyable
{
public:
  // Types.

  /// Encodes result of reset_release(), indicating what the calling Thread_local_ptr::reset() should do next.
  enum class Reset_result
  {
    /**
     * `*this` is operative; key was looked up (X = result, possibly null); X does not equal `new_value`;
     * X, if not null, was deleted from `*this`; proceed with next phase(s) of `reset()`.
     *
     * `*this` shall remain operative for the time being; that is within the highest-level user function executing
     * in this thread.
     */
    S_OK,

    /**
     * `*this` is operative; key was looked up (X = result, possibly null); X equals `new_value`;
     * `reset()` should no-op.
     *
     * `*this` shall remain operative for the time being; that is within the highest-level user function executing
     * in this thread.
     */
    S_DUPE,

    /**
     * `*this` is inoperative permanently (we are near thread exit or `exit()`); do not touch `*this` further;
     * `reset()` should operate independently of `*this`.
     *
     * `*this` shall remain inoperative permanently in this thread.
     */
    S_INOPERATIVE
  };

  // Constructors/destructor.

  /// Ctor.  No apparent effect.
  Thread_local_ptr_cache();

  /**
   * Dtor.  Frees relevant TLS in *this* thread; but note that is all internal book-keeping.  No user-specified
   * cleanup occurs!  As for the internal book-keeping: again, the cleanup (basically our part -- if any -- of the
   * TL-map) is for the current thread; it is opportunistic really, just 'cuz.  In another thread, if
   * get() is not null at this time, then that part of the map will leak until thread exit.
   */
  ~Thread_local_ptr_cache();

  // Methods.

  /**
   * Looks up and returns the value last placed via reset_release() + reset_set(), or null if those have not
   * been invoked, for this thread; returns empty `optional` in the near-thread-exit-or-`exit()` eventuality
   * wherein `*this` is permanently (for the remainder of this thread) inoperable.
   *
   * @return Empty if inoperable (near exit); else: what Thread_local_ptr::get() should return (possibly null).
   */
  inline std::optional<void*> get() const;

  /**
   * Phase 1.1 of Thread_local_ptr::reset(): Executes `reset(new_value)` up-to when one potentially calls
   * `cleanup_func(old_value)`; so not including inserting `new_value`.  For perf purposes it also returns
   * certain information -- see Reset_result docs -- that informs the situation it has detected.
   *
   * @param new_value
   *        See Thread_local_ptr::reset().
   * @return See above; especially see doc header(s) of Reset_result.
   */
  inline Reset_result reset_release(void* new_value);

  /**
   * Phase 3.2 of Thread_local_ptr::reset(): Executes `reset(new_value)` just-after the update of the canonical
   * `thread_specific_ptr`; so after `reset_release(new_value)`.  You must only call this after
   * reset_release() and only if that returned Reset_result::S_OK.  (So in particular get() shall not presently
   * return empty object.)
   *
   * `new_value` must not be null.  Rationale: there is just no point; reset_release() would have done any possible
   * required work.
   *
   * @param new_value
   *        See Thread_local_ptr::reset(); however `new_value` must not be null; else assertion may trip.
   */
  inline void reset_set(void* new_value);

  /**
   * Our part of Thread_local_ptr::release().  Lacks return value though (unnecessary;
   * `thread_specific_ptr::release()` is required anyway and will return the right thing).
   */
  inline void release();

private:
  // Types.

  /**
   * 64-bit (at worst) key for #Tlp_states_map, uniquely generated by Unique_id_holder.
   * `boost::thread_specific_ptr` in this situation uses `this` pointers.
   *
   * ### Rationale ###
   * Firstly it has better hashing behavior, we are told, compared to using a pointer as a hash-key.
   *
   * The second reason is arguably imprecise, in that due to having to cooperate with `boost::thread_specific_ptr`
   * means we can't eliminate the problem entirely; but it is still a great property to have architecturally.
   * It's this: the 64-bit ID is, across this process for *all time*, effectively unique.  Therefore a `*this`
   * will not have the same ID as another `*this`, no matter how long ago that one was relevant.
   * Hand-wavily this means that even a leaked Per_tlp_state -- with no Thread_local_ptr_cache corresponding
   * to it extant any longer -- will not cause trouble for some new Per_tlp_state; that would always have
   * a different #per_tlp_key_t.
   *
   * @see Thread_local_ptr doc header, section "A corner case."  It is relevant to the second property above.
   *      Relatedly note dtor ~Thread_local_ptr_cache().
   */
  using per_tlp_key_t = Unique_id_holder::id_t;

  /**
   * The data stored in the thread-local class-wide Tlp_states_map, per originating Thread_local_ptr_cache.
   * As of this writing it has only one member, but experience shows in these situations it's best to have
   * a `struct` type read to go.  Perf-wise it is identical.
   */
  struct Per_tlp_state
  {
    // Data.

    /// The argument `new_value` to reset_set(); never null.  `get() == nullptr` means there is no Per_tlp_state.
    void* m_tl_state_ptr;
  }; // struct Per_tlp_state

  /**
   * The map for a given thread, with a Per_tlp_state per Thread_local_ptr_cache (<=> Thread_local_ptr<...>)
   * such that the last `reset_set(X)` (<=> Thread_local_ptr::reset()) had `X != nullptr`.
   */
  using Tlp_states_map = boost::unordered_flat_map<per_tlp_key_t, Per_tlp_state>;

  /// Encapsulates two things: the thread's #Tlp_states_map; and what shall run at `thread_local` deinit.
  struct Global_tl_state
  {
    // Constructors/destructor.

    /**
     * Ctor: registers (`boost::this_thread::at_thread_exit()`) a thing that
     * sets #s_this_thread_global_tl_state_operative to `false` (*inoperative*) strictly before
     * `boost::thread_specific_ptr` cleanup (`cleanup_func()`s-or-`delete`s + map removal) executes.
     *
     * @see #s_this_thread_global_tl_state_operative doc header.  It explains why this should be done at that stage:
     *      in `~Global_tl_state()` dtor would be insufficient.
     */
    Global_tl_state();

    /**
     * Dtor that runs at `thread_local` deinit phase; unknown where among this thread's such deinits this runs;
     * but it runs before (in Linux at least) the `pthread_` TLS destructor phase.
     */
    ~Global_tl_state();

    // Data.

    /// The heart of the matter.
    Tlp_states_map m_tlp_states_map;
  }; // struct Global_tl_state

  // Data (thread-local).

  /**
   * A thread-local flag that starts at `true` and becomes `false` to indicate **do not access Global_tl_state**.
   * In practice: it becomes `false` just-ahead of `boost::thread_specific_ptr`'s cleanup phase in the given
   * thread, via `boost::this_thread::at_thread_exit()` in Global_tl_state ctor.
   *
   * @see Thread_local_ptr doc header impl section.  It explains why this should be done at that stage:
   *      in `~Global_tl_state()` dtor would be insufficient.  We now restate that same stuff (in slightly
   *      more concrete terms).
   *
   * It is important to understand that **do not access Global_tl_state** is not equal to
   * "`Global_tl_state` is destroyed."  Those would be equal if and only if a live `Global_tl_state`'s contents
   * (Global_tl_state::m_tlp_states_map) always mirror the `boost::thread_specific_ptr` that `*this` is
   * speeding up.  That is almost always the case, but there could be a sneaky exception:
   *
   * During cleanup, `boost::thread_specific_ptr` will run the cleanup callback/`delete` and then delete
   * the cleanup-target from its map.  However it won't call `Thread_local_ptr_cache::reset*()`; it has no
   * idea that is a thing.  It won't call `Thread_local_ptr::reset()` either; same thing.  So at that point
   * Global_tl_state::m_tlp_states_map keeps a thing that is no longer in the `tsp`.
   *
   * OK, but is that such a crime?  Does it really mean that the map diverging from the canonical map as
   * hypothesized means "do not access Global_tl_state"?  In and of itself no; but
   * actually yes: If another `cleanup_func()` now `.get()`s the earlier entry, that'll forward to
   * Thread_local_ptr_cache::get(); that will access the stale map and return a pointer to a likely-deleted
   * (by the earlier entry's cleanup) thing.
   *
   * That was hopefully all comprehensible, but it buried the lede: **do not access Global_tl_state**
   * (this flag = `false`) needs to be defined more conservatively than "`Global_tl_state` exists."
   * For example... it could be "just set it to `false` always."  That is very conservative, but then
   * this whole contraption (Thread_local_ptr) might as well not exist.  So it needs to be `true` through
   * most of the thread's lifetime -- sometime up to its end stages.  Answer: It is to become `false` just
   * ahead of the `tsp` cleanups running.  Until cleanup, by definition, all modifications of
   * the `tsp` go through Thread_local_ptr methods and will keep Global_tl_state::m_tlp_states_map accurate.
   * `false` just before then means cleanup phase shall never access `m_tlp_states_map`: It will not diverge,
   * nor will it be queried.  During cleanup it's all-`tsp`, no `*this`.
   */
  inline static thread_local bool s_this_thread_global_tl_state_operative = true;

  /// See Global_tl_state doc header.
  inline static thread_local Global_tl_state s_this_thread_state;

  // Data.

  /// `*this`'s unique ID; see important notes on #per_tlp_key_t.
  const per_tlp_key_t m_per_tlp_key;
}; // class Thread_local_ptr_cache

// Inline implementations.

Thread_local_ptr_cache::Reset_result Thread_local_ptr_cache::reset_release(void* new_value)
{
  // If in doubt about rationale of things here (as opposed to our mere contract): start with Thread_local_ptr::reset().

  if (s_this_thread_global_tl_state_operative)
  {
    const auto map_it = s_this_thread_state.m_tlp_states_map.find(m_per_tlp_key);
    if (map_it == s_this_thread_state.m_tlp_states_map.end())
    {
      /* Nothing there => return OK by contract.  Rationale:
       *   - If new_value truthy: mainstream new-insert case; in insert phase they'll call reset_set(new_value) next.
       *   - If new_value is falsy: no-op case; in insert phase they'll no-op, since they'll see new_value is null.
       *     So nothing to alert them about; hence OK. */
      return Reset_result::S_OK;
    }
    // else if (*this is in map):

    if (map_it->second.m_tl_state_ptr == new_value)
    {
      return Reset_result::S_DUPE; // Caller should skip insert phase entirely.
    }
    // else if (old_value != new_value): Delete old_value.

    /* Had we called get() for the above 2 checks, we'd have to re-search for key here.  Our approach is thus
     * faster/better (but wordier). */
    s_this_thread_state.m_tlp_states_map.erase(map_it);
    return Reset_result::S_OK;
  } // if (s_this_thread_global_tl_state_operative)
  // else if (!s_this_thread_global_tl_state_operative):

  return Reset_result::S_INOPERATIVE;
} // Thread_local_ptr_cache::reset_release()

void Thread_local_ptr_cache::reset_set(void* new_value)
{
  /* To avoid subsequent reshufflings, reserve this-sized map on first (across *all* `*this`es per thread) insert.
   * It is only for performance, and even so please remember this is *not* the fast-path (the way this whole
   * contraption is conceived in the first place); get() is the fast-path.  Couple other matters:
   *   - We could do this in a Global_tl_state() ctor; that would be prettier but does use some significant
   *     memory even for a thread that only ever get()s (and yields null).
   *   - The way we do it is less pretty and can also do some redundant .reserve()ing -- if
   *     we do empty=>non-empty=>reserve=>empty=>non-empty=> redundant reserve.  That said, then it's a quick
   *     no-op, and it's not the fast-path anyway.  Plus it is probably unlikely. */
  constexpr size_t MAP_RESERVED_SIZE = 1024;

  assert(s_this_thread_global_tl_state_operative
         && "By contract: only call us if `reset_release() != INOPERATIVE` and only immediately after that.");
  assert(new_value && "Null values not allowed; and reset_set() is not to be used for erasing.");

  if (s_this_thread_state.m_tlp_states_map.empty())
  {
    s_this_thread_state.m_tlp_states_map.reserve(MAP_RESERVED_SIZE);
  }

  s_this_thread_state.m_tlp_states_map[m_per_tlp_key] = { new_value };
} // Thread_local_ptr_cache::reset_set()

std::optional<void*> Thread_local_ptr_cache::get() const
{
  std::optional<void*> result;

  if (s_this_thread_global_tl_state_operative)
  {
    const auto map_it = s_this_thread_state.m_tlp_states_map.find(m_per_tlp_key);
    result.emplace((map_it == s_this_thread_state.m_tlp_states_map.end()) ? static_cast<void*>(nullptr)
                                                                          : map_it->second.m_tl_state_ptr);
  }
  // else { Leave result blank indicating INOPERATIVE. }

  return result;
}

void Thread_local_ptr_cache::release()
{
  if (s_this_thread_global_tl_state_operative)
  {
    s_this_thread_state.m_tlp_states_map.erase(m_per_tlp_key);
  }
}

} // namespace flow::util
