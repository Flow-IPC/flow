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
#include "flow/net_flow/node.hpp"
#include "flow/net_flow/peer_socket.hpp"
#include "flow/net_flow/detail/stats/bandwidth.hpp"
#include "flow/net_flow/detail/cong_ctl.hpp"
#include "flow/async/util.hpp"

namespace flow::net_flow
{

// Server_socket implementation.

Server_socket::Server_socket(log::Logger* logger_ptr, const Peer_socket_options* child_sock_opts) :
  Log_context(logger_ptr, Flow_log_component::S_NET_FLOW),
  /* If they supplied a Peer_socket_options, store a copy of it.  When new Peer_sockets are made
   * (when people connect to us), each peer socket's per-socket options will be copies of this.  If
   * they did not supply a Peer_socket_options, the Node's global Peer_socket_options will be used
   * for each subsequent Peer_socket. */
  m_child_sock_opts(child_sock_opts ? new Peer_socket_options{*child_sock_opts} : nullptr),
  m_state(State::S_CLOSED), // Incorrect; set explicitly.
  m_node(nullptr), // Incorrect; set explicitly.
  m_local_port(S_PORT_ANY) // Incorrect; set explicitly.
{
  // Only print pointer value, because most members are garbage at this point.
  FLOW_LOG_TRACE("Server_socket [" << static_cast<void*>(this) << "] created.");
}

Server_socket::~Server_socket()
{
  delete m_child_sock_opts; // May be null (that's okay).

  FLOW_LOG_TRACE("Server_socket [" << this << "] destroyed.");
}

Server_socket::State Server_socket::state() const
{
  Lock_guard lock{m_mutex}; // State is liable to change at any time.
  return m_state;
}

Node* Server_socket::node() const
{
  Lock_guard lock{m_mutex}; // m_node can simultaneously change to 0 if state changes to S_CLOSED.
  return m_node;
}

Error_code Server_socket::disconnect_cause() const
{
  Lock_guard lock{m_mutex};
  return m_disconnect_cause;
}

flow_port_t Server_socket::local_port() const
{
  return m_local_port; // No need to lock (it never changes).
}

Peer_socket::Ptr Server_socket::accept(Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Peer_socket::Ptr, accept, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in user thread U != W.

  Lock_guard lock{m_mutex};

  const Ptr serv = shared_from_this();
  if (!Node::ensure_sock_open(serv, err_code)) // Ensure it's open, so that we can access m_node.
  {
    return Peer_socket::Ptr{};
  }
  // else m_node is valid.

  // Forward the rest of the logic to Node, as is the general convention especially for logic affecting outside *this.
  return m_node->accept(serv, err_code);
} // Server_socket::accept()

Peer_socket::Ptr Server_socket::sync_accept(bool reactor_pattern, Error_code* err_code)
{
  return sync_accept_impl(Fine_time_pt{}, reactor_pattern, err_code);
}

Peer_socket::Ptr Server_socket::sync_accept_impl(const Fine_time_pt& wait_until, bool reactor_pattern,
                                                 Error_code* err_code)
{
  using boost::adopt_lock;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Peer_socket::Ptr, sync_accept_impl, wait_until, reactor_pattern, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in user thread U != W.

  Lock_guard lock{m_mutex};

  const Ptr serv = shared_from_this();
  if (!Node::ensure_sock_open(serv, err_code)) // Ensure it's open, so that we can access m_node.
  {
    return Peer_socket::Ptr{};
  }
  // else m_node is valid.

  lock.release(); // Release lock (mutex is still LOCKED).  sync_op() takes over holding the lock and unlocking.

  // See comment in Peer_socket::node_sync_send().

  /* Operating on Server_sockets, returning Peer_socket::Ptr; Event_set socket set type is
   * Server_sockets.
   * Object is serv; non-blocking operation is m_node->accept(...) -- or N/A in "reactor pattern" mode..
   * Peer_socket::Ptr{} is the "would-block" return value for this operation.
   * S_SERVER_SOCKET_ACCEPTABLE is the type of event to watch for here. */
  return m_node
           ->sync_op<Server_socket, Peer_socket::Ptr>
               (serv,
                reactor_pattern
                  ? Function<Peer_socket::Ptr ()>()
                  : Function<Peer_socket::Ptr ()>([this, serv, err_code]() -> Peer_socket::Ptr
                                                    { return m_node->accept(serv, err_code); }),
                Peer_socket::Ptr{}, Event_set::Event_type::S_SERVER_SOCKET_ACCEPTABLE,
                wait_until, err_code);
} // Server_socket::sync_accept_impl()

// Node implementations (methods dealing with individual Server_sockets).

Server_socket::Ptr Node::listen(flow_port_t local_port, Error_code* err_code,
                                const Peer_socket_options* child_sock_opts)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Server_socket::Ptr, listen, local_port, _1, child_sock_opts);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  using async::asio_exec_ctx_post;
  using async::Synchronicity;
  using boost::promise;
  using boost::unique_future;

  // We are in thread U != W.

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return Server_socket::Ptr{};
  }
  // else

  /* Basically we now need to do the following.
   *   -1- Reserve local_port in m_ports.
   *   -2- Create Server_socket serv.
   *   -3- Save serv in m_servs.
   *   (Both -1- and -3- can result in error.)
   *
   * -2- must be done in thread W, as m_servs is by design only to be accessed there.  -1-, however,
   * can be done in thread U, assuming m_ports is properly synchronized.  So here are our choices:
   *
   * I. Perform -1- in U.  If error, return.  Perform -2-.  Post callback to do -3- in W.  Return
   *    serv in U.  Meanwhile -2- will, at some point, be performed in W.  If error in -3-, save it
   *    in serv.  User will discover when trying to serv->accept().
   *
   * II. Perform -1- in U.  If error, return.  Perform -2-.  Post callback to do -3- in W.  Use a
   *     future/promise pair to wait for -3- to complete.  Meanwhile -3- will, at some point, be performed
   *     in W.  If error in -3-, save it in serv.  Either way, U will wait for the result and return
   *     error or serv to the user.
   *
   * III. Post callback to do -1-, -2-, -3- in W.  Use a future/promise pair to wait for -1-3- to complete.
   *      Meanwhile -1-3- will, at some point, be performed in W.  If error in -1-3-, save it in
   *      serv.  Either way, U will wait.  If error, return error to the user.  Else return serv to
   *      the user.
   *
   * Let's pick one.  III > II due to simpler code; future used either way but most code is in one
   * thread and one function (the W callback); the U part is a short wrapper.  Also, Port_space need
   * not be synchronized.  So it's I vs. III.  Advantage of I is speed; listen() will return in U
   * faster, especially if thread W is loaded with work.  (However III is still non-blocking, i.e.,
   * no waiting for network events.)  Advantage of III is simplicity of code (everything in one
   * thread and callback, in W; no explicit locking); and any pre-networking error is immediately
   * returned by listen() instead of being discovered later.  I believe here III wins, especially
   * because listen() should be fairly infrequent, so a split second speed difference should not be
   * significant.
   *
   * So we choose III.  We set up connect_worker() to run in W and wait for
   * it to succeed or fail.  asio_exec_ctx_post() does the promise/future stuff, or equivalent, so
   * the code is really simple. */

  // Load this onto thread W boost.asio work queue.  We won't return until it's done, so [&] is OK.
  Server_socket::Ptr serv;
  asio_exec_ctx_post(get_logger(), &m_task_engine, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION,
                     [&]() { listen_worker(local_port, child_sock_opts, &serv); });
  // If got here, the task has completed in thread W and signaled us to that effect.

  // listen_worker() indicates success or failure through this data member.
  if (serv->m_disconnect_cause)
  {
    *err_code = serv->m_disconnect_cause;
    return Server_socket::Ptr{}; // serv will go out of scope and thus will be destroyed.
  }
  // else
  err_code->clear();
  return serv;
} // Node::listen()

