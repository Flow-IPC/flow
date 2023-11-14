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

#include "flow/common.hpp"
#include <ostream>

namespace flow::net_flow
{
// Types.

// Find doc headers near the bodies of these compound types.

class Event_set;
class Net_env_simulator;
class Node;
struct Node_options;
class Peer_socket;
struct Peer_socket_receive_stats;
struct Peer_socket_info;
struct Peer_socket_options;
class Peer_socket_send_stats;
struct Remote_endpoint;
class Server_socket;

/// Result of a send or receive operation, used at least in stat reporting.
enum class Xfer_op_result
{
  /// Bytes transferred equals bytes expected.
  S_FULLY_XFERRED,
  /// Bytes transferred less than bytes expected.
  S_PARTIALLY_XFERRED,
  /// Error occurred -- probably no bytes transferred.
  S_ERROR
};

/// Logical Flow port type (analogous to a UDP/TCP port in spirit but in no way relevant to UDP/TCP).
using flow_port_t = uint16_t;

// Constants.

/**
 * Special Flow port value used to indicate "invalid port" or "please pick a random available ephemeral
 * port," depending on the context.  This is spiritually equivalent to TCP's port 0.
 */
extern const flow_port_t S_PORT_ANY;

// Free functions.

/**
 * Prints string representation of given socket to given standard `ostream` and returns the
 * latter.  The representation includes the local and remote endpoints and the hex pointer value.
 *
 * @relatesalso Peer_socket
 *
 * @note `shared_ptr` forwards `ostream` output to the underlying pointer type, so this will affect `Ptr`
 *       output as well.
 * @param os
 *        Stream to print to.
 * @param sock
 *        Object to serialize.  May be null.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Peer_socket* sock);

/**
 * Prints string representation of given socket to given standard `ostream` and returns the
 * latter.  The representation includes the local endpoint and the hex pointer value.
 *
 * @relatesalso Server_socket
 *
 * @note `shared_ptr` forwards `ostream` output to the underlying pointer type, so this will affect `Ptr`
 *       output as well.
 * @param os
 *        Stream to print to.
 * @param serv
 *        Object to serialize.  May be null.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Server_socket* serv);

/**
 * Prints string representation of the stats in the given stats object to the standard `ostream` and
 * returns the latter.  The representation is multi-line but ends in no newline.
 *
 * @relatesalso Peer_socket_receive_stats
 *
 * @param os
 *        Stream to which to print.
 * @param stats
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Peer_socket_receive_stats& stats);

/**
 * Prints string representation of the stats in the given stats object to the standard `ostream` and
 * returns the latter.  The representation is multi-line but ends in no newline.
 *
 * @relatesalso Peer_socket_send_stats
 *
 * @param os
 *        Stream to which to print.
 * @param stats
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Peer_socket_send_stats& stats);

/**
 * Prints string representation of the stats in the given stats object to the standard `ostream` and
 * returns the latter.  The representation is multi-line but ends in no newline.
 *
 * @relatesalso Peer_socket_info
 *
 * @param os
 *        Stream to which to print.
 * @param stats
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Peer_socket_info& stats);

/**
 * Prints the name of each option in the given Node_options, along with its
 * current value, to the given `ostream`.
 *
 * @param os
 *        Stream to which to serialize.
 * @param opts
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Node_options& opts);

/**
 * Prints the name of each option in the given Peer_socket_options, along with its
 * current value, to the given `ostream`.
 *
 * @param os
 *        Stream to which to serialize.
 * @param opts
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Peer_socket_options& opts);

/**
 * Prints string representation of the given Remote_endpoint to the given `ostream`.
 *
 * @relatesalso Remote_endpoint
 *
 * @param os
 *        Stream to which to write.
 * @param endpoint
 *        Endpoint to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Remote_endpoint& endpoint);

/**
 * Whether `lhs` is equal to `rhs`.
 *
 * @relatesalso Remote_endpoint
 * @param lhs
 *        Object to compare.
 * @param rhs
 *        Object to compare.
 * @return See above.
 */
bool operator==(const Remote_endpoint& lhs, const Remote_endpoint& rhs);

/**
 * Free function that returns `remote_endpoint.hash()`; has to be a free function named `hash_value`
 * for boost.hash to pick it up.
 *
 * @relatesalso Remote_endpoint
 *
 * @param remote_endpoint
 *        Object to hash.
 * @return `remote_endpoint.hash()`.
 */
size_t hash_value(const Remote_endpoint& remote_endpoint);

} // namespace flow::net_flow


/**
 * Contains classes that add boost.asio integration to the main Flow-protocol
 * classes such as net_flow::Node and net_flow::Peer_socket,
 * so that `net_flow` sockets can be easily used in boost.asio-driven event loops, e.g., ones also performing
 * TCP networking and scheduling timers.
 *
 * @see Main class net_flow::asio::Node.
 */
namespace flow::net_flow::asio
{
// Types.

// Find doc headers near the bodies of these compound types.

class Node;
class Peer_socket;
class Server_socket;

// Free functions.

/**
 * Prints string representation of given socket to given standard `ostream` and returns the
 * latter.  The representation includes the local and remote endpoints and the hex pointer value.
 *
 * @relatesalso Peer_socket
 *
 * @note `shared_ptr` forwards `ostream` output to the underlying pointer type, so this will affect `Ptr`
 *       output as well.
 * @param os
 *        Stream to print to.
 * @param sock
 *        Object to serialize.  May be null.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Peer_socket* sock);

/**
 * Prints string representation of given socket to given standard `ostream` and returns the
 * latter.  The representation includes the local and remote endpoints and the hex pointer value.
 *
 * @relatesalso Server_socket
 *
 * @note `shared_ptr` forwards `ostream` output to the underlying pointer type, so this will affect `Ptr`
 *       output as well.
 * @param os
 *        Stream to print to.
 * @param serv
 *        Object to serialize.  May be null.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Server_socket* serv);

} // namespace flow::net_flow::asio
