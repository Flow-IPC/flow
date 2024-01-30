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
#include "flow/log/async_file_logger.hpp"
#include "flow/log/detail/serial_file_logger.hpp"
#include "flow/error/error.hpp"
#include <algorithm>
#include <memory>
#include <utility>

namespace flow::log
{

namespace
{

  // Locally used utility types.  Leaving out doc boiler-plate to remain concise.  See do_log() for key context.

  /// @cond
  // -^- Doxygen, please ignore the following.  We don't need this appearing as undocumented classes or something.

  class Tight_blob final
  {
  public:
    explicit Tight_blob(const char* src, size_t sz) :
      m_size(sz),
      m_data(boost::movelib::make_unique_definit<char[]>(m_size)) // Save a few cycles by not zero-initializing.
    {
      std::memcpy(m_data.get(), src, size());
    }
    Tight_blob(const Tight_blob& other) :
      Tight_blob(other.data(), other.size())
    {
      // Done.
    }
    Tight_blob(Tight_blob&& other) noexcept = default;

    Tight_blob& operator=(const Tight_blob&) = delete;
    Tight_blob& operator=(Tight_blob&&) = delete;

    size_t size() const noexcept
    {
      return m_size;
    }

    const char* data() const
    {
      return m_data.get();
    }

  private:
    size_t m_size;
    boost::movelib::unique_ptr<char[]> m_data;
  }; // class Tight_blob

  // Same deal with *metadata.
  class Mdt_wrapper final
  {
  public:
    explicit Mdt_wrapper(Msg_metadata* metadata) :
      m_data(metadata)
    {
      // Done.
    }
    /* Reminder: this will not be called but is required for std::function<> + lambda capture to compile.
     * Thogh it's not criminal even if it were called: it merely copies a Msg_metadata. */
    Mdt_wrapper(const Mdt_wrapper& other) :
      Mdt_wrapper(new Msg_metadata(*(other.data())))
    {
      // Done.
    }
    Mdt_wrapper(Mdt_wrapper&& other) noexcept = default;

    Mdt_wrapper& operator=(const Mdt_wrapper&) = delete;
    Mdt_wrapper& operator=(Mdt_wrapper&&) = delete;

    const Msg_metadata* data() const
    {
      return m_data.get();
    }
    Msg_metadata* data()
    {
      return m_data.get();
    }

  private:
    boost::movelib::unique_ptr<Msg_metadata> m_data;
  }; // class Mdt_wrapper
} // namespace (anon)

// -v- Doxygen, please stop ignoring.
/// @endcond

// Implementations.

Async_file_logger::Async_file_logger(Logger* backup_logger_ptr,
                                     Config* config, const fs::path& log_path,
                                     bool capture_rotate_signals_internally) :
  /* Set up the logging *about our real attempts at file logging*, probably to a Simple_ostream_logger/cout;
   * or to null (meaning no such logging).  Either way we will still attempt to actually log user-requested messages
   * to log_path! */
  Log_context(backup_logger_ptr, Flow_log_component::S_LOG),
  m_config(config), // Save pointer, not copy, of the config given to us.  Hence thread safety is a thing.

  /* Set up throttling algorithm, which is always-on (see class doc header discussion), initial config/state.
   * Reminder that its output will be ignored, until/unless user calls throttling_cfg(true, ...).  They can then
   * set their own config values as well (or reuse this default which they can access via throttling_cfg() accessor).
   *
   * Note that these values -- 2GB, 1GB -- are not meant to be some kind of universally correct choice.  Users
   * can and should change them, but if they're not using the feature then they won't care anyway. */
#if 1
  m_throttling_states
    {
        // (@todo make_unique() was complaining of lacking copy ctor; not sure why it is needed.  Not very important.)
        boost::movelib::unique_ptr<Throttling>
          (new Throttling
                 {
                   // @todo Make some magic number `constexpr`s?
                   { 2ull * 1024 * 1024 * 1024, 2ull * 1024 * 1024 * 1024 },
                   0, 0 // No memory used yet; no throttling yet.
                 })
    },
#endif
  m_throttling(m_throttling_states.back().get()),
  m_throttling_active(false),

