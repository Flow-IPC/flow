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
#include "flow/net_flow/detail/port_space.hpp"
#include "flow/util/util.hpp"
#include "flow/error/error.hpp"
#include <limits>

namespace flow::net_flow
{

// Type checks.
static_assert(sizeof(size_t) > sizeof(flow_port_t),
              "@todo Investigate why this is required.  As of this writing I do not recall off-hand.");

// Static initializations.

const flow_port_t S_PORT_ANY = 0; // Not in Port_space, but the numbering must fit within Port_space's.
// Avoid overflow via above static assertion.
const size_t Port_space::S_NUM_PORTS = size_t(std::numeric_limits<flow_port_t>::max()) + 1;
const size_t Port_space::S_NUM_SERVICE_PORTS = S_NUM_PORTS / 2 - 1;
// -1 for S_PORT_ANY.  @todo I now have no idea what that comment means.  Figure it out....
const size_t Port_space::S_NUM_EPHEMERAL_PORTS = S_NUM_PORTS - S_NUM_SERVICE_PORTS - 1;
const size_t Port_space::S_MAX_RECENT_EPHEMERAL_PORTS = S_NUM_EPHEMERAL_PORTS / 32;
const flow_port_t Port_space::S_FIRST_SERVICE_PORT = 1;
const flow_port_t Port_space::S_FIRST_EPHEMERAL_PORT = S_FIRST_SERVICE_PORT + S_NUM_SERVICE_PORTS;

// Implementations.

Port_space::Port_space(log::Logger* logger_ptr) :
  log::Log_context(logger_ptr, Flow_log_component::S_NET_FLOW),
  // Set the bit fields to their permanent widths.
  m_service_ports(S_NUM_SERVICE_PORTS),
  m_ephemeral_ports(S_NUM_EPHEMERAL_PORTS),
  m_ephemeral_and_recent_ephemeral_ports(S_NUM_EPHEMERAL_PORTS),
  m_rnd_generator(Random_generator::result_type(util::time_since_posix_epoch().count()))
{
  // All 1s = all ports available.
  m_service_ports.set();
  m_ephemeral_ports.set();
  m_ephemeral_and_recent_ephemeral_ports.set();

  FLOW_LOG_INFO("Port_space [" << this << "] created; "
                "port PORT_ANY [" << S_PORT_ANY << "]; service ports [" << S_FIRST_SERVICE_PORT << ", "
                << (flow_port_sans_overflow_t(S_FIRST_SERVICE_PORT)
                    + flow_port_sans_overflow_t(S_NUM_SERVICE_PORTS) - 1) << "]; "
                "ephemeral ports [" << S_FIRST_EPHEMERAL_PORT << ", "
                << (flow_port_sans_overflow_t(S_FIRST_EPHEMERAL_PORT)
                    + flow_port_sans_overflow_t(S_NUM_EPHEMERAL_PORTS) - 1) << "].");
} // Port_space::Port_space()

bool Port_space::is_service_port(flow_port_t port) // Static.
{
  return util::in_closed_open_range(flow_port_sans_overflow_t(S_FIRST_SERVICE_PORT),
                                    flow_port_sans_overflow_t(port),
                                    flow_port_sans_overflow_t(S_FIRST_SERVICE_PORT)
                                      + flow_port_sans_overflow_t(S_NUM_SERVICE_PORTS));
}

flow_port_t Port_space::reserve_port(flow_port_t port, Error_code* err_code)
{
  if (port == S_PORT_ANY)
  {
    return reserve_ephemeral_port(err_code);
  }
  // else

  // Explicitly disallow reserving specific ephemeral ports.
  if (!is_service_port(port))
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_INVALID_SERVICE_PORT_NUMBER);
    FLOW_LOG_TRACE("Port was [" << port << "].");
    return S_PORT_ANY;
  }
  // else

  // Bits are indexed from 0, service ports from S_FIRST_SERVICE_PORT.
  const size_t port_bit_idx = port - S_FIRST_SERVICE_PORT;

  // Ensure not trying to reserve an already reserved service port.
  if (!m_service_ports.test(port_bit_idx))
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_PORT_TAKEN);
    FLOW_LOG_TRACE("Port was [" << port << "].");
    return S_PORT_ANY;
  }
  // else

  // Record as reserved.
  m_service_ports.set(port_bit_idx, false);

  err_code->clear();
  FLOW_LOG_INFO("Flow service port [" << port << "] reserved.");
  return port;
} // Port_space::reserve_port()

