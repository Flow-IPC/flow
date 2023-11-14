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

#include "flow/log/log.hpp"
#include "flow/log/detail/log_fwd.hpp"
#include "flow/async/single_thread_task_loop.hpp"
#include "flow/util/util_fwd.hpp"
#include <boost/asio.hpp>
#include <boost/move/unique_ptr.hpp>

namespace flow::log
{

// Types.

/**
 * An implementation of Logger that logs messages to a given file-system path but never blocks any logging thread
 * for file I/O; suitable for heavy-duty file logging.  Protects against garbling due to simultaneous logging from
 * multiple threads.
 *
 * For file logging, the two out-of-the-box `Logger`s currently suitable are this Async_file_logger and
 * Simple_ostream_logger.  Vaguely speaking, Simple_ostream_logger is suitable for console (`cout`, `cerr`) output;
 * and, in a pinch outside of a heavy-duty production/server environment, for file (`ofstream`) output.  For heavy-duty
 * file logging one should use this Async_file_logger.  The primary reason is performance; this is discussed
 * in the Logger class doc header; note Async_file_logger logs asynchronously.
 * A secondary reason is additional file-logging-specific utilities -- such as
 * rotation -- are now or in the future going to be in Async_file_logger, as its purpose is heavy-duty file logging
 * specifically.
 *
 * ### Thread safety ###
 * As noted above, simultaneous logging from multiple threads is safe from output corruption, in that
 * simultaneous do_log() calls for the same Logger targeting the same stream will log serially to each other.
 * However, if some other code or process writes to the same file, then all bets are off -- so don't.
 *
 * See thread safety notes and to-dos regarding #m_config in Simple_ostream_logger doc header.  These apply here also.
 *
 * There are no other mutable data (state), so that's that.
 *
 * @todo Lacking feature: Compress-as-you-log in Async_file_logger.  So, optionally, when characters are actually
 * written out to file-system, gzip/zip/whatever them instead of writing plain text.  (This is possible at least
 * for gzip.)  Background: It is common-place to compress a log file after it has been rotated (e.g., around rotation
 * time: F.log.1.gz -> F.log.2.gz, F.log -> F.log.1 -> F.log.1.gz). It is more space-efficient (at least), however,
 * to write to F.log.gz directly already in compressed form; then rotation requires only renaming (e.g.:
 * F.log.1.gz -> F.log.2.gz, F.log.gz [already gzipped from the start] -> F.log.1.gz).
 */
class Async_file_logger :
  public Logger,
  protected Log_context
{
public:
  // Constructors/destructor.

  /**
   * Constructs logger to subsequently log to the given file-system path.  It will append.
   *
   * @todo Consider adding Async_file_logger constructor option to overwrite the file instead of appending.
   *
   * @param config
   *        Controls behavior of this Logger.  In particular, it affects should_log() logic (verbosity default and
   *        per-component) and output format (such as time stamp format).  See thread safety notes in class doc header.
   *        This is saved in #m_config.
   * @param log_path
   *        File-system path to which to write subsequently.  Note that no writing occurs until the first do_log() call.
   * @param backup_logger_ptr
   *        The Logger to use for `*this` to log *about* its logging operations to the actual intended file-system path;
   *        or null to not log such things anywhere.  If you do not pass in null, but ensure
   *        `backup_logger_ptr->should_log()` lets through nothing more than `Sev::S_INFO` severity messages for
   *        `Flow_log_component::S_LOG`, then you can expect a reasonable amount of useful output that will not
   *        affect performance.  Tip: null is a reasonable value.  A Simple_ostream_logger logging to `cout` and `cerr`
   *        (or only `cout`) is also a good choice, arguably better than null.
   *        Lastly, setting verbosity to `INFO` for `*backup_logger_ptr` is typically a better choice than
   *        `TRACE` in practice.
   * @param capture_rotate_signals_internally
   *        If and only if this is `true`, `*this` will detect SIGHUP (or your OS's version thereof);
   *        upon seeing such a signal, it will fire the equivalent of log_flush_and_reopen(), as needed for classic
   *        log rotation.  (The idea is: If we are writing to path F, then your outside log rotation tool will rename
   *        F -> F.1 [and F.1 -> F.2, etc.]; even as we continue writing to the underlying file after it has been
   *        renamed; then the tool sends SIGHUP; we flush/close what is really F.1; reopen at the real path F
   *        again, which will create it anew post-rotation.)  If `false` then you'd have to do it yourself if desired.
   *        If this is `true`, the user may register their own signal handler(s) (for any purpose whatsoever) using
   *        `boost::asio::signal_set`. However, behavior is undefined if the program registers signal handlers via any
   *        other API, such as `sigaction()` or `signal()`.  If you need to set up such a non-`signal_set` signal
   *        handler, AND you require rotation behavior, then (1) set this option to `false`; (2) trap SIGHUP yourself;
   *        (3) in your handlers for the latter, simply call log_flush_and_reopen().  However, if typical, common-sense
   *        behavior is what you're after -- and either don't need additional signal handling or are OK with using
   *        `signal_set` for it -- then setting this to `true` is a good option.
   */
  explicit Async_file_logger(Logger* backup_logger_ptr,
                             Config* config, const fs::path& log_path,
                             bool capture_rotate_signals_internally);

  /// Flushes out anything buffered, returns resources/closes output file(s); then returns.
  ~Async_file_logger() override;

  // Methods.

  /**
   * Implements interface method by returning `true` if the severity and component (which is allowed to be null)
   * indicate it should.  As of this writing not thread-safe against changes to `*m_config`.
   *
   * @param sev
   *        Severity of the message.
   * @param component
   *        Component of the message.  Reminder: `component.empty() == true` is allowed.
   * @return See above.
   */
  bool should_log(Sev sev, const Component& component) const override;

  /**
   * Implements interface method by returning `true`, indicating that this Logger may need the contents of
   * `*metadata` and `msg` passed to do_log() even after that method returns.
   *
   * @return See above.
   */
  bool logs_asynchronously() const override;

  /**
   * Implements interface method by asynchronously logging the message and some subset of the metadata in a fashion
   * controlled by #m_config.
   *
   * @param metadata
   *        All information to potentially log in addition to `msg`.
   * @param msg
   *        The message.
   */
  void do_log(Msg_metadata* metadata, util::String_view msg) override;

  /**
   * Causes the log at the file-system path to be flushed/closed (if needed) and
   * re-opened; this will happen as soon as possible but may occur asynchronously after this method exits, unless
   * directed otherwise via `async` argument.
   *
   * ### Uses ###
   * Flushing: `log_flush_and_reopen(false)` is a reasonable and safe way to flush anything buffered in memory to the
   * file.  Naturally, for performance, it should not be done frequently.  For example this might be useful in the
   * event of an abnormal termination (`abort()`, etc.), in the signal handler before exiting program.
   *
   * Rotation: `log_flush_and_reopen(true)` is useful for rotation purposes; however, you need not do this manually if
   * you decided to (properly) use the `capture_rotate_signals_internally == true` option in Async_file_logger
   * constructor; the procedure will occur on receiving the proper signal automatically.
   *
   * @todo `Async_file_logger::log_flush_and_reopen(true)` is great for flushing, such as in an abort-signal handler,
   * but providing just the flushing part without the reopening might be useful.  At the moment we've left it
   * this way, due to the vague feeling that closing the file upon flushing it is somehow more final and thus safer
   * (in terms of accomplishing its goal) in an abort-signal scenario.  Feelings aren't very scientific though.
   *
   * @param async
   *        If `true`, the operation will execute ASAP but asynchronously, the method exiting immediately;
   *        else it will complete fully before this method returns.
   */
  void log_flush_and_reopen(bool async = true);

  // Data.  (Public!)

  /**
   * Reference to the config object passed to constructor.  Note that object is mutable; see notes on thread safety.
   *
   * @internal
   * ### Rationale ###
   * This can (and is but not exclusively) exclusively stored in `m_serial_logger->m_config`; it is stored here also
   * for `public` access to the user.  It's a pointer in any case.
   */
  Config* const m_config;

private:
  // Types.

  /// Short-hand for a signal set.
  using Signal_set = boost::asio::signal_set;

  // Methods.

  /**
   * SIGHUP/equivalent handler for the optional feature `capture_rotate_signals_internally` in constructor.
   * Assuming no error, executes `m_serial_logger->log_flush_and_reopen()` and then again waits for the next such
   * signal.
   *
   * @param sys_err_code
   *        The code from boost.asio.  Anything outside of `operation_aborted` is very strange.
   * @param sig_number
   *        Should be SIGHUP/equivalent, as we only wait for those signal(s).
   */
  void on_rotate_signal(const Error_code& sys_err_code, int sig_number);

  // Data.

  /**
   * This is the `Logger` doing all the real log-writing work (the one stored in Log_context is the
   * logger-about-logging, `backup_logger_ptr` from ctor args).  In itself it's a fully functional Logger, but
   * its main limitation is all calls between construction and destruction must be performed non-concurrently --
   * for example from a single thread or boost.asio "strand."  The way we leverage it is we simply only make
   * log-file-related calls (especially Serial_file_logger::do_log() but also
   * Serial_file_logger::log_flush_and_reopen()) from within #m_async_worker-posted tasks (in other words from
   * the thread #m_async_worker starts at construction).
   *
   * ### Design rationale ###
   * What's the point of Serial_file_logger?  Why not just have all those data and logic (storing the `ofstream`, etc.)
   * directly in Async_file_logger?  Answer: That's how it was originally.  The cumulative amount of state and logic
   * is the same; and there's no real usefulness for Serial_file_logger as a stand-alone Logger for general users;
   * so in and of itself splitting it out only adds a bit of coding overhead but is otherwise the same.  So why do it?
   * Answer:
   *
   * The reason we split it off was the following: We wanted to sometimes log to the real file from within the
   * #m_async_worker thread; namely to mark with a pair of log messages that a file has been rotated.  It was possible
   * to do this via direct calls to a would-be `impl_do_log()` (which is do_log() but already from the worker thread),
   * but then one couldn't use the standard `FLOW_LOG_*()` statements to do it; this was both inconvenient and difficult
   * to maintain over time (we predicted).  At that point it made sense that really Async_file_logger *is*
   * a would-be Serial_file_logger (one that works from one thread) with the creation of the one thread -- and other
   * such goodies -- added on top.  With that, `FLOW_LOG_SET_LOGGER(m_serial_logger)` + `FLOW_LOG_INFO()` trivially logs
   * to the file (as long as it's done from the worker thread), accomplishing that goal; and furthermore it makes
   * logical sense in terms of overall design.  The latter fact I (ygoldfel) would say is the best justification; and
   * the desire for `*this` to log to the file, itself, was the triggering use case.
   */
  boost::movelib::unique_ptr<Serial_file_logger> m_serial_logger;

  /**
   * The thread (1-thread pool, technically) in charge of all #m_serial_logger I/O operations including writes
   * to file.  Thread starts at construction of `*this`; ends (and is synchronously joined) at
   * destruction of `*this` (by means of delegating to #m_async_worker destructor).
   */
  async::Single_thread_task_loop m_async_worker;

  /**
   * Signal set which we may or may not be using to trap SIGHUP in order to auto-fire
   * `m_serial_logger->log_flush_and_reopen()` in #m_async_worker.  `add()` is called on it at init iff [sic]
   * that feature is enabled by the user via ctor arg `capture_rotate_signals_internally`.  Otherwise this object
   * just does nothing `*this` lifetime.
   */
  Signal_set m_signal_set;
}; // class Async_file_logger

} // namespace flow::log
