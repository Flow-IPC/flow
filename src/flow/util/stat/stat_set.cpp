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

#include "flow/util/stat/stat_set.hpp"
#include "flow/cfg/cfg_fwd.hpp"
#include <ostream>
#include <string>

namespace flow::util::stat
{

// Stat_name implementations.

Stat_name::Stat_name(String_view fragment) :
  m_base(nullptr),
  m_fragment(fragment),
  m_fragment_kind(Fragment_kind::S_LITERAL)
{
  // Yep.
}

Stat_name::Stat_name(const char* fragment) :
  Stat_name(String_view{fragment})
{
  // Yep.
}

Stat_name::Stat_name(const Stat_name& base, String_view fragment, Fragment_kind fragment_kind) :
  m_base(&base),
  m_fragment(fragment),
  m_fragment_kind(fragment_kind)
{
  // Yep.
}

void Stat_name::append_to(std::string* target) const
{
  // Keep in-sync with to_ostream().
  if (m_base)
  {
    m_base->append_to(target); // Root first: each fragment is appended once, in order.
  }

  if (m_fragment_kind == Fragment_kind::S_MEMBER_ID)
  {
    *target += cfg::value_set_member_id_to_opt_name(m_fragment, '_');
  }
  else
  {
    *target += m_fragment;
  }
}

void Stat_name::to_ostream(std::ostream& os) const
{
  // Keep in-sync with append_to().
  if (m_base)
  {
    m_base->to_ostream(os); // Root first: each fragment is written once, in order.
  }

  if (m_fragment_kind == Fragment_kind::S_MEMBER_ID)
  {
    os << cfg::value_set_member_id_to_opt_name(m_fragment, '_');
  }
  else
  {
    os << m_fragment;
  }
}

std::string Stat_name::str() const
{
  std::string name;
  append_to(&name);
  return name;
}

} // namespace flow::util::stat
