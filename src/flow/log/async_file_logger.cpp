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

// Implementations.

Async_file_logger::Async_file_logger(Logger* backup_logger_ptr,
                                     Config* config, const fs::path& log_path,
                                     bool capture_rotate_signals_internally) :
  /* Set up the logging *about our real attempts at file logging*, probably to a Simple_ostream_logger/cout;
   * or to null (meaning no such logging).  Either way we will still attempt to actually log user-requested messages
   * to log_path! */
  Log_context(backup_logger_ptr, Flow_log_component::S_LOG),
  m_config(config), // Save pointer, not copy, of the config given to us.  Hence thread safety is a thing.

  /* Set up throttling algorithm, which is always-on (see class doc header discussion).
   * Reminder that its output will be ignored, until/unless user calls throttling_cfg(true, ...).  They can then
   * set their own config values as well (or reuse this default which they can access via throttling_cfg() accessor). */
  m_throttling_cfg({ Throttling_cfg::S_HI_LIMIT_DEFAULT }),
  m_pending_logs_sz(0), // No memory used yet by pending messages.
  m_throttling_now(false), // No throttling yet.
  m_throttling_active(false),

  // Any I/O operations done here are the only ones not done from m_async_worker thread.
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
  Lock_guard lock(m_throttling_mutex);
  return m_throttling_cfg;
}

bool Async_file_logger::throttling_active() const
{
  return m_throttling_active.load(std::memory_order_relaxed);
}

void Async_file_logger::throttling_cfg(bool active, const Throttling_cfg& cfg)
{
  assert((cfg.m_hi_limit > 0) && "Per contract, hi_limit must be positive.");

  /* Please see Impl section of class doc header for detailed discussion; also Throttling and m_throttling_states doc
   * headers.  We just reiterate here that m_throttling_active and m_throttling_cfg are orthogonal to each other;
   * the latter is thus protected by mutex, while the former is atomic. */

  // Deal with `active`.

  const auto prev_active = m_throttling_active.exchange(active, std::memory_order_relaxed);
  if (prev_active != active)
  {
    FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                  "Config set: throttling feature active? [" << prev_active << "] => [" << active << "].");
  }

  // Deal with `cfg`.

  { // All this->m_ touched in { here } can concurrently change, unless we lock.
    Lock_guard lock(m_throttling_mutex);

    if (m_throttling_cfg.m_hi_limit != cfg.m_hi_limit)
    {
      const auto prev_throttling_now
        = m_throttling_now.exchange(m_pending_logs_sz >= static_cast<decltype(m_pending_logs_sz)>(cfg.m_hi_limit),
                                    std::memory_order_relaxed);
      // (m_throttling_now cannot change during lock; using .exchange() purely for convenience to grab prev value.)

      FLOW_LOG_INFO("Async_file_logger [" << this << "]: Config set: "
                    "hi_limit [" << m_throttling_cfg.m_hi_limit << "] => [" << cfg.m_hi_limit << "].  "
                    "Mem-use = [" << m_pending_logs_sz << "]; "
                    "throttling? = [" << prev_throttling_now << "] => [" << m_throttling_now << "]; "
                    "throttling feature active? = [" << active << "].  "
                    "Reminder: `throttling?` shall only be used if `throttling feature active?` is 1.");

      m_throttling_cfg.m_hi_limit = cfg.m_hi_limit;
    }
    /* else: As discussed in class doc header: no-op, unless they actually changed something; no state reset.
     * E.g., perhaps they changed `active` while passing-in `cfg = throttling_cfg()` unchanged. */
  } // Lock_guard lock(m_throttling_mutex);
} // Async_file_logger::throttling_cfg()

