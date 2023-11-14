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

#include "flow/net_flow/net_flow_fwd.hpp"
#include "flow/net_flow/options.hpp"
#include <boost/utility.hpp>
#include <boost/array.hpp>
#include <map>
#include <iostream>

namespace flow::net_flow
{
// Types.

/**
 * A data store that keeps stats about the incoming direction of a Peer_socket connection to another
 * Flow-protocol Peer_socket.  Note that this also includes stats about *outgoing* traffic that facilitates
 * further incoming traffic (namely, data on outgoing acknowledgments are in this structure).
 *
 * The objects also have an `ostream` output interface for human-readability, e.g., for logs.
 *
 * The user can obtain objects representing actual stats via Peer_socket::info().
 *
 * ### Thread safety ###
 * Same as any `struct` with no locking done therein.
 *
 * @warning It is very heavily discouraged to make algorithmic decisions based on the data in these
 * objects.  They are for human diagnostics, information, and debugging only.  For example, some data
 * represented may pertain to internal implementation; other data may not be up-to-date; etc.
 *
 * @internal
 *
 * The internal implementation keeps this `struct` within the accumulator
 * Peer_socket_receive_stats_accumulator and has some methods to conveniently accumulate the stats.
 */
struct Peer_socket_receive_stats
{
  // Constructors/destructor.

  /// Constructs object by initializing stats to their initial values.
  explicit Peer_socket_receive_stats();

  // Methods.

  /**
   * Outputs the current stats, across multiple lines but not ending with a newline, into the given
   * output stream.  `operator<<()` uses this and is more likely to be used directly by the class
   * user.
   *
   * @param os
   *        Stream to which to serialize.
   */
  void output(std::ostream* os) const;

  // Data.

  /// The time this object (or source object from assignment) was made; should be about equal to when socket was made.
  Fine_time_pt m_init_time;

  /// Bytes in DATA packets received on socket.
  uint64_t m_total_data_size;
  /// Number of DATA packets received on socket.
  uint64_t m_total_data_count;

  /// Of #m_total_data_size, the data that were new and acceptable into Receive buffer assuming there was space.
  uint64_t m_good_data_size;
  /// Of #m_total_data_count, the data that were new and acceptable into Receive buffer assuming there was space.
  uint64_t m_good_data_count;

  /**
   * Of #m_good_data_size, the data that were not dropped (so either delivered into Receive buffer or
   * queued for reassembly later).
   */
  uint64_t m_good_data_accepted_size;
  /**
   * Of #m_good_data_count, the data that were not dropped (so either delivered into Receive buffer
   * or queued for reassembly later).
   */
  uint64_t m_good_data_accepted_count;

  /**
   * Of #m_good_data_accepted_size, the data that were delivered into Receive buffer (either
   * immediately upon receipt or upon later reassembly).
   */
  uint64_t m_good_data_delivered_size;
  /**
   * Of #m_good_data_accepted_count, the data that were delivered into Receive buffer (either
   * immediately upon receipt or upon later reassembly).
   */
  uint64_t m_good_data_delivered_count;

  /**
   * Of #m_good_data_accepted_size, the data that were, upon receipt, queued for reassembly
   * (not immediately delivered into Receive buffer).
   */
  uint64_t m_good_data_first_qd_size;
  /**
   * Of #m_good_data_accepted_count, the data that were, upon receipt, queued for reassembly (not
   * immediately delivered into Receive buffer).
   */
  uint64_t m_good_data_first_qd_count;

  /**
   * Of #m_good_data_size, the data that were dropped due to insufficient Receive buffer
   * space.
   */
  uint64_t m_good_data_dropped_buf_overflow_size;
  /**
   * Of #m_good_data_count, the data that were dropped due to insufficient Receive buffer space.
   */
  uint64_t m_good_data_dropped_buf_overflow_count;

