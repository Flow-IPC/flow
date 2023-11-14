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
// -^- Doxygen, please ignore the following.  This is wacky macro magic and not a regular once-compiled file.

// See config_enum_start_cpp.macros.hpp to see what's going on here.  It probably looks pretty weird....

#define FLOW_LOG_CFG_COMPONENT_DEFINE(ARG_name_root, ARG_enum_val) \
  { FLOW_LOG_CFG_COMPONENT_ENUM_CLASS::S_ ## ARG_name_root, #ARG_name_root },
const boost::unordered_multimap<FLOW_LOG_CFG_COMPONENT_ENUM_CLASS, std::string>
  FLOW_LOG_CFG_COMPONENT_ENUM_NAME_MAP
    ({

// -v- Doxygen, please stop ignoring.
/// @endcond
