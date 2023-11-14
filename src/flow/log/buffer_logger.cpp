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
#include "flow/log/buffer_logger.hpp"
#include "flow/log/config.hpp"

namespace flow::log
{

// Implementations.

Buffer_logger::Buffer_logger(Config* config) :
  m_config(config),
  m_os_writer(*m_config, m_os.os())
{
  // Nothing else.
}

bool Buffer_logger::should_log(Sev sev, const Component& component) const // Virtual.
{
  return m_config->output_whether_should_log(sev, component);
}

bool Buffer_logger::logs_asynchronously() const // Virtual.
{
  return false;
}

void Buffer_logger::do_log(Msg_metadata* metadata, util::String_view msg) // Virtual.
{
  // Prevent simultaneous logging, reading-by-copy.
  util::Lock_guard<decltype(m_log_mutex)> lock(m_log_mutex);

  // m_os_writer wraps (as of this writing) String_ostream, which wraps std::string.  Write msg+metadata to std::string.
  m_os_writer.log(*metadata, msg);
}

const std::string& Buffer_logger::buffer_str() const
{
  return m_os.str(); // (Mutex lock wouldn't help here, even if we used it, as this returns [basically] a pointer.)
}

const std::string Buffer_logger::buffer_str_copy() const
{
  // Prevent simultaneous logging, reading.
  util::Lock_guard<decltype(m_log_mutex)> lock(m_log_mutex);

  return buffer_str(); // Copy occurs here; then mutex is unlocked.
}

} // namespace flow::log