  /**
   * Of #m_good_data_size, the data that were dropped due to insufficient Receive reassembly queue
   * space (only possible when retransmission is enabled).
   */
  uint64_t m_good_data_dropped_reassembly_q_overflow_size;
  /**
   * Of #m_good_data_count, the data that were dropped due to insufficient Receive reassembly queue
   * space (only possible when retransmission is enabled).
   */
  uint64_t m_good_data_dropped_reassembly_q_overflow_count;

  /**
   * Of #m_total_data_size, the data that contained some error about the sequence numbers so that
   * they were not acceptable, and the connection did/will close as a result.
   */
  uint64_t m_error_data_size;
  /**
   * Of #m_total_data_count, the data that contained some error about the sequence numbers so that
   * they were not acceptable, and the connection did/will close as a result.
   */
  uint64_t m_error_data_count;

  /**
   * Of #m_total_data_size, the data that had either already been received before or (more likely)
   * had been considered Dropped by now; therefore these data not delivered to Receive buffer.
   */
  uint64_t m_late_or_dupe_data_size;
  /**
   * Of #m_total_data_count, the data that had either already been received before or (more likely)
   * had been considered Dropped by now; therefore these data not delivered to Receive buffer.
   */
  uint64_t m_late_or_dupe_data_count;

  /**
   * Total number of bytes in hypothetical data packets that have been considered Dropped due to the
   * number of later packets that have been received.  For each such packet that does arrive in the
   * future (though unlikely), #m_late_or_dupe_data_size and #m_late_or_dupe_data_count will increase.
   */
  uint64_t m_presumed_dropped_data_size;

  /**
   * Bytes in received DATA packets acknowledged thus far or that have been received and are pending
   * to be acknowledged.
   */
  uint64_t m_total_to_send_acks_data_size;
  /**
   * Number of DATA packets acknowledged thus far or that have been received and are pending to be
   * acknowledged.
   */
  uint64_t m_total_to_send_acks_count;

  /**
   * Of #m_total_to_send_acks_data_size, the data that also satisfy the criteria in
   * #m_good_data_delivered_size.  That is, bytes acknowledged due to being received and delivered
   * into Receive buffer.
   */
  uint64_t m_good_to_send_acks_data_size;
  /**
   * Of #m_total_to_send_acks_count, the data that also satisfy the criteria in
   * #m_good_data_delivered_count.
   */
  uint64_t m_good_to_send_acks_count;

  /**
   * Of #m_total_to_send_acks_data_size, the data that also satisfy the criteria in
   * #m_late_or_dupe_data_size.  That is, bytes acknowledged due to being received but no longer
   * acceptable into Receive buffer (due to being duplicate or late).
   */
  uint64_t m_late_or_dupe_to_send_acks_data_size;
  /**
   * Of #m_total_to_send_acks_count, the data that also satisfy the criteria in
   * #m_late_or_dupe_data_count.
   */
  uint64_t m_late_or_dupe_to_send_acks_count;

  /**
   * Of #m_total_to_send_acks_count, the packets that have not yet been sent to the sender (pending
   * acknowledgments).
   */
  size_t m_current_pending_acks_count;

  /**
   * The total number of individual packet acknowledgments whose sending was delayed (via delayed
   * ACK mechanism) beyond the handler in which they were originally constructed.
   */
  uint64_t m_delayed_acks_count;

  /**
   * Number of DATA packets such that for a given packet an individual acknowledgment has been packaged
   * into an ACK packet that has been sent or will be sent as soon as possible.
   */
  uint64_t m_sent_individual_acks_count;

  /// Number of low-level ACK packets that have been sent or will be sent as soon as possible.
  uint64_t m_sent_low_lvl_acks_count;

