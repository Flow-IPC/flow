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
#include "flow/common.hpp"

namespace flow
{

/// @cond
/* -^- Doxygen, please ignore the following.  It gets confused by macro-magic, and there's nothing useful to document
 * anyway. */

// Static initializers.

/* The following flow::log-recommended trio of #directives completes the necessary flow::log::Component-related
 * definitions having to do with Flow's own Component-compatible enumeration.  Namely:
 *   - common.hpp should have already defined the `enum class Flow_log_component` itself.
 *   - Below's boiler-plate will define a (Flow_log_component -> std::string) multimap containing each enum value's
 *     string representation, auto-generated via macro magic. */
#include "flow/log/macros/config_enum_start_cpp.macros.hpp"
#include "flow/detail/macros/log_component_enum_declare.macros.hpp"
#include "flow/log/macros/config_enum_end_cpp.macros.hpp"

// -v- Doxygen, please stop ignoring.
/// @endcond

} // namespace flow
