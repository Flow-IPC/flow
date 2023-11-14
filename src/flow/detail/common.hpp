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

#include <boost/unordered_map.hpp>

namespace flow
{

/// @cond
// -^- Doxygen, please ignore the following.  common.hpp documented the relevant items specially.

/* The following flow::log-recommended trio of #directives begins the necessary flow::log::Component-related
 * definitions having to do with Flow's own Component-compatible enumeration.  Namely:
 *   - This defines the `enum class Flow_log_component` itself, auto-generated via macro magic.
 *   - This declares (but does not yet populate) a (Flow_log_component -> std::string) multimap containing
 *     each enum value's string representation.
 *     - common.cpp will complete this procedure by populating the map.
 *
 * IMPORTANT:
 * `flow/detail/macros/log_component_enum_declare.macros.hpp` declares the log components used by all of Flow
 * (i.e., by everything in `flow` namespace).  Hence, if enum value X is in that file, then it can be (a)
 * passed into Log_context ctor; (b) passed into FLOW_LOG_SET_CONTEXT(); (c) potentially seen in per-message
 * log output; and (d) be used to configure verbosity for messages with component X. */

// This tells the below and certain other boiler-plate macro files the name of the `enum class` being generated.
#define FLOW_LOG_CFG_COMPONENT_ENUM_CLASS Flow_log_component
/* This tells the below and certain other boiler-plate macro files the name of the enum-to-string multimap mapping each
 * enum value to its name for log output and config purposes. */
#define FLOW_LOG_CFG_COMPONENT_ENUM_NAME_MAP S_FLOW_LOG_COMPONENT_NAME_MAP

#include "flow/log/macros/config_enum_start_hdr.macros.hpp"
#include "flow/detail/macros/log_component_enum_declare.macros.hpp"
#include "flow/log/macros/config_enum_end_hdr.macros.hpp"

// -v- Doxygen, please stop ignoring.
/// @endcond

} // namespace flow
