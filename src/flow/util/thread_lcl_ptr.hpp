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

#include "flow/util/detail/thread_lcl_ptr_impl.hpp"
#include <boost/core/noncopyable.hpp>
#include <boost/thread/tss.hpp>

namespace flow::util
{
// Types.

/**
 * `boost::thread_specific_ptr` replacement (augmentation) whose fast-path `.get()` touches only `thread_local`s
 * and an advanced hash-map, avoiding the price of the `thread_specific_ptr`s use of OS threads API and
 * a relatively slow `std::map` lookup.
 *
 * In terms of contract, it is identical to `boost::thread_specific_ptr` -- including, at times crucially, the
 * cleanup algorithm and *when, for a given thread, it runs for all items*.
 *
 * ### Rationale: Performance ###
 * In terms of performance, we suspect it is superior for most use-cases; but there is a trade-off.  Ignoring the
 * memory cost, the trade-off is as follows.
 *   - get(), outside of rare invocations very near the end of a thread or `exit()`ing program, is faster.
 *     Boost's (as of June 2026, 1.91) involves -- taking Linux as the representative example -- a `pthread_`
 *     API call to access TLS via TLS-key; then a lookup in an old-school `std::map`.  `*this` will, instead:
 *     - Access a `thread_local bool`.  This is new but very cheap.
 *     - Access a `thread_local` map (replaces the `pthread_` or equivalent OS-call).  This will probably
 *       end up a single instruction: much better in absolute terms, all else being equal.
 *     - Perform a `boost::unordered_flat_map` lookup, using a previously generated 64-bit ID as opposed to
 *       `this` (probably somewhat better hashing behavior).  Overall: better.
 *   - However, the other basic operation -- reset() -- is strictly slower: in fact it generally performs
 *     internally *both* a `thread_specific_ptr::reset()` *and also* operations to update our new fast-access
 *     stuff.
 *
 * For most use-cases this is better.  Moreover, in some use-cases we've encountered it is better in a
 * salient, if unspectacular, way.  (A rudimentary benchmark is in our test suite; representative x86-64 Linux result:
 * get() ~1.4x faster than `boost::thread_specific_ptr`'s with 1 live instance, ~2x with 16 -- ours stays flat
 * as instance-count grows, theirs degrades -- while reset() is generally ~1.8x slower, as expected per above.)
 *
 * However!  Let us be fair and precise about the justification, because in absolute terms
 * `boost::thread_specific_ptr` on a modern Linux stack is *better* than its rep (along the lines of
 * "syscall is involved, slow") suggests: ~7-11ns per get() in the aforementioned benchmark runs.  For the vast majority
 * of use-cases that is negligible, and developing Thread_local_ptr would be over-engineering.  The specific
 * circumstances in which `*this` could be worthwhile:
 *   - *Hot paths*: `get()`s executed at millions-per-second rates, inside operations whose total budgets are
 *     themselves tens of nanoseconds (in our history: per-deallocation and per-pointer-dereference lookups
 *     in the SHM-heap machinery of our sister project Flow-IPC) -- there a several-ns lookup is a
 *     double-digit percentage of the entire operation.
 *   - *Occupancy scaling*: both impls keep one map per thread shared by all instances process-wide; the
 *     `thread_specific_ptr` lookup cost grows with total instance count (7 -> 11ns going 1 -> 16 in our
 *     runs), while ours stays flat.  So the gap widens, as a program grows busier.
 *   - *Cache realism*: a tight benchmark loop flatters the `std::map` (its nodes stay pinned in L1 cache);
 *     interleaved with real, cache-hostile work, a pointer-chasing tree re-misses in ways a single-probe
 *     flat-map does not.  We therefore *suspect* the field gap exceeds the naively-benchmarked one (we have
 *     not measured that).
 *
 * Generalizing/speculating: if the effort is taken to make something thread-local, as opposed to
 * using a cross-thread datum with a mutex, then perf is usually already a concern.  Sometimes the additional cycles
 * involved in the `thread_specific_ptr` fast-path are significant.  Using `*this` should not hurt, at least,
 * and provides a perf backstop with no contract differences.
 *
 * ### A corner case ###
 * See the warning in the ~Thread_local_ptr() dtor doc header; in short, as with `thread_specific_ptr`, it is
 * not formally allowed to have a *relevant thread* (other then the dtor-calling one) at dtor time, where
 * relevant thread means one with `.get() != null`.  That's the official contract.  Moreover this contract
 * affects Thread_local_state_registry (which uses a `*this`); in that thing if there's a different extant
 * thread at ctor time, for which `.this_thread_state_or_null() != null`, there is also possible instability.
 *
 * We now briefly discuss what actually can happen if one disregards this.  Firstly, assuming no stability
 * issue, `cleanup_func()`/`delete ...` will not immediately (at dtor time) run for the relevant threads.
 * Instead it will occur at thread-exit time for each relevant thread.  (That might be acceptable.
 * Thread_local_state_registry provides cross-thread cleanup in this situation; so it is timelier.)  So with that,
 * we're left with this scary instability.
 *
 * The reason is as follows.  Firstly: temporarily ignore Thread_local_ptr itself and consider its internal
 * core: `boost::thread_specific_ptr` (`tsp`).  `tsp`, per thread, maintains a map from `this` pointers to the user
 * TLS pointers.  Dtor deletes the entry for its own `this` but only from the calling thread's TL-map; the
 * others stay around.  This in itself is fine; it just means the cleanup now must occur no earlier than
 * each given thread's exit; it cannot be triggered via `x.reset()` anymore; as `x` is gone via dtor.
 * The problem is that `this` as a key is not unique across time, and it is possible another `tsp` with the
 * same address springs into existence: Now things can be unstable.  If this new `y` does `y.reset(a)`,
 * the `.reset()` code will erroneously think there is already a thing in this thread... but that's a different
 * thing; it might be of a different type for example, and the `cleanup_func()` might crash if it tries to
 * free it.  `y.get()` will also wrongly yield non-null; basically at that point it's just chaos.
 *
 * Now... Thread_local_ptr actually, in maintaining its fast-access map/cache, internally avoids this problem
 * by using a truly unique ID instead.  So -- at least up to the start of on-thread-exit `thread_local` deinit --
 * get() (by itself) is safe.  reset(), though, unfortunately is not -- because it maintains (internally) both
 * the fast-access map/cache *and* the internal `tsp`; the latter (as we know from above) is unsafe.  Plus it
 * interacts with any other `tsp` operating in the same thread, potentially.
 *
 * @todo Produce a modified version of `boost::thread_specific_ptr` wherein instead of using `this`
 * as the TL-map-key internally, it uses a 64-bit unique ID a-la our `Thread_local_ptr`'s fast-map/cache.
 * It can keep all its other code -- perhaps reduced to target C++17/compiler/OS we want to support -- but otherwise
 * stay as similar as possible, to retain its cleanup-timing guarantees et al and keep busy-work low.
 *
 * @todo Once the preceding to-do (w/r/t a more-stable `boost::thread_specific_ptr`) is done, trivially adjust
 * Thread_local_ptr to internally use that one instead of the less-stable one; the resulting Thread_local_ptr
 * will continue to be faster on get() than Boost-guy; and it'll also inherit the lack of the stability corner case.
 *
 * @internal
 * Impl strategy
 * -------------
 * First a digression in the form of advice:
 * In general we note that this particular topic can be akin to a siren's call; things seem not-too-complex;
 * C++'s `thread_local` support is solid; and the lack of concurrency, by definition, also makes it much
 * easier to reason about various things.  This is probably a misleading impression, we've found, so just
 * hand-wavily speaking we recommend caution, especially when it comes
 * to *reimplementing parts of `thread_specific_ptr`*; it is indeed a fairly aged chunk of code, and there
 * can be a temptation to assume it just hasn't kept up with the times -- but that is probably not exactly the
 * case.  Those guys knew what they were doing, and it is battle-tested.  Attempts to simplify/replace things
 * about it tend to result in wasted time and giving up.  One source of trouble is that arguably the motivations
 * behind this or that decision are not necessarily explained in the source code -- a big one being, e.g.,
 * "why not just use `thread_local`" (the answer "it wasn't available at the start, and they've decided not to
 * change a working thing" seems natural -- but, turns out, that's probably not it or at least not *all* of it).
 * So then one tries to assume answers not in evidence, tries to do it one's own way, and finds out that is
 * not so simple.
 *
 * (Being *slightly* more specific, the trouble tends to begin around cleanup and around thread
 * exit and/or `std::exit()`.)
 *
 * That said let's get into it.  As for the scary warning above, we discuss related stuff briefly below
 * under "How we didn't do it."
 *
 * ### How we did it ###
 * As is probably implied in the public-facing section above, this really wraps a `boost::thread_specific_ptr`;
 * plus in every reset() we additionally save the same thread-local information in our own data structure -- this
 * time `static thread_local`.  The `boost` impl's data structure is `std::map<>` from `this` itself to
 * the user's `new_value` (passed to `reset()`) -- it should be noted, merged among all instantiations of
 * the class template, one map (per thread).  We mimic this with two modifications:
 *   - The map type is a faster map type.
 *   - The key is a per-`*this` 64-bit ID from Unique_id_holder; this is likely to have better hashing
 *     properties than pointer values (but at least not worse; the cost for generating these IDs is negligible).
 *
 * An internal-use class Thread_local_ptr_cache encapsulates this additional data structure and its updates
 * and queries; `*this` code and Thread_local_ptr_cache's code are in cooperation.  So that is #m_cache; while
 * the nearby #m_tsp is the canonical `thread_specific_ptr`.
 *
 * @note We stress that #m_cache is 100% oblivious of `cleanup_func()` or equivalent.  It does not whatsoever
 *       participate in executing that.  It is just for storing #Thread_local_state in a fast-`get()`table way.
 *
 * There is a corner case, which may seem obscure -- but we assure you ignoring it can be quite fatal -- which
 * has to do with a certain gap around thread exit.  Suppose you're running an `std::thread` (not a `boost::thread`;
 * similar API, different impl; and note there is no `std::thread_specific_ptr` -- only `boost::`).
 * The gap: past a `static thread_local`'s deinit but before
 * `thread_specific_ptr`'s `pthread_`-or-equivalent-arranged per-thread deinit.  The cleanup (`delete ...` or
 * `cleanup_func()` as given to ctor) occurs during the latter phase which creates a potential problem.
 * Without detailing the various bad things that can happen in practice, the bottom line is simply that
 * get() or even reset() might be required during that gap -- quite possibly (we've seen empirically
 * even) *during* `cleanup_func()` and/or #Thread_local_state dtor.
 *
 * Our tactic for dealing with this is straightforward: determine when this (for a given thread) gap begins;
 * and simply do not access #m_cache after that.  This should present no problem simply because if
 * reset() or get() is required, one can simply forward to #m_tsp.  The get() is slower, naturally, but at
 * that point it hardly matters.
 *
 * Now: consider a `boost::thread` thread.  Things change somewhat.  `boost::thread`'s thread-body cooperates
 * with `boost::thread_specific_ptr`'s cleanup duties; as a result the `cleanup_func()`s-or-`delete`s stage
 * happens *before* `thread_local` deinit.  (The `pthread`-or-equivalent hook is nullified by `boost::thread`
 * code, so that part won't happen again after `thread_local` deinit.)  The danger that cleanup-or-delete
 * code could rely on some `thread_local` which would be gone already goes away with a `boost::thread` thread.
 *
 * Naturally we have to work with both, as `boost::thread_specific_ptr` does work with both.  That brings us to
 * the exact mechanism we use to mark that `m_caches` is *inoperative*.  This is all per-thread.  The mechanism:
 * a `thread_local bool`: Thread_local_ptr_cache::s_this_thread_global_tl_state_operative; starts `true`; once
 * it is `false` -- we only use the `boost::thread_specific_ptr`.
 *
 * The question is when to set it.  Pleasantly, `boost::this_thread::at_thread_exit(F)` registers `F`, so that
 * `F()` is called at thread exit (along with other so-registered functions in some order), just before the
 * `thread_specific_ptr` cleanups execute.  (If it is an `exit()`ing thread -- such as `main()` returning -- and
 * the `boost::thread_specific_ptr` cleanups don't run, then the at-thread-exit functions won't run either.)
 * So: We register an `F()` that sets the *inoperative* flag.  *First* the flag is set.  *Second* the cleanups
 * run.
 *   - If this occurs after `thread_local` deinit (`std::thread`):
 *     Good: by then the Thread_local_ptr_cache's `thread_local` fast-access map is gone, so trying to check
 *     anything in there is bad.  Cleanup functions/dtors accessing `.get()` or even `.reset()`-to-non-null
 *     will forward entirely to the `thread_specific_ptr`.  In addition: we must remember: either before or
 *     after calling a `cleanup_func()`-or-`delete`, the Boost code will -- a-la `.reset()` -- delete the
 *     entry in its internal TL-map.  It won't actually *call* *our* `.reset()` though.  If it did the result
 *     would be the same though; the flag indicates *inoperative* by then.  Good.
 *   - If this occurs before `thread_local` deinit (`boost::thread`):
 *     Neutral: by then the Thread_local_ptr_cache's `thread_local` fast-access map is still there, so checking
 *     if there would work fine -- but we would have set the flag to *inoperative*, so we won't touch it anyway.
 *     Slight perf hit, but it's only around the cleanup phase -- no problem.  Read on though!
 *     Cleanup functions/dtors accessing `.get()` or even `.reset()`-to-non-null
 *     will forward entirely to the `thread_specific_ptr`.  Again... neutral, neither good nor bad.  BUT!
 *     Again: Either before or after calling a `cleanup_func()`-or-`delete`, the Boost code will -- a-la `.reset()` --
 *     delete the entry in its internal TL-map.  Again: It won't actually *call* *our* `.reset()` though.
 *     This is where our having already set the flag to *inoperative* is not neutral but required.  Suppose
 *     we had not.  A `cleanup_func()` might delete entry A from the map -- without calling
 *     Thread_local_ptr::reset()!  So now there is a stale entry A in our cache-map, probably pointing to something
 *     now deleted (by `cleanup_func()`).  No problem yet... but suppose entry B's `cleanup_func()` now runs...
 *     and performs a `.get()` for A.  The flag is set to *operative* still in this what-if.  The `.get()` will
 *     return non-null: use-after-free (stale entry).
 *
 * So that is why we use `at_thread_exit()` in Thread_local_ptr_cache.  An easy bug would be to do it (set flag
 * to *inoperative*) only when the TL-state containing our fast-map is being destroyed.  Sure, in a sense the
 * flag would be more accurate: it would reflect whether the fast-map (cache) is usable.  However it would
 * cause the bug described in the 2nd bullet above: Cleanup for A might leave a stale entry for A in the
 * cache-map, diverging from `thread_specific_ptr`; then cleanup for B might use-after-free by still "seeing"
 * A as set.
 *
 * ### How we did not do it ###
 * While there are various approaches, the most tempting idea is a full reimpl that entirely bypasses
 * (1) `pthread_` or equivalent or (2) anything that uses it; and instead relies completely on `thread_local`.
 * To sketch it out:
 *   - Write a non-template impl-class that type-erases `Thread_local_state`, stores `void*`s, and handles
 *     the type-safe call of `cleanup_func(Thread_local_state*)` using a type-erasure trick.
 *     (`function<>` is probably easiest, and the perf should be pretty good -- and in any case well outside
 *     the fast-path, get().)  The impl problem then reduces to designing this impl-class:
 *   - Have a `struct Global_tl_state {}` containing the desired hash-map, just as in
 *     Thread_local_ptr_cache.  Can also use save some thread-local sequence numbers per node in this map,
 *     so cleanup can be invoked in reverse-chronological order.
 *   - Keep a simple `thread_local Global_tl_state`.  In its destructor -- which should run at thread exit
 *     or `std::exit()` -- perform the cleanup algorithm.
 *   - To mimick the as-of-this-writing algorithm in `thread_specific_ptr` the cleanup algorithm must wrestle
 *     with the fact that a `cleanup_func()` can add still more things to clean-up; so the algorithm must
 *     keep cycling through the list, until it is empty.
 *     - The seq num thing is optional, really, but it might be a good way to keep things more deterministic
 *       and less churn-y.
 *
 * All of this requires some care but is eminently doable.  In fact we've done it.  It is even arguably true
 * that the resulting this is generally usable.  However:
 *
 * It may be *usable*, but it is *not* a `thread_specific_ptr` replacement.  It's close, but it has a different
 * behavior.  The different behavior, that we know of, is at least as follows.
 *
 * Above we mention how `thread_specific_ptr` cleanup runs either before `thread_local` deinit or after it:
 * `boost::thread` versus `std::thread` at least.  In the above sketch, though, we stipulated running
 * cleanup *as part of* `thread_local` deinit.
 *   - Boost's impl shall run `cleanup_func()` or `delete ...` strictly after all that thread's `thread_local`s
 *     have been destroyed; or strictly before them.
 *   - The hypothetical replacement would, instead, run cleanup *during* that thread's `thread_local` deinit.
 *     - If cleanup touches some `thread_local`: it might work... or it might not work.
 *     - More to the point: it is simply earlier, or later (depending)!  That is not the same behavior!  One
 *       could even say that's better; or not; doesn't matter.  The point is: Boost's just runs "before" or "after"
 *       things, deterministically(ish?), while the alternative runs "during" things, less deterministically.
 *
 * Could one deal with it?  Probably yes -- though it is no joke actually -- there are techniques one can use
 * to affect `thread_local` deinit order.  With `thread_specific_ptr` (and therefore Thread_local_ptr) there is
 * just no need.  Maybe that's fine; but it is not a replacement; and code that relies on the Boost behavior
 * might start being unstable, particularly around thread or program exit time.  Then it is a can of worms.
 *
 * ### Another approach ###
 * We've mentioned the issue, inherited from `thread_specific_ptr`, wherein having a *relevant thread*
 * (one with non-null get()) extant -- excluding the calling thread -- at dtor time means undefined behavior,
 * due to `tsp`'s using `this` as a map-key.  There's a to-do about resolving it earlier up; and it is saying
 * to fix-up `tsp` (our #m_tsp in particular would gain) which would fix it up for Thread_local_ptr too.
 * A reasonable approach.
 *
 * There is a related line of thought we could follow, and it would have the benefit of speeding-up
 * reset() and not having to worry about `tsp` code anymore.  The basic insight is simple: the `thread_local`
 * Thread_local_ptr_cache::Global_tl_state is perfectly sufficient for most of the lifetime of a `*this`.
 * That's how the "How we did not do it" prototype was written.  It eliminates the key-clash problem too.
 * All it lacks is the cleanup "prowess" of `tsp`; and even then it is only about timing of when the cleanup is
 * launched on thread exit.  Doing it in `~Global_state()` is simply too early/too late -- but we could probably
 * rig `pthread_` and/or `at_thread_exit()` calls easily-enough; basically set-up what `tsp` sets up.
 * The required information is all available -- ultimately it is a list of
 * cleanup-functions to call -- and `pthread_`/`boost::thread` API can perhaps be rigged to do it.  The details
 * of that are likely the riskiest aspect here, so it'd be best to develop/prototype/sanity-check that part first.
 * Do recall that a `cleanup_func()` or `~Thread_local_state()` can do `.reset(non_null_value)` -- creating
 * more cleanup work to do immediately, but more to the point `.reset()` has to still work at that stage,
 * when there is no more `Global_state` or indeed any more `thread_local` mechanics available.
 * @endinternal
 *
 * @tparam Thread_local_state_t
 *         See #Thread_local_state.
 * */
template<typename Thread_local_state_t>
class Thread_local_ptr :
  private boost::noncopyable
{
public:
  // Types.

  /**
   * Pointer-to-this-type is stored thread-locally; and #cleanup_func_t takes an argument of pointer-to-this-type.
   * Requirements: See `boost::thread_specific_ptr`.  In short there are almost none.
   */
  using Thread_local_state = Thread_local_state_t;

  /// Type for `cleanup_func_t()` to ctor: takes `Thread_local_state*`; must not throw.
  using cleanup_func_t = void(*)(Thread_local_state*);

  // Constructors/destructor.

  /// Equivalent to `Thread_local_ptr(f)`, where `f(t)` performs `delete t;`.
  Thread_local_ptr();

  /**
   * Equivalent to `boost::thread_specific_ptr`: memorizes the given function pointer.  When cleanup executes
   * the following occurs: if `get()` is null: no-op; if `cleanup_func` is null: no-op; otherwise
   * `cleanup_func(get())`.  The following are all the cleanup points:
   *   - In `reset(new_value)`, if get() is not null and does not equal `new_value`.
   *   - In dtor, if get() is not null.
   *
   * @note In general we recommend perusing the Boost docs when it comes to cleanup algorithms; the above are
   *       the basics, but there is more subtlety to it than that.  (E.g., `cleanup_func()` might add more
   *       things to `cleanup_func()`... and so forth.)
   * @note Also it can throw (system error); so we can throw.
   *
   * @param cleanup_func
   *        See above.  Reminder: null means no cleanup.
   */
  explicit Thread_local_ptr(cleanup_func_t cleanup_func);

  /**
   * Destructor equivalent to that of `boost::thread_specific_ptr`.
   * In short it performs reset() (which is a no-op if get() is null) but -- as always -- can only affect the
   * current thread.  For other threads, if get() is not null, behavior is formally undefined -- see
   * Boost docs -- though at least in Linux what actually happens is that `get()` leaks (is not cleaned-up)
   * ever, modulo resource reclamation at program exit.
   *
   * @note In general we recommend perusing the Boost docs when it comes to the dtor; the above are
   *       the basics, but there is more subtlety to it than that.  (E.g., what about the main, `exit()`ing thread?)
   *
   * @warning As in `boost::thread_specific_ptr`, the formal contract is that the following results in
   *          undefined behavior: At dtor, there are *relevant threads* (threads for which get() is non-null) other
   *          than the current thread.  Hence for full safety either store `*this` as `static` or global; or
   *          join all relevant threads before dtor; or make them all irrelevant (reset()) (same thing really).
   */
  ~Thread_local_ptr();

  // Methods.

  /**
   * Equivalent to `boost::thread_specific_ptr`, potentially forgets/cleans current get(); potentially
   * records `new_value` instead.  To restate the Boost behavior: let `old_value = get()`:
   *   - `old_value == new_value` => no-op (including when non-null).  Else:
   *   - If `!old_value`: get() is made to equal null; then cleanup of `old_value` executes.
   *   - If `new_value` truthy: get() is made to equal `new_value`.
   *
   * @note `boost::thread_specific_ptr::reset()` can throw (system error); so we can throw.
   *       Generally that means `*this`, at least, is kaput.  Probably it's generally chaos in this thread too.
   *
   * @param new_value
   *        See above.
   */
  void reset(Thread_local_state* new_value = nullptr);

  /**
   * Equivalent to `boost::thread_specific_ptr`, returns the value last passed in this thread to
   * reset(); or null if it has not been called.
   *
   * @note The performance should be significantly better than Boost's, for this operation.
   *
   * @return See above.  May well be null.
   */
  Thread_local_state* get() const;

  /**
   * It's get().
   * @return See above.
   */
  Thread_local_state* operator->() const;

  /**
   * It's *(get()).  Behavior is undefined if get() is null; assertion may trip.
   * @return See above.
   */
  Thread_local_state& operator*() const;

  /**
   * Equivalent to `boost::thread_specific_ptr`, potentially forgets -- but does not clean as via `cleanup_func()` --
   * current get(), so that get() returns null; and returns the pre-condition value of get().
   *   - If pre-condition get() is null: no-op.
   *   - Either way returns pre-condition get().
   *
   * @return Pre-condition get() (possibly null).
   */
  Thread_local_state* release();

private:
  // Data.

  /**
   * The canonical `boost::thread_specific_ptr`.  It is declared before #m_cache; hence most saliently its
   * dtor runs before that of #m_cache.
   *
   * ### Rationale for order of `m_tsp` versus `m_cache` ###
   * This should not matter (much), that we can tell anyway, but the order `tsp`-then-`cache`
   * seems least entropy-laden based on the following reasoning:
   *
   * The crux-y op is reset() which in the non-null, `old_value != new_value` case does the following:
   *   -# `cache`: remove pair `this, old_value` from internal map FASTMAP;
   *   -# `tsp`: `.reset(new_value)` which internally is probably:
   *     -# remove pair `this, old_value` from internal map SLOWMAP;
   *     -# invoke cleanup of `old_value`;
   *     -# add pair `this, new_value` to internal map SLOWMAP;
   *   -# `cache`: add pair `this, new_value` to internal map FASTMAP.
   *
   * (If `new_value` is null then skip the last two bullets.)  Fine; the member order isn't relevant to that per se.
   * Now consider the `tsp` and `cache` dtors; each does this:
   *   -# (both) remove pair `this, old_value` from internal map ...MAP;
   *   -# (tsp only) invoke cleanup of released thing.
   * So if we set the order here as `tsp`, `cache`, then *our* dtor will do (referring to the above):
   *   -# `cache`: remove pair `this, old_value` from internal map FASTMAP;
   *   -# `tsp`: remove pair `this, old_value` from internal map SLOWMAP;
   *   -# `tsp`: invoke cleanup of released thing.
   *
   * That matches `reset(nullptr)`; and the dtor conceptually is to do reset() by contract.  So that's probably best...
   * or at least not worse.
   */
  boost::thread_specific_ptr<Thread_local_state> m_tsp;

  /// The `thread_local`-oriented duplication of information in #m_tsp.  See our class doc header.
  Thread_local_ptr_cache m_cache;
}; // class Thread_local_ptr

// Template implementations.

template<typename Thread_local_state_t>
Thread_local_ptr<Thread_local_state_t>::Thread_local_ptr(cleanup_func_t cleanup_func) :
  m_tsp(cleanup_func)
{
  // That's all.  Real action starts, potentially, in reset().
}

template<typename Thread_local_state_t>
Thread_local_ptr<Thread_local_state_t>::Thread_local_ptr() = default;

template<typename Thread_local_state_t>
Thread_local_ptr<Thread_local_state_t>::~Thread_local_ptr() = default; // See comment about member order near m_tsp.

template<typename Thread_local_state_t>
void Thread_local_ptr<Thread_local_state_t>::reset(Thread_local_state* new_value)
{
  using Reset_result = Thread_local_ptr_cache::Reset_result;

  /* See comment about member order near m_tsp; it shows the overall order of ops as a result of the following.
   * The main anti-entropy principles going into that are as follows.
   *   - Let old_value = get(); might be null.  Recall that `new_value == old_value` is possible -- means no-op --
   *     and both `*_value = null` is possible.  Assume for now `new_value != old_value`.
   *   - Split reset(new_value) into 3 parts:
   *     - release() equivalent (delete old_value)
   *     - cleanup w/r/t old_value
   *     - insert new_value.
   *   - An anti-chaos point is that the cleanup-step occurs with the pre-condition that this->get() is null
   *     already.  Like maybe we are reset(null), while cleanup_func() does reset(non_null_state) itself --
   *     that would work; whereas if cleanup happened first, there could be some recursive chaos.  No need to
   *     overthink the possibilities there; we are just pointing out this way is more controlled at least.
   *
   * Right, so since we need to maintain the canonical (but slow-get()-impl-having) m_tsp *and*
   * the touchier-around-deinit (but fast-get()-impl-having) m_cache, we can implement the above steps in the
   * way shown in the aforementioned comment in m_tsp doc header.  That's what you see below, modulo having
   * to also deal with the possibility m_cache is no longer usable (its TLS has been destroyed: thread exiting or
   * std::exit()ing) and `new_value == old_value` (no-op). */

  // Phase 1: delete old_value from m_cache, m_tsp (assuming m_cache is operative and new_value is not a dupe).

  // Phase 1, part 1/2: delete old_value from m_cache (unless m_cache is inoperative, or new_value is a dupe).
  const auto cache_result = m_cache.reset_release(new_value);
  if (cache_result == Reset_result::S_INOPERATIVE)
  {
    /* Phase 1.1 no-oped, because m_cache can no longer operate (we must be near thread exit or exit()).
     * Therefore short-circuit the whole operation (all phases) to simply defer to m_tsp which is canonically
     * correct. */
    m_tsp.reset(new_value);
    return;
  }
  /* else if (m_cache is operative; and we can trust what it reported, at least through the current method;
   *          in particular m_cache remains operative through all below): */

  if (cache_result == Reset_result::S_DUPE)
  {
    return; // Per contract this means no-op.  Since m_cache is accurate, we can skip m_tsp.reset(new_value) too.
  }
  // else if (m_cache is operative, and old_value-if-any has been deleted from it; now should be smooth sailing):

  /* Phase 1, part 2/2: delete old_value from m_tsp (if it's there). +
   * Phase 2: cleanup_func(old_value), if old_value was just there. +
   * [ Phase 3: insert new_value into m_tsp, m_cache. ]
   * Phase 3, part 1/2: insert new_value into m_tsp (unless new_value is falsy). */
  m_tsp.reset(new_value);
  // ^-- Can throw.  As advertised in that case *this should be abandoned.  So just let it happen.  @todo Revisit?

  // Phase 3, part 2/2: insert new_value into m_cache (unless new_value is falsy). */
  if (new_value)
  {
    m_cache.reset_set(new_value);
  }
} // Thread_local_ptr::reset()

template<typename Thread_local_state_t>
Thread_local_state_t* Thread_local_ptr<Thread_local_state_t>::get() const
{
  const auto cache_result_or_inoperative = m_cache.get();
  return cache_result_or_inoperative
           // Got the result real-fast.  This eventuality is the reason this class template exists.
           ? static_cast<Thread_local_state*>(*cache_result_or_inoperative)
           // m_cache inoperative (thread must be exiting or exit()ing), so just fallback to "slow" guy.
           : m_tsp.get();
}

template<typename Thread_local_state_t>
Thread_local_state_t* Thread_local_ptr<Thread_local_state_t>::operator->() const
{
  return get();
}

template<typename Thread_local_state_t>
Thread_local_state_t& Thread_local_ptr<Thread_local_state_t>::operator*() const
{
  Thread_local_state* const value = get();
  assert(value && "Used against contract.");
  return *value;
}

template<typename Thread_local_state_t>
Thread_local_state_t* Thread_local_ptr<Thread_local_state_t>::release()
{
  // Shouldn't matter but mimic order from reset() (phases 1.1, 1.2).
  m_cache.release();
  return m_tsp.release();
}

} // namespace flow::util