void Node::listen_worker(flow_port_t local_port, const Peer_socket_options* child_sock_opts,
                         Server_socket::Ptr* serv_ptr)
{
  assert(serv_ptr);

  // We are in thread W.  listen() is waiting for us to set serv_promise in thread U.

  // Create new socket and set all members that may be immediately accessed by user from thread U after we're done.

  auto& serv = *serv_ptr;
  if (child_sock_opts)
  {
    /* They provided custom per-socket options to distribute to any Peer_sockets created when people
     * connect to this server.  Before we give those to the new server socket, let's validate them
     * (for proper values and internal consistency, etc.). */

    Error_code err_code;
    const bool opts_ok = sock_validate_options(*child_sock_opts, nullptr, &err_code);

    // Due to the advertised interface of the current method, we must create a socket even on error.
    serv.reset(serv_create(child_sock_opts));

    // Now report error if indeed options were invalid.  err_code is already set and logged in that case.
    if (!opts_ok)
    {
      serv->m_disconnect_cause = err_code;
      return;
    }
    // else
  }
  else
  {
    /* More typically, they did not provide per-socket options.  So just pass null pointer into
     * Peer_socket constructor; this will mean that when a Peer_socket is generated on connection,
     * the code is to provide a copy of the global template for the per-socket options.  That will
     * happen later; we just pass in null. */
    serv.reset(serv_create(nullptr));
  }

  // Server socket created; set members.

  serv->m_node = this;
  serv_set_state(serv, Server_socket::State::S_LISTENING);

  // Allocate given port.
  serv->m_local_port = m_ports.reserve_port(local_port, &serv->m_disconnect_cause);
  if (serv->m_local_port == S_PORT_ANY)
  {
    // Error already logged and is in serv->m_disconnect_cause.
    return;
  }
  // else
  local_port = serv->m_local_port; // If they'd specified S_PORT_ANY, this is now a random port.

  FLOW_LOG_INFO("NetFlow worker thread listening for passive-connects on [" << serv << "].");

  if (util::key_exists(m_servs, local_port))
  {
    /* This is a passive connect (we're accepting future connections).  Therefore in particular it
     * should be impossible that our local_port() equals an already existing connection's
     * local_port(); Port_space is supposed to prevent the same port from being handed out to more
     * than one connection.  Therefore this must be a programming error. */

    FLOW_LOG_WARNING("Cannot set up [" << serv << "], because server at port [" << local_port << "] already exists!  "
                     "This is a port reservation error and constitutes either a bug or an extremely "
                     "unlikely condition.");

    // Mark/log error.
    Error_code* err_code = &serv->m_disconnect_cause;
    FLOW_ERROR_EMIT_ERROR(error::Code::S_INTERNAL_ERROR_PORT_COLLISION);

    // Return port.
    Error_code return_err_code;
    m_ports.return_port(serv->m_local_port, &return_err_code);
    assert(!return_err_code);
    return;
  } // if (that server entry already exists)
  // else

  m_servs[local_port] = serv; // Now SYNs will be accepted.
} // Node::listen_worker()

