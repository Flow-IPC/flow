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
#include "flow/net_flow/detail/drop_timer.hpp"
#include "flow/net_flow/detail/low_lvl_io.hpp"
#include "flow/net_flow/detail/cong_ctl.hpp"
#include "flow/util/sched_task.hpp"
#include <utility>

namespace flow::net_flow
{

// Implementations.

void Node::async_low_lvl_recv()
{
  // We are in thread W.
  m_low_lvl_sock.async_wait(Udp_socket::wait_read, [this](const Error_code& sys_err_code)
  {
    low_lvl_recv_and_handle(sys_err_code);
  });
}

void Node::low_lvl_recv_and_handle(Error_code sys_err_code)
{
  using util::Blob;
  using boost::asio::buffer;

  // We are in thread W.

  // Number of packets received and thus handled by handle_incoming().  Useful at least for a log message at the end.
  unsigned int handled_packet_count = 0;
  /* Number of packets received.  (The number handled by handle_incoming() may be lower or higher
   * if simulator is in use. */
  unsigned int recvd_packet_count = 0;

  /* The limit on the # of received packets to handle in this handler.  0 means unlimited. For
   * reasoning as to why we'd possibly want to limit it, see doc header for this option. */
  unsigned int recvd_packet_count_limit = opt(m_opts.m_dyn_max_packets_per_main_loop_iteration);

  if (!sys_err_code)
  {
    /* boost.asio has reason to believe there's at least one UDP datagram ready to read from the
     * UDP net-stack's buffer.  We'll read that one.  We'll also keep reading them until UDP net-stack
     * says it "would block" (i.e., no more datagrams have been received).  Why not simply call
     * async_low_lvl_recv() and let boost.asio deal with it and call us again?  Consider a user
     * waiting for Readable for a given Peer_socket.   If did that, we will
     * read one DATA packet, load it on the Receive buffer, and signal the waiting user.  Thus they
     * may immediately read from the Receive buffer and move onto the rest of their event loop
     * iteration.  However, there may be 50 more packets we're then going to immediately put on the
     * Receive buffer in thread W.  It would be arguably more efficient to read all 51 and THEN
     * signal the user.  Therefore we use Node::m_sock_events to accumulate active events
     * and then read all available datagrams; only after that do we then signal Event_sets for
     * accumulated events.
     *
     * @todo One can conceive of some pathological case where due to extreme traffic we'd keep
     * reading more and more datagrams and not get out of the loop for a long time.  Perhaps add a
     * knob for the maximum number of iterations to go through before ending the loop and
     * signaling about the accumulated events. */

    // Note: m_packet_data is a member that is reused repeatedly for some performance savings.
    auto& packet_data = m_packet_data;
    /* Don't let this dynamic option's value's change to affect this main loop iteration's value.
     * This way packet_data.capacity() will essentially remain constant which is easier to reason about than
     * the alternative.  Soon enough this method will exit, and any new value will take effect next time. */
    const size_t max_packet_size = opt(m_opts.m_dyn_low_lvl_max_packet_size);

    util::Udp_endpoint low_lvl_remote_endpoint;

    // Read until error or "would block" (no data available).
    size_t packet_size = 0;
    do
    {
      /* Make buffer big enough for any packet we'd accept as valid.
       * resize(MAX) will only work if packet_data.zero() (the case in first iteration) or if packet_data.capacity()
       * >= MAX.  Hence, if a loop iteration below has left packet_data with unsatisfactory capacity below MAX,
       * then ensure zero() is true before the resize(MAX) call.  In other words, reallocate packet_data but only
       * if necessary. */
      if ((!packet_data.zero()) && (packet_data.capacity() < max_packet_size))
      {
        packet_data.make_zero(); // This must be done explicitly: acknowledges we force reallocation in next line.
      }
      packet_data.resize(max_packet_size);
      // ^-- Could use UDP available(), but its meaning is slightly ambiguous, + I don't care for the tiny race it adds.

      // Read packet from UDP net-stack internal buffer into "packet_data."
      packet_size = m_low_lvl_sock.receive_from(buffer(packet_data.data(), packet_data.size()),
                                                low_lvl_remote_endpoint, 0, sys_err_code);
      if (!sys_err_code)
      {
        assert(packet_size <= packet_data.size());
        packet_data.resize(packet_size); // boost.asio NEVER resizes vector to the # of bytes it read.

        FLOW_LOG_TRACE("Received low-level packet at [UDP " << m_low_lvl_sock.local_endpoint() << "] from "
                       "[UDP " << low_lvl_remote_endpoint << "]:");

        // Count it against the limit.
        ++recvd_packet_count;

        handled_packet_count += handle_incoming_with_simulation(&packet_data, low_lvl_remote_endpoint);
        /* packet_data is still valid and owned by us but may have any structure at all.
         * As of this writing, in practice -- assuming no simulated delay -- packet_data would be untouched
         * (and therefore could be reused sans reallocation for the next packet read in above, if any) for all
         * packet types except DATA, which is moved elsewhere via std::move() semantics and would require
         * reallocation above. */
      }
      else if (sys_err_code != boost::asio::error::would_block) // "Would block" is normal (no data available).
      {
        /* Weird it failed, since it's UDP.  Oh well.
         * Note: One might be tempted to do something dire like close some sockets.  Note that in the current setup
         * we wouldn't know which socket(s) to close, since several live on the same low-level (UDP) port.  Even if
         * we did, the failure to read could mean many things, so individual err_codes would need to be examined
         * with various actions taken depending on what happened.  (@todo Consider it.)  Without that, just ignore it
         * as we would any lost packet.  Obviously, we deal with UDP's unreliability already.
         *
         * What about the would_block and try_again (EAGAIN/EWOULDBLOCK in POSIX world and similar for Windows)
         * error codes which often demand special handling?  Well, firstly, we are likely mostly not hitting that
         * situation, as async_receive() (the call for which this is the handler function) specifically attempts
         * to avoid those error codes by only executing the handler once the socket is Readable.
         * Still, it's probably not impossible: we used async_wait() as of this writing, which means the actual
         * receiving is done in this handler, not by boost.asio (and even if it was, we would still try more receives
         * until no more are available).  Between detection of Readable by boost.asio and the actual receive call,
         * the situation may have changed.  Well, fine.  What's there to do?  would_block/try_again means
         * not Readable right now... try later.  Great!  We do just that in any case below by executing
         * async_receive() again (inside the call at the end of this method).  So this read failed due to some
         * odd change in conditions; best we can do is wait for Readable and try again later.  And we do. */
        FLOW_ERROR_SYS_ERROR_LOG_WARNING();
        assert(packet_size == 0); // Should be the case on error.
      }
      else
      {
        assert(packet_size == 0);
      }
    }
    // while (still getting data && (haven't exceeded per-handler limit || there is no limit))
    while ((packet_size != 0)
           && ((recvd_packet_count_limit == 0) || (recvd_packet_count < recvd_packet_count_limit)));

    /* Have read all available low-level data.  This may have accumulated certain tasks, like
     * combining pending individual acknowledgments into larger ACK packet(s), to be performed at
     * the end of the handler.  Do so now. */
    perform_accumulated_on_recv_tasks();

    // Helpful in understanding the relationship between handler invocations and "delta" Event_set checks.
    FLOW_LOG_TRACE("Handled a total of [" << handled_packet_count << "] incoming packets "
                   "out of [" << recvd_packet_count << "] received (limit [" << recvd_packet_count_limit << "]) "
                   "in this boost.asio handler.");
  } // if (!err_code)
  else
  {
    // boost.asio called us with an error.  Strange, since this is UDP, but treat it same as a read error above.
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();
  }

  /* If socket errors are detected above, since it's UDP and connectionless, there is hope we're
   * still OK, so still set up wait at the end of this callback.  Do not stop event loop.
   *
   * @todo This isn't necessarily sound.  Could investigate the possible errors and act
   * accordingly. */

  // Register ourselves for the next UDP receive.
  async_low_lvl_recv();
} // Node::low_lvl_recv_and_handle()

unsigned int Node::handle_incoming_with_simulation(util::Blob* packet_data,
                                                   const util::Udp_endpoint& low_lvl_remote_endpoint,
                                                   bool is_sim_duplicate_packet)
{
  using util::Blob;
  using boost::chrono::milliseconds; // Just for the output streaming.
  using boost::chrono::round;

  // We are in thread W.

  unsigned int handled = 0; // How many times we called handle_incoming() inside this invocation.

  // Basically we should call handle_incoming(), but we may first have to simulate various network conditions.

  if (is_sim_duplicate_packet || (!m_net_env_sim) || (!m_net_env_sim->should_drop_received_packet()))
  {
    // See below.
    const bool must_dupe
      = (!is_sim_duplicate_packet) && m_net_env_sim && m_net_env_sim->should_duplicate_received_packet();

    Blob packet_data_copy(get_logger());
    if (must_dupe)
    {
      /* We will simulate duplication of the packet below.  Since packet handling can be
       * destructive (for performance reasons -- see handle_data_to_established()), we must create
       * a copy that will not be hurt by this process. */
      packet_data_copy = *(static_cast<const Blob*>(packet_data)); // Add const to express we require a copy, not move.
    }

    Fine_duration latency(m_net_env_sim ? m_net_env_sim->received_packet_latency() : Fine_duration::zero());
    if (latency == Fine_duration::zero())
    {
      // No simulated latency; just handle the packet now (mainstream case).
      handle_incoming(packet_data, low_lvl_remote_endpoint);
      // *packet_data may now be decimated!  Do not use it.
      ++handled;
    }
    else
    {
      /* Pretend it was actually delivered later, as the simulator told us to do.  More precisely,
       * call handle_incoming() but later. */
      FLOW_LOG_TRACE("SIMULATION: Delaying reception of packet by simulated latency "
                     "[" << round<milliseconds>(latency) << "].");
      async_wait_latency_then_handle_incoming(latency, packet_data, low_lvl_remote_endpoint);
      // *packet_data may now be decimated!  Do not use it.
    }

    // Possibly simulate packet duplication.
    if (must_dupe)
    {
      /* Simulator told us to pretend this packet was received twice.  Note that we only model a
       * single duplication (i.e., a duplicated packet will not be simulated to itself get
       * duplicated).  Also, we don't drop a duplicated packet. */
      FLOW_LOG_TRACE("SIMULATION: Duplicating received packet.");
      handled += handle_incoming_with_simulation(&packet_data_copy, // Pass the COPY, not the possibly damaged original.
                                                 low_lvl_remote_endpoint, true);
      // packet_data_copy may now be decimated!  Do not use it.
    }
  }
  else
  {
    // Got packet, but pretend it was dropped on the way due to packet loss, as simulator told us to do.
    FLOW_LOG_TRACE("SIMULATION: Dropped received packet.");
  }

  return handled;
} // Node::handle_incoming_with_simulation()

void Node::async_wait_latency_then_handle_incoming(const Fine_duration& latency,
                                                   util::Blob* packet_data,
                                                   const util::Udp_endpoint& low_lvl_remote_endpoint)
{
  using util::Blob;
  using util::schedule_task_from_now;
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using boost::shared_ptr;

  // We are in thread W.

  // Schedule call to handle_incoming() to occur asynchronously after the latency period passes.

  /* As advertised, *packet_data loses its buffer into this new container, so that caller can immediately
   * use it for whatever they want.  Meanwhile, we asynchronously own the actual data in it now.
   * Make a smart pointer to ensure it lives long enough for handler to execute... but likely no longer than that. */
  shared_ptr<Blob> packet_data_moved_ptr(new Blob(std::move(*packet_data)));

  // Unused if it doesn't get logged, which is a slight perf hit, but anyway this sim feature is a debug/test thing.
  const Fine_time_pt started_at = Fine_clock::now();

  schedule_task_from_now(get_logger(), latency, true, &m_task_engine,
                         [this, packet_data_moved_ptr, low_lvl_remote_endpoint, latency, started_at]
                           (bool)
  {
    // We are in thread W.

    FLOW_LOG_TRACE
      ("SIMULATOR: Handling low-level packet after "
       "simulated latency [" << round<milliseconds>(latency) << "]; "
       "actual simulated latency was "
       "[" << round<milliseconds>(Fine_clock::now() - started_at) << "]; "
       "from [UDP " << low_lvl_remote_endpoint << "].");

    // Move (again) the actual buffer to handle_incoming()'s ownership.
    Blob packet_data_moved_again(std::move(*packet_data_moved_ptr));
    // *packet_data_moved_ptr is now empty and will be deleted once that smart pointer goes out of scope below.
    handle_incoming(&packet_data_moved_again, low_lvl_remote_endpoint);
    // packet_data_moved_again may now be decimated also!  Do not use it.

    /* We must do this here for similar reasons as at the end of low_lvl_recv_and_handle().  Think
     * of the present closure as simply low_lvl_recv_and_handle() running and being able
     * to read off the one low-level UDP packet (the argument "packet"). */
    perform_accumulated_on_recv_tasks();

    // Log a similar thing to that in low_lvl_recv_and_handle().
    FLOW_LOG_TRACE("Handled a total of [1] incoming packets "
                   "out of a simulated [1] received in this boost.asio handler.");
  }); // Async callback.
} // Node::async_wait_latency_then_handle_incoming()

void Node::async_sock_low_lvl_packet_send(Peer_socket::Ptr sock, Low_lvl_packet::Const_ptr&& packet,
                                          bool delayed_by_pacing)
{
  async_low_lvl_packet_send_impl(sock->remote_endpoint().m_udp_endpoint, std::move(packet), delayed_by_pacing, sock);
}

void Node::async_no_sock_low_lvl_packet_send(const util::Udp_endpoint& low_lvl_remote_endpoint,
                                             Low_lvl_packet::Const_ptr packet)
{
  /* As of this writing we don't pace things, when no Peer_socket is involved (e.g., some RSTs) => always `false`: -|
   *                                                              v-------------------------------------------------| */
  async_low_lvl_packet_send_impl(low_lvl_remote_endpoint, packet, false, Peer_socket::Ptr());
}

void Node::async_low_lvl_packet_send_impl(const util::Udp_endpoint& low_lvl_remote_endpoint,
                                          Low_lvl_packet::Const_ptr packet,
                                          bool delayed_by_pacing, Peer_socket::Ptr sock)
{
  using boost::asio::buffer;

  assert(packet);
  const auto& packet_ref = *packet;
  const auto& packet_type_id = typeid(packet_ref);

  // We are in thread W.

  Sequence_number seq_num; // See below.

  /* As explained in send_worker(), if it's a DATA packet, then we must overwrite
   * m_sent_when. That way we'll avoid the deadly RTT drift (increase) due to pacing causing a
   * higher RTT, then using that RTT to spread out packets more, which causes higher RTT again,
   * etc.  (If pacing is disabled, then the effect of doing this here rather than in send_worker()
   * will not be dramatic.)
   *
   * Note that a number of important structures are based on m_sent_when; because m_sent_when
   * ordering determines how things are ordered in m_snd_flying_pkts_by_sent_when and friends and thus the
   * operation of the Drop Timer.  So whenever we do finalize m_sent_when, we should handle those
   * things too. */
  if (packet_type_id == typeid(Data_packet))
  {
    assert(sock);
    /* OK, so this is where I update m_sent_when and handle everything related to that
     * (updating the scoreboard, Drop Timer, etc.).  async_send_to() will indeed invoke UDP sendto()
     * synchronously; that call occurs just below. */

    mark_data_packet_sent(sock, static_cast<const Data_packet&>(packet_ref).m_seq_num);
  } // if (packet type is DATA)
  // @todo See the @todo in Node::async_low_lvl_ack_send() regarding correcting ACK delay value due to pacing.

  /* Serialize to raw data.  Important subtleties:
   *
   * This does *not* create a copied buffer.  It generates a Const_buffer_sequence,
   * which is a sequence container (like vector) containing a series of buffer
   * start pointers and associated lengths.  So it is a "scattered" buffer already in memory, namely within
   * the Low_lvl_packet `packet`; and the boost.asio UDP async_send_to() we call below performs a "gather"
   * operation when it generates the UDP datagram to send out.  This should be superior to performing a
   * copy that is at least proportional (in speed and memory use) to the size of the ultimate datagram.
   * (In fact, an earlier version of the code indeed serialized to a newly generated single buffer here.)
   *
   * The only thing one must be careful of here is that the generated Const_buffer_sequence is valid only as long
   * as the underlying areas in memory continue to exist; otherwise it's just a bunch of pointers into invalid
   * memory.  The underlying memory, as documented in Low_lvl_packet::serialize_to_raw_data() doc header,
   * is valid as long as the Low_lvl_packet on which the method is called exists.  For this reason, we pass
   * `packet` (which is a shared_ptr<>) to the completion handler.  Thus it will continue to exist until
   * after the async_send_to() attempt itself.  (If we hadn't dobe this, there's a large danger that
   * `packet`'s ref-count would drop to zero at method exit just below, and hence async_send_to() asynchronously
   * would crash a little later.)
   *
   * Update: Through empirical evidence, I found that, at least with Boost 1.63 and macOS, UDP async_send_to()
   * below will silently truncate to the first 64 elements of raw_bufs.  I could find no evidence of this being
   * a common OS limitation (I've found other limits like 1024 in Linux for writev()), though I didn't dig very hard.
   * The silent truncation was quite alarming and something to look into if only for educational purposes.
   * In any case, we must keep raw_bufs.size() fairly low -- probably this is good for performance as well.
   *
   * Update: I have since looked into this in another project.  This limit, 64, continues in Boost 1.75 and is actually
   * at least imposed by boost.asio code itself, though it might just be using that value as a lowest-common-denominator
   * technique.  It is my impression that they do this silently on account of thinking only of the TCP context,
   * where a send/write op call is free to write as few bytes as it wants, and one must deal with it in any
   * case (until would-block).  The silent dropping behavior in UDP, however, is a disaster and is really a Boost bug
   * (@todo file it against Boost if needed).  In any case it is what it is, and until it goes away, we must not
   * exceed it. */
  Low_lvl_packet::Const_buffer_sequence raw_bufs;
  const size_t bytes_to_send = packet->serialize_to_raw_data_and_log(&raw_bufs);
  assert(bytes_to_send != 0);

  // Count an actual UDP stack send() call.
  sock->m_snd_stats.low_lvl_packet_xfer_called(packet_type_id, delayed_by_pacing, bytes_to_send);

  const size_t limit = opt(m_opts.m_dyn_low_lvl_max_packet_size);
  if (bytes_to_send > limit)
  {
    // Bad and rare enough for a warning.
    FLOW_LOG_WARNING("Tried to send low-level packet but before doing so detected "
                     "serialized size [" << bytes_to_send << "] exceeds limit [" << limit << "]; "
                     "check max-block-size and low-lvl-max-packet-size options!  Serialized packet: "
                     "[\n" << packet->m_concise_ostream_manip << "].");
    // However, full packets should not be logged unless DATA log level allowed, period.
    FLOW_LOG_DATA("Detailed serialized packet details from preceding warning: "
                  "[\n" << packet->m_verbose_ostream_manip << "].");

    // Short-circuit this, since no send occurred.
    sock->m_snd_stats.low_lvl_packet_xfer_completed(packet_type_id, bytes_to_send, 0);
    return;
  }
  // else

  // Initiate asynchronous send.
  m_low_lvl_sock.async_send_to(raw_bufs,
                               low_lvl_remote_endpoint,
                               [this, sock, packet, bytes_to_send](const Error_code& sys_err_code, size_t n_sent)
  {
    low_lvl_packet_sent(sock, packet, bytes_to_send, sys_err_code, n_sent);
  });
} // Node::async_low_lvl_packet_send_impl()

void Node::mark_data_packet_sent(Peer_socket::Ptr sock, const Sequence_number& seq_num)
{
  using boost::chrono::microseconds;
  using boost::chrono::round;

  // We are in thread W.

  Peer_socket::Sent_pkt_ordered_by_when_iter sent_pkt_it = sock->m_snd_flying_pkts_by_sent_when.find(seq_num);
  if (sent_pkt_it == sock->m_snd_flying_pkts_by_sent_when.past_oldest())
  {
    // We haven't even (or we have just) sent packet, and its In-Flight data already gone?  Very weird but not fatal.
    FLOW_LOG_WARNING("Sending [DATA] packet over [" << sock << "] with "
                     "sequence number [" << seq_num << "] but cannot find corresponding Sent_packet.  "
                     "Cannot deal with some of the related data structures; still sending.  Bug?");
    // @todo Reading this now, it seems more than "very weird." Possibly it should be an assert() or at least RST?
    return;
  }
  // else

  Peer_socket::Sent_packet& pkt = *sent_pkt_it->second;

  /* Mark the given packet as being sent right now.  The following structures depend on this
   * event:
   *
   *   - pkt.m_sent_when: Obviously needs to be marked with current time stamp (for RTT calculation
   *     later, among other things).
   *   - sock->m_snd_drop_timer: Since Drop_timer has an event for a packet becoming In-flight in the
   *     sense that it was actually sent over wire, which is exactly what's happening.
   *     So we call that.
   *   - Idle Timer: m_snd_last_data_sent_when is the m_sent_when value of the last DATA packet sent.
   *     Obviously we set that here. */

  // Get current time; and record it for Idle Timeout calculation.  See top of send_worker() for how it is used.
  const Fine_time_pt& now = sock->m_snd_last_data_sent_when = Fine_clock::now();
  const size_t cwnd = sock->m_snd_cong_ctl->congestion_window_bytes();
  auto& last_send_attempt = pkt.m_sent_when.back();
  const Peer_socket::order_num_t order_num = last_send_attempt.m_order_num;
  auto const logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    const Fine_time_pt prev_sent_when = pkt.m_sent_when.back().m_sent_time;
    // REPLACE the values.  Note m_order_num is just left alone.
    last_send_attempt.m_sent_time = now;
    last_send_attempt.m_sent_cwnd_bytes = cwnd;
    const microseconds diff = round<microseconds>(now - prev_sent_when);

    FLOW_LOG_TRACE_WITHOUT_CHECKING
      ("Sending/sent [DATA] packet over [" << sock << "] with "
       "sequence number [" << seq_num << "] order_num [" << order_num << "].  Send timestamp changed from "
       "[" << prev_sent_when << "] -> [" << now << "]; difference [" << diff << "].");
  }
  else
  {
    // Same but no logging.
    last_send_attempt.m_sent_time = now;
    last_send_attempt.m_sent_cwnd_bytes = cwnd;
  }