  // Any I/O operations done here are the only ones not done from m_async_worker thread (until maybe dtor).
  m_serial_logger(boost::movelib::make_unique<Serial_file_logger>(get_logger(), m_config, log_path)),
  /* ...Speaking of which: start the worker thread right now; synchronously; return from this ctor once it has
   * certified it is up.  It will log (to *backup_logger_ptr if not null) about starting up right here, too, which
   * in practice would be the bulk of INFO-or-less-verbose log lines from us in my experience. */
  m_async_worker(backup_logger_ptr, util::ostream_op_string("Async_file_logger[", this, ']')),
  /* Set up a signal set object; this is a no-op until we .add() signals to it (which we may or may not do).
   * Whether we do or not is more significant than merely whether whatever handler we'd later register
   * via m_signal_set.async_wait() will be called; if we .add() zero signals, then IF some non-boost.asio
   * signal handler is currently registered (such as some default OS handler; or the user's non-boost.asio handler)
   * (or will be registered in the future) will continue to work undisturbed.  If, however, we .add() one or more
   * signals (or, equivalently, list 1 or more signal numbers right here in the constructor call),
   * then we will REPLACE such non-boost.asio handlers with boost.asio's mechanism.  (This behavior is
   * explicitly documented in boost.asio docs.)  Therefore we must be careful not to mindlessly .add() handler(s),
   * and/or (equivalently) list 1 or more signal numbers in this constructor call here.  The choice will be left to
   * later code which will .add() or not .add() deliberately.  (Copy/paste warning: This cmnt was copy-pasted from some
   * vaguely similar code in flow::net_flow; but the 2 modules are barely related if at all, so....) */
  m_signal_set(*(m_async_worker.task_engine()))
{
  m_async_worker.start();

  FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                "Log-writing worker thread(s) have started around now; ready to work.");

  // Immediately open file in worker thread.  Could be lazy about it but might as well log about any issue ASAP.
  log_flush_and_reopen(false); // Do it synchronously for good measure; doesn't matter but the reduced entropy is nice.

  if (capture_rotate_signals_internally)
  {
    FLOW_LOG_INFO("Setting up internal log-rotate-reopening signal handler.  "
                  "CAUTION!  User program MUST avoid using non-boost::asio::signal_set signal handling!  "
                  "If it does use non-boost.asio, behavior is undefined.");

    // Add the classic log-rotation-inducing signal numbers.
    m_signal_set.add(SIGHUP);

    /* At this point, receiving those signals will NOT do whatever default behavior would happen -- but I believe
     * that unlike for SIGTERM/etc. the default behavior is no-op.  Before actually reporting successful initialization
     * by exiting constructor, set up the handler that'll be called upon receiving the signals. */

    /* this->impl_log_flush_and_reopen() will be called on signal (or error).
     * Note that that function's contract (from its doc comment) is it must execute in m_async_worker (see
     * m_signal_set() init above in the init section of ctor).
     * Indeed boost::asio::io_service semantics guarantee it'll run in m_async_worker (not some
     * no-man's-land signal handler thread of execution, as one might fear could be the case) for the same reason
     * the various post()ed tasks will run in m_async_worker. */
    m_signal_set.async_wait([this](const Error_code& sys_err_code, int sig_number)
    {
      on_rotate_signal(sys_err_code, sig_number);
    });
  } // if (capture_rotate_signals_internally)
  /* else if (!capture_rotate_signals_internally)
   * {
   *   Do NOT .add() anything; and don't async_wait() anything.  As noted in comment at m_signal_set construction time,
   *   .add() does more than make it possible to .async_wait().  It also replaces any default OS or user's own
   *   non-boost.asio signal handling machinery with boost.asio's signal_set machinery.  That can be quite draconian,
   *   so user must specifically set that option to true.  If it's false (in all siblings of *this in entire  app),
   *   then whatever signal handling machinery the user wants to set up for themselves (or leave at OS's
   *   discretion) will remain undisturbed.  By the way, of course, they can use boost.asio machinery themselves too;
   *   it's just that doing so would still work even if capture_rotate_signals_internally were true, so that's
   *   not the dangerous scenario.  (Copy/paste warning: This cmnt was copy-pasted from some
   *   vaguely similar code in flow::net_flow; but the 2 modules are barely related if at all, so....)
   * } */
} // Async_file_logger::Async_file_logger()

