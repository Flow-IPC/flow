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
#include "flow/net_flow/info.hpp"

namespace flow::net_flow
{
// Types.

/**
 * A class that keeps a Peer_socket_receive_stats data store, includes methods to conveniently
 * accumulate data in it, and provides output to `ostream`.
 *
 * Any object of this class is intended to have a 1-to-1 relationship with a Peer_socket.  The
 * interface is intentionally simple; the class keeps some counters and limited auxilliary state
 * (such as creation time); the Node informs the class of events (like "DATA packet received"),
 * counts of which are stored in the class and are accessible through the interface (at least
 * through the `ostream` output operator).
 *
 * As usual, invoke the event handler methods of the class whenever the appropriate events occur.
 * Failing to do so will obviously result in wrong statistics.
 *
 * The class intentionally does not take a reference to a Peer_socket and avoids logging; it is a
 * simple accumulating data store.
 *
 * ### Thread safety ###
 * Same as any `struct` with no locking done therein.
 *
 * @see Peer_socket_receive_stats
 */
class Peer_socket_receive_stats_accumulator :
  private Peer_socket_receive_stats, // HAS-A relationship.
  private boost::noncopyable
{
public:
  // Methods.

  /**
   * Returns reference to non-modifiable current set of accumulated stats.
   *
   * @return Ditto.
   */
  const Peer_socket_receive_stats& stats() const;

  /**
   * Indicates one DATA packet has been received on socket.
   *
   * @param data
   *        User data size.
   */
  void total_data_packet(size_t data);

  /**
   * Indicates total_data_packet(), and these data are new and acceptable into Receive buffer
   * assuming there is space.
   *
   * @param data
   *        User data size.
   */
  void good_data_packet(size_t data);

  /**
   * Indicates good_data_packet(), but these data are dropped due to insufficient Receive buffer
   * space.
   *
   * @param data
   *        User data size.
   */
  void good_data_dropped_buf_overflow_packet(size_t data);

  /**
   * Indicates good_data_packet(), but these data are dropped due to insufficient Receive reassembly
   * queue space (only possible when retransmission is enabled).
   *
   * @param data
   *        User data size.
   */
  void good_data_dropped_reassembly_q_overflow_packet(size_t data);

  /**
   * Indicates good_data_packet(), and these data are not dropped (so either delivered into Receive
   * buffer or queued for reassembly later).
   *
   * @param data
   *        User data size.
   */
  void good_data_accepted_packet(size_t data);

  /**
   * Indicates good_data_accepted_packet(), and these data are delivered into Receive buffer (either
   * immediately upon receipt or upon later reassembly).
   *
   * @param data
   *        User data size.
   */
  void good_data_delivered_packet(size_t data);

  /**
   * Indicates good_data_accepted_packet(), and these data are, upon receipt, queued for reassembly
   * (not immediately delivered into Receive buffer).
   *
   * @param data
   *        User data size.
   */
  void good_data_first_qd_packet(size_t data);

  /**
   * Indicates total_data_packet(), but the arrived data have either already been received before or
   * (more likely) have been considered Dropped by now; therefore data not delivered to Receive
   * buffer.
   *
   * @param data
   *        User data size.
   */
  void late_or_dupe_data_packet(size_t data);

  /**
   * Indicates total_data_packet(), but there is some error about the sequence numbers so that they
   * are not acceptable, and the connection did/will close as a result.
   *
   * @param data
   *        User data size.
   */
  void error_data_packet(size_t data);

  /**
   * Indicates that one or more unreceived data packets have been considered Dropped due to the
   * number of later packets that have been received.  For each such packet that does arrive in the
   * future (though unlikely), late_or_dupe_data_packet() will be called.
   *
   * @param data
   *        User data size.
   */
  void presumed_dropped_data(size_t data);

  /**
   * Indicates that an individual acknowledgment for one packet will be sent.
   * @param data
   *        User data size.
   */
  void total_to_send_ack_packet(size_t data);

  /**
   * Indicates that good_data_delivered_packet() and therefore an individual acknowledgment for this
   * packet will be sent.  Note that good_data_packet() alone does not necessarily mean that an
   * acknowledgment will be sent in response (namely if we have to drop the packet due to insufficient
   * buffer space).
   *
   * @param data
   *        User data size.
   */
  void good_to_send_ack_packet(size_t data);

  /**
   * Indicates that late_or_dupe_data_packet() and therefore an individual acknowledgment for this
   * packet will be sent.
   * @param data
   *        User data size.
   */
  void late_or_dupe_to_send_ack_packet(size_t data);

  /**
   * Indicates that all incoming data in the current boost.asio handler have been scanned and any
   * ACKs that may be sent in this handler have been sent, and that immediately after that fact, the
   * number of individual acknowledgments (NOTE: not ACK packets but individual acknowledgments
   * therein) is as given in the `count` argument.  Note that unlike most methods in this class, this
   * indicates the current state as opposed to an event.  Internally this is necessary to maintain a
   * total count of individual acks that have ever been delayed (without double-counting).
   *
   * The current mechanism is that, in a given boost.asio low-level receive handler, we will either flush all
   * the acks that have built up over the last N (where N >= 1, including the current one) handlers, or we
   * will queue up 0 or more to send in a later handler.  (We do not add some more + flush some but not all,
   * nor do we just flush some but not all.)  Thus, in the 2 cases just described, we'd call this
   * method with count = 0 or some value >= the value from the last time, respectively.
   *
   * @param count
   *        Number of packets.  This should be either zero or >= the last value passed into
   *        the same call for this object.  Otherwise assertion trips.
   */
  void current_pending_to_ack_packets(size_t count);

  /**
   * Indicates than one individual acknowledgment of a data packet has been packaged into an ACK
   * packet that will be sent as soon as possible.
   */
  void sent_individual_ack();

  /**
   * Indicates than one low-level ACK packet will be sent as soon as possible.
   *
   * @param rcv_wnd_update_only
   *        `true` if and only if this ACK contains no individual acknowledgments; i.e., it is sent
   *        only with the purpose of advertising rcv_wnd.
   */
  void sent_low_lvl_ack_packet(bool rcv_wnd_update_only);

  /**
   * Indicates we seem to have detected that our rcv_wnd went from exhaustion to largely free, and
   * therefore we will now inform the sender of this, so that it can resume sending data.
   */
  void rcv_wnd_recovery_event_start();

  /**
   * Indicates that the recovery indicated by last rcv_wnd_recovery_event_start() call has
   * finished (we are no longer ACKing sender to advertise receive window).
   *
   * @param success
   *        `true` if recovery successful (sender has sent more DATA packets); `false` if recovery
   *        failed (we stopped due to exceeding long timeout).
   */
  void rcv_wnd_recovery_event_finish(bool success);

  /**
   * Indicates the Receive buffer was enqueued with data from network (so its `data_size()`
   * increased).
   *
   * @param size
   *        The # of bytes in the buffer after the enqueueing.
   */
  void buffer_fed(size_t size);

  /**
   * Indicates a `Low_lvl_packet& p` was received with `typeid(p) == type`,
   * deserialized from a buffer of length `size` bytes, targeted at the associated socket.
   *
   * Caveat: Some packets may not be targeted at any particular socket; probably malformed.
   * They would not be counted here.
   *
   * Caveat: SYN counts; but note that the Peer_socket with which this call is associated would need
   * to first be created before making the call.
   *
   * @param type
   *        `typeid(p)`, where p refers to an instance of a concrete Low_lvl_packet sub-type.
   * @param size
   *        Datagram contents' size.
   */
  void low_lvl_packet(const std::type_info& type, size_t size);
}; // class Peer_socket_receive_stats_accumulator

/**
 * A class that keeps a Peer_socket_send_stats data store, includes methods to conveniently
 * accumulate data in it, and provides output to `ostream`.
 *
 * All other comments in Peer_socket_receive_stats_accumulator doc header apply equally to this
 * class.
 *
 * @see Peer_socket_receive_stats_accumulator
 * @see Peer_socket_send_stats
 */
class Peer_socket_send_stats_accumulator :
  private Peer_socket_send_stats, // HAS-A relationship.
  private boost::noncopyable
{
public:
  // Methods.

  /**
   * Returns reference to non-modifiable current set of accumulated stats.
   *
   * @return Ditto.
   */
  const Peer_socket_send_stats& stats() const;

  /**
   * Indicates a DATA packet was sent.
   *
   * @param data
   *        User data size.
   * @param rexmission
   *        `true` if it is a retransmission (`rexmit_id > 0`), `false` otherwise.
   */
  void data_sent(size_t data, bool rexmission);

  /**
   * Indicates that 1 or more packets have been converted from In-flight to Dropped.
   *
   * @param data
   *        User data size.
   * @param count
   *        Packet count.
   */
  void dropped_data(size_t data, size_t count);

  /**
   * Indicates an ACK packet on this socket was received.
   *
   * @param rcv_wnd_update_only
   *        `true` if and only if the ACK contained no individual acks (rcv_wnd update only).
   */
  void received_low_lvl_ack_packet(bool rcv_wnd_update_only);

  /**
   * Indicates an ACK packet has advertised a rcv_wnd and whether this rcv_wnd would allow one to
   * send one full-sized packet assuming no other barriers (availability of data to send, congestion
   * control).  In other words indicates whether the other side's Receive buffer is about full to
   * the best of our knowledge.
   *
   * @param wnd_exhausted
   *        `true` if a packet would not be sent due to rcv_wnd; `false` otherwise.
   */
  void updated_rcv_wnd(bool wnd_exhausted);

  /**
   * Indicates one individual acknowledgment (of a DATA packet) inside an ACK has been received.
   * (This ack is not necessarily valid.)
   */
  void received_ack();

  /**
   * Indicates received_ack(), and the arrived acknowledgment properly acknowledged a DATA packet,
   * converting it from In-flight to Acknowledged.
   *
   * @param data
   *        User data size.
   */
  void good_ack(size_t data);

  /**
   * Indicates received_ack(), but the arrived acknowledgment is for a packet that has either
   * already been convered from In-flight to Acknowledged or (more likely) already been converted
   * from In-flight to Dropped.
   */
  void late_or_dupe_ack();

  /**
   * Indicates received_ack(), but there is some error about the sequence number so that it is not
   * acceptable, and the connection did/will close as a result.
   */
  void error_ack();

  /**
   * Indicates a new loss event (not counting Drop Timeouts) has been reported to congestion control
   * (Congestion_control_strategy::on_loss_event()).  (Note again that multiple individual
   * acknowledgments, and even multiple ACKs, can belong to one loss event and would only be counted
   * once in thise case.)
   */
  void loss_event();

  /// Indicates a Drop Timer fired once.
  void drop_timeout();

  /**
   * Indicates an idle timeout (in the send direction) has been detected and indicated to congestion
   * control module.
   */
  void idle_timeout();

  /**
   * Indicates the Send buffer was enqueued with data from a user `send*()` call (so its `data_size()`
   * increased).
   *
   * @param size
   *        The # of bytes in the buffer after the enqueueing.
   */
  void buffer_fed(size_t size);

  /**
   * Indicates a packet was given to the sending module to send, possibly subject to pacing.
   * We guess its serialized size is not yet known, because it has not been serialized yet.
   *
   * @todo It may make sense to serialize a packet as early as possible, so that the size is known at
   * the stage of Peer_socket_send_stats_accumulator::low_lvl_packet_xfer_requested() call.
   * Possibly knowing the serialized size this early can help the pacing module make smarter decisions.
   *
   * @param type
   *        `typeid(p)`, where p refers to an instance of a concrete Low_lvl_packet sub-type.
   */
  void low_lvl_packet_xfer_requested(const std::type_info& type);

  /**
   * Indicates low_lvl_packet_xfer_requested(), and that packet has now been given to OS UDP stack to actually send.
   *
   * @param type
   *        See low_lvl_packet_xfer_requested().
   * @param delayed_by_pacing
   *        `true` if and only if there was a pacing-related delay since low_lvl_packet_xfer_requested()
   *        before this call.
   * @param size
   *        Serialized size of the packet being sent.
   */
  void low_lvl_packet_xfer_called(const std::type_info& type, bool delayed_by_pacing, size_t size);

  /**
   * Indicates low_lvl_packet_xfer_called(), and that send operation has now reported its outcome by calling handler
   * (or synchronously returning the result).
   *
   * @param type
   *        See low_lvl_packet_xfer_called().
   * @param size_expected
   *        See low_lvl_packet_xfer_called().
   * @param size_xferred
   *        Size reported as actually sent by OS.  Use the default value to indicate OS indicated error (no bytes
   *        sent).
   */
  void low_lvl_packet_xfer_completed(const std::type_info& type,
                                     size_t size_expected = std::numeric_limits<size_t>::max(),
                                     size_t size_xferred = 0);
}; // class Peer_socket_send_stats_accumulator

} // namespace flow::net_flow
