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

// Peer_socket implementations.

Peer_socket::Peer_socket(log::Logger* logger_ptr,
                         util::Task_engine* task_engine, const Peer_socket_options& opts) :
  net_flow::Peer_socket(logger_ptr, task_engine, opts),
  m_target_task_engine(0)
{
  // Only print pointer value for same reason as in super-constructor.
  FLOW_LOG_TRACE("boost.asio-integrated Peer_socket [" << static_cast<void*>(this) << "] created; no Task_engine.");
}

Peer_socket::~Peer_socket() // Virtual.
{
  FLOW_LOG_TRACE("boost.asio-integrated Peer_socket [" << this << "] destroyed.");
}

util::Task_engine* Peer_socket::async_task_engine()
{
  // Not thread-safe against set_async_task_engine().  Discussed in my doc header.
  return m_target_task_engine;
}

const util::Task_engine& Peer_socket::async_task_engine_cref() const
{
  return *m_target_task_engine; // Same comment as async_task_engine().
}

void Peer_socket::set_async_task_engine(util::Task_engine* target_async_task_engine)
{
  // Not thread-safe against m_target_task_engine access.  Discussed in my doc header.
  FLOW_LOG_INFO("Object [" << this << "] has been assigned an Task_engine at [" << target_async_task_engine << "]; "
                "currently [" << static_cast<void*>(m_target_task_engine) << "].");
  m_target_task_engine = target_async_task_engine;
}

void Peer_socket::async_receive_impl(Target_bufs_ptr target, Handler_func&& on_result,
                                     const Fine_time_pt& wait_until)
{
  // This is the actual async_receive() body, after the args have been converted into a convenient form.

  auto owner_node = node_or_post_error(std::move(on_result));
  if (!owner_node)
  {
    return; // on_result() error invocation posted.  (BTW on_result() may be blown away at this point.)  Done.
  }
  // else
  assert(!on_result.empty()); // Feeble sanity check: node_or_post_error() can only touch on_result on error.

  /* The wrapper function guarantees `target` is null if and only if they desired this mode,
   * wherein any actual I/O op is up to the handler on_result, and we just call it when ready.
   * In particular no need to waste time on wrapping the non-blocking I/O op call, since we won't be
   * doing it ourselves. */
  const bool reactor_pattern = !target;

  const auto sock = cast(shared_from_this());

  /* Supply the closure that'll execute actual non-blocking op when appropriate, returning the result and
   * setting the passed-in Error_code. */
  Function<size_t (Error_code*)> non_blocking_func;
  if (!reactor_pattern)
  {
    // Perf note: The `target` capture is just a ref-counted pointer copy.
    non_blocking_func = [sock, target](Error_code* err_code) -> size_t { return sock->receive(*target, err_code); };
  }
  // else { Leave it blank; won't be used. }

  // Invoke this jack-of-all-trades, just supplying the op type-specific little pieces of the puzzle.
  owner_node->async_op
    <Peer_socket, net_flow::Peer_socket, size_t>
    (sock,
     std::move(non_blocking_func),
     0, Event_set::Event_type::S_PEER_SOCKET_READABLE, wait_until,
     std::move(on_result));
  // on_result, non_blocking_func may now be dead due to move(), but we are done with them (and everything else).
}

void Peer_socket::async_send_impl(Source_bufs_ptr source, Handler_func&& on_result,
                                  const Fine_time_pt& wait_until)
{
  // Much like async_receive_impl().  Keeping comments light.

  auto owner_node = node_or_post_error(std::move(on_result));
  if (!owner_node)
  {
    return;
  }
  // else
  assert(!on_result.empty());

  const bool reactor_pattern = !source;
  const auto sock = cast(shared_from_this());

  Function<size_t (Error_code*)> non_blocking_func;
  if (!reactor_pattern)
  {
    non_blocking_func = [sock, source](Error_code* err_code) -> size_t { return sock->send(*source, err_code); };
  }

  owner_node->async_op
    <Peer_socket, net_flow::Peer_socket, size_t>
    (sock,
     std::move(non_blocking_func),
     0, Event_set::Event_type::S_PEER_SOCKET_WRITABLE, wait_until,
     std::move(on_result));
}

Node* Peer_socket::node_or_post_error(Handler_func&& on_result)
{
  assert(async_task_engine()); // Note: cannot return civilized error, as nowhere to place error-receiving callback!

  const auto owner_node = static_cast<Node*>(node());
  if (!owner_node)
  {
    /* No Node* anymore => state is CLOSED, and reason why it was closed should be here.  This is how we report
     * attempts to do the non-blocking ops like receive() on CLOSED sockets, and we are no different. */
    const auto err_code = disconnect_cause();

    FLOW_LOG_WARNING("Cannot perform async op on object [" << this << "]: it is already closed for "
                     "reason [" << err_code << '/' << err_code.message() << "].");
    on_result(err_code, 0); // It post()s user's originally-passed-in handler.
    return 0;
  }
  // else
  return owner_node;
}