Async_file_logger::Throttling_cfg Async_file_logger::throttling_cfg() const
{
  // m_throttling only changes in throttling_cfg() mutator which is formally not allowed to call concurrently.

  const auto throttling = m_throttling.load(std::memory_order_relaxed);
  assert(throttling && "Somehow called before ctor returned?  Bug?");
  return throttling->m_cfg; // Copy it as promised.  Note tis given m_cfg itself cannot change ever.
}

bool Async_file_logger::throttling_active() const
{
  // m_throttling_active only changes in throttling_cfg() mutator which is formally not allowed to call concurrently.

  return m_throttling_active.load(std::memory_order_relaxed);
}

void Async_file_logger::throttling_cfg(bool active, const Throttling_cfg& cfg)
{
  /* Please see Impl section of class doc header for detailed discussion; also Throttling and m_throttling_states doc
   * headers.  Then come back here.  We rely on the background in that discussion frequently.
   *
   * Couple notes:
   *   - We cannot be called concurrently with ourselves; so we can count on m_throttling_states and m_throttling
   *     not changing under us.
   *   - Generally here we use strictest (seq_cst) ordering below, but it ~doesn't really matter; just we can count
   *     on being called infrequently and thus have negligible perf impact, so might as well use the strictest
   *     available mode.
   *     - But the much-more-frequent should_log(), do_log(), really_log() use `relaxed` ordering exclusively.
   *       Much discussion about the correctness of that approach is in the aforementioned class doc header Impl
   *       section. */

  // Deal with `active`.

  /* This is orthogonal to the more-complex Throttling m_throttling* stuff below.  It is also quite simple:
   * We set it -- might as well use the strictest ordering setting, since we are rarely called -- and should_log()
   * might read it (with `relaxed` order): whether there's a tiny lag in some thread's should_log()'s load
   * versus our store = unimportant in practice. */
  const auto prev_active = m_throttling_active.exchange(active, std::memory_order_seq_cst);
  FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                "Config set: throttling feature active => [" << active << "] (was [" << prev_active << "]).");

  // Deal with `cfg`.  Note m_throttling (the atomic ptr) and m_throttling_states cannot change, until we change them.

  const auto prev_throttling_ptr = m_throttling.load(std::memory_order_seq_cst);
  assert(prev_throttling_ptr && "It was supposed to be constructed to be non-null.");

  const auto& prev_throttling = *(m_throttling.load(std::memory_order_seq_cst));
  const auto& prev_cfg = prev_throttling.m_cfg;
  if ((prev_cfg.m_hi_limit == cfg.m_hi_limit) && (prev_cfg.m_lo_limit == cfg.m_lo_limit))
  {
    /* As discussed in class doc header: no-op, unless they actually changed something; no state reset.
     * E.g., perhaps they changed `active` while passing-in `cfg = throttling_cfg()` unchanged. */
    return;
  }
  // else

  /* As discussed in doc header(s), the new state starts with the same mem-used total from
   * the previous state.  If something is being do_log()ed or really_log()ed concurrently, with previous state, then
   * we might lose a few messages' worth of memory stats; no problem. */
  const auto prev_pending_logs_sz = prev_throttling.m_pending_logs_sz.load(std::memory_order_relaxed);

  // (@todo make_unique() was complaining of lacking copy ctor; not sure why it is needed.  Not very important.)
  boost::movelib::unique_ptr<Throttling> new_throttling
                                           (new Throttling
                                                  {
                                                    cfg, // Copy-in the new config which is different.
                                                    prev_pending_logs_sz,
                                                    // Most importantly cleanly initialize m_throttling_now.
                                                    prev_pending_logs_sz
                                                      >= static_cast<decltype(prev_pending_logs_sz)>(cfg.m_hi_limit)
                                                  });

  FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                "Config set: hi_limit [" << cfg.m_hi_limit << "]; lo_limit [" << cfg.m_lo_limit << "].  "
                "New throttling-algorithm initial state: "
                "mem-use = [" << prev_pending_logs_sz << "]; "
                "thottling? = [" << new_throttling->m_throttling_now << "].  "
                "Throttling feature active? = [" << active << "].  "
                "Reminder: `throttling?` shall only be used if `throttling feature active?` is 1.");

  /* Now switch-over the state machine to the new state.  Concurrent do_log(), really_log(), should_log() will
   * return soon, and the old state won't have any effect anymore. */
  m_throttling.store(new_throttling.get(), std::memory_order_seq_cst);

  /* We keep the old state around, indefinitely, for formal safety.  (As discussed in m_throttling_states
   * doc header, we could use atomic shared_ptr m_throttling instead -- with different trade-offs.) */
  m_throttling_states.emplace_back(std::move(new_throttling));

  /* There is a minor corner case which is the reason m_pending_logs_sz has a signed type instead of unsigned.
   * It can occur from the point above where we grab the previous *m_throttling's m_pending_logs_sz.
   * Suppose, say, it is 0 at that point, and a concurrent do_log() executes, adding 100.  Then we
   * replace m_throttling with our new_throttling.  Next, really_log() executes and subtracts 100 (because
   * that message is actually logged to file).  We "lost" the do_log()'s contribution to the stats, which
   * we have already accepted as no big deal in and of itself; it's just a bit of "drift" versus reality.
   * However the really_log() would operate on the new *m_throttling (our *new_throttling); hence the `-= 100`
   * would be off 0 and hence turn negative.  It doesn't affect in practice the efficacy of the algorithm:
   * our estimate of memory use is fine in bulk.  We do need to allow for negative values because of this.
   *
   * Such "drift" can also occur in the other direction, so that we "lose" a really_log()'s contribution to the
   * total, which means m_pending_logs_sz cannot get back down to 0 ever.  Again we don't mind this slight
   * inaccuracy.  That said there's technically a formal possibility that enough drifting like this can
   * eventually approach the limit m_hi_limit.  In practice it would take a huge number of config updates --
   * with very unlucky concurrent drift events that don't balance each other out at that -- for this to become
   * a factor.  @todo Still, it's a bit unpleasant from a purist's point of view.  Revisit to look into a perfect
   * fix.  Be very careful though: the current algorithm works because of the strict ordering achieved even
   * with mere `relaxed` access, due to atomic += and -= operations that depend on each other.  Any attempt to,
   * say, assign a "fixed" value to m_pending_logs_sz based on some kind of `if` will likely break this paradigm. */
} // Async_file_logger::throttling_cfg()