  /* Inform this guy as required, now that packet has actually been sent off over the wire (not merely In-flight
   * by the Peer_socket definition, wherein it's been placed into sock->m_snd_flying_pkts*). */
  const Drop_timer::Ptr drop_timer = sock->m_snd_drop_timer;
  drop_timer->start_contemporaneous_events();
  drop_timer->on_packet_in_flight(order_num);
  drop_timer->end_contemporaneous_events();
  /* ^-- @todo Is it possible to smoosh a bunch of mark_data_packet_sent() calls into a single
   * start/end_contemporaneous_events() group? There are probably situations where 2 or more packets are sent
   * at a time which means ultimately mark_data_packet_sent() is called essentially contemporaneously.
   * However, at least as of this writing, mark_data_packet_sent() arises from a variety of situations, at least
   * some of which are "singular" and not really contemporaneous with any other packet(s) being sent.
   * So to isolate the cases where that is in fact true (2+ calls to us are contemporaneous) is certainly non-trivial,
   * and the code to do it would be... highly technical and hard to maintain. So I wouldn't trip over myself to
   * attempt this; and actually whatever book-keeping might be necessary to pull it off might itself have
   * some performance cost. */
} // Node::mark_data_packet_sent()

void Node::low_lvl_packet_sent(Peer_socket::Ptr sock, Low_lvl_packet::Const_ptr packet,
                               size_t bytes_expected_transferred, const Error_code& sys_err_code,
                               size_t bytes_transferred)
{
  using std::numeric_limits;

  // We are in thread W.

  const auto& packet_ref = *packet;
  const auto& packet_type_id = typeid(packet_ref);

  // Note: we don't save `packet` anywhere, so the memory will finally be freed, when we're done here.

  // Log detailed info on packet but only if TRACE or DATA logging enabled.
  auto const logger_ptr = get_logger();
  if (logger_ptr && logger_ptr->should_log(log::Sev::S_TRACE, get_log_component()))
  {
    if (logger_ptr->should_log(log::Sev::S_DATA, get_log_component()))
    {
      FLOW_LOG_DATA_WITHOUT_CHECKING("Tried to send low-level packet: "
                                     "[\n" << packet->m_verbose_ostream_manip << "].");
    }
    else
    {
      FLOW_LOG_TRACE_WITHOUT_CHECKING("Tried to send low-level packet: "
                                      "[\n" << packet->m_concise_ostream_manip << "].");
    }
  }

  if (sys_err_code)
  {
    /* Weird it failed, since it's UDP.  Oh well.  (Update: One thing to watch out for is "Message too long."!)
     * Note: One might be tempted to do something dire like close some sockets.  Several Flow connections might be
     * using the same UDP port, so should we close all of them or just the one to which this outgoing packet applies?
     * Even if we made some smart decision there, the failure to write could mean many things, so individual err_codes
     * would need to be examined with various actions taken depending on what happened.  (@todo Consider it.)
     * Without that, just ignore it as we would any lost packet.  Obviously, we deal with UDP's unreliability already.
     *
     * What about the specific error_codes 'would_block' and 'try_again' (in POSIX these probably both point to
     * code EAGAIN which == EWOULDBLOCK; in Windows possibly just the former, but to be safe both should be handled the
     * same way)?   Currently I am ignoring this possibility.  Why?  Answer: we specifically use async_send() to wait
     * for the socket to be ready for the trasmit operation before actually transmitting.  This _should_ ensure
     * that those two errors are _specifically_ avoided.  However, that's probably not a 100% safe assumption.
     * Even if the OS reports "Writable" at time T, a few microseconds later some resource might get used up
     * (unrelated to our activities); and by the time the actual send executes, we might get would_block/try_again.
     * Now, it's possible that since we do NOT use async_wait() instead of async_send() call -- meaning we let
     * boost.asio both wait for Writable *and* itself execute the write -- that it would hide any such
     * corner-case EAGAIN/etc. from us and just not call this handler in that case and retry later by itself.
     * However, I don't know if it does that.  @todo Therefore it would be safer to treat those error codes,
     * would_block and try_again, specially here.  Simply, we should retry the async_send(), meaning it will wait for
     * Writable again and execute it again later on.  One might worry that this might open the possibility of
     * more than one outstanding async_send().  If this happens, it's fine.  Even if boost.asio would be so rash as
     * to allow outstanding async_send()s to execute their handlers in a different order from the calls, it's not
     * a problem, as out-of-order datagrams are handled just fine by the other side.
     *
     * Note: I've placed a more visible @todo in the class doc header.  So delete that if this gets done. */
    FLOW_ERROR_SYS_ERROR_LOG_WARNING();

    if (sock)
    {
      sock->m_snd_stats.low_lvl_packet_xfer_completed(packet_type_id);
    }

    return;
  }
  // else

  if (sock)
  {
    sock->m_snd_stats.low_lvl_packet_xfer_completed(packet_type_id, bytes_expected_transferred, bytes_transferred);
  }

  if (bytes_transferred != bytes_expected_transferred)
  {
    /* If a UDP send was partial, the datagram must have been too big, which we shouldn't have allowed; worth warning.
     * Update: Actually, at least on Mac with Boost 1.63 I've seen these outcomes:
     *   - If message is too big byte-wise, it results in truthy sys_err_code.
     *   - If too many sub-buffers in scatter/gather buffer container passed to async_send_to() were passed,
     *     they may be silently truncated (!sys_err_code but the present condition is triggered).  Limit was observed to
     *     be ~64. */
    FLOW_LOG_WARNING("Low-level packet sent, but only [" << bytes_transferred << "] of "
                     "[" << bytes_expected_transferred << "] bytes "
                     "were sent.  Internal error with packet size calculations?  More likely, did stack truncate?");
    return;
  }
  // else
  FLOW_LOG_TRACE("Success.");
} // Node::low_lvl_packet_sent()

