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

#include "flow/net_flow/asio/node.hpp"
#include "flow/net_flow/asio/peer_socket.hpp"
#include "flow/net_flow/asio/server_socket.hpp"

namespace flow::net_flow::asio
{

// Implementations.  For Node code pertaining to individual Server_sockets and Peer_sockets see their .cpp files.

Node::Node(log::Logger* logger_ptr, util::Task_engine* target_async_task_engine,
           const util::Udp_endpoint& low_lvl_endpoint,
           Net_env_simulator* net_env_sim, Error_code* err_code,
           const Node_options& opts) :
  net_flow::Node(logger_ptr, low_lvl_endpoint, net_env_sim, err_code, opts),
  m_target_task_engine(target_async_task_engine) // May be null.
{
  FLOW_LOG_INFO("Starting net_flow::asio::Node [" << static_cast<void*>(this) << "]; "
                "saving target Task_engine [" << static_cast<void*>(target_async_task_engine) << "].");
}

util::Task_engine* Node::async_task_engine()
{
  // Not thread-safe against set_async_task_engine().  Discussed in my doc header.
  return m_target_task_engine;
}

void Node::set_async_task_engine(util::Task_engine* target_async_task_engine)
{
  // Not thread-safe against m_target_task_engine access.  Discussed in my doc header.
  FLOW_LOG_INFO("Object [" << this << "] has been assigned an Task_engine at [" << target_async_task_engine << "]; "
                "currently [" << static_cast<void*>(m_target_task_engine) << "].");
  m_target_task_engine = target_async_task_engine;
}

} // namespace flow::net_flow::asio