Async_file_logger::~Async_file_logger() // Virtual.
{
  using async::Synchronicity;

  FLOW_LOG_INFO("Async_file_logger [" << this << "]: Deleting.  Worker thread will flush "
                "output if possible; then we will proceed to shut down.");

  /* Could do this from current thread (in fact if we simply deleted the following statement, that is what would
   * happen); that's still non-concurrent with other calls, as Serial_file_logger requires.
   * I (ygoldfel) have a soft spot for being able to say only 1 thread touches the file after ctor; maybe that's
   * silly.  Not a to-do to change it IMO. */
  m_async_worker.post([this]() { m_serial_logger.reset(); },
                      Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);

  // Now thread will exit and be joined, etc.
}

void Async_file_logger::on_rotate_signal(const Error_code& sys_err_code, int sig_number)
{
  // We are in m_async_worker thread.

  if (sys_err_code == boost::asio::error::operation_aborted)
  {
    return; // Stuff is shutting down; just get out.
  }
  // else

  FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                "Internal log-rotate handler executed with signal number [" << sig_number << "].");

  if (sys_err_code)
  {
    // This is odd, but there's no need to freak out about anything else.  Just log and get out.
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
    FLOW_LOG_WARNING("Async_file_logger [" << this << "]: "
                     "Internal signal handler executed with an error indicator.  Strange!  "
                     "Ignoring and continuing other operation.");
  }
  else
  {
    m_serial_logger->log_flush_and_reopen();
  }

  // Wait for it again, or else a rotation would only occur on the first signal but not thereafter.
  m_signal_set.async_wait([this](const Error_code& sys_err_code, int sig_number)
  {
    on_rotate_signal(sys_err_code, sig_number);
  });
} // Async_file_logger::on_rotate_signal()