void Node::async_no_sock_low_lvl_rst_send(Low_lvl_packet::Const_ptr causing_packet,
                                          const util::Udp_endpoint& low_lvl_remote_endpoint)
{
  using boost::asio::buffer;
  using boost::shared_ptr;

  // We are in thread W.

  // Fill out typical info.
  auto rst_base = Low_lvl_packet::create_uninit_packet_base<Rst_packet>(get_logger());
  // No sequence number.  See m_seq_num for discussion.
  rst_base->m_packed.m_src_port = causing_packet->m_packed.m_dst_port; // Right back atcha.
  rst_base->m_packed.m_dst_port = causing_packet->m_packed.m_src_port;
  rst_base->m_opt_rexmit_on = false; // Not used in RST packets, so set it to something.

  async_no_sock_low_lvl_packet_send(low_lvl_remote_endpoint, rst_base);
  // If that returned false: It's an RST, so there's no one to inform of an error anymore.  Oh well.
} // Node::async_no_sock_low_lvl_rst_send()

bool Node::async_sock_low_lvl_packet_send_paced(const Peer_socket::Ptr& sock,
                                                Low_lvl_packet::Ptr&& packet,
                                                Error_code* err_code)
{
  // We are in thread W.

  // This is the general-purpose method for sending a packet along a well-defined connection (sock).

  const auto& packet_ref = *packet;
  const auto& packet_type_id = typeid(packet_ref);

  sock->m_snd_stats.low_lvl_packet_xfer_requested(packet_type_id);

  // Fill out typical info.
  packet->m_packed.m_src_port = sock->m_local_port;
  packet->m_packed.m_dst_port = sock->remote_endpoint().m_flow_port;
  packet->m_opt_rexmit_on = sock->rexmit_on();

  /* Apply packet pacing, which tries to spread out bursts of packets to prevent loss.  For much
   * more detail, see struct Send_pacing_data comment. */

  if ((!sock->opt(sock->m_opts.m_st_snd_pacing_enabled)) ||
      (sock->m_snd_smoothed_round_trip_time == Fine_duration::zero()) ||
      (packet_type_id == typeid(Rst_packet)))
  {
    /* Per struct Send_pacing_data doc header, the pacing algorithm only begins once we have our
     * first round trip time (and thus SRTT); until then we send all packets as soon as possible.
     * Also pacing can just be disabled; in which case send all packets ASAP.
     *
     * Finally, if it's an RST, just send it ASAP.  This is discussed in more detail in the
     * Send_pacing_data struct header, but basically:  RST means packet is being CLOSED right now.
     * Queueing RST on the pacing queue means it may be sent after underlying socket is CLOSED.
     * Therefore we have to keep queue and all that machinery operating past socket being CLOSED.
     * As a rule of thumb, CLOSED => dead socket.  So just send RST right away.  This means it may
     * jump the queue ahead of DATA/ACK packets already there, but since it is an error condition
     * causing RST, we consider that OK (those packets will not be sent). */
    async_sock_low_lvl_packet_send(sock, std::move(packet), false); // false => not queued in pacing module.
    return true;
  }
  // else pacing algorithm enabled and both can and must be used.

  return sock_pacing_new_packet_ready(sock, std::move(packet), err_code);
} // Node::async_sock_low_lvl_packet_send_paced()

