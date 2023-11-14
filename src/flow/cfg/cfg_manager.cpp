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
#include "flow/cfg/cfg_manager.hpp"

namespace flow::cfg
{

// Implementations.

typename Option_set<Null_value_set>::Declare_options_func null_declare_opts_func()
{
  return [](const Option_set<Null_value_set>::Declare_options_func_args&) {};
}

typename Final_validator_func<Null_value_set>::Type null_final_validator_func()
{
  return Final_validator_func<Null_value_set>::null();
}

} // namespace flow::cfg
