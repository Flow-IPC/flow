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
  using boost::asio::const_buffer;

  assert(metadata);

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

  m_async_worker.post([this,
                       /* We could just capture `metadata` and `delete metadata` in the lambda {body};
                        * in fact we could do similarly with a raw `char* msg_copy` too.  However then they can leak
                        * if *this is destroyed before the lambda body has a chance to execute.  Whereas
                        * by wrapping them in unique_ptr<>s we get RAII deletion with ~no memory cost (just a pointer
                        * copy from `metadata` into m_data; sizeof(m_data) == sizeof(Msg_metadata*)). */
                       mdt_wrapper = Mdt_wrapper{metadata},
                       msg_copy_blob = Tight_blob{msg.data(), msg.size()}]() mutable
  {
    /* We are in m_async_worker thread, as m_serial_logger requires.
     * mdt_wrapper (effectively `*metadata`) and msg_copy_blob (copy of `msg`) are to be freed when done. */
    m_serial_logger->do_log(mdt_wrapper.data(),
                            String_view(msg_copy_blob.data(), msg_copy_blob.size()));
  }); // m_async_worker.post()
} // Async_file_logger::do_log()

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
  return m_serial_logger->should_log(sev, component);
}

bool Async_file_logger::logs_asynchronously() const // Virtual.
{
  return true;
}

} // namespace flow::log