Peer_socket::Ptr Node::accept(Server_socket::Ptr serv, Error_code* err_code)
{
  // We are in user thread U != W.

  // IMPORTANT: The logic here must be consistent with serv_is_acceptable().

  // serv->m_mutex already locked.

  if (serv->m_state == Server_socket::State::S_CLOSING)
  {
    /* This is the same to the user as CLOSED -- only different in that serv has not been disowned by the Node yet.
     * See rationale for this in the accept() documentation header. */
    FLOW_ERROR_EMIT_ERROR_LOG_INFO(serv->m_disconnect_cause);

    // Not listening anymore; pretend nothing on queue.
    return Peer_socket::Ptr{};
  }
  // else
  assert(serv->m_state == Server_socket::State::S_LISTENING);

  if (serv->m_unaccepted_socks.empty())
  {
    // Nothing on the queue.  As advertised, this is not an error in LISTENING state.
    err_code->clear();
    return Peer_socket::Ptr{};
  }
  // else

  // Pop from queue.  Linked_hash_set queues things up at the front (via insert()), so pop from the back.
  const auto it = --serv->m_unaccepted_socks.cend();
  Peer_socket::Ptr sock = *it;
  serv->m_unaccepted_socks.erase(it);

  /* Now that it's accepted, remove reference to the server socket, so that when the server socket
   * is closed, sock is not closed (since it's a fully functioning independent socket now). */
  sock->m_originating_serv.reset(); // This is synchronized together with m_unaccepted_socks.

  FLOW_LOG_INFO("Connection [" << sock << "] on [" << serv << "] accepted.");

  err_code->clear();
  return sock;
} // Node::accept()

bool Node::serv_is_acceptable(const boost::any& serv_as_any) const
{
  using boost::any_cast;

  const Server_socket::Const_ptr serv = any_cast<Server_socket::Ptr>(serv_as_any);

  Peer_socket::Lock_guard lock{serv->m_mutex}; // Many threads can access/write below state.

  /* Our task here is to return true if and only if at this very moment calling serv->accept()would
   * yield either a non-null return value OR a non-success *err_code.  In other words,
   * accept() would return "something."  This is used for Event_set machinery.
   *
   * This should mirror accept()'s algorithm.  @todo Should accept() call this, for code reuse?
   * Maybe/maybe not.  Consider performance when deciding.
   *
   * Basically, CLOSING/CLOSED => error (Acceptable); LISTENING + non-empty accept queue =>
   * returns a socket (Acceptable); LISTENING + empty queue => returns null and no error (not
   * Acceptable).  So the latter is the only way (though quite common) it can be NOT Acceptable. */

  return !((serv->m_state == Server_socket::State::S_LISTENING) && serv->m_unaccepted_socks.empty());
} // Node::serv_is_acceptable()

void Node::close_empty_server_immediately(const flow_port_t local_port, Server_socket::Ptr serv,
                                          const Error_code& err_code, bool defer_delta_check)
{
  // We are in thread W.

  // Check explicitly documented pre-conditions.

  assert(serv->m_state != Server_socket::State::S_CLOSED);
  // Caller should have closed all the associated sockets already.
  assert(serv->m_connecting_socks.empty());
  {
    Server_socket::Lock_guard lock{serv->m_mutex}; // At least m_unaccepted_socks can be accessed by user.
    assert(serv->m_unaccepted_socks.empty());
  }

  FLOW_ERROR_LOG_ERROR(err_code);
  FLOW_LOG_INFO("Closing and destroying [" << serv << "].");

  serv_close_detected(serv, err_code, true); // Sets S_CLOSED public state (and related data).

  // Next, remove serv from our main server list.

#ifndef NDEBUG
  const bool erased = 1 ==
#endif
    m_servs.erase(local_port);
  assert(erased); // Not S_CLOSED => it's in m_servs.  Otherwise there's a serious bug somewhere.

  // Return the port.
  Error_code return_err_code;
  m_ports.return_port(local_port, &return_err_code);
  assert(!return_err_code);

  /* serv has changed to CLOSED state.  Performing serv->accept() would therefore
   * certainly return an error.  Returning an error from that method (as opposed to null but no
   * error) is considered Acceptable (as we want to alert the user to the error, so her wait [if
   * any] wakes up and notices the error).  Therefore we should soon inform anyone waiting on any
   * Event_sets for serv to become Acceptable
   *
   * Caveat: Similar to that in Node::handle_syn_ack_ack_to_syn_rcvd() at similar point in the
   * code. */

  // Accumulate the event into the Node store (note: not any Event_set yet).
  if (m_sock_events[Event_set::Event_type::S_SERVER_SOCKET_ACCEPTABLE].insert(serv).second)
  {
    // Possibly inform the user for any applicable Event_sets right now.
    event_set_all_check_delta(defer_delta_check);
  }
} // Node::close_empty_server_immediately()