  /**
   * Number of times we detected (heuristically but fairly reliably) that the following event
   * occurred: our rcv_wnd (or free Receive buffer space) was exhausted; but then significatly freed
   * by application-layer `receive()` calls.  When this event is detected we immediately attempt to
   * inform the sender, so that it may resume sending (which it probably stopped due to empty
   * receive window); we then periodically keep informing the sender of this, until more data arrive
   * or a certain amount of time passes (whichever occurs first).
   *
   * In some situations the sender simply stopping to send data (at the application layer) can be
   * mistakenly identified as such an event, but this is basically harmless.
   */
  uint64_t m_rcv_wnd_recovery_count;

  /**
   * Of #m_rcv_wnd_recovery_count, the number of times the recovery was successful: i.e., resulted in
   * more data packets arriving from sender.
   */
  uint64_t m_rcv_wnd_recovery_success_count;

  /**
   * Of #m_rcv_wnd_recovery_count, the number of times the recovery failed: i.e., resulted in
   * a certain lengthy timeout firing before any more data packets arrived from sender.  (This would
   * happen either due to extremely loss, or because the sender had really stopped sending data at
   * the application layer [so this was not a real rcv_wnd exhaustion event after all].)
   */
  uint64_t m_rcv_wnd_recovery_timeout_count;

  /**
   * Of #m_sent_low_lvl_acks_count, the packets that contained no individual acknowledgments but
   * are to be sent only to advertise rcv_wnd.
   */
  uint64_t m_sent_low_lvl_rcv_wnd_only_acks_count;

  /// Maximum number of bytes in the Receive buffer so far.
  size_t m_max_buf_data_size;

  /**
   * Total size in serialized form of low-level packets received targeted at this socket, split up
   * by polymorphic sub-type ID `type_index(typeid(p))`, where `p` is a reference to an instance of a sub-type of
   * Low_lvl_packet (or, in English, by packet type).
   */
  std::map<std::type_index, uint64_t> m_low_lvl_packet_size_by_type;

  /**
   * Count of low-level packets received targeted at this socket, split up by packet type similarly to
   * #m_low_lvl_packet_size_by_type.
   */
  std::map<std::type_index, uint64_t> m_low_lvl_packet_count_by_type;
}; // struct Peer_socket_receive_stats

/**
 * A data store that keeps stats about the outgoing direction of a Peer_socket connection to another
 * Flow-protocol Peer_socket.  Note that this also includes stats about *incoming* traffic that facilitates
 * further outgoing traffic (namely, data on incoming acknowledgments are in this class).
 *
 * All other comments in Peer_socket_receive_stats doc header apply equally to this class.
 *
 * @see Peer_socket_receive_stats
 */
class Peer_socket_send_stats
{
public:
  // Constructors/destructor.

  /// Constructs object by initializing stats to their initial values.
  explicit Peer_socket_send_stats();

  // Methods.

  /**
   * Outputs the current stats, across multiple lines but not ending with a newline, into the given
   * output stream.  `operator<<()` uses this and is more likely to be used directly by the class
   * user.
   *
   * @param os
   *        Stream to which to serialize.
   */
  void output(std::ostream* os) const;

  // Data.

  /**
   * The time this object was created; should be about equal to when the socket was created.

   * @todo Peer_socket_receive_stats also independently stores a similar value, so to save memory
   * put #m_init_time elsewhere.
   */
  Fine_time_pt m_init_time;

  /// Total bytes sent in DATA packets.
  uint64_t m_sent_data_size;
  /// Number of packets sent in DATA packets.
  uint64_t m_sent_data_count;

  /// Of #m_sent_data_size, the data that were retransmitted (as opposed to original) data.
  uint64_t m_sent_rexmitted_data_size;
  /// Of #m_sent_data_count, the data that were retransmitted (as opposed to original) data.
  uint64_t m_sent_rexmitted_data_count;

  /// Number of low-level ACK packets received.
  uint64_t m_received_low_lvl_ack_count;

  /**
   * Of #m_received_low_lvl_ack_count, the packets that contained no individual acknowledgments
   * but sent only to advertise rcv_wnd.
   */
  uint64_t m_received_low_lvl_rcv_wnd_only_ack_count;

