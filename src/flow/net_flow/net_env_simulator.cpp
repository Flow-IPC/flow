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
#include "flow/net_flow/net_env_simulator.hpp"

namespace flow::net_flow
{
// Implementations.

Net_env_simulator::Net_env_simulator(log::Logger* logger_ptr,
                                     seed_type_t random_seed,
                                     prob_type_t recv_packet_loss_prob,
                                     const Packet_loss_seq& recv_packet_loss_seq,
                                     const Latency_range& recv_latency_range,
                                     const Latency_seq& recv_latency_seq,
                                     prob_type_t recv_packet_dup_prob,
                                     const Packet_dup_seq& recv_packet_dup_seq) :
  log::Log_context(logger_ptr, Flow_log_component::S_NET_FLOW),
  m_seed((random_seed == 0) ? seed_type_t(util::time_since_posix_epoch().count()) : random_seed),
  m_rnd_generator(m_seed),
  m_recv_packet_loss_seq(recv_packet_loss_seq),
  m_recv_packet_loss_distribution(recv_packet_loss_prob),
  m_recv_latency_seq(recv_latency_seq),
  m_recv_latency_distribution_msec(recv_latency_range.first.count(),
                                   recv_latency_range.second.count()),
  m_recv_packet_dup_seq(recv_packet_dup_seq),
  m_recv_packet_dup_distribution(recv_packet_dup_prob)
{
  using boost::chrono::round;
  using boost::chrono::milliseconds; // Just for the output streaming.

  FLOW_LOG_INFO("Net_env_simulator [" << this << "] started with random seed [" << m_seed << "]; "
                "receiver packet loss ["
                << (recv_packet_loss_prob * prob_type_t(100)) << "%]; "
                "latency range [" << round<milliseconds>(recv_latency_range.first) << ", "
                << round<milliseconds>(recv_latency_range.second) << "]; "
                "packet duplication probability ["
                << (recv_packet_dup_prob * prob_type_t(100)) << "%]"
                ".");
}

bool Net_env_simulator::should_drop_received_packet()
{
  if (m_recv_packet_loss_seq.empty())
  {
    // Ran out of prescribed outcomes; use randomness.
    return m_recv_packet_loss_distribution(m_rnd_generator);
  }
  // else
  const bool drop = m_recv_packet_loss_seq.front();
  m_recv_packet_loss_seq.pop();
  return drop;
}

bool Net_env_simulator::should_duplicate_received_packet()
{
  if (m_recv_packet_dup_seq.empty())
  {
    // Ran out of prescribed outcomes; use randomness.
    return m_recv_packet_dup_distribution(m_rnd_generator);
  }
  // else
  const bool dup = m_recv_packet_dup_seq.front();
  m_recv_packet_dup_seq.pop();
  return dup;
}

Fine_duration Net_env_simulator::received_packet_latency()
{
  if (m_recv_latency_seq.empty())
  {
    // Ran out of prescribed outcomes; use randomness.
    return Fine_duration{m_recv_latency_distribution_msec(m_rnd_generator)};
  }
  // else
  const Fine_duration latency = m_recv_latency_seq.front();
  m_recv_latency_seq.pop();
  return latency;
}

} // namespace flow::net_flow

