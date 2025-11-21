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

#include <flow/util/util.hpp>
#include <flow/util/string_view.hpp>
#include <flow/log/log.hpp>
#include <boost/thread.hpp>
#include <boost/unordered_map.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <string>
#include <vector>
#include <typeinfo>
#include <type_traits>

namespace flow::util
{
// Types.

/**
 * Similar to `boost::thread_specific_ptr<T>` but with built-in lazy-init semantics; and more importantly on
 * destruction deletes any outstanding `T`s belonging to threads that are still up; plus allows iteration
 * through all per-thread data.
 *
 * An object of this type manages thread-local data as encapsulated in the user-supplied-as-template-arg type,
 * each of which is instantiated (constructed) as needed for each given thread (on first this_thread_state() accessor
 * call in that thread); and cleaned (via destructor) at each thread's exit or `*this` destruction -- whichever
 * occurs earlier.
 *
 * ### Overview/rationale ###
 * Fundamentally `Thread_local_state_registry<T>` is quite similar to `boost::thread_specific_ptr<T>`.
 * There are some minor differences (it is more rigid, always using `new` and `delete` instead of leaving it
 * to the user; mandating a lazy-initialization semantic instead of leaving it to the user;
 * disallowing any reset to null and back from null), but these are just happenstance/preference-based.
 * Likely we'd have just used the Boost guy, if that's all we wanted.
 *
 * The main reason `Thread_local_state_registry<T>` exists is the following feature:
 *   - If `~Thread_local_state_registry` (a `*this` dtor) executes before a given thread X, that has earlier
 *     caused the creation of a thread-local `T` (by calling `this->this_thread_state()` from X), then:
 *   - That dtor, from whichever thread invoked it, deletes that thread-local `T` (for all `T`).
 *   - Corollary: A related feature is the ability to look at all per-thread data accumulated so far (from any
 *     thread).  See state_per_thread() accessor (+ while_locked()).
 *
 * So `*this` dtor does the equivalent of standard per-thread cleanup of per-thread data, if it is invoked
 * before such standard per-thread cleanup has run (because the relevant threads have simply not yet exited).
 *
 * `thread_specific_ptr` does not do that: you must either `.reset()` from each relevant thread, before
 * the `thread_specific_ptr` is itself deleted; or any such thread must exit before (causing an implicit `.reset()`).
 * Nor can one iterate through other threads' data.
 *
 * For this reason most people declare `thread_specific_ptr` either `static` or
 * as a global, as then the `thread_specific_ptr` always outlives the relevant threads, and everything is fine and easy.
 * What if you really must clean resources earlier, when they are no longer necessary, but relevant threads may
 * stay around?  Then try a `*this`.
 *
 * As a secondary reason (ignoring the above) `Thread_local_state_registry` has a more straightforward/rigid API
 * that enforces certain assumptions/conventions (some of this is mentioned above).  These might be appealing
 * depending on one's taste/reasoning.
 *
 * ### How to use ###
 * Ensure the template-arg #Thread_local_state has a proper destructor; and either ensure it has default (no-arg) ctor
 * (in which case it'll be created via `new Thread_local_state{}`), or assign #m_create_state_func (to choose
 * a method of creation yourself).
 *
 * Addendum: the default is `new Thread_local_state{this->get_logger()}` if and only if `Thread_local_state*` is
 * convertible to `Log_context_mt*` (i.e., the latter is a `public` super-class of the former).
 * See also Logging section below.
 *
 * From any thread where you need a #Thread_local_state, call `this->this_thread_state()`.  The first time
 * in a given thread, this shall perform and save `new Thread_local_state{}`; subsequent times it shall return
 * the same pointer.  (You can also save the pointer and reuse it; just be careful.)
 *
 * A given thread's #Thread_local_state object shall be deleted via `delete Thread_local_state` when one of
 * the following occurs, whichever happens first:
 *   - The thread exits.  (Deletion occurs shortly before.)
 *   - `*this` is destroyed (in some -- any -- thread, possibly a totally different one; or one of the ones
 *     for which this_thread_state() was called).
 *
 * That's it.  It is simple.  However we emphasize that using this properly may take some care: particularly
 * as concerns the contents of the #Thread_local_state dtor (whether auto-generated or explicit or some mix), which
 * might run from the relevant thread X in which the underlying object was created and used; but it might
 * also instead run from whichever thread invokes the Thread_local_state_registry dtor first.
 *
 * See doc header for ~Thread_local_state_registry() (dtor) for more on that particular subject.
 *
 * ### How to iterate over/possibly modify other threads' data (safely) ###
 * In short please see state_per_thread() doc header.
 *
 * Also, particularly if you are employing some variation on the "thread-cached access to central state" pattern,
 * it is potentially critical to read the relevant notes in the this_thread_state() doc header.
 *
 * ### Logging ###
 * Logging is somewhat more subtle than is typically the case, because a Thread_local_state_registry is often
 * declared `static` or even global, which means a log::Logger might not be available at times such as before `main()`,
 * after `main()`, or inside `main()` but outside when some main `Logger` is available.  Therefore it is
 * often useful to, e.g., start with `Logger* = nullptr`, change it to something else, then change it back.
 *
 * Please use super-class log::Log_context_mt::set_logger() accordingly.
 *
 * However to avoid any trouble if this_thread_state() is called during a `Logger` change:
 *   - Note that it is `Log_context_mt`, not `Log_context`, so this is thread-safe.
 *     - Internally, perf-wise, we take steps to avoid this having any appreciable effect on fast-path performance.
 *   - If and only if `Thread_local_state*` is convertible to `Log_context_mt*` (i.e., the latter is a `public`
 *     super-class of the former), set_logger() shall invoke `state->set_logger()` to each `state` in
 *     state_per_thread() (i.e., all `Thread_local_state`s currently extant).
 *     - Note that you can override `Log_context_mt::set_logger()` in your `Thread_local_state` so as to, e.g.,
 *       further propagate the new logger to other parts of `Thread_local_state` internals.
 *
 * @tparam Thread_local_state_t
 *         Managed object type -- see above.  We repeat: must have no-arg (default) ctor, or be compatible with your
 *         custom #m_create_state_func; dtor must perform
 *         appropriate cleanup which in particular shall be run from exactly one of exactly the following 2 contexts:
 *         (1) from the thread in which it was created via this_thread_state(), just before the thread exits;
 *         (2) from within the `*this` dtor from whichever thread that was invoked (which may be the creation-thread;
 *         one of the other creation-threads; or some other thread entirely).  See dtor doc header.
 */
template<typename Thread_local_state_t>
class Thread_local_state_registry :
  public log::Log_context_mt,
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for template parameter type.  See our class doc header for requirements.
  using Thread_local_state = Thread_local_state_t;

  /// General info (as of this writing for logging only) about a given entry (thread/object) in state_per_thread().
  struct Metadata
  {
    // Data.

    /// Thread nickname as per log::Logger::set_thread_info().  (Reminder: Might equal `m_thread_id`.)
    std::string m_thread_nickname;
    /// Thread ID.
    Thread_id m_thread_id;
  };

  /// Return type of state_per_thread().
  using State_per_thread_map = boost::unordered_map<Thread_local_state*, Metadata>;

  /// Short-hand for mutex lock; made public for use in while_locked() and state_per_thread().
  using Lock = Lock_guard<Mutex_non_recursive>;

  // Constants.

  /**
   * `true` if and only if #Thread_local_state is a public sub-class of log::Log_context_mt which has
   * implications on set_logger() and default #m_create_state_func behavior.
   */
  static constexpr bool S_TL_STATE_HAS_MT_LOG_CONTEXT = std::is_convertible_v<Thread_local_state*,
                                                                              log::Log_context_mt*>;

  static_assert(!std::is_convertible_v<Thread_local_state*, log::Log_context*>,
                "Thread_local_state_t template param type should not derive from log::Log_context, as "
                  "then set_logger() is not thread-safe; please use Log_context_mt (but be mindful of "
                  "locking-while-logging perf effects in fast-paths).");

