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

/* This file is to be `#include`d after the following:
 *   #define FLOW_LOG_CFG_COMPONENT_ENUM_CLASS X_log_component
 *   #define FLOW_LOG_CFG_COMPONENT_ENUM_NAME_MAP S_X_LOG_COMPONENT_NAME_MAP
 * where X is some way to name your flow::log-logging module/project.  The other parts of the name as flexible too;
 * above is a convention at most.
 *
 * After `#include`ing this file, declare X_log_component's enum members, typically by `#include`ing a file named
 * X/.../log_component_enum_declare.macros.hpp, featuring only lines of this format:
 *   FLOW_LOG_CFG_COMPONENT_DEFINE(FIRST_COMPONENT_NAME, 0)
 *   FLOW_LOG_CFG_COMPONENT_DEFINE(SECOND_COMPONENT_NAME, 1)
 *   ...etc....
 * Requirements/conventions/tips about these names and numbers can be found in the model file which is Flow's own
 *   flow/detail/macros/log_component_enum_declare.macros.hpp.
 *
 * After that, `#include` the "end-cap" counterpart to the present file (i.e., config_enum_end_hdr.macros.hpp).
 *
 * The above elements must be present in an .hpp included by all logging source files in module/project X.
 * For example Flow itself has this in common.hpp.
 *
 * Lastly, repeat the same procedure, but in a .cpp file (e.g., Flow's own common.cpp), except #include
 * _cpp counterparts to the present file and config_enum_end_hdr.macros.hpp.  That is,
 * config_enum_{start|end}_cpp.macros.hpp should book-end another
 * re-`#include`ing of X/.../log_component_enum_declare.macros.hpp. */
#define FLOW_LOG_CFG_COMPONENT_DEFINE(ARG_name_root, ARG_enum_val) \
  S_ ## ARG_name_root = ARG_enum_val,
/* @todo Want to say `::flow::log::Component::enum_raw_t`, but because of C++'s circular-reference nonsense, I
 * (ygoldfel) had difficulty with it.  The fact this is a part of preprocessor-macro voodoo complicates the situation.
 * A static_assert() in Component code will ensure this doesn't go out-of-sync (IS the same type) which is an OK but
 * imperfect solution.  Probably there's some _fwd.hpp-involving setup possible to be able to use enum_raw_t here.
 * Anyway, both this and Component are under log/, so it's not so bad. */
enum class FLOW_LOG_CFG_COMPONENT_ENUM_CLASS : unsigned int
{

// -v- Doxygen, please stop ignoring.
/// @endcond
