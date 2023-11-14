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
#include "flow/util/string_ostream.hpp"
#include "flow/util/util_fwd.hpp"

namespace flow::log
{

/**
 * An implementation of Logger that logs messages to an internal `std::string` buffer
 * and provides read-only access to this buffer (for example, if one wants to write out its contents
 * when exiting program).
 *
 * ### Thread safety ###
 * Simultaneous logging from multiple threads is safe from output corruption, in that
 * simultaneous do_log() calls for the same Logger will log serially to each other.
 * buffer_str_copy() is also safe.  However, buffer_str() may not be thread-safe; see its doc header
 * for more information.
 *
 * See thread safety notes and to-dos regarding #m_config in Simple_ostream_logger doc header.  These apply here also.
 *
 * There are no other mutable data (state), so that's that.
 */
class Buffer_logger :
  public Logger
{
public:
  // Constructors/destructor.

  /**
   * Constructs logger to subsequently log to a newly constructed internal `std::string` buffer.
   *
   * @param config
   *        Controls behavior of this Logger.  In particular, it affects should_log() logic (verbosity default and
   *        per-component) and output format (such as time stamp format).  See thread safety notes in class doc header.
   *        This is saved in #m_config.
   */
  explicit Buffer_logger(Config* config);

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
   * Internally, this will involve a possible allocation (to enlarge an internal buffer when
   * needed) and essentially copying of the `msg` and `*metadata` payloads onto the end of the buffer.  This does not
   * block (in the classic I/O sense) the calling thread.  Needless to say, memory use may become a concern depending
   * on the verbosity of the logging.
   *
   * @param metadata
   *        All information to potentially log in addition to `msg`.
   * @param msg
   *        The message.
   */
  void do_log(Msg_metadata* metadata, util::String_view msg) override;

  /**
   * Read-only access to the buffer string containing the messages logged thus far.
   *
   * @warning While high-performance, this is not thread-safe, in that other threads logging to
   *          `*this` may cause the returned string to be subsequently accessed mid-write and therefore possibly
   *          in an undefined state.  Take care to access the result only when you know no other thread
   *          is logging (e.g., when exiting program).
   *
   * @return Read-only reference.
   */
  const std::string& buffer_str() const;

  /**
   * Returns a copy of `buffer_str()` in thread-safe fashion.  However, this may be quite slow if done frequently.
   *
   * @return Newly created string.
   */
  const std::string buffer_str_copy() const;

  // Data.  (Public!)

  /// Reference to the config object passed to constructor.  Note that object is mutable; see notes on thread safety.
  Config* const m_config;

private:
  // Data.

  /// Like `ostringstream` but allows for fast access directly into its internal string buffer.
  util::String_ostream m_os;

  /// Wrapper around #m_os that will take care of prefacing each message with time stamp, etc.
  Ostream_log_msg_writer m_os_writer;

  /**
   * Mutex protecting against log messages being logged, accessing #m_os concurrently and thus corrupting (or at best
   * garbling) the output string `m_os.str()`.
   */
  mutable util::Mutex_non_recursive m_log_mutex;
}; // class Buffer_logger

} // namespace flow::log