  // Data.

  /**
   * this_thread_state(), when needing to create a thread's local new #Thread_local_state to return, makes
   * a stack copy of this member, calls that copy with no args, and uses the `Thread_local_state*` result
   * as the return value for that thread.
   *
   * If, when needed, this value is null (`m_create_state_func.empty() == true`), then:
   *   - If #Thread_local_state is default-ctible and #S_TL_STATE_HAS_MT_LOG_CONTEXT is `false`:
   *     uses `new Thread_local_state`.
   *   - If #Thread_local_state is ctible in form `Thread_local_state{lgr}` where (`lgr` is a `log::Logger*`),
   *     and #S_TL_STATE_HAS_MT_LOG_CONTEXT is `true`:
   *     uses `new Thread_local_state{get_logger()}`.
   *   - Otherwise: Behavior is undefined (assertion may trip at this time).
   *
   * `m_create_state_func` must return a pointer that can be `delete`d in standard fashion.
   *
   * ### Thread safety ###
   * It is not safe to assign this while a thread-first this_thread_state() is invoked.
   */
  Function<Thread_local_state* ()> m_create_state_func;

  // Constructors/destructor.

  /**
   * Create empty registry.  Subsequently you may call this_thread_state() from any thread where you want to use
   * (when called first time, create) thread-local state (a #Thread_local_state).
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param nickname_str
   *        See nickname().
   * @param create_state_func
   *        Initial value for #m_create_state_func.  Default is an `.empty()` (see member doc header for info).
   */
  explicit Thread_local_state_registry(log::Logger* logger_ptr, String_view nickname_str,
                                       decltype(m_create_state_func)&& create_state_func = {});

  /**
   * Deletes each #Thread_local_state to have been created so far by calls to this_thread_state() from various
   * threads (possibly but not necessarily including this thread).
   *
   * ### Careful! ###
   * No thread (not the calling or any other thread) must access a #Thread_local_state returned from `*this`, once
   * this dtor begins executing.  This is usually pretty natural to guarantee by having
   * your Thread_local_state_registry properly placed among various private data members and APIs accessing them.
   *
   * The dtor in the type #Thread_local_state itself must correctly run from *any* thread.
   *   - For many things that's no problem... just normal C++ data and `unique_ptr`s and such.
   *   - For some resources it might be a problem, namely for resources that are thread-local in nature that must
   *     be explicitly freed via API calls.
   *     Example: flushing a memory manager's thread-cache X created for/in thread T might be only possible
   *     in thread T; while also being a quite-natural thing to do in that thread, during thread T cleanup.
   *     From any other thread it might lead to undefined behavior.
   *     - In this case recall this *fact*:
   *       `~Thread_local_state()` shall run *either* from its relevant thread; *or* from
   *       the daddy `~Thread_local_state_registry()`.
   *     - Usually in the latter case, everything is going down anyway -- hence typically it is not necessary to
   *       specifically clean such per-thread resources as thread-caches.
   *     - So it is simple to:
   *       - Save a data member containing, e.g., `boost::this_thread::get_id()` in #Thread_local_state.
   *       - In its dtor check whether the thread-ID at *that* time equals the saved one.  If so -- great,
   *         clean the thing.  If not -- just don't (it is probably moot as shown above).
   *       - If it is not moot, you'll have to come up with something clever.  Unlikely though.
   */
  ~Thread_local_state_registry();

  // Methods.

  /**
   * Returns pointer to this thread's thread-local object, first constructing it via #m_create_state_func if
   * it is the first `this->this_thread_state()` call in this thread.  In a given thread this shall always return
   * the same pointer.
   *
   * The pointee shall be destroyed from `*this` dtor or just before this thread's exit, from this thread, whichever
   * occurs first.  You may not call this, or use the returned pointer, after either routine begins executing.
   *
   * ### Thread-caching of central canonical state: Interaction with while_locked() and state_per_thread() ###
   * The following is irrelevant in the fast-path, wherein this is *not* the first call to this method in the
   * current thread.  It is relevant only in the fast-path, wherein this *is* the first call to this method in the
   * current thread.  In that case we make the following formal guarantee:
   *
   *   - A ctor of #Thread_local_state is invoked (as you know).
   *   - It is invoked while_locked().
   *
   * The most immediate consequence of the latter is simply: Do not call while_locked() inside #Thread_local_state
   * ctor; it will deadlock.  That aside though:
   *
   * What's the rationale for this guarantee?  Answer: In many cases it does not matter, and other than the last bullet
   * one would not need to worry about it.  It *can* however matter in more complex setups, namely the
   * pattern "thread-caching of central canonical state."  In this pattern:
   *
   *   - Some kind of *central state* (e.g., *canonical* info being distributed into thread-local caches) must be
   *     - seeded (copied, as a *pull*) into any new #Thread_local_state; and
   *     - updated (copied, as a *push*) into any existing #Thread_local_state, if the canonical state itself is
   *       modified (usually assumed to be rare).
   *   - Suppose one invokes while_locked() whenever modifying the central (canonical) state (perhaps infrequently).
   *     - And we also guarantee it is already in effect inside #Thread_local_state ctor.
   *     - Hence, as is natural, we do the seeding/pulling of the central state inside that ctor.
   *   - In that case while_locked() being active in the call stack (<=> its implied mutex being locked) guarantees
   *     the synchronization of the following state:
   *     - which `Thread_local_state`s exist (in the sense that that they might be returned via
   *       this_thread_state() in the future) in the process;
   *     - the cached copies of the canonical state in all existent (as defined in previous bullet)
   *       `Thread_local_state`s;
   *     - the canonical state (equal to every cached copy!).
   *
   * This simplifies thinking about the situation immensely, as to the extent that the central state is distributed
   * to threads via thread-local `Thread_local_state` objects, the whole thing is monolithic: the state is synchronized
   * to all relevant threads at all times.  That said the following is an important corollary, at least for this
   * use-case:
   *
   *   - Assumption: To be useful, the central-state-copy must be accessed by users in relevant threads, probably
   *     via an accessor; so something like: `const auto cool_state_copy = X.this_thread_state()->cool_state()`.
   *     - There are of course variations on this; it could be a method of `Thread_local_state` that uses
   *       the value of the `private` central-state-copy for some computation.  We'll use the accessor setup for
   *       exposition purposes.
   *   - Fact: The central-state-copy inside `*X.this_thread_state()` for the current thread can change at any time.
   *     - Therefore: cool_state() accessor, internally, *must* lock/unlock *some* mutex in order to guarantee
   *       synchronization.
   *     - It would be safe for cool_state() to "simply" use while_locked().  However, in any perf-sensitive scenario
   *       (which is essentially guaranteed to be the case: otherwise why setup thread-cached access to the cool-state
   *       in the first place?) this is utterly unacceptable.  Now any threads using `->cool_state()` on their
   *       thread-local `Thread_local_state`s will contend for the same central mutex; it defeats the whole purpose.
   *   - Hence the corollary:
   *     - Probably you want to introduce your own additional mutex as a data member of #Thread_local_state.  Call
   *       it `m_cool_state_mutex`, say.
   *     - In `->cool_state()` impl, lock it, get the copy of the central-state-copy cool-state, unlock it, return
   *       the copy.
   *     - In the *push* code invoked when the *canonical* central-state is updated -- as noted, this occurs
   *       while_locked() already -- similarly, when pushing to per-thread `Thread_local_state* x`, lock
   *       `x->m_cool_state_mutex`, update the central-state-copy of `*x`, unlock.
   *       - Since there are up to 2 mutexes involved (while_locked() central mutex, `x->m_cool_state_mutex` "little"
   *         mutex), there is some danger of deadlock; but if you are careful then it will be fine:
   *         - Fast-path is in `x->cool_state()`: Only lock `x->m_cool_state_mutex`.
   *         - Slow-path 1 is in the central-state-updating (push) code: `while_locked(F)`; inside `F()` lock
   *           `x->m_cool_state_mutex` for each `x` in state_per_thread().
   *         - Slow-path 2 is in the `*x` ctor: central-state-init (pull) code: `while_locked()` is automatically
   *           in effect inside this_thread_state(); no need to lock `x->m_cool_state_mutex`, since no-place has
   *           access to `*x` until its ctor finishes and hence this_thread_state() returns.
   *     - The bad news: Your impl is no longer lock-free even in the fast-path: `X.this_thread_state()->cool_state()`
   *       does lock and unlock a mutex.
   *     - The good news: This mutex is ~never contended: At most 2 threads can even theoretically vie for it
   *       at a time; and except when *canonical* state must be updated (typically rare), it is only 1 thread.
   *       A regular mutex being locked/unlocked, sans contention, is quite cheap.  This should more than defeat
   *       the preceding "bad news" bullet.
   *
   * @return See above.  Never null.
   */
  Thread_local_state* this_thread_state();

