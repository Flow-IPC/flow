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

#include "flow/async/detail/async_fwd.hpp"
#include "flow/async/async_fwd.hpp"
#include "flow/util/util_fwd.hpp"
#include "flow/log/log.hpp"
#include <boost/move/unique_ptr.hpp>
#include <boost/thread/future.hpp>

namespace flow::async
{

/**
 * Internally used building block of various concrete Concurrent_task_loop subclasses that encapsulates a thread
 * that spawns at construction time and a dedicated-or-shared util::Task_engine (a/k/a boost.asio `io_service`)
 * `run()`ning in that new thread.
 *
 * This class:
 *   - At construction:
 *     - Takes a ref-counted reference to an existing util::Task_engine, which may be fresh of
 *       posted tasks or not; shared with another Task_qing_thread or not.
 *     - Starts a thread, ultimately executing an indefinitely-running `Task_engine::run()`, thus
 *       either comprising the single thread executing tasks from that `Task_engine`; or joining a pool
 *       of such threads.
 *       - `post(*(this->task_engine()), F)` can be used to queue up tasks to execute; and any already-queued
 *         work on `*(this->task_engine())` may execute in this new thread as well.  (This is all thread-safe due to
 *         the thread-safe nature of boost.asio util::Task_engine posting, including across thread creation and
 *         destruction.)
 *   - Ends* the thread and "joins" it in destructor.
 *     - Can also do this earlier (but still irreversibly) via the stop() API.
 *     - *If the `Task_engine` is shared with other `Task_qing_thread`s, per `own_task_engine` ctor arg,
 *       then the `Task_engine::stop()` call must be executed by outside code instead.
 *   - Exits the program (!) if a task posted via task_engine() throws an uncaught exception.
 *   - Provides access to low-level underlying util::Thread via raw_worker_thread();
 *     see async::optimize_pinning_in_thread_pool().  Note util::Thread in turn allows direct native access to
 *     the OS thread object (pthread handle, or whatever).
 *
 * ### Design rationale ###
 * Task_qing_thread() constructor doc header contains discussion worth reading.
 */
class Task_qing_thread :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Constants.

  /**
   * `exit()` code returned to OS in the event Task_qing_thread chooses to `exit()` the entire program (as of this
   * writing, this occurs on uncaught exception by a user task).
   */
  static const int S_BAD_EXIT;

  // Constructors/destructor.

