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

/// @cond
// -^- Doxygen, please ignore the following.  This is wacky macro magic and not a regular `#pragma once` header.

/* See common.hpp and common.cpp which #include us.
 * Long story short, the following macro invocations directly or indirectly specify:
 *   - each enum variable name of `enum class Flow_log_component`, the Component payload enum used by Flow log messages
 *     themselves (`S_` is auto-prepended to each name);
 *   - for each enum variable name, its numeric counterpart;
 *   - for each enum variable name, its string version for output and config-specification purposes (the name is
 *     auto-derived from variable name via the # macro operator).
 *
 * Requirements:
 *   - The numeric values must appear in ascending order (even though gaps are allowed; see below).
 *     - Technically, all that's really required is that the last one mentioned is the one with the highest numeric
 *       value, but just keep them in order, period.
 *   - Be aware that the `enum` member `S_END_SENTINEL` will be auto-appended, and its numeric value will equal
 *     that of the last (and highest) numeric value you specify, plus 1.  This is used by certain flow::log utilities,
 *     but we suppose the user might use it too (unlikely as it is).
 *     - Obviously you cannot declare one named `END_SENTINEL`, as a result.  Informally, you should also not add
 *       your own end sentinel member.
 *
 * A few common-sense reminders for convenience:
 *   - Do not prepend a namespace-like prefix to each var name.  In code, the enum class name will serve that role.
 *     In log output, the Config API allows one to specify a prefix applied to all enum member names, at runtime.
 *     In config specifications, ditto.  So don't worry that <your enum>::S_UTIL will in any way clash with
 *     <some other enum>::S_UTIL.
 *   - It is conventional (but not mandatory) to have (UNCAT, 0) be the first entry, representing "uncategorized" or
 *     "general."  It is also conventional to start with 0 and not a higher number.  Only unsigned are supported.
 *   - Numeric gaps are allowed but discouraged, unless there's a great reason (typically to do with backwards
 *     compatibility and component deprecation -- in author's experience).
 *   - The same number can correspond to 2+ enum members.  This is discouraged, unless there's a great reason
 *     (typically historic -- in author's experience).  If this is the case, then:
 *     - Verbosity X specified for any 1 of the 2+ enum members will apply to all 2+ of them.  In other words, all 2+
 *       components are really one, in a way, linked together by their common number.
 *     - String output of such a component will be a comma-separated list of all 2+ enum names.  If numeric output
 *       is configured instead, then by definition it will print the 1 number shared by 2+ enum names.
 *     - Any one of the 2+ names can be used in code and config to specify the component.  However, again, the effect
 *       will be as per the previous 2 bullet points. */

// Rarely used component corresponding to log call sites outside namespace `flow::X`, for all X in `flow`.
FLOW_LOG_CFG_COMPONENT_DEFINE(UNCAT, 0)
// Logging from namespace flow::log.
FLOW_LOG_CFG_COMPONENT_DEFINE(LOG, 1)
// Logging from namespace flow::error.
FLOW_LOG_CFG_COMPONENT_DEFINE(ERROR, 2)
// Logging from namespace flow::util.
FLOW_LOG_CFG_COMPONENT_DEFINE(UTIL, 3)
// Logging from namespace flow::async.
FLOW_LOG_CFG_COMPONENT_DEFINE(ASYNC, 4)
// Logging from namespace flow::perf.
FLOW_LOG_CFG_COMPONENT_DEFINE(PERF, 5)
// Logging from namespace flow::net_flow, when the following more-specific components don't apply.
FLOW_LOG_CFG_COMPONENT_DEFINE(NET_FLOW, 6)
// Logging from namespace flow::net_flow::asio, which integrates flow::net_flow sockets with boost.asio.
FLOW_LOG_CFG_COMPONENT_DEFINE(NET_FLOW_ASIO, 7)
// Logging from namespace flow::net_flow, in congestion control-related code.
FLOW_LOG_CFG_COMPONENT_DEFINE(NET_FLOW_CC, 8)
// Logging from namespace flow::cfg.
FLOW_LOG_CFG_COMPONENT_DEFINE(CFG, 9)

// -v- Doxygen, please stop ignoring.
/// @endcond