flow_port_t Port_space::reserve_ephemeral_port(Error_code* err_code)
{
  flow_port_t port = S_PORT_ANY; // Failure result.

  // Find a random available and not recently used ephemeral port.
  size_t port_bit_idx = find_available_port_bit_idx(m_ephemeral_and_recent_ephemeral_ports);
  if (port_bit_idx != Bit_set::npos)
  {
    // Found (typical).  Just reserve that port then.

    // Note: This shouldn't overflow.
    assert((flow_port_sans_overflow_t(port_bit_idx) + flow_port_sans_overflow_t(S_FIRST_EPHEMERAL_PORT))
           // Subtlety: The following exceeds domain of flow_port_t but not of flow_port_sans_overflow_t.
           < (flow_port_sans_overflow_t(S_FIRST_EPHEMERAL_PORT) + flow_port_sans_overflow_t(S_NUM_EPHEMERAL_PORTS)));
    port = port_bit_idx + S_FIRST_EPHEMERAL_PORT;
    m_ephemeral_ports.set(port_bit_idx, false);

    // Port not being returned => no effect on m_recent_ephemeral_ports.

    /* Maintain invariant: 0 in m_ephemeral_ports => 0 in m_ephemeral_and_recent_ephemeral_ports.
     * No effect on m_recent_ephemeral_ports => no effect on other invariant. */
    m_ephemeral_and_recent_ephemeral_ports.set(port_bit_idx, false);
  }
  else // if (port_bit_idx == Bit_set::npos) &&
    if (!m_recent_ephemeral_ports.empty())
  {
    // No not-recently-used available ports... but there are still some recently-used availables ones left.

    // Use the top of the queue: the port least recently added to the FIFO.
    port = m_recent_ephemeral_ports.front();
    m_recent_ephemeral_ports.pop();
    FLOW_LOG_INFO("Ran out of non-recently used and available Net-Flow ports; "
                  "reserving oldest recently used available Net-Flow port [" << port << "].");

    /* Port not being returned => no effect on m_recent_ephemeral_ports.
     * Port changing from recently-used-but available to reserved => still should be 0 to maintain invariant. */

    // Reserve port (and sanity-check some stuff).
    port_bit_idx = port - S_FIRST_EPHEMERAL_PORT;
    // If it was in the FIFO, it should be 0 in this guy (invariant).
    assert(!m_ephemeral_and_recent_ephemeral_ports.test(port_bit_idx));
    // If it was in the FIFO, it was available, so should not be 0 in this guy.
    assert(m_ephemeral_ports.test(port_bit_idx));
    // Reserve.
    m_ephemeral_ports.set(port_bit_idx, false);
  }
  // else simply no ephemeral ports available.

  if (port == S_PORT_ANY)
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_OUT_OF_PORTS);
  }
  else
  {
    err_code->clear();
    FLOW_LOG_INFO("Net-Flow ephemeral port [" << port << "] reserved.");
  }

  FLOW_LOG_TRACE("Available ephemeral port count [" << m_ephemeral_ports.count() << "]; "
                 "recently used ephemeral port count [" << m_recent_ephemeral_ports.size() << "].");
  return port;
} // Port_space::reserve_ephemeral_port()