Peer_socket::Ptr Peer_socket::cast(net_flow::Peer_socket::Ptr sock) // Static.
{
  using boost::static_pointer_cast;
  return static_pointer_cast<Peer_socket>(sock);
}

// Node implementations (dealing with individual Peer_sockets).

net_flow::Peer_socket* Node::sock_create(const Peer_socket_options& opts) // Virtual.
{
  // An asio::Node always creates asio::Peer_socket (subclass) instances.
  const auto sock = static_cast<Peer_socket*>(sock_create_forward_plus_ctor_args<Peer_socket>(opts));

  // As promised, propagate Node's service to created kid sockets (even if null).  They can overwrite.
  sock->set_async_task_engine(m_target_task_engine); // Thread-safe before we give out sock to others.

  return static_cast<net_flow::Peer_socket*>(sock);
}

void Node::async_connect_impl(const Remote_endpoint& to, const Fine_duration& max_wait,
                              const boost::asio::const_buffer& serialized_metadata,
                              const Peer_socket_options* opts,
                              Handler_func&& on_result)
{
  using boost::asio::null_buffers;

  // We are in thread U != W.

  /* This follows similar beats to sync_connect_impl(); though ultimately it happens to be simpler
   * by leveraging async_send(). */

#ifndef NDEBUG
  const auto target_task_engine = async_task_engine();
#endif
  assert(target_task_engine); // Note: cannot return civilized error, as nowhere to place error-receiving callback!

  Error_code conn_err_code;
  const auto sock = Peer_socket::cast(connect_with_metadata(to, serialized_metadata, &conn_err_code, opts));
  if (!sock)
  {
    // It's probably some user error like an invalid destination.
    on_result(conn_err_code, Peer_socket::Ptr()); // It post()s user's originally-passed-in handler.
    return;
  }
  // else we have a socket that has started connecting.

  /* We must clean up sock (call sock->close_abruptly(&dummy)) at any return point (including
   * exception throw) below, EXCEPT the success case. */

  /* "Cheat": we just want it to be writable, indicating it is connected (e.g., see sync_connect_impl()).
   * Fortunately we can use async_send(null_buffers()) for exactly that purpose: it will await writability
   * (not even conceivably competing with any other entity awaiting same, since we haven't given socket to
   * anyone yet); but will not actually write anything when ready, merely calling our handler. */
  const auto on_writable = [this, on_result = std::move(on_result), sock]
                             (const Error_code& wait_err_code, [[maybe_unused]] size_t n_sent0)
  {
    assert(n_sent0 == 0);
    FLOW_LOG_TRACE("Async connect op for new socket [" << sock << "] detected writable status with "
                   "result code [" << wait_err_code << '/' << wait_err_code.message() << "].");

    if (wait_err_code == error::Code::S_WAIT_USER_TIMEOUT)
    {
      // Our contract is timeout => report failure to user = pass null socket; so close the doomed socket now.
      Error_code dummy_prevents_throw;
      sock->close_abruptly(&dummy_prevents_throw);

      on_result(wait_err_code, Peer_socket::Ptr()); // It post()s user's originally-passed-in handler.
      // *sock should lose all references and destruct shortly, as we didn't pass it to on_result().
    }
    else if (wait_err_code)
    {
      // error reaching initial writability, except timeout, should have led to closure of socket.
      assert(sock->state() == Peer_socket::State::S_CLOSED);
      /* So we won't even pass the short-lived socket to callback, indicating failure via null pointer.
       * See comment in sync_connect_impl() about how we avoid passing an error socket for user to discover. */

      on_result(wait_err_code, Peer_socket::Ptr()); // It post()s user's originally-passed-in handler.
      // As above, *sock should destruct soon.
    }
    else
    {
      assert(!wait_err_code);
      on_result(wait_err_code, Peer_socket::Ptr()); // It post()s user's originally-passed-in handler.
      // *sock lives on by being passed to them and probably saved by them!
    }

    FLOW_LOG_TRACE("Finished executing user handler for async connect of [" << sock << "].");
  };
  // `on_result` might be hosed now (move()d).

  // As advertised, this is expected (and we need it for the async_send() below, as it post()s on_writable()):
  assert(sock->async_task_engine() == target_task_engine);

  // Go!
  sock->async_send(null_buffers(), max_wait, on_writable);
} // Node::async_connect_impl()

// Free implementations.

std::ostream& operator<<(std::ostream& os, const Peer_socket* sock)
{
  return
    sock
      ? (os << "Asio_flow_socket "
               "[Task_engine@" << static_cast<const void*>(&(sock->async_task_engine_cref())) << "] ["
            << static_cast<const net_flow::Peer_socket*>(sock) // Show underlying net_flow:: socket's details.
            << "] @" << static_cast<const void*>(sock))
      : (os << "Asio_flow_socket@null");
}

} // namespace flow::net_flow::asio