  /**
   * Returns pointer to this thread's thread-local object, if it has been created via an earlier this_thread_state()
   * call; or null if that has not yet occurred.
   *
   * @return See above.
   */
  Thread_local_state* this_thread_state_or_null();

  /**
   * Returns reference to immutable container holding info for each thread in which this_thread_state() has been
   * called: the keys are resulting `Thread_local_state*` pointers; the values are potentially interesting thread
   * info such as thread ID.
   *
   * ### What you may do ###
   * You may access the returned data structure, including the #Thread_local_state pointees, in read-only mode.
   *
   * You may write to each individual #Thread_local_state pointee.  Moreover you are guaranteed (see
   * "Thread safety" below) that no while_locked() user is doing the same simultaneously (byt while_locked()
   * contract).
   *
   * If you *do* write to a particular pointee, remember these points:
   *   - Probably (unless you intentionally avoid it) you're writing to it *not* from the thread to which it
   *     belongs (in the sense that this_thread_state() would be called to obtain the same pointer).
   *   - Therefore you must synchronize any such concurrent read/write accesses from this thread and the owner
   *     thread (your own code therein presumably).  You can use a mutex, or the datum could be `atomic<>`; etc.
   *     - Generally speaking, one uses thread-local stuff to avoid locking, so think hard before you do this.
   *       That said, locking is only expensive assuming lock contention; and if state_per_thread() work
   *       from a not-owner thread is rare, this might not matter perf-wise.  It *does* matter complexity-wise
   *       though (typically), so informally we'd recommend avoiding it.
   *     - Things like `atomic<bool>` flags are pretty decent in these situations.  E.g., one can put into
   *       #Thread_local_state an `atomic<bool> m_do_flush{false}`; set it to `true` (with most-relaxed atomic mode)
   *       via while_locked() + state_per_thread() block when wanting a thread to perform an (e.g.) "flush" action;
   *       and in the owner-thread do checks like:
   *       `if (this_thread_state()->m_do_flush.compare_exchange_strong(true, false, relaxed) { flush_stuff(); }`.
   *       It is speedy and easy.
   *   - You could also surround any access, from the proper owner thread, to that `Thread_local_state` pointee
   *     with while_locked().  Again, usually one uses thread-local stuff to avoid such central-locking actions;
   *     but it is conceivable to use it judiciously.
   *
   * ### Thread safety ###
   * Behavior is undefined, if this is called *not* from within while_locked().
   * Rationale: It might seem like it would have been safe to "just" make a copy of this container (while locking
   * its contents briefly) and return that.  In and of itself that's true, and as long as one never dereferences
   * any `Thread_local_state` pointees, it is safe.  (E.g., one could look at the thread IDs/nicknames in the
   * thus-stored Metadata objects and log them.  Not bad.)  However dereferencing such a #Thread_local_state pointee
   * is not safe outside while_locked(): at any moment its rightful-owning thread might exit and therefore
   * `delete` it.
   *
   * @param safety_lock
   *        Please pass the argument to `task()` given to while_locked().
   * @return See above.
   */
  const State_per_thread_map& state_per_thread(const Lock& safety_lock) const;

  /**
   * Locks the non-recursive registry mutex, such that no access or modification of the (deep or shallow) contents
   * of state_per_thread() shall concurrently occur from within `*this`
   * or other `this->while_locked()` call(s); executes given task; and unlocks said mutex.
   *
   * It is informally expected, though not required, that `task()` shall use state_per_thread().
   * Please see state_per_thread() doc header.
   *
   * Behavior is undefined (actually: deadlock) if task() calls `this->while_locked()` (the mutex is non-recursive).
   *
   * ### Interaction with #Thread_local_state ctor ###
   * See this_thread_state() doc header.  To briefly restate, though: #Thread_local_state ctor, when invoked by
   * this_thread_state() on first call in a given thread, is invoked inside a while_locked().  Therefore do not
   * call while_locked() from such a ctor, as it will deadlock.  From a more positive perspective, informally speaking:
   * you may rely on while_locked() being active at all points inside a #Thread_local_state ctor.
   *
   * @tparam Task
   *         Function object matching signature `void F(const Lock&)`.
   * @param task
   *        This will be invoked as follows: `task(lock)`.
   */
  template<typename Task>
  void while_locked(const Task& task);

  /**
   * Returns nickname, a brief string suitable for logging.  This is included in the output by the `ostream<<`
   * operator as well.  This always returns the same value.
   *
   * @return See above.
   */
  const std::string& nickname() const;

  /**
   * Performs `Log_context_mt::set_logger(logger_ptr)`; and -- if #S_TL_STATE_HAS_MT_LOG_CONTEXT is `true` --
   * propagates it to each extant #Thread_local_state via `state->set_logger(logger_ptr)`.
   *
   * @see also #m_create_state_func doc header w/r/t the effect of #S_TL_STATE_HAS_MT_LOG_CONTEXT on that by
   *      default.
   *
   * ### Thread safety ###
   * It is safe to call this concurrently with (any thread-first invocation of) this_thread_state() on `*this`.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.  Reminder: can be null.
   */
  void set_logger(log::Logger* logger_ptr);

private:
  // Types.

  /// Short-hand for mutex type.
  using Mutex = Lock::mutex_type;

  /**
   * The entirety of the cross-thread registry state, in a `struct` so as to be able to wrap it in a `shared_ptr`.
   * See doc header for Registry_ctl::m_state_per_thread for key info.
   */
  struct Registry_ctl
  {
    // Data.

    /// Protects the Registry_ctl (or `m_state_per_thread`; same difference).
    mutable Mutex m_mutex;

    /**
     * Registry containing each #Thread_local_state, one per distinct thread to have created one via
     * this_thread_state() and not yet exited (rather, not yet executed the on-thread-exit cleanup
     * of its #Thread_local_state).  In addition the mapped values are informational metadata Metadata.
     *
     * ### Creation and cleanup of each `Thread_local_state` (using this member) ###
     * So, in a given thread T:
     *   - The first (user-invoked) this_thread_state() call shall: lock #m_mutex, insert into #m_state_per_thread,
     *     unlock.
     *   - If `*this` is around when T is exiting, the on-thread-exit cleanup function shall:
     *     obtain `shared_ptr<Registry_ctl>` (via `weak_ptr` observer); then lock, delete from #m_state_per_thread,
     *     `delete` the #Thread_local_state itself, unlock.
     *   - If `*this` is not around when T is exiting:
     *     - The cleanup function will not run at all, as the `thread_specific_ptr` controlling that is gone.
     *     - To free resources in timely fashion, the dtor shall (similarly to cleanup function):
     *       lock, delete from #m_state_per_thread (`.clear()` them all), `delete` the #Thread_local_state itself,
     *       unlock.
     *   - If `*this` is around when T is exiting, but `*this` is being destroyed, and `shared_ptr<Registry_ctl>`
     *     has been destroyed already (as seen via `weak_ptr` observer); then the `*this` dtor has run already,
     *     so cleanup function will do (almost) nothing and be right to do so.
     */
    State_per_thread_map m_state_per_thread;
  }; // struct Registry_ctl

