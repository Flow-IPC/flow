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
#include "flow/error/error.hpp"

namespace flow::error
{

// Implementations.

Runtime_error::Runtime_error(const Error_code& err_code_or_success, util::String_view context) :
  // This strange-looking dichotomy is explained in ###Rationale### in m_context_if_no_code doc header.
  boost::system::system_error(err_code_or_success,
                              err_code_or_success
                                ? std::string(context)
                                : std::string()),
  // code() == err_code_or_success, now.
  m_context_if_no_code(code()
                         ? std::string()
                         : std::string(context))
{
  // Nothing.
}

Runtime_error::Runtime_error(util::String_view context) :
  Runtime_error(Error_code(), context) // Our formal contract is to be equivalent to this.
{
  // Nothing.
}

const char* Runtime_error::what() const noexcept // Virtual.
{
  // This dichotomy is explained in ###Rationale### in m_context_if_no_code doc header.  See ctor above also.
  return code()
           ? boost::system::system_error::what()
           : m_context_if_no_code.c_str();
}

} // namespace flow::error
