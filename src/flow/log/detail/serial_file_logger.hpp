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

#include "flow/log/detail/log_fwd.hpp"
#include "flow/log/ostream_log_msg_writer.hpp"
#include "flow/log/log.hpp"

namespace flow::log
{

// Types.

/**
 * An internal-use implementation of Logger that logs messages to a given file-system path, blocking the calling
 * thread while the I/O occurs, and usable safely only if logging occurs non-concurrently.  As of this writing it
 * is essentially an internal component needed by Async_file_logger; but it is a full-fledged Logger nevertheless.
 *
 * ### Thread safety ###
 * The file I/O-using APIs, notably do_log() and log_flush_and_reopen() but not should_log(), are unsafe to call
 * concurrently.  The logging user -- as of this writing Async_file_logger but it's fully conceivable other uses
 * exist -- must therefore provide any anti-concurrency measures (use one thread; a strand; mutex; etc.).
 *
 * See thread safety notes and to-dos regarding #m_config in Simple_ostream_logger doc header.  These apply here also.
 *
 * @todo Consider having Serial_file_logger internally buffer any attempted log requests that it couldn't write to
 * file due to I/O error.  The logic already attempts re-open repeatedly but doesn't attempt to re-log failed
 * lines.
 */
class Serial_file_logger :
  public Logger,
  protected Log_context
{
public:
  // Constructors/destructor.

  /**
   * Constructs logger to subsequently log to the given file-system path.  It will append.  This constructor
   * itself does not perform any I/O operations.
   *
   * @param config
   *        See Async_file_logger ctor.
   * @param log_path
   *        See Async_file_logger ctor.
   * @param backup_logger_ptr
   *        See Async_file_logger ctor.
   */
  explicit Serial_file_logger(Logger* backup_logger_ptr,
                              Config* config, const fs::path& log_path);

  /// Flushes out anything buffered, returns resources/closes output file(s); then returns.
  ~Serial_file_logger() override;

  // Methods.

  /**
   * See Async_file_logger::should_log().
   *
   * @param sev
   *        See Async_file_logger::should_log().
   * @param component
   *        See Async_file_logger::should_log().
   * @return See Async_file_logger::should_log().
   */
  bool should_log(Sev sev, const Component& component) const override;

  /**
   * Implements interface method by returning `false`, indicating that this Logger will not need the contents of
   * `*metadata` and `msg` passed to do_log() after that method returns.
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

  /**
   * Causes the log at the file-system path to be flushed/closed (if needed) and
   * re-opened; this will occur synchronously meaning it will complete before the method returns.
   */
  void log_flush_and_reopen();

  // Data.  (Public!)

  /// See Async_file_logger::m_config.
  Config* const m_config;

private:
  // Data.

  /// File-system path to which to write subsequently.
  const fs::path m_log_path;

  /// The file to which to write.  Because only the worker thread ever accesses it, no mutex is needed.
  fs::ofstream m_ofs;

  /**
   * Stream writer via which to log messages to #m_ofs.  A `m_ofs_writer.log()` call synchronously writes to
   * #m_ofs and flushes it to the output device (file).  Same thread safety situation as #m_ofs (only worker thread
   * access it).
   */
  Ostream_log_msg_writer m_ofs_writer;

  /**
   * Starts at `false`, becomes `true` at entry to log_flush_and_reopen(), then becomes `false` again.
   * Simple anti-recursion measure.
   */
  bool m_reopening;
}; // class Serial_file_logger

} // namespace flow::log