  /**
   * The actual user #Thread_local_state stored per thread as lazily-created in this_thread_state(); plus
   * a small bit of internal book-keeping.  What book-keeping, you ask?  Why not just a #Thread_local_state, you ask?
   * Answer:
   *
   * ### Rationale w/r/t the `weak_ptr` ###
   * The essential problem is that in cleanup() (which is called by thread X that earlier issued
   * `Thread_local_state* x` via this_thread_state() if and only if at X exit `*this` still exists, and therefore
   * so does #m_this_thread_state_or_null) we cannot be sure that `x` isn't being concurrently `delete`d and
   * removed from #m_ctl by the (unlikely, but possibly) concurrently executing `*this` dtor.  To do that
   * we must first lock `m_ctl->m_mutex`.  However, `*m_ctl` might concurrently disappear!  This is perfect
   * for `weak_ptr`: we can "just" capture a `weak_ptr` of `shared_ptr` #m_ctl and either grab a co-shared-pointer
   * of `m_ctl` via `weak_ptr::lock()`; or fail to do so which simply means the dtor will do the cleanup anyway.
   *
   * Perfect!  Only one small problem: `thread_specific_ptr` does take a cleanup function... but not a cleanup
   * *function object*.  It takes a straight-up func-pointer.  Therefore we cannot "just capture" anything.  This
   * might seem like some bizarre anachronism, where boost.thread guys made an old-school API and never updated it.
   * This is not the case though.  A function pointer is a pointer to code -- which will always exist.  A functor
   * stores captures.  So now they have to decide where/how to store that.  To store it as regular non-thread-local
   * data would mean needing a mutex, and in any case it breaks their guiding principle of holding only thread-local
   * data -- either natively via pthreads/whatever of via `thread_local`.  Storing it as thread-local means it's
   * just more thread-local state that either itself has to be cleaned up -- which means user could just place it
   * inside the stored type in the first place -- or something that will exist/leak beyond the `thread_specific_ptr`
   * itself assigned to that thread.
   *
   * ### Rationale w/r/t the `weak_ptr` being in the `thread_specific_ptr` itself ###
   * To summarize then: The member #m_ctl_observer is, simply, the (per-thread) `weak_ptr` to the registry's #m_ctl,
   * so that `cleanup(X)` can obtain Registry_ctl::m_state_per_thread and delete the `Thread_local_state* X` from
   * that map (see Registry_ctl::m_state_per_thread doc header).  Simple, right?  Well....
   *
   * If cleanup() runs and finishes before dtor starts, then things are simple enough!  Grab `m_ctl` from
   * `m_ctl_observer`.  Delete the Tl_context from Registry_ctl::m_state_per_thread.  Delete the `Thread_local_state`
   * and the Tl_context (passed to cleanup() by `thread_specific_ptr`).
   *
   * If dtor runs before a given thread exits, then again: simple enough.  Dtor can just do (for each thread's stuff)
   * what cleanup() what have done; hence for the thread in question it would delete the `Thread_local_state` and
   * `Tl_context` and delete the entry from Registry_ctl::m_state_per_thread.  cleanup() will just not run.
   *
   * The problems begin in the unlikely but eminently possible, and annoying, scenario wherein they both run at
   * about the same time, but the dtor gets to the `m_mutex` first and deletes all the `Tl_context`s as well as
   * clearing the map.  cleanup() is already running though... and it needs the `weak_ptr m_ctl_observer` so it
   * can even try to cooperate with the dtor, via `m_ctl_observer.lock()` to begin-with... except the `Tl_context`
   * was just deleted: crash/etc.
   *
   * It's a chicken/egg problem: *the* chicken/egg problem.  The `weak_ptr` cannot itself be part of the watched/deleted
   * state, as it is used to synchronize access to it between dtor and cleanup(), if they run concurrently.
   * So what do we do?  Well... won't lie to you... we leak the `weak_ptr` and `Tl_context` that stores it
   * (roughly 24-32ish bytes in x86-64), in the case where dtor runs first, and cleanup() doesn't (meaning, a thread
   * outlives the `*this`).  (If cleanup() runs, meaning the `*this` outlives a thread, such as if `*this` is
   * being stored `static`ally or globally, then no leak.)  It is a tiny leak, per thread (that outlives
   * a `Thread_local_state_registry` object), per `Thread_local_state_registry` object.
   *
   * Any way to avoid it?  Probably.  Possibly.  One approach (which we tried) is to store a
   * `static thread_local unordered_map<Thread_local_state*, weak_ptr<Registry_ctl>` and save the observer-ptr
   * in that, while Tl_context is not necessary, and #m_this_thread_state_or_null holds a `Thread_local_state*`
   * directly.  The thread-outlives-`*this` scenario just then means any "leak" is only until the thread
   * exits (at which point the whole `unordered_map` goes away by C++ rules of `thread_local`s, including any
   * "leaked" entries in that map).  That is better.  The problem (which we observed -- painfully) is
   * it cannot be guaranteed that this new `static thread_local` map de-init occurs *after* every cleanup() runs;
   * it might happen before: then it's all over; cleanup() cannot trust the map's contents and might even crash.
   *
   * Now the problem is the interplay between a `thread_specific_ptr` and built-in `thread_local`; 2 `thread_local`s
   * and their relative de-init order is already an obscure enough topic; but the `thread_specific_ptr` behavior in this
   * sense is unspecified (and empirically speaking I (ygoldfel) couldn't see anything obvious; in terms of impl
   * it might be doing native stuff in Linux as opposed to `thread_local`... but I digress... it is not workable).
   *
   * It is possibly (probably?) doable to abandon `thread_specific_ptr` and (essentially) reimplement that part
   * by using `thread_local` directly.  However that thing must be `static`, so now we have to reimplement a
   * map from `this`es to `Thread_local_state*`... and so on.  Having done that -- difficult/tedious enough -- now
   * we have to wrangle `static thread_local` relative de-init order.  Supposedly the order is guaranteed by the
   * standard but... it's not particularly pleasant a prospect to deal with it.  Hence I am not making this a formal
   * to-do; even though a part of me thinks that's maybe the most solid approach and puts things in our control most
   * firmly.
   *
   * Just, the Tl_context wrapper-with-small-possible-leak-per-thread design is fairly pragmatic without having to
   * engage in all kinds of masochism.  Still it's a bit yucky in an aesthetic sense.
   */
  struct Tl_context
  {
    /// Observer of (existent or non-existent) daddy's #m_ctl.  See Tl_context doc header for explanation.
    boost::weak_ptr<Registry_ctl> m_ctl_observer;
    /**
     * The main user state.  Never null; but `*m_state` has been freed (`delete`d) if and only if the pointer
     * `m_state` is no longer in `m_ctl_observer.lock()->m_state_per_thread`, or if `m_ctl_observer.lock() == nullptr`.
     */
    Thread_local_state* m_state;
  };

  // Methods.

  /**
   * Called by `thread_specific_ptr` for a given thread's `m_this_thread_state_or_null.get()`, if `*this` dtor has not
   * yet destroyed #m_this_thread_state_or_null.  With proper synchronization:
   * does `delete ctx->m_state` and `delete ctx` and removes the former from Registry_ctl::m_state_per_thread.
   * It is possible that the `*this` dtor runs concurrently (if a relevant thread is exiting right around
   * the time the user chooses to invoke dtor) and manages to `delete ctx->m_state` first; however it will *not*
   * delete the surrounding `ctx`; so that cleanup() can be sure it can access `*ctx` -- but not necessarily
   * `*ctx->m_state`.
   *
   * @param ctx
   *        Value stored in #m_this_thread_state_or_null; where `->m_state` was returned by at least one
   *        this_thread_state() in this thread.  Not null.
   */
  static void cleanup(Tl_context* ctx);