void Port_space::return_port(flow_port_t port, Error_code* err_code)
{
  // Check if it's a service port.
  if (is_service_port(port))
  {
    // Straightforward; just return it if it's really reserved.
    assert(port >= S_FIRST_SERVICE_PORT);
    const size_t port_bit_idx = port - S_FIRST_SERVICE_PORT;

    // Check for "double-return."
    if (m_service_ports.test(port_bit_idx))
    {
      FLOW_ERROR_EMIT_ERROR(error::Code::S_INTERNAL_ERROR_PORT_NOT_TAKEN);
      FLOW_LOG_TRACE("Port was [" << port << "].");
      return;
    }
    // else
    m_service_ports.set(port_bit_idx, true);

    err_code->clear();
    FLOW_LOG_INFO("Net-Flow service port [" << port << "] returned.");
    return;
  } // if (service port)
  // else

  // Check if it's an ephemeral port.
  if (util::in_closed_open_range(flow_port_sans_overflow_t(S_FIRST_EPHEMERAL_PORT),
                                 flow_port_sans_overflow_t(port),
                                 // Subtlety: The following would probably overflow without the conversion.
                                 flow_port_sans_overflow_t(S_FIRST_EPHEMERAL_PORT)
                                   + flow_port_sans_overflow_t(S_NUM_EPHEMERAL_PORTS)))
  {
    assert(port >= S_FIRST_EPHEMERAL_PORT);
    const size_t port_bit_idx = port - S_FIRST_EPHEMERAL_PORT;

    // Check for "double-return."
    if (m_ephemeral_ports.test(port_bit_idx))
    {
      FLOW_ERROR_EMIT_ERROR(error::Code::S_INTERNAL_ERROR_PORT_NOT_TAKEN);
      FLOW_LOG_TRACE("Port was [" << port << "].");
      return;
    }
    // else

    // Sanity-check invariant (0 in m_ephemeral_ports => 0 in m_ephemeral_and_recent_ephemeral_ports).
    assert(!m_ephemeral_and_recent_ephemeral_ports.test(port_bit_idx));

    // Unreserve.
    m_ephemeral_ports.set(port_bit_idx, true);

    // As promised, any newly returned port goes to the back of the queue (most recent).
    m_recent_ephemeral_ports.push(port);

    // If we've thus exceeded max size of queue, free the least recent "recent" port of "recent" status.
    if (m_recent_ephemeral_ports.size() > size_t(S_MAX_RECENT_EPHEMERAL_PORTS))
    {
      // Front of queue = oldest.
      const flow_port_t port = m_recent_ephemeral_ports.front();
      assert(port >= S_FIRST_EPHEMERAL_PORT);
      const size_t port_bit_idx = port - S_FIRST_EPHEMERAL_PORT;
      m_recent_ephemeral_ports.pop();

      // Sanity-check we always clip the queue right after the maximum is exceeded.
      assert(m_recent_ephemeral_ports.size() == S_MAX_RECENT_EPHEMERAL_PORTS);
      // Sanity-check: recently used port we're evicting was indeed not reserved.
      assert(m_ephemeral_ports.test(port_bit_idx));
      // Sanity-check invariant: port was in m_recent_ephemeral_ports => bit is 0 in this structure.
      assert(!m_ephemeral_and_recent_ephemeral_ports.test(port_bit_idx));

      // Maintain invariant: all other ports are 1 in m_ephemeral_and_recent_ephemeral_ports.
      m_ephemeral_and_recent_ephemeral_ports.set(port_bit_idx, true);
    }

    err_code->clear();
    FLOW_LOG_INFO("Net-Flow ephemeral port [" << port << "] returned.");
    FLOW_LOG_TRACE("Available ephemeral port count [" << m_ephemeral_ports.count() << "]; "
                   "recently used ephemeral port count [" << m_recent_ephemeral_ports.size() << "].");
    return;
  } // else if (ephemeral port)
  // else

  // Invalid port number => it was never reserved.  Yeah.  Am I wrong?  I am not.
  FLOW_ERROR_EMIT_ERROR(error::Code::S_INTERNAL_ERROR_PORT_NOT_TAKEN);
  FLOW_LOG_TRACE("Port was [" << port << "].");
} // Port_space::return_port()

size_t Port_space::find_available_port_bit_idx(const Bit_set& ports)
{
  using boost::random::uniform_int_distribution;

  // Pick a random bit in bit field.
  uniform_int_distribution<size_t> range(0, ports.size() - 1);
  size_t port_bit_idx = range(m_rnd_generator);

  // If that bit is 0, go right until you find a 1.
  if (!ports.test(port_bit_idx))
  {
    port_bit_idx = ports.find_next(port_bit_idx);
    // If no 1s to the right, wrap around at the beginning and find the first 1.
    if (port_bit_idx == Bit_set::npos)
    {
      port_bit_idx = ports.find_first();
      // And if still no 1s found, then so be it (return npos).
    }
  }
  return port_bit_idx;
} // Port_space::find_available_port_bit_idx

} // namespace flow::net_flow