void Node::serv_set_state(Server_socket::Ptr serv, Server_socket::State state)
{
  Server_socket::Lock_guard lock{serv->m_mutex};

  // @todo Add TRACE logging.

  serv->m_state = state;
  if (state == Server_socket::State::S_CLOSED)
  {
    /* Important convention: S_CLOSED means socket is permanently incapable of accepting more
     * connections.  At this point the originating Node removes the
     * socket from its internal structures.  Therefore, the Node itself may even go away -- while
     * this Server_socket still exists.  Since we use shared_ptr when giving our socket objects,
     * that's fine -- but we want to avoid returning an invalid Node* in node().  So, when
     * S_CLOSED, serv->m_node = nullptr. */
    serv->m_node = nullptr;
  }
}

Peer_socket::Ptr Node::handle_syn_to_listening_server(Server_socket::Ptr serv,
                                                      boost::shared_ptr<const Syn_packet> syn,
                                                      const util::Udp_endpoint& low_lvl_remote_endpoint)
{
  using util::Blob;
  using boost::random::uniform_int_distribution;

  // We are in thread W.

  /* We just got SYN (an overture from the other side).  Create a peer-to-peer socket to track that
   * connection being established. */

  Peer_socket::Ptr sock;
  if (serv->m_child_sock_opts)
  {
    /* They provided custom per-socket options in listen(), and we've been storing them in *serv.
     * They're already validated at listen() time, so we just give them to Peer_socket constructor,
     * which copies them. */
    sock.reset(sock_create(*serv->m_child_sock_opts));
  }
  else
  {
    /* More typically, they did not provide per-socket options.  So we just pass our global template
     * for the per-socket options to the Peer_socket constructor.  The only caveat is that template
     * may be concurrently changed, so we must lock it.  Could do it with opt(), but that introduces
     * an extra copy of the entire struct, so just do it explicitly (read-only lock for
     * performance).
     *
     * Note: no need to validate; global options (including per-socket ones) are validated
     * elsewhere when set. */
    Options_lock lock{m_opts_mutex};
    sock.reset(sock_create(m_opts.m_dyn_sock_opts));
  }

  // Socket created; set members.

  sock->m_active_connect = false;
  sock->m_node = this;
  sock_set_state(sock, Peer_socket::State::S_OPEN, Peer_socket::Open_sub_state::S_CONNECTING);
  sock->m_remote_endpoint = Remote_endpoint{ low_lvl_remote_endpoint, syn->m_packed.m_src_port };
  sock->m_local_port = serv->m_local_port;
  // Save it for user to be able to call sock->get_connect_metadata().  Add const to express we want copy, not move.
  sock->m_serialized_metadata = static_cast<const Blob&>(syn->m_serialized_metadata);
  sock->m_int_state = Peer_socket::Int_state::S_CLOSED; // Kind of pedantic.  We'll set SYN_RCVD a bit later on.
  // Save the start of the sequence number series based on their initial sequence number.
  sock->m_rcv_init_seq_num = syn->m_init_seq_num;
  sock->m_rcv_next_seq_num = sock->m_rcv_init_seq_num + 1;

  /* Initialize the connection's send bandwidth estimator (object that estimates available
   * outgoing bandwidth based on incoming acknowledgments).  It may be used by m_snd_cong_ctl,
   * depending on the strategy chosen, but may be useful in its own right.  Hence it's a separate
   * object, not inside *m_snd_cong_ctl. */
  sock->m_snd_bandwidth_estimator.reset(new Send_bandwidth_estimator{get_logger(), sock});

  // Initialize the connection's congestion control strategy based on the configured strategy.
  sock->m_snd_cong_ctl.reset
    (Congestion_control_selector::create_strategy(sock->m_opts.m_st_cong_ctl_strategy, get_logger(), sock));
  // ^-- No need to use opt() yet: user doesn't have socket and cannot set_options() on it yet.

  const Socket_id& socket_id = Node::socket_id(sock);
  FLOW_LOG_INFO("NetFlow worker thread starting passive-connect of [" << sock << "] on [" << serv << "].  "
                "Received [" << syn->m_type_ostream_manip << "] with ISN [" << syn->m_init_seq_num << "].");

  // Ensure we can support the specified packet options.

  if (syn->m_opt_rexmit_on != sock->rexmit_on())
  {
    FLOW_LOG_WARNING("NetFlow worker thread starting passive-connect of [" << sock << "] on [" << serv << "].  "
                     "Received [" << syn->m_type_ostream_manip << "] with "
                     "opt_rexmit_on [" << syn->m_opt_rexmit_on << "]; was configured otherwise on this side; "
                     "resetting connection.");
    /* We'd inform the user here, but they didn't open the connection (it's a passive open, and they
     * haven't yet called accept()).  We can respond with RST, however, to tell the other side this
     * connection isn't going to happen.  We didn't place sock into m_socks, so just let it
     * disappear via shared_ptr<> magic. */
    async_no_sock_low_lvl_rst_send(Low_lvl_packet::const_ptr_cast(syn), low_lvl_remote_endpoint);
    return Peer_socket::Ptr{};
  }
  // else

  // Ensure this socket pair does not yet exist in our peer-to-peer socket table.

  if (util::key_exists(m_socks, socket_id))
  {
    /* This is a passive connect (they're intiating the connection).  Therefore in particular it
     * should be impossible that our local_port() equals an already existing connection's
     * local_port(); Port_space is supposed to prevent the same ephemeral or service port from being
     * handed out to more than one connection.  Therefore this must be a programming error. */

    FLOW_LOG_WARNING("Cannot add [" << sock << "], because such a connection already exists.  "
                     "This is an ephemeral or service port collision and "
                     "constitutes either a bug or an extremely unlikely condition.");

    // Same reasoning as above: send RST, and let sock disappear.
    async_no_sock_low_lvl_rst_send(syn, low_lvl_remote_endpoint);
    return Peer_socket::Ptr{};
  } // if (that socket pair already exists)
  // else

  /* Try the packet send (as just below) again if SYN_ACK not acknowledged within a certain amount of
   * time.  Give up if that happens too many times.
   * Follow same order of ops (schedule, send) as in the SYN case elsewhere. */
  setup_connection_timers(socket_id, sock, true);

  // Send SYN_ACK to continue the handshake.  Save some *sock data first, as they are used in create_syn_ack().

  /* Initial Sequence Number (the start of our own series).
   * Remember it in case we must retransmit the SYN.  (m_snd_next_seq_num may have been further increased by then.) */
  Sequence_number& init_seq_num = sock->m_snd_init_seq_num;
  init_seq_num = m_seq_num_generator.generate_init_seq_num();
  // Same comment as when calling sock->m_snd_init_seq_num.set_metadata() elsewhere.  See that.
  init_seq_num.set_metadata('L',init_seq_num + 1, sock->max_block_size());
  // Sequence number of first bit of actual data.
  sock->m_snd_next_seq_num = init_seq_num + 1;
  // Security token.  Random number from entire numeric range.  Remember it for later verification.
  sock->m_security_token = m_rnd_security_tokens();
  // Initial receive window is simply the entire empty Receive buffer.
  sock->m_rcv_last_sent_rcv_wnd = sock_rcv_wnd(sock);

  // Make a packet; fill out common fields in and asynchronously send it.
  auto syn_ack = create_syn_ack(sock);
  async_sock_low_lvl_packet_send_paced(sock, Low_lvl_packet::ptr_cast(syn_ack));

  /* send will happen asynchronously, and the registered completion handler will execute in this
   * thread when done (NO SOONER than this method finishes executing). */

  // No more errors: Map socket pair to the socket data structure (kind of analogous to a TCP net-stack's TCB).
  m_socks[socket_id] = sock;

  // Also record it within the server socket (more comments inside this method).
  serv_peer_socket_init(serv, sock);

  // CLOSED -> SYN_RCVD.
  sock_set_int_state(sock, Peer_socket::Int_state::S_SYN_RCVD);

  return sock;
} // Node::handle_syn_to_listening_server()

