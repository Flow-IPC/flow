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

#include "flow/log/ostream_log_msg_writer.hpp"
#include "flow/log/log.hpp"
#include "flow/util/util_fwd.hpp"
#include <boost/array.hpp>
#include <boost/shared_ptr.hpp>

namespace flow::log
{

// Types.

/**
 * An implementation of Logger that logs messages to the given `ostream`s (e.g., `cout` or
 * an `ofstream` for a file).  Protects against garbling due to simultaneous logging from multiple
 * threads.
 *
 * Vaguely speaking, this Logger is suitable for console (`cout`, `cerr`) output; and, in a pinch outside of a
 * heavy-duty production/server environment, for file (`ofstream`) output.  For heavy-duty file logging one
 * should use Async_file_logger.  The primary reason is performance; this is discussed in the Logger class doc header.
 * A secondary reason is additional file-logging-specific utilities -- such as rotation -- are now or in the future
 * going to be in Async_file_logger, as its purpose is heavy-duty file logging specifically.
 *
 * ### Thread safety ###
 * As noted above, simultaneous logging from multiple threads is safe from output corruption, in that
 * simultaneous do_log() calls for the same Logger targeting the same stream will log serially to each other.
 *
 * Additionally, changes to the Config (which controls should_log() behavior among other things), pointer to which
 * is passed to constructor, are thread-safe against should_log() and do_log() if and only if class Config doc header
 * describes them as such.  Short version: You can't modify the Config anymore (while should_log() and do_log())
 * except you can dynamically modify verbosity via Config::configure_default_verbosity() and
 * Config::configure_component_verbosity() or Config::configure_component_verbosity_by_name().
 *
 * There are no other mutable data (state), so that's that.
 *
 * ### Thread safety: Corner case: Don't cross the `ostream`s! ###
 * There is another, arguably subtle, thread safety issue.
 * Bottom line/summary: If you pass `ostream` S (including `cout` and `cerr`)
 * into object Simple_ostream_logger S1 constructor, then don't
 * access S from any other code, including outside code printing to S, AND including indirect access via any other
 * Simple_ostream_logger S2 that isn't S1 -- at least not when S1 might be logging.
 *
 * Now in more detail: Suppose S is an underlying `ostream`; for example S could be `cerr`.
 * Suppose Simple_ostream_logger S1 is constructed with S, meaning it will always print to `cerr`.  Finally,
 * suppose some other unrelated code *also* accesses the stream S *while* a log statement is printing
 * to S through S1; for example that code might be writing some raw printouts to standard error by means
 * of `cerr <<`.  Hence, S is being write-accessed from 2 threads.
 * At best, this can cause garbled output.  (This is unavoidable but could be seen as acceptable functionally.)
 * However the danger doesn't end there; internally S1 code will use stateful formatters to affect the output.
 * Write ops via `ostream` formatters are *not* thread-safe against use of formatters on the same `ostream` from
 * another thread.  This can cause very-hard-to-identify crashes, such as double-frees reported
 * from deep inside `ostream`-related library code.  Garbled output may be okay in a pinch, but crashes and other
 * concurrency-corruption issues are never okay!
 *
 * One's reaction to that might be "of course!" -- it's simply messing with an `ostream` from 2+ different threads
 * simultaneously: it should be expected to break.  However, an essentially identical problem -- which one
 * might assume wouldn't exist (but one would be wrong) -- is the following.  Suppose everything is as described
 * in the previous paragraph, except that the "outside code accessing stream from another thread simultaneously"
 * isn't unrelated but rather simply *another* Simple_ostream_logger S2 that happened to be
 * constructed against `ostream` S, same as S1 was.  This, too, can break dangerously in the same ways and for
 * the same reason.  That is, Simple_ostream_logger takes no special measures to prevent this; all built-in
 * thread-safety measures apply to multi-threaded uses of each given `*this` -- not across different `*this`es.
 */
class Simple_ostream_logger :
  public Logger
{
public:
  // Constructors/destructor.

  /**
   * Constructs logger to subsequently log to the given standard `ostream` (or 2 thereof).
   *
   * @warning It is strongly recommended that numeric format state (for integers) in `os` and `os_for_err` be at
   *          default values (e.g, no `std::hex` or non-default fill character) at entry to this constructor.
   *          Otherwise the contents of prefixes within log output will be formatted in some undefined way.
   *
   * Note that usually one SHOULD avoid writing anything
   * to `os` or `os_for_err` once they've been passed to Simple_ostream_logger.  Logs should not (usually) be mixed with
   * other output, as (1) it would look odd; and (2) interleaving may occur, since there is no mutex making logging
   * mutually exclusive against other output to the same stream(s).  Update: It might be worse that that; see
   * thread safety notes in Simple_ostream_logger doc header.  Reiterating: Really one and only one
   * Simple_ostream_logger should access a given `ostream`, and no other code should.
   *
   * The case `&os == &os_for_err` (in other words they are the same stream) is not only allowed but likely common,
   * so that post-factum redirect of `stderr` into `stdout` (often done via `2>&1` on command line) becomes unnecessary.
   *
   * @param config
   *        Controls behavior of this Logger.  In particular, it affects should_log() logic (verbosity default and
   *        per-component) and output format (such as time stamp format).  See thread safety notes in class doc header.
   *        This is saved in #m_config.
   * @param os
   *        `ostream` to which to log messages of severity strictly less severe than Sev::S_WARNING.
   * @param os_for_err
   *        `ostream` to which to log messages of severity Sev::S_WARNING or more severe.
   */
  explicit Simple_ostream_logger(Config* config,
                                 std::ostream& os = std::cout, std::ostream& os_for_err = std::cerr);

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
   * Implements interface method by returning `false`, indicating that this Logger will not need the contents of
   * `*metadata` and `msg` passed to do_log(), as the latter will output them synchronously.
   *
   * @return See above.
   */
  bool logs_asynchronously() const override;

  /**
   * Implements interface method by synchronously logging the message and some subset of the metadata in a fashion
   * controlled by #m_config.
   *
   * @param metadata
   *        All information to potentially log in addition to `msg`.
   * @param msg
   *        The message.
   */
  void do_log(Msg_metadata* metadata, util::String_view msg) override;

  // Data.  (Public!)

  /// Reference to the config object passed to constructor.  Note that object is mutable; see notes on thread safety.
  Config* const m_config;

private:

  // Types.

  /// Short-hand for ref-counted pointer to a given Ostream_log_msg_writer.  See #m_os_writers for ref-count rationale.
  using Ostream_log_msg_writer_ptr = boost::shared_ptr<Ostream_log_msg_writer>;

  // Data.

  /**
   * Stream writers via which to log messages, each element corresponding to a certain set of possible severities
   * (as of this writing, warnings or worse versus all others).
   * 2+ pointers stored may point to the same object (see constructor body), if they share the same `ostream`.
   * In that case, ref-counted pointer mechanics will ensure the shared Ostream_log_msg_writer is freed when
   * #m_os_writers `array` is freed, when `*this` is destroyed.
   */
  boost::array<Ostream_log_msg_writer_ptr, 2> m_os_writers;

  /// Mutex protecting against log messages being logged concurrently and thus being garbled.
  mutable util::Mutex_non_recursive m_log_mutex;
}; // class Simple_ostream_logger

} // namespace flow::log
