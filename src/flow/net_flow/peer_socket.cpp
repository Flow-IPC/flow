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
#include "flow/net_flow/peer_socket.hpp"
#include "flow/net_flow/detail/drop_timer.hpp"
#include "flow/net_flow/detail/stats/bandwidth.hpp"
#include "flow/net_flow/detail/cong_ctl.hpp"
#include "flow/net_flow/node.hpp"
#include "flow/util/sched_task.hpp"
#include "flow/async/util.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/tuple/tuple.hpp>
#include <utility>

namespace flow::net_flow
{

// Implementations.

// Peer_socket implementations.

Peer_socket::Peer_socket(log::Logger* logger_ptr,
                         util::Task_engine* task_engine,
                         const Peer_socket_options& opts) :
  Log_context(logger_ptr, Flow_log_component::S_NET_FLOW),
  m_opts(opts),
  m_active_connect(false), // Meaningless; set explicitly.
  m_state(State::S_CLOSED), // Incorrect; set explicitly.
  m_open_sub_state(Open_sub_state::S_DISCONNECTING), // Incorrect; set explicitly.
  m_node(0), // Incorrect; set explicitly.
  m_rcv_buf(logger_ptr, 0), // Receive buffer mode: block size irrelevant (see Socket_buffer doc header).
   // Send buffer mode: pack data into block-sized chunks for dequeueing speed.  See Socket_buffer doc header.
  m_snd_buf(logger_ptr, max_block_size()),
  m_serialized_metadata(logger_ptr),
  m_local_port(S_PORT_ANY), // Incorrect; set explicitly.
  m_int_state(Int_state::S_CLOSED), // Incorrect; set explicitly.
  m_rcv_syn_rcvd_data_cumulative_size(0), // Meaningless unless queue has elements but might as well initialize.
  m_rcv_reassembly_q_data_size(0),
  m_rcv_pending_acks_size_at_recv_handler_start(0),
  m_snd_pending_rcv_wnd(0), // Meaningless originally but might as well initialize.
  m_rcv_last_sent_rcv_wnd(0),
  m_rcv_in_rcv_wnd_recovery(false),
  m_rcv_delayed_ack_timer(*task_engine),
  m_snd_flying_bytes(0),
  m_snd_last_order_num(0),
  m_snd_rexmit_q_size(0),
  m_snd_remote_rcv_wnd(0),
  m_snd_smoothed_round_trip_time(0),
  m_round_trip_time_variance(0),
  m_snd_drop_timeout(0),
  m_snd_pacing_data(task_engine),
  m_security_token(0), // Incorrect; set explicitly.
  m_init_rexmit_count(0)
{
  // Only print pointer value, because most members are garbage at this point.
  FLOW_LOG_TRACE("Peer_socket [" << static_cast<void*>(this) << "] created.");

  // Log initial option values.  Arguable if this should be INFO or TRACE.  @todo Reconsider?
  FLOW_LOG_TRACE("\n\n" << options());
}

Peer_socket::~Peer_socket() // Virtual.
{
  /* Note that m_snd_cong_ctl, m_snd_bandwidth_estimator (etc.) and others store no Ptr(this),
   * so this dtor will indeed execute (no circular shared_ptr problem). */

  FLOW_LOG_TRACE("Peer_socket [" << this << "] destroyed.");
}

Peer_socket::State Peer_socket::state(Open_sub_state* open_sub_state) const
{
  Lock_guard lock(m_mutex); // State is liable to change at any time.
  if (open_sub_state && (m_state == State::S_OPEN))
  {
    *open_sub_state = m_open_sub_state;
  }
  return m_state;
}

Node* Peer_socket::node() const
{
  Lock_guard lock(m_mutex); // m_node can simultaneously change to 0 if state changes to S_CLOSED.
  return m_node;
}

Error_code Peer_socket::disconnect_cause() const
{
  Lock_guard lock(m_mutex);
  return m_disconnect_cause;
}

bool Peer_socket::sync_send(const boost::asio::null_buffers& tag, Error_code* err_code)
{
  return sync_send(tag, Fine_duration::max(), err_code);
}

bool Peer_socket::sync_send_reactor_pattern_impl(const Fine_time_pt& wait_until, Error_code* err_code)
{
  // Similar to sync_send_impl(), so keeping comments light.  Reminder: Goal is to wait until *this is Writable.

  namespace bind_ns = util::bind_ns;
  using bind_ns::bind;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, Peer_socket::sync_send_reactor_pattern_impl,
                                     bind_ns::cref(wait_until), _1);

  Lock_guard lock(m_mutex);

  const Function<size_t (size_t)> empty_snd_buf_feed_func;
  assert(empty_snd_buf_feed_func.empty());

  lock.release();

  // Intentionally pass empty function obj to indicate "reactor pattern" mode.
  node_sync_send(empty_snd_buf_feed_func, wait_until, err_code);
  return !*err_code; // Socket is Writable if and only if !*err_code (i.e., no timeout or other error while waiting).
}

size_t Peer_socket::node_send(const Function<size_t (size_t max_data_size)>& snd_buf_feed_func,
                              Error_code* err_code)
{
  // Everything is locked.  (See send() template.)

  const Ptr sock = shared_from_this();
  if (!Node::ensure_sock_open(sock, err_code)) // Ensure it's open, so that we can access m_node.
  {
    return 0;
  }
  // else m_node is valid.

  return m_node->send(sock, snd_buf_feed_func, err_code);
}

size_t Peer_socket::node_sync_send(const Function<size_t (size_t max_data_size)>& snd_buf_feed_func_or_empty,
                                   const Fine_time_pt& wait_until,
                                   Error_code* err_code)
{
  using boost::adopt_lock;

  // Everything is locked.  (See sync_send() template.)
  Lock_guard lock(m_mutex, adopt_lock); // Adopt already-locked mutex.

  const Ptr sock = shared_from_this();
  if (!Node::ensure_sock_open(sock, err_code)) // Ensure it's open, so that we can access m_node.
  {
    return 0;
  }
  // else m_node is valid.

  /* Because all Node::sync_*() implementations would follow the same pattern (create Event_set,
   * add Readable/Writable/Acceptable event, wait, try non-blocking op, if that fails try again with
   * wait_until ever closer, etc.), for major code reuse we use the sync_op() function template and plug in
   * the various Peer_socket/send-specific pieces as arguments.
   *
   * Performance cost: The only part about this that's not as fast as copy/pasting sync_op() N times, once
   * for each type of socket/op, is the need to lambda the proper send() call into a function object.
   * This amounts to storing and copying the arguments and the function pointer, which should not be
   * too bad and is worth the code reuse IMO. */

  lock.release(); // Again, release lock (mutex is still locked!).

  /* Operating on Peer_sockets, returning size_t; Event_set socket set type is Peer_sockets.
   * Object is sock; non-blocking operation is m_node->send(...) -- or N/A in "reactor pattern" mode.
   * size_t(0) is the "would-block" return value for this operation.  S_PEER_SOCKET_WRITABLE
   * is the type of event to watch for here. */
  return m_node
           ->sync_op<Peer_socket, size_t>
               (sock,
                snd_buf_feed_func_or_empty.empty()
                  ? Function<size_t ()>() // Reactor pattern mode.
                  : Function<size_t ()>([this, sock, snd_buf_feed_func_or_empty, err_code]() -> size_t
                                          { return m_node->send(sock, snd_buf_feed_func_or_empty, err_code); }),
                0, Event_set::Event_type::S_PEER_SOCKET_WRITABLE,
                wait_until, err_code);
} // Peer_socket::node_sync_send()

bool Peer_socket::sync_receive(const boost::asio::null_buffers& tag, Error_code* err_code)
{
  return sync_receive(tag, Fine_duration::max(), err_code);
}

bool Peer_socket::sync_receive_reactor_pattern_impl(const Fine_time_pt& wait_until, Error_code* err_code)
{
  // Similar to sync_receive_impl(), so keeping comments light.  Reminder: Goal is to wait until *this is Readable.

  namespace bind_ns = util::bind_ns;
  using bind_ns::bind;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, Peer_socket::sync_receive_reactor_pattern_impl,
                                     bind_ns::cref(wait_until), _1);

  Lock_guard lock(m_mutex);

  const Function<size_t ()> empty_rcv_buf_consume_func;
  assert(empty_rcv_buf_consume_func.empty());

  lock.release();

  // Intentionally pass empty function obj to indicate "reactor pattern" mode.
  node_sync_receive(empty_rcv_buf_consume_func, wait_until, err_code);
  return !*err_code; // Socket is Readable if and only if !*err_code (i.e., no timeout or other error while waiting).
}

size_t Peer_socket::node_receive(const Function<size_t ()>& rcv_buf_consume_func,
                                 Error_code* err_code)
{
  // Everything is locked.  (See receive() template.)

  const Ptr sock = shared_from_this();
  if (!Node::ensure_sock_open(sock, err_code)) // Ensure it's open, so that we can access m_node.
  {
    return 0;
  }
  // else m_node is valid.

  return m_node->receive(sock, rcv_buf_consume_func, err_code);
}

size_t Peer_socket::node_sync_receive(const Function<size_t ()>& rcv_buf_consume_func_or_empty,
                                      const Fine_time_pt& wait_until,
                                      Error_code* err_code)
{
  using boost::adopt_lock;

  // Everything is locked.  (See sync_send() template.)
  Lock_guard lock(m_mutex, adopt_lock); // Adopt already-locked mutex.

  const Ptr sock = shared_from_this();
  if (!Node::ensure_sock_open(sock, err_code)) // Ensure it's open, so that we can access m_node.
  {
    return 0;
  }
  // else m_node is valid.

  lock.release(); // Again, release lock (mutex is still locked!).

  // See comment in Peer_socket::node_sync_send().

  /* Operating on Peer_sockets, returning size_t; Event_set socket set type is Peer_sockets.
   * Object is sock; non-blocking operation is m_node->receive(...) -- or N/A in "reactor pattern" mode.
   * size_t(0) is the "would-block" return value for this operation.   S_PEER_SOCKET_READABLE
   * is the type of event to watch for here. */
  return m_node
           ->sync_op<Peer_socket, size_t>
               (sock,
                rcv_buf_consume_func_or_empty.empty()
                  ? Function<size_t ()>() // Reactor pattern mode.
                  : Function<size_t ()>([this, sock, rcv_buf_consume_func_or_empty, err_code]() -> size_t
                                          { return m_node->receive(sock, rcv_buf_consume_func_or_empty, err_code); }),
                0, Event_set::Event_type::S_PEER_SOCKET_READABLE,
                wait_until, err_code);
} // Peer_socket::node_sync_receive()

void Peer_socket::close_abruptly(Error_code* err_code)
{
  if (flow::error::exec_void_and_throw_on_error
        ([this](Error_code* actual_err_code) { close_abruptly(actual_err_code); },
         err_code, FLOW_UTIL_WHERE_AM_I_STR()))
  {
    return;
  }
  // else

  // We are in user thread U != W.

  Lock_guard lock(m_mutex); // Lock m_node/m_state; also it's a pre-condition for Node::close_abruptly().

  const Ptr sock = shared_from_this();
  if (!Node::ensure_sock_open(sock, err_code)) // Ensure it's open, so that we can access m_node.
  {
    // *err_code will be set to original close reason (m_disconnect_cause) in this case, as advertised.
    return;
  }
  // else m_node is valid.

  // Forward to Node, as is the general pattern for Peer_socket method implementations.
  lock.release(); // Let go of the mutex (mutex is still LOCKED).
  m_node->close_abruptly(sock, err_code);
  // No m_mutex.unlock(): Node::close_abruptly() MUST take care of it.
} // Peer_socket::close_abruptly()

bool Peer_socket::set_options(const Peer_socket_options& opts, Error_code* err_code)
{
  namespace bind_ns = util::bind_ns;
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Peer_socket::set_options, bind_ns::cref(opts), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  Lock_guard lock(m_mutex); // Lock m_node at least.

  const Ptr sock = shared_from_this();
  if (!Node::ensure_sock_open(sock, err_code)) // Ensure it's open, so that we can access m_node.
  {
    return false;
  }
  // else m_node is valid.

  // As is typical elsewhere, pass the rest of the logic to a Node method.
  return m_node->sock_set_options(sock, opts, err_code);
} // Peer_socket::set_options()

Peer_socket_options Peer_socket::options() const
{
  return opt(m_opts);
}

Peer_socket_info Peer_socket::info() const
{
  // We are in user thread U != W.

  /* There are two cases.  If the socket is open (not S_CLOSED), then an m_node owns it and may
   * change the stats we want to copy in its thread W at any time.  In this case we must copy it in
   * thread W (which we do using a future and post(io_context&), as in listen() and other places in
   * Node).  In the socket is closed (S_CLOSED), then no m_node owns it, so there is no thread W
   * applicable to this socket anymore, and we can just copy the data in thread U != W. */

  Lock_guard lock(m_mutex); // Lock m_node; also it's a pre-condition for Node::sock_info().

  const Const_ptr sock = shared_from_this();

  // See which case it is.
  Error_code dummy;
  if (!Node::ensure_sock_open(sock, &dummy))
  {
    // Socket is closed.  Done and done.  Return the final stats cached at S_CLOSED time.
    return m_info_on_close;
  }
  // else m_node is valid.

  // Forward to Node, as is the general pattern for Peer_socket method implementations.
  lock.release(); // Let go of the mutex (mutex is still LOCKED).
  return m_node->sock_info(sock);
  // No m_mutex.unlock(): Node::sock_info() MUST take care of it.
} // Peer_socket::info()

size_t Peer_socket::max_block_size() const
{
  return opt(m_opts.m_st_max_block_size);
}

size_t Peer_socket::max_block_size_multiple(const size_t& opt_val_ref,
                                            const unsigned int* inflate_pct_val_ptr) const
{
  // Similar to opt() but specialized for this purpose.  Lock once to get both values.
  Options_lock lock(m_opts_mutex);

  const size_t& max_block_size = m_opts.m_st_max_block_size;
  const unsigned int inflate_pct = inflate_pct_val_ptr ? (*inflate_pct_val_ptr) : 0;

  /* We want N's nearest multiple M of B such that M >= N.  M = ceil(N/B) * B (no actual floating-point math involved).
   *
   * Oh, and N is opt_val_ref inflated by K%, or opt_val_ref * (100 + K)%. */
  return util::ceil_div(opt_val_ref * (100 + inflate_pct) / 100, max_block_size)
         * max_block_size;
}

bool Peer_socket::rexmit_on() const
{
  return opt(m_opts.m_st_rexmit_on);
}

const Remote_endpoint& Peer_socket::remote_endpoint() const
{
  // Can't change; no locking needed.  Safe info even if S_CLOSED.
  return m_remote_endpoint;
}

flow_port_t Peer_socket::local_port() const
{
  // Can't change; no locking needed.  Safe (if outdated) info even if S_CLOSED.
  return m_local_port;
}

size_t Peer_socket::get_connect_metadata(const boost::asio::mutable_buffer& buffer,
                                         Error_code* err_code) const
{
  namespace bind_ns = util::bind_ns;
  using std::memcpy;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, Peer_socket::get_connect_metadata, bind_ns::cref(buffer), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in user thread U != W.

  Lock_guard lock(m_mutex); // Lock m_serialized_metadata (it can be changed in sock_free_memory()).

  if (!ensure_open(err_code)) // Ensure it's open; other m_serialized_metadata has been cleared.
  {
    return 0;
  }
  // else m_serialized_metadata is valid.

  err_code->clear();
  const size_t size = std::min(m_serialized_metadata.size(), buffer.size());
  if (size != 0)
  {
    memcpy(buffer.data(), m_serialized_metadata.const_data(), size);
  }

  return size;
} // Peer_socket::get_connect_metadata()

bool Peer_socket::ensure_open(Error_code* err_code) const
{
  return Node::ensure_sock_open(shared_from_this(), err_code);
}

std::string Peer_socket::bytes_blocks_str(size_t bytes) const
{
  using util::String_ostream;
  using std::flush;

  const auto block = max_block_size();
  String_ostream os;
  os.os() << bytes << '~' << (bytes / block);
  if ((bytes % block) != 0)
  {
    os.os() << '+';
  }
  os.os() << flush;
  return os.str();
}

Peer_socket::Sent_packet::Sent_packet(bool rexmit_on,
                                      boost::shared_ptr<Data_packet> packet,
                                      const Sent_when& sent_when) :
  m_size(packet->m_data.size()),
  m_sent_when({ sent_when }),
  m_acks_after_me(0),
  m_packet(rexmit_on ? packet : boost::shared_ptr<Data_packet>()) // Store packet only if we may have to rexmit later.
{
  // Nothing.
}

Peer_socket::Received_packet::Received_packet(log::Logger* logger_ptr, size_t size, util::Blob* src_data) :
  m_size(size),
  m_data(logger_ptr)
{
  if (src_data)
  {
    // Retransmission is on: save *src_data for later reassembly.
    assert(m_size == size); // As promised in docs....

    m_data = std::move(*src_data); // O(1) operation -- *src_data is probably cleared.
  }
}

// Node implementations (dealing with individual Peer_sockets).

// Static initializations.

// Per RFC 5681 (Reno Fast Recovery; used in other congestion control specifications as well to detect drops).
const Peer_socket::Sent_packet::ack_count_t Node::S_MAX_LATER_ACKS_BEFORE_CONSIDERING_DROPPED = 2;
const uint8_t Node::S_DEFAULT_CONN_METADATA = 0; // Keep in sync with doc get_connect_metadata() doc header.

// Implementations.

void Node::handle_syn_ack_to_syn_sent(const Socket_id& socket_id,
                                      Peer_socket::Ptr sock,
                                      boost::shared_ptr<const Syn_ack_packet> syn_ack)
{
  // We are in thread W.

  /* We'd sent SYN and just got SYN_ACK.  Assuming their SYN is valid, our side of connection can
   * move to ESTABLISHED state.  We can also complete the other side's connection by sending
   * SYN_ACK_ACK. */

  FLOW_LOG_INFO("NetFlow worker thread continuing active-connect of [" << sock << "].  "
                "Received [" << syn_ack->m_type_ostream_manip << "] with "
                "ISN [" << syn_ack->m_init_seq_num << "]; "
                "security token [" << syn_ack->m_packed.m_security_token << "].");

  // Send SYN_ACK_ACK to finish the handshake.

  if (!async_low_lvl_syn_ack_ack_send_or_close_immediately(sock, syn_ack))
  {
    return;
  }
  /* send will happen asynchronously, and the registered completion handler will execute in this
   * thread when done (NO SOONER than this method finishes executing). */

  // No more errors.

  // Handle the logical SYN part of their SYN_ACK.

  // Save the start of the sequence number series based on their initial sequence number.
  sock->m_rcv_init_seq_num = syn_ack->m_init_seq_num;
  sock->m_rcv_next_seq_num = sock->m_rcv_init_seq_num + 1;

  // Move ourselves to connected state.

  // Public state.
  sock_set_state(sock, Peer_socket::State::S_OPEN, Peer_socket::Open_sub_state::S_CONNECTED);
  // Internal state.  SYN_SENT -> ESTABLISHED.
  sock_set_int_state(sock, Peer_socket::Int_state::S_ESTABLISHED);

  // Got the acknowledgment to SYN, so cancel retransmits and the timeout for that SYN.
  cancel_timers(sock);

  // Setup the Drop Timeout engine (m_snd_drop_timer).
  setup_drop_timer(socket_id, sock);

  // Record initial rcv_wnd; it should be the entire size of the other side's Receive buffer.
  sock->m_snd_remote_rcv_wnd = syn_ack->m_packed.m_rcv_wnd;

  /* Since sock is now connected and has an empty Send buffer, it is certainly now Writable.
   * Therefore we should soon inform anyone waiting on any Event_sets for sock to become Writable.
   *
   * Caveat: Similar to that in Node::handle_syn_ack_ack_to_syn_rcvd() at similar point in the
   * code. */

  // Accumulate the event into the Node store (note: not any Event_set yet).
  if (m_sock_events[Event_set::Event_type::S_PEER_SOCKET_WRITABLE].insert(sock).second)
  {
    // Possibly inform the user for any applicable Event_sets right now.
    event_set_all_check_delta(true);
    /* ^-- defer_delta_check == true: because the only way to get to this method is from
     * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
     * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
  }
} // Node::handle_syn_ack_to_syn_sent()

void Node::handle_syn_ack_to_established(Peer_socket::Ptr sock,
                                         boost::shared_ptr<const Syn_ack_packet> syn_ack)
{
  // We are in thread W.

  /* We're ESTABLISHED but got a duplicate (valid) SYN_ACK again.  For reasons explained in
   * handle_incoming() at the call to the current method, we simply give them a SYN_ACK_ACK again
   * and continue like nothing happened. */

  FLOW_LOG_INFO("NetFlow worker thread working on [" << sock << "].  "
                "In [" << Peer_socket::Int_state::S_ESTABLISHED << "] state "
                "received duplicate [" << syn_ack->m_type_ostream_manip << "] with "
                "ISN [" << syn_ack->m_init_seq_num << "]; "
                "security token [" << syn_ack->m_packed.m_security_token << "].  "
                "Could be from packet loss.");

  // Everything has already been validated.

  async_low_lvl_syn_ack_ack_send_or_close_immediately(sock, syn_ack);
} // Node::handle_syn_ack_to_established()

void Node::handle_data_to_established(const Socket_id& socket_id,
                                      Peer_socket::Ptr sock,
                                      boost::shared_ptr<Data_packet> packet,
                                      bool syn_rcvd_qd_packet)
{
  /* This is a complex method that does many things.  Therefore readability is hard to accomplish, as the logic
   * makes sense when writing it, but the big picture is hard to see when reading it.  The necessary heavy commenting
   * further increases the size and therefore (along that dimension) decreases readability.  For these reasons,
   * many logically distinct parts were placed into helper methods -- not to increase code reuse but to help
   * the aforementioned consideration. */

  // We are in thread W.

  /* Connection is open, and we got data from other side.  Note: For maintainability, this method features
   * (and should continue to feature) mainly high-level flow control and method calls, as opposed to tons of lower-level
   * detail (this should be factored out into methods being called).
   *
   * Summary of below (assuming no misbehavior by other side; also ignoring that every action is categorized
   * in sock->m_rcv_stats for statistical purposes):
   *
   *   - Determine `dupe` (is packet a duplicate of previously received packet?) by checking against
   *     sock->m_rcv_{next_seq_num|packets_with_gaps}.  If so:
   *     - (Op AAA) Acknowledge packet (ACK to other side).
   *     - Return (do not close connection).
   *   - Determine `slide` (are packet's data the next expected [first -- by seq. # -- not-yet-received] data?)
   *     by checking against sock->m_rcv_{next_seq_num|packets_with_gaps}.
   *   - If retransmission is off:
   *     - (Op ###) Pass packet's data to Receive buffer sock->m_rcv_buf!
   *       - Except if that would overflow sock->m_rcv_buf, then return (do not close connection).
   *     - (Op %%%) Inform the event subsystem that Receive buffer is readable!
   *     - (Op AAA)
   *     - If (!slide):
   *       - Save packet info (except packet->m_data itself!) in sock->m_rcv_packets_with_gaps.
   *         - But if that overflows sock->m_rcv_packets_with_gaps, then also pretend
   *           gap before start of sock->m_rcv_packets_with_gaps has all been filled: set `slide = true;`.
   *           (This will cause below to pop sock->m_rcv_packets_with_gaps to not overflow.)
   *     - If `slide`:
   *       - (Op ***) Update sock->m_rcv_{next_seq_num|packets_with_gaps} (increment the former,
   *         possibly pop-front contiguous packets from the other).
   *   - Else, if retransmission is on:
   *     - If `slide`:
   *       - (Op ###)
   *       - (Op ***)
   *         - Plus, for each packet popped from sock->m_rcv_packets_with_gaps, in increasing seq. # order:
   *           Pass packet's data to Receive buffer sock->m_rcv_buf!
   *       - (Op %%%)
   *     - Else if (!slide):
   *       - Save packet info (including packet->m_data itself!) in sock->m_rcv_packets_with_gaps (reassembly queue).
   *         - But if that WOULD overflow sock->m_rcv_packets_with_gaps, then don't;
   *           and return (do not close connection).
   *     - (Op AAA) */

  /* Set up some short-hand references to commonly used sock members.  This should also help
   * performance a little by skipping the shared_ptr dereference.  (Should be safe since sock
   * cannot get ref-counted down to zero in this method, unless there is an error, at which point
   * we return anyway.)  Just remember these are not simply local variables -- nor const references -- but refer
   * to on-the-heap stuff! */
  const bool rexmit_on = sock->rexmit_on();
  const Sequence_number& seq_num = packet->m_seq_num;

  auto& data = packet->m_data; // NOT const, since we may well be _moving_ this into Receive buffer, etc.
  assert(!data.empty()); // This should have been verified immediately in handle_incoming().
  // Save this before we possibly destroy `data`'s contents below when _moving_ into Receive buffer, etc.
  const size_t data_size = data.size();

  // Register one packet with N bytes of data (not necessarily acceptable data).
  Peer_socket_receive_stats_accumulator& rcv_stats = sock->m_rcv_stats;
  rcv_stats.total_data_packet(data_size);

  // Before potential changes, log.

  FLOW_LOG_TRACE("NetFlow worker thread working on [" << sock << "].  "
                 "Received [" << packet->m_type_ostream_manip << "] with "
                 "sequence number [" << seq_num << "]; data size [" << data_size << "].");
  // Very verbose and CPU-intensive!
  FLOW_LOG_DATA("Data [" << util::buffers_dump_string(data.const_buffer(), "", size_t(-1)) << "].");
  // In below TRACE logging we will omit most of the above details, since they'll be already logged.

  log_rcv_window(sock); // Especially log this state.

  /* Compute `dupe` and `slide[_size]`, bits of info that are key to how the incoming packet fits into the rcv window.
   * Also, regardless of anything else we need to register N bytes worth of data in DATA packets via
   * one rcv_stats.<...>_data_packet(data_size); we can determine the <...> based on dupe, slide, or lack thereof. */

  /* True will means it's a duplicate packet -- ACK but don't give to the user again.
   * False will mean it's a new packet -- ACK and save to a buffer for eventual consumption (unless overflow). */
  bool dupe;
  // Will mean this packet is the first (by seq. #) unreceived packet we want.  Only applies if !dupe.
  bool slide;
  /* ^-- @todo Eliminate this; use slide_size == 0 to mean !slide? Less state is a good thing.
   * Also, slide_size can be assumed to be data_size, except in one case below -- *never* via
   * sock_categorize_data_to_established(); both of these improvements will lead to cleaner code. */
  size_t slide_size; // If (slide), this will be how much to increment m_rcv_next_seq_num.

  const Error_code cat_result = sock_categorize_data_to_established(sock, packet, &dupe, &slide, &slide_size);
  if (cat_result)
  {
    // Register one packet with N bytes of data (not acceptable due to error).
    rcv_stats.error_data_packet(data_size);

    /* Close connection in our structures (inform user if necessary as well).  Pre-conditions
     * assumed by call: sock in m_socks and sock->state() == S_OPEN (yes, since m_int_state ==
     * S_ESTABLISHED); 3rd arg contains the reason for the close (yes).  This will empty the Send
     * and Receive buffers.  That is OK, because this is the abrupt type of close (error). */
    rst_and_close_connection_immediately(socket_id, sock, cat_result, true);
    // ^-- defer_delta_check == true: for similar reason as in handle_syn_ack_ack_to_syn_rcvd().
    return;
  }
  // else

  // If we got here, no error so far; `dupe` and `slide` are both set properly.

  if (dupe)
  {
    /* It's a duplicate received packet.  We should still acknowledge every valid packet, even if
     * duplicate, since at least it helps the other side measure congestion.  Is it "lying," since
     * we're throwing this dupe away?  No, because we DID receive it earlier; and in fact that
     * earlier packet's ACK packet may have itself gotten lost by the network.  (Example: A sends P
     * to B; A receives and responds with ACK of P; that's lost; A receives dupe of P and responds
     * with ACK; B receives that ACK.  Good.)  Anyway if the other side doesn't like it, it can just
     * ignore it.
     *
     * It is also important to ack a duplicate packet, if retransmission is enabled.  For example,
     * sender may send packet X, and we'll ack it; but the ACK may be lost.  Then the sender will
     * retransmit X thinking X was lost; if we don't ACK the retransmitted one, the sender will
     * retransmit again, until it runs out of retransmissions and closes connection... all because
     * of one lousy lost ACK. */

    // Plenty of TRACE logging about duplicate packets above; and here is probably too verbose for an INFO; => no log.

    // Register one packet with N bytes of data (not acceptable into Receive buffer but probably legal, just late).
    rcv_stats.late_or_dupe_data_packet(data_size);

    // Register one individual acknowledgment of N bytes of data (will go out but acks late DATA).
    rcv_stats.late_or_dupe_to_send_ack_packet(data_size);

    // ACK will happen asynchronously (not in this handler, and at best once UDP net-stack considers itself writable).
    async_acknowledge_packet(sock, seq_num, packet->m_rexmit_id, data_size); // rcv_stats kept inside.
    return;
  }
  // else if (!dupe), i.e. data to be saved in Receive buffer or reassembly queue (unless overflow).

  // Register one packet with N bytes of data (legal and acceptable into Receive buffer).
  rcv_stats.good_data_packet(data.size());

  /* Behavior is different at this point depending on whether retransmission is enabled or
   * disabled.  Many of the building blocks are the same and have been factored out into helpers. */

  if (!rexmit_on)
  {
    /* No retransmission, so things are fairly simple.  Firstly any new received data go
     * straight to Receive buffer (out of order or not). */

    if (!sock_data_to_rcv_buf_unless_overflow(sock, packet))
    {
      /* Not so fast.  There's no space in the Receive buffer, so there's no choice except to drop the
       * packet despite all of the above.  Note that this means the packet was not "received" (and
       * we can't slide the window forward either).
       *
       * Should we RST/close?  Absolutely not.  The sender did nothing wrong (except maybe they suck
       * at detecting congestion caused by our user not reading the Receive buffer fast enough and
       * thus letting it fill up, or maybe they just suck at congestion control).  Our user is not
       * consuming the Receive buffer in time.  We drop packet and let chips fall where they may
       * (reliability measures will handle it).
       *
       * Should we still acknowledge it?  No.  Dropping a packet at this late stage is still
       * dropping a packet and indicates congestion of the network, of sorts; if we ACK it, the
       * other side will assume the packet is being delivered and won't slow down its packet
       * onslaught.  So nothing else to. */
      return;
    }

    /* DO NOT use `data` from this point forward -- it was just emptied by sock_data_to_rcv_buf_unless_overflow()!
     * data_size is fine. */

    /* Since sock now has a non-empty Receive buffer, it is certainly now Readable.  Handle implications
     * on relevant waiting Event_sets. */
    sock_rcv_buf_now_readable(sock, syn_rcvd_qd_packet);

    // Successfully wrote to Receive buffer.  Can certainly acknowledge it at this point.

    // Register one individual acknowledgment of N bytes of data (will go out and acks new, acceptable DATA).
    rcv_stats.good_to_send_ack_packet(data_size);

    // ACK will happen asynchronously (not in this handler, and at best once UDP net-stack considers itself writable).
    async_acknowledge_packet(sock, seq_num, 0, data_size); // rcv_stats kept inside.

    if (!slide)
    {
      /* !slide means new packet didn't resolve the first unreceived gap; hence by definition
       * sock->m_rcv_packets_with_gaps must be updated. Due to certain overflow mechanisms, this may also
       * cause the removal of part of the first gap, ironically! So pass in &slide, etc.
       *
       * Pass in data_size, since data.size() would run on an emptied `data` as noted above and be useless. */
      sock_track_new_data_after_gap_rexmit_off(sock, packet, data_size, &slide, &slide_size);

      // `slide` may now be true or not.
    }

    // `slide` may now be true or not.

    /* Finally, update the window, since we've received a new packet.  Maintain large invariant described in doc headers
     * for Peer_socket::m_rcv_packets_with_gaps and related members. */

    if (slide)
    {
      sock_slide_rcv_next_seq_num(sock, slide_size, false);
    }
  } // if (!rexmit_on)
  else // if (rexmit_on)
  {
    /* Retransmission is on, so we have to deal with the reassembly queue.  Namely if this packet
     * fills the gap between stuff already given to Receive buffer and the first packet in the
     * reassembly queue, then we should feed-to-user not just the new packet but also all contiguous packets
     * at the front of the queue into Receive buffer.  If it does not fill it, then we have to add
     * it to reassembly queue in the proper spot. */

    if (slide)
    {
      // New packet filled at least part of the first gap.  So we should feed it to Receive buffer.

      if (!sock_data_to_rcv_buf_unless_overflow(sock, packet))
      {
        /* Not so fast.  If there's no space in the Receive buffer, there's no choice except to drop the
         * packet despite all of the above.  All comments from same spot in the no-retransmission
         * code above apply (not repeating here). */
        return;
      }
      // else

      /* DO NOT use `data` from this point forward -- it was just emptied by sock_data_to_rcv_buf_unless_overflow().
       * data_size is fine. */

      /* Now update the receive window structure.  Maintain invariants described in doc headers
       * for m_rcv_packets_with_gaps and related members.  Additionally, since retransmission is
       * on, if the new packet bridged gap to the first packet(s) in the reassembly queue, then
       * add their data to Receive buffer also (the `true` argument triggers this). */

      sock_slide_rcv_next_seq_num(sock, slide_size, true);

      /* Since sock nsqow has a non-empty Receive buffer, it is certainly now Readable.  Handle implications
       * on relevant waiting Event_sets. */
      sock_rcv_buf_now_readable(sock, syn_rcvd_qd_packet);
    } // if (slide)
    else if (!sock_data_to_reassembly_q_unless_overflow(sock, packet)) // && (!slide)
    {
      /* Out-of-order packet.  Couldn't feed to Receive buffer, so fed to reassembly queue (in sock_data_to_reass...()).
       * However, if we're here, then that indicated we overflowed reassembly queue and decided to drop the packet
       * instead. Bail out; which essentially just means don't acknowledge it, as that would occur just below. */
      return;
    }

    // Either fed to Receive buffer or reassembly queue.  Can certainly acknowledge it at this point.

    // Register one individual acknowledgment of N bytes of data (will go out and acks new, acceptable DATA).
    rcv_stats.good_to_send_ack_packet(data_size);

    // ACK will happen asynchronously (not in this handler, and at best once UDP net-stack considers itself writable).
    async_acknowledge_packet(sock, seq_num, packet->m_rexmit_id, data_size); // More rcv_stats kept inside.
  } // else if (rexmit_on)

  // After changes, log.
  log_rcv_window(sock);
} // Node::handle_data_to_established()

Error_code Node::sock_categorize_data_to_established(Peer_socket::Ptr sock,
                                                     boost::shared_ptr<const Data_packet> packet,
                                                     bool* dupe, bool* slide, size_t* slide_size)
{
  assert(dupe && slide && slide_size);

  /* Note this is a helper to handle_data_to_established() to make it more manageable. See comments and
   * flow in that caller first.
   *
   * Note: not dealing with rcv_stats, as it's less code (assuming 1 call to us anyway) to do it based on our result. */

  // See comment in same spot in handle_data_to_established().
  Peer_socket_receive_stats_accumulator& rcv_stats = sock->m_rcv_stats;
  const Sequence_number& rcv_next_seq_num = sock->m_rcv_next_seq_num;
  const Peer_socket::Recvd_pkt_map& rcv_packets_with_gaps = sock->m_rcv_packets_with_gaps;

  const auto& data = packet->m_data;
  const Sequence_number& seq_num = packet->m_seq_num;

  // Get the sequence number just past the last datum in this packet.
  Sequence_number seq_num_end = seq_num;
  advance_seq_num(&seq_num_end, data.size());

  // If false, all received packets are followed by all unreceived ones.  Otherwise there's at least 1 gap.
  bool first_gap_exists;
  // If true, then this is the sequence number of the first datum right after that first gap.
  Sequence_number seq_num_after_first_gap;
  rcv_get_first_gap_info(sock, &first_gap_exists, &seq_num_after_first_gap);

  // Validate the 1st sequence number in DATA against the ISN.

  if (seq_num <= sock->m_rcv_init_seq_num)
  {
    /* Sequence number precedes or equals the original SYN's sequence number.  Either the other side
     * is an a-hole, or somehow a socket_id was reused from a recent connection, which we do try to
     * avoid like the plague.  Therefore, send them an RST and abort connection.  If they send more
     * data packets to this port (which is quite possible; many could already be on the way),
     * they'll get more RSTs still. */

    // Interesting/rare enough to log a WARNING.
    FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                     "Received [" << packet->m_type_ostream_manip << "] with "
                     "sequence number [" << seq_num << "]; data size [" << data.size() << "]; "
                     "sequence number precedes "
                     "ISN [" << sock->m_rcv_init_seq_num << "].");

    return error::Code::S_SEQ_NUM_IMPLIES_CONNECTION_COLLISION; // Bad behavior from other side is fatal.
  }
  // else if (seq_num >= sock->m_rcv_init_seq_num)

  if (seq_num < rcv_next_seq_num)
  {
    /* The packet claims to begin BEFORE the first gap (i.e., unreceived packet).  This may be a
     * valid duplicate packet.  First, though, ensure it's not a "straddling" packet, i.e., that its
     * last datum's sequence number is not past rcv_next_seq_num.  If it is, that would imply one
     * sequence number's datum is in two packets that are not duplicates of each other which is illegal. */

    if (seq_num_end > rcv_next_seq_num)
    {
      // Interesting/rare enough to log a WARNING.
      FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                       "Received [" << packet->m_type_ostream_manip << "] with "
                       "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                       "data size [" << data.size() << "]; "
                       "straddle first unreceived "
                       "sequence number [" << rcv_next_seq_num << "].");

      // Yep, it straddles the boundary.  Other side is behaving badly.  RST/close as above.
      return error::Code::S_SEQ_NUM_ARITHMETIC_FAILURE;
    }
    // else ([seq_num, end seq_num] is before the first unreceived packet sequence, a/k/a gap)

    FLOW_LOG_TRACE("Duplicate packet before first unreceived sequence number [" << rcv_next_seq_num << "].");

    *dupe = true;
    *slide = false;
    return Error_code();
  } // if (seq_num < rcv_next_seq_num)
  // else if (seq_num >= rcv_next_seq_num)

  /* Packet claims to be in what TCP would call the receive window (somewhere at or after the
   * first gap).  Pin down in what part of that space it is, in order of increasing seq. #s. */

  // First see if it's right at the start of the first gap.

  if (seq_num == rcv_next_seq_num)
  {
    /* Great.  It's at the start of the first gap, so we should be able to advance the window
     * (increment rcv_next_seq_num).  First check that it doesn't straddle the next received packet
     * after the gap, if any.  (Again, if it does that means one sequence number is inside 2
     * packets that aren't dupes of each other, which is illegal.) */
    if (first_gap_exists && (seq_num_end > seq_num_after_first_gap))
    {
      // Interesting/rare enough to log a WARNING.
      FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                       "Received [" << packet->m_type_ostream_manip << "] with "
                       "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                       "data size [" << data.size() << "]; "
                       "supposed gap-filling data "
                       "straddle the boundary of packet [" << seq_num_after_first_gap << ", ...).");

      return error::Code::S_SEQ_NUM_ARITHMETIC_FAILURE; // As above.
    }
    // else legal -- can slide window to the right and save to Receive buffer.

    FLOW_LOG_TRACE("Packet filled first [" << data.size() << "] unreceived sequence numbers "
                   "starting with [" << rcv_next_seq_num << "].");

    *dupe = false;
    *slide = true;
    *slide_size = size_t(seq_num_end - seq_num);
    assert(*slide_size == data.size());
    return Error_code();
  }

  // else if:
  assert(seq_num > rcv_next_seq_num);

  *slide = false; // This much is certain, as we're not filling the first gap from the front.

  /* Packet doesn't fill that first gap.  It's somewhere after the start of the first gap.  Now
   * there are 3 possibilities:
   *
   *  -1- It's illegal: it straddles the boundary of one of the packets in m_rcv_packets_with_gaps,
   *      meaning some sequence number is inside 2 non-identical packets.  RST/close as above.
   *
   *  -2- It is a duplicate (same starting sequence number and length) of one of the packets
   *      past the first gap (i.e., of the packets in rcv_packets_with_gaps).  Thus dupe =
   *      true (we should ACK but not save to Receive buffer).
   *
   *  -3- It fits into one of the gaps; i.e. its sequence number range is either entirely
   *      before that of rcv_packets_with_gaps; entirely after it; or entirely before the
   *      first sequence number of an element of rcv_packets_with_gaps AND entirely after the
   *      last sequence number of the preceding element of rcv_packets_with_gaps.  Thus we
   *      should ACK and save to Receive buffer.
   *
   * Determine which one it is.
   *
   * @todo Below technique is fun and all, but I now suspect the following might be simpler:
   * 1, is seq_num in rcv_packets_with_gaps already?  If so but different length, error; if so but
   * but same length, *dupe is true.  Otherwise: 2, insert a thing representing `packet` into rcv_packets_with_gaps
   * as if for real; call inserted thing P.  3, check for straddling against right edge of prior(P), if any;
   * if so, error.  4, check for straddling against left edge of next(P), if any; if so, error.
   * 5, *dupe is false.  The problem?  It requires insertion, when this is supposed to not modify `packet` but only
   * categorize it.  Can of course remove it at the end, but that's cheesy.  Can also modify our contract
   * accordingly, but that reduces separation of concerns in caller's algorithm.  Also, possibly the resulting
   * algorithm might be easier to grok but not much shorter, if at all, anyway.  Finally, could leave the
   * straddling detection to later parts of the algorithm (again, changing our contract to be weaker though).
   * In any case, not a top concern; and in terms of performance I doubt it would differ much from below. */

  /* Find where we are compared to the various received packets past the first gap.
   * This gets the first packet whose first sequence number is >= seq_num.  There are 3 possibilities:
   * that is equal to seq_num, past seq_num, or there is no such packet.
   *
   * Note that the lookup is O(log n) amortized, and then the subsequent checking is O(1).
   * This is one of the reasons to use a sorted map by seq. #. */
  const Peer_socket::Recvd_pkt_const_iter next_packet = rcv_packets_with_gaps.lower_bound(seq_num);

  if (next_packet == rcv_packets_with_gaps.end())
  {
    /* There is no packet after ours, and there is no packet equal to ours.  Thus we'll just
     * insert our packet at the end.  Check, however, that there is no straddling (-1- above).
     * What packet's boundary can we straddle?  At least the last one (assuming there's a gap).  Its
     * last number may be >= seq_num.  (Its first is guaranteed to be < seq_num based on the
     * above check.)  If we don't straddle that boundary, we can't straddle any other packet's boundary,
     * since all other packets precede the last one, so just check the last one (if exists). */
    if (first_gap_exists)
    {
      const Peer_socket::Recvd_pkt_const_iter last_packet = prior(rcv_packets_with_gaps.end());
      Sequence_number seq_num_last_end;
      get_seq_num_range(last_packet, 0, &seq_num_last_end);

      if (seq_num_last_end > seq_num) // (Corner case check: == means it contiguously precedes `packet`; no straddle.)
      {
        // Yep, packet straddles boundary of last_packet.

        // Interesting/rare enough to log a WARNING.
        FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                         "Received [" << packet->m_type_ostream_manip << "] with "
                         "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                         "data size [" << data.size() << "]; "
                         "supposed middle gap-filling packet data "
                         "straddle the boundary of last packet [..., " << seq_num_last_end << ").");

        // Register one packet with N bytes of data (not acceptable due to error).
        rcv_stats.error_data_packet(data.size());
        return error::Code::S_SEQ_NUM_ARITHMETIC_FAILURE; // As above.
      }
      // else OK, we're a new packet that happens to be the newest (by sequence number).

      FLOW_LOG_TRACE("New packet is newest packet after unreceived gap; "
                     "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                     "first unreceived packet [" << rcv_next_seq_num << "].");
    }
    else // if (!first_gap_exists)
    {
      // OK, we're a new packet that happens to be the packet that forms the first gap by being after that gap.

      FLOW_LOG_TRACE("New packet forms gap; sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                     "first unreceived packet [" << rcv_next_seq_num << "].");
    }

    *dupe = false;
    return Error_code();
  } // if (next_packet does not exist)
  // else if (next_packet exists at the same or later sequence number as seq_num)

  // Get the [range) of sequence numbers in the packet that starts at or after seq_num.
  Sequence_number seq_num_next_start, seq_num_next_end;
  get_seq_num_range(next_packet, &seq_num_next_start, &seq_num_next_end);

  if (seq_num_next_start == seq_num)
  {
    /* Our first datum has same sequence number as next_packet.  Thus it's a duplicate.
     * Check, however, that their last sequence numbers are also identical.  Otherwise, again,
     * one datum is in two different packets, which is illegal. */
    if (seq_num_next_end != seq_num_end)
    {
      // Yep, not a valid duplicate.

      // Interesting/rare enough to log a WARNING.
      FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                       "Received [" << packet->m_type_ostream_manip << "] with "
                       "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                       "data size [" << data.size() << "]; "
                       "do not match supposed "
                       "duplicate packet [" << seq_num << ", " << seq_num_next_end << ").");

      return error::Code::S_SEQ_NUM_ARITHMETIC_FAILURE; // As above.
    }
    // else

    /* @todo With rexmit_on we can also/instead compare `data` against actual data payload in next_packet -- not just
     * the sequence numbers. With !rexmit_on, there's no need to store the payloads, as they're always fed directly
     * to user upon receipt, even out of order. */

    FLOW_LOG_TRACE("Duplicate packet after unreceived data; "
                   "sequence numbers [" << seq_num << ", " << seq_num_end << ").");

    *dupe = true;
    return Error_code();
  } // if (seq_num_next_start == seq_num)
  // else if:
  assert(seq_num_next_start > seq_num); // lower_bound() is not horrifically broken.

  // We've eliminated all dupe possibilities above. It's either error or not, at this point.
  *dupe = false;

  /* Since next_packet starts after `packet`, the best outcome is that packet is entirely
   * before next_packet and entirely after prev_packet, where prev_packet == prior(next_packet) (if
   * such a thing exists).  So we must check that we don't straddle
   * either next_packet's starting boundary or prev_packet's ending boundary.  All other
   * preceding boundaries are straddled if and only if the prev_packet end is, and all
   * succeding boundaries iff next_packet start is. */

  if (seq_num_end > seq_num_next_start) // Corner case check: == means `packet` contiguously precedes next_packet.
  {
    // Straddle one or more succeding packets.  RST/close as above.

    // Interesting/rare enough to log a WARNING.
    FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                     "Received [" << packet->m_type_ostream_manip << "] with "
                     "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                     "data size [" << data.size() << "]; "
                     "supposed middle gap-filling packet data "
                     "straddle the left boundary of packet "
                     "[" << seq_num_next_start << ", " << seq_num_next_end << ").");

    return error::Code::S_SEQ_NUM_ARITHMETIC_FAILURE; // Bad behavior is fatal to connection, as above.
  }
  // else succeding packets OK. Check preceding packets.

  if (next_packet == rcv_packets_with_gaps.begin())
  {
    FLOW_LOG_TRACE("New packet partially fills first gap without sliding window; "
                   "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                   "first unreceived packet [" << rcv_next_seq_num << "].");
    return Error_code(); // There are none. We're good.
  }

  const Peer_socket::Recvd_pkt_const_iter prev_packet = prior(next_packet);
  Sequence_number seq_num_prev_start, seq_num_prev_end;
  get_seq_num_range(prev_packet, &seq_num_prev_start, &seq_num_prev_end);

  if (seq_num_prev_end > seq_num) // Corner case check: == means prev_packet contiguously precedes `packet`.
  {
    // Straddling one or more preceding packets.  RST/close as above.

    // Interesting/rare enough to log a WARNING.
    FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                     "Received [" << packet->m_type_ostream_manip << "] with "
                     "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                     "data size [" << data.size() << "]; "
                     "supposed middle gap-filling packet data "
                     "straddle the right boundary of packet "
                     "[" << seq_num_prev_start << ", " << seq_num_prev_end << ").");

    return error::Code::S_SEQ_NUM_ARITHMETIC_FAILURE; // Bad behavior is fatal to connection, as above.
  }
  // else preceding packets OK.

  FLOW_LOG_TRACE("New packet fills some middle gap; "
                 "sequence numbers [" << seq_num << ", " << seq_num_end << "); "
                 "first unreceived packet [" << rcv_next_seq_num << "].");

  return Error_code();
} // Node::sock_categorize_data_to_established()

bool Node::sock_data_to_rcv_buf_unless_overflow(Peer_socket::Ptr sock,
                                                boost::shared_ptr<Data_packet> packet)
{
  using util::Blob;

  /* Note this is a helper to handle_data_to_established() to make it more manageable. See comments and
   * flow in that caller first. */

  // See comment in same spot in handle_data_to_established().
  Peer_socket_receive_stats_accumulator& rcv_stats = sock->m_rcv_stats;
  Blob& data = packet->m_data; // NOT const due to Socket_buffer::feed*().  See below.
  // Save this before we possibly destroy data's contents below (for performance).
  const size_t data_size = data.size();

  size_t buf_size;
  {
    // Receive Buffer can be consumed by user threads (not W) at the same time.  Must lock.
    Peer_socket::Lock_guard lock(sock->m_mutex);

    /* First we must check if block will fit into sock->m_rcv_buf.  Why not just use feed_buf_move()'s
     * max_data_size argument?  Because that would allow to partially enqueue the block, if there's
     * space for some but not all of the block.  Since we can't partially ACK a packet, we have to
     * drop the whole thing in that case.
     *
     * Round up to a multiple of max-block-size to ensure we never fragment a max-block-size-sized
     * chunk of data when they're using unreliable mode!  Also apply the slack % to account for
     * the fact that rcv_wnd sent to the other side may lag behind reality (the key is to NOT
     * apply the slack % when sending rcv_wnd, so that it is more conservative). */
    if ((sock->m_rcv_buf.data_size() + data_size)
        > sock->max_block_size_multiple(sock->m_opts.m_st_rcv_buf_max_size,
                                        &sock->m_opts.m_st_rcv_buf_max_size_slack_percent))
    {
      // Receive buffer overflow.

      // Register one packet of N bytes of acceptable data that we unfortunately have to drop due to buffer overflow.
      rcv_stats.good_data_dropped_buf_overflow_packet(data_size);

      // Not an error but interesting.  Might be too verbose for INFO but what the hell.
      FLOW_LOG_INFO("NetFlow worker thread working on [" << sock << "].  "
                    "Received [" << packet->m_type_ostream_manip << "] with "
                    "sequence numbers [" << packet->m_seq_num << ", " << (packet->m_seq_num + data_size) << "); "
                    "data size [" << data_size << "]; "
                    "dropping because Receive buffer full.");
      return false;
    }
    // else can successfully write to Receive buffer (enough space for entire block).

    /* Let's make data available to user!  This is a constant-time operation that MOVES
     * packet.data's contents into m_rcv_buf (via swap).  That's why packet is Ptr and not
     * Const_ptr.  Note that after that we no longer work with packet -- it's a goner; data.empty()
     * is true.
     *
     * No need to provide max buffer size -- we already checked that's not an issue above. */

#ifndef NDEBUG
  const size_t written =
#endif
      sock->m_rcv_buf.feed_buf_move(&data, std::numeric_limits<size_t>::max());
    // `data` is now empty.
    assert(written == data_size);

    buf_size = sock->m_rcv_buf.data_size();
  } // lock(sock->m_mutex)

  // Register one packet of N bytes of acceptable data that we accepted -- did not drop.
  rcv_stats.good_data_accepted_packet(data_size);
  // Register one packet of N bytes of acceptable data that we delivered to user.
  rcv_stats.good_data_delivered_packet(data_size);
  // Register that the Receive buffer grew.
  rcv_stats.buffer_fed(buf_size);

  // They've sent reasonable data -- so handle the implications on rcv_wnd recovery (if any).
  receive_wnd_recovery_data_received(sock);

  return true;
} // Node::sock_data_to_rcv_buf_unless_overflow()

void Node::sock_rcv_buf_now_readable(Peer_socket::Ptr sock, bool syn_rcvd_qd_packet)
{
  /* We are told sock now has a non-empty Receive buffer and is thus Readable.  Therefore we
   * should soon inform anyone waiting on any Event_sets for sock to become Readable.
   *
   * Caveat: Similar to that in Node::handle_syn_ack_ack_to_syn_rcvd() at similar point in the
   * code.
   *
   * Also: why do this outside the locked block that likely preceded this to actually write to the
   * Receive buffer?  Avoid possibility of deadlock, since there
   * are two mutexes at play: sock->m_mutex (locked in the likely Receive buffer
   * update and in event_set_all_check_delta()) and Event_set::m_mutex (which is locked in
   * event_set_all_check_delta()).  Different mutexes should always be locked in the same order,
   * and other threads lock in the sock->m_mutex/event_set->m_mutex order.
   *
   * Finally: if this packet was not received in ESTABLISHED but rather in SYN_RCVD and saved
   * until ESTABLISHED, then we skip this (syn_rcvd_qd_packet).
   * Why?  Answer: in this case the socket has not yet been
   * given to the user (they need to call accept() or equivalent).  Therefore, they could not have
   * added it to an Event_set and thus are not interested in Readable status on it.  (For
   * background on this queueing, see handle_data_to_syn_rcvd(). */

  // Accumulate the event into the Node store (note: not any Event_set yet) (if received during ESTABLISHED).
  if ((!syn_rcvd_qd_packet) &&
      m_sock_events[Event_set::Event_type::S_PEER_SOCKET_READABLE].insert(sock).second)
  {
    // Possibly inform the user for any applicable Event_sets right now.
    event_set_all_check_delta(true);
    /* ^-- defer_delta_check == true: because the only way to get to this method is from
     * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
     * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
  }
} // Node::sock_rcv_buf_now_readable()

void Node::sock_track_new_data_after_gap_rexmit_off(Peer_socket::Ptr sock,
                                                    boost::shared_ptr<const Data_packet> packet,
                                                    size_t data_size,
                                                    bool* slide, size_t* slide_size)
{
  using std::make_pair;

  /* Note this is a helper to handle_data_to_established() to make it more manageable.  See comments and
   * flow in that caller first. */

  *slide = false;
  *slide_size = 0;

  // See comment in same spot in handle_data_to_established().
  Peer_socket_receive_stats_accumulator& rcv_stats = sock->m_rcv_stats;
  Peer_socket::Recvd_pkt_map& rcv_packets_with_gaps = sock->m_rcv_packets_with_gaps;
  const Sequence_number& seq_num = packet->m_seq_num;

  /* Since we may increase rcv_packets_with_gaps size below, we may exceed the limit as described
   * in m_rcv_packets_with_gaps doc header.  (The limit is due to memory concerns.)  Let's compute
   * that limit. */
  const size_t max_packets_after_unrecvd_packet = sock_max_packets_after_unrecvd_packet(sock);

  /* A pre-condition is: The received packet is NOT the first (earliest) unreceived packet we're waiting
   * for; in other words it is not the packet at the start of the first gap.  So we should save
   * the packet into rcv_packets_with_gaps.  (This will elsewhere help us, at least, detect if this
   * packet comes in again [duplicate]. See sock_categorize_data_to_established().) */
#ifndef NDEBUG
  const auto insert_result =
#endif
    rcv_packets_with_gaps.insert
      (make_pair(seq_num,
                 Peer_socket::Received_packet::Ptr(new Peer_socket::Received_packet(get_logger(), data_size, 0))));
  // m_rcv_reassembly_q_data_size untouched because !rexmit_on.
  assert(!sock->rexmit_on());
  assert(insert_result.second); // If was already there, there's some serious bug in above logic.
  // No other part of the invariant is violated, so that's it.

  bool first_gap_exists;
  // The sequence number of the first datum right after the first unreceived gap.
  Sequence_number seq_num_after_first_gap;

  rcv_get_first_gap_info(sock, &first_gap_exists, &seq_num_after_first_gap);
  assert(first_gap_exists);

  /* We would be done here, except we need to protect against rcv_packets_with_gaps growing too
   * large.  This is explained in detail in the m_rcv_packets_with_gaps doc comment.  Long story
   * short: if we exceed a certain length in this structure, pretend we have "received" the entire
   * first gap, which will allow us to slide the window forward and eliminate all the contiguous
   * received packets following this gap, of which there will be at least one
   * (rcv_packets_with_gaps.begin()), bringing the structure's size back to the limit. */

  if (rcv_packets_with_gaps.size() == max_packets_after_unrecvd_packet + 1)
  {
    // Use these output knobs to reduce rcv_packets_with_gaps.size() after all to avoid overflow.
    *slide = true;
    *slide_size = size_t(seq_num_after_first_gap - sock->m_rcv_next_seq_num);

    // Register unknown # of packets with N bytes of data, which we are assuming are dropped.
    rcv_stats.presumed_dropped_data(data_size);

    // Not an error but interesting.  Might be too verbose for INFO but what the hell.
    FLOW_LOG_INFO("NetFlow worker thread working on [" << sock << "].  "
                  "Received [" << packet->m_type_ostream_manip << "] with "
                  "sequence numbers [" << packet->m_seq_num << ", " << (packet->m_seq_num + data_size) << "); "
                  "exceeded max gapped packet list size [" << max_packets_after_unrecvd_packet << "]; "
                  "assuming Dropped; "
                  "will fake receiving all [" << slide_size << "] sequence numbers in the first unreceived gap.");
  }
  else
  {
    // Our logic shouldn't be allowing the max to be exceeded by more than 1 at any time; we "wrist-slap" it above at 1.
    assert(rcv_packets_with_gaps.size() <= max_packets_after_unrecvd_packet);
  }
} // Node::sock_track_new_data_after_gap_rexmit_off()

bool Node::sock_data_to_reassembly_q_unless_overflow(Peer_socket::Ptr sock,
                                                     boost::shared_ptr<Data_packet> packet)
{
  using std::make_pair;

  /* Note this is a helper to handle_data_to_established() to make it more manageable. See comments and
   * flow in that caller first. */

  Peer_socket_receive_stats_accumulator& rcv_stats = sock->m_rcv_stats;
  Peer_socket::Recvd_pkt_map& rcv_packets_with_gaps = sock->m_rcv_packets_with_gaps;
  const Sequence_number& seq_num = packet->m_seq_num;

  auto& data = packet->m_data; // NOT const due to the move into Received_packet; see below.
  // Save this before we possibly destroy data's contents below (for performance).
  const size_t data_size = data.size();

  /* Since we will increase rcv_packets_with_gaps size below, we may exceed the limit as
   * described in m_rcv_packets_with_gaps doc header.  (The limit is due to memory concerns.)
   * Let's compute that limit. */
  size_t max_packets_after_unrecvd_packet = sock_max_packets_after_unrecvd_packet(sock);

  /* Update: Actually, that limit is (as noted in the doc header for Peer_socket::m_rcv_packets_with_gaps, whose
   * growth we are constraining here) more of a formality, as in practice things like sender's CWND or
   * sender's following our rcv-wnd guidance should keep the size of this retranmission queue much lower than
   * the limit that was just computed.  However!  There IS a retransmission-enabled-exclusive limit we should
   * apply here, and it may at times be applied in practice, unlike what we just computed.  Namely, consider
   * that if we receive N in-order, fully populated (up to max-block-size) DATA packets, and NxMBS exceeds
   * max-on-Receive-buffer, then indeed we will drop the overflowing portion and not put into Receive buffer;
   * but if we don't receive 1 in-order packet, get the next (N - 1) packets, and then finally get the one
   * missing DATA packet, then they will all be delivered to Receive buffer without a problem.  (The next in-order
   * packet would indeed hit overflow, unless user dequeues some.  This only highlights the oddness.)
   * Why?  Because the above-computed limit is far higher than the equivalent max-on-Receive-buffer configuration
   * (typically), so the reassembly queue would be loaded up with stuff without hitting any limit, and the
   * code that dequeues from reassembly queue into Receive buffer does not follow any overflow logic (nor can it,
   * really, since by that point those DATA packets have long since been ACKed, and we do not renege ACKs).
   * Long story short, that is not good, and we should simply apply the max-on-Receive-buffer to not just
   * the Receive buffer but to this reassembly queue PLUS the Receive buffer.
   *
   * Caution! This policy means the rcv-wnd advertisements to the other side must follow this policy too.
   *
   * OK, make the computation as described.  First compute the max-on-Receive-buffer, same as when actually computing
   * that when enqueueing that structure.  Then subtract how much of it we've used in actual Receive buffer.
   * What remains is what's allowed for rcv_packets_with_gaps:
   *
   *   Rbufdata + Rqdata <= Rbufmax <==> Rqdata <= Rbufmax - Rbufdata = S.
   *   S_blocks = floor(S / max-block-size).
   *   Ensure Rcurdata_blocks + 1 <= S_blocks.
   *
   * This is about right but actually slightly oversimplified, because that limit assumes the data are packed
   * in max-block-sized packets except possibly the last one.  In reality the existing payload of the reassembly queue
   * may be not stored so efficiently (who knows how stuff got packetized or supplied by user or both?).  To compute
   * this quite carefully (maybe overkill, but I feel deterministically understood to be correct = a good thing), we
   * model it as the queue already storing what it's storing; and we must allow a certain number of packets
   * on top of that and no more; and the question is whether that's enough for the incoming 1 DATA packet.
   * So then, we want this:
   *
   *   Ensure Rqcurdata_blocks + 1 <= Rqcurdata_blocks + Sleft_blocks.
   *   Sleft_blocks = # additional packets allowed by policy = floor(Sleft / max-block-size).
   *   Sleft = max(Rbufmax - Rqcurdata - Rbufdata, 0).
   *
   * So we're doctoring it: we know Rqcurdata_blocks = rcv_packets_with_gaps.size() are already used; so we will
   * allow some # of packets beyond that, and the question is what is that # according to our policy?  Well, it's just
   * the configured limit minus the used Receive buffer in bytes and minus the sum of rcv_packets_with_gaps's bytes.
   * Since we're using bytes there, that's the maximum possible accuracy, without any inefficiency being assumed to
   * not exist.  Note that we have Rqcurdata* being subtracted from Rqcurdata* on one side, and that may seem like
   * those should cancel each other out to zero, but no -- that was the case in the simpler model above, but the more
   * realistic one means those are (sligthly, potentially) different. */
  size_t max_packets_in_reassembly_q
    = sock->max_block_size_multiple(sock->m_opts.m_st_rcv_buf_max_size,
                                    &sock->m_opts.m_st_rcv_buf_max_size_slack_percent);
  // We have to momentarily lock sock due to access to sock->m_rcv_buf.
  size_t rcv_buf_size;
  {
    Peer_socket::Lock_guard lock;
    rcv_buf_size = sock->m_rcv_buf.data_size(); // This access requires locking.
  }
  util::subtract_with_floor(&max_packets_in_reassembly_q, rcv_buf_size) && // [sic]
    util::subtract_with_floor(&max_packets_in_reassembly_q, sock->m_rcv_reassembly_q_data_size);
  // Convert from bytes to max-block-sizes.  Note this is the floor of the division (so it is strict).
  max_packets_in_reassembly_q /= sock->max_block_size();
  /* Okay, we have Sleft in blocks now; add this for direct comparison to the left side, which will be .size() + 1,
   * where the 1 is the incoming packet `packet`.  Full-circle, this is `Rqcurdata_blocks + Sleft_blocks` from
   * the above big comment. */
  max_packets_in_reassembly_q += rcv_packets_with_gaps.size();

  // The final limit is the lower of the two limits; realistically we expect max_packets_in_reassembly_q to "win."
  if (max_packets_in_reassembly_q < max_packets_after_unrecvd_packet)
  {
    max_packets_after_unrecvd_packet = max_packets_in_reassembly_q;
  }
  else
  {
    // Not an error but pretty weird configuration (but too verbose for INFO, if it really does occur).
    FLOW_LOG_TRACE("Unexpected Receive buffer limits: safety net [" << max_packets_after_unrecvd_packet << "] <= "
                   "real limit [" << max_packets_in_reassembly_q << "], but the opposite is typical.  "
                   "See details just below."); // See next log message.
  }

  if (rcv_packets_with_gaps.size() + 1 > max_packets_after_unrecvd_packet)
  {
    /* Overflow.  Drop this new packet instead of queueing it.  Note that this is different
     * from the handling of the same situation in the no-retransmit case.  In that case, this
     * situation is probably more common under loss, since once a packet is considered Dropped by sender, it is NEVER
     * re-sent; thus Receiver eventually also considers it Dropped and (instead of dropping
     * the new packet, which would be a disastrous policy) simply pretends the gap has been
     * filled, thus consolidating the front of rcv_packets_with_gaps. */

    // Register one packet of N bytes of acceptable data that we unfortunately have to drop due to overflow.
    rcv_stats.good_data_dropped_reassembly_q_overflow_packet(data_size);

    // This is an error, though not our fault.
    FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                     "Received [" << packet->m_type_ostream_manip << "] with "
                     "sequence numbers [" << packet->m_seq_num << ", " << (packet->m_seq_num + data_size) << "); "
                     "exceeded max gapped packet list size [" << max_packets_after_unrecvd_packet << "]; "
                     "dropping packet.");
    return false;
  }
  // else we can insert into reassembly queue (priority queue by seq. #) rcv_packets_with_gaps.

  FLOW_LOG_TRACE("NetFlow worker thread working on [" << sock << "].  "
                 "Enqueueing [" << packet->m_type_ostream_manip << "] payload onto reassembly queue with "
                 "sequence numbers [" << packet->m_seq_num << ", " << (packet->m_seq_num + data_size) << ") "
                 "of size [" << data_size << "]; "
                 "successfully fit into max gapped packet list size [" << max_packets_after_unrecvd_packet << "]; "
                 "could have fit [" << (max_packets_after_unrecvd_packet - rcv_packets_with_gaps.size()) << "] more.");

  // This decimates `data` but is constant time, much like the buffer enqueueing done elsewhere.
#ifndef NDEBUG
  const auto insert_result =
#endif
    rcv_packets_with_gaps.insert
      (make_pair(seq_num, // Decimation occurs in here: ------------------v, hence the `&`: -------------v.
                 Peer_socket::Received_packet::Ptr(new Peer_socket::Received_packet(get_logger(), data_size, &data))));
  sock->m_rcv_reassembly_q_data_size += data_size;
  assert(insert_result.second); // If was already there, there's some serious bug in above logic.
  // No other part of the invariant is violated, so that's it.

  // DO NOT use `data` from this point forward -- it was just emptied by moving into the new Received_packet.

  // Register one packet of N bytes of acceptable data that we accepted -- did not drop.
  rcv_stats.good_data_accepted_packet(data_size);
  // Register one packet of N bytes of acceptable data that we queued for reassembly -- not yet in Receive buffer.
  rcv_stats.good_data_first_qd_packet(data_size);

  // They've sent reasonable data -- so handle the implications on rcv_wnd recovery (if any).
  receive_wnd_recovery_data_received(sock);

  return true;
} // Node::sock_data_to_reassembly_q_unless_overflow()

void Node::sock_slide_rcv_next_seq_num(Peer_socket::Ptr sock, size_t slide_size, bool reassembly_in_progress)
{
  /* Note this is a helper to handle_data_to_established() to make it more manageable. See comments and
   * flow in that caller first. */

  // See comment in same spot in handle_data_to_established().
  Peer_socket_receive_stats_accumulator& rcv_stats = sock->m_rcv_stats;
  Peer_socket::Recvd_pkt_map& rcv_packets_with_gaps = sock->m_rcv_packets_with_gaps;
  Sequence_number& rcv_next_seq_num = sock->m_rcv_next_seq_num;

  /* OK, caller determined that the front of the gap between rcv_next_seq_num and
   * seq_num_after_first_gap has been received.  Indeed mark this fact by sliding the former to a higher value,
   * indicating sliding right of the left edge of the receive window, in TCP terminology. */
  rcv_next_seq_num += slide_size; // Use op+= over advance_seq_num(): slide_size is of Sequence_numbers, not bytes.

  FLOW_LOG_TRACE("First unreceived packet pointer moved from "
                 "[" << (rcv_next_seq_num - slide_size) << "] to "
                 "[" << rcv_next_seq_num << "].");

  /* Now update the receive window structure.  Maintain invariant described in doc headers for
   * for m_rcv_packets_with_gaps and related members.  Additionally, IF retranmission-related
   * reassembly is in progress (presumably, because retransmission is enabled), and if the new packet bridged
   * gap to the first seq.-#-contiguous packet(s) in the reassembly queue, then add their data to Receive buffer
   * also. */

  // Start of range to delete.
  const Peer_socket::Recvd_pkt_iter start_contig_it = rcv_packets_with_gaps.begin();
  // End of range to delete (just past last element to delete).
  Peer_socket::Recvd_pkt_iter end_contig_it;
  size_t total_written = 0;

  // The following loop is O(n) worst case.
  for (end_contig_it = start_contig_it;
       /* Search until the infinite gap is found; or the first finite gap is found.
        * Note invariant at entry to each loop iteration: rcv_next_seq_num is seq. # just past last received
        * packet's data (so for contiguousness, it must equal the 1st seq. # in next packet). */
       (end_contig_it != rcv_packets_with_gaps.end()) && (end_contig_it->first == rcv_next_seq_num);
       ++end_contig_it)
  {
    Peer_socket::Received_packet& rcvd_packet = *end_contig_it->second;

    if (reassembly_in_progress)
    {
      /* Receive Buffer can be consumed by user threads (not W) at the same time.  Must lock.
       * @todo Probably possible to make the critical section smaller.
       *
       * Conversely, maybe it's better to lock around the entire while () loop, for potentially less
       * locking/unlocking while another thread is reading from buffer, which intuitively "feels" churn-y.
       * Arguments against: the loop may have 0 iterations, meaning the locking was a waste; also, locking
       * once per packet is no worse in aggregate than if we'd received these packets in order without
       * needing reassembly -- and that's the much more typical state of affairs; so it's not like we're
       * adding some unusually excessive amount of locking/unlocking by locking once per packet during
       * reassembly. */
      size_t written;
      size_t buf_size;
      {
        Peer_socket::Lock_guard lock(sock->m_mutex);

        /* Reassemble!  This is constant-time.  Note we don't check for overflow here, but that's because we
         * checked for it cleverly in first enqueueing this in rcv_packets_with_gaps
         * (see sock_data_to_reassembly_q_unless_overflow()). */
        written = sock->m_rcv_buf.feed_buf_move(&rcvd_packet.m_data, std::numeric_limits<size_t>::max());
        // rcvd_packet.m_data is now empty.
        buf_size = sock->m_rcv_buf.data_size();
      }
      total_written += written;

      // Similarly to when receiving a first-gap-filling (or just in-order, if there is no gap) DATA packet:
      rcv_stats.good_data_delivered_packet(written);
      rcv_stats.buffer_fed(buf_size);

      assert(written != 0);
    }

    advance_seq_num(&rcv_next_seq_num, rcvd_packet.m_size);

    FLOW_LOG_TRACE("First unreceived packet pointer moved again to "
                   "[" << rcv_next_seq_num << "]; packet subsumed by this move.");
  } // while (keep encountering contiguous packets)

  // The following, according to STL requirements, is O(k + log n), where k is # erased; thus O(n) worst case.
  rcv_packets_with_gaps.erase(start_contig_it, end_contig_it); // Does nothing if end_contig_it == start_contig_it.
  sock->m_rcv_reassembly_q_data_size -= total_written;
} // Node::sock_slide_rcv_next_seq_num()

size_t Node::sock_max_packets_after_unrecvd_packet(Peer_socket::Const_ptr sock) const
{
  /* The limit itself is not an option but rather computed from other options to be
   * more dynamic.  Let N be the desired max ratio of rcv_packets_with_gaps.size() * max-block-size
   * to the max Receive buffer size, expressed in percent.  Then the max
   * rcv_packets_with_gaps.size() value is N% * <max Receive buffer size> / max-block-size / 100%.
   * N is the option m_st_rcv_max_packets_after_unrecvd_packet_ratio_percent. */
  return uint64_t(sock->opt(sock->m_opts.m_st_rcv_max_packets_after_unrecvd_packet_ratio_percent)) *
         uint64_t(sock->opt(sock->m_opts.m_st_rcv_buf_max_size)) /
         uint64_t(sock->max_block_size()) /
         100;
}

void Node::rcv_get_first_gap_info(Peer_socket::Const_ptr sock,
                                  bool* first_gap_exists, Sequence_number* seq_num_after_first_gap)
{
  // If false, all received packets are followed by all unreceived ones.  Otherwise there's at least 1 gap.
  *first_gap_exists = !sock->m_rcv_packets_with_gaps.empty();
  // If true, then this is the sequence number of the first datum right after that first gap.
  if (*first_gap_exists)
  {
    *seq_num_after_first_gap = sock->m_rcv_packets_with_gaps.begin()->first;
  }
}

void Node::async_acknowledge_packet(Peer_socket::Ptr sock, const Sequence_number& seq_num, unsigned int rexmit_id,
                                    size_t data_size)
{
  // We are in thread W.

  // Plenty of info logged in caller, so don't re-log.
  FLOW_LOG_TRACE("Accumulating for acknowledgment.");

  // Register one packet with N bytes of data (not necessarily acceptable data).
  sock->m_rcv_stats.total_to_send_ack_packet(data_size);

  const size_t acks_pending_before_this = sock->m_rcv_pending_acks.size();

  static_assert(std::is_aggregate_v<Peer_socket::Individual_ack>,
                "We want it to be direct-initializable.");
  static_assert((!std::is_copy_constructible_v<Peer_socket::Individual_ack>)
                  && (!std::is_copy_assignable_v<Peer_socket::Individual_ack>),
                "We want it to be noncopyable but rather passed-around via its ::Ptr.");

  /* Just the starting sequence number sufficient to identify a single packet.  The time point saved
   * here is subtracted from time_now() at ACK send time, to compute the artificial delay introduced
   * by ACK delaying (explained just below).  This helps other side calculate a more accurate RTT by
   * substracting the ACK delay from its RTT measurement. */
  sock->m_rcv_pending_acks.push_back
    (Peer_socket::Individual_ack::Ptr
       (new Peer_socket::Individual_ack{ seq_num, rexmit_id, Fine_clock::now(), data_size }));

  /* m_rcv_pending_acks now stores at least one packet to acknowledge.  We can acknowledge it
   * immediately (modulo UDP layer availability of course).  However, suppose there is a fast stream
   * of packets coming in, such that several DATA packets were read in within one
   * low_lvl_recv_and_handle() call.  Then each DATA packet will result in one ACK packet.
   * This introduces a ton of overhead, as the header is quite large given that the payload is just
   * a Sequence_number.  Instead we would want to pack all the DATA packets' acknowledgments into
   * one ACK packet (unless it overflows, in which case create more ACK packets as needed).  So we
   * only accumulate the individual acknowledgments here; we will possibly send the actual ACK(s) in
   * perform_accumulated_on_recv_tasks(), which runs at the end of low_lvl_recv_and_handle() (or its
   * bro, the async part of async_wait_latency_then_handle_incoming()).
   *
   * Caveat: The above is rock-solid if the different DATA packets being acked were contiguous to
   * each other chronologically.  What if there is another type of packet between some two of these
   * DATAs?  Well, it depends on what it is.  Ignoring the misbehaving/duplicate/whatever packets
   * (SYN, for example) -- which will just be discarded basically -- let's consider the
   * possibilities.  If the packet is ACK, then it is irrelevant; NetFlow (like TCP) is full-duplex
   * (actually more so, since there's no DATA+ACK piggy-backing), therefore the micro-ordering of
   * traffic in opposite directions is irrelevant.  If the packet is RST, then that means the socket
   * will get closed (no longer ESTABLISHED) before we get a chance to send any of the individual
   * acknowledgments.  However, that is more or less OK; if the other side sent RST, then they won't
   * accept any ACKs we may send them anyway.  The only other possibility has to with graceful close,
   * but that is not yet implemented.
   * @todo Revisit this when graceful close is implemented.  (Preliminary idea: force immediate ACK
   * handling when FIN/etc. detected?  Or something.) */

  if (m_socks_with_accumulated_pending_acks.insert(sock).second)
  {
    /* First acknowledgment to be accumulated in this handler (low_lvl_recv_and_handle() or
     * async part of async_wait_latency_then_handle_incoming()).  So mark down whether at that time there were
     * already timer-delayed acknowledgments pending (and how many).  See
     * sock_perform_accumulated_on_recv_tasks() for details on delayed ACKs. */
    sock->m_rcv_pending_acks_size_at_recv_handler_start = acks_pending_before_this;
  }
  // else already had registered pending acknowledgment in this handler.
} // Node::async_acknowledge_packet()

void Node::handle_accumulated_pending_acks(const Socket_id& socket_id, Peer_socket::Ptr sock)
{
  using boost::chrono::milliseconds;
  using boost::chrono::microseconds;
  using boost::chrono::duration_cast;
  using boost::chrono::round;
  using std::vector;

  // We are in thread W.

  // For background see Node::perform_accumulated_on_recv_tasks().

  // For brevity and speed:
  vector<Peer_socket::Individual_ack::Ptr>& pending_acks = sock->m_rcv_pending_acks;

  if (sock->m_int_state != Peer_socket::Int_state::S_ESTABLISHED)
  {
    // For example, we got DATA and then RST on the same socket almost simultaneously.
    FLOW_LOG_TRACE("Was about to perform accumulated acknowledgment tasks on [" << sock << "] but skipping because "
                   "state is now [" << sock->m_int_state << "].");
    return;
  }

  // Check explicit pre-condition.
  assert(!pending_acks.empty());

  /* Deal with any accumulated acknowledgments.  Naively, we'd simply call async_low_lvl_ack_send()
   * here, which would take pending_acks and bundle them up into as few as possible ACK
   * packets and send them off.
   *
   * However, we potentially instead use delayed ACKing as in typical TCP implementations (based on
   * various standard RFCs).  The idea is that a few DATA packets have come in around the same time,
   * but not close enough to be handled in one receive handler.  So upon detecting the first DATA
   * packet in the steady state, start a timer; until it fires accumulate more packets in
   * pending_acks; and when it fires finally assemble and flush (send) the ACK(s).  Something else may trigger
   * the flushing of the ACK(s) ahead of this timer or even immediately.
   *
   * These are situations where we must short-circuit the timer and send the ACK(s)
   * immediately:
   *
   *   1. From TCP (RFC 5681-4.2), which says that an ACK should be generated for at
   *      least every second full-sized (data size = MSS) incoming data segment.  The reasoning is
   *      two-fold: causing bursty sending by the receiver of the ACKs; and slowing down slow start
   *      in Reno (and others) congestion control.  The latter is not really a problem for us (since
   *      ACKs are not cumulative but selective and handled as such by our congestion control logic);
   *      but the former is definitely an easily demonstrable issue. @todo This paragraph is difficult
   *      to understand right now.  There might be 1 or more unintentional meaning inversions, wherein
   *      I mean to say X is good, but instead say X is bad, or vice vera, or at least it's unclear.  Research;
   *      rephrase.
   *
   *   2. Also from TCP (RFC 5681-3.2), which says that an ACK should be
   *      immediately generated upon detecting an out-of-order data segment.  This is to inform
   *      congestion control of any loss event as soon as possible (Fast Recovery algorithm).
   *
   * Note that TCP RFCs don't account for the implementation detail that several packets can be
   * received "simultaneously" (in one handler in our case), nor for selective ACKs (in this
   * context), so when they say we must send an ACK for every 2 incoming segments at least, we do
   * not take this literally.  Instead, we just say that if (here, after a full receive handler has
   * run) there are at least 2 full blocks' worth of pending acknowledgments (there could be many
   * more in theory) and/or there's an out-of-order DATA packet, then we send immediate ACK(s), thus
   * following the spirit of the rules in the RFC.  The spirit of the rule is to short-circuit the
   * timer the moment at least 2 full packets can be acknowledged.
   *
   * We detect both of these situations below and act accordingly.  We also start the delayed ACK
   * timer, if necessary, otherwise.  Oh, and there's a mode to disable delayed ACKs.
   *
   * @todo We may also force immediate ACKing during graceful shutdown.  Revisit when graceful
   * shutdown is implemented.... */

  const Fine_duration delayed_ack_timer_period = sock->opt(sock->m_opts.m_st_delayed_ack_timer_period);

  bool force_ack = delayed_ack_timer_period == Fine_duration::zero(); // Delayed ACKs disabled.

  if (force_ack)
  {
    FLOW_LOG_TRACE
      ("Delayed [ACK] feature disabled on [" << sock << "]; forcing immediate [ACK].  "
       "Receive window state: [" << sock->m_rcv_init_seq_num << ", " << sock->m_rcv_next_seq_num << ") "
       "| " << sock->m_rcv_packets_with_gaps.size() << ":{...}.");
  }
  else if (!sock->m_rcv_packets_with_gaps.empty())
  {
    /* Scan to see if there was an out-of-order DATA packet.  That is to say, have we received a
     * DATA packet -- i.e., have we queued a pending acknowledgment in this receive handler -- that
     * follows at least one unreceived packet in the sequence number space.
     *
     * There is a gap in the received sequence number space, so this is potentially possible.  Scan
     * only the DATA packets (acknowledgments) accumulated in THIS handler (since previous ones
     * have already been checked, and unreceived gaps can't just appear out of nowhere later).  If
     * any is past the first gap, it qualifies.  (The reverse is true.  If it's past any gap, it's
     * past the first gap.) */
    Peer_socket::Individual_ack::Const_ptr ack;
    for (size_t ack_idx = sock->m_rcv_pending_acks_size_at_recv_handler_start;
         ack_idx != pending_acks.size(); ++ack_idx)
    {
      ack = pending_acks[ack_idx];
      if (ack->m_seq_num > sock->m_rcv_next_seq_num)
      {
        force_ack = true;
        break;
      }
    }

    if (force_ack)
    {
      FLOW_LOG_TRACE
        ("On [" << sock << "] "
         "received out-of-order packet [" << ack->m_seq_num << ", size " << ack->m_data_size << ", "
         "rexmit " << ack->m_rexmit_id << "]; "
         "forcing immediate [ACK].  "
         "Receive window state: [" << sock->m_rcv_init_seq_num << ", " << sock->m_rcv_next_seq_num << ") "
         "| " << sock->m_rcv_packets_with_gaps.size() << ":{...}.");
    }
  }
  if (!force_ack)
  {
    // No out-of-order stuff.  See if there are at least N * max-block-size bytes pending to be acknowledged.

    const size_t limit // Default 2.
      = sock->opt(sock->m_opts.m_st_max_full_blocks_before_ack_send) * sock->max_block_size();
    size_t bytes = 0;
    for (Peer_socket::Individual_ack::Const_ptr ack : pending_acks)
    {
      bytes += ack->m_data_size;
      if (bytes >= limit)
      {
        force_ack = true;
        break;
      }
    }

    if (force_ack)
    {
      FLOW_LOG_TRACE("On [" << sock << "] "
                     "accumulated at least [" << limit << "] bytes to acknowledge; "
                     "forcing immediate [ACK].");
    }
  }

  // OK; force_ack is set finally.

  if (force_ack)
  {
    /* Yep, must send ACK(s) now.  There are two possibilities.  One, a delayed ACK timer may
     * already be running.  If so, we should cancel it and send immediately.  If the cancel fails
     * (returns 0 tasks canceled), then it was already queued to fire very soon, so we should
     * just let the ACKing happen that way instead of sending immediately.
     *
     * Two, a timer is not running, so we shouldn't cancel and should just send immediately.
     *
     * How to determine if timer is currently running?  If
     * m_rcv_pending_acks_size_at_recv_handler_start == 0, then the timer was either never scheduled
     * (only scheduled when pending_acks.empty()) or was triggered and handled before the current
     * handler; therefore it is not running.  Otherwise, there were pending acks to send, yet they
     * were not sent by the end of the last handler, which means the timer must be running.
     *
     * (There may be some corner case I'm not imagining such that the timer was running even while
     * m_rcv_pending_acks_size_at_recv_handler_start == 0, but even then the worst that will happen is
     * that we will perform the ACKing here, not cancel that wait, and that timer will
     * harmlessly expire with the timer handler doing nothing.) */

    if (sock->m_rcv_pending_acks_size_at_recv_handler_start != 0)
    {
      FLOW_LOG_TRACE("On [" << sock << "] "
                     "canceling delayed [ACK] timer due to forcing "
                     "immediate [ACK]; would have fired "
                     "in [" << round<milliseconds>(sock->m_rcv_delayed_ack_timer.expires_from_now()) << "] "
                     "from now.");

      Error_code sys_err_code;
      const size_t num_canceled = sock->m_rcv_delayed_ack_timer.cancel(sys_err_code);
      if (sys_err_code)
      {
        FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log the non-portable system error code/message.

        // Pretty unlikely, but let's send RST and abort connection, since something crazy is going on.

        // As above....
        rst_and_close_connection_immediately(socket_id, sock,
                                             error::Code::S_INTERNAL_ERROR_SYSTEM_ERROR_ASIO_TIMER, true);
        // ^-- defer_delta_check == true: for similar reason as in handle_syn_ack_ack_to_syn_rcvd().
        return;
      }
      // else

      if (num_canceled == 0)
      {
        /* Unlikely but legitimate; timer was queued to trigger very soon, so we could not
         * cancel it.  No problem -- just let the ACKing happen per timer.  Log INFO due to
         * rarity of this situation. */
        FLOW_LOG_INFO("On [" << sock << "] "
                      "tried to cancel delayed [ACK] timer while "
                      "forcing [ACK], but it was already just about to fire.");
        force_ack = false;
      }
    } // if (m_rcv_pending_acks_size_at_recv_handler_start != 0) [timer was running]

    // If still forcing immediate ACK, finally do it.
    if (force_ack)
    {
      async_low_lvl_ack_send(sock, true);
      // ^-- defer_delta_check == true: for similar reason as in handle_syn_ack_ack_to_syn_rcvd().

      assert(pending_acks.empty());
    }
  } // if (force_ack)
  else // if (!force_ack)
  {
    /* There are pending individual acks but no reason to send them off right now.  The only
     * remaining question is whether we need to schedule the delayed ACK timer to send them
     * later.  That depends on whether the timer is already running.  If
     * m_rcv_pending_acks_size_at_recv_handler_start == 0, then the timer was either never scheduled
     * or was triggered and handled before the current handler; therefore it is not running.  So
     * in that case we should start it, as we've just received our first ackable DATA since
     * we've sent off our last ACK.  If m_rcv_pending_acks_size_at_recv_handler_start != 0, then the
     * timer must be running, because there were pending acks to send, yet they were not send by
     * the end of the last handler (which would have caused this very code to schedule the
     * timer).
     *
     * (There may be some corner case I'm not imagining such that the timer was running even while
     * m_rcv_pending_acks_size_at_recv_handler_start == 0, but even then it can't possibly be set to
     * the right time [which is S_DELAYED_ACK_TIMER_PERIOD for now], so we need to re-set it
     * anyway.  [Re-setting the expiry time will cancel that running timer wait.  Even if that
     * somehow fails, the worst case is that the ACK(s) will be sent prematurely.]) */

    if (sock->m_rcv_pending_acks_size_at_recv_handler_start == 0)
    {
      // First individual acknowledgment accumulated: start countdown to send the next batch of acknowledgments.

      Error_code sys_err_code;
      sock->m_rcv_delayed_ack_timer.expires_from_now(delayed_ack_timer_period, sys_err_code);
      if (sys_err_code)
      {
        FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log the non-portable system error code/message.

        // Pretty unlikely, but let's send RST and abort connection, since something crazy is going on.

        /* Close connection in our structures (inform user if necessary as well).  Pre-conditions
         * assumed by call: sock in m_socks and sock->state() == S_OPEN (yes, since m_int_state ==
         * S_ESTABLISHED); 3rd arg contains the reason for the close (yes).  This will empty the Send
         * and Receive buffers.  That is OK, because this is the abrupt type of close (error). */
        rst_and_close_connection_immediately(socket_id, sock,
                                             error::Code::S_INTERNAL_ERROR_SYSTEM_ERROR_ASIO_TIMER, true);
        // ^-- defer_delta_check == true: for similar reason as in handle_syn_ack_ack_to_syn_rcvd().
        return;
      }
      // else

      FLOW_LOG_TRACE("On [" << sock << "] "
                     "scheduled delayed [ACK] timer to fire "
                     "in [" << round<milliseconds>(delayed_ack_timer_period) << "].");

      // When triggered or canceled, call this->async_low_lvl_ack_send(sock, false, <error code>).
      sock->m_rcv_delayed_ack_timer.async_wait([this, socket_id, sock](const Error_code& sys_err_code)
      {
        async_low_lvl_ack_send(sock, false, sys_err_code);
      });
      // ^-- defer_delta_check == false: for similar reason as in send_worker_check_state() calling send_worker().
    }
    // else the timer is already started, so just accumulating onto pending_acks is enough.  Done.
  } // if (!force_ack)

  // Register the current # of DATA packets to acknowledge.  Note that we're near the end of current handler.
  sock->m_rcv_stats.current_pending_to_ack_packets(pending_acks.size());
} // Node::sock_perform_accumulated_on_recv_tasks()

void Node::log_rcv_window(Peer_socket::Const_ptr sock, bool force_verbose_info_logging) const
{
  using std::vector;
  using std::string;
  using boost::algorithm::join;

  // We're in thread W.

  // For brevity and a little speed:
  const Peer_socket::Recvd_pkt_map& rcv_packets_with_gaps = sock->m_rcv_packets_with_gaps;

  // force_verbose_info_logging => log the most detail, as INFO (if INFO logging enabled).

  auto const logger_ptr = get_logger();
  if (((!logger_ptr) || (!logger_ptr->should_log(log::Sev::S_DATA, get_log_component()))) &&
      (!(force_verbose_info_logging && logger_ptr->should_log(log::Sev::S_INFO, get_log_component()))))
  {
    // Can't print entire In-flight data structure, but can print a summary, if TRACE enabled.
    FLOW_LOG_TRACE
      ("Receive window state for [" << sock << "]: "
       "[" << sock->m_rcv_init_seq_num << ", " << sock->m_rcv_next_seq_num << ") "
       "| " << rcv_packets_with_gaps.size() << ":{...}.");
    return;
  }
  // else

  /* Construct full printout of the packets we've received past the first unreceived gap.
   *
   * Very verbose and slow!  Even so, if it gets beyond a certain size it's absurd, so skip some in
   * that case even though DATA logging is sanctioned.  (That amount of data cannot really be useful
   * in any case.) */

  vector<string> pkt_strs;
  pkt_strs.reserve(rcv_packets_with_gaps.size());

  const size_t MAX_TO_SHOW = 100;
  bool skipped_some = false;
  size_t count = 0;

  for (Peer_socket::Recvd_pkt_const_iter pkt_it = rcv_packets_with_gaps.begin();
       pkt_it != rcv_packets_with_gaps.end();
       ++pkt_it)
  {
    const bool last_iteration = (count == rcv_packets_with_gaps.size() - 1);

    if ((!skipped_some) && (count > MAX_TO_SHOW) && (!last_iteration))
    {
      // First packet past the limit we can print.  Start skipping mode.
      skipped_some = true;
      ++count;
      continue;
    }
    // else either we are in skipping more from before, or we are not in skipping mode.

    string pkt_str;

    if (skipped_some)
    {
      // We are in skipping mode from before.
      if (!last_iteration)
      {
        // Since it's not the last iteration, skip: print nothing.
        ++count;
        continue;
      }
      // else we are in skipping more from before, and this is the last iteration.  Print the placeholder.
      pkt_str = "[...skipped...] ";
    }
    // Either we are not in skipping mode (just print the thing) or we are and it's last iteration (also print it).

    Sequence_number start, end;
    get_seq_num_range(pkt_it, &start, &end);

    util::ostream_op_to_string(&pkt_str, '[', start, ", ", end, ')');
    pkt_strs.push_back(pkt_str);

    ++count;
  } // for (packets in rcv_packets_with_gaps)

  FLOW_LOG_WITHOUT_CHECKING
    (force_verbose_info_logging ? log::Sev::S_INFO : log::Sev::S_DATA,
     "Receive window state for [" << sock << "]: "
       "[" << sock->m_rcv_init_seq_num << ", " << sock->m_rcv_next_seq_num << ") "
       "| " << rcv_packets_with_gaps.size() << ":{" << join(pkt_strs, " ") << "}.");
} // Node::log_rcv_window()

void Node::handle_ack_to_established(Peer_socket::Ptr sock,
                                     boost::shared_ptr<const Ack_packet> ack)
{
  // We are in thread W.

  /* packet is an ACK, so its payload consists of at least m_rcv_wnd (the current advertised Receive
   * buffer space on the receiver) and packet->m_rcv_acked_packets, which is basically a list of ZERO or
   * more sequence numbers, each of which represents a packet we'd (hopefully) sent that the
   * receiver has received.  Naively we'd just handle the window update and each individual ack here
   * in a loop, then inform congestion control, etc. etc.  However there is an optimization to make.
   * Suppose in the calling low_lvl_recv_and_handle() or async-part-of-async_wait_latency_then_handle_incoming()
   * there are several more ACKs for this socket sock that will be received. This may well happen in
   * high traffic; for instance the sender may have had too many individual acks for one ACK and
   * thus sent several; or maybe the UDP net-stack had a few packets ready by the time boost.asio was
   * free in thread W.  In this case, it is better to collect all the individuals acks in these
   * several ACKs, and then handle them all at the same time.  Why?  Answer: it will update our
   * sender state (what's ACKed, what's dropped) entirely in one go instead of doing it in two or
   * more steps.  Because congestion control activities ("on drop event," "on acknowledgment") are
   * performed after handling all the available acks, it gives a truer, simpler picture to the
   * congestion control module, when compared to giving it one picture and then almost instantly
   * giving it another. Another way to think of it is simply that since the different ACKs arrived
   * at the same time, and all an ACK is is a collection of individual acks that could fit into the
   * ACK packet, then conceptually this is no different from being one super-ACK with all the
   * individual acks contained in it.  Therefore it is at least not worse.
   *
   * (In addition, m_rcv_wnd also affects the decision on whether to send more data over the wire,
   * as can_send() is part of that same algorithm.)
   *
   * Caveat: The above is rock-solid if the different ACKs being combined were contiguous to each
   * other chronologically.  What if there is another type of packet between some two of these ACKs?
   * Well, it depends on what it is.  Ignoring the misbehaving/duplicate/whatever packets (SYN, for
   * example) -- which will just be discarded basically -- let's consider the possibilities.  If
   * the packet is DATA, then it is irrelevant; NetFlow (like TCP) is full-duplex (actually more so,
   * since there's no DATA+ACK piggy-backing), therefore the micro-ordering of traffic in opposite
   * directions is irrelevant.  If the packet is RST, then that means the socket will get closed (no
   * longer ESTABLISHED) before we get a chance to process any of the individual acknowledgments.
   * However, that is more or less OK; if the other side sent RST, then they won't accept any
   * further data we may send after processing the acknowledgments anyway.  The only other
   * possibility has to with graceful close, but that is not yet implemented.
   * @todo Revisit this when graceful close is implemented.  (Preliminary idea: accumulate DATA and
   * FIN/etc. packets and always handle them after handling ACKs.  Then the DATA/FIN stream will not
   * have a chance to disrupt (by initiating closing the connection) the ACK handling, while the ACK
   * handling should have no bearing on the DATA/FIN stream.)
   *
   * So, let's accumulate the individual acks in packet->m_rcv_acked_packets into a big
   * sock->m_rcv_acked_packets to be handled from perform_accumulated_on_recv_tasks() at the end of the
   * current handler.  Similarly save m_rcv_wnd into sock->m_pending_rcv_wnd.  To let that method
   * know sock has a new m_pending_rcv_wnd and possibly non-empty sock->m_rcv_acked_packets, insert sock
   * into m_socks_with_accumulated_acks. */

  /* Note: We're not setting the actual sock->m_snd_remote_rcv_wnd until
   * perform_accumulated_on_recv_tasks().
   *
   * Also note: the latest ACK to arrive in this receive handler will contain the most up-to-date
   * rcv_wnd value (previous ones are overwritten by this). */
  sock->m_snd_pending_rcv_wnd = ack->m_rcv_wnd;

  // It's a (ref-counted) pointer copy. Note there may be 0 elements there, if it's just an m_rcv_wnd update alone.
  sock->m_rcv_acked_packets.insert(sock->m_rcv_acked_packets.end(), // Append.
                                   ack->m_rcv_acked_packets.begin(), ack->m_rcv_acked_packets.end());
  m_socks_with_accumulated_acks.insert(sock); // May already be in there.

  FLOW_LOG_TRACE("NetFlow worker thread working on [" << sock << "].  "
                 "Received and accumulated [" << ack->m_type_ostream_manip << "] with "
                 "[" << ack->m_rcv_acked_packets.size() << "] individual acknowledgments "
                 "and rcv_wnd = [" << ack->m_rcv_wnd << "]; total for this socket in this "
                 "receive handler is [" << sock->m_rcv_acked_packets.size() << "] individual acknowledgments.");

  sock->m_snd_stats.received_low_lvl_ack_packet(ack->m_rcv_acked_packets.empty());
} // Node::handle_ack_to_established()

void Node::handle_accumulated_acks(const Socket_id& socket_id, Peer_socket::Ptr sock)
{
  using std::min;
  using std::vector;
  using boost::tuple;
  using boost::unordered_set;
  using boost::chrono::round;
  using boost::chrono::milliseconds;
  using boost::chrono::seconds;

  /* This is a complex method that does many things.  Therefore readability is hard to accomplish, as the logic
   * makes sense when writing it, but the big picture is hard to see when reading it.  The necessary heavy commenting
   * further increases the size and therefore (along that dimension) decreases readability.  For these reasons,
   * many logically distinct parts were placed into helper methods -- not to increase code reuse but to help
   * the aforementioned consideration. */

  // We are in thread W.

  log_accumulated_acks(sock);
  // Below TRACE messages omit most of the just-logged detail, since it's already logged now.

  // For brevity and a little speed:
  using Acks = vector<Ack_packet::Individual_ack::Ptr>;
  Acks& acked_packets = sock->m_rcv_acked_packets;
  /* To not put already-handled acknowledgments up for handling again in the next run of this method
   * (which would be wrong), we must clear acked_packets before exiting this method.  To be safe,
   * make sure acked_packets.clear() runs no matter how this method exits. */
  util::Auto_cleanup cleanup = util::setup_auto_cleanup([&]() { acked_packets.clear(); });

  /* Handle all the acknowledgments we've received in this receive handler.  Background on the
   * accumulation tactic is in handle_ack_to_established().  As explained in that method, some
   * packet between the first and last ACK received in this handler may have changed state away from
   * ESTABLISHED.  For example, there could have been an RST.  Check for that. */
  if (sock->m_int_state != Peer_socket::Int_state::S_ESTABLISHED)
  {
    // Rare/interesting enough for INFO.
    FLOW_LOG_INFO("NetFlow worker thread working on [" << sock << "].  "
                  "Accumulated [ACK] packets with [" << acked_packets.size() << "] "
                  "individual acknowledgments, but state is now [" << sock->m_int_state << "]; ignoring ACKs forever.");
    return;
  }
  // else OK.  Handle the accumulated acknowledgments.
  assert(sock->m_int_state == Peer_socket::Int_state::S_ESTABLISHED);

  /* The individual acknowledgments are (sequence number, ACK delay in unit X, retransmission ID)
   * triples, where the latter is always zero unless retransmission is enabled.  Let's handle each
   * one by updating m_snd_flying_pkts* (i.e., removing that packet from m_snd_flying_pkts*) and
   * informing congestion control.  Before continuing reading the method please look at the large
   * comments for Peer_socket::m_snd_flying_pkts_by_{sent_when|seq_num} (drawing a diagram might also help).
   *
   * Before continuing, quick discussion of corner cases:
   *
   * Any two given such triples may have equal sequence number/retransmission ID entries.  This
   * means that during the last ACK delay timer or boost.asio handler, while accumulating the
   * acknowledgments for this ACK packet, the receiver received the same packet twice (duplicate).
   * (This can happen due to network misbehavior; and due to ACK loss and other conditions when
   * retransmits are enabled.)  Call 2 such packets P1 and P2, where P1 was received first and thus
   * appears earlier in acked_packets.  How do we handle this?
   *
   * Suppose instead of being in the same ACK, P1 and P2 were in different ACKs that arrived in that
   * order, P1 and P2 (something that certainly could happen depending on how the delayed ACK timer
   * works out).  That situation is basically the same (except that if they're in one ACK there's
   * the added guarantee that we KNOW what P1 is acknowledging arrived to the receiver before
   * what P2 was acknowledging did, which is even more solid knowledge).  Therefore, it makes sense
   * to simply handle each acknowledgment in the ACK in the order they're listed in acked_packets.
   * The 2nd, 3rd, etc. occurrence will thus be treated the same way as if it arrived in a later
   * ACK. */

  /* Congestion control: First see introduction to this topic in class Congestion_control_strategy
   * doc header.  Then resume here.
   *
   * Since information stored in ACKs is of paramount importance to how congestion control views the
   * pipe, congestion control is very relevant in this method: this method is the main (but not
   * only) source of events for m_snd_cong_ctl.
   *
   * These ACK-based events are of interest to m_snd_cong_ctl:
   *
   *   - on_acks(N, M): N bytes in M packets have just been converted from In-flight to
   *     Acknowledged.  Note that these packets have NOT been previously Acknowledged or considered
   *     Dropped (they are In-flight just before the ACK).
   *     - This should also be immediately preceded with M on_individual_ack(N', T, CWND) events, where N'
   *       is the # of bytes in the individual acked packet; and T is the RTT of the packet, and CWND is the
   *       # of bytes in cwnd that was used when the acked data pkt was sent.
   *       In the rest of the discussion I omit this event, as it can be thought of as part of
   *       on_acks() for purposes of the discussion.
   *   - on_loss_event(N', M'): N' bytes in M' packets have just been converted from In-flight to
   *     Dropped.
   *
   * The basic idea is to determine which of these events are implied by the acks passed to this
   * method, inform m_snd_cong_ctl, and then check if the new m_snd_cong_ctl->congestion_window_bytes()
   * value (a/k/a CWND) -- if it has changed -- allows us to now send more bytes (if we have any).
   *
   * An important decision (and one sadly not very explicitly exposed [perhaps as an exercise to the
   * reader, or to avoid being too rigid] in the various TCP RFCs) is how to group these events and
   * in what order.  In other words, do we call on_acks(N, 1) for each acknowledged packet?  Do we
   * then check for drops and call on_loss_event(N', M') immediately, or wait to process all acked
   * packets first?
   *
   * The answer we choose is simple.  First, scan all individual (i.e., for each sent packet) acks
   * given to us and update m_snd_flying_pkts_by_seq_num (the "scoreboard").  While doing so keep track of
   * the cumulative N and M.  Having done that, we will also expose zero or more In-flight packets
   * as Dropped.  (In this method, a given packet is exposed as Dropped if the total number of
   * acknowledged packets AFTER that packet exceeds a constant like 2.  So basically if the acks we
   * process here make that counter exceed that limit for a given packet P, P is Dropped and removed
   * from m_snd_flying_pkts_by_seq_num.)  So after the ack scanning phase, tally up all packets now
   * considered Dropped, which gives us N' and M'.
   *
   * Finally, call on_loss_event(N', M') (assuming N' and M' are not zero).  And then call
   * on_acks(M, N) (assuming N and M are not zero).
   *
   * Let's justify this.  First, assume it's correct to tally these things up and then just
   * call each method once.  Is the "report loss, report acks" order right?  Yes.  Intuitively,
   * m_snd_cong_ctl wants to know about events in the chronological order they occur.  While the Drop(s)
   * are detected at the same time as the Ack(s), the actual packet dropping INFERRED from the
   * Ack(s) occurred in the past; we're only deducing it now.  The received Acks are in fact for
   * packets AFTER the now-Dropped packets.  Hence this is the right order.
   *
   * Now the only remaining thing is to justify combining the ack and drop events in one (each).  For
   * acknowledgments, it's straightforward: so far, most Congestion_control_strategy modules
   * don't need to know about each individual ack, so for simplicity/efficiency we can just combine
   * them.  (However, some algorithms do need it; e.g., FAST would need it; still, many don't.
   * Other modules, like Send_bandwidth_estimator, may also care about individual acks.)
   *
   * What about the drop events?  Why combine all the drops into one?  Should we include all the
   * drops into the one?  To answer, I use as a reference DCCP CCID 2 RFC 4341 (standards track)
   * which describes a protocol similar to ours and implies the following model.  Basically, over
   * time, the pipe experiences a series of 0 or more congestion events (more accurately loss
   * events).  Two loss events cannot overlap in this implied model.  Thus any given Dropped packet
   * belongs to exactly one loss event.  Here is how the RFC (section 5) more or less formally
   * defines whether 2 packets belong to one event: "As in TCP, two losses [...] are considered part
   * of a single congestion event when the second packet was sent before the loss [...] of the first
   * packet was detected."  Presumably the text also assumes that the "second" packet was
   * found to be dropped either at the same or later time as the "first" packet was found to be
   * dropped (otherwise the text makes no sense, as the very earliest Dropped packet would be in the
   * same congestion event as the very last Dropped packed in a very long session).  Let's build an
   * algorithm inductively based on this definition.
   *
   * At first there are no loss events.  We get a group of acks which render another group of
   * packets P1, P2, ... (in order of increasing sequence number) Dropped.  Certainly P1 is in a
   * loss event; call it L1.  P2 was found to be dropped at the same or later time as P1; and it was
   * obviously sent before L1 was detected (which was NOW; call it T1).  So P2 is in loss event L1.
   * Similarly, so is P3, P4, ....  Now let's say some time passes and we get more acks and thus
   * dropped packets P7, P8, P9, ....  Suppose P7 was sent before T1 (but found Dropped at T2 > T1),
   * which is quite possible (e.g., T2 could be just after T1).  Then by the above definition P7 is
   * in loss event L1 (no new loss event).  P8 could be in the same situation.  In fact, all Dropped
   * packets from this ack group may be in L1.  Suppose, conversely, that P9 was sent AFTER T1.  By
   * the above definition, it is part of a new loss event L2, detected at T2.  Now P10, is certainly
   * in L2 as well, since it was sent before T2, obviously.  Thus we can, for each Dropped packet P,
   * determine whether it's part of the preceding loss event or part of a new one.
   *
   * Intuitively, it makes sense as well.  If, say, we got 5 dropped packets at the same time, and
   * informed Congestion_control_classic (Reno) with 5 calls to on_loss_event(), then CWND would get
   * halved 5 times!  Intuitively that's not right (and way too conservative).  More likely the 5
   * packets belong to the same congestion or loss event, so CWND should only be halved once.  Then
   * the only question is how to group packets into separate loss events.  The above algorithm,
   * roughly speaking, considers two packets as part of the same loss event if they're within an RTT
   * of each other (indeed RFC 4341 says one can use the SRTT to approximate the above algorithm,
   * although we choose to use the exact definition instead).
   *
   * Therefore the final algorithm is justified and is as follows:
   *
   *   0. Before the current method is ever called, set time stamp m_snd_last_loss_event_when =
   *      -infinity.
   *   1. Scan all acknowledgments, updating m_snd_flying_pkts* and m_snd_flying_bytes.
   *      Keep track of total acknowledgment stats (bytes and packets). (Inform side modules like
   *      Send_bandwidth_estimator with any required individual ack info like RTTs.)
   *      Ignore acks of packets not in m_snd_flying_pkts* (not In-flight).
   *   2. Tally up which packets are exposed as Dropped by the above m_snd_flying_pkts* updates.
   *      Keep track of total loss stats (bytes and packets).  However, when doing the latter ignore
   *      any packet P for which P.m_sent_when < m_snd_last_loss_event_when.
   *   3. If at least 1 packet exposed as Dropped in step 2, call
   *      m_snd_cong_ctl->on_loss_event(...stats...); and set m_snd_last_loss_event_when to the current time,
   *      marking this the start of a new loss event.
   *   4. If at least 1 packet exposed as Acknowledged in step 1, call
   *      m_snd_cong_ctl->on_acks(...stats...). */

  // Set up work state and save certain "before" values.

  // For RTT at least.  Use steady, high-res clock.  Use one coherent value for entire method to simulate simultaneity.
  const Fine_time_pt time_now = Fine_clock::now();

  // For brevity and a little speed:
  const bool rexmit_on = sock->rexmit_on();
  auto& snd_stats = sock->m_snd_stats;
  auto& snd_flying_pkts_by_when = sock->m_snd_flying_pkts_by_sent_when;
  /* These guys are only stored in Peer_socket (instead of creating locally here) for a bit of performance.  Reuse now.
   * Note that clear() is a very fast operation; it will essentially just set the internal element count to 0. */
  auto& pkts_marked_to_drop = sock->m_snd_temp_pkts_marked_to_drop;
  pkts_marked_to_drop.clear();

  // To check, at the end, whether we've changed can_send() false => true.
  const bool could_send_before_acks = can_send(sock);
  // To check, at the end, whether we've changed snd_deqable() false => true.
  const bool had_rexmit_data_before_acks = !sock->m_snd_rexmit_q.empty();

  /* Includes each order number (unique packet ID) for which the packet was acknowledged.
   * Used for Drop_timer events to register at the bottom; and also to feed the priority queue high_ack_count_q
   * (explained below in detail). */
  unordered_set<Peer_socket::order_num_t> flying_now_acked_pkts;

  // These are the N, M arguments to on_acks() described just above in big comment.
  size_t clean_acked_bytes = 0;
  size_t clean_acked_packets = 0;

  /* The are the individual T, N' (RTT, acked_bytes, sent_cwnd_bytes) arguments to pass to on_individual_ack() described
   * just above in big comment.  We will be accumulating these across all the acks in the loop below. */
  using Clean_acked_packet = tuple<Fine_duration, size_t, size_t>;
  vector<Clean_acked_packet> clean_acked_packet_events;
  clean_acked_packet_events.reserve(min(acked_packets.size(), snd_flying_pkts_by_when.size())); // Small optimization.

  /* Handle each acknowledgment in the order that the corresponding packet was received by other
   * side (earliest to latest) per above discussion. */
  for (const Ack_packet::Individual_ack::Const_ptr ack : acked_packets)
  {
    /* Use helper to classify this individual ack as one of the following:
     *   - Malformed/illegal. => error_ack is true. Else:
     *   - Legal but referring to an already-acknowledged packet, or arriving too late. => dupe_or_late is true.
     *     - The packet being acknowledged is unknown. => flying_pkt_it == past_oldest() (a/k/a end()).
     *     - The packet being acknowledged is known. => flying_pkt_it points to that acked packet.
     *   - Legal and acking a not-yet-acked packet, arriving in time. => dupe_or_late is false.
     *                                                                => flying_pkt_it points to that acked packet.
     * Note: The helper takes care of snd_stats updating, closing socket on error, and relevant logging. */

    Peer_socket::Sent_pkt_ordered_by_when_iter flying_pkt_it;
    bool dupe_or_late;

    const bool error_ack = !categorize_individual_ack(socket_id, sock, ack, &dupe_or_late, &flying_pkt_it);
    if (error_ack)
    {
      return; // Fatal error for entire socket (malformed ack, etc.).  Socket is closed; all logged; bail out now.
    }
    // else

    // Note these may never be initialized.
    Fine_duration round_trip_time;
    Peer_socket::Sent_packet::Const_ptr flying_pkt;
    const Peer_socket::Sent_packet::Sent_when* sent_when;

    // Compute RTT, assuming we ID'ed the original DATA.  (RTT logged even if we still throw away the ack just below.)
    if (flying_pkt_it != snd_flying_pkts_by_when.past_oldest())
    {
      // Use helper to compute RTT and, as a side effect, get `Sent_when* sent_when` set to point to appriate structure.
      flying_pkt = flying_pkt_it->second;
      round_trip_time = compute_rtt_on_ack(flying_pkt, time_now, ack, &sent_when); // It logs details.
    } // Done computing (if possible) RTT and logging it.

    if (dupe_or_late)
    {
      continue; // Do NOT return!  There may well be valid individual acks after it.  All logged; get out now.
    }

    // else it's an in-time acking of DATA packet that has not yet been acked (is considered In-flight)!
    assert(!dupe_or_late);
    // The following is guaranteed by helper above, since !dupe_or_late. Hence, also, flying_pkt, sent_when, RTT set.
    assert(flying_pkt_it != snd_flying_pkts_by_when.past_oldest());
    assert(flying_pkt);

    // Update SRTT, etc.
    new_round_trip_time_sample(sock, round_trip_time);

    /* Similarly, inform congestion control (see big comment at top of method).  Some strategies
     * use individual acks to gauge the pipe's properties.  Save the info to
     * later call on_individual_ack().  Why not just call
     * it here?  Answer: Congestion_control_strategy interface specifies that
     * on_individual_ack() must be called AFTER on_loss_event() (which can only be called once
     * we've fully updated snd_flying_pkts, thus handled all acks).  It also specifies that
     * snd_flying_pkts must be updated to reflect the handled ack.  So we have no choice but
     * to save it.  (@todo Performance?) */
    const size_t bytes_acked = flying_pkt->m_size;
    const size_t cwnd_bytes = sent_when->m_sent_cwnd_bytes;
    clean_acked_packet_events.emplace_back(round_trip_time, bytes_acked, cwnd_bytes);

    // Maintain invariant.  Packet acknowledged, so remove from In-flight packet list and related structures.
    snd_flying_pkts_erase_one(sock, flying_pkt_it);

    // Bona fide In-flight->Acknowledged data; accumulate to inform congestion control below.
    clean_acked_bytes += bytes_acked;
    ++clean_acked_packets;

    /* If we got here, then it is in fact what we consider a valid acknowledgment of packet
     * sent at time sent_when.  Therefore, we should increment m_acks_after_me for any packet that has NOT
     * been acknowledged that was sent earlier than sent_when.  (Later we'd consider Dropped any
     * packets for which this value is too high, as in TCP Fast Recovery/Retransmit.)  Note that if
     * retransmission is off, that's the same as all packets with a lower first sequence number.
     * However if retransmission is on, then a packet may have a lower sequence number but be sent
     * later.  Thus we use sent_when and not seq_num.
     *
     * Naively, we could just have a for () loop here to increment all such data members.  However
     * that's inefficient -- order O(k * n), where k = acked_packets.size() and n =
     * snd_flying_pkts*.size(), in the worst case.  Moreover, some of the Sent_packet structs in
     * which we increment m_acks_after_me may be acknowledged and thus erased from snd_flying_pkts*
     * in subsequent iterations of the for () loop we are in, wasting that work.
     *
     * So instead we count the individual acks in a hash map that maps sent_when to the number of
     * times (in this ACK) that sequence number's packet was validly acknowledged.  This is O(k)
     * amortized total.  Then elsewhere we use that hash map to more efficiently update m_acks_after_me
     * where appropriate.  In addition, this hash map is used to register certain Drop_timer
     * at the end of the method. */

    /* Note that we track these by "order number"; each sent packet (no matter if retransmitted or
     * not) gets a unique order number, higher than all previous.  Since no two packets will have
     * the same order number, we keep a set of order numbers. */
    flying_now_acked_pkts.insert(sent_when->m_order_num);
  } // for (all acked_packets)

  /* snd_flying_pkts* is updated w/r/t removing the In-flight-now-acked packets.  Now, realize that
   * for a given packet P still In-flight, if packets sent BEFORE it have just become acked, intuitively
   * it raises the probability P has been lost and should be considered Dropped.  In fact, as explained in
   * helper categorize_pkts_as_dropped_on_acks(), if one finds the latest-sent such packet P, then all
   * packets sent before it should all be dropped as well.  So, let's find this latest-sent P: */
  const Peer_socket::Sent_pkt_ordered_by_when_iter last_dropped_pkt_it
    = categorize_pkts_as_dropped_on_acks(sock, flying_now_acked_pkts);

  /* OK, so P and all In-flight packets sent before it must be dropped.  This helper takes all the actions
   * necessary (or at least records data we use to take such actions below) w/r/t all those packets.
   * Namely: erases them from snd_flying_pkts*; accumulates packet and bytes counts to do with these
   * dropped packets; saves the packet IDs from Drop timer purposes into pkts_marked_to_drop. */
  size_t dropped_pkts;
  size_t dropped_bytes;
  size_t cong_ctl_dropped_bytes;
  size_t cong_ctl_dropped_pkts;
  if (!drop_pkts_on_acks(sock, last_dropped_pkt_it,
                         &cong_ctl_dropped_pkts, &cong_ctl_dropped_bytes,
                         &dropped_pkts, &dropped_bytes, &pkts_marked_to_drop))
  {
    return; // Already closed/logged/etc. (too many retransmissions probably).
  }

  // As long promised since the top of this method, let congestion control (and B/W estimator) know what happened!

  /* Bandwidth estimation: It can be useful to estimate the available outgoing bandwidth (available
   * meaning the total bandwidth of the empty pipe minus any other traffic other than this
   * connection [NetFlow or otherwise] currently occupying this pipe).  Mostly it's useful for certain
   * congestion control strategies like Congestion_control_classic_with_bandwidth_est, but it may be
   * good information to have if only for the user's general information.  Therefore we keep an
   * independent m_snd_bandwidth_estimator regardless of the congestion control strategy in use.
   * Like Congestion_control_strategy, it updates its state based on events.  It currently cares
   * about at least one event: on_acks(N), where N is the number of bytes acknowledged.  This is
   * very similar to the on_acks(N, M) event for congestion control (see above).  None of the other
   * aspects of the above discussion (such as loss events) apply to m_snd_bandwidth_estimator. */

  // Note that the order is as required by Congestion_control_strategy() API: loss, individual acks, consolidated acks.

  // Report loss event info to congestion control.
  if (dropped_pkts != 0)
  {
    // @todo Might be too verbose to keep it as INFO!
    FLOW_LOG_INFO("NetFlow worker thread working on [" << sock << "].  "
                  "Considering Dropped: [" << dropped_bytes << "] bytes = [" << dropped_pkts << "] packets.");

    if (cong_ctl_dropped_pkts != 0) // Again, cong_ctl_dropped_pkts != dropped_pkts, potentially.
    {
      // New loss event!
      assert(cong_ctl_dropped_bytes != 0); // Empty blocks not allowed (should have been eliminated by now).

      FLOW_LOG_INFO("cong_ctl [" << sock << "] update: loss event: "
                    "Dropped [" << cong_ctl_dropped_bytes << "] bytes "
                    "= [" << cong_ctl_dropped_pkts << "] packets.");

      sock->m_snd_cong_ctl->on_loss_event(cong_ctl_dropped_bytes, cong_ctl_dropped_pkts);
      sock->m_snd_last_loss_event_when = Fine_clock::now();

      // As a silver lining, we probably got some nice new acknowledgments following that drop.
    }
  }
  else
  {
    assert(dropped_pkts == 0);
    assert(cong_ctl_dropped_pkts == 0);
  }

  if (clean_acked_packets != 0)
  {
    assert(clean_acked_bytes != 0); // Empty blocks not allowed (should have been eliminated by now).
    assert(!clean_acked_packet_events.empty());

    // Report individual (clean) acks to congestion control.
    for (const auto& [rtt, bytes, cwnd_bytes] : clean_acked_packet_events)
    {
      FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: clean individual acknowledgment: "
                     "[" << sock->bytes_blocks_str(bytes) << "] with RTT [" << round<milliseconds>(rtt) <<
                     "] and sent_cwnd_bytes [" << cwnd_bytes << "].");

      sock->m_snd_cong_ctl->on_individual_ack(rtt, bytes, cwnd_bytes);
    }

    FLOW_LOG_TRACE("cong_ctl/bw_est [" << sock << "] update: clean acknowledgments: "
                   "[" << sock->bytes_blocks_str(clean_acked_bytes) << "] = "
                   "[" << clean_acked_packets << "] packets.");

    // Report the totality of (clean) acks to congestion control and bandwidth estimator.
    sock->m_snd_bandwidth_estimator->on_acks(clean_acked_bytes);
    sock->m_snd_cong_ctl->on_acks(clean_acked_bytes, clean_acked_packets);
  }

  /* For debugging it can be useful to log socket state right after loss and handling everything.
   * Do so but only if the last time we so logged was some time ago; this is a CPU-intensive
   * operation.
   *
   * Also, register dropped data in snd_stats. */
  if (dropped_pkts != 0)
  {
    // Register that we have convered N bytes over M packets from In-flight to Acknowledged.
    snd_stats.dropped_data(dropped_bytes, dropped_pkts);

    const seconds MIN_TIME_BETWEEN_LOGS(1);
    const Fine_duration since_last_loss_sock_log = Fine_clock::now() - m_last_loss_sock_log_when;

    if (since_last_loss_sock_log > MIN_TIME_BETWEEN_LOGS)
    {
      FLOW_LOG_INFO("Will log socket state on loss, because last such loss-driven logging was "
                    "[" << round<milliseconds>(since_last_loss_sock_log) << " >"
                    " " << MIN_TIME_BETWEEN_LOGS << "] ago.");
      sock_log_detail(sock);
      m_last_loss_sock_log_when = Fine_clock::now();
    }
    else
    {
      FLOW_LOG_INFO("Will NOT log socket state on loss, because last such loss-driven logging was "
                    "[" << round<milliseconds>(since_last_loss_sock_log) << " <="
                    " " << MIN_TIME_BETWEEN_LOGS << "] ago.");
    }
  }

  // Log the send window state after the above changes (if at least TRACE enabled).
  log_snd_window(sock);

  /* Handle possible effect of above activities on the Drop Timer.  (It may get disabled or restarted anew.)
   * Why not just do this right when we erase the associated packets from snd_flying_pkts*?  Answer: We don't want to
   * trigger disruptive behavior like possibly retransmitting everything in the middle of all that accounting
   * which is not yet complete.  Now it's complete, so it's the right time to handle this.
   *
   * Recall that snd_flying_pkts* have been updated and no longer contain the associated packets' info. */

  const Drop_timer::Ptr drop_timer = sock->m_snd_drop_timer;
  drop_timer->start_contemporaneous_events();

  for (const auto pkt_order_num : flying_now_acked_pkts)
  {
    drop_timer->on_ack(pkt_order_num);
    drop_timer->on_packet_no_longer_in_flight(pkt_order_num);
  }
  for (const auto pkt_order_num : pkts_marked_to_drop)
  {
    drop_timer->on_packet_no_longer_in_flight(pkt_order_num);
  }

  drop_timer->end_contemporaneous_events();

  /* As avertised, handle the rcv_wnd update: the latest ACK we are handling here contains the
   * latest info about the Receive buffer space on the other side that is available. */
  if (sock->m_snd_pending_rcv_wnd != sock->m_snd_remote_rcv_wnd)
  {
    FLOW_LOG_TRACE("Other side advertised "
                   "rcv_wnd change [" << sock->m_snd_remote_rcv_wnd << "] => [" << sock->m_snd_pending_rcv_wnd << "].");
    sock->m_snd_remote_rcv_wnd = sock->m_snd_pending_rcv_wnd;
    /* Why have this intermediate m_snd_pending_rcv_wnd thing at all then?  Answer: can_send(),
     * checked at the start of this method and saved into could_send_before_acks, uses the "before
     * handling the ACKs" state, which should not yet include the receive window update.  Then
     * since we update m_snd_remote_rcv_wnd after that is computed, but before can_send() is
     * re-checked just below, we are able to see if the ACKs have changed can_send() from false to
     * true. */

    /* Register whether after this window update, if we had a packet to send and no data In-flight,
     * we would be able to send at least one full DATA packet or not (i.e., can_send() would return
     * true). That is, register whether Receive window is ~0. */
    sock->m_snd_stats.updated_rcv_wnd(sock->m_snd_remote_rcv_wnd < sock->max_block_size());
  }

  /* We've received ACKs and thus have quite likely reduced the number of bytes we
   * consider In-flight.  Moreover we may have increased CWND.  Moreover we may have added packets
   * to retransmit queue (if retransmission is on).  Moreover we may have increased m_snd_remote_rcv_wnd.
   * Therefore can_send() may now return true while at the beginning of the method it returned
   * false; and similarly for snd_deqable().  So have send_worker() check and send more if possible.
   * See Node::send() for discussion of overall strategy on this topic. */
  if ((!could_send_before_acks) || (rexmit_on && (!had_rexmit_data_before_acks)))
  {
    send_worker(sock, true);
    /* ^-- defer_delta_check == true: because the only way to get to this method is from
     * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
     * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
  }
} // Node::handle_accumulated_acks()

bool Node::categorize_individual_ack(const Socket_id& socket_id, Peer_socket::Ptr sock,
                                     Ack_packet::Individual_ack::Const_ptr ack,
                                     bool* dupe_or_late, Peer_socket::Sent_pkt_ordered_by_when_iter* acked_pkt_it)
{
  assert(dupe_or_late);
  assert(acked_pkt_it);

  /* This helper of handle_accumulated_acks() exists to make the latter method briefer/readable, not for code reuse
   * as of this writing. It figures out whether the given individual ack is invalid, valid but duplicate/late, or
   * valid and on-time. Results go into the return value and *dupe_or_late and *acked_pkt_it. */

  /* Now to discuss what happens when an ACK is received, with a seemingly valid sequence number
   * (i.e., in [m_snd_init_seq_num + 1, m_snd_next_seq_num - 1] range) -- but the corresponding
   * packet is not in m_snd_flying_pkts_by_seq_num.  What does this mean?  One, unlikely, possibility is
   * that it's a fake/wrong acknowledgment, not pertaining to any packet we'd sent but in the range
   * of sequence numbers we did send (in other words, the sequence number is in the right range but
   * doesn't correspond to a first sequence number of a packet we'd really sent).  Unfortunately we
   * have no way to detect that fully, since it's not in m_snd_flying_pkts_by_seq_num, and that's basically the only
   * place we store packet boundaries of sent packets.  Suppose we eliminate that possibility.
   *
   * Then the only remaining possibility is that this acknowledgment is a duplicate of a previous
   * one, which had caused us to remove that packet from m_snd_flying_pkts_by_seq_num.  So, how DO we handle
   * a duplicate acknowledgment?  We already know they got packet, as we've already measured RTT
   * from the previous copy of this ack, so there's nothing useful for us.  Conclusion: ignore
   * duplicate acknowledgments.
   *
   * Note that the above discussion pertains to a dupe ack where both the sequence number and the
   * retransmission ID are the same as a previous one.  If the retransmission ID is different (only
   * legal when retransmission is enabled), that's a different situation -- the acknowledgment is
   * not duplicate but rather acknowledging a different send attempt for the same-numbered packet.
   * That is less of a corner case and is handled below explicitly.
   *
   * Sent, unacknowledged packets are eventually considered Dropped.  In terms of our data structures
   * they are handled just like acknowledged ones.  Therefore, an acknowledgment of such a Dropped
   * packet may arrive.  This is a "late" acknowledgment.  It is treated just like a duplicate
   * acknowledgment (in fact, there is no way to tell them apart).  (Note that a packet is still
   * considered Dropped even if retransmission is on -- it's just that in that case it's also queued
   * on the retransmission queue to be re-sent when possible.)
   *
   * Another caveat is that two acknowledgments that are duplicates of each other can get
   * mis-ordered and thus arrive in opposite order.  Thus the one with the longer one-way time would
   * yield the higher RTT, while the shorter one would get ignored.  However, RTT measurement is an
   * art, not a science, so this is acceptable.
   *
   * @todo Acknowledgments themselves could actually be identified with something other other
   * than sequence numbers and retransmission IDs; e.g., with reflected sender time stamps.  Then
   * one could do fancier stuff... but let's not overdo it for now. */

  // For brevity and a little speed:
  const bool rexmit_on = sock->rexmit_on();
  auto& snd_flying_pkts_by_when = sock->m_snd_flying_pkts_by_sent_when;
  auto& snd_flying_pkts_by_seq = sock->m_snd_flying_pkts_by_seq_num;
  auto& snd_stats = sock->m_snd_stats;

  // First sequence number in acknowledged packet.
  const Sequence_number& seq_num = ack->m_seq_num;
  // Retransmission ID (0 = first attempt, 1 = 1st retransmission, 2 = 2nd, ...).
  const unsigned int rexmit_id = ack->m_rexmit_id;
  assert(rexmit_on || (rexmit_id == 0)); // Should be guaranteed by deserialization.

  // Register one individual acknowledgment of unknown # of bytes of data (may or may not be acceptable).
  snd_stats.received_ack();

  /* Ensure it's within the range of sequence numbers we've already sent.
   * Note that this doesn't really guarantee its validity.  It could be in that range but still
   * not correspond to any packet we'd actually sent.  We try to detect that below. */

  if (!util::in_open_open_range(sock->m_snd_init_seq_num, seq_num, sock->m_snd_next_seq_num))
  {
    /* Either the other side is an a-hole, or somehow a socket_id was reused from a recent
     * connection, which we do try to avoid like the plague.  Therefore, send them an RST and
     * abort connection.  If they send more data packets to this port (which is quite possible;
     * many could already be on the way), they'll get more RSTs still. */

    // Interesting/rare enough to log a WARNING.
    FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                     "Received [ACK]; "
                     "acknowledgment [" << seq_num << ", ...) is outside (ISN, snd_next) "
                     "range (" << sock->m_snd_init_seq_num << ", " << sock->m_snd_next_seq_num << ").");

    // Register one individual acknowledgment of unknown # of bytes of data (not acceptable due to error).
    snd_stats.error_ack();

    /* Close connection in our structures (inform user if necessary as well).  Pre-conditions
     * assumed by call: sock in m_socks and sock->state() == S_OPEN (yes, since m_int_state ==
     * S_ESTABLISHED); 3rd arg contains the reason for the close (yes).  This will empty the Send
     * and Receive buffers.  That is OK, because this is the abrupt type of close (error). */
    rst_and_close_connection_immediately(socket_id, sock,
                                         error::Code::S_SEQ_NUM_IMPLIES_CONNECTION_COLLISION, true);
    /* ^-- defer_delta_check == true: because the only way to get to this method is from
     * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
     * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
    return false; // Other out-params are meaningless.
  }
  // else within sane range.

  // Check if the sequence number matches that of one of the packets we've sent and want acnowledged.
  *acked_pkt_it = snd_flying_pkts_by_when.find(seq_num);
  if (*acked_pkt_it == snd_flying_pkts_by_when.past_oldest()) // A/k/a end().
  {
    /* No such packet.  Assuming no foul play/dumbassery, it's probably a duplicate acknowledgment
     * (i.e., we've already sent and got the ack, removing that packet from snd_flying_pkts*)
     * or a late acknowledgment (i.e., we've already sent and eventually considered Dropped the
     * the packet, removing it from snd_flying_pkts*).
     *
     * There is a corner case if retransmission is on.  Suppose we sent packet P, consider it
     * Dropped (removing it from snd_flying_pkts*), and thus we place it on retransmission
     * queue.  Suppose there is not enough CWND space to send it right away, so while it's pending
     * on that queue, we now get a late ack for it.  Ideally in this case we'd remember it was in
     * retransmission queue, remove it from there, and basically act as if we hadn't removed it
     * from snd_flying_pkts* and got the ack for it.  Instead we're just going to ignore this
     * information and needlessly retransmit.  So why do this?  Answer: It is troublesome to
     * design and code this.  The part where we wouldn't retransmit it is fairly straightforward
     * and is a nice @todo.  However acting as if it was normally ACKed after all is complex; for
     * instance, since we thought it was Dropped, we already informed m_cong_ctl of the loss event
     * -- how can we undo that in a clean way?  It does not seem worth it.  Again, checking
     * and updating the retransmission queue, though, is a nice @todo (but would ideally need fast
     * lookup into that linked list so not totally trivial).
     *
     * So, let's say that the concession described in the previous paragraph is OK.
     *
     * Could also be invalid.  We only know seq_num (one boundary of packet), so how do we detect
     * it's invalid?  One case where we know it's invalid is if this left boundary happens to be
     * straddled by a sequence number range in an element of snd_flying_pkts_by_seq.  That would mean
     * that the same sequence number is in two different packets, which is in no way legal.
     * Example: we sent [5, 10), then received ACK with [7, ...).  7 is inside [5, 10) and is
     * thus illegal. */

    /* Here's the technique we use.  snd_flying_pkts_by_seq.upper_bound(S) gets the first packet
     * [U1, U2) such that U1 > S.  Let prev(P) denote the packet preceding P in
     * snd_flying_pkts_by_seq; let prev([U1, U2)) = [L1, L2).  Note that [U1, U2) may not exist
     * -- i.e., nothing after S is in the map.  If so, [U1, U2) == snd_flying_pkts_by_seq.end().  Even
     * in that case [L1, L2) = prev([U1, U2)) MAY still exist; it is the last element of
     * snd_flying_pkts_by_seq in that situation.
     *
     * Given that, here are all the situations that mean P is straddled by a packet:
     *
     * - S inside [U1, U2) or any packet after it.
     *   - Impossible.  U1 > S by definition; so S is not inside any packet at U1 or later.
     * - S inside [L1, L2).
     *   - Possible.  We know S > L1, since otherwise S <= L1, which means we can't be inside this
     *     if (and we are), or snd_flying_pkts_by_seq.upper_bound(S) == [L1, L2) (not true, since
     *     snd_flying_pkts_by_seq.upper_bound(S) == [U1, U2), which != [L1, L2)).  So, since S > L1,
     *     we must check for S < L2.  If true, S is straddled.
     * - S inside some packet [K1, K2) before [L1, L2).
     *   - Impossible.  Suppose S is inside [K1, K2) immediately preceding [L1, L2).  Then
     *     snd_flying_pkts_by_seq.upper_bound(S) == [L1, L2).  But we already know
     *     snd_flying_pkts_by_seq.upper_bound(S) == [U1, U2) (which != [L1, L2)).  So that's
     *     impossible.  Repeat this logic for all packets [K1, K2) preceding [L1, L2) to show that
     *     it can't be straddled by any of those either.
     *
     * Therefore, S is straddled by a packet if and only if:
     *   - prev(snd_flying_pkts_by_seq.upper_bound(S)) exists; call it [L1, L2); and
     *   - S < L2.
     *
     * This can be further restated as:
     *   - snd_flying_pkts_by_seq.upper_bound(S) != snd_flying_pkts_by_seq.begin(); and
     *   - (letting [L1, L2) = prev(snd_flying_pkts_by_seq.upper_bound(S)))
     *     S < L2.
     *
     * So check for that. */

    // Find U.
    Peer_socket::Sent_pkt_ordered_by_seq_const_iter pkt_it = snd_flying_pkts_by_seq.upper_bound(seq_num);
    // Check that prev(U) exists.
    if (pkt_it != snd_flying_pkts_by_seq.begin())
    {
      // prev(U) = L exists.  Compute L.
      --pkt_it;
      // Compute [L1, L2), and check for straddling: S < L2.  pkt_it->second points into snd_flying_pkts_by_when.
      Sequence_number l1, l2;
      get_seq_num_range(pkt_it->second, &l1, &l2);

      assert(l1 < seq_num); // Sanity-check of upper_bound().
      if (seq_num < l2)
      {
        // Straddles.  Other side is sending us bad stuff.  As above, warn and RST/close.

        // Register one individual acknowledgment of unknown # of bytes of data (not acceptable due to error).
        snd_stats.error_ack();

        FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                         "Received [ACK]; "
                         "acknowledgment [" << seq_num << ", ...) is at least partially inside "
                         "packet [" << l1 << ", " << l2 << ").");
        rst_and_close_connection_immediately(socket_id, sock, error::Code::S_SEQ_NUM_ARITHMETIC_FAILURE, true);
        /* ^-- defer_delta_check == true: because the only way to get to this method is from
         * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
         * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
        return false; // Other out-params are meaningless.
      }
      // else if (seq_num >= l2) { It's past [L1, L2); does not straddle. }
    }
    // else { Legit because there is no packet L that could possibly straddle seq_num. }

    /* OK, so NOW do we know it's a duplicate/late acknowledgment?  Well, no.  Suppose we sent packet
     * [5, 10) and get ACK with [5, ...).  That's fine.  So we erase [5, 10) from
     * snd_flying_pkts_by_seq.  Now say we get ACK with [7, ...).  Well, that's in the
     * [m_snd_next_seq_num, m_snd_next_seq_num) range certainly; and it doesn't get straddled by
     * any member of snd_flying_pkts_by_seq.  Yet it's certainly invalid: we never sent (and could've
     * never sent) [7, ...).  We can't know that, however, since [5, 10) is gone from
     * snd_flying_pkts_by_seq.  Is this OK?  More or less, yes.  What do we do with a duplicate/late
     * acknowledgment just below?  We log and ignore it.  That doesn't seem harmful.  NORMALLY
     * when something is invalid we'd RST and close connection, but here we can't know we should
     * do that; however ignoring it still seems fine and better than doggedly inventing data
     * structures to detect this corner case.
     *
     * What about m_snd_cong_ctl?  Should we report this in m_snd_cong_ctl->on_acks()?
     * No.  on_acks() specifically documents that it wants info on
     * In-flight->Acknowledged acknowledgments, not duplicates.  (Briefly,
     * that's because it's measuring sent data in the pipe; acknowledgment duplication has unclear
     * implications about what it's acknowledging; it is unlikely that it represents more pipe
     * being available than if only one acknolwedgment had been received.  In any case this should
     * hopefully be pretty rare and thus not too significant either way.)
     *
     * Same reasoning for not counting it in m_snd_bandwidth_estimator->on_acks(). */

    // Per above discussion, ignore duplicate (or maybe invalid, but we can't know/assume that) acknowledgment.

    // Register one individual acknowledgment of unknown # of bytes of data (late, dupe, or maybe invalid).
    snd_stats.late_or_dupe_ack();

    FLOW_LOG_INFO("NetFlow worker thread working on [" << sock << "].  "
                  "Acknowledged packet [" << seq_num << ", ...) is duplicate or late (or invalid).  "
                  "RTT unknown.  Ignoring.");

    // Ensure out-params indicating a dupe/late ack such that the packet being acked is not known.
    *dupe_or_late = true;
    assert(*acked_pkt_it == snd_flying_pkts_by_when.past_oldest()); // A/k/a end().
    return true;
  } // if (seq_num is not in snd_flying_pkts*) // i.e., duplicate/late acknowledgment with unknown acked packet.
  // else if (seq_num IS in snd_flying_pkts*): *acked_pkt_it points to snd_flying_pkts_by_when[seq_num].
  assert(*acked_pkt_it != snd_flying_pkts_by_when.past_oldest());

  // It's an ack of sequence number we'd sent, but if retransmission is on it may not be of the one we LAST sent.

  const Peer_socket::Sent_packet& acked_pkt = *((*acked_pkt_it)->second);
  const unsigned int acked_rexmit_id = rexmit_on ? acked_pkt.m_packet->m_rexmit_id : 0;
  Sequence_number seq_num_end; // Get sequence number just past last datum in packet.
  get_seq_num_range(*acked_pkt_it, 0, &seq_num_end);

  // Note that both rexmit_id and acked_rexmit_id are guaranteed 0 at this point if !rexmit_on.

  if (rexmit_id > acked_rexmit_id)
  {
    // This is entirely illegal.  Can't acknowledge a packet copy we hadn't sent yet.
    FLOW_LOG_WARNING("NetFlow worker thread working on [" << sock << "].  "
                     "Acknowledged packet [" << seq_num << ", " << seq_num_end << ") "
                     "rexmit_id [" << int(rexmit_id) << "] "
                     "exceeds highest sent rexmit_id [" << int(acked_rexmit_id) << "].");
    rst_and_close_connection_immediately(socket_id, sock, error::Code::S_SEQ_NUM_ARITHMETIC_FAILURE, true);
    /* ^-- defer_delta_check == true: because the only way to get to this method is from
     * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
     * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
    return false; // Other out-params are meaningless.
  }
  // else if (rexmit_id <= acked_rexmit_id)

  if (rexmit_id != acked_rexmit_id)
  {
    assert(rexmit_id < acked_rexmit_id);

    /* This is legal: it's possible we had sent packet P, considered it Dropped, retransmitted it
     * (thus incrementing rexmit_id), and have now received a late acknowledgment of the
     * PREVIOUS attempt to send P (before retransmission).  We could actually consider this
     * entirely equivalent to simply getting the last attempt acked.  In fact I specifically kept
     * an array for m_sent_when, so that we can even compute accurate RTT.  Yet, at least for now,
     * I am going to ignore such an acknowledgment.  Reasons:
     *
     *   - The RTT may be an outlier affected by some random event; we considered it Dropped, so
     *     if those heuristics are generally sound, getting a late ack is suspicious.
     *
     *   - Suppose I do take the RTT and report to congestion control, use for SRTT computation,
     *     and remove from snd_flying_pkts*.  I've in effect recorded a loss but then also
     *     reported a successful retransmission, even though the ack is not for the retransmission
     *     but more like a correction on the original loss.  That's potentially fine, but chances
     *     are I will soon receive the ack for the latest transmission, which is what I was really
     *     expecting.  That one will now be considered a late ack and will be ignored, even though
     *     that RTT is actually probably more accurate, since chances are it arrived before the
     *     retransmission would've been considered Dropped as well.  So, basically, we're kind of
     *     trying to use the "two wrongs make a right" philosophy, which seems messy.
     *
     *   - Earlier in the method, I mentioned that if we detect P as dropped and queue it for
     *     retransmission but get P acked *before* we get a chance to retransmit, then we consider
     *     that ack as late and ignore it (and will still retransmit P).  The reasons for that are
     *     given in that comment.  However, given that we made that decision, it would seem
     *     strange to follow a different philosophy just because we did happen to get to
     *     retransmit P.  That would be inconsistent.
     *
     *   - Keeping it in perspective, it should be fairly rare that a packet we considered Dropped
     *     is acked after all.  So it is perhaps not worth the trouble to go crazy about this
     *     corner case.
     *
     * Nevertheless, a @todo would be to experimentally measure the effect of this policy and
     * decide whether it is sound.  In that case also consider the aforementioned "P is acked
     * after queued for retransmission but before retransmitted" corner case. */

    // Register one individual acknowledgment of unknown # of bytes of data (late).
    snd_stats.late_or_dupe_ack();

    FLOW_LOG_INFO("NetFlow worker thread working on [" << sock << "].  "
                  "Acknowledged packet [" << seq_num << ", " << seq_num_end << ") "
                  "order_num [" << acked_pkt.m_sent_when[rexmit_id].m_order_num << "] "
                  "rexmit_id [" << int(rexmit_id) << "] "
                  "is less than highest sent [" << int(acked_rexmit_id) << "].  Ignoring.");

    // Ensure out-params indicating a dupe/late ack of a specific known sent packet.
    *dupe_or_late = true;
    assert(*acked_pkt_it != snd_flying_pkts_by_when.past_oldest()); // A/k/a end().
    return true;
  }
  // else
  assert(rexmit_id == acked_rexmit_id);

  // Do not log this mainstream case; only the exceptions above.  RTT will probably be logged separately.

  // Register one individual acknowledgment of N bytes of data (converts from In-flight to Acknowledged).
  snd_stats.good_ack(acked_pkt.m_size);

  // Ensure out-params indicating an in-time, first ack of a specific known sent packet.
  *dupe_or_late = false;
  assert(*acked_pkt_it != snd_flying_pkts_by_when.past_oldest()); // A/k/a end().
  return true;
} // Node::categorize_individual_ack()

Fine_duration Node::compute_rtt_on_ack(Peer_socket::Sent_packet::Const_ptr flying_pkt,
                                       const Fine_time_pt& time_now,
                                       Ack_packet::Individual_ack::Const_ptr ack,
                                       const Peer_socket::Sent_packet::Sent_when** sent_when) const
{
  using boost::chrono::milliseconds;
  using boost::chrono::round;

  Fine_duration round_trip_time;

  /* This helper of handle_accumulated_acks() exists to make the latter method briefer/readable, not for code reuse
   * as of this writing.  It computes the RTT implied by the given individual ack and also returns the Sent_when
   * (which contains info on when the original packet was sent) structure as an out-param. */

  /* RTT subtleties:
   *
   * How long did the other side, upon receiving the acked packet, wait before sending this
   * containing ACK with that individual acknowledgment?  Why do we care?  For RTT.  Why do we
   * want RTT?  To measure how long it takes for a sent packet to reach the receiver (one-way trip
   * time, or OWTT).  Since mesuring OWTT is quite hard/impossible due to lack of absolute clock
   * synchronization between us and the receiver, RTT/2 is used as the next best way to get OWTT.
   * We can measure RTT by subtracting our recorded packet send time from the current time (ACK
   * receipt time).  However, the ACK delay introduced by the receiver to reduce ACK overhead has
   * nothing to do with OWTT; it just (randomly, from the other side's point of view) inflates the RTT.
   * Thus we subtract the ACK delay from the RTT to get the actual RTT we use for congestion control, etc. */

  const unsigned int rexmit_id = ack->m_rexmit_id;
  // Get the RTT for the transmission attempt that is actually being acknowledged (always 0 if retransmission off).
  *sent_when = &(flying_pkt->m_sent_when[rexmit_id]);
  const Peer_socket::order_num_t order_num = (*sent_when)->m_order_num;

  /* ?second-resolution value (ack_delay) subtracted from max-resolution values.  If ack_delay is
   * also in the max-resolution time unit, then there is no loss of precision.  Otherwise we lose
   * precision by subtracting a number with fewer significant digits from one with more
   * significant digits.  So Ack_delay_time_unit should ideally be Fine_duration, for precise RTT
   * values (especially for queueing delay-based congestion control algorithms); however that
   * decision is discussed elsewhere (Low_lvl_packet). */
  const auto& ack_delay = ack->m_delay;
  round_trip_time = time_now - (*sent_when)->m_sent_time - ack_delay;

  if (round_trip_time.count() < 0)
  {
    /* Because this combines measurements on both sides, and each may have some error (plus or
     * minus a few hundred microseconds, possibly), and the result can be quite close to zero in
     * extremely low-latency situations, this may come out to be negative.  So assume zero and
     * log a TRACE message at most.
     *
     * @todo Should we put also a ceiling on the RTT?
     * @todo For the floor, maybe it's better to use a higher guess than zero? */
    FLOW_LOG_TRACE("Acknowledged packet [" << ack->m_seq_num << ", ...) "
                   "order_num [" << order_num << "] has negative "
                   "RTT [" << round_trip_time << "]; assuming zero.  "
                   "Sent at [" << (*sent_when)->m_sent_time << "]; "
                   "received at [" << time_now << "]; "
                   "receiver-reported ACK delay [" << ack_delay << "].");
    round_trip_time = Fine_duration::zero();
  }
  FLOW_LOG_TRACE("Acknowledged packet [" << ack->m_seq_num << ", ...) "
                 "order_num [" << order_num << "] "
                 "has RTT [" << round<milliseconds>(round_trip_time) << "] "
                 "(ACK delay [" << round<milliseconds>(ack_delay) << "]).");

  return round_trip_time;
} // Node::compute_rtt_on_ack()

Peer_socket::Sent_pkt_ordered_by_when_iter
  Node::categorize_pkts_as_dropped_on_acks(Peer_socket::Ptr sock,
                                           const boost::unordered_set<Peer_socket::order_num_t>& flying_now_acked_pkts)
{
  using std::priority_queue;

  /* This helper of handle_accumulated_acks() exists to make the latter method briefer/readable, not for code reuse
   * as of this writing. The background is that once a set of individual acks has been processed in the sense that
   * sock->m_snd_flying_pkts* (which tracks In-flight outbound DATA packets) has been updated by removing the
   * acked packets (they are no longer In-flight), it's time to also recategorize certain further In-flight
   * packets as Dropped -- the intuition being that once N packets sent LATER than a given packet P have been
   * acked, it's highly probable that P has been Dropped by the network. This method determines the packets to drop
   * in that fashion.
   *
   * Now, as explained below, when ack set S causes packet set P' to be Dropped, this (possibly null) set P'
   * always has the following form: there is some particular packet P which is the most-recently-sent one
   * that is in P'; and therefore ALL other In-flight packets sent before P must be dropped too and also are in P'.
   * Thus P is necessary/sufficient to specify P'.  Thus this method simply finds and returns a thing pointing to P. */

  // For brevity and a little speed:
  auto& snd_flying_pkts_by_when = sock->m_snd_flying_pkts_by_sent_when;

  /* OK, snd_flying_pkts* has been updated, in that we've removed any Sent_packet entries
   * corresponding to valid acknolwedgments in this ACK.  As promised elsewhere we should also update
   * the remaining Sent_packets' m_acks_after_me entries and erase any Sent_packets that we consider
   * Dropped due to too high m_acks_after_me values.  (As in TCP Fast Retransmit/Recovery, an
   * unacknowledged packet is considered Dropped based on the heuristic that a few packets with
   * higher sequence numbers have been acknowledged.  Except since we have Sent_when, that should be
   * even better than using sequence number ordering as TCP would.)
   *
   * (Warning: below and nearby, I make pseudo-code-y leaps, such as saying flying_now_acked_pkts stores
   * Sent_whens when really it stores order_nums; just bear with me by trusting that it makes the logic
   * easier to explain, and that the actual structure in code is sufficiently
   * similar to the wording here to not make a salient difference in practice.)
   *
   * Consider the two structures we have now.  snd_flying_pkts_by_when (call
   * it F) is a collection of Sent_packets, each with Sent_packet::m_acks_after_me, ordered by decreasing
   * Sent_when.  flying_now_acked_pkts (call it C) is an unordered collection that contains each Sent_when
   * (i.e., reference to a send-packet attempt) that has been ACKed.  That is, flying_now_acked_pkts tells us
   * by Sent_when which exact send attempts from the past are acknowledged in this set of accumulated acks.
   *
   * Even less formally -- just for sanity's sake -- F are In-flight packets; C are just-acked packets that were
   * very recently in F.  C may be interleaved among F if viewed in increasing Sent_when order:
   * e.g., [ F F F F C F C F C C ] (where F represents a still-In-flight send attempt, or an F element;
   * C a just-acked send attempt, thus a C element; and the order is from earlier/lower Sent_when to
   * later/higher Sent_when).
   *
   * Note that, conceptually, the key sets (Sent_when values) in F and C are disjoint,
   * since each send attempt has a unique Sent_when value (because it at least consists of a unique m_order_num).
   * How do we correctly yet efficiently increment m_acks_after_me (call it A) for each
   * element in F to represent the new ackage?  First observe that if F[t].A is incremented by N, then
   * F[prev(t)].A should be incremented by N PLUS the number of acks for all packets sent at times in range
   * (prev(t), t), where prev(t) is the element of C with the next lower (ealier) Sent_when.
   * Consider the example scoreboard above, [ F F F F C F# C F* C C ].  F*.A is incremented by 2, because
   * plainly there are two Cs after it.  Therefore, the preceding F, which is F#,
   * is also incremented by 2; plus another 1, because there is another C (recall, simply another acknowledgment)
   * between F# and F*.  And so it goes for all the Fs.  Side/sanity note: The range is (prev[t], t), not
   * [prev(t), t), simply because F and C are disjoint; and prev(t) by definition is in F (hence not in C, hence
   * no ack for that seq. #).
   *
   * This suggests a simple inductive algorithm, wherein the latest F element's F[t].A is incremented by I, which
   * is the count of C elements with Sent_when > t; memorize I; now for each progressively older F[t],
   * count C elements in (t, next(t)) and increment F[T].A by I += <that count>.  Repeat until all Fs incremented.
   * Ultimately I = # of new valid, acknowledgments.  (Recall: scoreboard cannot begin with any Cs, [C C ... ], as
   * such a C would be acking a non-In-flight send attempt, so a dupe, and we specifically eliminate dupes from
   * consideration before inserting into C.)  So that's O(F.size()) increment operations.
   *
   * OK, but how do we get this "count of acks between t and next(t)"?  Let t be the last element of
   * F.  For it, that count is the count of all keys > t in C (i.e., the total # of acks for all
   * packets sent after t).  Let the lowest such key (Sent_when value) be `s`. Now let t' = prev(t) as before.
   * For t', the count of acks sent in (t', t) is the count of all elements in C with keys
   * in (s', s), where s' is again the lowest key > t'. Having counted that, set s = s, and repeat for each key t' of F.
   *
   * Of course, for that to be practical, C would need to be sorted by Sent_when.  Since in reality it's not sorted,
   * we could first sort it in O(k log k) operations, worst-case, k = C.size().  More convenient, however, is to
   * construct a priority queue (heap) from C; then keep popping the Sent_whens down to and
   * including s at each step.  That's O(k) to make the heap and O(k log k) total time spent
   * popping it.
   *
   * The above explanation strikes me as somewhat cryptic, but hopefully the code will clarify it; I was just
   * trying to explain why the code works. */

  /* Make heap out of flying_now_acked_pkts; top()/pop() will return the element with the highest (latest) Sent_when.
   * Just store the Sent_when values directly in the heap; std::pair::operator<() will do
   * the right thing since no element's Sent_when equals another element's Sent_when (they were
   * stored in a uniquely-keyed dictionary in the first place).
   *
   * Let cur_sent_pkt be the element of snd_flying_pkts_by_sent_when we're currently
   * considering, and it starts at F.newest() and progresses accordingly through F.
   * Then, invariant: high_ack_count_q contains the acks for all send attempts P where
   * P.m_sent_when < cur_sent_pkt.m_sent_when. In particular, P.m_sent_when.top < cur_sent_pkt.m_sent_when. */
  priority_queue<Peer_socket::order_num_t>
    high_ack_count_q(flying_now_acked_pkts.begin(), flying_now_acked_pkts.end());

  // Invariant: this will be the m_acks_after_me increment applied to the just-considered packet in snd_flying_pkts*.
  using ack_count_t = Peer_socket::Sent_packet::ack_count_t;
  ack_count_t ack_increment_after_me = 0;

  // As explained above, start with the first (latest send time) unacked packet and go forward (earlier and earlier).
  Peer_socket::Sent_pkt_ordered_by_when_iter last_dropped_pkt_it;
  for (last_dropped_pkt_it = snd_flying_pkts_by_when.newest();
       last_dropped_pkt_it != snd_flying_pkts_by_when.past_oldest();
       ++last_dropped_pkt_it) // Up to k repetitions.
  {
    Peer_socket::Sent_packet& cur_sent_pkt = *(last_dropped_pkt_it->second);
    const Peer_socket::Sent_packet::Sent_when& cur_pkt_sent_when = cur_sent_pkt.m_sent_when.back();

    /* We will increment cur_sent_pkt.m_acks_after_me by ack_increment_after_me + X, where X is
     * the total number of acks for packets with send times between cur_pkt_sent_when and the
     * cur_pkt_sent_when in the last loop iteration (or infinity if this is the first loop
     * iteration).  The high_ack_count_q invariant we maintain is that high_ack_count_q holds the
     * ack counts for all packets with Sent_when values EXCEPT those >= the previous
     * iteration's cur_pkt_sent_when.  Therefore, we need only find all elements of high_ack_count_q
     * whose Sent_whens are > our cur_pkt_sent_when.  Since high_ack_count_q.top() is always the ack
     * count with the highest sent_when in that structure (priority queue), we just pop and sum
     * until high_ack_count_q.top() < cur_pkt_sent_when. */

    // We've just assigned cur_sent_pkt, breaking invariant; pop until it holds again.
    while ((!high_ack_count_q.empty()) &&
           // Compare order numbers -- they are always unique.
           (high_ack_count_q.top() > cur_pkt_sent_when.m_order_num))
    {
      // Found acked packet with sent_when > cur_pkt_sent_when (but < previous iteration's cur_pkt_sent_when).
      ++ack_increment_after_me; // So add that packet's ack.

      // And remove it, bringing the next highest entry to the top.  O(log k).
      high_ack_count_q.pop(); // Note this maintains the invariant that defines high_ack_count_q.
    }
    // Note we've maintained the invariant defining ack_increment_after_me.

    // Hence this many more acks for packets after us have occurred within this ack set.
    cur_sent_pkt.m_acks_after_me += ack_increment_after_me;

    if (cur_sent_pkt.m_acks_after_me > S_MAX_LATER_ACKS_BEFORE_CONSIDERING_DROPPED)
    {
      /* Ah ha!  For this packet we've exceeded the limit -- we will consider it Dropped.  What
       * about the next (meaning, earlier-sent) unacknowledged packets?  Observe that packets with
       * earlier send times MUST (if we were to continue the loop in this manner) end up with
       * equal or larger cur_sent_pkt.m_acks_after_me.  (Intuitively: any acknowledgment after
       * packet P is also after any packet preceding P in the sent_when ordering.)  Therefore, we
       * can break out of the loop and consider Dropped ALL packets from last_dropped_pkt_it to
       * snd_flying_pkts_by_when.past_oldest().  Yay! */

      auto const logger_ptr = get_logger();
      if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
      {
        Sequence_number cur_pkt_seq_num, cur_pkt_seq_num_end;
        get_seq_num_range(last_dropped_pkt_it, &cur_pkt_seq_num, &cur_pkt_seq_num_end);

        FLOW_LOG_TRACE_WITHOUT_CHECKING
          ("Unacknowledged packet [" << cur_pkt_seq_num << ", " << cur_pkt_seq_num_end << ") "
           "order_num [" << cur_pkt_sent_when.m_order_num << "] has "
           "had [" << cur_sent_pkt.m_acks_after_me << "] acknowledgments "
           "for later packets; considering it and "
           "all unacknowledged packets sent earlier as Dropped.");
      }

      break;
    }
    // else

    // ack_increment_after_me and high_ack_count_q invariants hold, so the next iteration can proceed.
  } // for (all elements in snd_flying_pkts_by_when, in decreasing m_sent_when order: newest -> oldest)

  return last_dropped_pkt_it;
} // Node::categorize_pkts_as_dropped_on_acks()

bool Node::drop_pkts_on_acks(Peer_socket::Ptr sock,
                             const Peer_socket::Sent_pkt_ordered_by_when_iter& last_dropped_pkt_it,
                             size_t* cong_ctl_dropped_pkts, size_t* cong_ctl_dropped_bytes,
                             size_t* dropped_pkts, size_t* dropped_bytes,
                             std::vector<Peer_socket::order_num_t>* pkts_marked_to_drop)
{
  // using boost::next; // Still ambiguous for some reason (in clang at least).

  /* This helper of handle_accumulated_acks() exists to make the latter method briefer/readable, not for code reuse
   * as of this writing. The background is that once a set of individual acks has been processed in the sense that
   * sock->m_snd_flying_pkts* (which tracks In-flight outbound DATA packets) has been updated by removing the
   * acked packets (they are no longer In-flight), it's time to also recategorize certain further In-flight
   * packets as Dropped -- the intuition being that once N packets sent LATER than a given packet P have been
   * acked, it's highly probable that P has been Dropped by the network. This method does that (dropping
   * all such packets P) and certain related tasks such as tracking the associated loss event(s) for congestion
   * control.
   *
   * Now, as explained elsewhere, when ack set S causes packet set P' to be Dropped, this (possibly null) set P'
   * always has the following form: there is some particular packet P which is the most-recently-sent one
   * that is in P'; and therefore ALL other In-flight packets sent before P must be dropped too and also are in P'.
   * Thus P is necessary/sufficient to specify P'.  last_droppped_pkt_it argument points to P' and is determined
   * elsewhere and used by this helper. */

  // For brevity and a little speed:
  const bool rexmit_on = sock->rexmit_on();
  auto& snd_flying_pkts_by_when = sock->m_snd_flying_pkts_by_sent_when;
  auto& snd_stats = sock->m_snd_stats;

  /* Pre-condition: all elements starting with (inclusive) last_dropped_pkt_it (within
   * snd_flying_pkts_by_when) should be considered Dropped.  If last_dropped_pkt_it ==
   * snd_flying_pkts_by_when.past_oldest() a/k/a end(), then none should be considered
   * Dropped (i.e., no m_acks_after_me became high enough).
   *
   * Given that, we have a number of tasks remaining:
   *
   *   1. Count the total # of packets and bytes now considered Dropped and pass this to congestion control.
   *      Omit those packets/bytes heuristically determined to belong to a loss event detected in an earlier
   *      call, namely those for which m_sent_when < m_snd_last_loss_event_when.
   *   2. (If retransmission is enabled) Queue those Dropped packets for retransmission in retransmission queue.
   *   3. Erase the Dropped packets from snd_flying_packets*.
   *
   * For (non-asymptotic) performance, ideally we want to traverse snd_flying_pkts_by_when just once,
   * computing what's needed for these. Drilling down a bit:
   *
   * (2) and (3) are simple and involve walking over the Dropped range that has been computed (pre-condition above)
   * and adding-elsewhere or erasing those elements, respectively, though (2) must be done in chronological order
   * (increasing Sent_when).
   *
   * (1) is a matter of walking in anti-chronological (decreasing Sent_when) order over that same range, until
   * a certain Sent_when threshold is found, and stopping there.
   *
   * Thus, the kitchen-sink algorithm emerges: walk through Dropped range in decreasing Sent_when order, so
   * from last_dropped_pkt_it along snd_flying_pkts_by_when.  Accumulate bytes/packets for (1), but stop
   * accumulating once m_snd_last_loss_event_when is reached w/r/t m_sent_when.  Erase from snd_flying_pkts*
   * (carefully, since we are walking along one of them), for (3).  And add to the retransmission queue, but in
   * reverse order versus the walking order, for (2). */

  *dropped_pkts = snd_flying_pkts_by_when.size(); // We will just compute the final value by subtracting "after."
  *dropped_bytes = sock->m_snd_flying_bytes; // Ditto.

  *cong_ctl_dropped_bytes = 0;
  *cong_ctl_dropped_pkts = 0;
  bool loss_event_finished = false;

  /* We want to add to retransmission queue (if retransmission is on).  We also want to traverse
   * snd_flying_pkts_by_when in forward newest->oldest order (for convenience and also to efficiently compute
   * cong_ctl_dropped_*).  However we want to retransmit in reverse order (oldest->newest).  So we
   * put the packets to retransmit in the latter order into snd_rexmit_q, at the end of the latter.
   * So, if it was [ABC], and we dropped [DEF], then we want to insert to yield [ABCFED] (ABC->ABCFED).
   * list<>::insert(it, v) will insert `v` before *it and return iterator to just-inserted element.
   * So we can memorize the latter and pass it in as `it` in the next insert(), rinse, repeat.
   * In the above example: ABC->ABC(D)->ABC(E)D->ABC(F)ED. // () is inserted element.
   *                          ^     ^       ^        // ^ is "fulcrum": insertion point for insertion following next ->.
   *
   * snd_rexmit_q_fulcrum_it, the insertion point, is so named due to being the "fulcrum" between the old and
   * new parts of snd_rexmit_q.  History: Used to use a local new list<> here which would be spliced onto
   * the real queue at the end; but IMO this is more elegant (and probably a bit speedier). */
  auto& snd_rexmit_q = sock->m_snd_rexmit_q;
  decltype(sock->m_snd_rexmit_q)::iterator snd_rexmit_q_fulcrum_it = snd_rexmit_q.end();

  // We are to fill this up, so it should not have anything yet.
  assert(pkts_marked_to_drop->empty());

  auto pkt_it = last_dropped_pkt_it;
  while (pkt_it != snd_flying_pkts_by_when.past_oldest())
  {
    // We can't just ++pkt_it later on, because we are going to erase() at pkt_it soon, invalidating it.
    auto next_pkt_it = boost::next(pkt_it);
    // Now see end of loop body.

    // Accumulate stuff for passing into congestion control at the end.

    const Peer_socket::Sent_packet::Ptr sent_pkt = pkt_it->second;
    const Peer_socket::Sent_packet::Sent_when& sent_when = sent_pkt->m_sent_when.back();

    if (!loss_event_finished)
    {
      if (// This is part of a new loss event if: There has been no loss event before this...
          (sock->m_snd_last_loss_event_when != Fine_time_pt())
          // ...OR there has, but this packet was sent after that event was detected.
          && (sent_when.m_sent_time < sock->m_snd_last_loss_event_when))
      {
        /* This is the first packet encountered to be part of a previous loss event.  If
         * retransmission is off, this will also cause the loop to exit. */
        loss_event_finished = true;
      }
      else
      {
        // Only got here if this packet and all Dropped packets after it are part of a new loss event.
        *cong_ctl_dropped_bytes += sent_pkt->m_size;
        ++(*cong_ctl_dropped_pkts);
      }
    }
    // else { Already found end of new loss event, if any, so no need to keep looking for it. }

    // Add to retransmission queue if applicable.

    if (rexmit_on)
    {
      if (!ok_to_rexmit_or_close(sock, pkt_it, true)) // Ensure not too many retransmissions already.
      /* ^-- defer_delta_check == true: because the only way to get to this method is from
       * async_low_lvl_recv(), which will perform event_set_all_check_delta(false) at the end of itself,
       * before the boost.asio handler exits.  See Node::m_sock_events doc header for details. */
      {
        return false; // Already closed/logged/etc.
      }
      // else

      /* Save a ref-counted pointer (to what includes packet data) in retransmission queue.  We'll soon remove such
       * a pointer from snd_flying_pkts*, lowering the ref-count again.  In other words, we are moving the sent-packet
       * object from snd_flying_pkts* to snd_rexmit_q (Dropped -> In-flight).
       *
       * Insert at the same position each time to ultimately arrange them in the reversed order that we want. */
      snd_rexmit_q_fulcrum_it = snd_rexmit_q.insert(snd_rexmit_q_fulcrum_it, sent_pkt);
      ++sock->m_snd_rexmit_q_size;
    }

    /* Finally, we can erase it from snd_flying_pkts* and adjust snd_flying_bytes.
     * Will NOT invalidate other iterators into snd_flying_pkts_by_when.
     *
     * Also, save in pkts->pkts_marked_to_drop as advertised. */

    static_assert
      (!util::Container_traits<Peer_socket::Sent_pkt_by_sent_when_map>::S_CHANGE_INVALIDATES_ITERATORS,
       "Scoreboard must not get otherwise changed when a packet is erased.");
    pkts_marked_to_drop->push_back(sent_when.m_order_num);
    snd_flying_pkts_erase_one(sock, pkt_it);

    pkt_it = next_pkt_it;
  } // while (pkt_it != snd_flying_pkts_by_when.past_oldest())

  // Includes ALL Dropped packets (not just ones from new loss event, if any), so != cong_ctl_dropped_pkts.
  *dropped_pkts -= snd_flying_pkts_by_when.size(); // Subtract "after" from "before" to get dropped count.
  *dropped_bytes -= sock->m_snd_flying_bytes; // Similar.

  if (*cong_ctl_dropped_pkts != 0)
  {
    // Register that we've detected a NEW loss event (not the same as dropped_data() -- see that elsewhere).
    snd_stats.loss_event();
  }

  return true;
} // Node::drop_pkts_on_acks()

void Node::log_accumulated_acks(Peer_socket::Const_ptr sock) const
{
  using boost::algorithm::join;
  using boost::chrono::symbol_format;
  using std::string;
  using std::vector;
  using std::transform;
  using std::ostream;

  // We are in thread W.

  // This helper of handle_accumulated_acks() just logs the individual acks about to be processed.

  // For brevity and a little speed:
  using Ack = Ack_packet::Individual_ack;
  using Acks = vector<Ack::Ptr>;
  const Acks& acked_packets = sock->m_rcv_acked_packets;

  auto const logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_DATA, get_log_component())) // Very verbose and slow!
  {
    // Prepare serialization of m_rcv_acked_packets for TRACE logging; quite verbose and slow!
    vector<string> ack_strs(acked_packets.size());
    transform(acked_packets.begin(), acked_packets.end(), ack_strs.begin(),
              [](Ack::Const_ptr ack) -> string
    {
      return util::ostream_op_string('[', ack->m_seq_num, ", ", int(ack->m_rexmit_id), ", ",
                                     symbol_format,
                                     ack->m_delay, ']'); // "ns," not "nanoseconds."
    });
    const string ack_str = join(ack_strs, " ");

    FLOW_LOG_DATA_WITHOUT_CHECKING("NetFlow worker thread working on [" << sock << "].  "
                                   "Accumulated [ACK] packets with "
                                   "acknowledgments [seq_num, rexmit_id, delay]: "
                                   "[" << ack_str << "].");
  } // if (DATA)
  else
  {
    FLOW_LOG_TRACE("NetFlow worker thread working on [" << sock << "].  "
                   "Accumulated [ACK] packets with "
                   "[" << acked_packets.size() << "] individual acknowledgments.");
  }

  if (sock->m_int_state == Peer_socket::Int_state::S_ESTABLISHED)
  {
    log_snd_window(sock);
  }
  // else { Why is this possible?  Answer: See handle_accumulated_acks() for explanation near similar check. }
} // Node::log_accumulated_acks()

void Node::drop_timer_action(Peer_socket::Ptr sock, bool drop_all_packets)
{
  using std::list;
  using boost::prior;

  // We are in thread W.

  // Since we call m_snd_drop_timer->done() when exiting ESTABLISHED, this should hold.
  assert(sock->m_int_state == Peer_socket::Int_state::S_ESTABLISHED);

  // For brevity and a bit of speed:
  auto& snd_flying_pkts_by_when = sock->m_snd_flying_pkts_by_sent_when;
  auto& snd_flying_pkts_by_seq = sock->m_snd_flying_pkts_by_seq_num;

  // Timer must not be running if there are no In-flight packets.  Thus it should not have fired.
  assert(!snd_flying_pkts_by_when.empty());

  /* Drop Timer fired and is telling us to consider Dropped some packets.  If drop_all_packets, then
   * it's all of them.  Otherwise it's just the earliest unacknowledged packet
   * (m_snd_flying_pkts_by_sent_when.begin()). */

  // Log details of the In-flight packets before we change things.
  log_snd_window(sock);

  const bool rexmit_on = sock->rexmit_on();
  // To check, at the end, whether we've changed can_send() false => true.
  const bool could_send_before_drops = can_send(sock);
  // To check, at the end, whether we've changed snd_deqable() false => true.
  const bool had_rexmit_data_before_drops = !sock->m_snd_rexmit_q.empty();
  // Will store ID of the one packet to drop; reserved value 0 will mean ALL packets are dropped.
  Peer_socket::order_num_t packet_marked_to_drop_or_drop_all;

  // Used below for congestion control.
  size_t cong_ctl_dropped_bytes = 0;
  size_t cong_ctl_dropped_pkts = 0;

  if (drop_all_packets)
  {
    cong_ctl_dropped_bytes = sock->m_snd_flying_bytes;
    cong_ctl_dropped_pkts = snd_flying_pkts_by_when.size();

    // Queue them for retransmission, to be sent as soon as CWND provides enough space (could even be immediately).
    if (rexmit_on)
    {
      // Order is from earliest-sent to latest-sent (retransmission in the same order as transmission).
      for (Peer_socket::Sent_pkt_by_sent_when_map::Reverse_iterator pkt_it = snd_flying_pkts_by_when.oldest();
           pkt_it != snd_flying_pkts_by_when.past_newest();
           ++pkt_it)
      {
        // The forward iterator F pointing to same list element as reverse iterator R is prior(R.base()). Google it.
        if (!ok_to_rexmit_or_close(sock, prior(pkt_it.base()), false)) // Ensure not too many retransmissions already.
        /* ^-- defer_delta_check == false: because we were invoked from a timer event.  Therefore, we will NOT perform
         * event_set_all_check_delta(false) before the boost.asio handler exits.  Therefore boost.asio
         * may sleep (block) before event_set_all_check_delta(false).  Therefore that would delay
         * delivery of the event to the user.  Therefore force the delta check immediately.  See
         * Node::m_sock_events doc header for details. */
        {
          return; // Already closed/logged/etc.
        }
        // else

        sock->m_snd_rexmit_q.push_back(pkt_it->second); // Only a ref-counted pointer copy (constant time).
      }
      sock->m_snd_rexmit_q_size += cong_ctl_dropped_pkts;
    }
    // else { Just drop it. }

    // Update our image of the pipe.  For efficiency we use clear() instead of doing it one-by-one above.

    // Update byte count.
    snd_flying_pkts_updated(sock, snd_flying_pkts_by_when.newest(), snd_flying_pkts_by_when.past_oldest(), false);
    snd_flying_pkts_by_when.clear();
    snd_flying_pkts_by_seq.clear();

    packet_marked_to_drop_or_drop_all = 0; // Means drop all.
  }
  else
  {
    // Get the packet that was sent before all the others.
    const Peer_socket::Sent_pkt_ordered_by_when_iter& oldest_pkt_it = prior(snd_flying_pkts_by_when.past_oldest());
    Peer_socket::Sent_packet::Ptr oldest_pkt = oldest_pkt_it->second;

    cong_ctl_dropped_bytes = oldest_pkt->m_size;
    cong_ctl_dropped_pkts = 1;

    // Queue it for retransmission, to be sent as soon as CWND provides enough space (could even be immediately).
    if (rexmit_on)
    {
      if (!ok_to_rexmit_or_close(sock, oldest_pkt_it, false)) // Ensure not too many retransmissions already.
      // ^-- false <= Same as comment above.
      {
        return; // Already closed/logged/etc.
      }
      // else

      sock->m_snd_rexmit_q.push_back(oldest_pkt); // Only a ref-counted pointer copy (constant time).
      ++sock->m_snd_rexmit_q_size;
    }
    // else { Just drop it. }

    // Remember it short-term for the Drop_timer consolidated book-keeping below...
    packet_marked_to_drop_or_drop_all = oldest_pkt->m_sent_when.back().m_order_num;

    // ...and in fact mark that packet Dropped (update our image of the pipe).
    snd_flying_pkts_erase_one(sock, oldest_pkt_it);
  }

  /* Deal with congestion control.  For introduction to the general topic see the large comment
   * near the top of handle_accumulated_acks().
   *
   * Since a Drop Timeout implies a large loss event, the congestion control module must be
   * informed.  It may adjust the congestion window (used in can_send() and controlling how many
   * packets we are allowed to have In-flight at a time), probably downward.
   *
   * Also, this is a new loss event.  Why?  (For detailed explanation of what a loss event is, and
   * how we keep track of them, see that large comment in handle_accumulated_acks().  It
   * may be required to understand the rest of this paragraph.)  Certainly this Drop is part of some
   * loss event by definition, but is it a new loss event, or merely the previous one (if such
   * exists)?  Well, a Drop Timeout is, in practice, at least 1 second (which is likely 4 times a
   * pretty large RTT of 250 msec) and can also be estimated to be 3 * SRTT.  In other words it is
   * probably much larger than SRTT, and certainly is at least a little larger than SRTT.  Therefore
   * most likely any packet(s) Dropped by this DTO were sent after the last loss event (if any) was
   * detected.  Hence this DTO event is a new loss event.  We could explicitly check for this, but
   * it seems unnecessarily complex and intuitively unnecessary.
   *
   * Per handle_accumulated_acks(), when a new loss event is seen, m_snd_last_loss_event_when
   * is set to NOW. */

  // @todo Arguable if it should be INFO or TRACE.  We'll see.
  FLOW_LOG_INFO("cong_ctl [" << sock << "] update: Drop Timeout event: "
                "Dropped [" << cong_ctl_dropped_bytes << "] bytes = [" << cong_ctl_dropped_pkts << "] packets.");

  // MUST call this after, not before, updating m_snd_flying_{packets|bytes} per method doc.
  sock->m_snd_cong_ctl->on_drop_timeout(cong_ctl_dropped_bytes, cong_ctl_dropped_pkts);
  sock->m_snd_last_loss_event_when = Fine_clock::now();

  // Register that there was a timeout, and that bytes were converted from In-flight to Dropped.
  sock->m_snd_stats.drop_timeout();
  sock->m_snd_stats.dropped_data(cong_ctl_dropped_bytes, cong_ctl_dropped_pkts);

  // Now log the "after."
  log_snd_window(sock);

  // Since we've changed snd_flying_pkts*, Drop_timer events have occurred. Cleanly handle them all in one go.

  const Drop_timer::Ptr drop_timer = sock->m_snd_drop_timer;
  drop_timer->start_contemporaneous_events();

  /* Handle possible effect of above activities on the Drop Timer.  (It may get disabled or restarted anew.)
   * Why not just do this right when we erase the associated packets from snd_flying_pkts*? We don't want to
   * trigger disruptive behavior like possibly retransmitting everything in the middle of all that accounting
   * which is not yet complete.  Now it's complete, so it's the right time to handle this.
   *
   * Recall that snd_flying_pkts* have been updated and no longer contain the associated packet(s)'s info. */
  if (packet_marked_to_drop_or_drop_all == 0)
  {
    // Note that this is equivalent to calling ...packet_no_longer_in_flight(P) for all P -- just faster.
    drop_timer->on_no_packets_in_flight_any_longer();
  }
  else // if (packet_marked_to_drop_or_drop_all refers to, in fact, a specific packet)
  {
    drop_timer->on_packet_no_longer_in_flight(packet_marked_to_drop_or_drop_all);
    /* Could also call on_no_packets_in_flight_any_longer() if now none is In-flight, but performance-wise that'd
     * be the same; ...packet_no_longer_in_flight() will check the same condition anyway. So don't bother. */
  }

  drop_timer->end_contemporaneous_events();

  /* We've definitely reduced the number of packets we consider In-flight.  We may also have added
   * packets to retransmission queue (if retransmission is on).  Therefore can_send() may now return
   * true while at the beginning of the method it returned false; snd_deqable() may now return true
   * similarly.  So have send_worker() check and send more if possible.  See Node::send() for
   * discussion of overall strategy on this topic. */
  if ((!could_send_before_drops) || (rexmit_on && (!had_rexmit_data_before_drops)))
  {
    send_worker(sock, false);
    // ^-- defer_delta_check == false: for similar reason as in send_worker_check_state() calling send_worker().
  }
} // Node::drop_timer_action()

void Node::new_round_trip_time_sample(Peer_socket::Ptr sock, Fine_duration round_trip_time)
{
  using std::min;
  using std::max;
  using boost::ratio;
  using boost::ratio_subtract;
  using boost::ratio_string;
  using boost::chrono::round;
  using boost::chrono::milliseconds;
  using boost::chrono::microseconds;
  using boost::chrono::seconds;

  // We are in thread W.

  // For brevity and a bit of speed:
  Fine_duration& srtt = sock->m_snd_smoothed_round_trip_time;
  Fine_duration& rtt_var = sock->m_round_trip_time_variance;
  Fine_duration& dto = sock->m_snd_drop_timeout;
  const Fine_duration& rtt = round_trip_time;

  /* An ACK has supplied the given round_trip_time for a specific packet.  We are to update the
   * smoothed RTT for the socket which is an estimate for the smooth "current" RTT for the socket.
   * Use RFC 6298 algorithm for SRTT calculation.
   *
   * RFC 6298 specifies the formula in "seconds."  Of course it need not be seconds; it can be any
   * unit.  We leave the unit we use unspecified, except to say that we will use the unit of
   * Fine_duration, which is the duration type of Fine_clock, which is the highest-resolution clock
   * available in the OS/hardware.  Since, where possible, we keep using Fine_duration without
   * truncation to compute round_trip_time, assuming we don't introduce any unit conversions
   * (truncations, roundings) in the below code, the SRTT will maintain those units as well.
   * boost::chrono::duration will specifically cause compile failures if we don't explicitly specify
   * every truncation-inducing operation (duration_cast<>, round<>, etc.).
   *
   * BTW, this "unspecified" unit is probably nanoseconds.
   *
   * Note that the units used do NOT guarantee any particular clock granularity.  E.g., I can give
   * you the time in milliseconds, but if I always say it in multiples of 1000 milliseconds, then I
   * may be working with milliseconds, but the resolution is 1 sec. */

  if (srtt == Fine_duration::zero())
  {
    // First RTT measurement; initialize according to algorithm.
    srtt = rtt;
    rtt_var = rtt / 2;

    // Truncate results to millisecond representation for readability.
    FLOW_LOG_TRACE("First SRTT calculation for [" << sock << "]: "
                   "srtt = [" << round<milliseconds>(srtt) << " = " << srtt << "]; "
                   "rtt_var = [" << round<milliseconds>(rtt_var) << " = " << rtt_var << "]; "
                   "rtt = [" << rtt << "].");
  }
  else // if (SRTT was defined before this sample.)
  {
    // Subsequent RTT measurements.

    // @todo Per last paragraph of RFC 6298-5, we MAY want to clear srtt/rtt_var afer multiple RTOs or maybe idleness.
    // (RTO = Retransmission Timeout, though we call it a Drop Timeout more accurately [we don't necessarily
    // retransmit on loss in NetFlow, unlike TCP].)

    const Fine_duration prev_srtt = srtt;
    const Fine_duration prev_rtt_var = rtt_var;

    /* Reason I used ratio<> instead of floating point constants: I don't want to use floating
     * points in production code that much.  I don't necessarily trust it for consistent behavior across platforms...
     * and in general I just find integers more predictable/easier to reason about in most contexts of net_flow.
     * Reason I used ratio<> instead of just having separate integer constants for numerators and
     * denominators: I'd rather have ratio<> do the arithmetic for me (at compile time to boot!). */
    using Alpha = ratio<1, 8>; // 1/8, per RFC.
    using One_minus_alpha = ratio_subtract<ratio<1>, Alpha>;
    using Beta = ratio<1, 4>; // 1/4, per RFC.
    using One_minus_beta = ratio_subtract<ratio<1>, Beta>;
    // Now I can use X::num and X::den, such that X is the ratio X::num/X::den.

    // Compute |srtt - rtt|.
    Fine_duration abs_srtt_minus_rtt = srtt - rtt;
    if (abs_srtt_minus_rtt.count() < 0)
    {
      abs_srtt_minus_rtt = -abs_srtt_minus_rtt;
    }

    // Update the results per RFC.
    rtt_var
      = rtt_var * One_minus_beta::num / One_minus_beta::den
        + abs_srtt_minus_rtt * Beta::num / Beta::den;
    srtt
      = srtt * One_minus_alpha::num / One_minus_alpha::den
        + rtt * Alpha::num / Alpha::den;

    // Truncate results to millisecond representation for readability.
    FLOW_LOG_TRACE("Next SRTT calculation for [" << sock << "]: "
                   "srtt = [" << round<milliseconds>(srtt) << " = " << srtt << "]; "
                   "rtt_var = [" << round<milliseconds>(rtt_var) << " = " << rtt_var << "]; "
                   "rtt = [" << rtt << "]; "
                   "prev_srtt = [" << prev_srtt << "]; "
                   "prev_rtt_var = [" << prev_rtt_var << "]; "
                   "alpha = " << (ratio_string<Alpha, char>::prefix()) << "; "
                   "(1 - alpha) = " << (ratio_string<One_minus_alpha, char>::prefix()) << "; "
                   "beta = " << (ratio_string<Beta, char>::prefix()) << "; "
                   "(1 - beta) = " << (ratio_string<One_minus_beta, char>::prefix()) << "; "
                   "|srtt - rtt| = [" << abs_srtt_minus_rtt << "].");
  } // else if (SRTT was defined before this sample)

  /* Now compute Drop Timeout (DTO), similar to TCP's RTO (Retransmission Timeout): the minimum
   * amount of time we give an In-flight packet to get Acknowledged before considering it Dropped.
   * Again we use RFC 6298 for DTO computation.
   *
   * The formula is DTO = srtt + max(G, K * rtt_var), where K = 4 and G is the "clock
   * granularity."  Additionally, we are to put a floor of 1 second on DTO.  Finally, we are allowed
   * to put a ceiling on DTO, as long as that ceiling is at least 60 seconds.
   *
   * G plays an important part in the RTO caclulation algorithm, so we must know it.  So what is it?
   * We don't know.  We do however have a reasonably conservative upper bound; boost.timer
   * documentation lists some popular OS+CPU combinations and notes that for none of them does
   * high_resolution_timer exceed 5 microseconds.  Therefore, let us pick the exceedingly
   * conservative G = 500 microseconds = 1/2 milliseconds. */

  const Fine_duration clock_resolution_at_least = microseconds(500);
  const Fine_duration floor = seconds(1);
  const Fine_duration ceiling = sock->opt(sock->m_opts.m_dyn_drop_timeout_ceiling);
  const unsigned int k = 4;

  const Fine_duration prev_dto = dto;
  const Fine_duration rtt_var_k = rtt_var * k;
  const Fine_duration srtt_plus_var_term = srtt + max(clock_resolution_at_least, rtt_var_k);
  dto = max(srtt_plus_var_term, floor);
  dto = min(dto, ceiling);

  // Truncate results to millisecond representation for readability.
  FLOW_LOG_TRACE("Drop Timeout (DTO) calculation: "
                 "dto = [" << round<milliseconds>(dto) << " = " << dto << "]; "
                 "rtt_var * k = [" << rtt_var_k << "]; "
                 "srtt + max(G, rtt_var * k) = [" << srtt_plus_var_term << "]; "
                 "k = [" << k << "]; "
                 "floor = [" << floor << "]; ceiling = [" << ceiling << "]; "
                 "clock_resolution = [" << clock_resolution_at_least << "]; "
                 "prev_dto = [" << prev_dto << "].");
} // void Node::new_round_trip_time_sample()

void Node::log_snd_window(Peer_socket::Const_ptr sock, bool force_verbose_info_logging) const
{
  using std::vector;
  using std::list;
  using std::string;
  using boost::algorithm::join;
  using boost::prior;
  using util::String_ostream;
  using std::flush;

  // We're in thread W.

  // For brevity and a little speed:
  const auto& snd_flying_pkts_by_seq = sock->m_snd_flying_pkts_by_seq_num;
  const auto& snd_flying_pkts_by_when = sock->m_snd_flying_pkts_by_sent_when;
  const size_t num_flying_pkts = snd_flying_pkts_by_seq.size();

  // force_verbose_info_logging => log the most detail, as INFO (if INFO logging enabled).

  if (snd_flying_pkts_by_seq.empty())
  {
    // No In-flight packets, so this is brief enough for TRACE as opposed to DATA.
    FLOW_LOG_WITH_CHECKING(force_verbose_info_logging ? log::Sev::S_INFO : log::Sev::S_TRACE,
                           "Send window state for [" << sock << "]: cong_wnd "
                             "[" << sock->bytes_blocks_str(sock->m_snd_cong_ctl->congestion_window_bytes()) << "]; "
                             "sent+acked/dropped "
                             "[" << sock->m_snd_init_seq_num << ", " << sock->m_snd_next_seq_num << ") "
                             "unsent [" << sock->m_snd_next_seq_num << ", ...).");
    return;
  }
  // else

  auto const logger_ptr = get_logger();
  if (((!logger_ptr) || (!logger_ptr->should_log(log::Sev::S_DATA, get_log_component()))) &&
      (!(force_verbose_info_logging && logger_ptr->should_log(log::Sev::S_INFO, get_log_component()))))
  {
    // Can't print entire In-flight data structure, but can print a summary, if TRACE enabled.
    FLOW_LOG_TRACE
      ("Send window state for [" << sock << "]: cong_wnd "
       "[" << sock->bytes_blocks_str(sock->m_snd_cong_ctl->congestion_window_bytes()) << "]; "
       "sent+acked/dropped [" << sock->m_snd_init_seq_num << ", " << snd_flying_pkts_by_seq.begin()->first << ") "
       "in-flight [" << sock->m_snd_flying_bytes << "] bytes: " << num_flying_pkts << ":{...} "
       "unsent [" << sock->m_snd_next_seq_num << ", ...).");
    return;
  }
  // else

  // Very verbose and slow!

  const bool rexmit_on = sock->rexmit_on();

  vector<string> pkt_strs;
  pkt_strs.reserve(num_flying_pkts);
  for (Peer_socket::Sent_pkt_ordered_by_seq_const_iter pkt_it_it = snd_flying_pkts_by_seq.begin();
       pkt_it_it != snd_flying_pkts_by_seq.end();
       ++pkt_it_it)
  {
    Sequence_number start, end;
    get_seq_num_range(pkt_it_it->second, &start, &end);

    Peer_socket::Sent_packet::Const_ptr sent_pkt = pkt_it_it->second->second;

    String_ostream pkt_str_os;
    pkt_str_os.os() << '[' << start;
    if (rexmit_on)
    {
      pkt_str_os.os() << '[' << int(sent_pkt->m_packet->m_rexmit_id) << '/' << sent_pkt->m_sent_when.back().m_order_num
                      << "], ";
    }
    else
    {
      pkt_str_os.os() << ", ";
    }
    pkt_str_os.os() << end << ")<" << sent_pkt->m_acks_after_me << "acks" << flush;

    pkt_strs.push_back(pkt_str_os.str());
  }

  FLOW_LOG_WITHOUT_CHECKING
    (force_verbose_info_logging ? log::Sev::S_INFO : log::Sev::S_DATA,
     "Send window state for [" << sock << "]: cong_wnd "
       "[" << sock->bytes_blocks_str(sock->m_snd_cong_ctl->congestion_window_bytes()) << "]; "
       "sent+acked/dropped [" << sock->m_snd_init_seq_num << ", " << snd_flying_pkts_by_seq.begin()->first << ") "
       "in-flight "
       "[" << sock->m_snd_flying_bytes << "] bytes: " << num_flying_pkts << ":{" << join(pkt_strs, " ") <<
       "} unsent [" << sock->m_snd_next_seq_num << ", ...).");

  if (!rexmit_on)
  {
    return;
  }
  // else

  // Since retransmission is on, also useful to show the packets sorted by when they were sent.

  vector<string> pkt_strs_time;
  pkt_strs_time.reserve(num_flying_pkts);
  // Note I don't use `auto` only for clarity (to express it is a reverse iterator, hence why didn't use for(:)).
  for (Peer_socket::Sent_pkt_by_sent_when_map::Const_reverse_iterator pkt_it = snd_flying_pkts_by_when.const_oldest();
       pkt_it != snd_flying_pkts_by_when.const_past_newest();
       ++pkt_it)
  {
    Sequence_number start, end;
    // The forward iterator F pointing to same list element as reverse iterator R is prior(R.base()) [sic].  Google it.
    get_seq_num_range(prior(pkt_it.base()), &start, &end);

    Peer_socket::Sent_packet::Const_ptr sent_pkt = pkt_it->second;

    string pkt_str;
    util::ostream_op_to_string(&pkt_str,
                               start, '[', int(sent_pkt->m_packet->m_rexmit_id), '/',
                               sent_pkt->m_sent_when.back().m_order_num, "], ", end, ")<",
                               sent_pkt->m_acks_after_me, "acks");
    pkt_strs_time.push_back(pkt_str);
  }

  // Log it only if it is different (only possible if some retransmitted packets are actually involved).
  if (pkt_strs_time != pkt_strs)
  {
    FLOW_LOG_WITHOUT_CHECKING
      (force_verbose_info_logging ? log::Sev::S_INFO : log::Sev::S_DATA,
       "Sorted by time sent: {" << join(pkt_strs_time, " ") << "}.");
  }
} // Node::log_snd_window()

Sequence_number Node::snd_past_last_flying_datum_seq_num(Peer_socket::Const_ptr sock) // Static.
{
  using boost::prior;

  const Peer_socket::Sent_pkt_by_seq_num_map& flying_packets = sock->m_snd_flying_pkts_by_seq_num;
  if (flying_packets.empty())
  {
    return Sequence_number(); // Default value.  Less than all others.
  }
  // else

  // Get the sequence number of the first datum in the last unhandled packet.
  const Peer_socket::Sent_pkt_by_seq_num_map::value_type& highest_val = *(prior(flying_packets.end()));
  Sequence_number seq_num = highest_val.first;

  // Advance just past the data in that packet to get what we want.
  advance_seq_num(&seq_num, highest_val.second->second->m_size);

  return seq_num;
}

void Node::snd_flying_pkts_erase_one(Peer_socket::Ptr sock, Peer_socket::Sent_pkt_ordered_by_when_iter pkt_it)
{
  // using boost::next; // Still ambiguous for some reason (in clang at least).

  auto const logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    const Peer_socket::Sent_packet& sent_pkt = *pkt_it->second;
    const Peer_socket::order_num_t order_num = sent_pkt.m_sent_when.back().m_order_num;
    Sequence_number seq_num, seq_num_end;
    get_seq_num_range(pkt_it, &seq_num, &seq_num_end);

    if (sock->rexmit_on())
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING
        ("On [" << sock << "] erasing packet [" << seq_num << ", " << seq_num_end << ") "
         "order_num [" << order_num << "] rexmit_id [" << int(sent_pkt.m_packet->m_rexmit_id) << "] from "
         "snd_flying_pkts* and friends.");
    }
    else
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING
        ("On [" << sock << "] erasing packet [" << seq_num << ", " << seq_num_end << ") "
         "order_num [" << order_num << "] from snd_flying_pkts* and friends.");
    }
  }

  // Update byte count.
  snd_flying_pkts_updated(sock, pkt_it, boost::next(pkt_it), false);

  // Finally erase from main structures.
  sock->m_snd_flying_pkts_by_seq_num.erase(pkt_it->first);
  sock->m_snd_flying_pkts_by_sent_when.erase(pkt_it);

  // Note: As advertsied, we do NOT inform sock->m_snd_drop_timer.  It is up to the caller to do the right thing there.
}

void Node::snd_flying_pkts_push_one(Peer_socket::Ptr sock,
                                    const Sequence_number& seq_num,
                                    Peer_socket::Sent_packet::Ptr sent_pkt)
{
  using std::pair;
  using std::make_pair;
  // using boost::next; // Still ambiguous for some reason (in clang at least).

  // For brevity and a bit of speed:
  auto& snd_flying_pkts_by_when = sock->m_snd_flying_pkts_by_sent_when;

#ifndef NDEBUG
  const auto insert_result =
#endif
    snd_flying_pkts_by_when.insert(make_pair(seq_num, sent_pkt));

  // In this map, last added (a/k/a last sent) packet = first in the ordering!
  const Peer_socket::Sent_pkt_ordered_by_when_iter& pkt_it = snd_flying_pkts_by_when.begin();
  assert(insert_result.second); // Sequence numbers must not repeat ever.
  assert(insert_result.first == pkt_it); // Check that just-inserted element is ordered at the start.

  snd_flying_pkts_updated(sock, pkt_it, boost::next(pkt_it), true); // Update byte count.

  // Accordingly, insert packet (in the form of iterator into the above map) into sequence-number-ordered "scoreboard."
#ifndef NDEBUG
  const auto insert_result_by_seq =
#endif
    sock->m_snd_flying_pkts_by_seq_num.insert(make_pair(seq_num, pkt_it));

  // Check invariant: Key X is in ..._by_sent_when <=> key X is in ..._by_seq_num.
  assert(insert_result_by_seq.second);

  /* Caution: As noted in the doc header for this method, note that while we've already inserted sent_pkt into
   * snd_flying_pkts_by_when, the actual value of sent_pkt->m_sent_when.back() -- the absolute "when" -- isn't ready.
   * It will only be finalized once we actually send off the packet (after pacing, if any), in mark_data_packet_sent().
   * Nevertheless, we know the packet will be sent sometime fairly soon; and in fact AFTER all the packets
   * following it it in snd_flying_pkts_by_when's iterator ordering and in fact BEFORE any packets that
   * would be subsequently ahead of it in snd_flying_pkts_by_when's iterator ordering.  That is, we can
   * place it there now, despite not knowing the _absolute_ time when it be sent, because we are confident about
   * its _relative_ order of when it will be sent vs. all the other packets in that structure, past or future. */

  // Everything following this point is logging only.

  auto const logger_ptr = get_logger();
  if ((!logger_ptr) || (!logger_ptr->should_log(log::Sev::S_TRACE, get_log_component())))
  {
    return;
  }
  // else

  Sequence_number seq_num_end;
  get_seq_num_range(pkt_it, 0, &seq_num_end);
  if (sock->rexmit_on())
  {
    FLOW_LOG_TRACE_WITHOUT_CHECKING
      ("On [" << sock << "] pushing packet [" << seq_num << ", " << seq_num_end << ") "
       "rexmit_id [" << int(sent_pkt->m_packet->m_rexmit_id) << "] onto snd_flying_pkts and friends.");
  }
  else
  {
    FLOW_LOG_TRACE_WITHOUT_CHECKING
      ("On [" << sock << "] pushing packet [" << seq_num << ", " << seq_num_end << ") "
       "onto snd_flying_pkts and friends.");
  }
}

void Node::snd_flying_pkts_updated(Peer_socket::Ptr sock,
                                   Peer_socket::Sent_pkt_ordered_by_when_const_iter pkt_begin,
                                   const Peer_socket::Sent_pkt_ordered_by_when_const_iter& pkt_end,
                                   bool added)
{
  // We are in thread W.

  if (pkt_begin == pkt_end)
  {
    return; // Wouldn't do anything anyway, but return here to avoid logging.
  }

  // For brevity and a bit of speed:
  const auto& snd_flying_pkts_by_when = sock->m_snd_flying_pkts_by_sent_when;
  size_t& snd_flying_bytes = sock->m_snd_flying_bytes;

  // Optimization for when they effectively clear() snd_flying_pkts* (e.g., possibly on Drop Timeout):
  if ((!added)
      && (pkt_begin == snd_flying_pkts_by_when.const_newest())
      && (pkt_end == snd_flying_pkts_by_when.const_past_oldest()))
  {
    snd_flying_bytes = 0;
  }
  else
  {
    size_t delta_bytes = 0;
    for ( ; pkt_begin != pkt_end; ++pkt_begin)
    {
      delta_bytes += pkt_begin->second->m_size;
    }
    added ? (snd_flying_bytes += delta_bytes) : (snd_flying_bytes -= delta_bytes);
  }

  FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: "
                 "In-flight [" << sock->bytes_blocks_str(snd_flying_bytes) << "].");
}

bool Node::ok_to_rexmit_or_close(Peer_socket::Ptr sock,
                                 const Peer_socket::Sent_pkt_ordered_by_when_iter& pkt_it,
                                 bool defer_delta_check)
{
  const Peer_socket::Sent_packet& pkt = *pkt_it->second;

  Sequence_number seq_num, seq_num_end;
  get_seq_num_range(pkt_it, &seq_num, &seq_num_end);

  const unsigned int rexmit_id = pkt.m_packet->m_rexmit_id;
  FLOW_LOG_TRACE("On [" << sock << "] attempting to queue for retransmission "
                 "[" << seq_num << ", " << seq_num_end << "] which has been "
                 "retransmitted [" << rexmit_id << "] times so far.");
  if (rexmit_id == sock->opt(sock->m_opts.m_st_max_rexmissions_per_packet))
  {
    rst_and_close_connection_immediately(socket_id(sock), sock,
                                         error::Code::S_CONN_RESET_TOO_MANY_REXMITS, defer_delta_check);
    return false;
  }
  // else
  return true;
}

Peer_socket::Ptr Node::connect(const Remote_endpoint& to, Error_code* err_code,
                               const Peer_socket_options* opts)
{
  return connect_with_metadata(to, boost::asio::buffer(&S_DEFAULT_CONN_METADATA, sizeof(S_DEFAULT_CONN_METADATA)),
                               err_code, opts);
}

Peer_socket::Ptr Node::connect_with_metadata(const Remote_endpoint& to,
                                             const boost::asio::const_buffer& serialized_metadata,
                                             Error_code* err_code,
                                             const Peer_socket_options* sock_opts)
{
  namespace bind_ns = util::bind_ns;
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Peer_socket::Ptr, Node::connect_with_metadata,
                                     bind_ns::cref(to), bind_ns::cref(serialized_metadata), _1, sock_opts);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  namespace bind_ns = util::bind_ns;
  using async::asio_exec_ctx_post;
  using async::Synchronicity;
  using bind_ns::bind;

  // We are in thread U != W.

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return Peer_socket::Ptr();
  }
  // else

  // If it's good enough for DATA packets, it's good enough for metadata in SYN.
  if (serialized_metadata.size() > max_block_size())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_CONN_METADATA_TOO_LARGE);
    return Peer_socket::Ptr();
  }

  /* Put the rest of the work into thread W.  For justification, see big comment in listen().
   * Addendum regarding performance: connect() is probably called more frequently than listen(), but
   * I doubt the performance impact is serious even so.  send() and receive() might be a different
   * story. */

  Peer_socket::Ptr sock;
  /* Load this->connect_worker(...) onto thread W boost.asio work queue.
   * We don't return until it finishes; therefore it is fine to do total & capture. */
  asio_exec_ctx_post(get_logger(), &m_task_engine, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION,
                     [&]() { connect_worker(to, serialized_metadata, sock_opts, &sock); });
  // If got here, the task has completed in thread W and signaled us to that effect.

  // connect_worker() indicates success or failure through this data member.
  if (sock->m_disconnect_cause)
  {
    *err_code = sock->m_disconnect_cause;
    return Peer_socket::Ptr(); // sock will go out of scope and thus will be destroyed.
  }
  // else
  err_code->clear();
  return sock;
} // Node::connect_with_metadata()

void Node::connect_worker(const Remote_endpoint& to, const boost::asio::const_buffer& serialized_metadata,
                          const Peer_socket_options* sock_opts,
                          Peer_socket::Ptr* sock_ptr)
{
  using boost::asio::buffer;
  using boost::asio::ip::address;

  assert(sock_ptr);

  // We are in thread W.  connect() is waiting for us to set *sock_ptr and return.

  // Create new socket and set all members that may be immediately accessed by user in thread U after we're done.

  auto& sock = *sock_ptr;
  if (sock_opts)
  {
    /* They provided custom per-socket options.  Before we give those to the new socket, let's
     * validate them (for proper values and internal consistency, etc.). */

    Error_code err_code;
    const bool opts_ok = sock_validate_options(*sock_opts, 0, &err_code);

    // Due to the advertised interface of the current method, we must create a socket even on error.
    sock.reset(sock_create(*sock_opts));

    // Now report error if indeed options were invalid.  err_code is already set and logged in that case.
    if (!opts_ok)
    {
      sock->m_disconnect_cause = err_code;
      return;
    }
    // else
  }
  else
  {
    /* More typically, they did not provide per-socket options.  So we just pass our global
     * template for the per-socket options to the Peer_socket constructor.  The only caveat is
     * that template may be concurrently changed, so we must lock it.  Could do it with opt(), but
     * that introduces an extra copy of the entire struct, so just do it explicitly.
     *
     * Note: no need to validate; global options (including per-socket ones) are validated
     * elsewhere when set. */
    Peer_socket* sock_non_ptr;
    {
      Options_lock lock(m_opts_mutex);
      sock_non_ptr = sock_create(m_opts.m_dyn_sock_opts);
    }
    sock.reset(sock_non_ptr);
  }

  // Socket created; set members.

  sock->m_active_connect = true;
  sock->m_node = this;
  sock_set_state(sock, Peer_socket::State::S_OPEN, Peer_socket::Open_sub_state::S_CONNECTING);
  sock->m_remote_endpoint = to;
  // Will be sent in SYN to be deserialized by user on the other side.  Save here if we must retransmit SYN.
  sock->m_serialized_metadata.assign_copy(serialized_metadata);

  /* Initialize the connection's send bandwidth estimator (object that estimates available
   * outgoing bandwidth based on incoming acknowledgments).  It may be used by m_snd_cong_ctl,
   * depending on the strategy chosen, but may be useful in its own right.  Hence it's a separate
   * object, not inside *m_snd_cong_ctl. */
  sock->m_snd_bandwidth_estimator.reset(new Send_bandwidth_estimator(get_logger(), sock));

  // Initialize the connection's congestion control strategy based on the configured strategy.
  sock->m_snd_cong_ctl.reset
    (Congestion_control_selector::create_strategy(sock->m_opts.m_st_cong_ctl_strategy, get_logger(), sock));
  // ^-- No need to use opt() yet: user doesn't have socket and cannot set_options() on it yet.

  /* Tweak: If they specify the "any" IP address as the destination (which means any interface on
   * this machine), response traffic will look as though it's coming from the loopback IP address,
   * or another specific IP address -- not "any."  Thus it will not be able to be properly
   * demultiplexed to this socket, since that will be saved at the "any" address in our data
   * structures.  So that's an error. */
  bool ip_addr_any_error = false;
  const address& addr = to.m_udp_endpoint.address(); // Short-hand.
  if (addr.is_v4())
  {
    if (addr.to_v4() == util::Ip_address_v4::any())
    {
      ip_addr_any_error = true;
    }
  }
  else if (addr.is_v6())
  {
    if (addr.to_v6() == util::Ip_address_v6::any())
    {
      ip_addr_any_error = true;
    }
  }
  // else a new version of IP!  Yay!
  if (ip_addr_any_error)
  {
    // Mark/log error.
    Error_code* err_code = &sock->m_disconnect_cause;
    FLOW_ERROR_EMIT_ERROR(error::Code::S_CANNOT_CONNECT_TO_IP_ANY);
    return;
  }
  // else

  // Allocate ephemeral local port.

  sock->m_local_port = m_ports.reserve_ephemeral_port(&sock->m_disconnect_cause);
  if (sock->m_local_port == S_PORT_ANY)
  {
    // Error already logged and is in sock->m_disconnect_cause.
    return;
  }
  // else

  const Socket_id socket_id = Node::socket_id(sock);
  FLOW_LOG_INFO("NetFlow worker thread starting active-connect of [" << sock << "].");

  if (util::key_exists(m_socks, socket_id))
  {
    /* This is an active connect (we're intiating the connection).  Therefore in particular it
     * should be impossible that our local_port() equals an already existing connection's
     * local_port(); Port_space is supposed to prevent the same ephemeral port from being handed out
     * to more than one connection.  Therefore this must be a programming error. */

    FLOW_LOG_WARNING("Cannot add [" << sock << "], because such a connection already exists.  "
                     "This is an ephemeral port collision and "
                     "constitutes either a bug or an extremely unlikely condition.");

    // Mark/log error.
    Error_code* err_code = &sock->m_disconnect_cause;
    FLOW_ERROR_EMIT_ERROR(error::Code::S_INTERNAL_ERROR_PORT_COLLISION);

    // Return port.
    Error_code return_err_code;
    m_ports.return_port(sock->m_local_port, &return_err_code);
    assert(!return_err_code);

    return;
  } // if (that socket pair already exists)
  // else

  /* Try the packet send just below again if SYN not acknowledged within a certain amount of time.
   * Give up if that happens too many times.  Why do this BEFORE sending packet?  Because
   * this can fail, in which case we don't want a weird situation where we've sent
   * the packet but failed to start the retransmit/timeout timers.
   * Update: It can no longer fail, so that reasoning is N/A.  Not moving, though, because it's still fine here. */
  setup_connection_timers(socket_id, sock, true);

  /* Initial Sequence Number (ISN) (save before create_syn() uses it).
   * Remember it in case we must retransmit the SYN.  (m_snd_next_seq_num may have been further increased by then.) */
  Sequence_number& init_seq_num = sock->m_snd_init_seq_num;
  init_seq_num = m_seq_num_generator.generate_init_seq_num();
  /* Setting this now ensures ALL subsequent copies (essentially, every single Sequence_number on this socket's
   * local data number line!) will have the same nice metadata (hence nice logging) too.
   * The `+ 1` nuance is explained in class Sequence_number doc header, *Metadata* section. */
  init_seq_num.set_metadata('L', init_seq_num + 1, sock->max_block_size());
  // Sequence number of first bit of actual data.
  sock->m_snd_next_seq_num = init_seq_num + 1;

  // Make a SYN packet to send.
  auto syn = create_syn(sock);

  // Fill out common fields and asynchronously send packet.
  if (!async_sock_low_lvl_packet_send_paced(sock,
                                            Low_lvl_packet::ptr_cast(syn),
                                            &sock->m_disconnect_cause))
  {
    // Error marked and logged already.

    // Return port.
    Error_code return_err_code;
    m_ports.return_port(sock->m_local_port, &return_err_code);
    assert(!return_err_code);

    // Cancel any timers set up above.
    cancel_timers(sock);

    return;
  }
  /* send will happen asynchronously, and the registered completion handler will execute in this
   * thread when done (NO SOONER than this method finishes executing). */

  // No more erros: Map socket pair to the socket data structure (kind of analogous to a TCP net-stack's TCB structure).
  m_socks[socket_id] = sock;

  // CLOSED -> SYN_SENT.
  sock_set_int_state(sock, Peer_socket::Int_state::S_SYN_SENT);
} // Node::connect_worker()

Peer_socket::Ptr Node::sync_connect(const Remote_endpoint& to, Error_code* err_code,
                                    const Peer_socket_options* sock_opts)
{
  return sync_connect_with_metadata(to, Fine_duration::max(),
                                    boost::asio::buffer(&S_DEFAULT_CONN_METADATA, sizeof(S_DEFAULT_CONN_METADATA)),
                                    err_code, sock_opts);
}

Peer_socket::Ptr Node::sync_connect_with_metadata(const Remote_endpoint& to,
                                                  const boost::asio::const_buffer& serialized_metadata,
                                                  Error_code* err_code, const Peer_socket_options* opts)
{
  return sync_connect_with_metadata(to, Fine_duration::max(), serialized_metadata, err_code, opts);
}

Peer_socket::Ptr Node::sync_connect_impl(const Remote_endpoint& to, const Fine_duration& max_wait,
                                         const boost::asio::const_buffer& serialized_metadata,
                                         Error_code* err_code, const Peer_socket_options* sock_opts)
{
  namespace bind_ns = util::bind_ns;
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Peer_socket::Ptr, Node::sync_connect_impl,
                                     bind_ns::cref(to), bind_ns::cref(max_wait), bind_ns::cref(serialized_metadata),
                                     _1, sock_opts);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  using util::bind_ns::bind;

  // We are in thread U != W.

  /* This is actually pretty simple.  All we want to do is connect(), which is non-blocking, and
   * then block until the connection is ready (at least according to our side).  Ready means that
   * the socket is Writable (since user has no access to the socket yet, nothing can be loading
   * data onto the Send buffer, and obviously the congestion window is clear, so it must be
   * Writable).  Note that, like BSD sockets, we specifically don't consider a socket Writable
   * until in ESTABLISHED internal state. */

  /* For the "block until Writable" part, create and load the Event_set.  Do this before connect(),
   * so that if it fails we don't have to then clean up the socket before returning error to user. */

  const Event_set::Ptr event_set = event_set_create(err_code);
  if (!event_set)
  {
    assert(*err_code == error::Code::S_NODE_NOT_RUNNING);
    return Peer_socket::Ptr(); // *err_code is set.
  }
  // Now we know Node is running(); and we have event_set.

  // We must clean up event_set at any return point below.
  Error_code dummy_prevents_throw;
  util::Auto_cleanup event_set_cleanup = util::setup_auto_cleanup([&]()
  {
    // Eat any error when closing Event_set, as it's unlikely and not interesting to user.
    event_set->close(&dummy_prevents_throw);
  });

  const auto sock = connect_with_metadata(to, serialized_metadata, err_code, sock_opts);
  if (!sock)
  {
    return sock; // *err_code is set.  It's probably some user error like an invalid destination.
  }
  // else we have a socket that has started connecting.

  /* We must clean up sock (call sock->close_abruptly(&dummy_prevents_throw)) at any return point (including
   * exception throw) below, EXCEPT the success case.  Because of the latter, we can't use the
   * auto_cleanup trick we used on event_set.  So, we'll just have to handle sock cleanup
   * manually. */

  // Add the one event about which we care.
  bool result = event_set->add_wanted_socket<Peer_socket>(sock, Event_set::Event_type::S_PEER_SOCKET_WRITABLE,
                                                          &dummy_prevents_throw);
  assert(result); // Node is running, so there's no way that should have failed.

  // Wait for Writable.
  result = event_set->sync_wait(max_wait, err_code);
  if (!result)
  {
    if (*err_code == error::Code::S_EVENT_SET_CLOSED)
    {
      // It's unlikely, but I guess someone could have destroyed Node during the wait (we do allow that during sleep).
      *err_code = error::Code::S_NODE_NOT_RUNNING;
    }
    else
    {
      // This is quite common and is analogous to POSIX's EINTR semantics (signal interrupted the blocking call).
      assert(*err_code == error::Code::S_WAIT_INTERRUPTED);
    }

    // Clean up (as discussed above).
    sock->close_abruptly(&dummy_prevents_throw); // Eat any error; user doesn't care.
    return Peer_socket::Ptr(); // *err_code is set.
  } // if (sync_wait() failed)
  // else we know event_set is still open, and sync_wait() succeeded.

  // OK; either that returned 1 event, or 0 events (timeout).
  const bool ready = event_set->events_detected(err_code);
  /* Node had not been destroyed by the time sync_wait() finished, and we don't allow simultaneous
   * ~Node() outside a blocking sleep (see notes in class Node doc header).  The only way this
   * failed is if Event_set was closed, and that could only happen if Node was destroyed. */
  assert(!*err_code);

  if (ready)
  {
    /* Didn't time out; socket is Writable.  However, that does not mean it's Writable for "good"
     * reasons.  If an error was encountered since the original non-blocking connect (e.g., RST
     * received; or handshake timeout expired), then it is now Writable, but any operation like
     * send() or receive() will immediately yield an error.  If that is the case,
     * close_connection_immediately() has set user-visible state to S_CLOSED.  So let's check for
     * it and return an error in that case.
     *
     * We could also not; pretend socket is ready and let user discover error when trying to
     * transmit.  However it seems like a good property to help him out. */

    if (sock->state() == Peer_socket::State::S_CLOSED)
    {
      // No need to cleanup socket; it is already closed.

      // Return error as above.
      *err_code = sock->m_disconnect_cause; // No need to lock; m_disconnect_cause set and can't change later.
      return Peer_socket::Ptr();
    }
    // else it's probably really ready for action.

    return sock; // *err_code is success.
  }
  // else

  // Timed out!  Clean up socket, as above, and return null with a specific error (as advertised).
  sock->close_abruptly(&dummy_prevents_throw);
  *err_code = error::Code::S_WAIT_USER_TIMEOUT;
  return Peer_socket::Ptr();
} // Node::sync_connect_impl()

void Node::setup_connection_timers(const Socket_id& socket_id, Peer_socket::Ptr sock, bool initial)
{
  using util::schedule_task_from_now;
  using util::scheduled_task_fired;
  using boost::chrono::microseconds;
  using boost::chrono::duration_cast;
  using boost::weak_ptr;

  // We are in thread W.

  Fine_duration rexmit_from_now = sock->opt(sock->m_opts.m_st_connect_retransmit_period);

  // Finalize the retransmit scheduled task firing time; and update the # retries statistic.
  if (!initial)
  {
    assert(scheduled_task_fired(get_logger(), sock->m_init_rexmit_scheduled_task));

    ++sock->m_init_rexmit_count;
    /* This is a bit more precise than leaving rexmit_from_now alone, as it counts from when firing was
     * actually scheduled, vs. when the timer was actually triggered by boost.asio.  The 2nd addend should be a bit
     * negative and thus decrease rexmit_from_now a bit. */
    rexmit_from_now += scheduled_task_fires_from_now_or_canceled(get_logger(), sock->m_init_rexmit_scheduled_task);
    /* @todo RFC 6298 mandates that this must be doubled after each attempt instead of keeping
     * the same value.  Doesn't mean we should follow it. */
  }

  // Firing time is set; start timer.  Call that body when task fires, unless it is first canceled.
  sock->m_init_rexmit_scheduled_task
    = schedule_task_from_now(get_logger(), rexmit_from_now, true, &m_task_engine,
                             [this, socket_id,
                              sock_observer = weak_ptr<Peer_socket>(sock)]
                               (bool)
  {
    auto sock = sock_observer.lock();
    if (sock)
    {
      handle_connection_rexmit_timer_event(socket_id, sock);
    }
    // else { Possible or not, allow for this possibility for maintainability. }
  });

  // Also set up the timeout that will stop these retries from happening.
  if (initial)
  {
    sock->m_connection_timeout_scheduled_task
      = schedule_task_from_now(get_logger(),
                               sock->opt(sock->m_opts.m_st_connect_retransmit_timeout),
                               true, &m_task_engine,
                               [this, socket_id,
                                sock_observer = weak_ptr<Peer_socket>(sock)]
                                 (bool)
   {
      // We are in thread W.

      auto sock = sock_observer.lock();
      if (!sock)
      {
        return; // Possible or not, allow for this possibility for maintainability.
      }
      // else

      FLOW_LOG_INFO("Connection handshake timeout timer [" << sock << "] has been triggered; was on "
                    "attempt [" << (sock->m_init_rexmit_count + 1) << "].");

      assert((sock->m_int_state == Peer_socket::Int_state::S_SYN_SENT)
             || (sock->m_int_state != Peer_socket::Int_state::S_SYN_RCVD));

      // Timeout.  Give up.  Send RST, in case they do come to their senses -- but it's too late for us.

      /* Close connection in our structures and inform user.  Pre-conditions
       * assumed by call: sock in m_socks and sock->state() == S_OPEN (yes, since m_int_state ==
       * S_SYN_SENT/RCVD); err_code contains the reason for the close (yes). */
      rst_and_close_connection_immediately(socket_id, sock, error::Code::S_CONN_TIMEOUT, false);
      /* ^-- defer_delta_check == false: for similar reason as when calling send_worker() from
       * send_worker_check_state(). */
    });
  } // if (initial)
} // Node::setup_connection_timers()

void Node::handle_connection_rexmit_timer_event(const Socket_id& socket_id, Peer_socket::Ptr sock)
{
  using util::Blob;

  // We are in thread W.

  assert((sock->m_int_state == Peer_socket::Int_state::S_SYN_SENT)
         || (sock->m_int_state != Peer_socket::Int_state::S_SYN_RCVD));

  // Not an error (so not WARNING), but it's rare and interesting enough for INFO.
  FLOW_LOG_INFO("Connection handshake retransmit timer [" << sock << "] triggered; was on "
                "attempt [" << (sock->m_init_rexmit_count + 1) << "].");

  // Try again.  Reproduce the SYN or SYN_ACK... but first set up the next timer.

  // Setup the next timer before sending packet for the same reason as in the original SYN/SYN_ACK-sending code.
  setup_connection_timers(socket_id, sock, false);

  /* Send packet.
   * @todo More code reuse?  Or save the serialized version inside socket and resend here verbatim? */

  Low_lvl_packet::Ptr re_syn_base;
  if (sock->m_active_connect)
  {
    auto syn = create_syn(sock);
    re_syn_base = Low_lvl_packet::ptr_cast(syn);
  }
  else
  {
    // (Subtlety: As of this writing it wouldn't have changed since original SYN_ACK, but safe>sorry.)
    sock->m_rcv_last_sent_rcv_wnd = sock_rcv_wnd(sock);

    auto syn_ack = create_syn_ack(sock);
    re_syn_base = Low_lvl_packet::ptr_cast(syn_ack);
  }

  // Fill out common fields and asynchronously send packet.
  if (!async_sock_low_lvl_packet_send_or_close_immediately(sock, std::move(re_syn_base), false))
  {
    /* ^-- defer_delta_check == false: for similar reason as when calling send_worker() from
     * send_worker_check_state(). */
    return;
  }
} // Node::handle_connection_rexmit_timer_event()

void Node::cancel_timers(Peer_socket::Ptr sock)
{
  using util::scheduled_task_cancel;
  using util::Scheduled_task_handle;

  // We are in thread W.

  /* Cancel any timers.  Note that this will NOT prevent a given timer's handler from running.
   * It will try to make it run ASAP with operation_aborted error code.  However, it may not even
   * succeed in that.  In particular, if by the time the current handler started the timer handler
   * event was already queued inside m_task_engine, then canceling the timer now will not load
   * operation_aborted into the handler call; it will instead fire as if the timer really expired
   * (which it did).  Therefore the timer handler should be careful to check the state of the socket
   * and exit if the state is not suitable (in this case, S_CLOSED).
   *
   * Even so, try to cancel with operation_aborted just to cut down on entropy a bit (at least by
   * executing all handlers ASAP).
   *
   * Update: However, scheduled_task_cancel() will indeed cleanly cancel.  `Timer`s are still in direct use
   * as well however, so the above still applies to some of the below. */

  sock->m_rcv_delayed_ack_timer.cancel();
  sock->m_snd_pacing_data.m_slice_timer.cancel();

  if (sock->m_init_rexmit_scheduled_task)
  {
    scheduled_task_cancel(get_logger(), sock->m_init_rexmit_scheduled_task);
    sock->m_init_rexmit_scheduled_task = Scheduled_task_handle();
  }
  if (sock->m_connection_timeout_scheduled_task)
  {
    scheduled_task_cancel(get_logger(), sock->m_connection_timeout_scheduled_task);
    sock->m_connection_timeout_scheduled_task = Scheduled_task_handle();
  }
  if (sock->m_rcv_in_rcv_wnd_recovery)
  {
    scheduled_task_cancel(get_logger(), sock->m_rcv_wnd_recovery_scheduled_task);
    sock->m_rcv_in_rcv_wnd_recovery = false;
  }

  if (sock->m_snd_drop_timer)
  {
    // This Drop_timer guy actually will prevent any callbacks from firing.
    sock->m_snd_drop_timer->done();

    /* The two `shared_ptr`s (sock and m_snd_drop_timer) point to each other.  Nullify this to break the cycle
     * and thus avoid memory leak. */
    sock->m_snd_drop_timer.reset();
  }
}

void Node::setup_drop_timer(const Socket_id& socket_id, Peer_socket::Ptr sock)
{
  sock->m_snd_drop_timeout = sock->opt(sock->m_opts.m_st_init_drop_timeout);

  const auto on_fail = [this, socket_id, sock](const Error_code& err_code)
  {
    rst_and_close_connection_immediately(socket_id, sock, err_code, false);
    // ^-- defer_delta_check == false: for similar reason as when calling send_worker() from send_worker_check_state().
  };
  const auto on_timer = [this, socket_id, sock](bool drop_all_packets)
  {
    drop_timer_action(sock, drop_all_packets);
  };

  /* Set up the Drop Timer.  Basically give it some key fields of sock (DTO value, the In-flight
   * queue) and the callbacks to call when events occur, such as the Drop Timer expiring.
   * Additionally, when events m_snd_drop_timer wants to know about happen, we will call
   * m_snd_drop_timer->on_...(). */
  sock->m_snd_drop_timer = Drop_timer::create_drop_timer(get_logger(), &m_task_engine, &sock->m_snd_drop_timeout,
                                                         Peer_socket::Ptr(sock), on_fail, on_timer);
}

size_t Node::send(Peer_socket::Ptr sock,
                  const Function<size_t (size_t max_data_size)>& snd_buf_feed_func,
                  Error_code* err_code)
{
  using boost::asio::post;

  /* We are in user thread U != W.
   * It's important to keep that in mind in this method.  In particular, it is absolutely unsafe to
   * access m_int_state, which belongs solely to thread W and is never locked. */

  // IMPORTANT: The logic here must be consistent with sock_is_writable().

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return 0;
  }
  // else

  // Pre-condition is that m_mutex is locked already.  So EVERYTHING that can be locked, is, including the buffers.

  // Pre-condition.
  assert(sock->m_state == Peer_socket::State::S_OPEN); // Locked.

  if (sock->m_disconnect_cause) // Locked.
  {
    // Error has been recorded, and we're not CLOSED => we are DISCONNECTING.
    assert(sock->m_open_sub_state == Peer_socket::Open_sub_state::S_DISCONNECTING);

    /* Disconnection is underway.  Adding more data to the Send buffer is pointless; we
     * don't allow more data to be queued to be sent after an error (though existing buffered data
     * may yet be sent... but that's not relevant here).  @todo No graceful close yet. */

    // Mark in *err_code and log.
    FLOW_ERROR_EMIT_ERROR_LOG_INFO(sock->m_disconnect_cause);
    return 0;
  }
  // else

  // No fatal error (socket not disconnecing or closed).  However it may still be connecting.

  if (sock->m_open_sub_state == Peer_socket::Open_sub_state::S_CONNECTING)
  {
    /* Here we draw a line in the sand and refuse to buffer any data.  We could easily allow
     * buffering data even when still S_CONNECTING.  However, I am copying BSD socket semantics
     * here, as they do seem to be useful.  As a user I don't want to think I've "sent" gobs of data
     * while there's little to suggest that there's even anyone listening on the other side. */
    err_code->clear();
    return 0;
  }
  // else
  assert(sock->m_open_sub_state == Peer_socket::Open_sub_state::S_CONNECTED);

  const bool was_deqable = snd_deqable(sock); // See below.

  /* Write the user-provided data into m_snd_buf; provide the missing argument (max_data_size).
   * Round up to a multiple of max-block-size to ensure we never fragment a max-block-size-sized
   * chunk of data when they're using unreliable mode! */
  const size_t sent = snd_buf_feed_func(sock->max_block_size_multiple(sock->m_opts.m_st_snd_buf_max_size));

  // Register that the Send buffer possibly grew.
  sock->m_snd_stats.buffer_fed(sock->m_snd_buf.data_size());

  /* We've done the minimal thing send() does: added data to the send buffer.  Now we may need to
   * kick off the actual asynchronous sending of some of these data by thread W.  It's important to
   * discuss the overall strategy for how that works.
   *
   * Key question: how does W send low-level packets over UDP?  Answer: if there's anything on the
   * Send buffer or retransmission queue (if retransmission is enabled), and there is no other
   * (congestion control, probably) reason NOT to send packets, then dequeue a packet from
   * retransmission queue or Send buffer and send it off to the UDP layer; repeat in a tight loop
   * until both Send queues are empty, or there's some reason NOT to send packets (again, congestion
   * control). Let's write this in pseudo-code:
   *
   *   DEQ(sock): // Thread W only.
   *     if (!sendable(sock)):
   *       return // Slight optimization; perform this first check before locking.
   *     lock sock // Must lock because sock->m_snd_buf accessible from other threads.
   *     while (sendable(sock) && deqable(sock)):
   *       dequeue sock->m_snd_buf -> block
   *       serialize block into packet
   *       send packet via UDP
   *     unlock sock
   *
   *   sendable(sock):
   *     return <...probably some congestion control condition involving CWND or something>
   *
   *   deqable(sock):
   *     return !(sock->m_rexmit_q.empty() && sock->m_snd_buf.empty())
   *
   * When should DEQ(sock) execute?  Answer: whenever sendable(sock) and deqable(sock) are true.  If
   * they're true, but DEQ(sock) doesn't run for time period P, then it's practically like adding
   * sleep(P) from the user's point of view.  So how do we get DEQ(sock) to execute as soon as those
   * conditions are true?  Well, running it repeatedly in a thread W tight loop would do it, but
   * obviously that's unacceptable.
   *
   * So consider the initial state after sock enters ESTABLISHED state.  sendable(sock) is true;
   * deqable(sock) is false.  The moment deqable(sock) becomes true, we should execute DEQ(sock); in
   * other words in the first sock->send(), as that will add to m_snd_buf.  After DEQ(sock) exits,
   * there's no need to call DEQ(sock) until again both conditions are true.  Therefore, the
   * algorithm is: whenever sendable(sock) goes from false to true, and/or deqable(sock) from false
   * to true, call DEQ(sock).  If inside DEQ(sock) one of the conditions is still false, it will
   * quickly return.  (Call the latter a NOOP.)
   *
   * Now we must come up with a scheme that will ensure DEQ(sock) will run very quickly after either
   * condition (sendable(sock), deqable(sock)) becomes true; and that will not peg the CPU.
   *
   * Consider sendable().  Only thread W (transport layer) can determine this value: it depends on
   * wholly internal details like packets in-flight and CWND.  Therefore sendable(sock) can go
   * false->true only in W.  Hence W, whenever changing any component that might affect
   * sendable(sock) would do:
   *
   *   // ... Something related to sendable(sock) has changed....
   *   DEQ(sock) // So check and send if possible.
   *
   * Clearly this calls DEQ(sock) as soon as humanly possible after sendable(sock) becomes true.
   * Clearly it wastes no CPU cycles either.  OK.
   *
   * Now consider deqable().  sock->m_snd_buf can only change from empty to non-empty in the
   * previous statement (snd_buf_feed_func()).  That is in thread U != W.  Suppose we write:
   *
   *   SEND(sock, blocks): // Non-W threads only.
   *     lock sock // Must lock because sock->m_snd_buf accessible from other threads.
   *     add blocks -> sock->m_snd_buf
   *     if (sock->m_snd_buf was empty before previous statement)
   *       // Queue DEQ(sock) for asynchronous execution on thread W as soon as it's free:
   *       post(W, DEQ(sock))
   *     unlock sock
   *
   * Does this call DEQ(sock) as soon as deqable(sock) becomes true?  Well, DEQ(sock) can only run
   * on thread W, and the enqueuing of blocks can only happen on thread U, and post() will cause
   * DEQ(sock) to run as soon as possible.  Therefore that's as good as it can be.  Is it correct,
   * however?  The mainstream case is that once "unlock sock" finished in SEND(), thread W will get
   * some free time, execute the just-queued DEQ(), and thus everything works out.  OK so far.
   *
   * Since, however, post() is (obviously) asynchronous and done from thread non-W, there is
   * potential for other tomfoolery.  First consider competing SEND() calls from other threads.
   * Because of locking, they will be entirely sequential even from different threads and thus can
   * be considered as all in one thread U != W.  Now suppose SEND() placed DEQ() onto W, and another
   * SEND() executes before DEQ() executes on W.  No problem: since only DEQ() can dequeue the Send
   * buffer, and the 1st SEND() made the buffer non-empty, the 2nd SEND() will not affect the DEQ()
   * situation, since it cannot make m_snd_buf become non-empty after being empty (was already
   * non-empty).
   *
   * Second consider SEND(sock, blocks) executing while a W handler is executing.  Now suppose this
   * W handler discovers that sendable() may be affected and thus calls DEQ(sock) as shown above;
   * meanwhile SEND() posts DEQ(sock) onto W as well.  W will wait until SEND(sock, blocks) exits
   * (due to the lock) before executing most of DEQ(sock), but when it does it will be ITS DEQ(sock)
   * that executes first (regardless of whether the post from thread U happened first).  This
   * DEQ(sock) will not be a NOOP, which is great.  Now, thread W should exit that handler and
   * finally execute SEND()'s posted DEQ() -- which will be a NOOP, because the synchronous
   * DEQ(sock) from thread W preempted it.
   *
   * Is this OK?  Most likely.  It'll spend some extra CPU cycles on the check in the NOOP, but
   * that's it.  Now, there is some conceivable way that, maybe, such NOOPs could happen a lot in a
   * very busy system and perhaps even "bunch" up to peg the CPU.  However, after doing many thought
   * experiments, I unable to come up with anything actually worrying.
   *
   * The other way deqable(sock) can become true is if m_rexmit_q was empty but becomes non-empty.
   * In other words, if we detect packet as Dropped, we will have added it (if retransmission is on)
   * to m_rexmit_q.  This can only happen on thread W and thus is handled similarly to
   * sendable(sock):
   *
   *   // ... Something related to deqable(sock) has changed....
   *   DEQ(sock) // So check and send if possible.
   *
   * So this system should be OK.  Now let's map the above pseudocode to actual code.
   *
   * SEND(sock, blocks) is the very method you're reading now (Peer_socket::send() and
   * Node::send(), runs in thread U != W).  DEQ(sock) is Node::send_worker(sock) (runs in thread
   * W).  sendable(sock) is Node::can_send(sock).  deqable(sock) is Node::snd_deqable(sock).
   * post(W, f) is post(Node::m_task_engine, f).
   *
   * OK, there is one more small caveat.  If DEQ(sock) is placed onto W by SEND(sock, blocks),
   * then before this DEQ() is executed, thread W may change the state of sock (for example, close
   * it).  Therefore, DEQ() must also ensure it's operating in a state where it can send data
   * (ESTABLISHED at least), and if not, NOOP.  Of course if DEQ() is executed synchronously by W,
   * then this is unnecessary (since W code wouldn't execute DEQ() directly unless already in a
   * proper state for this).  So, send_worker_check_state() is actually a little bit more than just
   * DEQ(), while send_worker() is just DEQ().  send() posts send_worker_check_state(), while
   * thread W executes send_worker() directly. */

  if ((!was_deqable) && (sent != 0))
  {
    // Possibly send_worker() can send packets now (send buffer went from empty to not).
    post(m_task_engine, [this, sock]() { send_worker_check_state(sock); });
  }

  err_code->clear();
  return sent;
  // Note that sock->m_mutex is unlocked here (and send_worker() will lock it again when it [probably soon] executes).
} // Node::send()

bool Node::sock_is_writable(const boost::any& sock_as_any) const
{
  using boost::any_cast;

  const Peer_socket::Const_ptr sock = any_cast<Peer_socket::Ptr>(sock_as_any);

  Peer_socket::Lock_guard lock(sock->m_mutex); // Many threads can access/write below state.

  /* Our task here is to return true if and only if at this very moment calling sock->send() would
   * yield either a return value of > 0 OR a non-success *err_code.  In other words, send() would
   * return "something."  This is used for Event_set machinery.
   *
   * This should mirror send()'s algorithm.  @todo Should send() call this, for code reuse?
   * Maybe/maybe not.  Consider performance when deciding.
   *
   * - If state is CLOSED, then some sort of error/terminating condition occurred, so send()
   *   would return 0 and non-success Error_code == sock->m_disconnect_cause.  (Writable.)
   * - Otherwise, if state is OPEN+DISCONNECTING, then graceful close (@todo implement it) is
   *   underway; we do not allow more data to be sent (except what's already in Sent buffer), so
   *   send() would return 0 and non-success Error_code == sock->m_disconnect_cause.
   *   (Writable.)
   * - Otherwise, if state is OPEN+CONNECTED, and there is Send buffer space, send() would return >
   *   0 and no error.  (Writable.)
   * - The other remaining possibilities:
   *   - OPEN+CONNECTED but no Send buffer space (returns 0, no error).  (Not Writable.)
   *   - OPEN+CONNECTING -- we don't allow accumulating data in Send buffer (returns 0, no error).
   *     (Not Writable.) */

  return (sock->m_state == Peer_socket::State::S_CLOSED)
         || (sock->m_open_sub_state == Peer_socket::Open_sub_state::S_DISCONNECTING)
         || ((sock->m_open_sub_state == Peer_socket::Open_sub_state::S_CONNECTED)
             && snd_buf_enqable(sock));
} // Node::sock_is_writable()

void Node::send_worker_check_state(Peer_socket::Ptr sock)
{
  // See big comment block in Node::send() first.

  // We are in thread W.

  /* This method can be thought of as the chunk of the finite state machine that defines what
   * happens when the "user called send, adding at least 1 block to the send buffer" (@todo: or any data
   * at all, if in reliable mode?) event is defined.  Therefore, we will have a switch() that will handle every
   * state and decide what should happen when that event fires in that state.
   *
   * send() placed us onto thread W.  When send() did so, m_int_state (which it was not allowed to
   * check, as only thread W can access it) was at least ESTABLISHED (since state was
   * S_OPEN+S_CONNECTED, ensured via assert()).  Therefore, we can eliminate several states with
   * assert()s: SYN_SENT, SYN_RCVD. */

  switch (sock->m_int_state)
  {
  case Peer_socket::Int_state::S_ESTABLISHED:
    // Mainstream case.
    send_worker(sock, false);
    /* ^-- defer_delta_check == false: because we were invoked from thread U != W, we are NOT
     * invoked from async_low_lvl_recv().  Therefore, we will NOT perform
     * event_set_all_check_delta(false) before the boost.asio handler exits.  Therefore boost.asio
     * may sleep (block) before event_set_all_check_delta(false).  Therefore that would delay
     * delivery of the Writable event to the user.  Therefore force the delta check immediately.
     * See Node::m_sock_events doc header for details. */
    break;
  case Peer_socket::Int_state::S_CLOSED:
    // Unlikely but legitimate.
    FLOW_LOG_INFO('[' << sock << "] "
                  "in state [" << sock->m_int_state << "] "
                  "closed before asynchronous send_worker() could proceed.");
    break;
  case Peer_socket::Int_state::S_SYN_SENT:
  case Peer_socket::Int_state::S_SYN_RCVD:
    // Crash.  See above reasoning.
    FLOW_LOG_WARNING('[' << sock << "] "
                     "in state [" << sock->m_int_state << "] "
                     "somehow had send() called on it.");
    assert(false);
    break;
  } // switch (sock->m_int_state)
} // Node::send_worker_check_state()

void Node::send_worker(Peer_socket::Ptr sock, bool defer_delta_check)
{
  using boost::asio::buffer;
  using boost::next;
  using boost::ratio;
  using boost::ratio_string;
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using boost::shared_ptr;
  using std::list;

  // We are in thread W.

  // See big comment block in Node::send() first.

  // Pre-condition.
  assert(sock->m_int_state == Peer_socket::Int_state::S_ESTABLISHED);

  /* We are about to potentially send a bunch of DATA packets.  Before sending a given packet, we
   * will call can_send() which will ask the congestion control module whether there is space in
   * what it thinks is the available pipe and return true if so (as well as check rcv_wnd, ensuring
   * the receiver's Receive buffer can handle the data once they arrive).  However, how it answers
   * that question depends on the size of the pipe (m_snd_cong_ctl->congestion_window_bytes(),
   * a/k/a CWND). Many (most?) congestion control modules will want to reduce CWND when a
   * connection has been idle -- not sending anything, due to no data to be sent in Send buffer --
   * for a while.  Thus we must call m_snd_cong_ctl->on_idle_timeout() if we've hit Idle Timeout.
   *
   * The definition of Idle Timeout we use is from TCP RFC 5681-4.1 (and DCCP CCID 2 RFC 4341-5.1).
   * It's simple: Idle Timeout is DTO (Drop Timeout) time units since a DATA packet has been last
   * sent.  While I basically grasp the intuition behind it (if a DTO since even the last-sent
   * packet has expired, and no retransmission/further transmission has occurred, then there must
   * have been no more data for a while), I can't quite prove to myself that it's exactly right,
   * mostly due to the fact that DTO may change over time.  It's probably right though, as RFC 4341
   * recommends it, even though that protocol is closer to NetFlow than TCP (full selective ACKs).
   * Anyway, if we see too many false Idle timeouts, revisit this.
   *
   * Why check this now?  Why not start a proper timer, each time packet is sent, instead and just
   * inform m_snd_cong_ctl when it fires?  Answer: timer management is somewhat of a pain in the ass
   * (as you can see in our other various timers, such as m_snd_drop_timer).  Here we have an opportunity
   * to simply check the condition and affect CWND right before CWND would be used anyway
   * (can_send()).  It's simpler, and the performance impact is negligible (it's just a
   * Fine_clock::now() call and a comparison).  You ask, why not do the same for other timers
   * then, in particular the Drop Timer?  Answer: for Drop Timer, we really need to know exactly
   * when it fires, so that we can Drop In-flight packets right then and possibly send more
   * packets (among other things).  In this case there is no such requirement; we only care about
   * whether the Idle Timeout has tripped when we're about to send something. */

  /* To avoid a very close race between DTO and idle timeout, apply a slight factor of > 1 to DTO.
   * Using boost::ratio<> instead of a double or something for same reason as in
   * new_round_trip_time_sample(). */
  using Idle_timeout_dto_factor = ratio<110, 100>;
  const Fine_duration idle_timeout
    = sock->m_snd_drop_timeout * Idle_timeout_dto_factor::num / Idle_timeout_dto_factor::den;
  const Fine_duration since_last_send = Fine_clock::now() - sock->m_snd_last_data_sent_when;

  if ((sock->m_snd_last_data_sent_when != Fine_time_pt()) && (since_last_send > idle_timeout))
  {
    // Arguable if this should be INFO or TRACE.  We'll see.
    FLOW_LOG_INFO("Idle timeout triggered for [" << sock << "]; "
                  "last activity [" << round<milliseconds>(since_last_send) << "] ago "
                  "exceeds idle timeout [" << round<milliseconds>(idle_timeout) << "] "
                  "= " << (ratio_string<Idle_timeout_dto_factor, char>::prefix()) << " x "
                  "[" << round<milliseconds>(sock->m_snd_drop_timeout) << "].");
    sock->m_snd_cong_ctl->on_idle_timeout();
    sock->m_snd_stats.idle_timeout();
  }

  /* Check networking conditions (presumably congestion control) and flow control (rcv_wnd).
   * Ideally this would always be true, but then we'd overwhelm the link when send() is invoked on
   * large amounts of data and/or repeatedly. */
  if (!can_send(sock))
  {
    FLOW_LOG_TRACE('[' << sock << "]: "
                   "Initial check: can_send() is false.");
    return;
  }
  // else can send if there are data to send.

  /* Didn't lock sock above, as can_send() depends only on internal state, which is accessed from
   * thread W only.  This is an optimization to avoid thread contention (with non-W send()s) for the
   * lock in the case when congestion control is preventing sends.
   *
   * Have to lock now, for sock->m_snd_buf access (at least). */

  const bool rexmit_on = sock->rexmit_on();
  bool writable; // See below.
  {
    Peer_socket::Lock_guard lock(sock->m_mutex);

    // Check whether enough data in retransmission queue or snd_buf to send a packet.
    if (!snd_deqable(sock))
    {
      FLOW_LOG_TRACE('[' << sock << "]: "
                     "Initial check: can_send() is true, but no data to send.");
      return;
    }
    // else can send >= 1 packet.

    // For brevity and a bit of speed:
    Socket_buffer& snd_buf = sock->m_snd_buf;
    list<Peer_socket::Sent_packet::Ptr>& rexmit_q = sock->m_snd_rexmit_q;
    size_t& rexmit_q_size = sock->m_snd_rexmit_q_size;
    Sequence_number& snd_next_seq_num = sock->m_snd_next_seq_num;

    // @todo Implement graceful close.
    assert(sock->m_open_sub_state != Peer_socket::Open_sub_state::S_DISCONNECTING);

    FLOW_LOG_TRACE('[' << sock << "]: "
                   "Initial check: Will send from rexmit queue of size [" << rexmit_q_size << "] and/or "
                   "Send buffer with total size [" << snd_buf.data_size() << "].");
    // Very verbose and CPU-intensive!
    FLOW_LOG_DATA("Send buffer data = [\n" << snd_buf << "].");

    // Send packets until one or both of can_send() and snd_deqable() become false.
    do
    {
      shared_ptr<Data_packet> data;
      Peer_socket::Sent_packet::Ptr sent_pkt;
      bool rexmit = false;

      /* Record send time.  It's only a temporary value for logging, until we
       * actually send packet.  However, do generate the permanent m_order_num, which is unique. */
      Peer_socket::Sent_packet::Sent_when sent_when{ sock_get_new_snd_order_num(sock), Fine_clock::now(), 0 };

      /* To provide the best experience on the receiving side, retransmit before sending new data,
       * so that Receive buffer on other side receives data as soon as possible. */
      if (rexmit_q.empty())
      {
        // Nothing in retransmission queue, so something is in Send buffer.

        // Create low-level DATA packet.
        data = Low_lvl_packet::create_uninit_packet<Data_packet>(get_logger());
        data->m_rexmit_id = 0; // First (if retransmission is off, only) send attempt.

        // Dequeue one block into the packet's data field.

        /* Try to dequeue the head block directly into data.m_data.  Because we are operating snd_buf
         * with block_size_hint == sock->max_block_size(); and because we don't send unless CWND
         * allows for at least max_block_size() bytes to be sent, the following should be a
         * constant-time operation (a swap of internal buffers) as opposed to a copy. */
        snd_buf.consume_buf_move(&data->m_data, sock->max_block_size());

        // snd_deqable() returned true, so there must be at least one byte available.
        assert(!data->m_data.empty());

        // Set sequence number; then advance the next sequence number variable for the next time we do this.
        data->m_seq_num = snd_next_seq_num;
        advance_seq_num(&snd_next_seq_num, data);

        /* We are just about to send the packet.  Assume it has been sent.  It is not yet Acknowledged
         * and not yet Dropped.  Therefore it is now In-flight.  We should place its info at the back of
         * m_snd_flying_pkts_by_sent_when.  We must maintain the invariant w/r/t that structure (see comment
         * for m_snd_flying_pkts_by_sent_when).
         *
         * Purpose of keeping these data: at least for comparison against Congestion Window,
         * for congestion control. */

        // Guarantee that the new sequence number is > all the currently In-flight ones.
        assert(data->m_seq_num >= snd_past_last_flying_datum_seq_num(sock));
        /* Therefore we will add the following to the end of the map's ordering.  Note we've
         * incremented m_snd_next_seq_num already, maintaining that member's invariant relationship
         * with m_snd_flying_pkts_by_sent_when. */

        // New packet: create new metadata object.  Record send time.  (The latter will be rewritten later.)
        sent_pkt = Peer_socket::Sent_packet::Ptr(new Peer_socket::Sent_packet(rexmit_on, data, sent_when));
      }
      else // if (!rexmit_q.empty())
      {
        // Get packet and metadata from front of retransmission queue.
        rexmit = true;
        sent_pkt = rexmit_q.front();

        --rexmit_q_size;
        rexmit_q.pop_front();

        // We'd saved the packet we sent last time -- just need to update some things before resending.
        data = sent_pkt->m_packet;

        // Retransmitting -- update retransmit count ID (used to match acks to the acked transmit attempt).
        ++data->m_rexmit_id;

        // Record the send time of this newest attempt.  (If pacing enabled this will be rewritten later.)
        sent_pkt->m_sent_when.push_back(sent_when);

        // Chronologically, no packets sent after this one have been acked yet, as this packet is new.
        sent_pkt->m_acks_after_me = 0;
      }

      /* Note: We have saved Fine_clock::now() as the send time of the packet. However, especially
       * if pacing is enabled, we want to record it at the time it is actually sent (pacing may
       * delay it).  Even if pacing is disabled, CPU pegging may cause a delay in sending (although
       * whether that should "count" is a more philosophical question).  With pacing, though, since
       * pacing spreads out packets over SRTT, and SRTT is measured based on
       * Sent_packet::m_sent_when, RTTs artifically become longer and longer if we record the send
       * time now.  Anyway, this means m_sent_when.back() should be overwritten when the packet is
       * actually sent (which should be very soon, unless pacing is enabled).
       * See async_sock_low_lvl_packet_send_paced(). */

      // data and sent_pkt are ready.

      // Add to snd_flying_pkts* and friends; update byte counts.
      snd_flying_pkts_push_one(sock, data->m_seq_num, sent_pkt);

      /* By adding to m_snd_flying_pkts_by_sent_when (i.e., increasing In-flight byte count), we may have
       * affected the result of can_send().  We do check it at the end of the while () body, so OK. */

      // Fill out common fields and asynchronously send packet (packet pacing potentially performed inside).
      if (!async_sock_low_lvl_packet_send_or_close_immediately(sock,
                                                               Low_lvl_packet::ptr_cast(data),
                                                               defer_delta_check))
      {
        return;
      }

      sock->m_snd_stats.data_sent(data->m_data.size(), rexmit);
    }
    while (can_send(sock) && snd_deqable(sock)); // (there is CWND/rcv_wnd space; and either rexmittable or new data)

    FLOW_LOG_TRACE('[' << sock << "]; connection [" << sock << "]: "
                   "Final check: "
                   "can_send() == [" << can_send(sock) << "]; "
                   "snd_deqable() == [" << snd_deqable(sock) << "].");

    writable = snd_buf_enqable(sock); // Must do before releasing lock.
  } // lock

  /* Finally, check if the above has dequeued enough of m_snd_buf for it to accept more data from
   * user.  If so, sock is certainly now Writable.  Therefore we should soon inform anyone waiting
   * on any Event_sets for sock to become Writable.
   *
   * Caveat: Similar to that in Node::handle_syn_ack_ack_to_syn_rcvd() at similar point in the
   * code.
   *
   * Also: why do this outside the above locked block?  Same reason as similar code in
   * handle_data_to_established(). */
  if (writable &&
      m_sock_events[Event_set::Event_type::S_PEER_SOCKET_WRITABLE].insert(sock).second)
  {
    // Possibly inform the user for any applicable Event_sets right now.
    event_set_all_check_delta(defer_delta_check);
  }

  /* @todo After we implement graceful close, if we'd emptied m_snd_buf above, then here we should
   * advance the graceful close towards the final situation (m_int_state and m_state both
   * S_CLOSED). */
} // Node::send_worker()

bool Node::can_send(Peer_socket::Const_ptr sock) const
{
  using std::min;

  /* m_snd_cong_ctl is the congestion control module, and its CWND value determines how many bytes can
   * be In-flight at any given time.  If there are enough free bytes (CWND - In-flight) to send
   * data, then we can send.  Otherwise we cannot.  Easy, except what's "data"?  There are two
   * reasonable answers.  One: a byte or more.  Two: min(max-block-size, Send buffer size).  The former
   * answer is fine but somewhat annoying, because then we have to lock sock here***.  The 2nd answer
   * clearly works but is potentially a little greedier than necessary (i.e., if the 1st block to
   * send is small enough to fit into CWND, but CWND doesn't have max-block-size space).
   * However, actually, we pretty much have to choose the 2nd answer regardless, as we don't want to
   * fragment max-block-size-sized chunks, if we can help it (in the spirit of the reliability
   * guarantee [when running in unreliable mode] made in send() method doc header).
   *
   * I choose the 2nd answer, because (1) it's easier (no locking of sock); (2) it is used by real
   * TCP implementations which keep CWND in multiples of MSS (equivalent of max-block-size); (3)
   * it's still safe; and (4) see previous paragraph's end.  Regarding safety: it's safe, since
   * there can be no deadlock, because even if there's < MBS bytes free, eventually In-flight
   * packets will become Acknowledged or Dropped and no longer be In-flight, freeing up CWND space;
   * and CWND is guaranteed to always be at least 1 * MBS. Thus eventually can_send() will return
   * true.
   *
   * *** - I am now not sure why I wrote this.  Why would we have to lock sock here in that case? */

  // We have rcv_wnd also; so pretend previous paragraph has: s/CWND/min(CWND, rcv_wnd)/.

  const size_t pipe_taken = sock->m_snd_flying_bytes;
  const size_t cong_wnd = sock->m_snd_cong_ctl->congestion_window_bytes();
  const size_t& rcv_wnd = sock->m_snd_remote_rcv_wnd; // @todo Any particular reason this has & but not pipe_taken?
  // Send no more than the network NOR the other side's Receive buffer can take.
  const size_t pipe_total = min(cong_wnd, rcv_wnd);

  const bool can
    = (pipe_taken < pipe_total) && ((pipe_total - pipe_taken) >= sock->max_block_size());

  FLOW_LOG_TRACE("cong_ctl [" << sock << "] info: can_send = [" << can << "]; "
                 "pipe_taken = [" << sock->bytes_blocks_str(pipe_taken) << "]; "
                 "cong_wnd = [" << sock->bytes_blocks_str(cong_wnd) << "]; "
                 "rcv_wnd = [" << sock->bytes_blocks_str(rcv_wnd) << "].");

  return can;
} // Node::can_send()

size_t Node::receive(Peer_socket::Ptr sock,
                     const Function<size_t ()>& rcv_buf_consume_func,
                     Error_code* err_code)
{
  using boost::asio::post;

  /* We are in user thread U != W.
   * It's important to keep that in mind in this method.  In particular, it is absolutely unsafe to
   * access m_int_state, which belongs solely to thread W and is never locked. */

  // IMPORTANT: The logic here must be consistent with sock_is_readable().

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return 0;
  }
  // else

  // Pre-condition is that m_mutex is locked already.  So EVERYTHING that can be locked, is, including the buffers.

  // Pre-condition.
  assert(sock->m_state == Peer_socket::State::S_OPEN); // Locked.
  assert((sock->m_open_sub_state == Peer_socket::Open_sub_state::S_CONNECTED) ||
         (sock->m_open_sub_state == Peer_socket::Open_sub_state::S_CONNECTING) ||
         (sock->m_open_sub_state == Peer_socket::Open_sub_state::S_DISCONNECTING));

  /* In the rest of the method we must ensure we handle all the cases (-1a/b/c-, -2-) documented in
   * the Peer_socket::receive() documentation header.  -3- was already handled by
   * Peer_socket::receive() before calling us. */

  // Try to dequeue stuff into their buffer.
  const bool no_bytes_available = sock->m_rcv_buf.empty();
  const size_t bytes_consumed = rcv_buf_consume_func();

  if (bytes_consumed != 0)
  {
    /* Unequivocal: if there was stuff in the Receive buffer and was able to place it into their
     * buffer then there is no error.  (Even if m_disconnect_cause is not success, we are only
     * supposed to report that after the Receive buffer has been emptied.)
     *
     * This handles case -2-. */
    FLOW_LOG_TRACE("User thread receive() for [" << sock << "] "
                   "has successfully returned [" << bytes_consumed << "] bytes.");
    err_code->clear();

    /* We have changed (increased) the amount of free space in m_rcv_buf.  This has rcv_wnd
     * implications.  We have to at least check whether we should send a window update to the
     * other side.  However all such book-keeping must be done in thread W due to the data
     * involved; call this->receive_wnd_updated(sock). */
    post(m_task_engine, [this, sock]() { receive_wnd_updated(sock); });

    if (sock->m_rcv_buf.empty()
        && (sock->m_open_sub_state == Peer_socket::Open_sub_state::S_DISCONNECTING))
    {
      /* We've emptied the Receive buffer; and we're in the middle of a graceful close.  (@todo
       * Graceful close not yet implemented.)  There are two possibilities.  One, m_int_state ==
       * S_CLOSED.  In this case the graceful close, at the transport layer, is over, and the only
       * thing stopping us from entering m_state == S_CLOSED (via close_connection_immediately())
       * was that the user hadn't read all of m_rcv_buf.  In this case thread W should
       * close_connection_immediately().  Two, m_int_state may be after ESTABLISHED but before
       * CLOSED, in which case thread W still has to finish up graceful closing anyway.
       *
       * We are in thread W and cannot work with m_int_state, so checking it here is not possible.
       * Therefore we put this task onto thread W. */
      post(m_task_engine,
           [this, sock]() { receive_emptied_rcv_buf_while_disconnecting(sock); });
    }
    return bytes_consumed;
  }
  // else if (bytes_consumed == 0)

  if (sock->m_open_sub_state == Peer_socket::Open_sub_state::S_CONNECTING)
  {
    /* This is case -1b-.  Since we are CONNECTING, no data could have been received yet (simply
     * not at that stage of connection opening), so Receive buffer is empty. */
    FLOW_LOG_TRACE("User thread receive() for [" << sock << "] "
                   "has successfully returned no bytes because still not fully connected.");
    err_code->clear();
    return 0;
  }
  // else if (state is CONNECTED or DISCONNECTING)

  /* We're CONNECTED or DISCONNECTING but could get no bytes.  Let's examine each state.
   *
   *   - CONNECTED: Either they provided a zero-sized target buffer (in which case
   *     !no_bytes_available), or the Receive buffer is simply empty.  Thus this is either -1a- or
   *     -1c- (no_bytes_available determines which).
   *
   *   - DISCONNECTING: Either:
   *     - the initial block was too large for the max_data_size they provided in their receive()
   *       call (in which case !no_bytes_available); or
   *     - they called close_final() (@todo not yet implemented) and thus the Receive buffer was
   *       cleared at that time, and all incoming data were ignored after that; thus the Receive
   *       buffer is empty, but a graceful close is still in progress; or
   *     - they did not call close_final(), but there is a graceful close in progress, and the
   *       Receive buffer is simply empty.
   *     Thus this is either -1a- or -1c-. */

  if (!no_bytes_available)
  {
    // This is case -1c-.
    FLOW_LOG_TRACE("User thread receive() for [" << sock << "] "
                   "has data to return, but the provided buffer size is too small.");
    err_code->clear();
    return 0;
  }
  // else if (no_bytes_available)

  // This is case -1a-.
  FLOW_LOG_TRACE("User thread receive() for [" << sock << "] "
                 "returning no data because Receive buffer empty.");

  err_code->clear();

  /* @todo Sigh.  There's more.  Yes, in some situations we can return 0/success here.  In other
   * situations, though, we should return 0/<Error_code for graceful close> here.  The latter
   * case would be in the situations where we know no data is coming, or user has said he doesn't
   * care about any more data:
   *
   *   -1- A graceful close was initiated by the OTHER side.  (Therefore no data could be coming to
   *       save into Receive buffer.)
   *   -2- Only we initiated the graceful close, but it was via close_final(), i.e., user is not
   *       interested in any incoming data anymore.  (Therefore we'll always just ignore any
   *       incoming DATA and not put it into Receive buffer.)
   *   -3- Only we initiated the graceful close, and it was via close_start() (i.e., user cares
   *       about further incoming data); however, the final handshake has reached a state in which
   *       further data cannot be incoming.  (Therefore no data could be coming to save into Receive
   *       buffer.)
   *
   * I am not writing code for this logic at this time.  The implementations depends on how
   * exactly our graceful close works.  This entire method, right now, is dead code, since there is
   * no graceful close, but I wrote it anyway to provide a skeleton for the future, since I
   * already thought about it.  However it would be unreasonable to implement the above logic in the
   * absence of graceful close in the first place, skeleton or not.  Therefore, dead code or not, I
   * do the "conservative" thing: return 0/success even in the above situations.  Eventually the
   * graceful close will complete, at which point we'll return an error anyway, so the user won't be
   * left uninformed forever (worst case: the close will time out).
   *
   * For when we do implement the above logic, some thoughts: Detecting the situation in thread U
   * != W may be difficult and may introduce complex synchronization issues.  One way
   * to do it might be to introduce synchronized bool Peer_socket::m_no_more_rcv_data, which
   * starts at false and can become true (but not false again).  This member would be set to true,
   * by thread W, if and only if one of the above situations is detected by thread W.  Then here
   * we'd check it, and if it's true, return error; otherwise return success.
   *
   * IMPORTANT: The logic here must be consistent with sock_is_readable(). */
  return 0;
} // Node::receive()

bool Node::sock_is_readable(const boost::any& sock_as_any) const
{
  using boost::any_cast;

  const Peer_socket::Const_ptr sock = any_cast<Peer_socket::Ptr>(sock_as_any);

  Peer_socket::Lock_guard lock(sock->m_mutex); // Many threads can access/write below state.

  /* Our task here is to return true if and only if at this very moment calling sock->receive(),
   * assuming sufficient user buffer space, would yield either a return value of > 0 OR a
   * non-success *err_code.  In other words, receive() would return "something."  This is used for
   * Event_set machinery.
   *
   * This should mirror receive()'s algorithm.  @todo Should receive() call this, for code reuse?
   * Maybe/maybe not.  Consider performance when deciding.
   *
   * - If state is CLOSED, then some sort of error/terminating condition occurred, so receive()
   *   would return 0 and non-success Error_code == sock->m_disconnect_cause.  (Readable.)
   * - Otherwise, if Receive buffer can be dequeued, receive() would return > 0.
   * - Otherwise, if Receive buffer cannot be dequeued, receive() would return 0 and no error. (Not
   *   Readable.)  Note that Receive buffer is guaranteed to be clear when entering non-Readable
   *   non-error states (OPEN+CONNECTING, OPEN+DISCONNECTING).  (Readable.)
   *
   * @todo Once we implement graceful close, there will be situations where Receive buffer is empty, state is
   * OPEN+DISCONNECTING, m_disconnect_cause = <cause of disconnect>, and we should return true (Readable)
   * here (only when we also know that no future Receive traffic possible).  See receive(). */

  return (sock->m_state == Peer_socket::State::S_CLOSED) || rcv_buf_deqable(sock);
} // Node::sock_is_readable()

void Node::receive_wnd_updated(Peer_socket::Ptr sock)
{
  // We are in thread W.

  /* rcv_wnd (free Receive buffer space) is sent to other side opportunistically in ACKs.  While
   * sender is sending data, they will have a good idea of our rcv_wnd as well.  Is that (in a
   * one-way-traffic situation) sufficient however?  If the sender is not sending data, because the
   * application on the sender doesn't provide more data to send, then the discussion is moot.
   * What if the sender is not sending data, because we have told it rcv_wnd is 0 (meaning our
   * Receive buffer is full)?  This can and will happen.  For example suppose our application layer
   * simply stops reading from Receive buffer for a while, resulting in rcv_wnd 0 sent in one of the
   * ACKs.  Now sender knows rcv_wnd is 0.  Now suppose our application reads off the entire Receive
   * buffer.  rcv_wnd is now 100%, but since sender is not sending (because it thinks rcv_wnd is
   * still 0), there will be no ACKs onto which to add rcv_wnd.  Thus the traffic completely stops.
   *
   * Original RFC 793 (as well as RFC 1122) suggests TCP sender should deal with this by "probing"
   * with 1-byte (I think) data segments sent regularly (every RTO; our DTO) in order to trigger
   * ACKs, which would eventually expose the non-zero rcv_wnd.  To me this seems to have the
   * disadvantage of complexity and implications on how we packetize data (especially since in
   * unreliable mode we're not supposed to break up contiguous blocks of max-block-size bytes).
   * Also it is not as responsive as it could be.  Consider that the most common scenario in
   * high-speed downloads is that the Receive buffer is exceeded only momentarily (due to thread
   * contention on receiver or something) but is then quickly emptied (once the thread contention is
   * resolved).  In case that happens in a fraction of a second, having the probe occur a DTO later
   * wastes a long time.  Instead the RECEIVER could take initiative and send an empty ACK with a
   * rcv_wnd update.  When should it do this?  A naive answer would be to do it simply EVERY time
   * free Receive buffer space increases.  However that'd be terrible, as in a typical scenario
   * (where lots of bytes arrive, while user reads off lots of bytes due to them becoming available
   * to read) it would explode the number of ACKs.  Even in the "sender has stopped due to
   * rcv_wnd=0" situation, this would result in a ton of ACKs.  Plus it would cause sender to start
   * recovering with quite small windows which is inefficient.  So the less naive way is to send the
   * ACK of our volition if free buffer space has increased by some % of its max capacity (like
   * 50%).
   *
   * This would certainly solve aforementioned situation where Receive buffer fills up momentarily
   * but then is quickly cleared.  A fraction of a second later, the free space will have increased
   * by over 50%, an ACK would go to sender, and sender would work with a nice large rcv_wnd.
   * However, if the Receiver only reads off 49% of the data and then stops, traffic would remain
   * stuck (even though 49% of the buffer is available).  This is where the sender-side probing
   * would solve it (slowly); though sender-side unsolicited ACKing on a timer would also do.  I
   * leave that as a @todo; probably important in a widely-used net_flow; but without it it should be
   * sufficient for the initial intended purpose of net_flow.  In that use scenario, we count on the
   * receiver code to be well behaved and read from Receive buffer as soon as the computer lets it.
   *
   * With that settled, there is one more concern.  This is intuitively clear but is also mentioned
   * in RFC 1122-4.2.2.17.  Suppose the receiver-initiated ACK after 50% of buffer is cleared is
   * dropped by the network.  ACKs are not reliable (there are no ACKs of ACKs), so then we're back
   * in no-more-traffic-forever land.  To solve this, I implement this scheme: Having sent that ACK,
   * start a timer and then send it again periodically, until some long time period (something like
   * a minute) expires (just in case) OR we get a new DATA packet from the sender.  In the latter
   * case we're back in business, as it implies sender got our window update.  Note that this
   * mechanism is not necessary any longer, once we implement sender-side probing as explained
   * above. */

  // As always, no need to lock m_state, etc., unless we plan to alter them, since no other thread can alter them.

  if (sock->m_int_state != Peer_socket::Int_state::S_ESTABLISHED)
  {
    /* Yes, they emptied Receive buffer.  However, we haven't finished the graceful close.
     * Therefore -- even though one more barrier to reaching m_state == S_CLOSED has been removed --
     * there's nothing further to do at this time.  In fact, in certain situations we might even
     * get more data onto the Receive buffer!  @todo No graceful close yet. */
    FLOW_LOG_INFO('[' << sock << "] Receive buffer space freed, "
                  "but state is now [" << sock->m_int_state << "]; ignoring.");
    return;
  }
  // else if (m_int_state == S_ESTABLISHED)

  if (sock->m_rcv_in_rcv_wnd_recovery)
  {
    /* We have already sent the unsolicited ACK and are currently in the phase where we're
     * periodically sending more, until we get some DATA from sender or a long period of time
     * passes.  Even if we've freed yet another large chunk of the buffer since the last ACK, do
     * not start again... just let it continue. */
    FLOW_LOG_TRACE('[' << sock << "] Receive buffer space freed, but "
                   "we are already in rcv_wnd recovery mode.  Nothing to do.");
    return;
  }
  // else

  // Grab available Receive buffer space.
  const size_t rcv_wnd = sock_rcv_wnd(sock);
  // @todo That was a ~copy/paste of Node::async_low_lvl_ack_send().  Add code reuse.

  const size_t& last_rcv_wnd = sock->m_rcv_last_sent_rcv_wnd;

  if (rcv_wnd <= last_rcv_wnd)
  {
    /* This seems odd, but one can imagine more data arriving between when we were placed onto W's
     * task queue and when we executed.  So it's not that odd and not worth INFO or WARNING. */
    FLOW_LOG_TRACE('[' << sock << "] Receive buffer space freed, but "
                   "free space [" << sock->bytes_blocks_str(rcv_wnd) << "] <= prev "
                   "free space [" << sock->bytes_blocks_str(last_rcv_wnd) << "].  Nothing to do.");
    return;
  }
  // else

  const size_t diff = rcv_wnd - last_rcv_wnd;
  const unsigned int pct = sock->opt(sock->m_opts.m_st_rcv_buf_max_size_to_advertise_percent);
  const size_t max_rcv_buf_size = sock->max_block_size_multiple(sock->m_opts.m_st_rcv_buf_max_size);
  const size_t min_inc = max_rcv_buf_size * pct / 100;

  if (diff < min_inc)
  {
    // Not big enough increase; wait until more space is freed before informing other side.
    FLOW_LOG_TRACE('[' << sock << "] Receive buffer space "
                   "freed is [" << sock->bytes_blocks_str(diff) << "] since last advertisement; "
                   "< threshold [" << pct << "%] x "
                   "[" << sock->bytes_blocks_str(max_rcv_buf_size) << "] = "
                   "[" << sock->bytes_blocks_str(min_inc) << "].  Not advertising rcv_wnd yet.");
    return;
  }
  // else cool. Let's advertise it.

  // This is ~equally as rare as Receive buffer overflows, so this is worth an INFO message.
  FLOW_LOG_INFO('[' << sock << "] Receive buffer space "
                "freed is [" << sock->bytes_blocks_str(diff) << "] since last advertisement; "
                "rcv_wnd = [" << sock->bytes_blocks_str(rcv_wnd) << "]; "
                ">= threshold [" << pct << "%] x "
                "[" << sock->bytes_blocks_str(max_rcv_buf_size) << "] = "
                "[" << sock->bytes_blocks_str(min_inc) << "].  Sending unsolicited rcv_wnd-advertising ACK "
                "and entering rcv_wnd recovery.");

  // Prevent any further shenanigans (see above), until we exit this mode.
  sock->m_rcv_in_rcv_wnd_recovery = true;
  // Mark this down, so that we exit this mode eventually.
  sock->m_rcv_wnd_recovery_start_time = Fine_clock::now();

  // Record we started the mode.
  sock->m_rcv_stats.rcv_wnd_recovery_event_start();

  async_rcv_wnd_recovery(sock, rcv_wnd);
} // Node::receive_wnd_updated()

void Node::async_rcv_wnd_recovery(Peer_socket::Ptr sock, size_t rcv_wnd)
{
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using boost::weak_ptr;

  // We are in thread W.

  // As discussed in Node::receive_wnd_updated(), send the ACK and then periodically re-send it until canceled.

  // Create an ACK with no packets acknowledged (so just a window update) and send it off.
  auto ack = Low_lvl_packet::create_uninit_packet<Ack_packet>(get_logger());
  ack->m_rcv_wnd = rcv_wnd;
  // Record that it was advertised!
  sock->m_rcv_last_sent_rcv_wnd = rcv_wnd;

  if (!async_sock_low_lvl_packet_send_or_close_immediately(sock,
                                                           Low_lvl_packet::ptr_cast(ack),
                                                           false))
  // ^-- defer_delta_check == false: for similar reason as in send_worker_check_state() calling send_worker().
  {
    return;
  }
  // else

  // Register one ACK packet we will send ASAP (and that it acknowledged no individual packets).
  sock->m_rcv_stats.sent_low_lvl_ack_packet(true);

  // ACK queued to send soon.  Now, as discussed, protect against it being lost by scheduling a timer.

  const Fine_duration fire_when_from_now = sock->opt(sock->m_opts.m_dyn_rcv_wnd_recovery_timer_period);

  FLOW_LOG_INFO("Setting timer to fire "
                "[" << round<milliseconds>(fire_when_from_now) << "] from now.");

  /* As usual, when scheduling a thing we can use the much simpler util::schedule_task_*() API; or the
   * full-featured boost.asio Timer.  We don't need the advanced features; so the only possible reason
   * to go with Timer would be the perf considerations (see schedule_task_from_now() doc header for discussion).
   * It is emphatically NOT the case that lots of these tasks are scheduled/fired/canceled per unit time;
   * e.g., we see it as rare enough to be OK with an INFO log message.  Hence no need to reuse a Timer repeatedly,
   * so use the simple API. */

  sock->m_rcv_wnd_recovery_scheduled_task
    = schedule_task_from_now(get_logger(), fire_when_from_now, true, &m_task_engine,
                             [this, sock_observer = weak_ptr<Peer_socket>(sock)](bool)
  {
     // We are in thread W.

    auto sock = sock_observer.lock();
    if (!sock)
    {
      return; // Possible or not, allow for this possibility for maintainability.
    }
    // else

    const Fine_duration since_recovery_started = Fine_clock::now() - sock->m_rcv_wnd_recovery_start_time;
    if (since_recovery_started > sock->opt(sock->m_opts.m_dyn_rcv_wnd_recovery_max_period))
    {
      // We've kept ACKing for a long time, and still no data.  Give up: it's all up to the sender now.

      // This is ~equally as rare as Receive buffer overflows, so this is worth an INFO message.
      FLOW_LOG_INFO('[' << sock << "]: still no new DATA arrived since last rcv_wnd advertisement; "
                    "Time since entering recovery [" << round<milliseconds>(since_recovery_started) << "] expired.  "
                    "Ending rcv_wnd recovery.");
      sock->m_rcv_in_rcv_wnd_recovery = false;

      // Record we ended in timeout.
      sock->m_rcv_stats.rcv_wnd_recovery_event_finish(false);

      return;
    }
    // else

    // Still in rcv_wnd recovery.  Send another unsolicited ACK (as in receive_wnd_updated()).

    // Re-grab available Receive buffer space.
    const size_t rcv_wnd = sock_rcv_wnd(sock);

    // This is ~equally as rare as Receive buffer overflows, so this is worth an INFO message.
    FLOW_LOG_INFO('[' << sock << "]: still no new DATA arrived since last rcv_wnd advertisement; "
                  "rcv_wnd = [" << sock->bytes_blocks_str(rcv_wnd) << "]; "
                  "time since entering recovery [" << round<milliseconds>(since_recovery_started) << "].  "
                  "Sending unsolicited rcv_wnd-advertising ACK and continuing rcv_wnd recovery.");

    async_rcv_wnd_recovery(sock, rcv_wnd);
  }); // on-scheduled-task-fired
} // Node::async_rcv_wnd_recovery()

void Node::receive_wnd_recovery_data_received(Peer_socket::Ptr sock)
{
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using util::scheduled_task_cancel;

  // We are in thread W.

  // We got some good DATA.  If we were sending unsolicited window update ACKs, we can now stop.

  if (!sock->m_rcv_in_rcv_wnd_recovery)
  {
    // We weren't.
    return;
  }
  // else

  // This is ~equally as rare as Receive buffer overflows, so this is worth an INFO message.
  FLOW_LOG_INFO('[' << sock << "]: Canceling rcv_wnd recovery; "
                "Time since entering recovery "
                "[" << round<milliseconds>(Fine_clock::now() - sock->m_rcv_wnd_recovery_start_time) << "].");

  sock->m_rcv_in_rcv_wnd_recovery = false;
#ifndef NDEBUG
  const bool canceled =
#endif
  scheduled_task_cancel(get_logger(), sock->m_rcv_wnd_recovery_scheduled_task);
  assert(canceled);

  // Record we ended in success.
  sock->m_rcv_stats.rcv_wnd_recovery_event_finish(true);
}

size_t Node::sock_rcv_wnd(Peer_socket::Const_ptr sock) const
{
  using std::numeric_limits;

  // We are in thread W.

  if (!sock->opt(sock->m_opts.m_st_rcv_flow_control_on))
  {
    /* Flow control disabled, so if we always advertise the same huge value, the other side will
     * never stop sending due to rcv_wnd.  On this side, we won't activate rcv_wnd recovery, because
     * the "last advertised" window will always equal the current window. */
    return numeric_limits<size_t>::max();
  }
  // else

  // Grab available Receive buffer space.  We have to momentarily lock sock due to access to sock->m_rcv_buf.
  size_t rcv_buf_size;
  {
    Peer_socket::Lock_guard lock;
    rcv_buf_size = sock->m_rcv_buf.data_size();
  }

  // Add the reassembly queue cumulative stored data size.  Why? See sock_data_to_reassembly_q_unless_overflow().
  if (sock->rexmit_on())
  {
    rcv_buf_size += sock->m_rcv_reassembly_q_data_size; // (At least one reason we must be in thread W.)
  }

  const size_t max_rcv_buf_size = sock->max_block_size_multiple(sock->m_opts.m_st_rcv_buf_max_size);

  return (max_rcv_buf_size > rcv_buf_size) ? (max_rcv_buf_size - rcv_buf_size) : 0;
}

void Node::receive_emptied_rcv_buf_while_disconnecting(Peer_socket::Ptr sock)
{
  // We are in thread W.

  /* As always, no need to lock m_state, etc., unless we plan to alter them, since no other thread can alter them.
   * ...On the other hand, we are going to be checking m_rcv_buf for emptiness below, and if it's not empty,
   * a user thread U != W may be altering it right now by consuming it.  So, lock.
   *
   * Could think about locking later in this function, but this is called so rarely I'd rather not have to
   * worry about whether it's OK to do that and just not. */
  Peer_socket::Lock_guard lock(sock->m_mutex);

  if (sock->m_state == Peer_socket::State::S_CLOSED)
  {
    /* When were placed onto thread W, state was S_OPEN+S_DISCONNECTING, but before boost.asio
     * could execute us, it executed another handler which already moved us to S_CLOSED for
     * whatever reason (there are many valid ones).  So just don't do anything, as we no longer
     * apply.  It's kind of interesting, so log INFO message. */
    FLOW_LOG_INFO('[' << sock << "] "
                  "was completely closed before asynchronous "
                  "receive_emptied_rcv_buf_while_disconnecting() could proceed.");
    return;
  }
  // else

  // Sanity-check (we cannot be called until there's a graceful close underway).
  assert((sock->m_state == Peer_socket::State::S_OPEN) &&
         (sock->m_open_sub_state == Peer_socket::Open_sub_state::S_DISCONNECTING));

  const Socket_id socket_id = Node::socket_id(sock);

  if (sock->m_int_state != Peer_socket::Int_state::S_CLOSED)
  {
    /* Yes, they emptied Receive buffer.  However, we haven't finished the graceful close.
     * Therefore -- even though one more barrier to reaching m_state == S_CLOSED has been removed --
     * there's nothing further to do at this time.  In fact, in certain situations we might even
     * get more data onto the Receive buffer!  @todo No graceful close yet. */
    FLOW_LOG_TRACE('[' << sock << "] "
                   "is gracefully closing, and Receive buffer is empty, but graceful close itself not yet finished.");
    return;
  }
  // else if (m_int_state == S_CLOSED)

  // Ensure Receive buffer is indeed still empty.  (Can still get data while gracefully closing.)
  if (!sock->m_rcv_buf.empty())
  {
    /* Some data arrived between the time we were placed on thread W and boost.asio executing us.
     * So we can't do anything; user has to receive() the stuff first, which should call us again. */
    FLOW_LOG_TRACE('[' << sock << "] "
                   "is gracefully closing, but Receive buffer has data again.");
    return;
  }
  // else if (m_int_state == S_CLOSED, and m_rcv_buf is empty)

  // Yes, the transport layer final handshake is finished.  Since Receive buffer now empty, no more barriers remain.
  FLOW_LOG_TRACE('[' << sock << "] "
                 "is gracefully closing, and Receive buffer is now empty.  Ready to permanently close.");
  close_connection_immediately(socket_id, sock,
                               Error_code(), /* err_code == success indicates clean close here. */
                               false);
  /* ^-- defer_delta_check == false: for similar reason as when calling send_worker() from
   * send_worker_check_state(). */
} // Node::receive_emptied_rcv_buf_while_disconnecting()

void Node::close_abruptly(Peer_socket::Ptr sock, Error_code* err_code)
{
  using boost::adopt_lock;
  using async::asio_exec_ctx_post;
  using async::Synchronicity;

  /* We are in user thread U != W.
   * It's important to keep that in mind in this method.  In particular, it is absolutely unsafe to
   * access m_int_state, which belongs solely to thread W and is never locked. */

  {
    /* WARNING!!!  sock->m_mutex is locked, but WE must unlock it before returning!  Can't leave that
     * to the caller, because we must unlock at a specific point below, right before post()ing
     * close_abruptly_worker() onto thread W.  Use a Lock_guard that adopts an
     * already-locked mutex. */
    Peer_socket::Lock_guard lock(sock->m_mutex, adopt_lock);

    if (!running())
    {
      FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
      return;
    }
    // else

    // Pre-condition.
    assert(sock->m_state == Peer_socket::State::S_OPEN);

    /* Put the rest of the work into thread W.  For justification, see big comment in listen().
     * Addendum regarding performance: close_abruptly() is probably called more frequently than
     * listen(), but I doubt the performance impact is serious even so.  send() and receive() might be
     * a different story. */

    // We're done -- must unlock so that thread W can do what it wants to with sock.
  } // lock

  // Load this onto thread W boost.asio work queue.  We don't return until it runs, so [&].
  asio_exec_ctx_post(get_logger(), &m_task_engine, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION, [&]()
  {
    // We are in thread W.  Thread U is waiting for us to do our stuff and return.

    /* Since we were placed onto thread W, another handler may have been executed before boost.asio
     * got to us.  Therefore we may already be S_CLOSED.  Detect this. */

    if (sock->m_state == Peer_socket::State::S_CLOSED) // No need to lock: only W can write to this.
    {
      // Yep, already closed.  sock->m_disconnect_cause is already set to closure reason.  Done.
      *err_code = sock->m_disconnect_cause;
      return;
    }
    // else

    /* Cool, we're not quite closed yet.  We could be connecting... or connected... or even in the
     * middle of graceful close (@todo that's not yet implemented).  Any of those situations allow
     * close_abruptly(), just as (indeed because of the fact that) any of those situations allow
     * close_connection_immediately() (..., error::...).
     *
     * Therefore simply do the following.  Pre-conditions hold: sock is in m_socks and is S_OPEN
     * (because not S_CLOSED); 3rd arg contains failure reason. */
    rst_and_close_connection_immediately(socket_id(sock), sock, error::Code::S_USER_CLOSED_ABRUPTLY, false);
    /* ^-- defer_delta_check == false: for similar reason as when calling send_worker() from
     * send_worker_check_state(). */

    // That set sock->m_disconnect_cause.  Closure successful.  Done.
    err_code->clear(); // Success.
  }); // asio_exec_ctx_post()
  // If got here, the task has completed in thread W and signaled us to that effect.
} // Node::close_abruptly()

void Node::close_connection_immediately(const Socket_id& socket_id, Peer_socket::Ptr sock,
                                        const Error_code& err_code, bool defer_delta_check)
{
  using boost::lexical_cast;
  using std::string;

  // We are in thread W.

  // @todo OK if a graceful close (S_OPEN+S_DISCONNECTING) is already in progress?  Below provides for it, but ensure.
  assert(sock->m_state == Peer_socket::State::S_OPEN);

  if (err_code)
  {
    FLOW_ERROR_LOG_ERROR(err_code);
    FLOW_LOG_INFO("Closing and destroying [" << sock << "] abruptly.");
  }
  else
  {
    // m_disconnect_cause has already been set and logged.
    FLOW_LOG_INFO("Closing and destroying [" << sock << "] after graceful close.");
  }
  // Log final state report.
  sock_log_detail(sock);

  /* Thread safety: we're in thread W, so no need to lock things by default (as most resources can
   * also only be accessed from thread W).  Exceptions are certain data members in Peer_socket
   * sock and Server_socket serv that may have originated it (if it was a passive open).  I will
   * comment on the locking situation for those data members as they come up in the code. */

  // First, set various state in *sock (including emptying Send and Receive buffers and setting m_node = 0).

  /* Save the final set of stats for Peer_socket::info(), as the source data will probably get
   * purged just below in sock_disconnect_*(). */
  sock_load_info_struct(sock, &sock->m_info_on_close);
  // We may have to massage it a little more, because some info is set below, by when it's too late.

  if (err_code)
  {
    // sock->m_disconnect_cause has not yet been set; so sock_load_info_struct() did not copy it properly yet.  Do so.
    sock->m_info_on_close.m_disconnect_cause = err_code;
    // Similarly:
    sock->m_info_on_close.m_int_state_str = lexical_cast<string>(Peer_socket::Int_state::S_CLOSED);

    /* This is an abrupt close.  This can be called in any situation once sock is in m_socks.  It's
     * our responsibility to move directly to transport layer state S_CLOSED and user state
     * S_CLOSED. */
    sock_set_int_state(sock, Peer_socket::Int_state::S_CLOSED); // Thread W access only; no need to lock.
    // Sets S_CLOSED public state (and related data, including m_disconnect_cause).  Locked inside.
    sock_disconnect_detected(sock, err_code, true);
  }
  else
  {
    /* We are in a graceful close and have reached the final stage of it (connection entirely
     * closed without having to abruptly close; buffers emptied gracefully by user and/or Node).
     * Therefore m_int_state is already S_CLOSED (method pre-condition), so
     * we just complete the user-visible state change. */

    assert(sock->m_int_state == Peer_socket::Int_state::S_CLOSED); // Thread W access only; no need to lock.
    sock_disconnect_completed(sock); // Sets S_CLOSED public state (and related data).  Locked inside.
  }

  // Next, remove sock from our main socket list.

#ifndef NDEBUG
  const auto erased = 1 ==
#endif
    m_socks.erase(socket_id);
  assert(erased); // S_OPEN => it's in m_socks.  Otherwise there's a serious bug somewhere.

  // Next, if this potentially is an unaccepted connection, delete it from the corresponding server socket.
  if (!sock->m_active_connect)
  {
    /* What is that Server_socket though?  Well, it's in sock->m_originating_serv... but that data
     * member can be accessed from a non-W thread, so we'd have to lock it.  But the mutex that
     * protects it in in *m_originating_serv itself!  So it's a chicked/egg problem.  However, we
     * can find that Server_socket (if it applies to sock) another way: through the port.  Its port
     * must be the same as local_port.  If such a Server_socket exists, cool; and if sock is
     * tracked inside it, cool.  Otherwise we needn't do anything. */
    Port_to_server_map::const_iterator port_to_server_it = m_servs.find(sock->m_local_port);
    if (port_to_server_it != m_servs.end()) // Server at same port number exists.  Not necessarily our guy though.
    {
      // If it is our guy, delete us from him.
      Server_socket::Ptr serv = port_to_server_it->second;
      serv_peer_socket_closed(serv, sock); // Thread-safe (in particular with respect to simultaneous serv->accept()).
    }
  }

  // sock now should not be (directly or indirectly) referenced in any Node data structures.

  // Cancel any timers.
  cancel_timers(sock);

  /* Return the port -- but only if it is an active open.  If it's a passive open the port is
   * still reserved for the server socket. */
  if (sock->m_active_connect)
  {
    Error_code return_err_code;
    m_ports.return_port(sock->m_local_port, &return_err_code);
    assert(!return_err_code);
  }

  /* sock has changed to CLOSED state.  Performing sock->receive() or sock->write() would therefore
   * certainly return an error.  Returning an error from those methods (as opposed to 0 but no
   * error) is considered Readable and Writable, respectively (as we want to alert the user to the
   * error, so her wait [if any] wakes up and notices the error).  Therefore we should soon inform
   * anyone waiting on any Event_sets for sock to become Readable or Writable.
   *
   * Caveat: Similar to that in Node::handle_syn_ack_ack_to_syn_rcvd() at similar point in the
   * code. */

  // Accumulate the event into the Node store (note: not any Event_set yet).
  const bool inserted_rd = m_sock_events[Event_set::Event_type::S_PEER_SOCKET_READABLE].insert(sock).second;
  const bool inserted_wr = m_sock_events[Event_set::Event_type::S_PEER_SOCKET_WRITABLE].insert(sock).second;
  if (inserted_rd || inserted_wr) // Must always perform both insert()s, hence the use of the 2 variables.
  {
    // Possibly inform the user for any applicable Event_sets right now.
    event_set_all_check_delta(defer_delta_check);
  }
} // Node::close_connection_immediately()

void Node::rst_and_close_connection_immediately(const Socket_id& socket_id, Peer_socket::Ptr sock,
                                                const Error_code& err_code, bool defer_delta_check)
{
  // We are in thread W.
  async_sock_low_lvl_rst_send(sock);
  close_connection_immediately(socket_id, sock, err_code, defer_delta_check);
}

Syn_packet::Ptr Node::create_syn(Peer_socket::Const_ptr sock)
{
  using util::Blob;

  auto syn = Low_lvl_packet::create_uninit_packet<Syn_packet>(get_logger());
  // Initial Sequence Number.
  syn->m_init_seq_num = sock->m_snd_init_seq_num;
  /* Send serialized version of arbitrary user data, which user can deserialize on the other side
   * after accepting connection.
   * Add const to express we require a copy, not move. */
  syn->m_serialized_metadata = static_cast<const Blob&>(sock->m_serialized_metadata);

  return syn;
}

Syn_ack_packet::Ptr Node::create_syn_ack(Peer_socket::Const_ptr sock)
{
  auto syn_ack = Low_lvl_packet::create_uninit_packet<Syn_ack_packet>(get_logger());
  // Initial Sequence Number (the start of our own series).
  syn_ack->m_init_seq_num = sock->m_snd_init_seq_num;
  // Random security token.
  syn_ack->m_packed.m_security_token = sock->m_security_token;
  // Advertise initial rcv_wnd.
  syn_ack->m_packed.m_rcv_wnd = sock->m_rcv_last_sent_rcv_wnd;

  return syn_ack;
}

bool Node::async_low_lvl_syn_ack_ack_send_or_close_immediately(const Peer_socket::Ptr& sock,
                                                               boost::shared_ptr<const Syn_ack_packet>& syn_ack)
{
  // Make a packet.
  auto syn_ack_ack = Low_lvl_packet::create_uninit_packet<Syn_ack_ack_packet>(get_logger());
  // No sequence number (not the initial SYN; not data).
  // Security token: give it back to them (they will verify).
  syn_ack_ack->m_packed.m_security_token = syn_ack->m_packed.m_security_token;
  // Initial receive window is probably the entire, ~empty Receive buffer.  Save the advertised rcv_wnd as promised.
  syn_ack_ack->m_packed.m_rcv_wnd = sock->m_rcv_last_sent_rcv_wnd = sock_rcv_wnd(sock);

  // Fill out common fields and asynchronously send packet.
  return async_sock_low_lvl_packet_send_or_close_immediately(sock,
                                                             Low_lvl_packet::ptr_cast(syn_ack_ack),
                                                             true); // Warns on error.
  // ^-- defer_delta_check == true: for similar reason as in handle_syn_ack_ack_to_syn_rcvd().
}

void Node::async_low_lvl_ack_send(Peer_socket::Ptr sock, bool defer_delta_check, const Error_code& sys_err_code)
{
  using boost::chrono::milliseconds;
  using boost::chrono::duration_cast;
  using std::make_pair;
  using std::vector;
  using std::numeric_limits;

  // We are in thread W.

  // Handle the timer-related corner cases (if we were invoked by m_rcv_delayed_ack_timer triggering).

  // For brevity and speed:
  vector<Peer_socket::Individual_ack::Ptr>& pending_acks = sock->m_rcv_pending_acks;

  if (sys_err_code == boost::asio::error::operation_aborted)
  {
    FLOW_LOG_TRACE("Delayed [ACK] timer [" << sock << "] canceled; "
                   "pending acknowledgment count [" << pending_acks.size() << "].");
    return;
  }
  // else

  FLOW_LOG_TRACE("Delayed [ACK] timer [" << sock << "] triggered, or ACK forced; "
                 "pending acknowledgment count [" << pending_acks.size() << "].");

  if (sys_err_code)
  {
    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log non-portable error.
    // Nothing else to do here.  We don't know what this means.  So just treat it as if timer was triggered.
  }

  if (sock->m_int_state != Peer_socket::Int_state::S_ESTABLISHED)
  {
    /* This is unlikely but legitimate.  (Can happen if, by the time the handler that advanced state
     * from ESTABLISHED to another state started, this timer also was triggered and thus queued the
     * current handler inside m_task_engine.) */
    FLOW_LOG_TRACE("Delayed [ACK] timer [" << sock << "] triggered, "
                   "but socket already in inapplicable state [" << sock->m_int_state << "].  Ignoring.");
    return;
  }
  // else

  if (pending_acks.empty())
  {
    /* This is probably a bug if we're here.  However, assert() or connection closure seems a bit
     * drastic... carry on. */
    FLOW_LOG_WARNING("Delayed [ACK] timer [" << sock << "] triggered, "
                     "but socket has no pending acknowledgments.  This is likely an internal bug.  Ignoring.");
    return;
  }
  // else

  /* OK, let's do it.  Basically just shove all the acknowledgments into an ACK packet.  Namely, for
   * each one, shove the starting sequence number and the amount of time since we first received it
   * (so the other side can subtract that to compute RTT, if it wants).
   *
   * However we may run out of space and need more ACKs.  To keep track of how much space we've
   * used, compute an estimate for serializing those two pieces of data and keep adding that for
   * each acknowledgment handled.  The budget is given by max-block-size; a DATA packet is allowed
   * that much payload on top of the normal header stuff, so that should be good enough for us too.
   * There's probably some constant overhead on top of that, but it's close enough.
   *
   * ACK is also used as an opportunistic way to send rcv_wnd to the other side, which informs
   * them of how much more data we can take at this time.  Naively we should just have rcv_wnd =
   * the max buffer size minus the buffer space currently taken, and that is the most accurate
   * thing.  However RFC 793 ("Window Management Suggestions") and probably other literature
   * suggest to (when the available space is increasing) advertise the window in larger steps (so
   * withhold the higher rcv_wnd value until it increases even further up to some threshold).  For
   * now I forego such fanciness.  See also the rcv_wnd-related comment in
   * Node::receive_wnd_increased() for further reasoning on rcv_wnd (namely surrounding the fact
   * that sometimes we must send ACKs with no packets acknowledged to ensure a connection does not
   * stall due to a zero rcv_wnd). */

  // Grab available Receive buffer space.  Save it for later comparison.
  const size_t& rcv_wnd = sock->m_rcv_last_sent_rcv_wnd = sock_rcv_wnd(sock);

  auto ack = Low_lvl_packet::create_uninit_packet<Ack_packet>(get_logger());
  ack->m_rcv_wnd = rcv_wnd; // Advertise receive window.  @todo Code reuse?

  const size_t max_block_size = sock->max_block_size();
  size_t size_est_inc
    = sizeof(Ack_packet::ack_delay_t) + sizeof(Sequence_number::seq_num_t);
  if (sock->rexmit_on())
  {
    size_est_inc += sizeof(Low_lvl_packet::rexmit_id_t);
  }
  assert(size_est_inc <= max_block_size); // At least one has to fit.

  const Fine_time_pt time_now = Fine_clock::now();
  size_t size_est_so_far = sizeof(Low_lvl_packet::rcv_wnd_t); // How many raw bytes we have, approximately, used.
  for (Peer_socket::Individual_ack::Const_ptr ind_ack : pending_acks)
  {
    if (size_est_so_far + size_est_inc > max_block_size)
    {
      // Too big.  Send off what we have.
      if (!async_sock_low_lvl_packet_send_or_close_immediately(sock,
                                                               Low_lvl_packet::ptr_cast(ack),
                                                               defer_delta_check))
      {
        return;
      }
      // else

      // Register one ACK packet we will send ASAP.
      sock->m_rcv_stats.sent_low_lvl_ack_packet(false);

      // As async_sock_low_lvl_packet_send_paced() says, we cannot reuse ack's pointed-to-object.  Make new one.
      ack = Low_lvl_packet::create_uninit_packet<Ack_packet>(get_logger());
      ack->m_rcv_wnd = rcv_wnd; // Advertise receive window.  @todo Code reuse?

      size_est_so_far = sizeof(Low_lvl_packet::rcv_wnd_t);
    }

    // Add the acknowledgment to the current ACK.

    // First sequence number in packet.
    const Sequence_number& seq_num = ind_ack->m_seq_num;

    // ACK delay for this individual acknowledgment.  Compute it; then validate it.

    /* @todo In low_lvl_io, we perform packet pacing but currently choose to assign a value of
     * 0 bytes to an ACK.  That is, while we do preserve the order of DATA and ACK packets -- if
     * both happen to be in the outgoing stream -- we do not delay the sending of the ACK once it is
     * the next packet to be sent out.  However, even so, an ACK's sending may be delayed by the
     * pacing applied to DATA packets intermixed with it.  Therefore the ACK delay measurement we
     * take here may be incorrect (too low) in that case.  This can cause overestimated RTTs on the
     * sender's side.  The to-do is to correct the ACK delay value in a given ACK by adding the
     * pacing delay (if any) of the ACK to the individual ACK delays within it.  Conceptually this
     * is similar to the sent_when value being set when choosing to send a DATA packet and then
     * corrected in the pacing module later.
     *
     * This to-do is not important until we in practice start mixing sending and receiving at the
     * application layer... but still -- it's worth knowing that there is a design bug here. */

    // Shouldn't be negative.
    Fine_duration delay = time_now - ind_ack->m_received_when;
    if (delay.count() < 0)
    {
      /* This is pretty crazy and should not happen according to the documented properties of
       * Fine_clock.  No need to crash or disconnect though, so do our best.... */
      FLOW_LOG_WARNING("Delayed [ACK] timer [" << sock << "] triggered; "
                       "delay for packet [" << seq_num << ", ...) is "
                       "negative: [" << delay << "]; using zero.");
      delay = Fine_duration::zero();
    }

    /* Convert whatever resolution Fine_clock uses to milliseconds because we want to keep that
     * field of the ACK sized according to how the low-level packet handling code prefers it for
     * efficiency.  Overflow is possible.  Use duration_cast (truncation) instead of rounding,
     * because in very low-latency situations the extra microseconds rounding up can cause a
     * negative RTT calculation on the other side (when this ACK is received).  The ACK handling
     * code will just clamp the value at zero on the other side, but let's try to avoid it anyway
     * on this side.
     *
     * @todo This comment appears to be outdated, as Ack_delay_time_unit is just Fine_duration.
     * Look into this. */
    Ack_packet::Ack_delay_time_unit pkt_delay = duration_cast<Ack_packet::Ack_delay_time_unit>(delay);
    const Ack_packet::ack_delay_t MAX_DELAY_VALUE = numeric_limits<Ack_packet::ack_delay_t>::max();
    if (uint64_t(pkt_delay.count()) > uint64_t(MAX_DELAY_VALUE))
    {
      /* This is pretty crazy though not 100% impossible if the CPU is really loaded, or some other
       * shenanigans.  So do our best.... */
      FLOW_LOG_WARNING("Delayed [ACK] timer [" << sock << "] triggered; "
                       "delay for packet [" << seq_num << ", ...) is [" << pkt_delay << "]; overflow; "
                       "using max value [" << MAX_DELAY_VALUE << "] units.");
      // @todo Maybe there's a more sane ceiling value than the absolute maximum?
      pkt_delay = Ack_packet::Ack_delay_time_unit(MAX_DELAY_VALUE);
    }

    // Finally write the individual acknowledgment.
    if (sock->rexmit_on())
    {
      ack->m_rcv_acked_packets_rexmit_on_out.push_back
        (Ack_packet::Individual_ack_rexmit_on(seq_num,
                                              ind_ack->m_rexmit_id,
                                              Ack_packet::ack_delay_t(pkt_delay.count())));
    }
    else
    {
      ack->m_rcv_acked_packets_rexmit_off_out.push_back
        (Ack_packet::Individual_ack_rexmit_off(seq_num,
                                               Ack_packet::ack_delay_t(pkt_delay.count())));
    }
    size_est_so_far += size_est_inc;

    // Register one packet of unknown size that we've packaged into an ACK and will send ASAP.
    sock->m_rcv_stats.sent_individual_ack();
  } // for (ind_ack : pending_acks)

  // Don't forget the last non-full ACK, if any.
  if ((size_est_so_far != 0)
      && (!async_sock_low_lvl_packet_send_or_close_immediately(sock,
                                                               Low_lvl_packet::ptr_cast(ack),
                                                               defer_delta_check)))
  {
    return;
  }

  // Register one ACK packet we will send ASAP.
  sock->m_rcv_stats.sent_low_lvl_ack_packet(false);

  // All serialized to be sent; the timer can start again when a packet must be acknowledged.
  pending_acks.clear();

  // Register that now there are 0 pending individual acks.
  sock->m_rcv_stats.current_pending_to_ack_packets(0);

  // Note that all the ACKs are sent off outside this handler and only once UDP is ready.
} // Node::async_low_lvl_ack_send()

Node::Socket_id Node::socket_id(Peer_socket::Const_ptr sock) // Static.
{
  // We are in thread W.
  return Socket_id{ sock->remote_endpoint(), sock->local_port() };
}

bool Node::snd_deqable(Peer_socket::Const_ptr sock) const
{
  // There is stuff to send if there is anything to retransmit or at least new user data.
  return !(sock->m_snd_rexmit_q.empty() && sock->m_snd_buf.empty());
}

bool Node::snd_buf_enqable(Peer_socket::Const_ptr sock) const
{
  // See doc comment for rationale for keeping this in a function.

  /* Since 1 block can be at most max-block-size, if that much space is free, then definitely one
   * can enqueue onto m_snd_buf.  Note that if less than max-block-size space is free, it would
   * still be possible to enqueue a smaller block; yet we still return false.  We are intentionally
   * conservative, because we are guaranteeing ANY one enqueueing will work.  More importantly, this
   * guarantees our Socket_buffer scheme (see class doc header) to guarantee constant-time
   * dequeueing will work.
   *
   * We're not overly conservative, either; i.e., no one is likely to complain this policy is too
   * stingy. */
  return sock->m_snd_buf.data_size() + sock->max_block_size()
           <= sock->opt(sock->m_opts.m_st_snd_buf_max_size);
}

bool Node::rcv_buf_deqable(Peer_socket::Const_ptr sock) const
{
  // See doc comment for rationale for keeping this in a function.
  return !sock->m_rcv_buf.empty();
}

void Node::sock_set_int_state(Peer_socket::Ptr sock, Peer_socket::Int_state new_state)
{
  // We are in thread W.

  FLOW_LOG_TRACE('[' << sock << "] changing state from [" <<
                 sock->m_int_state << "] to [" << new_state << "].");
  sock->m_int_state = new_state;
}

void Node::sock_set_state(Peer_socket::Ptr sock, Peer_socket::State state, Peer_socket::Open_sub_state open_sub_state)
{
  Peer_socket::Lock_guard lock(sock->m_mutex);

  // @todo Add TRACE logging.

  sock->m_state = state;
  if (state == Peer_socket::State::S_OPEN)
  {
    sock->m_open_sub_state = open_sub_state;
  }
  else // (state == Peer_socket::State::S_CLOSED)
  {
    /* Important convention: S_CLOSED means socket is permanently incapable of sending or
     * receiving more data.  At this point the originating Node removes the socket from its internal
     * structures.  Therefore, the Node itself may even go away -- while this Peer_socket still
     * exists.  Since we use shared_ptr when giving our socket objects, that's fine -- but we want to
     * avoid returning an invalid Node* in node().  So, when S_CLOSED, sock->m_node = 0. */
    sock->m_node = 0;
  }
}

void Node::sock_disconnect_detected(Peer_socket::Ptr sock, const Error_code& disconnect_cause, bool close)
{
  Peer_socket::Lock_guard lock(sock->m_mutex);

  sock->m_disconnect_cause = disconnect_cause;

  if (close)
  {
    // DONE.
    sock_set_state(sock, Peer_socket::State::S_CLOSED); // Reentrant mutex => OK.
    sock_free_memory(sock);
  }
  else
  {
    // This socket is screwed, but let user get any remaining buffer data out.

    // Reentrant mutex => OK:
    sock_set_state(sock, Peer_socket::State::S_OPEN, Peer_socket::Open_sub_state::S_DISCONNECTING);
  }
}

void Node::sock_disconnect_completed(Peer_socket::Ptr sock)
{
  Peer_socket::Lock_guard lock(sock->m_mutex);

  // Sanity-check pre-conditions.  (Basically ensure disconnect_detected(err_code, false) was previously called.)
  assert(sock->m_disconnect_cause);
  assert((sock->m_state == Peer_socket::State::S_OPEN)
         && (sock->m_open_sub_state == Peer_socket::Open_sub_state::S_DISCONNECTING));

  sock_set_state(sock, Peer_socket::State::S_CLOSED); // Reentrant mutex => OK.
  sock_free_memory(sock);
}

void Node::sock_free_memory(Peer_socket::Ptr sock)
{
  sock->m_rcv_buf.clear();
  sock->m_snd_buf.clear();
  sock->m_rcv_packets_with_gaps.clear();
  sock->m_rcv_reassembly_q_data_size = 0;
  sock->m_snd_flying_pkts_by_sent_when.clear();
  sock->m_snd_flying_pkts_by_seq_num.clear();
  sock->m_snd_rexmit_q.clear();
  sock->m_serialized_metadata.make_zero(); // clear() does not deallocate, but this does.
  sock->m_rcv_syn_rcvd_data_q.clear();
  sock->m_rcv_pending_acks.clear();
  sock->m_rcv_acked_packets.clear();
  sock->m_snd_pacing_data.m_packet_q.clear();

  /* Destroy memory stored in m_snd_cong_ctl which may be non-O(1).  This is a little questionable;
   * maybe should leave it to destructor?  However since we store it as a pointer and are to free
   * any "significant" memory, and this may be significant, we may as well just delete it. */
  sock->m_snd_cong_ctl.reset();
  // Same deal.
  sock->m_snd_bandwidth_estimator.reset();
}

bool Node::sock_set_options(Peer_socket::Ptr sock, const Peer_socket_options& opts, Error_code* err_code)
{
  // We are in thread U != W.

  if (!running())
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_NODE_NOT_RUNNING);
    return false;
  }
  // else

  /* We just want to replace m_opts with a copy of opts.  First validate opts (including with
   * respect to m_opts, and also check for invalid values and such), then copy it over. */

  // Log new options values.  A bit computationally expensive so just use TRACE for now.  @todo Reconsider?
  FLOW_LOG_TRACE("For [" << sock << "]:\n\n" << opts);

  // Will be writing sock->m_opts if all goes well, so must acquire exclusive ownership of m_opts.
  Peer_socket::Options_lock lock(sock->m_opts_mutex);

  /* Validate the new option set (including ensuring they're not changing static options' values).
   * Note that an explicit pre-condition of this method is that m_opts_mutex is locked if needed,
   * hence the above locking statement is not below this call. */
  if (!sock_validate_options(opts, &sock->m_opts, err_code))
  {
    return false;
  }
  // else

  // Boo-ya.
  sock->m_opts = opts;
  return true;
} // Node::sock_set_options()

/// @cond
/* -^- Doxygen, please ignore the following.  (Don't want docs generated for temp macro; this is more maintainable
 * than specifying the macro name to omit it, in Doxygen-config EXCLUDE_SYMBOLS.) */

/* Normaly I try to avoid macro cleverness, but in this case to get a nice printout we need the
 * # technique, and also this eliminates quite a bit of repetition.  So let's.... */
#define VALIDATE_STATIC_OPTION(ARG_opt) \
  validate_static_option(opts.ARG_opt, prev_opts->ARG_opt, #ARG_opt, err_code)
#define VALIDATE_CHECK(ARG_check) \
  validate_option_check(ARG_check, #ARG_check, err_code)

// -v- Doxygen, please stop ignoring.
/// @endcond

bool Node::sock_validate_options(const Peer_socket_options& opts,
                                 const Peer_socket_options* prev_opts,
                                 Error_code* err_code) const
{
  /* We are to validate the given set of per-socket option values.  If prev_opts, then the context
   * is that an already-existing socket (with already-set options) is being called with
   * set_options(), i.e. user is modifying options for an existing socket.  In that case we must
   * ensure that no static (unchangeable) option's value would be changed by this.
   *
   * If not prev_opts, then the per-socket options within the global per-Node Node_options object
   * are being changed.  Per-socket options in that context are always dynamic, since if they were
   * static, there'd be no point in making the per-socket in the first place.  So in that case that
   * static option check is to be skipped.
   *
   * Finally, we must check for individual integrity of the specified values (including consistency
   * with other option values). */

  using boost::chrono::seconds;
  using std::numeric_limits;

  // We are in thread U != W or in thread W.

  if (prev_opts)
  {
    /* As explained above, they're trying to change an existing socket's option values.  Ensure
     * all the static options' values are the same in opts and prev_opts. */

    // Explicitly documented pre-condition is that *prev_opts is already locked if necessary.  So don't lock.

    const bool static_ok
      = VALIDATE_STATIC_OPTION(m_st_max_block_size) &&
        VALIDATE_STATIC_OPTION(m_st_connect_retransmit_period) &&
        VALIDATE_STATIC_OPTION(m_st_connect_retransmit_timeout) &&
        VALIDATE_STATIC_OPTION(m_st_snd_buf_max_size) &&
        VALIDATE_STATIC_OPTION(m_st_rcv_buf_max_size) &&
        VALIDATE_STATIC_OPTION(m_st_rcv_flow_control_on) &&
        VALIDATE_STATIC_OPTION(m_st_rcv_buf_max_size_slack_percent) &&
        VALIDATE_STATIC_OPTION(m_st_rcv_buf_max_size_to_advertise_percent) &&
        VALIDATE_STATIC_OPTION(m_st_rcv_max_packets_after_unrecvd_packet_ratio_percent) &&
        VALIDATE_STATIC_OPTION(m_st_delayed_ack_timer_period) &&
        VALIDATE_STATIC_OPTION(m_st_max_full_blocks_before_ack_send) &&
        VALIDATE_STATIC_OPTION(m_st_rexmit_on) &&
        VALIDATE_STATIC_OPTION(m_st_max_rexmissions_per_packet) &&
        VALIDATE_STATIC_OPTION(m_st_init_drop_timeout) &&
        VALIDATE_STATIC_OPTION(m_st_snd_pacing_enabled) &&
        VALIDATE_STATIC_OPTION(m_st_snd_bandwidth_est_sample_period_floor) &&
        VALIDATE_STATIC_OPTION(m_st_cong_ctl_strategy) &&
        VALIDATE_STATIC_OPTION(m_st_cong_ctl_init_cong_wnd_blocks) &&
        VALIDATE_STATIC_OPTION(m_st_cong_ctl_max_cong_wnd_blocks) &&
        VALIDATE_STATIC_OPTION(m_st_cong_ctl_cong_wnd_on_drop_timeout_blocks) &&
        VALIDATE_STATIC_OPTION(m_st_cong_ctl_classic_wnd_decay_percent) &&
        VALIDATE_STATIC_OPTION(m_st_drop_packet_exactly_after_drop_timeout) &&
        VALIDATE_STATIC_OPTION(m_st_drop_all_on_drop_timeout) &&
        VALIDATE_STATIC_OPTION(m_st_out_of_order_ack_restarts_drop_timer);

    if (!static_ok)
    {
      // validate_static_option() has set *err_code.
      return false;
    }
    // else
  } // if (prev_opts)

  // Now sanity-check the values themselves.  @todo Comment and reconsider these?
  const bool checks_ok
    = VALIDATE_CHECK(opts.m_st_max_block_size >= 512) &&
      VALIDATE_CHECK(opts.m_st_connect_retransmit_period.count() > 0) &&
      VALIDATE_CHECK(opts.m_st_connect_retransmit_timeout.count() > 0) &&
      VALIDATE_CHECK(opts.m_st_snd_buf_max_size >= 4 * opts.m_st_max_block_size) &&
      VALIDATE_CHECK(opts.m_st_rcv_buf_max_size >= 4 * opts.m_st_max_block_size) &&
      VALIDATE_CHECK(util::in_open_closed_range(0u, opts.m_st_rcv_buf_max_size_to_advertise_percent, 100u)) &&
      VALIDATE_CHECK(opts.m_st_rcv_max_packets_after_unrecvd_packet_ratio_percent >= 100) &&
      VALIDATE_CHECK(opts.m_st_delayed_ack_timer_period <= seconds(1)) &&
      VALIDATE_CHECK(util::in_closed_range(Fine_duration::zero(),
                                           opts.m_st_delayed_ack_timer_period,
                                           Fine_duration(seconds(1)))) &&
      VALIDATE_CHECK(opts.m_st_max_full_blocks_before_ack_send >= 1) &&
      VALIDATE_CHECK(opts.m_st_max_rexmissions_per_packet >= 1) &&
      VALIDATE_CHECK(opts.m_st_max_rexmissions_per_packet <= numeric_limits<Low_lvl_packet::rexmit_id_t>::max());
      VALIDATE_CHECK(opts.m_st_init_drop_timeout.count() > 0) &&
      VALIDATE_CHECK(opts.m_st_snd_bandwidth_est_sample_period_floor.count() > 0) &&
      VALIDATE_CHECK(opts.m_st_cong_ctl_init_cong_wnd_blocks <= opts.m_st_cong_ctl_max_cong_wnd_blocks) &&
      VALIDATE_CHECK
        (4 * opts.m_st_cong_ctl_max_cong_wnd_blocks * opts.m_st_max_block_size <= opts.m_st_rcv_buf_max_size) &&
      VALIDATE_CHECK(opts.m_st_cong_ctl_cong_avoidance_increment_blocks < 20) &&
      VALIDATE_CHECK(opts.m_st_cong_ctl_classic_wnd_decay_percent <= 100) &&
      VALIDATE_CHECK(util::in_closed_range<size_t>(1, opts.m_st_cong_ctl_cong_wnd_on_drop_timeout_blocks, 10)) &&
      VALIDATE_CHECK(opts.m_dyn_drop_timeout_ceiling > 4 * opts.m_st_init_drop_timeout) &&
      VALIDATE_CHECK(opts.m_dyn_drop_timeout_backoff_factor >= 1) &&
      VALIDATE_CHECK(opts.m_dyn_rcv_wnd_recovery_timer_period.count() > 0);

  // On error, validate_option_check() has set *err_code.

  return checks_ok;

#undef VALIDATE_CHECK
#undef VALIDATE_STATIC_OPTION
} // Node::sock_validate_options()

Peer_socket_info Node::sock_info(Peer_socket::Const_ptr sock)
{
  using async::asio_exec_ctx_post;
  using async::Synchronicity;
  using boost::adopt_lock;

  // We are in thread U != W.

  Peer_socket_info stats;
  {
    /* WARNING!!!  sock->m_mutex is locked, but WE must unlock it before returning!  Can't leave that
     * to the caller, because we must unlock at a specific point below, right before post()ing
     * sock_info_worker() onto thread W.  Use a Lock_guard that adopts an already-locked mutex. */
    Peer_socket::Lock_guard lock(sock->m_mutex, adopt_lock);

    if (!running())
    {
      /* This is kind of a weird case, in that sock's Node having stopped running is a problem, but
       * in this case they just want the socket stats.  The only reason we're in this method --
       * calling sock->info() did not simply return the stats itself -- is that there was a danger
       * thread W might change the stats, while we'd be copying them.  Well, if !running() there is no
       * danger of that.  So we can just: */
      sock_load_info_struct(sock, &stats);
      return stats;
    }
    // else

    /* Okay -- Node is running and may change stats's source info at any time.  Therefore, since we
     * do not have a mutex for all that source info, we place a task on W and set up a future as a
     * way for it to inform us it's done.  This has a certain performance penalty, but that's better
     * than having to lock each time we need to modify this source data throughout W's operations.
     * Moreover we warned about the performance penalty in the doc header for Peer_socket::info(). */

    // We're done -- must unlock so that thread W can do what it wants to with sock.
  } // lock

  // Load this onto thread W boost.asio work queue.  We don't return until it's done, so [&] is OK.
  asio_exec_ctx_post(get_logger(), &m_task_engine, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION,
                     [&]() { sock_load_info_struct(sock, &stats); });
  // If got here, the task has completed in thread W and signaled us to that effect.

  return stats;
} // Node::sock_info()

void Node::sock_load_info_struct(Peer_socket::Const_ptr sock, Peer_socket_info* stats) const
{
  using boost::lexical_cast;
  using std::string;

  // We are in thread W.

  stats->m_rcv = sock->m_rcv_stats.stats();
  stats->m_snd = sock->m_snd_stats.stats();

  // @todo This is more suitable for the non-existent Node_info and Node::load_info_struct().  (It's not per-socket.)
  stats->m_low_lvl_max_buf_size = m_low_lvl_max_buf_size;

  stats->m_int_state_str = lexical_cast<string>(sock->m_int_state);
  stats->m_is_active_connect = sock->m_active_connect;
  // No need to lock: no thread but W can write to it.
  stats->m_disconnect_cause = sock->m_disconnect_cause;

  {
    // Gotta lock, as Receive and Send buffers can be modified at any time by thread U at least.
    Peer_socket::Lock_guard lock(sock->m_mutex);
    stats->m_rcv_buf_size = sock->m_rcv_buf.data_size();
    stats->m_snd_buf_size = sock->m_snd_buf.data_size();
  }

  stats->m_rcv_wnd = sock_rcv_wnd(sock);
  stats->m_rcv_wnd_last_advertised = sock->m_rcv_last_sent_rcv_wnd;
  stats->m_rcv_reassembly_q_data_size = sock->m_rcv_reassembly_q_data_size;
  stats->m_rcv_packets_with_gaps = sock->m_rcv_packets_with_gaps.size();
  stats->m_rcv_syn_rcvd_data_cumulative_size
    = sock->m_rcv_syn_rcvd_data_q.empty() ? 0 : sock->m_rcv_syn_rcvd_data_cumulative_size;
  stats->m_rcv_syn_rcvd_data_q_size = sock->m_rcv_syn_rcvd_data_q.size();

  stats->m_snd_rcv_wnd = sock->m_snd_remote_rcv_wnd;
  stats->m_snd_cong_ctl_in_flight_bytes = sock->m_snd_flying_bytes;
  stats->m_snd_cong_ctl_in_flight_count = sock->m_snd_flying_pkts_by_sent_when.size();
  stats->m_snd_cong_ctl_wnd_bytes = sock->m_snd_cong_ctl->congestion_window_bytes();
  stats->m_snd_cong_ctl_wnd_count_approx = stats->m_snd_cong_ctl_wnd_bytes / sock->max_block_size();
  stats->m_snd_smoothed_round_trip_time = sock->m_snd_smoothed_round_trip_time;
  stats->m_snd_round_trip_time_variance = sock->m_round_trip_time_variance;
  stats->m_snd_drop_timeout = sock->m_snd_drop_timeout;
  stats->m_snd_pacing_packet_q_size = sock->m_snd_pacing_data.m_packet_q.size();
  stats->m_snd_pacing_bytes_allowed_this_slice = sock->m_snd_pacing_data.m_bytes_allowed_this_slice;
  stats->m_snd_pacing_slice_start = sock->m_snd_pacing_data.m_slice_start;
  stats->m_snd_pacing_slice_period = sock->m_snd_pacing_data.m_slice_period;
  stats->m_snd_est_bandwidth_mbit_per_sec
    = util::to_mbit_per_sec<Send_bandwidth_estimator::Time_unit>
        (sock->m_snd_bandwidth_estimator->bandwidth_bytes_per_time());

  stats->m_sock_opts = sock->opt(sock->m_opts); // Lock and copy... probably not the fastest thing ever....
  stats->m_node_opts = opt(m_opts); // Ditto.
}

void Node::sock_log_detail(Peer_socket::Const_ptr sock) const
{
  // We are in thread W.

  /* We are to log details about the given socket.  Since the idea is that this would be called on
   * the order of at most once or twice a second, we can be as verbose as we think is useful without
   * (too much) concern for performance. */

  Peer_socket_info stats;
  sock_load_info_struct(sock, &stats); // This involves some copying, but, again, we are not too concerned with speed.

  FLOW_LOG_INFO("[=== Socket state for [" << sock << "]. ===\n" << stats);

  // Log receive and send windows details.  Force the logging of the most verbose possible amount of info.
  log_snd_window(sock, true);
  log_rcv_window(sock, true);
  // @todo Should this be inside Peer_socket_info also?

  FLOW_LOG_INFO("=== Socket state for [" << sock << "]. ===]");
} // Node::sock_log_detail()

void Node::advance_seq_num(Sequence_number* seq_num, boost::shared_ptr<const Data_packet> data) // Static.
{
  /* We just need to increment *seq_num, which points to the start of the data in `data`,
   * to a value that points to the data just past the end of the data in `data`.  Why is this in a
   * separate method?  Answer: We may want to change the mapping from sequence number to byte of data.  In
   * particular the mapping can be one-to-one, as in TCP.  Or it can be one sequence number to all bytes in a
   * particular packet, which I've seen in certain lesser known custom protocols.  This allows us to
   * (hopefully) change the code in one place. */

  advance_seq_num(seq_num, data->m_data.size());
} // Node::advance_seq_num()

void Node::advance_seq_num(Sequence_number* seq_num, size_t data_size)
{
  /* For now go with TCP's convention (one byte to one sequence number, no gaps).  While we deal
   * with blocks, instead of streams, this may complicate the math a bit and use more sequence
   * number space (faster wrapping).  However, it would make it easier to adapt the algorithms
   * when we move to byte streams; and we currently use a sequence number so large that wrapping
   * is impossible.  Update: we have moved to streams. */
  *seq_num += data_size;
}

template<typename Packet_map_iter>
void Node::get_seq_num_range(const Packet_map_iter& packet_it,
                             Sequence_number* seq_num_start, Sequence_number* seq_num_end) // Static.
{
  const Sequence_number& seq_num_start_cref = packet_it->first;
  if (seq_num_start)
  {
    *seq_num_start = seq_num_start_cref;
  }
  if (seq_num_end)
  {
    *seq_num_end = seq_num_start_cref;
    advance_seq_num(seq_num_end, packet_it->second->m_size);
  }
}

Peer_socket::order_num_t Node::sock_get_new_snd_order_num(Peer_socket::Ptr sock) // Static.
{
  // Since m_snd_last_order_num starts at 0, this ensures 0 is reserved, as advertised.
  return ++sock->m_snd_last_order_num;
}

Peer_socket* Node::sock_create(const Peer_socket_options& opts) // Virtual.
{
  // Just make a regular net_flow::Peer_socket.
  return sock_create_forward_plus_ctor_args<Peer_socket>(opts);
}

// Free implementations.

std::ostream& operator<<(std::ostream& os, const Peer_socket* sock)
{
  return
    sock
      ? (os
         << "NetFlow_socket "
         << "[" << sock->remote_endpoint() << "]<=>[NetFlow [:" << sock->local_port() << "]] "
         "@" << static_cast<const void*>(sock))
      : (os << "NetFlow_socket@null");
}

/// @cond
/* -^- Doxygen, please ignore the following.  (Don't want docs generated for temp macro; this is more maintainable
 * than specifying the macro name to omit it, in Doxygen-config EXCLUDE_SYMBOLS.) */

// That's right, I did this.  Wanna fight about it?
#define STATE_TO_CASE_STATEMENT(ARG_state) \
  case Peer_socket::Int_state::S_##ARG_state: \
    return os << #ARG_state

// -v- Doxygen, please stop ignoring.
/// @endcond

std::ostream& operator<<(std::ostream& os, Peer_socket::Int_state state)
{
  switch (state)
  {
    STATE_TO_CASE_STATEMENT(CLOSED);
    STATE_TO_CASE_STATEMENT(SYN_SENT);
    STATE_TO_CASE_STATEMENT(SYN_RCVD);
    STATE_TO_CASE_STATEMENT(ESTABLISHED);
  }
  return os;
#undef STATE_TO_CASE_STATEMENT
}

} // namespace flow::net_flow