void Node::handle_syn_ack_ack_to_syn_rcvd(const Socket_id& socket_id,
                                          Peer_socket::Ptr sock,
                                          boost::shared_ptr<const Syn_ack_ack_packet> syn_ack_ack)
{
  using boost::shared_ptr;

  // We are in thread W.

  /* We'd sent SYN_ACK and just got SYN_ACK_ACK.  Assuming their SYN_ACK_ACK is valid, our side of
   * connection can move to ESTABLISHED state, as theirs already has. */

  FLOW_LOG_INFO("NetFlow worker thread continuing passive-connect of socket [" << sock << "].  "
                "Received [" << syn_ack_ack->m_type_ostream_manip << "]; "
                "security token [" << syn_ack_ack->m_packed.m_security_token << "].");

  // First, validate their security token equals the one we've sent.
  if (sock->m_security_token != syn_ack_ack->m_packed.m_security_token)
  {
    FLOW_LOG_WARNING("Received [" << syn_ack_ack->m_type_ostream_manip << "] targeted at state "
                     "[" << Peer_socket::Int_state::S_SYN_RCVD << "] socket [" << sock << "] "
                     "with mismatching security token "
                     "[" << syn_ack_ack->m_packed.m_security_token << "]; we had received and sent and expect "
                     "[" << sock->m_security_token << "].  Closing.");
    /* Close connection in our structures (inform user if necessary as well).  Pre-conditions
     * assumed by call: sock in m_socks and sock->state() == S_OPEN (yes, since m_int_state ==
     * S_SYN_RCVD); 3rd arg contains the reason for the close (yes). */
    rst_and_close_connection_immediately(socket_id, sock, error::Code::S_CONN_RESET_BAD_PEER_BEHAVIOR, true);
    // ^-- defer_delta_check == true: for similar reason as at the end of this method.
    return;
  }
  // else OK.

  // No more errors.

  // Move ourselves to connected state.

  // The server socket to which the other side sent SYN to create the peer socket sock.
  const Server_socket::Ptr serv = sock->m_originating_serv;

  // Public state (thread-safe).
  sock_set_state(sock, Peer_socket::State::S_OPEN, Peer_socket::Open_sub_state::S_CONNECTED);
  // Internal state.  SYN_RCVD -> ESTABLISHED.
  sock_set_int_state(sock, Peer_socket::Int_state::S_ESTABLISHED);

  // Got the acknowledgment to SYN_ACK, so cancel retransmits and the timeout for that SYN_ACK.
  cancel_timers(sock);

  // Setup the Drop Timeout engine (m_snd_drop_timer).
  setup_drop_timer(socket_id, sock);

  // Add the peer socket to the server socket's accept queue (thread-safe)!  accept() will return this.
  serv_peer_socket_acceptable(serv, sock);
  // BTW serv->m_originating_serv is now null.

  // Record initial rcv_wnd; it should be the entire size of the other side's Receive buffer.
  sock->m_snd_remote_rcv_wnd = syn_ack_ack->m_packed.m_rcv_wnd;

  /* We may have queued up some DATA packets while we were SYN_RCVD (due to loss and/or
   * re-ordering).  See handle_data_to_syn_rcvd() for more information.  So, handle the queued DATA
   * packets as if they'd just arrived. */
  for (shared_ptr<Data_packet> qd_packet : sock->m_rcv_syn_rcvd_data_q)
  {
    auto const logger_ptr = get_logger();
    if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING
        ("Handling [" << qd_packet->m_type_ostream_manip << "] packet "
         "received/queued in [" << Peer_socket::Int_state::S_SYN_RCVD << "] state; "
         "packet data size = [" << qd_packet->m_data.size() << "].");

      // Very verbose and CPU-intensive, especially DATA version!
      if (logger_ptr->should_log(log::Sev::S_DATA, get_log_component()))
      {
        FLOW_LOG_DATA_WITHOUT_CHECKING("Readable representation is: "
                                       "[\n" << qd_packet->m_verbose_ostream_manip << "].");
      }
      else
      {
        FLOW_LOG_TRACE_WITHOUT_CHECKING("Readable representation is: "
                                        "[\n" << qd_packet->m_concise_ostream_manip << "].");
      }
    }

    handle_data_to_established(socket_id, sock, qd_packet, true); // true <=> packet was queued during SYN_RCVD.
    // qd_packet has probably been decimated for performance, so don't rely on qd_packet.m_data at this point!
  }
  if (!sock->m_rcv_syn_rcvd_data_q.empty())
  {
    FLOW_LOG_TRACE("Handled a total of [" << sock->m_rcv_syn_rcvd_data_q.size() << "] queued packets with "
                   "cumulative data size [" << sock->m_rcv_syn_rcvd_data_cumulative_size << "].");
  }
  sock->m_rcv_syn_rcvd_data_q.clear(); // Save memory.

  /* Since we just added sock to serv's acceptable socket queue, certainly serv is now Acceptable.
   * Therefore we should soon inform anyone waiting on any Event_sets for serv to become Acceptable.
   *
   * Caveat: The user could have called serv->accept() right after the previous statement in this
   * method, which could indeed make serv not Acceptable again.  That is OK.  We only promise to
   * inform the user of an event within a "non-blocking" amount of time of it occurring.  If that
   * same user decides to mess himself over by acting on these events prematurely, that is not our
   * problem [assuming we don't crash things, which we do not].  Worst case is that the user will
   * detect the event, try to accept() and get nothing [which is an eventuality for which any decent
   * user code would prepare]. */

  // Accumulate the event into the Node store (note: not any Event_set yet).
  if (m_sock_events[Event_set::Event_type::S_SERVER_SOCKET_ACCEPTABLE].insert(serv).second)
  {
    // Possibly inform the user for any applicable Event_sets right now.
    event_set_all_check_delta(true);
    /* ^-- defer_delta_check == true: because the only way to get to this method is from
     * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
     * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
  }

  /* Do not m_sock_events[S_PEER_SOCKET_WRITABLE].insert(sock), as sock has not been accept()ed and
   * therefore cannot be waited on currently. */
} // Node::handle_syn_ack_ack_to_syn_rcvd()