  // Data.

  /// See nickname().
  const std::string m_nickname;

  /**
   * In a given thread T, `m_this_thread_state_or_null.get()` is null if this_thread_state() has not yet been
   * called by `*this` user; else (until either `*this` dtor runs, or at-thread-exit cleanup function runs)
   * pointer to T's thread-local Tl_context object which consists mainly of a pointer to T's
   * thread-local #Thread_local_state object; plus a bit of book-keeping.  (See Tl_context for details on the
   * latter.)
   *
   * ### Cleanup: key discussion ###
   * People tend to declare `thread_specific_ptr x` either `static` or global, because in that case:
   *   - Either `delete x.get()` (default) or `cleanup_func(x.get())` (if one defines custom cleanup func)
   *     runs for each thread...
   *   - ...and *after* that during static/global deinit `x` own dtor runs.  (It does do `x.reset()` in *that*
   *     thread but only that thread; so at "best" one thread's cleanup occurs during `thread_specific_ptr` dtor.)
   *
   * We however declare it as a non-`static` data member.  That's different.  When #m_this_thread_state_or_null
   * is destroyed (during `*this` destruction), if a given thread T (that is not the thread in which dtor is
   * executing) has called this_thread_state() -- thus has `m_this_thread_state_or_null.get() != nullptr` -- and
   * is currently running, then its #Thread_local_state shall leak.  Cleanup functions run only while the owner
   * `thread_specific_ptr` exists.  Boost.thread docs specifically say this.
   *
   * Therefore, in our case, we can make it `static`: but then any cleanup is deferred until thread exit;
   * and while it is maybe not the end of the world, we strive to be better; the whole point of the registry
   * is to do timely cleanup.  So then instead of that we:
   *   - Keep a non-thread-local registry Registry_ctl::m_state_per_thread of each thread's thread-local
   *     #Thread_local_state.
   *   - In dtor iterate through that registry and delete 'em.
   *
   * Let `p` stand for `m_this_thread_state_or_null.get()->m_state`: if `p != nullptr`, that alone does not
   * guarantee that `*p` is valid.  It is valid if and only if #m_ctl is a live `shared_ptr` (as determined
   * via `weak_ptr`), and `p` is in Registry_ctl::m_state_per_thread.  If #m_ctl is not live
   * (`weak_ptr::lock()` is null), then `*this` is destroyed or very soon to be destroyed, and its dtor thus
   * has `delete`d `p`.  If #m_ctl is live, but `p` is not in `m_ctl->m_state_per_thread`, then
   * the same is true: just we happened to have caught the short time period after the dtor deleting all states
   * and clearing `m_state_per_thread`, but while the surrounding Registry_ctl still exists.
   *
   * So is it safe to access `*p`, when we do access it?  Answer: We access it in exactly 2 places:
   *   - When doing `delete p` (in dtor, or in on-thread-exit cleanup function for the relevant thread).
   *     This is safe, because `p` is live if and only if it is not in Registry_ctl::m_state_per_thread
   *     (all this being mutex-synchronized).
   *   - By user code, probably following this_thread_state() to obtain `p`.  This is safe, because:
   *     It is illegal for them to access `*this`-owned state after destroying `*this`.
   *
   * As for the the stuff in `m_this_thread_state_or_null.get()` other than `p` -- the Tl_context surrounding it --
   * again: see Tl_context doc header.
   */
  boost::thread_specific_ptr<Tl_context> m_this_thread_state_or_null;

  /// The non-thread-local state.  See Registry_ctl docs.  `shared_ptr` is used only for `weak_ptr`.
  boost::shared_ptr<Registry_ctl> m_ctl;
}; // class Thread_local_state_registry

/**
 * Optional-use companion to Thread_local_state_registry that enables the `Polled_share_state` pattern wherein
 * from some arbitrary thread user causes the extant thread-locally-activated threads opportunistically collaborate
 * on/using locked shared state, with the no-op fast-path being gated by a high-performance-low-strictness
 * atomic-flag being `false`.
 *
 * This `Polled_shared_state` pattern (I, ygoldfel, made that up... don't know if it's a known thing) is
 * maybe best explained by example.  Suppose we're using Thread_local_state_registry with
 * `Thread_local_state` type being `T`.  Suppose that sometimes some event occurs, in an arbitrary thread (for
 * simplicity let's say that is not in any thread activated by the `Thread_local_state_registry<T>`) that
 * requires each state to execute `thread_locally_launch_rocket()`.  Lastly, suppose that upon launching the
 * *last* rocket required, we must report success via `report_success()` from whichever thread did it.
 *
 * However there are 2-ish problems at least:
 *   - We're not in any of those threads; we need to inform them somehow they each need to
 *     `thread_locally_launch_rocket()`.  There's no way to signal them to do it immediately necessarily;
 *     but we can do it opportunistically to any thread that has already called `this_thread_state()` (been activated).
 *     - Plus there's the non-trivial accounting regarding "last one to launch does a special finishing step" that
 *       requires keeping track of work-load in some shared state.
 *     - Not to mention the fact that the "let's launch missiles!" event might occur before the planned launches
 *       have had a chance to proceed; since then more threads may have become activated and would need to be
 *       added to the list of "planned launches."
 *   - Typically we don't need to launch any rockets; and the *fast-path* is that we in fact don't.
 *     It is important that each activated thread can ask "do we need to launch-rocket?" and get the probable
 *     answer "no" extremely quickly: without locking any mutex, and even more importantly without any contention if
 *     we do.  If the answer is "yes," which is assumed to be rare, *then* even lock-contended-locking is okay.
 *
 * To handle these challenges the pattern is as follows.
 *   - The #Shared_state template param here is perhaps `set<T*>`: the set of `T`s (each belonging to an
 *     activated thread that has called `Thread_local_state_registry<T>::this_thread_state()`) that should execute,
 *     and have not yet executed, `thread_locally_launch_rocket()`.
 *   - Wherever the `Thread_local_state_registry<T>` is declared/instantiated -- e.g., `static`ally --
 *     also declare `Polled_shared_state<set<T*>>`, *immediately before* the registry.
 *   - In `T` ctor -- which by definition executes only in an activated thread and only once -- prepare
 *     an opaque atomic-flag-state by executing this_thread_poll_state() and saving the returned `void*`
 *     into a non-`static` data member of `T` (say, `void* const m_missile_launch_needed_poll_state`).
 *   - If the "let's launch missiles" event occurs, in its code do:
 *
 *   ~~~
 *   registry.while_locked([&](const auto& lock) // Any access across per-thread state is done while_locked().
 *   {
 *     const auto& state_per_thread = registry.state_per_thread(lock);
 *     if (state_per_thread.empty()) { return; } // No missiles to launch for sure; forget it.
 *
 *     // Load the shared state (while_locked()):
 *     missiles_to_launch_polled_shared_state.while_locked([&](set<T*>* threads_that_shall_launch_missiles)
 *     {
 *       // *threads_that_shall_launch_missiles is protected against concurrent change.
 *       for (const auto& state_and_mdt)
 *       {
 *         T* const active_per_thread_t = state_and_mdt.first;
 *         threads_that_shall_launch_missiles->insert(active_per_thread_t);
 *       }
 *     });
 *
 *     // *AFTER!!!* loading the shared state, arm the poll-flag:
 *     for (const auto& state_and_mdt)
 *     {
 *       T* const active_per_thread_t = state_and_mdt.first;
 *       missiles_to_launch_polled_shared_state.arm_next_poll(active_per_thread_t->m_missile_launch_needed_poll_state);
 *       // (We arm every per-thread T; but it is possible and fine to do it only for some.)
 *       // Also note it might already be armed; this would keep it armed; no problem.  Before the for()
 *       // the set<> might already have entries (launches planned, now we're adding possibly more to it).
 *     }
 *   }
 *   ~~~
 *
 * So that's the setup/arming; and now to consume it:
 *   - In each relevant thread, such that `this_thread_state()` has been called in it (and therefore a `T` exists),
 *     whenever the opportunity arises, check the poll-flag, and in the rare case where it is armed,
 *     do `thread_locally_launch_rocket()`:
 *
 *   ~~~
 *   void opportunistically_launch_when_triggered() // Assumes: bool(registry.this_thread_state_or_null()) == true.
 *   {
 *     T* const this_thread_state = registry.this_thread_state()l
 *     if (!missiles_to_launch_polled_shared_state.poll_armed(this_thread_state->m_missile_launch_needed_poll_state))
 *     { // Fast-path!  Nothing to do re. missile-launching.
 *       return;
 *     }
 *     // else: Slow-path.  Examine the shared-state; do what's needed.  Note: poll_armed() would now return false.
 *     missiles_to_launch_polled_shared_state.while_locked([&](set<T*>* threads_that_shall_launch_missiles)
 *     {
 *       if (threads_that_shall_launch_missiles->erase(this_thread_state) == 0)
 *       {
 *         // Already-launched?  Bug?  It depends on your algorithm.  But the least brittle thing to do is likely:
 *         return; // Nothing to do (for us) after all.
 *       }
 *       // else: Okay: we need to launch, and we will, and we've marked our progress about it.
 *       thread_locally_launch_rocket();
 *
 *       if (threads_that_shall_launch_missiles->empty())
 *       {
 *         report_success(); // We launched the last required missile... report success.
 *       }
 *     }
 *   }
 *   ~~~
 *
 * Hopefully that explains it.  It is a little rigid and a little flexible; the nature of #Shared_state is
 * arbitrary, and the above is probably the simplest form of it (but typically we suspect it will usually involve
 * some container(s) tracking some subset of extant `T*`s).
 *
 * Though, perhaps an even simpler scenario might be #Shared_state being an empty `struct Dummy {};`,
 * so that the atomic-flags being armed are the only info actually being transmitted.
 * In the above example that would have been enough -- if not for the requirement to `report_success()`,
 * when the last missile is launched.
 *
 * ### Performance ###
 * The fast-path reasoning is that (1) the arming event occurs rarely and therefore is not part of any fast-path;
 * and (2) thread-local logic can detect `poll_armed() == false` first-thing and do nothing further.
 * Internally we facilitate speed further by poll_armed() using an `atomic<bool>` with an optimized memory-ordering
 * setting that is nevertheless safe (impl details omitted here).  Point is, `if (!....poll_armed()) { return }` shall
 * be a quite speedy check.
 *
 * Last but not least: If #Shared_state is empty (formally: `is_empty_v<Shared_state> == true`; informally:
 * use, e.g., `struct Dummy {};`), then while_locked() will not be generated, and trying to write code that
 * calls it will cause a compile-time `static_assert()` fail.  As noted earlier using Polled_shared_state, despite
 * the name, for not any shared state but just the thread-local distributed flag arming/polling = a perfectly
 * valid approach.
 */
