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
#include "flow/log/simple_ostream_logger.hpp"
#include "flow/log/config.hpp"

namespace flow::log
{

// Implementations.

Simple_ostream_logger::Simple_ostream_logger(Config* config,
                                             std::ostream& os, std::ostream& os_for_err) :
  m_config(config)
{
  /* If stream is same for both types of messages, might as well use only one Ostream_log_msg_writer for both.
   * It's just prettier (e.g., stream state saved/restores 1x instead of 2x) and a little better for performance.
   * However there could be 2+ Simple_ostream_loggers sharing the same ostream; nothing we can (reasonably) do to
   * detect that -- nor NEED we worry about it really; just a nicety. */
  m_os_writers[0] = Ostream_log_msg_writer_ptr(new Ostream_log_msg_writer(*m_config, os));
  m_os_writers[1]
    = (&os == &os_for_err)
        ? m_os_writers[0]
        : Ostream_log_msg_writer_ptr(new Ostream_log_msg_writer(*m_config, os_for_err));
  // See m_os_writers doc header for details about how they are freed at `*this` destruction.
}

bool Simple_ostream_logger::should_log(Sev sev, const Component& component) const // Virtual.
{
  return m_config->output_whether_should_log(sev, component);
}

bool Simple_ostream_logger::logs_asynchronously() const // Virtual.
{
  return false;
}

void Simple_ostream_logger::do_log(Msg_metadata* metadata, util::String_view msg) // Virtual.
{
  assert(metadata);

  /* Prevent simultaneous logging.
   * (Consider also mutex per each of m_os_writers[] instead of sharing one.  That would reduce lock contention.
   * However, often m_os_writers[] would really write to the same device such as terminal console or log file.
   * Thus, possibly this would cause interleaving of output in an undesirable way, which is basically what the mutex
   * is designed to avoid in the first place.  Maybe some buffering aspect would prevent interleaving in that case,
   * but it seems imprudent to count on it, or at least to have to worry about it.)
   *
   * Oh, of course we could also invoke only one mutex if m_os_writers[] streams are the same object -- trivial.
   * However, even if they are different, the user can then make them write to the same thing using 2>&1 on command
   * line, for example.  So it still seems best to avoid any interleaving even when it's further away from our control,
   * as in that latter situation.  So just don't mess with it and always use one mutex to avoid as much interleaving
   * as we can. */
  util::Lock_guard<decltype(m_log_mutex)> lock(m_log_mutex);

  /* This next part will block calling thread for however long the writing takes.
   * This is likely not an issue with cout/cerr but can be a significant issue with ofstream.  Hence why
   * Async_file_logger exists. */
  m_os_writers[(metadata->m_msg_sev > Sev::S_WARNING) ? 0 : 1]->log(*metadata, msg);
}

} // namespace flow::log
