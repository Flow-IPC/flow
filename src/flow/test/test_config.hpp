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

#pragma once

#include <flow/log/log.hpp>
#include <boost/any.hpp>

namespace flow::test
{

/**
 * Test configuration.
 */
class Test_config
{
public:
  /**
   * Returns the singleton to the configuration.
   */
  static Test_config& get_singleton()
  {
    static Test_config s_config;
    return s_config;
  }

  /// Help command-line parameter.
  static const std::string S_HELP_PARAM;
  /// Minimum log severity command-line parameter.
  static const std::string S_LOG_SEVERITY_PARAM;

  /// Minimum log severity.
  log::Sev m_sev = flow::log::Sev::S_DATA;

private:
  /// Constructor.
  Test_config() {}
}; // class Test_config

} // namespace flow::test