void Node::handle_data_to_syn_rcvd(Peer_socket::Ptr sock,
                                   boost::shared_ptr<Data_packet> packet)
{
  // We are in thread W.

  /* We'd sent SYN_ACK, were waiting for SYN_ACK_ACK, but instead we got a DATA.
   * This seems wrong at a first glance but can be legitimate.  One possibility is they sent
   * SYN_ACK_ACK and then some DATA, but the SYN_ACK_ACK was dropped (recall that the SYN_ACK_ACK is
   * not itself acknowledged in our scheme).  Another possibility is they sent both, but then DATA
   * got re-ordered to in front of SYN_ACK_ACK.
   *
   * What does TCP do here?  Well, it doesn't really have this problem, because every segment must
   * have an ACK in it.  So if a TCP gets data in SYN_RCVD, it must also contain the ACK to the
   * SYN_ACK (what we call SYN_ACK_ACK) (in TCP, any packet without ACK is simply considered
   * corrupt/invalid and would not be sent in the first place).  So it's impossible to send data
   * without acknowledging the SYN_ACK at the same time.
   *
   * For us, however ACK packets are independent of DATA packets, as are SYN_ACK_ACK packets.
   * Therefore we should either drop these DATAs and hope whatever reliability implementation is
   * used restores them later, or we should queue them for consumption when ESTABLISHED arrives.
   * Let's do the latter.  It's hard enough to deal with actual loss; introducing loss when we
   * actually have the stuff seems absurd.
   *
   * So we just save them in a packet queue, and when we're ESTABLISHED we feed all the packets to
   * handle_incoming() as if they'd just arrived.  The only caveat is size of this queue.  Since
   * we have a maximum on the Receive buffer (sock->m_rcv_buf) and the packets-with-gaps structure
   * (sock->m_rcv_packets_with_gaps), we must have one here as well.  Since Receive buffer is
   * empty until ESTABLISHED, it seems natural to limit this queue's cumulative byte size
   * according to the limit imposed on Receive buffer.  (There is some extra overhead to store the
   * packet header info, but it's close enough.)  After that, as when the Receive buffer fills up,
   * we drop packets. */

  assert(sock->m_int_state == Peer_socket::Int_state::S_SYN_RCVD);
  const bool first_time = sock->m_rcv_syn_rcvd_data_q.empty();

  // Not a WARNING, because we didn't do anything wrong; could be network conditions; and avoid verbosity after 1st one.
  FLOW_LOG_WITH_CHECKING(first_time ? log::Sev::S_INFO : log::Sev::S_TRACE,
                         "NetFlow worker thread received [" << packet->m_type_ostream_manip << "] packet while "
                           "in [" << Peer_socket::Int_state::S_SYN_RCVD << "] state for [" << sock << "]; "
                           "saving for processing later when in [" << Peer_socket::Int_state::S_ESTABLISHED << "] "
                           "state; packet data size = [" << packet->m_data.size() << "]; "
                           "first time? = [" << first_time << "].");

  if (first_time)
  {
    sock->m_rcv_syn_rcvd_data_cumulative_size = 0; // It's garbage at the moment.
  }
  else if ((sock->m_rcv_syn_rcvd_data_cumulative_size + packet->m_data.size())
           > sock->opt(sock->m_opts.m_st_snd_buf_max_size))
  {
    // Not a WARNING, because we didn't do anything wrong; could be network conditions.
    FLOW_LOG_INFO("NetFlow worker thread received [" << packet->m_type_ostream_manip << "] packet while "
                  "in [" << Peer_socket::Int_state::S_SYN_RCVD << "] state for [" << sock << "]; "
                  "dropping because Receive queue full at [" << sock->m_rcv_syn_rcvd_data_cumulative_size << "].");
    return;
  }
  // else

  sock->m_rcv_syn_rcvd_data_cumulative_size += packet->m_data.size();
  sock->m_rcv_syn_rcvd_data_q.push_back(packet); // Note that this is not a copy of the packet (just a pointer).

  FLOW_LOG_TRACE("Receive queue now has [" << sock->m_rcv_syn_rcvd_data_q.size() << "] packets; "
                 "cumulative data size is [" << sock->m_rcv_syn_rcvd_data_cumulative_size << "].");
} // Node::handle_data_to_syn_rcvd()