Async_file_logger::~Async_file_logger() // Virtual.
{
  using async::Synchronicity;

  {
    Lock_guard lock(m_throttling_mutex); // Careful: really_log()s may well be happening right now via m_async_worker.

    FLOW_LOG_INFO("Async_file_logger [" << this << "]: Deleting.  Worker thread will flush "
                  "output if possible; then we will proceed to shut down.  Current mem-use of queued "
                  "log-requests is [" << m_pending_logs_sz << "]; if it is large, this might take some time.");
  }

  /* Could do this from current thread (in fact if we simply deleted the following statement, that is what would
   * happen); that's still non-concurrent with other calls, as Serial_file_logger requires.
   * However there is a subtle and important reason why the following is currently actively desired:
   * By post()ing it -- meaning enqueuing it -- after any log-requests (function objects with a Log_request
   * capture in each), and then awaiting the completion, we ensure those log requests are first serviced,
   * or more in English: any pending log messages are actually written to file.
   *
   * Were we to not do so, some aspects of Log_request would be leaked here; as of this writing the message and
   * the metadata.  (They are not RAII for reasons explained in Log_request doc header.)
   *
   * Note: If we wanted to avoid this behavior -- e.g., if huge amounts of log-requests have been queued, this
   * statement can very much take quite a while, and maybe we want that, or maybe we don't -- and wanted to
   * avoid a leak, we'd need to come up with some new trick.  For the time being though by doing this flush
   * we both get desired functional behavior and avoid the leak. */
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
  using std::memcpy;

  assert(metadata);

  /* Our essential task is to push the log-request onto the m_async_worker queue; the code for processing
   * that log-request, once the queue gets to it in that worker thread, is called really_log() and is in here too.
   *
   * Key points about how the asynchronicity/logging/queueing works: We aim to return synchronously ASAP and leave the
   * potentially blocking (like if hard drive has to turn on from sleep) I/O ops to the worker thread asynchronously.
   * There are 2 pieces of data, *metadata and `msg`, to deal with, both of which essentially must be available when
   * we in fact write to file via m_serial_logger.  By contract of do_log() and logs_asynchronously()==true:
   *   - We are to NOT copy *metadata; and therefore we are to `delete metadata` when done with it; i.e., after
   *     m_serial_logger->do_log() returns having written to file-system.
   *   - We MUST copy `msg` so we can use it asynchronously; we are to therefore free the copy ourselves once
   *     m_serial_logger->do_log() returns.  (In reality this is all because `msg` is a shared thread-lcl thing that is
   *     reused in every subsequent and preceding FLOW_LOG_WARNING/etc. invocation in this thread.  We technically don't
   *     know or care about that reality in here; only the *result* wherein we cannot use `msg` after do_log()
   *     synchronously returns.)
   *
   * Here's the Log_request encapsulating that stuff.  See Log_request doc header for some nitty-gritty details. */
  const auto msg_sz = msg.size();
  Log_request log_request{ new char[msg_sz], msg_sz, metadata,
                           false }; // We can't know m_throttling_begins yet.
  memcpy(log_request.m_msg_copy, msg.data(), msg_sz);

  /* Before enqueuing that stuff, though, let's tally up the stats and otherwise proceed with the throttling algorithm.
   * Please see Impl section of class doc header for detailed discussion.  Then come back here.
   * We rely on the background in that discussion frequently.
   *
   * Reminder: We should be careful to minimize computation in this section (but nowhere near as important to do so
   * compared to should_log()).  Similarly keep the locked section as small as possible. */

  using logs_sz_t = decltype(m_pending_logs_sz);
  const auto logs_sz = mem_cost(log_request);
  auto& throttling_begins = log_request.m_throttling_begins;
  logs_sz_t limit;
  logs_sz_t pending_logs_sz; // For logging.
  logs_sz_t prev_pending_logs_sz;
  {
    Lock_guard lock(m_throttling_mutex);
    limit = static_cast<logs_sz_t>(m_throttling_cfg.m_hi_limit);
    prev_pending_logs_sz = m_pending_logs_sz;
    pending_logs_sz = (m_pending_logs_sz += logs_sz);
    /* Minor/subtlety: We could also remove the m_throttling_now check here and instead use
     * `throttling_begins = (m_throttling_now.exchange() == false)` in { body }.  However we already had to lock
     * mutex for ~unrelated reasons, and therefore m_throttling_now cannot change at the moment; so we
     * might as well gate the integer comparisons on its value -- which rarely changes -- and only assign
     * it, if its value would in fact change. */
    if ((!m_throttling_now.load(std::memory_order_relaxed))
        && (pending_logs_sz >= limit) && (prev_pending_logs_sz < limit))
    {
      m_throttling_now.store(true, std::memory_order_relaxed);
      throttling_begins = true;
    }
  }
  if (throttling_begins)
  {
    // Log about it in the backup Logger (Logger-about-logging).
    FLOW_LOG_WARNING("Async_file_logger [" << this << "]: "
                     "do_log() throttling algorithm: a message reached hi_limit; next message-to-be => likely dropped, "
                     "if feature active.  Config: hi_limit [" << limit << "].  "
                     "Mem-use = [" << prev_pending_logs_sz << "] => [" << pending_logs_sz << "]; "
                     "throttling? = 1 (see above); "
                     "throttling feature active? = [" << m_throttling_active.load(std::memory_order_relaxed) << "].  "
                     "Reminder: `throttling?` shall only be used if `throttling feature active?` is 1.  "
                     "Limit-triggering message's contents follow: [" << msg << "].");
  }
#if 1 // XXX Obv change to `if 1` if debugging + want to see it.  Could just use TRACE but avoiding should_log() cost.
  else
  {
    FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                  "do_log() throttling algorithm: a message was processed; situation (reminder: beware concurrency): "
                  "Config: hi_limit [" << limit << "].  "
                  "Mem-use = [" << prev_pending_logs_sz << "] => [" << pending_logs_sz << "]; "
                  "throttling feature active? = [" << m_throttling_active.load(std::memory_order_relaxed) << "].  "
                  "Message's contents follow: [" << msg << "].");
  }
