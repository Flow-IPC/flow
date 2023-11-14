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
#include "flow/util/uniq_id_holder.hpp"
#include <limits>

namespace flow::util
{

// Static initializations.

std::atomic<Unique_id_holder::id_t> Unique_id_holder::s_last_id(std::numeric_limits<id_t>::min());

// Implementations.

Unique_id_holder::Unique_id_holder() :
  /* Get the next available ID in the universe.  Use large type for id_t to avoid overflow.
   * Use atomic<> to ensure thread safety.  Use pre-increment so that the smallest possible ID is reserved and
   * never assigned to an actual m_id (might come in handy for debugging someday, identifying failure of this
   * feature). */
  m_id(++s_last_id)
{
  // Nothing else.
}

Unique_id_holder::Unique_id_holder(const Unique_id_holder&) :
  Unique_id_holder() // This (unusual) behavior is explained in class doc header.  Basically new obj = separate obj.
{
  // Nothing else.
}

const Unique_id_holder& Unique_id_holder::operator=(const Unique_id_holder&) const
{
  // Intentionally blank.  This (unusual) behavior is explained in class doc header.
  return *this;
}

Unique_id_holder::id_t Unique_id_holder::unique_id() const
{
  return m_id;
}

Unique_id_holder::id_t Unique_id_holder::create_unique_id() // Static.
{
  return Unique_id_holder().unique_id();
}

} // namespace flow::util