bool Node::sock_pacing_new_packet_ready(Peer_socket::Ptr sock, Low_lvl_packet::Ptr packet,
                                        Error_code* err_code)
{
  using boost::chrono::duration_cast;
  using boost::chrono::microseconds;
  using boost::static_pointer_cast;
  using boost::dynamic_pointer_cast;
  using boost::shared_ptr;

  // We are in thread W.

  const auto& packet_ref = *packet;
  const auto& packet_type_id = typeid(packet_ref);

  // For brevity and a bit of speed.
  Send_pacing_data& pacing = sock->m_snd_pacing_data;

  const bool is_data_packet = packet_type_id == typeid(Data_packet);
  // For logging, get the first sequence number mentioned (depending on whether it's DATA or ACK).
  Sequence_number init_seq_num;
  shared_ptr<const Data_packet> data;
  if (is_data_packet)
  {
    init_seq_num = static_pointer_cast<const Data_packet>(packet)->m_seq_num;
  }
  else
  {
    const auto& acked_packets = static_pointer_cast<const Ack_packet>(packet)->m_rcv_acked_packets;
    if (!acked_packets.empty())
    {
      init_seq_num = acked_packets.front()->m_seq_num;
    }
  }

  const bool q_was_empty = pacing.m_packet_q.empty();

  /* No matter what, we can't send packet before the ones already in the queue, so push onto end
   * of the queue.  (If queue is currently empty, then that is not a special case; we may well
   * immediately pop and send it below.) */
  pacing.m_packet_q.push_back(packet);

  FLOW_LOG_TRACE("Pacing: On [" << sock << "] packet of type [" << packet->m_type_ostream_manip << "] "
                 "is newly available for sending; pushed onto queue; queue size [" << pacing.m_packet_q.size() << "]; "
                 "initial sequence number [" << init_seq_num << "].");

  if (!q_was_empty)
  {
    const auto data = dynamic_pointer_cast<const Data_packet>(pacing.m_packet_q.front());
    // !data if it's not a DATA packet.

    assert((!data) || (pacing.m_bytes_allowed_this_slice < data->m_data.size()));

    FLOW_LOG_TRACE("Pacing: On [" << sock << "]: was already in progress; queued and done.");

    /* There were already packets in the queue, so the timer is running; certainly we can't send
     * packet ahead of those in the queue, so push on the back of the queue above.  Should we send
     * the head packet now?  No; if the last sock_pacing_new_packet_ready() or
     * sock_pacing_time_slice_end() left a non-empty queue, then the timer has been set to fire when
     * the slice ends, and more packets can be sent.  Done. */
    return true;
  }
  // else if (q_was_empty)

  // We have just pushed the sole packet on the queue.

  if (!is_data_packet)
  {
    assert(packet_type_id == typeid(Ack_packet)); // Sanity check.
    FLOW_LOG_TRACE("Pacing: On [" << sock << "]: due to packet type, sending immediately since at head of queue; "
                   "queue empty again.");

    pacing.m_packet_q.clear();

    /* Per discussion in struct Send_pacing_data doc header, if it's a non-DATA packet, then --
     * other than not being allowed to be re-ordered -- it does not "count" as a paced packet.  That
     * is, it is "worth" zero bytes when compared to m_bytes_allowed_this_slice and should be sent
     * as soon as it is at the head of the queue (which it is here, since we just placed it as the
     * sole element on the queue).  Since it doesn't count against m_bytes_allowed_this_slice, the
     * pacing timing is irrelevant to it, and based on the "send ASAP" rule, we send it now. */
    async_sock_low_lvl_packet_send(sock, std::move(packet), false); // false => not queued in pacing module.
    return true;
  }
  // else packet is DATA packet.

  const Fine_time_pt now = Fine_clock::now();
  if ((pacing.m_slice_start == Fine_time_pt()) || (now >= (pacing.m_slice_start + pacing.m_slice_period)))
  {
    /* We are past the current time slice (if there is such a thing) and have a packet to send.  By
     * the algorithm in struct Send_pacing_data doc header, this means we create a new time slice with
     * starting time = now. */

    FLOW_LOG_TRACE("Pacing: On [" << sock << "]: "
                   "current time "
                   "slice [epoch+" << duration_cast<microseconds>(pacing.m_slice_start.time_since_epoch()) << " "
                   "over " << duration_cast<microseconds>(pacing.m_slice_period) << "] is over.");

    sock_pacing_new_time_slice(sock, now);
    /* pacing.{m_slice_start, m_slice_period, m_bytes_allowed_this_slice} have all been recomputed.
     * By definition "now" is in [m_slice_start, m_slice_start + m_slice_period), since m_slice_start
     * IS now (plus some epsilon). */
  }
  // else if ("now" is basically in time range [m_slice_start, m_slice_start + m_slice_period)) {}

  /* We are in the current time slice.  The one just-queued packet can be sent if there is
   * m_bytes_allowed_this_slice budget; otherwise the timer must be started to fire at slice
   * end, at which point we're cleared to send the packet.  sock_pacing_process_q() performs this
   * (it is generalized to work with multiple packets on the queue, but it will work fine with just
   * one also).
   *
   * Note: If we just created the new time slice above, then m_bytes_allowed_this_slice >=
   * max_block_size(), so certainly the following statement will immediately send the just-queued
   * packet. If the time slice was in progress, then it depends. */

  return sock_pacing_process_q(sock, err_code, false);
} // Node::sock_pacing_new_packet_ready()

