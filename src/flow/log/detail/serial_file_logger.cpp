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
#include "flow/log/detail/serial_file_logger.hpp"
#include "flow/log/config.hpp"

namespace flow::log
{

// Implementations.

Serial_file_logger::Serial_file_logger(Logger* backup_logger_ptr,
                                       Config* config, const fs::path& log_path) :
  /* Set up the logging *about our real attempts at file logging, probably to a Simple_ostream_logger/cout;
   * or to null (meaning no such logging).  Either way we will still attempt to actually log user-requested messages
   * to log_path! */
  Log_context(backup_logger_ptr, Flow_log_component::S_LOG),
  m_config(config), // Save pointer, not copy, of the config given to us.  Hence thread safety is a thing.
  m_log_path(log_path),
  /* Initialize ostream m_ofs; but this is guaranteed to only mess with, at most, general ostream (not ofstream)
   * state such as locales and formatting; no I/O operations (writing) occur here; if they tried it'd be a problem,
   * as m_ofs is not open yet. */
  m_ofs_writer(*m_config, m_ofs),
  m_reopening(false)
{
  FLOW_LOG_INFO("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: These log messages are going to "
                "associated backup logger [" << backup_logger_ptr << "].  The messages actually logged through "
                "Logger API requests to `*this` will go to aforementioned log path, not to the present output.");

  // Immediately open file in worker thread.  Could be lazy about it but might as well log about any issue ASAP.
  log_flush_and_reopen();
  // ^-- Synchronous.  Doesn't matter what thread this is: they can't call do_log() until we return.
} // Serial_file_logger::Serial_file_logger()

Serial_file_logger::~Serial_file_logger() // Virtual
{
  FLOW_LOG_INFO("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: Deleting.  Will flush "
                "output if possible; then we will proceed to shut down.");

  if (m_ofs.is_open())
  {
    FLOW_LOG_INFO("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: Flushing before destruction "
                  "finishes in other thread.");
    m_ofs.flush();
  }
}

void Serial_file_logger::do_log(Msg_metadata* metadata, util::String_view msg) // Virtual.
{
  /* The current I/O error-handling cycle is set up as follows:
   *   At construction: Attempt open; bool(m_ofs) becomes T or F.  Done.
   *   At do_log() 1: If last op resulted in bool(m_ofs)==F, attempt re-open.  bool(m_ofs) becomes T or F.
   *                  If bool(m_ofs)==T, attempt to write string to m_ofs.  bool(m_ofs) becomes T or F.
   *                    [If F, then string is probably lost forever.]
   *   At do_log() 2: If last op resulted in bool(m_ofs)==F, attempt re-open.  bool(m_ofs) becomes T or F.
   *                  If bool(m_ofs)==T, attempt to write string to m_ofs.  bool(m_ofs) becomes T or F.
   *                    [If F, then string is probably lost forever.]
   *   At do_log() 3: ...etc.
   *
   * The idea is, basically: If the I/O op (either open or write) results in error at the time, retrying is documented
   * as unlikely or impossible to succeed at the time.  So log about any such problems to the *backup* logger but then
   * move on, until the next (later in time) operation is attempted; at which point attempt re-open for max
   * possibility of success.
   *
   * A formal to-do in my doc header is to perhaps buffer any failed impl_do_log()s, so they can be output upon
   * hypothetical successful reopen later. */

  if (!m_ofs)
  {
    FLOW_LOG_WARNING("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: do_log() wants to log, but "
                     "file stream is in bad state, spuriously or due to a previous op; will attempt reopen first.");
    log_flush_and_reopen();
    if (!m_ofs)
    {
      FLOW_LOG_WARNING("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: "
                       "Stream still in bad state; giving up on that log message.  "
                       "Will retry file stream for next message.");
      return;
    }
    // else { Cool, re-open succeeded, so fall through to in fact log. }
  }
  // else { Was in good state after last op; no reason to suspect otherwise now. }
  assert(m_ofs);

  m_ofs_writer.log(*metadata, msg);

  if (!m_ofs)
  {
    FLOW_LOG_WARNING
      ("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: do_log() just tried to log "
       "msg when file stream was in good state, but now file stream is in bad state, probably because the "
       "write failed.  Giving up on that log message.  Will retry file stream for next message.");
  }
} // Serial_file_logger::do_log()

void Serial_file_logger::log_flush_and_reopen()
{
  // See notes in do_log() about the error handling cycle.

  /* Discussion of us logging through *this, meaning FLOW_LOG_*() to our own target file:
   * It's easy to do: FLOW_LOG_SET_LOGGER(this); and then just do it.  However, logging while enabling the user to
   * log naturally raises fears of infinite loops/recursion:
   *   - do_log() can detect m_ofs bad state and trigger log_flush_and_reopen() which then FLOW_LOG_*()s which
   *     do_log()s which....
   *   - log_flush_and_reopen() may be called by user which FLOW_LOG_*()s (as it should) which do_log()s which
   *     can detect m_ofs bad state and trigger log_flush_and_reopen() which then....
   *
   * So we do this to simply never execute log_flush_and_reopen() logic if currently called from it in the first
   * place.  If necessary the on-error log_flush_and_reopen() will be re-attempted from the next user do_log().
   *
   * In addition, just to avoid entropy, we do not log to *this logger (the first and last lines per log file)
   * if m_ofs is in bad state (is falsy).  We could, and it would just fail, but it's best to just not.
   * As of this writing, actually, that m_ofs check would be sufficient to prevent the recursion problem by itself;
   * but this felt too brittle (easy to break with an inadvertent change). */
  if (m_reopening)
  {
    FLOW_LOG_TRACE("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: Reopen method invoked "
                   "by internal code while already being asked to reopen file.  Doing nothing.");
    return;
  }
  // else
  m_reopening = true;

  // Use inline lambda to ensure no matter how the below returns, we execute `m_reopening = false;` at the end.
  ([&]()
  {
    if (m_ofs.is_open())
    {
      FLOW_LOG_INFO("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: Closing before re-opening.");
      // XXX
#if 0
      if (m_ofs) // As noted above, avoid entropy if stream is in bad state anyway.
      {
        /* This shall be *the* last message logged before the close.  If part of a rotation operation -- a HUP sent
         * to us to cause log_flush_and_reopen() to execute -- then it'll be the last message in the file, which is
         * renamed before the HUP is sent.
         *
         * This *will* invoke do_log() -- but that do_log() *will not* invoke us again/infinitely due to
         * the m_reopening check at the top, at a minimum; plus as of this writing do_log() will not invoke us
         * at all, as it only triggers log_flush_and_reopen() if !m_ofs... but we just guaranteed the opposite. */
        FLOW_LOG_SET_LOGGER(this);
        FLOW_LOG_INFO("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: "
                      "Closing before re-opening.");
      }
#endif

      m_ofs.flush(); // m_ofs_writer `flush`es after each message... but just in case.
      m_ofs.close();
    }

    FLOW_LOG_INFO("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: "
                  "Opening for append.  Will log on error.");
    m_ofs.open(m_log_path, std::ios_base::app);

    if (m_ofs) // As noted above, avoid entropy if stream is in bad state anyway.
    {
      // XXX
#if 0
      /* This shall be *the* first message logged after the re-open.  If part of a rotation operation (see above),
       * then it'll be the first message in the new file, as the preceding file has been renamed, while we were still
       * writing to it. */
      FLOW_LOG_SET_LOGGER(this);
      FLOW_LOG_INFO("detail/Serial_file_logger [" << this << "] @ [" << m_log_path << "]: "
                    "Opened successfully for further logging.");
#endif
      m_ofs.flush(); // m_ofs_writer `flush`es after each message... but just in case.
    }
    else // if (!m_ofs)
    {
      FLOW_LOG_WARNING("Could not open.  Will probably retry subsequently.");
    }
  })();

  m_reopening = false;
} // Serial_file_logger::log_flush_and_reopen()

bool Serial_file_logger::should_log(Sev sev, const Component& component) const // Virtual.
{
  return m_config->output_whether_should_log(sev, component);
}

bool Serial_file_logger::logs_asynchronously() const // Virtual.
{
  return false;
}

} // namespace flow::log