void Node::serv_close_detected(Server_socket::Ptr serv,
                               const Error_code& disconnect_cause, bool close)
{
  /* @todo Nothing calls this yet, as we don't support any way to close a Server_socket yet.
   * Probably will reconsider this method when we do. */

  Server_socket::Lock_guard lock{serv->m_mutex};
  serv->m_disconnect_cause = disconnect_cause;
  if (close)
  {
    // DONE.
    serv_set_state(serv, Server_socket::State::S_CLOSED); // Reentrant mutex => OK.
  }
  else
  {
    // This socket is screwed but not yet out of the Node's system.
    serv_set_state(serv, Server_socket::State::S_CLOSING); // Reentrant mutex => OK.
  }
}

void Node::serv_peer_socket_closed(Server_socket::Ptr serv, Peer_socket::Ptr sock)
{
  using std::list;

  // We are in thread W.

  /* sock is in one of two stages:
   *   - stage 1: serv->m_connecting_socks (not available via accept()), the earlier stage;
   *   - stage 2: serv->m_unaccepted_socks (available via accept()), the later stage.
   *
   * Try stage 1, then stage 2. */

  // Stage 1.
  const bool erased = serv->m_connecting_socks.erase(sock) == 1;
  if (erased)
  {
    /* Maintain invariant.  No need to lock mutex, because sock is in serv->m_connecting_socks, which
     * means it is not in serv->m_unaccepted_socks yet, which means accept() cannot yield it, which means
     * no non-W thread could be accessing m_originating_serv at the same time. */
    sock->m_originating_serv.reset();
    return;
  }
  // else

  // Stage 2.

  /* Remove from serv->m_unaccepted_socks.  At this point accept() can access serv->m_unaccepted_socks and
   * m_originating_serv, so we must lock. */
  Server_socket::Lock_guard lock{serv->m_mutex};

  sock->m_originating_serv.reset(); // Maintain invariant.

  // O(1)ish.
  serv->m_unaccepted_socks.erase(sock);

  /* Notes:
   *
   * The unaccepted socket queue of serv can be accessed by accept()ing threads outside
   * of thread W.  So we must lock object at least to avoid corruption.  We do that above.
   *
   * Now, let's think about the race.  Suppose close_connection_immediately() starts and wins
   * the race to lock *this; removes sock from serv->m_unaccepted_socks; unlocks *this; then the
   * user immediately gets to call accept().  The user will not get sock as the result of the
   * accept(), as we'd removed it in time.  Good.  Now suppose close_connection_immediately() starts
   * but loses the race to lock *sock; user calls accept() first, and accept() yields sock (in
   * S_ESTABLISHED state, though with empty Receive buffer, which is a pre-condition for
   * close_connection_immediately()); then we lock sock and remove sock from
   * serv->m_unaccepted_socks.  Is this OK?  Well, it is not different from this situation: they
   * accept()ed, and then quickly there was an error on the resulting socket, so we closed it
   * before any data came in.  Therefore, yes, this is also OK. */
} // Node::serv_peer_socket_closed()

