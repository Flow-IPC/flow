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
#pragma once

#include "flow/net_flow/detail/net_flow_fwd.hpp"
#include "flow/net_flow/net_flow_fwd.hpp"
#include "flow/net_flow/error/error.hpp"
#include "flow/log/log.hpp"
#include "flow/util/random.hpp"
#include <boost/dynamic_bitset.hpp>
#include <boost/utility.hpp>
#include <boost/random.hpp>
#include <queue>

namespace flow::net_flow
{

/**
 * Internal `net_flow` class that maintains the available Flow-protocol port space, somewhat similarly to the
 * classic TCP or UDP port scheme.  The class's main use case it to be stored in a Node, though it
 * could be used in other scenarios of desired.  `net_flow` users should not use this class directly.
 *
 * The port scheme is similar to TCP or UDP's but not identical.  net_flow::flow_port_t maximum value
 * defines the total number of ports available.  Port #S_PORT_ANY is a special value and cannot be
 * used as a port.  The remaining port space is divided into 2 parts: #S_NUM_SERVICE_PORTS "service
 * ports" and #S_NUM_EPHEMERAL_PORTS "ephemeral ports."  Thus the port ranges are
 * [`S_FIRST_SERVICE_PORT`, `S_FIRST_SERVICE_PORT + S_NUM_SERVICE_PORTS - 1`] and
 * [`S_FIRST_EPHEMERAL_PORT`, `S_FIRST_EPHEMERAL_PORT + S_NUM_EPHEMERAL_PORTS - 1`], respectively.  Both
 * types of ports are on the same number line so that Flow-protocol socket addressing can be symmetrical
 * between client and server once a connection has been set up.
 *
 * At construction all ports are available.  A port can then be "reserved" by the class user.  That
 * port can then no longer be reserved until it is "returned" via return_port().
 *
 * One can either reserve to a specified service port (reserve_port()) or a random available
 * ephemeral port (reserve_ephemeral_port()).  Unlike with TCP and UDP ports, it's not possible to
 * reserve a user-supplied ephemeral port (reserve_port() will fail in this case).  Therefore,
 * service ports and ephemeral ports are entirely separate (it is not merely a convention as in TCP
 * and UDP ports).
 *
 * While this is transparent to the class user, this implementation avoids reserving ephemeral ports
 * that have been recently returned, to avoid confusion in the socket implementation.  It does so by
 * keeping a queue (of some reasonably large length up to a maximum size) of such ports.  Ports
 * within this queue are reserved only if there is absolutely no ephemeral port space left other
 * than the ports on the queue.
 *
 * Main (all?) differences from TCP/UDP ports: different allocation of ephemeral vs. non-ephemeral
 * port space; no privileged ports; enforced inability to reserve a specific ephemeral port; no
 * service name -> port number mapping (yet?).
 *
 * ### Thread safety ###
 * Separate objects: All functionality safe for simultaneous execution from multiple
 * threads.  Same object: Not safe to execute a non-const operation simultaneously with any other
 * operation; otherwise safe.  Rationale: Node only deals with its Port_space in thread W.
 *
 * ### Implementation notes ###
 * `bit_set` objects are used to represent the set of reserved vs. available ports of a
 * given type (0 means taken, 1 means available).  This is not only quick and memory-compact but
 * also allows a convenient search for a random available port (random bit -> `find_next()` if not
 * open -> `find_first()` if still not found -> no ports available if even still not found).
 *
 * @todo While the ephemeral port reuse avoidance works well within a single Port_space lifetime,
 * it does not carry over across different Port_spaces.  I.e., if Port_space is destructed and then
 * constructed, the container of recently used ports starts out empty, so a port may (with some low
 * but sizable probability) be reused.  This could be avoided by persisting (e.g., via
 * boost.serialization) those structures to disk (perhaps at a user-supplied
 * `boost::filesystem::path`) in `~Port_space()`.  This wouldn't work if Port_space crashed, however.  To
 * solve that, we could easily start a thread in Port_space() (and join it in `~Port_space()`) that
 * would save that state every N seconds.  If that's not enough, we can instead have that thread run
 * a boost.asio `io_service::run()` event loop and `io_service::post()` the save operation onto it (from
 * arbitrary threads) each time a new ephemeral port is reserved (with some kind of protection
 * against incessant disk writing built into this worker thread; e.g., don't save if saved in the
 * last N msec).
 */
class Port_space :
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Constants.

  /// Total number of ports in the port space, including #S_PORT_ANY.
  static const size_t S_NUM_PORTS;

  /// Total number of "service" ports (ones that can be reserved by number with reserve_port()).
  static const size_t S_NUM_SERVICE_PORTS;

  /// Total number of "ephemeral" ports (ones reserved at random with reserve_ephemeral_port()).
  static const size_t S_NUM_EPHEMERAL_PORTS;

  /// The port number of the lowest service port.
  static const flow_port_t S_FIRST_SERVICE_PORT;

  /// The port number of the lowest ephemeral port.
  static const flow_port_t S_FIRST_EPHEMERAL_PORT;

  // Constructors/destructor.

