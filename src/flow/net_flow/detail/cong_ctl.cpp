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
#include "flow/net_flow/detail/cong_ctl/cong_ctl_classic.hpp"
#include "flow/net_flow/detail/cong_ctl/cong_ctl_classic_bw.hpp"

namespace flow::net_flow
{

// Class Congestion_control_strategy.

// Implementations.

Congestion_control_strategy::Congestion_control_strategy(log::Logger* logger_ptr, Peer_socket::Const_ptr sock) :
  Log_context(logger_ptr, Flow_log_component::S_NET_FLOW_CC),
  m_sock(sock) // Get a weak_ptr from a shared_ptr.
{
  assert(sock);
}

void Congestion_control_strategy::on_acks(size_t, size_t) // Virtual.
{
  // The vacuous congestion control module just ignores acknowledgments.
}

void Congestion_control_strategy::on_loss_event(size_t, size_t)
  // Virtual.
{
  // The vacuous congestion control module just ignores loss.
}

void Congestion_control_strategy::on_drop_timeout(size_t, size_t)
  // Virtual.
{
  // The vacuous congestion control module just ignores loss.
}

void Congestion_control_strategy::on_idle_timeout() // Virtual.
{
  // The vacuous congestion control module just ignores idleness.
}

void Congestion_control_strategy::on_individual_ack(const Fine_duration&, size_t, size_t) // Virtual.
{
  // The vacuous congestion control module just ignores individual RTTs.  (Some actual strategies do too.)
}

Peer_socket::Const_ptr Congestion_control_strategy::socket() const
{
  const Peer_socket::Const_ptr sock = m_sock.lock();

  /* This trips only if Node violates the "Peer_socket exists while Congestion_control_strategy
   * exists" rule.  This assert() is the only reason sock is a weak_ptr<const Peer_socket> instead
   * of a "const Peer_socket*".  Without that we couldn't perform the assert() and would just
   * crash later. */
  assert(sock);

  return sock;
}

// Class Congestion_control_selector.

// Static initializations.

const std::map<std::string, Congestion_control_selector::Strategy_choice>
        Congestion_control_selector::S_ID_TO_STRATEGY_MAP
          ({
             { "classic",
               Strategy_choice::S_CLASSIC },
             { "classic_with_bandwidth_est",
               Strategy_choice::S_CLASSIC_BANDWIDTH_ESTIMATED }
           });
// Annoying we have to copy/paste/flip S_ID_TO_STRATEGY_MAP, but it's not that bad.
const std::map<Congestion_control_selector::Strategy_choice, std::string>
        Congestion_control_selector::S_STRATEGY_TO_ID_MAP
          ({
             { Strategy_choice::S_CLASSIC,
               "classic" },
             { Strategy_choice::S_CLASSIC_BANDWIDTH_ESTIMATED,
               "classic_with_bandwidth_est" }
           });

// Methods.

Congestion_control_strategy* Congestion_control_selector::create_strategy
                               (Strategy_choice strategy_choice,
                                log::Logger* logger_ptr, Peer_socket::Const_ptr sock) // Static
{
  switch (strategy_choice)
  {
  case Strategy_choice::S_CLASSIC:
    return new Congestion_control_classic{logger_ptr, sock};
  case Strategy_choice::S_CLASSIC_BANDWIDTH_ESTIMATED:
    return new Congestion_control_classic_with_bandwidth_est{logger_ptr, sock};
  }
  assert(false);
  return nullptr;
}

void Congestion_control_selector::get_ids(std::vector<std::string>* ids) // Static.
{
  ids->clear();
  for (const auto& id_and_strategy : S_ID_TO_STRATEGY_MAP)
  {
    ids->push_back(id_and_strategy.first);
  }
}

std::istream& operator>>(std::istream& is, Congestion_control_selector::Strategy_choice& strategy_choice)
{
  /* It's slightly weird, but the prototype for this is in options.hpp, so that it is available
   * to the user.  But the implementation is here, since we need access to Congestion_control_selector....
   * @todo It's irregular but seems OK.  Revisit. */

  using std::string;

  // Read a token (space-delimited word) from the input stream into the string.

  string id;
  is >> id;
  // If there was nothing left in stream, or other error, is.good() is false now.  Not our problem.

  // Try to map that token to the known IDs.  If unknown, choose the default (Reno for now).
  auto it = Congestion_control_selector::S_ID_TO_STRATEGY_MAP.find(id);
  strategy_choice = (it == Congestion_control_selector::S_ID_TO_STRATEGY_MAP.end())
                      ? Congestion_control_selector::Strategy_choice::S_CLASSIC
                      : it->second;
  return is;
}

std::ostream& operator<<(std::ostream& os, const Congestion_control_selector::Strategy_choice& strategy_choice)
{
  // See comment at top of operator<<().

  // Just print the text ID of the given enum value.
  auto it = Congestion_control_selector::S_STRATEGY_TO_ID_MAP.find(strategy_choice);
  assert(it != Congestion_control_selector::S_STRATEGY_TO_ID_MAP.end());

  return os << it->second;
}

} // namespace flow::net_flow