void Node::serv_peer_socket_acceptable(Server_socket::Ptr serv, Peer_socket::Ptr sock)
{
  // We are in thread W.

  {
    Server_socket::Lock_guard lock{serv->m_mutex};
    serv->m_unaccepted_socks.insert(sock); // Remember that Linked_hash_set<> insert()s at the *front*.
  }
  // This guy is only to be accessed from thread W (which we're in), so no lock needed.
  serv->m_connecting_socks.erase(sock);
}

void Node::serv_peer_socket_init(Server_socket::Ptr serv, Peer_socket::Ptr sock)
{
  // We are in thread W.

  /* Add this connecting socket to the pool of such connecting sockets maintained by the
   * Server_socket.  Once it is fully established, it will move from here to the queue
   * serv->m_unaccepted_socks, where users can claim it via accept().  The utility of serv->m_unaccepted_socks
   * is obvious, but why keep serv->m_connecting_socks?  After all we've already added sock to m_socks, so
   * demultiplexing of those messages (like SYN_ACK_ACK) will work without any additional structure.
   * Answer: we need serv->m_connecting_socks at least for the case where we or the user close the
   * listening socket (serv).  In this case all pending connections must be aborted via RST (to let
   * the other side know), and we'd know which ones to contact via serv->m_connecting_socks.
   * The disallowing of accept()s after the associated listen() has been canceled is discussed in
   * Server_socket documentation, but in short that behavior is similar to the BSD sockets
   * behavior. */
  serv->m_connecting_socks.insert(sock);

  // And vice versa: maintain the invariant.
  sock->m_originating_serv = serv;

  // We didn't lock, because socket not yet available via accept(), so not accessed from non-W threads.
} // Node::serv_peer_socket_init()

Server_socket* Node::serv_create(const Peer_socket_options* child_sock_opts) // Virtual.
{
  // Just make a regular net_flow::Server_socket.
  return serv_create_forward_plus_ctor_args<Server_socket>(child_sock_opts);
}

// Free implementations.

std::ostream& operator<<(std::ostream& os, const Server_socket* serv)
{
  return
    serv
      ? (os
         << "NetFlow_server [NetFlow [:" << serv->local_port() << "]] @" << static_cast<const void*>(serv))
      : (os << "NetFlow_server@null");
}

/// @cond
/* -^- Doxygen, please ignore the following.  (Don't want docs generated for temp macro; this is more maintainable
 * than specifying the macro name to omit it, in Doxygen-config EXCLUDE_SYMBOLS.) */

// That's right, I did this.  Wanna fight about it?
#define STATE_TO_CASE_STATEMENT(ARG_state) \
  case Server_socket::State::S_##ARG_state: \
    return os << #ARG_state

// -v- Doxygen, please stop ignoring.
/// @endcond

std::ostream& operator<<(std::ostream& os, Server_socket::State state)
{
  switch (state)
  {
    STATE_TO_CASE_STATEMENT(LISTENING);
    STATE_TO_CASE_STATEMENT(CLOSING);
    STATE_TO_CASE_STATEMENT(CLOSED);
  }
  return os;
#undef STATE_TO_CASE_STATEMENT
}

} // namespace flow::net_flow