template<typename Shared_state_t>
class Polled_shared_state :
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for template parameter type.
  using Shared_state = Shared_state_t;

  // Constructors/destructor.

  /**
   * Forwards to the stored object's #Shared_state ctor.  You should also, in thread-local context,
   * memorize ptr returned by this_thread_poll_state().
   *
   * Next: outside thread-local context use while_locked() to check/modify #Shared_state contents safely; then
   * for each relevant per-thread context `this->arm_next_poll(x)`, where `x` is the saved this_thread_poll_state();
   * this shall cause `this->poll_armed()` in that thread-local context to return `true` (once, until you
   * again arm_next_poll() it).
   *
   * @tparam Ctor_args
   *         See above.
   * @param shared_state_ctor_args
   *        See above.
   */
  template<typename... Ctor_args>
  Polled_shared_state(Ctor_args&&... shared_state_ctor_args);

  // Methods.

  /**
   * Locks the non-recursive shared-state mutex, such that no access or modification of the contents
   * of the #Shared_state shall concurrently occur; executes given task; and unlocks said mutex.
   *
   * Behavior is undefined (actually: deadlock) if task() calls `this->while_locked()` (the mutex is non-recursive).
   *
   * @tparam Task
   *         Function object matching signature `void F(Shared_state*)`.
   * @param task
   *        This will be invoked as follows: `task(shared_state)`.  `shared_state` shall point to the object
   *        stored in `*this` and constructed in our ctor.
   */
  template<typename Task>
  void while_locked(const Task& task);

  /**
   * To be called from a thread-local context in which you'll be checking poll_armed(), returns opaque pointer
   * to save in your Thread_local_state_registry::Thread_local_state and pass to poll_armed().
   *
   * @return See above.
   */
  void* this_thread_poll_state();

  /**
   * To be called from any context (typically not the targeted thread-local context in which you'll be checking
   * poll_armed, though that works too), this causes the next poll_armed() called in the thread in which
   * `thread_poll_state` was returned to return `true` (once).
   *
   * Tip: Typically one would use arm_next_poll() inside a Thread_local_state_registry::while_locked()
   * statement, perhaps cycling through all of Thread_local_state_registry::state_per_thread() and
   * arming the poll-flags of all or some subset of those `Thread_local_state`s.
   *
   * @param thread_poll_state
   *        Value from this_thread_poll_state() called from within the thread whose next poll_armed() you are
   *        targeting.
   */
  void arm_next_poll(void* thread_poll_state);

  /**
   * If the given thread's poll-flag is not armed, no-ops and returns `false`; otherwise returns `true` and resets
   * poll-flag to `false`.  Use arm_next_poll(), typically from a different thread, to affect when
   * this methods does return `true`.  Usually that means there has been some meaningful change to
   * the stored #Shared_state, and therefore you should look there (and/or modify it) while_locked() immediately.
   *
   * @param thread_poll_state
   *        See arm_next_poll().
   * @return See above.
   */
  bool poll_armed(void* thread_poll_state);

private:
  // Data.

  /**
   * An atomic "do-something" flag per thread; usually/initially `false`; armed to `true` by arm_next_poll()
   * and disarmed by poll_armed().
   */
  Thread_local_state_registry<std::atomic<bool>> m_poll_flag_registry;

  /// Protects #m_shared_state.
  mutable Mutex_non_recursive m_shared_state_mutex;

  /// The managed #Shared_state.
  Shared_state m_shared_state;
}; // class Polled_shared_state

// Free functions: in *_fwd.hpp.

// Thread_local_state_registry template implementations.

template<typename Thread_local_state_t>
Thread_local_state_registry<Thread_local_state_t>::Thread_local_state_registry
  (log::Logger* logger_ptr, String_view nickname_str,
   decltype(m_create_state_func)&& create_state_func) :

  log::Log_context_mt(logger_ptr, Flow_log_component::S_UTIL),

  m_create_state_func(std::move(create_state_func)),
  m_nickname(nickname_str),
  m_this_thread_state_or_null(cleanup),
  m_ctl(boost::make_shared<Registry_ctl>())
{
  FLOW_LOG_INFO("Tl_registry[" << *this << "]: "
                "Registry created (watched type has ID [" << typeid(Thread_local_state).name() << "]).");
}

template<typename Thread_local_state_t>
typename Thread_local_state_registry<Thread_local_state_t>::Thread_local_state*
  Thread_local_state_registry<Thread_local_state_t>::this_thread_state_or_null()
{
  const auto ctx = m_this_thread_state_or_null.get();
  return ctx ? ctx->m_state : nullptr;
}