void Async_file_logger::do_log(Msg_metadata* metadata, util::String_view msg) // Virtual.
{
  using util::String_view;

  assert(metadata);

  /* Our essential task is to push the log-request onto the m_async_worker queue; the code for processing
   * that log-request, once the queue gets to it in that worker thread, is called really_log() and is in here too.
   *
   * First, though, let's tally up the stats and otherwise proceed with the throttling algorithm.
   * Please see Impl section of class doc header for detailed discussion.  Then come back here.
   * We rely on the background in that discussion frequently.
   *
   * Reminder: We should be careful to minimize computation in this section (but nowhere near as important to do
   * compared to should_log()). */

  bool throttling_begins; // Will be true if and only if m_pending_logs_sz increment passed m_cfg.m_hi_limit.
  auto& throttling = *(m_throttling.load(std::memory_order_relaxed));
  /* @todo This should work according to standard/cppreference.com, but at least some STLs lack it.  Revisit.
   * using logs_sz_t = decltype(throttling.m_pending_logs_sz)::value_type; */
  using logs_sz_t = int64_t;
  const auto limit = static_cast<logs_sz_t>(throttling.m_cfg.m_hi_limit);
  const auto logs_sz = static_cast<logs_sz_t>(mem_cost(metadata, msg));
  const auto prev_pending_logs_sz
    = throttling.m_pending_logs_sz.fetch_add(logs_sz, std::memory_order_relaxed);
  const auto pending_logs_sz = prev_pending_logs_sz + logs_sz;
  if ((throttling_begins = ((pending_logs_sz >= limit) && (prev_pending_logs_sz < limit))))
  {
    /* Flip m_throttling_now.  Do not assign `true`, to avoid formal reordering danger -- explain in aforementioned
     * doc header Impl section. */
    throttling.m_throttling_now.fetch_xor(1, std::memory_order_relaxed);
  }

  /* Done! State updated, and throttling_begins determined for really_log().
   *
   * Now for the enqueueing of the log-request. */

  /* Key points about how the asynchronicity/logging/queueing works: We aim to return synchronously ASAP and leave the
   * potentially blocking (like if hard drive has to turn on from sleep) I/O ops to the worker thread asynchronously.
   * There are 2 pieces of data, *metadata and `msg`, to deal with, both of which essentially must be available when
   * we in fact write to file via m_serial_logger.  By contract of do_log() and logs_asynchronously()==true:
   *   - We are to NOT copy *metadata; and therefore we are to `delete metadata` when done with it; i.e., after
   *     m_serial_logger->do_log() returns having written to file-system.
   *   - We MUST copy `msg` so we can use it asynchronously; we are to therefore free the copy ourselves once
   *     m_serial_logger->do_log() returns.  (In reality this is all because `msg` is a shared thread-lcl thing that is
   *     reused in every subsequent and preceding FLOW_LOG_WARNING/etc. invocation in this thread.  We technically don't
   *     know or care about that reality in here; only the *result* wherein we cannot use `msg` after do_log()
   *     synchronously returns.) */

  /* The other thing going on here: We need to capture a copy of `msg` in some form in the lambda post()ed below.
   * The native way would be to perhaps create a `string`, or `vector<char>`, or util::Basic_blob, but this
   * code is executed quite frequently -- so we'd like to minimize processor use -- and perhaps more importantly
   * in very heavy logging conditions we want to minimize the memory used by this copy.  This means:
   *   - guarantee allocating exactly msg.size() bytes -- no overhead (there's no formal guarantee string
   *     or vector ctor will do that, though Basic_blob does);
   *   - do not store extra members.  string and vector store an m_capacity but also m_size; but for our purposes
   *     (since the copy is never modified in any way; it is just logged and destroyed) m_size is extra.
   *     Basic_blob similarly does that, plus it has an extra feature for which it stores an extra size_t m_start.
   *   - The object needs to have RAII semantics, meaning if it is destroyed, then the allocated buffer is
   *     deallocated.  (So we can't just store a raw `char*`; need smart pointer ultimately to ensure deletion even
   *     if lambda is never executed but is instead destroyed first.)
   * So we use a custom little thing called Tight_blob.
   * (@todo Take those parts of Basic_blob and make a util::Tight_blob; then write Basic_blob in terms of Tight_blob.
   * Then it'll be reusable.)
   * Tight_blob is straightforward, and really we'd like to just capture simply a unique_ptr and m_size, in which
   * case Tight_blob wouldn't even be needed; but a peculiarity of std::function prevents this:
   *   - Any captured type must be *copyable*, even though this ability is never exercised.  So capturing unique_ptr
   *     would not compile: it has no copy ctor.  shared_ptr works great, but it does store a control block; we're
   *     going for absolute minimum memory overhead.
   *   - So we do write this little Tight_blob and outfit it with a functioning copy ctor -- but remember, it won't
   *     really be called. */

  auto really_log
    = [this,
       /* We could just capture `metadata` and `delete metadata` in the lambda {body};
        * in fact we could do similarly with a raw `char* msg_copy` too.  However then they can leak
        * if *this is destroyed before the lambda body has a chance to execute.  Whereas
        * by wrapping them in unique_ptr<>s we get RAII deletion with ~no memory cost (just a pointer
        * copy from `metadata` into m_data; sizeof(m_data) == sizeof(Msg_metadata*)). */
       mdt_wrapper = Mdt_wrapper{metadata},
       msg_copy_blob = Tight_blob{msg.data(), msg.size()},
       throttling_begins]() mutable
  {
    const auto metadata = mdt_wrapper.data();
    const String_view msg{msg_copy_blob.data(), msg_copy_blob.size()};

    /* Throttling: do, essentially, the opposite of what do_log() did when issuing the log-request.
     * Again please refer to Impl section of class doc header for reasoning about this algorithm. */

    auto& throttling = *(m_throttling.load(std::memory_order_relaxed));
    const auto& cfg = throttling.m_cfg;
    const auto limit = static_cast<logs_sz_t>(cfg.m_lo_limit);
    const auto logs_sz = static_cast<logs_sz_t>(mem_cost(metadata, msg));
    // @todo ^-- Maybe instead save+capture this in do_log()?  Trade-off is RAM (currently favoring it) vs cycles.
    const auto prev_pending_logs_sz
      = throttling.m_pending_logs_sz.fetch_sub(logs_sz, std::memory_order_relaxed);
    const auto pending_logs_sz = prev_pending_logs_sz - logs_sz;
    if ((pending_logs_sz <= limit) && (prev_pending_logs_sz > limit))
    {
      /* Flip m_throttling_now.  Do not assign `false`, to avoid formal reordering danger -- explained in aforementioned
       * doc header Impl section. */
      throttling.m_throttling_now.fetch_xor(1, std::memory_order_relaxed);

      // Performance in this block is not of huge import; this is a fairly rare event.
      FLOW_LOG_SET_CONTEXT(m_serial_logger.get(), this->get_log_component());

      FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                    "The following message, when its log-request was dequeued (now), caused pending-logs RAM usage "
                    "to go below configured lo_limit.  If throttling feature was active, preceding messages were "
                    "likely dropped starting at the point in time where the system had reached hi_limit.  "
                    "A message should have appeared earlier to indicate that point in the log-request queue.  "
                    "Current state follows (but beware concurrency; this is an informational snapshot only): "
                    "Config: hi_limit [" << cfg.m_hi_limit << "]; lo_limit [" << cfg.m_lo_limit << "].  "
                    "mem-use = [" << pending_logs_sz << "]; "
                    "thottling? = 1 (see above).  "
                    "Throttling feature active? = [" << m_throttling_active.load(std::memory_order_relaxed) << "].  "
                    "Reminder: `throttling?` shall only be used if `throttling feature active?` is 1.");
    }

    /* We are in m_async_worker thread, as m_serial_logger requires.
     * mdt_wrapper (effectively `*metadata`) and msg_copy_blob (copy of `msg`) are to be freed when done. */
    m_serial_logger->do_log(metadata, msg);

    // Oh and obey throttling_begins for this log-request, if it was computed to be true in do_log().
    if (throttling_begins)
    {
      // Performance in this block is not of huge import; this is a fairly rare event.
      FLOW_LOG_SET_CONTEXT(m_serial_logger.get(), this->get_log_component());

      FLOW_LOG_WARNING("Async_file_logger [" << this << "]: "
                       "The preceding message, when its log-request was enqueued, caused pending-logs RAM usage "
                       "to exceed configured hi_limit.  If throttling feature was active, subsequent messages-to-be "
                       "were likely dropped (never enqueued), until the system processed the backlog to get back to "
                       "lo_limit.  A message should appear later to indicate that point in the log-request queue.  "
                       "Current state follows (but beware concurrency; this is an informational snapshot only): "
                       "Config: hi_limit [" << cfg.m_hi_limit << "]; lo_limit [" << cfg.m_lo_limit << "].  "
                       "Mem-use = [" << pending_logs_sz << "]; "
                       "thottling? = 0 (see above).  "
                       "Throttling feature active? = [" << m_throttling_active.load(std::memory_order_relaxed) << "].  "
                       "Reminder: `throttling?` shall only be used if `throttling feature active?` is 1.");
    } // if (throttling_begins)
  }; // really_log() =

  // Enqueue it, after whatever others are already pending (hopefully not too many; ideally none).
  m_async_worker.post(std::move(really_log));
} // Async_file_logger::do_log()