  /**
   * Constructs the Port_space with all ports available.
   *
   * @param logger
   *        The Logger implementation to use subsequently.
   */
  explicit Port_space(log::Logger* logger);

  // Methods.

  /**
   * Reserve the specified service port, or reserve_ephemeral_port() if the specified port is
   * #S_PORT_ANY.
   *
   * @param port
   *        A valid and still available service port number, or #S_PORT_ANY.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_INVALID_SERVICE_PORT_NUMBER, error::Code::S_PORT_TAKEN, or
   *        whatever set by reserve_ephemeral_port() if `port == S_PORT_ANY`.
   * @return On success, the reserved port; on failure, #S_PORT_ANY.
   */
  flow_port_t reserve_port(flow_port_t port, Error_code* err_code);

  /**
   * Reserve a randomly chosen available ephemeral port.
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_OUT_OF_PORTS.
   * @return On success, the reserved port; on failure, #S_PORT_ANY.
   */
  flow_port_t reserve_ephemeral_port(Error_code* err_code);

  /**
   * Return a previously reserved port (of any type).
   *
   * @param port
   *        A previously reserved port.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  error::Code generated:
   *        error::Code::S_PORT_TAKEN.
   */
  void return_port(flow_port_t port, Error_code* err_code);

private:
  // Types.

  /// Short-hand for bit set of arbitary length, representing a port set (each bit is a port; 1 open, 0 reserved).
  using Bit_set = boost::dynamic_bitset<>;

  /// Random number generator.
  using Random_generator = util::Rnd_gen_uniform_range_base::Random_generator;

  /// A type same as #flow_port_t but larger, useful when doing arithmetic that might hit overflow in corner cases.
  using flow_port_sans_overflow_t = uint32_t;

  // Type checks.
  static_assert((std::numeric_limits<flow_port_sans_overflow_t>::is_integer // Mostly silly sanity checks....
                   == std::numeric_limits<flow_port_t>::is_integer)
                  && (std::numeric_limits<flow_port_sans_overflow_t>::is_signed
                        == std::numeric_limits<flow_port_t>::is_signed)
                  && (sizeof(flow_port_sans_overflow_t) > sizeof(flow_port_t)), // ...but main point is here.
                "flow_port_sans_overflow_t must be similar to flow_port_t but with larger max value.");

  // Methods.

  /**
   * Returns `true` if and only if the given port is a service port (as opposed to ephemeral or
   * #S_PORT_ANY).
   *
   * @param port
   *        Port to check.
   * @return See above.
   */
  static bool is_service_port(flow_port_t port);

  /**
   * Helper method that, given a reference to a bit set representing available ports (1 available, 0
   * reserved) finds a random available (1) port in that bit set.
   *
   * @param ports
   *        A bit set representing port availability (e.g., #m_ephemeral_ports).
   * @return The bit index of a random available port, or `Bit_set::npos` if all are taken.
   */
  size_t find_available_port_bit_idx(const Bit_set& ports);

  // Constants.

  /// The maximum size of #m_recent_ephemeral_ports.
  static const size_t S_MAX_RECENT_EPHEMERAL_PORTS;

  // Data.

  /**
   * Current service port set.  Bit 0 is port #S_FIRST_SERVICE_PORT, bit 1 is `S_FIRST_SERVICE_PORT + 1`,
   * etc.  In particular, this starts off as all 1s (all ports open).
   */
  Bit_set m_service_ports;

  /**
   * Current ephemeral port set; indexing analogous to #m_service_ports but starting at
   * #S_FIRST_EPHEMERAL_PORT.
   */
  Bit_set m_ephemeral_ports;

  /**
   * Set representing the union of the set of current reserved ephemeral ports and the set of the
   * last up-to-#S_MAX_RECENT_EPHEMERAL_PORTS returned and available ephemeral ports.  In other
   * words, the union of the ports represented by #m_ephemeral_ports and #m_recent_ephemeral_ports.
   * In yet other words, a 1 bit represents an available and not-recently-used port.  This is kept
   * for performance, so that when choosing a random port one can find a random 1 bit to find a
   * not-recently-used and open port.
   *
   * Invariants:
   *   - Port `P` is in #m_recent_ephemeral_ports => bit `(P - S_FIRST_EPHEMERAL_PORT)` is 0 in this.
   *   - Bit `N` is 0 in #m_ephemeral_ports => bit `N` is 0 in this.
   *   - All other bits are 1.
   */
  Bit_set m_ephemeral_and_recent_ephemeral_ports;

  /**
   * A FIFO of recently used but currently available ephemeral ports.  Algorithm: push port onto
   * back of queue when it's returned; pop front of queue if #m_recent_ephemeral_ports is beyond
   * #S_MAX_RECENT_EPHEMERAL_PORTS long.  If port is needed and no more
   * non-#m_ephemeral_and_recent_ephemeral_ports ports are available, pop front of queue (i.e.,
   * oldest recently used port) and use that.  If emptied, there are simply no more ports left.
   */
  std::queue<flow_port_t> m_recent_ephemeral_ports;

  /// Random number generator for picking ports.
  Random_generator m_rnd_generator;
}; // class Port_space

} // namespace flow::net_flow