template<typename Thread_local_state_t>
typename Thread_local_state_registry<Thread_local_state_t>::Thread_local_state*
  Thread_local_state_registry<Thread_local_state_t>::this_thread_state()
{
  using log::Logger;

  auto ctx = m_this_thread_state_or_null.get();
  if (!ctx)
  {
    // (Slow-path.  It is OK to log and do other not-so-fast things.)

    /* We shall be accessing (inserting into) m_state_per_thread which understandably requires while_locked().
     * So bracket the following with while_locked().  Notice, though, that we do this seemingly earlier that needed:
     * Inside, we (1) construct the new Thread_local_state; and only then (2) add it into m_state_per_thread.
     * The mutex-lock is only necessary for (2).  So why lock it now?  Answer: We promised to do so.  Why did we?
     * Answer: See method doc header for rationale. */

    while_locked([&](auto&&...) // Versus this_thread_state()/cleanup().
    {
      // Time to lazy-init.  As advertised:
      decltype(m_create_state_func) create_state_func;
      if (m_create_state_func.empty())
      {
        if constexpr(S_TL_STATE_HAS_MT_LOG_CONTEXT && std::is_constructible_v<Thread_local_state, Logger*>)
        {
          create_state_func = [&]() -> auto { return new Thread_local_state{get_logger()}; };
        }
        else if constexpr((!S_TL_STATE_HAS_MT_LOG_CONTEXT) && std::is_default_constructible_v<Thread_local_state>)
        {
          create_state_func = []() -> auto { return new Thread_local_state; };
        }
        else
        {
          FLOW_LOG_FATAL("Chose not to supply m_create_state_func at time of needing a new Thread_local_state.  "
                         "In this case you must either have <derived from Log_context_mt and supplied ctor "
                         "form T_l_s{lgr} (where lgr is a Logger*)> or <*not* derived from Log_context_mt but "
                         "supplied made T_l_s default-ctible>.  Breaks contract; aborting.");
          assert(false && "Chose not to supply m_create_state_func at time of needing a new Thread_local_state.  "
                            "In this case you must either have <derived from Log_context_mt and supplied ctor "
                            "form T_l_s{lgr} (where lgr is a Logger*)> or <*not* derived from Log_context_mt but "
                            "supplied made T_l_s default-ctible>.  Breaks contract.");
          std::abort();
        }
        /* Subtlety: The is_*_constructible_v checks may seem like mere niceties -- why not just let it not-compile
         * if they don't provide the expected ctor form given S_TL_STATE_HAS_MT_LOG_CONTEXT being true or false --
         * or even actively bad (why not just let it not-compile, so they know the problem at compile-time; or
         * why not static_assert() it?).  Not so: Suppose they *did* provide m_create_state_func, always, but
         * lack the ctor form needed for the case where they hadn't.  Then this code path would still try to get
         * compiled -- and fail to compile -- even though there's no intention of it even ever executing.  That would
         * be annoying and unjust.  The only downside of the solution is the null-m_create_state_func path can only
         * fail at run-time, not compile-time; on balance that is better than the unjust alternative. */
      } // if (m_create_state_func.empty())
      else // if (!m_create_state_func.empty())
      {
        create_state_func = m_create_state_func; // We specifically said we'd copy it.
      }

      ctx = new Tl_context;
      ctx->m_ctl_observer = m_ctl;
      ctx->m_state = create_state_func();

      m_this_thread_state_or_null.reset(ctx);

      /* Now to set up the later cleanup, either at thread-exit, or from our ~dtor(), whichever happens first;
       * and also to provide access to us via enumeration via state_per_thread(). */

      typename decltype(Registry_ctl::m_state_per_thread)::value_type state_and_mdt{ctx->m_state, Metadata{}};
      auto& mdt = state_and_mdt.second;
      /* Save thread info (for logging).  (Note: Logger::set_thread_info() semantics are a bit surprising, out of
       * the log-writing context.  It outputs nickname if available; else thread ID if not.) */
      log::Logger::set_thread_info(&mdt.m_thread_nickname,
                                   &mdt.m_thread_id);
      if (mdt.m_thread_id == Thread_id{})
      {
        mdt.m_thread_id = this_thread::get_id(); // Hence get it ourselves.
      }
      // else { nickname is blank.  Nothing we can do about that though. }

      FLOW_LOG_INFO("Tl_registry[" << *this << "]: Adding thread-local-state @[" << ctx->m_state << "] "
                    "for thread ID [" << mdt.m_thread_id << "]/nickname [" << mdt.m_thread_nickname << "]; "
                    "watched type has ID [" << typeid(Thread_local_state).name() << "]).");
#ifndef NDEBUG
      const auto result =
#endif
      m_ctl->m_state_per_thread.insert(state_and_mdt);
      assert(result.second && "How did `state` ptr value get `new`ed, if another thread has not cleaned up same yet?");
    }); // while_locked()
  } // if (!ctx)
  // else if (ctx) { Fast path: state already init-ed.  Do not log or do anything unnecessary. }

  return ctx->m_state;
} // Thread_local_state_registry::this_thread_state()

template<typename Thread_local_state_t>
Thread_local_state_registry<Thread_local_state_t>::~Thread_local_state_registry()
{
  using std::vector;

  FLOW_LOG_INFO("Tl_registry[" << *this << "]: "
                "Registry shutting down (watched type has ID [" << typeid(Thread_local_state).name() << "]).  "
                "Will now delete thread-local-state for each thread that has not exited before this point.");
  vector<Thread_local_state*> states_to_delete;
  while_locked([&](auto&&...) // Versus cleanup() (possibly even 2+ of them).
  {
    for (const auto& state_and_mdt : m_ctl->m_state_per_thread)
    {
      const auto state = state_and_mdt.first;
      const auto& mdt = state_and_mdt.second;
      FLOW_LOG_INFO("Tl_registry[" << *this << "]: Deleting thread-local-state @[" << state << "] "
                    "for thread ID [" << mdt.m_thread_id << "]/nickname [" << mdt.m_thread_nickname << "].");

      // Let's not `delete state` while locked, if only to match cleanup() avoiding it.
      states_to_delete.push_back(state);
    }
    m_ctl->m_state_per_thread.clear();
  }); // while_locked()

  for (const auto state : states_to_delete)
  {
    delete state;
    /* Careful!  We delete `state` (the Thread_local_state) but *not* the Tl_context (we didn't even store
     * it in the map) that is actually stored in the thread_specific_ptr m_this_thread_state_or_null.
     * See Tl_context doc header for explanation.  In short by leaving it alive we leave cleanup() able to
     * run concurrently with ourselves -- unlikely but possible. */
  }

  /* Subtlety: When m_this_thread_state_or_null is auto-destroyed shortly, it will auto-execute
   * m_this_thread_state_or_null.reset() -- in *this* thread only.  If in fact this_thread_state() has been
   * called in this thread, then it'll try to do cleanup(m_this_thread_state_or_null.get()); nothing good
   * can come of that really.  We could try to prevent it by doing m_this_thread_state_or_null.reset()... but
   * same result.  Instead we do the following which simply replaces the stored (now bogus) ptr with null, and
   * that's it.  We already deleted it, so that's perfect. */
  m_this_thread_state_or_null.release();

  // After the }, m_ctl is nullified, and lastly m_this_thread_state_or_null is destroyed (a no-op in our context).
} // Thread_local_state_registry::~Thread_local_state_registry()