  /**
   * Constructs object, immediately spawning new (worker) thread, memorizing a ref-counted reference to
   * the provided util::Task_engine which may or may not be a fresh one and meant to be shared with other
   * `Task_qing_thread`s or exclusively by this one.  The post-condition of this constructor is:
   *   - If `done_promise_else_block` is null, it's:
   *     - The new thread has started.
   *     - If `init_func_or_empty` is not `.empty()`, then: `init_func_or_empty()` ran *first-thing* in that thread and
   *       returned.
   *       - Note: This is a useful guarantee, if, say, you need to perform some privileged actions at thread setup;
   *         once this post-condition holds, it is safe to drop privileges.
   *     - The thread is ready to participate in task-posting via `post(*this->task_engine(), F)` and
   *       similar.
   *   - If `done_promise_else_block` is *not* null, it's:
   *     - None.  However, if one executes `done_promise_else_block->get_future().wait()`, upon return from that
   *       statement, the above if-`done_promise_else_block`-is-null post-conditions will hold.
   *
   * @note `!done_promise_else_block` mode is typical.  Just wait for construction, then `post()` away.  The
   *       other mode is helpful when one has an N-thread pool and wants each Task_qing_thread to initialize
   *       concurrently with the others instead of serially.  Then one can pass in N `promise`s to N Task_qing_thread
   *       ctor calls, then wait on the N respective `unique_future`s.
   *
   * @note When creating the util::Task_engine `task_engine`, it is usually a good idea for perf to pass the
   *       concurrency-hint arg value 1 (one) if either `own_task_engine == true`, or the overall thread pool simply
   *       will have but 1 thread.  This will allow boost.asio to optimize internal locking and such.
   *
   * The worker thread started by each Task_qing_thread constructor will exit upon any uncaught exception by one
   * of the user-supplied `Task`s `post()`ed onto it subsequently.  If this occurs, the handler will `exit()` the entire
   * program with a non-zero code after logging (to `*logger_ptr`) the exception message.  (It is informally
   * recommended that all other threads in the application do the same.)
   *
   * Assuming no such uncaught exception is thrown, the thread will run until stop() or the destructor is called and
   * returns.
   *
   * ### Basic concept discussion: To share or not to share (a `Task_engine`)? ###
   * The choice of `own_task_engine` flag, as of this writing, does not actually affect much of `*this` behavior.
   * If `true`, then you're saying this is the only thread to run tasks on the `Task_engine` (call it E).
   * If `false`, it may be shared with other threads.  In practice, though, as of this writing, this only controls
   * whether stop() will perform `E->stop()` (which causes all `E->run()`s to return and hence threads to soon exit)
   * for you (`true`), or you must do it for the shared E yourself (which has other objects like `*this` associated
   * with it).  Either way, E can be used before or after `*this` thread runs in whatever way one prefers, including:
   * one can pre-queueing tasks (via `post(*E, F)` and such) for it to join in executing in new thread; one can inherit
   * any not-yet-executed tasks after stop(), to execute them in some other thread/run().
   *
   * That said, despite the small practical impact in *this* class, the decision of whether to assign one
   * Task_qing_thread (and hence util::Thread and hence native thread) to a `Task_engine` in one-to-one fashion,
   * versus sharing the latter with more `Task_qing_thread`s, is a central one to your design.
   * It is central to specifying the pattern of how `post()`ed `Task`s are
   * spread across actual threads in a pool.  In particular, if it's `true` (not shared), then one *must*
   * select a specific Task_qing_thread (and, therefore, its corresponding worker thread) before
   * actually `post()`ing; otherwise it will be selected intelligently by boost.asio.  On the other hand, if it's
   * `false` (shared), then to guarantee two tasks FG will not execute
   * concurrently (<= desirable if they're assigned to one async::Op) one must probably
   * use a util::Strand.  Meanwhile if it IS `true` (not shared), then one can simply guarantee it by posting
   * onto the same Task_qing_thread (i.e., worker thread)... which is straightforward but theoretically worse at
   * using available time slices across threads.  It's worse that way, but on the other hand thread-to-core-pinning
   * is arguably more predictable in terms of ultimate effect on performance when `Strand`s aren't used.  Plus it
   * might cause thread-caching perf increases.
   *
   * Very informally, and perhaps arguably, the `true` (do-not-share-engine) mode is the legacy way and is how
   * certain entrenched legacy daemons do it; the `false` (share-engine-among-threads) is the common-sense
   * boost.asio-leveraging way which might be the default for new applications; but it depends also on perf analysis
   * of thread caching benefits of the `true` way as well.
   *
   * ### Rationale/history ###
   * Also informally: The hairiness of forcing the user to have to make this decision, and then write potentially
   * `if`-laden code that subsequently actually posts tasks, is a chief motivation for abstracting such details
   * behind the interfaces Concurrent_task_loop and async::Op.  Then the user gets to just post `Task`s, optionally
   * tagged with `Op`s to prevent unwanted concurrency, while the aforementioned interfaces will deal with
   * the different ways of using Task_qing_thread.  Therefore, Task_qing_thread is a detail/ class not to be used
   * by or exposed to the user.
   *
   * Historically, a certain proof of concept (PoC) started out by having "user" code deal with `Task_engine`s directly,
   * quickly morphing to wrap them with Task_qing_thread for ease of use.  Then once this PoC desired to have knobs
   * controlling how tasks are scheduled across threads, without having the "user" code worry about it after
   * initial thread-pool setup, Task_qing_thread was moved from the public area into detail/, and Concurrent_task_loop
   * was born (along with with helper type async::Op).
   *
   * ### Logging subtlety ###
   * For convenience, as promised by at least Cross_thread_task_loop::start() doc header: If user has specified
   * a Config::this_thread_verbosity_override() setting (to reduce or increase log volume temporarily), then
   * we take it upon ourselves to apply this setting to the spawned thread during exactly the following times:
   *   - Any startup logging in the spawned thread, by `*this`.
   *   - Any shutdown logging in the spawned thread, by `*this`, after stop() triggers thread exit.
   *
   * @param logger_ptr
   *        Logger to use for subsequently logging.
   * @param nickname
   *        Brief, human-readable nickname of the new thread pool, as of this writing for logging only.
   * @param task_engine
   *        The util::Task_engine E such that the body of the new thread will be essentially `E->run()`.
   * @param own_task_engine
   *        Essentially, `true` if you do not wish to share `*task_engine` with other `Task_qing_thread`s;
   *        `false` if you do wish to share it with other such threads.  See more detailed notes above.
   *        Also see stop().
   * @param init_func_or_empty
   *        If not `.empty()`, `init_func_or_empty()` shall execute first-thing in the new thread, before internal
   *        code begins the thread's participation in the `*task_engine` event loop (i.e., before task_engine->run()).
   * @param done_promise_else_block
   *        If null, ctor will block until the thread has started and is ready to participate in
   *        `task_engine()->post()`ing (etc.; see above text).  If not null, then it will kick things off asynchronously
   *        and satisfy the `promise *done_promise_else_block` once the thread has started and is ready to p... you
   *        get the idea.
   */
  explicit Task_qing_thread(flow::log::Logger* logger_ptr, util::String_view nickname,
                            const Task_engine_ptr& task_engine, bool own_task_engine,
                            boost::promise<void>* done_promise_else_block = 0,
                            Task&& init_func_or_empty = Task());

