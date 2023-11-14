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

#include <boost/regex.hpp>

namespace flow::cfg
{
// Constants.

/// An internal constant for value_set_member_id_to_opt_name_keyed().
extern const boost::regex VALUE_SET_MEMBER_ID_TO_OPT_NAME_KEYED_REGEX;

} // namespace flow::cfg