void Node::sock_pacing_new_time_slice(Peer_socket::Ptr sock, const Fine_time_pt& now)
{
  using boost::chrono::duration_cast;
  using boost::chrono::microseconds;
  using boost::chrono::milliseconds;
  using std::max;

  // We are in thread W.

  // For brevity and a bit of speed.
  Send_pacing_data& pacing = sock->m_snd_pacing_data;
  const Fine_duration& srtt = sock->m_snd_smoothed_round_trip_time;

  assert(srtt != Fine_duration::zero());

  // New slice starts now.
  pacing.m_slice_start = now;

  /* Per struct Send_pacing_data doc header: If we had perfectly fine timer precision, then we'd
   * want a slice that is SRTT / CWND.  CWND = (congestion window in bytes / max-block-size).
   * To minimize truncation error, then, it is X / (Y / Z) = X * Z / Y, where X, Y, Z are SRTT,
   * max-block-size, and congestion window in bytes, respectively. */
  Fine_duration slice_ideal_period
    = srtt * sock->max_block_size() / sock->m_snd_cong_ctl->congestion_window_bytes();
  if (slice_ideal_period == Fine_duration::zero())
  {
    // Avoid division by zero and any other tomfoolery below....
    slice_ideal_period = Fine_duration(1);
  }

  Fine_duration timer_min_period = opt(m_opts.m_st_timer_min_period);
  if (timer_min_period == Fine_duration::zero())
  {
    /* They want us to pick the a nice upper bound on the timer precision ourselves.
     *
     * This is conservative; in my tests Windows seem to have the worst timer precision, and it is
     * about 15 msec. @todo Perhaps choose here based on platform. It can get hairy, as there is
     * wide variation, so it would require much experimentation; but might be worth it for
     * performance. */
    const Fine_duration TIMER_MIN_PERIOD_DEFAULT = milliseconds(15);
    timer_min_period = TIMER_MIN_PERIOD_DEFAULT;
  }

  /* Per Send_pacing_data doc header, the actual slice period is slice_ideal_period, unless that
   * is below the timer's capabilities; in which case it is timer_min_period. */
  pacing.m_slice_period = max(slice_ideal_period, timer_min_period);

  /* Finally, allow (in this time slice) a burst of as few full-sized blocks as possible while
   * staying below the target SRTT / CWND rate: floor(m_slice_period / slice_ideal_period).
   * Note that when slice_ideal_period >= timer_min_period, i.e. the timer precision is fine enough
   * to handle the desired rate exactly, then this value will always equal 1.
   *
   * Also, convert to bytes when actually assigning the data member.
   *
   * @todo Consider doing some kind of rounding instead of using floor(). */
  pacing.m_bytes_allowed_this_slice
    = static_cast<size_t>(pacing.m_slice_period * sock->max_block_size() / slice_ideal_period);

  /* If I just use the above math, I notice that over time the queue size can drift becoming slowly
   * larger and larger as more and more time slices go by. I believe it's due to our floor math
   * above, but I have not yet fully investigated it. @todo Investigate it.
   *
   * However if I just increase the number of bytes allowed per slice a little bit, it makes the
   * drift go away and probably doesn't reduce the effectiveness of the pacing much. */
  const size_t QUEUE_SIZE_DRIFT_PREVENTION_PCT = 110; // @todo Maybe make it a configurable option.
  pacing.m_bytes_allowed_this_slice *= QUEUE_SIZE_DRIFT_PREVENTION_PCT;
  pacing.m_bytes_allowed_this_slice /= 100;

  assert(pacing.m_bytes_allowed_this_slice >= sock->max_block_size());

  FLOW_LOG_TRACE("Pacing: On [" << sock << "]: "
                 "new time "
                 "slice [epoch+" << duration_cast<microseconds>(pacing.m_slice_start.time_since_epoch()) << " "
                 "over " << duration_cast<microseconds>(pacing.m_slice_period) << "]; "
                 "ideal slice period = [SRTT " << duration_cast<microseconds>(srtt) << "] / "
                 "([cong_wnd " << sock->m_snd_cong_ctl->congestion_window_bytes() << "] / "
                 "[max-block-size " << sock->max_block_size() << "]) = "
                 "[" << duration_cast<microseconds>(slice_ideal_period) << "]; "
                 "timer_min_period = [" << duration_cast<microseconds>(timer_min_period) << "]; "
                 "bytes_allowed = max(ideal, min) / ideal * max-block-size * "
                 "[" << QUEUE_SIZE_DRIFT_PREVENTION_PCT << "%] = "
                 "[" << pacing.m_bytes_allowed_this_slice << "].");
} // Node::sock_pacing_new_time_slice()