#endif

  /* Done! State updated, and throttling_begins determined for really_log().
   *
   * Now for the enqueueing of the log-request.  This is what will execute in m_async_worker, when it's its turn. */

  auto really_log = [this, log_request = std::move(log_request)]() mutable
  {
    const auto metadata = log_request.m_metadata;
    const String_view msg{log_request.m_msg_copy, log_request.m_msg_size};

    /* Throttling: do, essentially, the opposite of what do_log() did when issuing the log-request.
     * Again please refer to Impl section of class doc header for reasoning about this algorithm. */

    const auto logs_sz = mem_cost(log_request);
    // @todo ^-- Maybe instead save this in Log_request?  Trade-off is RAM vs cycles (currently favoring RAM).
    bool throttling_ends = false;
    logs_sz_t limit; // For logging.
    logs_sz_t pending_logs_sz; // For logging.
    logs_sz_t prev_pending_logs_sz;
    {
      Lock_guard lock(m_throttling_mutex);
      limit = m_throttling_cfg.m_hi_limit; // Just for logging in this case.
      prev_pending_logs_sz = m_pending_logs_sz;
      assert((prev_pending_logs_sz >= logs_sz) && "Bug?  really_log() has no matching do_log()?");
      pending_logs_sz = (m_pending_logs_sz -= logs_sz);
      /* Minor/subtlety: Versus the do_log() equivalent above, it looks more economical to check pending_logs_sz first,
       * since it just compares one value to 0 in this case (as opposed to 2 values and < and >=), and then both set
       * m_throttling_now and check its preceding value in one op. */
      if (pending_logs_sz == 0)
      {
        // m_throttling_now should be false; but detect whether this is the change-over from true as opposed to no-op.
        throttling_ends = (m_throttling_now.exchange(false, std::memory_order_relaxed) == true);
      }
    }

    if (throttling_ends)
    {
      // Log about it in the backup Logger (Logger-about-logging).
      FLOW_LOG_INFO("Async_file_logger [" << this << "]: last pending message was logged; "
                    "next message-to-be => likely first one to *not* be dropped, if throttling feature active.  "
                    "Config: hi_limit [" << limit << "].  "
                    "Mem-use = [" << prev_pending_logs_sz << "] => [" << pending_logs_sz << "]; "
                    "throttling? = 0 (see above); "
                    "throttling feature active? = [" << m_throttling_active.load(std::memory_order_relaxed) << "].  "
                    "Reminder: `throttling?` shall only be used if `throttling feature active?` is 1.  "
                    "Queue-clearing message's contents follow: [" << msg << "].");

      // Log about it in file itself.  (Performance in this block is not of huge import; this is a fairly rare event.)
      FLOW_LOG_SET_CONTEXT(m_serial_logger.get(), this->get_log_component());

      FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                    "really_log() throttling algorithm: last pending message was logged; "
                    "next message-to-be => likely first one to *not* be dropped, if throttling feature active.  "
                    "Config: hi_limit [" << limit << "].  "
                    "Mem-use = [" << prev_pending_logs_sz << "] => 0; "
                    "throttling feature active? = [" << m_throttling_active.load(std::memory_order_relaxed) << "].  "
                    "Queue-clearing message is the one immediately following me in file.  "
                    "Compare its time stamp to mine to see time lag due to queueing.");
    }