size_t Async_file_logger::mem_cost(const Msg_metadata* metadata, util::String_view msg) // Static.
{
  /* We should strive to be quick here (also almost certainly we will be inlined, with full optimization anyway).
   * This is called in every do_log(), which can be non-infrequent; and really_log() in the background thread --
   * though extreme efficiency there is less important.
   *
   * This is an estimate; it need not be exact, as we use it as merely a heuristic when to throttle.  That said
   * it should be roughly proportional to the memory used. */

#if (!defined(__GNUC__)) || (!defined(__x86_64__))
#  error "An estimation trick below has only been checked with x64 gcc and clang.  Revisit code for other envs."
#endif

  const auto call_thread_nickname_sz = metadata->m_call_thread_nickname.size();
  return msg.size() // Presumably the biggest/most important.
         // The lambda captures.
         + sizeof(Mdt_wrapper) + sizeof(Tight_blob) + sizeof(bool) + sizeof(Async_file_logger*)
         /* Mdt_wrapper = Msg_metadata contains `std::string m_call_thread_nickname`.
          * If the thread nickname exists and is long enough to not fit inside the std::string object itself
          * (common optimization in STL: Small String Optimization), then it'll allocate a buffer in heap.
          * We could even determine whether it actually happened here at runtime, but that wastes cycles.
          * Instead we've established experimentally that with default STL and clangs 4-17 and gccs 5-13
          * SSO is active for .size() <= 15.  @todo Check LLVM libc++ too.  Probably similar. */
         + ((call_thread_nickname_sz <= 15) ? 0 : call_thread_nickname_sz);
} // Async_file_logger::mem_cost()

