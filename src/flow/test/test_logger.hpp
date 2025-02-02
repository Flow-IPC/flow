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

#include "flow/test/test_config.hpp"
#include "flow/log/simple_ostream_logger.hpp"
#include "flow/common.hpp"

namespace flow::test
{

/**
 * Logger used for testing purposes.
 */
class Test_logger :
  public flow::log::Logger
{
public:
  /**
   * Constructor.
   *
   * @param min_severity Lowest severity that will pass through logging filter.
   */
  Test_logger(const flow::log::Sev& min_severity = Test_config::get_singleton().m_sev) :
    m_config(min_severity),
    m_logger(&m_config)
  {
    // Yes, it is formally (and practically) fine to do this after the Logger took the m_config ptr already.
    m_config.init_component_to_union_idx_mapping<Flow_log_component>
      (100, log::Config::standard_component_payload_enum_sparse_length<Flow_log_component>());
    m_config.init_component_names<Flow_log_component>(S_FLOW_LOG_COMPONENT_NAME_MAP, false, "flow-");
    // Now the logging may commence.
  }
  /**
   * Returns the logging configuration.
   *
   * @return See above.
   */
  log::Config& get_config()
  {
    return *m_logger.m_config;
  }

  /// Forwards to console Logger.
  bool should_log(log::Sev sev, const log::Component& component) const override
  {
    return m_logger.should_log(sev, component);
  }

  /// Forwards to console Logger.
  bool logs_asynchronously() const override
  {
    return m_logger.logs_asynchronously();
  }

  void do_log(log::Msg_metadata* metadata, util::String_view msg) override
  {
    m_logger.do_log(metadata, msg);
  }

private:
  /// Logging configuration.
  log::Config m_config;

  /// The real logger.
  log::Simple_ostream_logger m_logger;
}; // class Test_logger

} // namespace flow::test