  /// Individual acknowledgments inside low-level ACK packets that have been received (not necessarily all valid).
  uint64_t m_received_ack_count;

  /// Whether the last rcv_wnd update from other side indicated window size < max-block-size bytes.  `false` initially.
  bool m_rcv_wnd_exhausted;

  /// Number of times #m_rcv_wnd_exhausted changed from `false`.
  uint64_t m_remote_rcv_wnd_exhaustion_events;

  /// Number of times #m_rcv_wnd_exhausted changed from `true` (not counting it being initially set to `false`).
  uint64_t m_remote_rcv_wnd_recovery_events;

  /**
   * Of the individual acknowledgments covered by #m_received_ack_count, the total bytes in the DATA
   * packets that were properly acknowledged and converted from In-flight to Acknowledged status.
   */
  uint64_t m_good_ack_data_size;
  /**
   * Of #m_received_ack_count, those packets that were properly acknowledged and converted from In-flight to
   * Acknowledged status.
   */
  uint64_t m_good_ack_count;

  /**
   * Of #m_received_ack_count, those packets that had either already been convered from In-flight to
   * Acknowledged or (more likely) already been converted from In-flight to Dropped.
   */
  uint64_t m_late_or_dupe_ack_count;

  /**
   * Of #m_received_ack_count, those acknowledgments that contained some error about the sequence
   * numbers so that they were not acceptable, and the connection did/will close as a result.
   */
  uint64_t m_error_acks_data_count;

  /**
   * Total bytes in sent DATA packets that are considered permanently dropped (i.e., whose status
   * has changed from In-flight to Dropped).
   */
  uint64_t m_dropped_data_size;
  /**
   * Number of sent DATA packets that are considered permanently dropped (i.e., whose status
   * has changed from In-flight to Dropped).
   */
  uint64_t m_dropped_data_count;

  /**
   * Number of loss events so far (not counting Drop Timeouts) that have been reported to congestion
   * control module.  (Note that multiple individual acknowledgments, and even multiple ACKs, can
   * belong to one loss event and would only be counted once in thise case.)
   */
  uint64_t m_loss_events;

  /**
   * Number of idle timeouts (in the send direction) so far that have been detected and indicated to
   * congestion control module.
   */
  uint64_t m_idle_timeouts;

  /// Number of times Drop Timer has fired so far.
  uint64_t m_drop_timeouts;

  /// Maximum number of bytes in the Send buffer so far.
  size_t m_max_buf_data_size;

  /**
   * Total count of low-level packets that were given to the sending module to be sent, subject to pacing if applicable,
   * split up by packet type similarly to Peer_socket_receive_stats::m_low_lvl_packet_size_by_type.
   */
  std::map<std::type_index, uint64_t> m_low_lvl_packet_xfer_requested_count_by_type;

  /**
   * Total size in serialized form of low-level packets that were given to OS in a send call, from this socket,
   * split up by packet type similarly to Peer_socket_receive_stats::m_low_lvl_packet_size_by_type.
   */
  std::map<std::type_index, boost::array<uint64_t, 2>> m_low_lvl_packet_xfer_called_size_by_type;

  /**
   * Count of low-level packets that were given to OS in a send call, from this socket,
   * split up by packet type similarly to Peer_socket_receive_stats::m_low_lvl_packet_size_by_type.
   */
  std::map<std::type_index, boost::array<uint64_t, 2>> m_low_lvl_packet_xfer_called_count_by_type;

  /**
   * Of #m_low_lvl_packet_xfer_called_size_by_type, the data for which the send completion handler executed,
   * split up further by the transfer result reported to the handler.
   */
  std::map<std::type_index, std::map<Xfer_op_result, uint64_t>> m_low_lvl_packet_xfer_completed_size_by_type_and_result;