#if 0 // Obv change to `if 1` if debugging + want to see it.  Could just use TRACE but avoiding should_log() cost.
    else
    {
      FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                    "really_log() throttling algorithm: a message is about to be written to file; "
                    "situation (reminder: beware concurrency): Config: hi_limit [" << limit << "].  "
                    "Mem-use = [" << prev_pending_logs_sz << "] => [" << pending_logs_sz << "]; "
                    "throttling feature active? = [" << m_throttling_active.load(std::memory_order_relaxed) << "].  ");
                    "Message's contents follow: [" << msg << "].");
    }
#endif

    // We are in m_async_worker thread, as m_serial_logger requires.  Go!
    m_serial_logger->do_log(metadata, msg);

    // Oh and obey throttling_begins for this log-request, if it was computed to be true in do_log().
    if (log_request.m_throttling_begins)
    {
      // Performance in this block is not of huge import; this is a fairly rare event.
      FLOW_LOG_SET_CONTEXT(m_serial_logger.get(), this->get_log_component());

      FLOW_LOG_WARNING("Async_file_logger [" << this << "]: "
                       "really_log() throttling algorithm: The preceding message, when its log-request was "
                       "earlier enqueued, caused pending-logs RAM usage to exceed then-configured hi_limit.  "
                       "If throttling feature was active, subsequent messages-to-be (log-requests) were dropped.  "
                       "We only just got around to being able to log it (satisfy log-request) after all the "
                       "preceding ones in FIFO order.  Nowadays: Config: hi_limit [" << limit << "].  "
                       "Mem-use = [" << prev_pending_logs_sz << "] => [" << pending_logs_sz << "]; "
                       "throttling feature active? = [" << m_throttling_active.load(std::memory_order_relaxed) << "].  "
                       "Limit-crossing (in the past) message is the one immediately preceding the current one "
                       "you're reading in file.  "
                       "Compare its time stamp to mine to see time lag due to queueing.");
    } // if (log_request.throttling_begins)

    /* Last but not least, as discussed in Log_request doc header, we must do this explicitly.
     * To reiterate: as of this writing, if this lambda body is not actually executed, then these will leak.
     * As of this writing we ensure it *is* executed, in the Async_file_logger dtor at the latest. */
    delete[] log_request.m_msg_copy;
    delete metadata;
  }; // really_log =

  // Enqueue it, after whatever others are already pending (hopefully not too many; ideally none).
  m_async_worker.post(std::move(really_log));
} // Async_file_logger::do_log()