  /**
   * stop(), followed by forgetting the `Task_engine` returned by task_engine(); the latter action may
   * destroy that `Task_engine` synchronously.
   *
   * In particular task_engine() shall be destroyed by this destructor, unless you've saved a copy of that `shared_ptr`
   * elsewhere (particularly in `own_task_engine == false` mode in ctor, it is likely to be saved in another
   * Task_qing_thread).
   *
   * Since stop() has the post-condition that the thread has been joined,
   * the same post-condition holds for this destructor.  It is, of course, safe to call this destructor after
   * already having called stop().
   *
   * @see stop() which is useful when you want the thread to exit/be joined, but the underlying `Task_engine` must
   *      continue to exist for a bit; in particular `post()` on it would execute but do nothing.
   *      Then once you've ensured no more such `post()`s are forthcoming, and hence it's safe,
   *      "finish the job" by destroying `*this`.
   *
   * @see task_engine() through which one can obtain a ref-counted util::Task_engine, for example with the idea
   *      to have another thread `task_engine()->run()`, thus inheriting any queued work/tasks and able to enqueue
   *      and execute future ones.
   */
  ~Task_qing_thread();

  // Methods.

  /**
   * Returns pointer to util::Task_engine such that `post()`ing to it will cause the subsequent asynchronous execution
   * of that task in a way explained in the Task_qing_thread() constructor doc header.  This is the same object
   * passed to ctor.
   *
   * Do note that the user's saving a copy of this pointer can extend the life of the returned `Task_engine`
   * (which is NOT at all the same as extending the life of raw_worker_thread(); also NOT at all the same as
   * making it possible to actually execute work which requires threads).
   *
   * Could technically be `const`, but `const` usage is OK to be conservative.  In spirit, at least, it's not `const`.
   *
   * @return See above.
   */
  Task_engine_ptr task_engine();

  /**
   * Returns the util::Thread -- a thin wrapper around the native OS thread handle -- corresponding to the worker
   * thread started in constructor.
   *
   * The intended use of this is to set thread attributes (such as processor-core
   * affinity) in a way that won't affect/disturb the concurrently executing thread's ability to execute tasks;
   * meaning one might grab its native ID and then set some affinity attribute, but it wouldn't (say) suspend the
   * thread or join it.  Slightly informally, then: any such steps ("such" being the informal part) lead to undefined
   * behavior.
   *
   * @return Pointer to `Thread`, not null.  Guaranteed valid until destructor is invoked; guaranteed to be
   *         not-a-thread after stop() and not not-a-thread before it.
   */
  util::Thread* raw_worker_thread();

  /**
   * Blocks the calling thread until the constructor-started thread has finished; if the underlying `Task_engine` is not
   * shared then first signals it to stop executing any further `Task`s, thus causing the constructor-started thread
   * to in fact finish soon and hence this method to return soon.
   *
   * After stop() has returned once already, stop() will immediately return.  Concurrently executing stop() from
   * 2+ different threads leads to undefined behavior.
   *
   * In effect: If we own the `Task_engine` (`own_task_engine == true` in constructor), this method
   * causes the `Task_engine` to stop executing tasks ASAP and then waits as long as necessary for the thread to exit;
   * then returns.  This will be fast if `Task`s are well behaved (do not block).
   *
   * In effect: If we share an external `Task_engine` (`own_task_engine == false` in constructor), this
   * method simply waits for the thread to exit as long as needed.  Hence the caller must trigger the shared
   * `Task_engine` to exit this thread's `Task_engine::run()`.  (In particular, `Task_engine::stop()` will do this.)
   * Otherwise this method will block until then.
   *
   * The key fact is that, after this returns, the `Task_engine` returned by task_engine() shall not have been
   * destroyed by this method.  In particular, `post()` on that `Task_engine` object will still work without
   * undefined behavior/crashing.  The `post()`ed function just won't actually run.  (It may run on another thread but
   * not this one, since by definition this thread has been joined.)
   */
  void stop();

private:
  // Data.

  /// See task_engine().
  Task_engine_ptr m_task_engine;

  /// See constructor.
  bool m_own_task_engine;

  /// Thread created in constructor.  Not-a-thread after stop(); not not-a-thread before stop().
  boost::movelib::unique_ptr<util::Thread> m_worker_thread;
}; // class Task_qing_thread

} // namespace flow::async