  /**
   * Of #m_low_lvl_packet_xfer_called_count_by_type, the data for which the send completion handler executed,
   * split up further by the transfer result reported to the handler.
   */
  std::map<std::type_index, std::map<Xfer_op_result, uint64_t>>
    m_low_lvl_packet_xfer_completed_count_by_type_and_result;
}; // class Peer_socket_send_stats

/**
 * A data store that keeps stats about the a Peer_socket connection.
 * This includes a Peer_socket_receive_stats `struct`, a Peer_socket_send_stats `struct`, and a few
 * other individual pieces of data.  Typically the former 2 accumulate data, while the other members
 * may go up or down.  This is not a hard rule however.
 *
 * The objects also have an `ostream` output interface for human-readability, e.g., for logs.
 *
 * The user can obtain objects representing actual stats via Peer_socket::info().
 *
 * ### Thread safety ###
 * Same as any `struct` with no locking done therein.
 *
 * @warning It is very heavily discouraged to make algorithmic decisions based on the data in these
 * objects.  They are for human diagnostics, information, and debugging only.  For example, some data
 * represented may pertain to internal implementation; other data may not be up-to-date; etc.
 *
 * @internal
 *
 * This `struct` is only really used to communicate info to users; it bundles up some `struct`s and a
 * few other pieces of data into one thing.  It is not stored internally in thread W.
 *
 * Because they're user-exposed types here, #Fine_duration and #Fine_time_pt are re-aliased explicitly.
 */
struct Peer_socket_info
{
  // Types.

  /// Short-hand for a fine boost.chrono time duration type.
  using Fine_duration = flow::Fine_duration;

  /// Short-hand for a fine boost.chrono time point type.
  using Fine_time_pt = flow::Fine_time_pt;

  // Constructors/destructor.

  /// Constructs object by initializing stats to their initial values.
  explicit Peer_socket_info();

  // Methods.

  /**
   * Outputs the current stats, across multiple lines but not ending with a newline, into the given
   * output stream.  `operator<<()` uses this and is more likely to be used directly by the class
   * user.
   *
   * @param os
   *        Stream to which to serialize.
   */
  void output(std::ostream* os) const;

  /**
   * Helper for various `output()` methods that outputs a one-line representation of a sorted map,
   * with arbitrary keys and values.  This representation will lack newlines and will include a wrapping
   * pair of brackets `"[]"`.
   *
   * @tparam Key
   *         Type such that `output_pkt_count_key(os, k)`, with `Key k`, will output a one-line representation
   *         of `k` to `*os`.  The representation should include wrapping brackets if desired.
   * @tparam Value
   *         Analogous to `Key` but the function is `output_pkt_count_value(os, v)`.
   * @param os
   *        See output().
   * @param count_by_type
   *        Map whose values represent -- at some level (not necessarily directly) -- a packet count.
   * @param size_by_type
   *        Map whose values represent a total byte count in packets counted by `count_by_type`.
   *        null pointer if no such map applies (i.e., only a count is available).
   */
  template<typename Key, typename Value>
  static void output_map_of_pkt_counts(std::ostream* os,
                                       const std::map<Key, Value>& count_by_type,
                                       const std::map<Key, Value>* size_by_type);

  // Data.

  /// Stats for incoming direction of traffic.  As opposed to the other `m_rcv_*` members, this typically accumulates.
  Peer_socket_receive_stats m_rcv;

  /// Stats for outgoing direction of traffic.  As opposed to the other `m_snd_*` members, this typically accumulates.
  Peer_socket_send_stats m_snd;

  /**
   * The UDP receive buffer maximum size, as reported by an appropriate call to the appropriate
   * `getsockopt()` immediately after the Node sets the UDP receive buffer size via `setsockopt()`.
   * The set value is determined by a socket option, but the OS may or may not respect that value.
   *
   * This value remains constant and the same for all Peer_socket objects for a given Node.  It is set at
   * Peer_socket creation.
   *
   * @internal
   *
   * @todo This is really a `Node`-wide value; we should probably have a `Node_info` class for such
   * things.
   */
  size_t m_low_lvl_max_buf_size;