bool Node::sock_pacing_process_q(Peer_socket::Ptr sock, Error_code* err_code, bool executing_after_delay)
{
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using boost::shared_ptr;
  using boost::weak_ptr;
  using boost::static_pointer_cast;
  using boost::dynamic_pointer_cast;
  using std::max;

  // We are in thread W.

  /* Pre-condition is that the current time is within the current time slice, and that all other
   * invariants hold (including that the head packet, if any, is a DATA packet).  So now we send as
   * many packets as still allowed by the budget in m_bytes_allowed_this_slice.  If anything remains
   * beyond that, we schedule a timer to hit at the end of the time slice to get the rest. */

  // For brevity and a bit of speed.
  Send_pacing_data& pacing = sock->m_snd_pacing_data;
  shared_ptr<const Data_packet> head_packet;

  FLOW_LOG_TRACE("Pacing: On [" << sock << "]: processing queue; queue size [" << pacing.m_packet_q.size() << "]; "
                 "byte budget [" << sock->bytes_blocks_str(pacing.m_bytes_allowed_this_slice) << "] remaining in this "
                 "slice.");

  /* Pop things from queue until we've run out of pacing byte budget for this time slice, or until
   * there is nothing left to send. */
  while ((!pacing.m_packet_q.empty())
         && (pacing.m_bytes_allowed_this_slice
             >= (head_packet = static_pointer_cast<const Data_packet>(pacing.m_packet_q.front()))->m_data.size()))
  {
    // Explicit invariant: header_packet is DATA.  We always send non-DATA packets as soon as they get to head of queue.

    // It is a DATA packet at head of queue, and there is enough pacing budget to send it now.

    // Use up the budget.
    pacing.m_bytes_allowed_this_slice -= head_packet->m_data.size();

    FLOW_LOG_TRACE("Will send [" << head_packet->m_data.size() << "] bytes of data; budget now "
                   "[" << sock->bytes_blocks_str(pacing.m_bytes_allowed_this_slice) << "]; "
                   "queue size now [" << (pacing.m_packet_q.size() - 1) << "].");

    // Send it.  executing_after_delay <=> packet being sent was delayed by pacing as opposed to sent immediately.
    async_sock_low_lvl_packet_send(sock, std::move(head_packet), executing_after_delay);

    pacing.m_packet_q.pop_front(); // After this the raw pointer in head_packet should be freed.

    /* Since we've popped a packet, another packet is now at the head.  We must maintain the
     * invariant that no non-DATA packet is at the head of the queue (see struct Send_pacing_data
     * doc header for reasoning), i.e. we should send any such packets immediately.  Do so until
     * we run out or encounter a DATA packet. */

    // Subtlety: Using dynamic_pointer_cast<> instead of typeid() to check packet type to avoid "side effect" warning.
    Low_lvl_packet::Const_ptr head_packet_base;
    while ((!pacing.m_packet_q.empty())
           && (!(dynamic_pointer_cast<const Data_packet>(head_packet_base = pacing.m_packet_q.front()))))
    {
      FLOW_LOG_TRACE("Pacing: On [" << sock << "]: due to packet type, sending immediately since at head of queue; "
                     "queue size now [" << (pacing.m_packet_q.size() - 1) << "].");

      // See above cmnt about last arg.
      async_sock_low_lvl_packet_send(sock, std::move(head_packet_base), executing_after_delay);

      pacing.m_packet_q.pop_front(); // After this the raw pointer in head_packet should be freed.

      // Note that, as discussed in struct Send_pacing_data doc header, a non-DATA packet is worth 0 budget.
    }
  } // while ((m_packet_q not empty) && (more m_bytes_allowed_this_slice budget available))

  if (pacing.m_packet_q.empty())
  {
    FLOW_LOG_TRACE("Pacing: Queue emptied.");

    // Successfully sent off entire queue.  Pacing done for now -- until the next sock_pacing_new_packet_ready().
    return true;
  }
  // else

  /* No more budget left in this pacing time slice, but there is at least one packet in the queue
   * still.  Per algorithm, a fresh slice should begin ASAP (since packets must be sent ASAP but no
   * sooner).  Therefore schedule timer for the end of the time slice, which is just
   * m_slice_start + m_slice_period. */
  const Fine_time_pt slice_end = pacing.m_slice_start + pacing.m_slice_period;

  Error_code sys_err_code;
  pacing.m_slice_timer.expires_at(slice_end, sys_err_code);
  // (Even if slice_end is slightly in the past, that'll just mean it'll fire ASAP.)

  FLOW_LOG_TRACE("Pacing: Exhausted budget; queue size [" << pacing.m_packet_q.size() << "]; "
                 "scheduling next processing at end of time slice "
                 "in [" << round<milliseconds>(slice_end - Fine_clock::now()) << "].");

  if (sys_err_code) // If that failed, it's probably the death of the socket....
  {
    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log the non-portable system error code/message.
    FLOW_ERROR_EMIT_ERROR(error::Code::S_INTERNAL_ERROR_SYSTEM_ERROR_ASIO_TIMER);

    return false;
  }
  // else

  // When triggered or canceled, call this->sock_pacing_time_slice_end(sock, <error code>).
  pacing.m_slice_timer.async_wait([this, sock_observer = weak_ptr<Peer_socket>(sock)]
                                    (const Error_code& sys_err_code)
  {
    auto sock = sock_observer.lock();
    if (sock)
    {
      sock_pacing_time_slice_end(sock, sys_err_code);
    }
    // else { Possible or not, allow for this possibility for maintainability. }
  });

  // More work to do later, but for now we've been successful.
  return true;

  /* That's it.  The only reason the timer would get canceled is if we go into CLOSED state, in
   * which case it can just do nothing. */
} // Node::sock_pacing_process_q()

