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
#include "flow/net_flow/detail/net_flow_fwd.hpp"

namespace flow::net_flow
{
// Implementations.

std::ostream& operator<<(std::ostream& os, const Function<std::ostream& (std::ostream&)>& os_manip)
{
  // Do exactly what the namespace std version of operator<<() does when os_manip is an equivalent function pointer.
  return os_manip(os);
}

} // namespace flow::net_flow