template<typename Thread_local_state_t>
void Thread_local_state_registry<Thread_local_state_t>::cleanup(Tl_context* ctx) // Static.
{
  /* If the relevant *this has been destroyed, typically we would not be called.
   * However it is possible that our thread T is exiting, and just then user in another thread chose to
   * invoke *this dtor.  Therefore we must carefully use locking and weak_ptr (as you'll see) to contend
   * with this possibility; it might be worthwhile to read cleanup() and the dtor in parallel.
   *
   * By the way: Among other things, the relevant *this's Log_context might be around at one point but not another;
   * and by contract same with the underlying Logger.  So we cannot use ->get_logger()/log using that necessarily; we
   * will just have to be quiet; that's life. */

  auto& weak_ptr_to_ctl = ctx->m_ctl_observer;
  const auto shared_ptr_to_ctl = weak_ptr_to_ctl.lock();
  if (!shared_ptr_to_ctl)
  {
    /* Relevant Thread_local_state_registry dtor was called late enough to coincide with current thread about to exit
     * but not quite late enough for its thread_specific_ptr ->m_this_thread_state_or_null to be destroyed.
     * Its shared_ptr m_ctl did already get destroyed though.  So -- we need not worry about cleanup after all.
     * This is rare and fun, but it is no different from that dtor simply running before this thread exited.
     * It will be/is cleaning up our stuff (and everything else) -- except the *ctx wrapper itself.  So clean that
     * up (not actual ctx->m_state payload!); and GTFO. */
    delete ctx;
    return;
  }
  // else

  /* Either the relevant Thread_local_state_registry dtor has not at all run yet, or perhaps it has started to run --
   * but we were able to grab the m_ctl fast enough.  So now either they'll grab m_ctl->m_mutex first, or we will. */
  bool do_delete;
  {
    Lock lock{shared_ptr_to_ctl->m_mutex}; // Versus this_thread_state()/dtor/other thread's/threads' cleanup()(s).
    do_delete = (shared_ptr_to_ctl->m_state_per_thread.erase(ctx->m_state) == 1);
  } // shared_ptr_to_ctl->m_mutex unlocks here.

  /* We don't want to `delete ctx->m_state` inside the locked section; it might not be necessarily always criminal --
   * but in some exotic but real situations the Thread_local_state dtor might launch a new, presumably detached, thread
   * that would itself call this_thread_state() which would deadlock trying to lock the same mutex, if the
   * dtor call doesn't finish fast enough. */
  if (do_delete)
  {
    delete ctx->m_state; // We got the mutex first.  Their ~Thread_local_state() dtor runs here.
  }
  /* else { Guess the concurrently-running dtor got there first!  It `delete`d ctx->m_state and
   *        m_state_per_thread.clear()ed instead of us. } */

  delete ctx; // Either way we can free the little Tl_context; dtor never does that (known/justified leak by dtor).
} // Thread_local_state_registry::cleanup() // Static.

template<typename Thread_local_state_t>
template<typename Task>
void Thread_local_state_registry<Thread_local_state_t>::while_locked(const Task& task)
{
  Lock lock{m_ctl->m_mutex};
  task(lock);
}

template<typename Thread_local_state_t>
const typename Thread_local_state_registry<Thread_local_state_t>::State_per_thread_map&
  Thread_local_state_registry<Thread_local_state_t>::state_per_thread([[maybe_unused]] const Lock& safety_lock) const
{
  assert(safety_lock.owns_lock() && "Please call with value while_locked() passed to your task().");

  return m_ctl->m_state_per_thread;
}

template<typename Thread_local_state_t>
const std::string& Thread_local_state_registry<Thread_local_state_t>::nickname() const
{
  return m_nickname;
}

template<typename Thread_local_state_t>
void Thread_local_state_registry<Thread_local_state_t>::set_logger(log::Logger* logger_ptr)
{
  using log::Log_context_mt;

  Log_context_mt::set_logger(logger_ptr);

  if constexpr(S_TL_STATE_HAS_MT_LOG_CONTEXT)
  {
    while_locked([&](auto&&...)
    {
      for (const auto& state_and_mdt : m_ctl->m_state_per_thread)
      {
        const auto state = state_and_mdt.first;

        state->set_logger(logger_ptr);
      }
    });
  }
} // Thread_local_state_registry::set_logger()

template<typename Thread_local_state_t>
std::ostream& operator<<(std::ostream& os, const Thread_local_state_registry<Thread_local_state_t>& val)
{
  return os << '[' << val.nickname() << "]@" << &val;
}

// Polled_shared_state template implementations.

template<typename Shared_state_t>
template<typename... Ctor_args>
Polled_shared_state<Shared_state_t>::Polled_shared_state(Ctor_args&&... shared_state_ctor_args) :
  m_poll_flag_registry(nullptr, "",
                       []() -> auto { using Atomic_flag = typename decltype(m_poll_flag_registry)::Thread_local_state;
                                      return new Atomic_flag{false}; }),
  m_shared_state(std::forward<Ctor_args>(shared_state_ctor_args)...)
{
  // Yep.
}

template<typename Shared_state_t>
template<typename Task>
void Polled_shared_state<Shared_state_t>::while_locked(const Task& task)
{
  static_assert(!(std::is_empty_v<Shared_state>),
                "There is no need to call while_locked(), when your Shared_state type is empty; "
                  "doing the latter is useful when only Polled_shared_state thread-local flag arm/poll feature "
                  "is needed; but then there's no need to lock anything.");

  flow::util::Lock_guard<decltype(m_shared_state_mutex)> lock{m_shared_state_mutex};
  task(&m_shared_state);
}

template<typename Shared_state_t>
void* Polled_shared_state<Shared_state_t>::this_thread_poll_state()
{
  return static_cast<void*>(m_poll_flag_registry.this_thread_state());
}

template<typename Shared_state_t>
void Polled_shared_state<Shared_state_t>::arm_next_poll(void* thread_poll_state)
{
  using Atomic_flag = typename decltype(m_poll_flag_registry)::Thread_local_state;

  static_cast<Atomic_flag*>(thread_poll_state)->store(true, std::memory_order_acquire);

  /* Explanation of memory_order_release here + memory_order_acquire when loading it in
   * if_requested_destroy_tcaches_and_possibly_finish_arena_kills():
   *
   * Our goal is to signal thread_poll_state's thread to -- when it next has a chance (opportunistic piggy-backing) --
   * do stuff.  Yet since it is opportunistic piggy-backing, the fast-path (where nothing needs to be done)
   * needs to be lightning-fast.  So we "just" set that bool flag.  However
   * we also need to tell it some more info and/or even have it update it, namely whatever
   * Shared_state the user should have set-up before calling this arm_next_poll().  So we do it in the order:
   *   -1- user updates Shared_state while_locked()
   *   -2- set flag = true
   * and in the proper thread later
   *   -A- check flag (poll_armed()), and if that returns true
   *   -B- user reads/updates Shared_state while_locked()
   * The danger is that, to the other thread, our steps -1-2- will be reordered to -2-1-, and it will
   * see flag=true with Shared_state not-ready/empty/whatever (disaster).  However by using RELEASE for -2- and ACQUIRE
   * for -A-, together with the presence of mutex-locking around -1- and -B- (while_locked(); plus the if-relationship
   * between -A-B-), we guarantee the ordering -1-2-A-B- as required.
   *
   * Regarding perf: probably even the strict memory_order_seq_cst in both here and poll_armed() would've been
   * reasonably quick; but we did better than that, approaching the minimally-strict memory_order_relaxed (but
   * not quite). */
} // Polled_shared_state::arm_next_poll()

template<typename Shared_state_t>
bool Polled_shared_state<Shared_state_t>::poll_armed(void* thread_poll_state)
{
  using Atomic_flag = typename decltype(m_poll_flag_registry)::Thread_local_state; // some_namespace::atomic<bool>.

  /* Replace true (armed) with false (no longer armed); return true (was armed)...
   * ...but if was already false (not armed), do nothing ("replace" it with false); return false (was not armed).
   * memory_order_release: See explanation in arm_next_poll(). */
  return static_cast<Atomic_flag*>(thread_poll_state)->exchange(false, std::memory_order_release);

  /* (I (ygoldfel) initially wrote it as:
   *   bool exp = true; return ...->compare_exchange_strong(exp, false, ...release);
   * because it "felt" somehow more robust to "formally" do-nothing, if it is already `false`... but it is clearly
   * slower/weirder to most eyes.) */
}

} // namespace flow::util