void Node::sock_pacing_time_slice_end(Peer_socket::Ptr sock, [[maybe_unused]] const Error_code& sys_err_code)
{
  // We are in thread W.

  // As always, no need to lock m_state, unless we plan to alter it, since no other thread can alter it.
  if (sock->m_state == Peer_socket::State::S_CLOSED)
  {
    /* Once state is CLOSED, the socket is dead -- all packets have been sent.  A corollary of
     * that is that if we must send an RST, then always send it immediately (even if it has to
     * jump ahead of other packets waiting on queue). */
    return;
  }

  // We only cancel the timer when we close socket, and we've already returned if we'd done that.
  assert(sys_err_code != boost::asio::error::operation_aborted);

  // There could be other errors, but as in other timer handlers, we've no idea what that means, so pretend all is OK.

  // Pre-condition: if we set up the timer, then the queue had to have been non-empty at the time.
  assert(!sock->m_snd_pacing_data.m_packet_q.empty());

  FLOW_LOG_TRACE("Pacing: On [" << sock << "]: slice end timer fired; creating new slice and processing queue.");

  /* Timer fired, so right now we are somewhere after the end of the current time slice.  Therefore,
   * begin the next time slice. */
  sock_pacing_new_time_slice(sock, Fine_clock::now());

  /* pacing.{m_slice_start, m_slice_period, m_bytes_allowed_this_slice} have all been recomputed.
   * By definition "now" is in [m_slice_start, m_slice_start + m_slice_period), since m_slice_start
   * IS now (plus some epsilon). */

  /* We are in the current time slice.  We just created the new time slice above, so
   * m_bytes_allowed_this_slice >= max_block_size(), and certainly the following statement will
   * immediately send at least one packet. */

  Error_code err_code;
  if (!sock_pacing_process_q(sock, &err_code, true)) // Process as many packets as the new budget allows.
  {
    /* Error sending.  Unlike in sock_pacing_process_q() or sock_pacing_new_packet_ready() --
     * which are called by something else that would handle the error appropriately -- we are
     * called by boost.asio on a timer event.  Therefore we must handle the error ourselves.  As is
     * standard procedure elsewhere in the code, in this situation we close socket. */

    // Pre-conditions: sock is in m_socks and S_OPEN, err_code contains reason for closing.
    rst_and_close_connection_immediately(socket_id(sock), sock, err_code, false); // This will log err_code.
    /* ^-- defer_delta_check == false: for similar reason as when calling send_worker() from
     * send_worker_check_state(). */
  }
} // Node::sock_pacing_time_slice_end()