  /**
   * The internal state of the socket, rendered into string (e.g., "SYN_RECEIVED" or "ESTABLISHED").
   * Reminder: this is for informational purposes only -- for human consumption; do not try to base
   * program code on this value under any circumstances.
   */
  std::string m_int_state_str;

  /// `true` if this is the "client" socket (`connect()`ed); `false` otherwise (`accept()`ed).
  bool m_is_active_connect;

  /// If the socket is closing or closed, this is the reason for the closure; otherwise the default-constructed value.
  Error_code m_disconnect_cause;

  /**
   * The number of bytes in the internal Receive buffer.  These are the bytes intended for the
   * application layer's consumption that the application layer has not yet claimed with
   * Peer_socket::receive() or similar method.  When this exceeds the Receive buffer max size,
   * incoming bytes will be dropped (which will cause at least the slowdown of traffic).
   *
   * Same reminder as for #m_int_state_str applies here.
   */
  size_t m_rcv_buf_size;

  /// Receive window size = max Receive buffer space minus space taken.  Infinity if flow control disabled.
  size_t m_rcv_wnd;

  /// The last rcv_wnd (receive window) size sent to sender (not necessarily received; packets can be lost).
  size_t m_rcv_wnd_last_advertised;

  /// If `rexmit_on` is `false` then 0; otherwise the total DATA payload in the reassembly queue of the socket.
  size_t m_rcv_reassembly_q_data_size;

  /**
   * Number of DATA packets tracked in structure tracking all valid received packets such at least one packet
   * that has not yet been received precedes (by sequence #) each one (in other words, all post-first-gap packets).
   * If `rexmit_on` is `true`, this is the reassembly queue length; otherwise is is length of structure used to
   * detect duplicate receipt of data past the first gap (and possibly other accounting purposes).
   * An important subtlety is that in the latter case, as of this writing, there is no need to (and hence
   * we don't) store the actual payload of those packets but only metadata like sequence numbers.
   * That's why in that case #m_rcv_reassembly_q_data_size is zero and doesn't, in particular, count against
   * rcv_wnd.  (However, if `rexmit_on`, then it's not zero and does count.)
   */
  size_t m_rcv_packets_with_gaps;

  /// Total size of DATA payload queued while waiting for SYN_ACK_ACK in SYN_RCVD state.
  size_t m_rcv_syn_rcvd_data_cumulative_size;

  /// Number of DATA packets queued while waiting for SYN_ACK_ACK in SYN_RCVD state.
  size_t m_rcv_syn_rcvd_data_q_size;

  /**
   * The number of bytes in the internal Send buffer.  These are the bytes provided by the
   * application layer to send that have not yet been "sent over the network," for some definition
   * of "sent over the network."  The latter definition is not entirely unambiguous, but that should
   * not matter to the library user.
   *
   * When this exceeds the Send buffer max size, Peer_socket::send() and similar methods will return
   * 0 (meaning, cannot accept more bytes to send at this time).  That should happen only if some
   * aspect of networking is preventing Node from sending more data over network at this time.
   *
   * Same reminder as for #m_int_state_str applies here.
   */
  size_t m_snd_buf_size;

  /**
   * The receive window (rcv_wnd a/k/a free Receive buffer space) value of the peer socket on the
   * other side, last advertised by the other side.
   */
  size_t m_snd_rcv_wnd;

  /// In congestion control, the current congestion window (number of outgoing data bytes allowed In-flight currently).
  size_t m_snd_cong_ctl_wnd_bytes;

  /// In congestion control, the approximate equivalent of #m_snd_cong_ctl_in_flight_bytes as a full packet count.
  size_t m_snd_cong_ctl_wnd_count_approx;

  /**
   * In congestion control, the current sent data bytes that have been neither acknowledged nor considered lost
   * @todo Does #m_snd_cong_ctl_in_flight_bytes count data queued in the pacing module or truly In-flight data only?
   */
  size_t m_snd_cong_ctl_in_flight_bytes;

