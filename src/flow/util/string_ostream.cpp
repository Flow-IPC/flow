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

#include "flow/util/string_ostream.hpp"

namespace flow::util
{

String_ostream::String_ostream(std::string* target_str) :
  m_target(target_str ? target_str : (&m_own_target_str)), // m_own_target_str blank/ignored if target_str not null.
  m_target_inserter(*m_target),
  m_target_appender_ostream(m_target_inserter)
{
  // Nothing else.
}

std::ostream& String_ostream::os()
{
  return m_target_appender_ostream;
}

const std::ostream& String_ostream::os() const
{
  return m_target_appender_ostream;
}

const std::string& String_ostream::str() const
{
  return *m_target;
}

void String_ostream::str_clear()
{
  m_target->clear();
}

} // namespace flow::util