bool Node::async_sock_low_lvl_packet_send_or_close_immediately(const Peer_socket::Ptr& sock,
                                                               Low_lvl_packet::Ptr&& packet,
                                                               bool defer_delta_check)
{
  Error_code err_code;
  if (!async_sock_low_lvl_packet_send_paced(sock, std::move(packet), &err_code))
  {
    close_connection_immediately(socket_id(sock), sock, err_code, defer_delta_check);
    return false;
  }
  // else
  return true;
}

void Node::async_sock_low_lvl_rst_send(Peer_socket::Ptr sock)
{
  // We are in thread W.

  // Fill out common fields and asynchronously send packet.
  auto rst = Low_lvl_packet::create_uninit_packet_base<Rst_packet>(get_logger());
  Error_code dummy;
  async_sock_low_lvl_packet_send_paced(sock, std::move(rst), &dummy);

  // If that returned false: It's an RST, so there's no one to inform of an error anymore.  Oh well.
} // Node::async_sock_low_lvl_rst_send()

void Node::sync_sock_low_lvl_rst_send(Peer_socket::Ptr sock)
{
  using boost::asio::buffer;

  // We are in thread W.

  // Fill out fields.
  auto rst = Low_lvl_packet::create_uninit_packet_base<Rst_packet>(get_logger());
  rst->m_packed.m_src_port = sock->m_local_port;
  rst->m_packed.m_dst_port = sock->remote_endpoint().m_flow_port;
  rst->m_opt_rexmit_on = false; // Unused in RST packets, so set it to something.

  // Serialize to a buffer sequence (basically sequence of pointers/lengths referring to existing memory areas).
  Low_lvl_packet::Const_buffer_sequence raw_bufs;
  const size_t size = rst->serialize_to_raw_data_and_log(&raw_bufs); // Logs TRACE/DATA.

  // This special-case sending path should report stats similarly to the main path elsewhere.
  const auto& rst_type_id = typeid(Rst_packet); // ...a/k/a typeid(*rst).
  sock->m_snd_stats.low_lvl_packet_xfer_requested(rst_type_id);
  sock->m_snd_stats.low_lvl_packet_xfer_called(rst_type_id, false, size);

  // Same check as when using async_send_to().  @todo Code reuse?
  const size_t limit = opt(m_opts.m_dyn_low_lvl_max_packet_size);
  if (size > limit)
  {
    // Bad and rare enough for a warning.
    FLOW_LOG_WARNING("Tried to send RST but before doing so detected "
                     "serialized size [" << size << "] exceeds limit [" << limit << "]; "
                     "check max-block-size and low-lvl-max-packet-size options!  Serialized packet: "
                     "[\n" << rst->m_concise_ostream_manip << "].");

    sock->m_snd_stats.low_lvl_packet_xfer_completed(rst_type_id, size, 0); // No send occurred.
  }
  else
  {
    // Synchronously send to remote UDP.  If non-blocking mode and not sendable, this will return error.
    Error_code sys_err_code;
    const size_t size_sent = m_low_lvl_sock.send_to(raw_bufs,
                                                    sock->remote_endpoint().m_udp_endpoint, 0, sys_err_code);
    if (sys_err_code)
    {
      FLOW_ERROR_SYS_ERROR_LOG_WARNING();
      sock->m_snd_stats.low_lvl_packet_xfer_completed(rst_type_id);
    }
    else
    {
      sock->m_snd_stats.low_lvl_packet_xfer_completed(rst_type_id, size, size_sent);
    }
  }
} // Node::sync_sock_low_lvl_rst_send()

Send_pacing_data::Send_pacing_data(util::Task_engine* task_engine) :
  m_slice_period(0), // Comment says undefined is zero() so don't leave it uninitialized.  @todo Is it really needed?
  m_bytes_allowed_this_slice(0),
  m_slice_timer(*task_engine)
{
  // Nothing.
}

} // namespace flow::net_flow