  /// In congestion control, the current sent data packets that have been neither acknowledged nor considered lost.
  size_t m_snd_cong_ctl_in_flight_count;

  /// Estimated current round trip time of packets, computed as a smooth value over the past individual RTTs.
  Fine_duration m_snd_smoothed_round_trip_time;

  /// RTTVAR used for #m_snd_smoothed_round_trip_time calculation; it is the current RTT variance.
  Fine_duration m_snd_round_trip_time_variance;

  /// Drop Timeout: how long a given packet must remain unacknowledged to be considered dropped due to Drop Timeout.
  Fine_duration m_snd_drop_timeout;

  /// In pacing, number of packets currently queued to be sent out by the pacing module.
  size_t m_snd_pacing_packet_q_size;

  /// In pacing, the time point marking the beginning of the current pacing time slice.
  Fine_time_pt m_snd_pacing_slice_start;

  /// In pacing, the duration of the current pacing time slice.
  Fine_duration m_snd_pacing_slice_period;

  /**
   * This many bytes worth of DATA packets may still be sent, at this time, within the time slice
   * defined by `m_slice_start` and `m_slice_period`.
   */
  size_t m_snd_pacing_bytes_allowed_this_slice;

  /**
   * Estimate of the currently available (to this connection) outgoing bandwidth, in megabits per
   * second.  A megabit is 1000 x 1000 bits (not 1024 x 1024, as people sometimes assume, that being a mebibit).
   */
  double m_snd_est_bandwidth_mbit_per_sec;

  /// Per-socket options currently set on the socket.
  Peer_socket_options m_sock_opts;

  /**
   * Per-node options currently set on the socket's Node.  Note that this includes the full set of per-socket
   * options as well, but these will only potentially affect future sockets generated by the Node, not this socket.
   * For the latter, see #m_sock_opts.
   */
  Node_options m_node_opts;

private:
  /**
   * Helper for output_map_of_pkt_counts() that outputs a count or size.
   *
   * @param os
   *        See output().
   * @param value
   *        Value.
   */
  static void output_pkt_count_value(std::ostream* os, uint64_t value);

  /**
   * Helper for output_map_of_pkt_counts() that outputs a pair of counts or sizes: the first for non-delayed
   * packets; the second for ones delayed by pacing.
   *
   * @param os
   *        See output().
   * @param value_by_delay_type
   *        Value.
   */
  static void output_pkt_count_value(std::ostream* os, const boost::array<uint64_t, 2>& value_by_delay_type);

  /**
   * Helper for output_map_of_pkt_counts() that outputs a mapping of counts or sizes, spliut up by the result
   * of the transfer operation to which the count or size applies.  Subtlety: If the value is a size *sent*,
   * it should refer to the number that was EXPECTED to be sent (as opposed to the amount actually sent
   * as reported by OS, which may report a number less than that expected or imply 0 by generating error).
   *
   * @param os
   *        See output().
   * @param value_by_op_result
   *        Value.
   */
  static void output_pkt_count_value(std::ostream* os,
                                     const std::map<Xfer_op_result, uint64_t>& value_by_op_result);

  /**
   * Helper for output_map_of_pkt_counts() that outputs a raw packet type ID index, namely `type_index(typeid(p))`,
   * where `p` is a reference to an instance of a concrete Low_lvl_packet sub-type.
   *
   * @param os
   *        See output().
   * @param type_id
   *        Value.
   */
  static void output_pkt_count_key(std::ostream* os, const std::type_index& type_id);

  /**
   * Helper for output_map_of_pkt_counts() that outputs a transfer operation result.
   *
   * @param os
   *        See output().
   * @param op_result
   *        Value.
   */
  static void output_pkt_count_key(std::ostream* os, Xfer_op_result op_result);
}; // struct Peer_socket_info

// Free functions: in *_fwd.hpp.

} // namespace flow::net_flow