size_t Async_file_logger::mem_cost(const Log_request& log_request) // Static.
{
  /* We should strive to be quick here (also almost certainly we will be inlined, with full optimization anyway).
   * This is called in every do_log(), which can be not-infrequent; and really_log() in the background thread --
   * though extreme efficiency there is less important.
   *
   * This is an estimate; it need not be exact, as we use it as merely a heuristic when to throttle.  For example
   * personally I (ygoldfel) only vaguely understand how function<> stores stores a lambda and its captures.
   * That said it should be roughly proportional to the memory used. */

  return sizeof(async::Task) // Don't forget the function object itself.  Then the captures:
         + sizeof(Async_file_logger*) // `this`.
         + sizeof(Log_request) // The main capture is this.  Firstly its shallow size.
         + deep_size(log_request); // And its size beyond sizeof(its members combined).

  /* Style/maintanability notes: At the risk of being overly formal: There's a pattern in play here:
   * The total-size of aggregate object `X x` is sizeof(X) + D, where D is its mem-use beyond the shallow object.
   * To compute D, take each member `Y m_y` of X; obtain its mem-use beyond sizeof(Y); sum these values to get D.
   * Namely for each m_y:
   *   - It might have no non-shallow mem-use (e.g., a `float`).  0 then.
   *   - It may itself be an aggregate object or pointer to one in which case:
   *     - If there is no deep_size() function that takes `const Y&` (or similar), it has no non-shallow mem-use.  0.
   *     - If there is, call it for the result, on the member or member's pointee.
   *       Then that deep_size() should itself follow the present pattern.
   *     - *Either way*: If it's a pointer, you probably must also add sizeof(pointee type).  Do not forget!
   *   - Its mem-use might be stored in a nearby data member; e.g., `Y* m_y; size_t m_y_size; `.  That's the result.
   *
   * So in our case you see, when we got to Log_request, we took its sizeof() and then added its deep_size()
   * which does exist, because Log_request does have at least 1 member that has non-shallow mem-use.
   *
   * Rationale: Keep things organized; provide reusable deep_size() APIs where applicable.
   *
   * Why not include sizeof() in deep_size()?  Answer: Consider
   *   struct X { struct { int m_a; float m_b; } m_c; bool m_d; };  X x{ ... };
   * Its mem-use is simply sizeof(X).  So we get the sizeof() part, which is mandatory, for "free," without having
   * to error-pronely enumerate members at various levels, plus the boiler-plate of needing deep_size() at each level;
   * and so on.  So it makes sense to separate the sizeof() part -- executed once at the top level -- and the
   * deep_size() part which is potentially recursive but only needs to explicitly define deep_size() for those with
   * non-zero non-shallow mem-use; and only need to mention such members (and no others) in mem-use formulas.
   *
   * @todo We could probably get fancier with it, like having a deep_size<T>(const T* t) specialization that would
   * forward to `sizeof T + deep_size(*t)` if it exists... or something like that.  For now seems like overkill. */
} // Async_file_logger::mem_cost()

size_t Async_file_logger::deep_size(const Log_request& val) // Static.
{
  // We're following the loose pattern explained at the end of Async_file_logger::mem_cost().

  using log::deep_size;

  return val.m_msg_size // m_msg_copy = char*; we saved its pointee raw array's mem-use here.
         // m_metadata is Msg_metadata*:
         + sizeof(Msg_metadata) // Msg_metadata shallow (to it) parts are non-shallow to *us* (it's in heap).
         + deep_size(*val.m_metadata); // Msg_metadata's non-shallow mem-use (recursively implement deep_size() ptrn).
}

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
  if (!m_serial_logger->should_log(sev, component)) // In normal conditions this is likeliest to return false.
  {
    return false;
  }
  // else

  /* As explained in doc header (please see there for discussion), throttling -- if on -- can prevent logging.
   * It is important that the following code is as fast as possible, though by placing it below the above
   * forwarded should_log() check we've performed a key optimization already, as in a properly configured
   * system verbosity knobs should throw out most messages-to-be. */

  if (!m_throttling_active.load(std::memory_order_relaxed))
  {
    return true;
  }
  // else

  const auto throttled = m_throttling_now.load(std::memory_order_relaxed);

#if 0 // Obv change to `if 1` if debugging + want to see it.  Could just use TRACE but avoiding should_log() cost.
  FLOW_LOG_INFO("Async_file_logger [" << this << "]: "
                "should_log(sev=[" << sev << "]; component=[" << component.payload_enum_raw_value() << "]) "
                "throttling algorithm situation (reminder: beware concurrency): "
                "Throttling feature active? = 1; throttling? = [" << throttled << "].");
#endif

  return !throttled;
} // Async_file_logger::should_log()

bool Async_file_logger::logs_asynchronously() const // Virtual.
{
  return true;
}

} // namespace flow::log
