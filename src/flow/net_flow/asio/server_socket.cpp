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

namespace flow::net_flow::asio
{

// Implementations.

// Server_socket implementations.

Server_socket::Server_socket(log::Logger* logger_ptr, const Peer_socket_options* child_sock_opts) :
  net_flow::Server_socket(logger_ptr, child_sock_opts),
  m_target_task_engine(0)
{
  // Only print pointer value, because most members are garbage at this point.
  FLOW_LOG_TRACE("boost.asio-integrated Server_socket [" << static_cast<void*>(this) << "] created; no Task_engine.");
}

Server_socket::~Server_socket() // Virtual.
{
  FLOW_LOG_TRACE("boost.asio-integrated Server_socket [" << this << "] destroyed.");
}

util::Task_engine* Server_socket::async_task_engine()
{
  // Not thread-safe against set_async_task_engine().  Discussed in my doc header.
  return m_target_task_engine;
}

const util::Task_engine& Server_socket::async_task_engine_cref() const
{
  return *m_target_task_engine; // Same comment as async_task_engine().
}

void Server_socket::set_async_task_engine(util::Task_engine* target_async_task_engine)
{
  // Not thread-safe against m_target_task_engine access.  Discussed in my doc header.
  FLOW_LOG_INFO("Object [" << this << "] has been assigned an Task_engine at [" << target_async_task_engine << "]; "
                "currently [" << static_cast<void*>(m_target_task_engine) << "].");
  m_target_task_engine = target_async_task_engine;
}

void Server_socket::async_accept_impl(Handler_func&& on_result,
                                      const Fine_time_pt& wait_until, bool reactor_pattern)
{
  // This is the actual async_accept() body, after the args have been converted into a convenient form.

  // Need owner Node* to proceed.
  assert(m_target_task_engine); // Note: cannot return civilized error, as nowhere to place error-receiving callback!
  const auto owner_node = static_cast<Node*>(node());
  if (!owner_node)
  {
    /* No Node* anymore => state is CLOSED, and reason why it was closed should be here.  This is how we report
     * attempts to do the non-blocking ops like accept() on CLOSED server sockets, and we are no different. */
    const auto err_code = disconnect_cause();

    FLOW_LOG_WARNING("Cannot perform async op on object [" << this << "]: it is already closed for "
                     "reason [" << err_code << '/' << err_code.message() << "].");
    on_result(err_code, Peer_socket::Ptr()); // It post()s user's originally-passed-in handler.
    return;
  }
  // else
  assert(!on_result.empty()); // Feeble attempt to ensure on_result not blown away, because no error above.

  const auto serv = cast(shared_from_this());

  /* The wrapper function allows to reactor_pattern mode,
   * wherein any actual I/O op is up to the handler on_result, and we just call it when ready.
   * In particular no need to waste time on wrapping the non-blocking I/O op call, since we won't be
   * doing it ourselves. */
  Function<Peer_socket::Ptr (Error_code*)> non_blocking_func;
  if (!reactor_pattern)
  {
    non_blocking_func = [serv](Error_code* err_code) -> Peer_socket::Ptr
    {
      return Peer_socket::cast(serv->accept(err_code));
    };
  }
  // else { Leave it .empty() to indicate reactor_pattern. }

  // Invoke this jack-of-all-trades, just supplying the op type-specific little pieces of the puzzle.
  owner_node->async_op
    <Server_socket, net_flow::Server_socket, Peer_socket::Ptr>
    (serv,
     std::move(non_blocking_func),
     0, Event_set::Event_type::S_SERVER_SOCKET_ACCEPTABLE, wait_until,
     std::move(on_result));
}

Server_socket::Ptr Server_socket::cast(net_flow::Server_socket::Ptr serv) // Static.
{
  using boost::static_pointer_cast;
  return static_pointer_cast<Server_socket>(serv);
}

// Node implementations (dealing with individual Server_sockets).

net_flow::Server_socket* Node::serv_create(const Peer_socket_options* child_sock_opts) // Virtual.
{
  // An asio::Node always creates asio::Server_socket (subclass) instances.
  const auto serv = static_cast<Server_socket*>(serv_create_forward_plus_ctor_args<Server_socket>(child_sock_opts));

  // As promised, propagate Node's service to created kid sockets (even if null).  They can overwrite.
  serv->set_async_task_engine(m_target_task_engine); // Thread-safe before we give out serv to others.

  return static_cast<net_flow::Server_socket*>(serv);
}

// Free implementations.

std::ostream& operator<<(std::ostream& os, const Server_socket* serv)
{
  return
    serv
      ? (os << "Asio_flow_server [Task_engine@" << static_cast<const void*>(&(serv->async_task_engine_cref())) << "] ["
            << static_cast<const net_flow::Server_socket*>(serv) // Show underlying net_flow:: socket's details.
            << "] @" << static_cast<const void*>(serv))
      : (os << "Asio_flow_server@null");
}

} // namespace flow::net_flow::asio
