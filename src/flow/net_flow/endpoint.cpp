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
#include "flow/net_flow/endpoint.hpp"

namespace flow::net_flow
{

size_t Remote_endpoint::hash() const
{
  using boost::hash_combine;
  using boost::hash_range;
  using boost::asio::ip::address;

  size_t val = 0;

  // Hash Flow port.
  hash_combine(val, m_flow_port);

  // Combine with hash of m_udp_endpoint.  Port is straightforward.
  hash_combine(val, m_udp_endpoint.port());

  /* @todo The following is kind of slow... lots of copies....  Maybe just use the above and allow
   * for some hash collisions?  Or just use std::set/map instead of hash sets/maps? */

  /* Hashing the address is trickier.
   * Could just hash to_string(), but seems slicker/faster to use the byte representation itself. */
  const address& addr = m_udp_endpoint.address();
  if (addr.is_v4())
  {
    // Host byte order, but that's cool, as the hash value is computed and used only on this machine.
    hash_combine(val, addr.to_v4().to_uint());
  }
  else if (addr.is_v6())
  {
    // Network byte order.  Hash each byte of this byte array.
    const util::Ip_address_v6::bytes_type& addr_bytes = addr.to_v6().to_bytes();
    hash_range(val, addr_bytes.begin(), addr_bytes.end());
  }
  // else they've invented another IP version... so we'll just have to deal with some hash collisions.

  return val;
} // Remote_endpoint::hash()

bool operator==(const Remote_endpoint& lhs, const Remote_endpoint& rhs)
{
  return (lhs.m_flow_port == rhs.m_flow_port) && (lhs.m_udp_endpoint == rhs.m_udp_endpoint);
}

std::ostream& operator<<(std::ostream& os, const Remote_endpoint& endpoint)
{
  return os << "NetFlow [UDP " << endpoint.m_udp_endpoint << "]:" << endpoint.m_flow_port;
}

size_t hash_value(const Remote_endpoint& remote_endpoint)
{
  return remote_endpoint.hash();
}

} // namespace flow::net_flow