void Async_file_logger::log_flush_and_reopen(bool async)
{
  using async::Synchronicity;

  // We are in some unspecified user thread that isn't m_async_worker.

  FLOW_LOG_INFO("Async_file_logger [" << this << "]: Non-worker (user) thread "
                "requested [" << (async ? "asynchronous" : "synchronous") << "] file flush/close (if needed) and "
                "re-open, such as for rotation or at initialization.");

  m_async_worker.post([this]() { m_serial_logger->log_flush_and_reopen(); },
                      async ? Synchronicity::S_ASYNC : Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
}

bool Async_file_logger::should_log(Sev sev, const Component& component) const // Virtual.
{
  return m_serial_logger->should_log(sev, component) // In normal conditions this is likeliest to return false.
         &&
         /* As explained in doc header (please see there for discussion), throttling -- if on -- can prevent logging.
          * It is important that the following code is as fast as possible, though by placing it below the above
          * forwarded should_log() check we've performed a key optimization already, as in a properly configured
          * system verbosity knobs should throw out most messages-to-be. */
         ((!m_throttling_active.load(std::memory_order_relaxed)) // Whether to even consult throttling algo.
          || // Whether throttling algo is currently saying we should drop incoming messages-to-be.
          (!m_throttling.load(std::memory_order_relaxed)->m_throttling_now.load(std::memory_order_relaxed)));
}

bool Async_file_logger::logs_asynchronously() const // Virtual.
{
  return true;
}

} // namespace flow::log
