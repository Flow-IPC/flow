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
#include "flow/net_flow/info.hpp"
#include "flow/net_flow/detail/low_lvl_packet.hpp"
#include <boost/array.hpp>

namespace flow::net_flow
{

// Peer_socket_receive_stats implementations.

Peer_socket_receive_stats::Peer_socket_receive_stats() :
  m_init_time(Fine_clock::now()),
  m_total_data_size(0),
  m_total_data_count(0),
  m_good_data_size(0),
  m_good_data_count(0),
  m_good_data_accepted_size(0),
  m_good_data_accepted_count(0),
  m_good_data_delivered_size(0),
  m_good_data_delivered_count(0),
  m_good_data_first_qd_size(0),
  m_good_data_first_qd_count(0),
  m_good_data_dropped_buf_overflow_size(0),
  m_good_data_dropped_buf_overflow_count(0),
  m_good_data_dropped_reassembly_q_overflow_size(0),
  m_good_data_dropped_reassembly_q_overflow_count(0),
  m_error_data_size(0),
  m_error_data_count(0),
  m_late_or_dupe_data_size(0),
  m_late_or_dupe_data_count(0),
  m_presumed_dropped_data_size(0),
  m_total_to_send_acks_data_size(0),
  m_total_to_send_acks_count(0),
  m_good_to_send_acks_data_size(0),
  m_good_to_send_acks_count(0),
  m_late_or_dupe_to_send_acks_data_size(0),
  m_late_or_dupe_to_send_acks_count(0),
  m_current_pending_acks_count(0),
  m_delayed_acks_count(0),
  m_sent_individual_acks_count(0),
  m_sent_low_lvl_acks_count(0),
  m_rcv_wnd_recovery_count(0),
  m_rcv_wnd_recovery_success_count(0),
  m_rcv_wnd_recovery_timeout_count(0),
  m_sent_low_lvl_rcv_wnd_only_acks_count(0),
  m_max_buf_data_size(0)
{
  // Nothing.
}

void Peer_socket_receive_stats::output(std::ostream* os) const
{
  using boost::chrono::duration;
  using boost::chrono::duration_cast;
  using boost::chrono::seconds;
  using Float_seconds = duration<double, seconds::period>;

  // Use a floating point representation for socket lifetime so that it can be printed in floating-point seconds.
  const Float_seconds lifetime = duration_cast<Float_seconds>(Fine_clock::now() - m_init_time);

  /* Compute the drop rate based on the # of bytes in unreceived packets that had been unreceived
   * for so long we consider them to be forever gone; this is divided by that plus the # of data
   * bytes that were received normally.  The latter does include any data we dropped due to
   * insufficient buffer space, as we're trying to measure the network drop rate which is, loosely,
   * a function of the network and the congestion control algorithm's behavior
   * on the sender side. */
  const double den = double(m_presumed_dropped_data_size + m_good_data_size);
  const double drop_pct
    = (den == double(0))
        ? 0
        : (double(m_presumed_dropped_data_size) * double(100) / den);

  *os
    << "[rcv] format of sizes: [bytes|packets]\n"
       "[rcv] socket_lifetime [" << lifetime << "]\n"
       "[rcv] lifetime_goodput_delivered ["
    << util::to_mbit_per_sec<Float_seconds>(double(m_good_data_delivered_size) / lifetime.count()) << " Mbit/s]\n"
       "[rcv] total_data [" << m_total_data_size << '|' << m_total_data_count << "]\n"

       "[rcv]   good [" << m_good_data_size << '|' << m_good_data_count << "]\n"

       "[rcv]     accepted [" << m_good_data_accepted_size << '|' << m_good_data_accepted_count << "]\n"

       "[rcv]       delivered [" << m_good_data_delivered_size << '|' << m_good_data_delivered_count << "]\n"

       "[rcv]         immediately [" << (m_good_data_delivered_size - m_good_data_first_qd_size)
    << '|' << (m_good_data_delivered_count - m_good_data_first_qd_count) << "] "
       "first_queued [" << m_good_data_first_qd_size << '|' << m_good_data_first_qd_count << "]\n"

       "[rcv]       queued [" << (m_good_data_accepted_size - m_good_data_delivered_size)
    << '|' << (m_good_data_accepted_count - m_good_data_delivered_count) << "]\n"


       "[rcv]     dropped_buf_overflow [" << m_good_data_dropped_buf_overflow_size
    << '|' << m_good_data_dropped_buf_overflow_count << "]\n"

       "[rcv]     dropped_reassembly_q_overflow [" << m_good_data_dropped_reassembly_q_overflow_size
    << '|' << m_good_data_dropped_reassembly_q_overflow_count << "]\n"

       "[rcv]   late_or_dupe [" << m_late_or_dupe_data_size << '|' << m_late_or_dupe_data_count << "]\n"

       "[rcv]   error [" << m_error_data_size << '|' << m_error_data_count << "]\n"

       "[rcv] presumed_dropped [" << m_presumed_dropped_data_size << " bytes]\n"

       "[rcv]   observed_net_drop_rate = presumed_dropped / (presumed_dropped + good) = [" << drop_pct << "%]\n"

       "[rcv] total_to_send_acks_data [" << m_total_to_send_acks_data_size
    << '|' << m_total_to_send_acks_count << "]\n"

       "[rcv]   good [" << m_good_to_send_acks_data_size
    << '|' << m_good_to_send_acks_count << "]\n"

       "[rcv]   late_or_dupe [" << m_late_or_dupe_to_send_acks_data_size
    << '|' << m_late_or_dupe_to_send_acks_count << "]\n"

       "[rcv] delayed_acks [" << m_delayed_acks_count << " individual acknowledgments]\n"

       "[rcv] current_pending_acks [" << m_current_pending_acks_count << " individual acknowledgments]\n"

       "[rcv] sent_individual_acks [" << m_sent_individual_acks_count << " individual acknowledgments]\n"

       "[rcv] sent_low_lvl_acks [" << m_sent_low_lvl_acks_count << " packets]\n"

       "[rcv]   rcv_wnd_update_only_acks [" << m_sent_low_lvl_rcv_wnd_only_acks_count << " packets]\n"

       "[rcv] rcv_wnd_recovery_events [" << m_rcv_wnd_recovery_count << "]\n"

       "[rcv]   successful [" << m_rcv_wnd_recovery_success_count << "] "
       "timed_out [" << m_rcv_wnd_recovery_timeout_count << "] "
       "in_progress ["
    << (m_rcv_wnd_recovery_count - m_rcv_wnd_recovery_success_count - m_rcv_wnd_recovery_timeout_count) << "]\n"

       "[rcv] max_buf_data_size [" << m_max_buf_data_size << " bytes]\n"
    << "[rcv] low_lvl_packets (AFTER simulation): ";

    Peer_socket_info::output_map_of_pkt_counts(os, m_low_lvl_packet_count_by_type, &m_low_lvl_packet_size_by_type);
} // Peer_socket_receive_stats::output()

std::ostream& operator<<(std::ostream& os, const Peer_socket_receive_stats& stats)
{
  stats.output(&os);
  return os;
}

// Peer_socket_send_stats implementations.

Peer_socket_send_stats::Peer_socket_send_stats() :
  m_init_time(Fine_clock::now()),
  m_sent_data_size(0),
  m_sent_data_count(0),
  m_sent_rexmitted_data_size(0),
  m_sent_rexmitted_data_count(0),
  m_received_low_lvl_ack_count(0),
  m_received_low_lvl_rcv_wnd_only_ack_count(0),
  m_received_ack_count(0),
  m_rcv_wnd_exhausted(false),
  m_remote_rcv_wnd_exhaustion_events(0),
  m_remote_rcv_wnd_recovery_events(0),
  m_good_ack_data_size(0),
  m_good_ack_count(0),
  m_late_or_dupe_ack_count(0),
  m_error_acks_data_count(0),
  m_dropped_data_size(0),
  m_dropped_data_count(0),
  m_loss_events(0),
  m_idle_timeouts(0),
  m_drop_timeouts(0),
  m_max_buf_data_size(0)
{
  // Nothing.
}

void Peer_socket_send_stats::output(std::ostream* os) const
{
  using boost::chrono::duration;
  using boost::chrono::duration_cast;
  using boost::chrono::seconds;
  using std::map;
  using std::type_index;

  using Float_seconds = duration<double, seconds::period>;

  // Use a floating point representation for socket lifetime so that it can be printed in floating-point seconds.
  const Float_seconds lifetime = duration_cast<Float_seconds>(Fine_clock::now() - m_init_time);

  /* Compute the loss rate based based on Dropped vs. Acknowledged bytes.  In-flight bytes are
   * considered as having unknown status for now and are not included.  If a Dropped packet is later
   * acknowledged, it has still already been Dropped, so it will still be only counted as Dropped.
   * However there is a separate statistic for such late acknowledgments. */
  double den = double(m_dropped_data_size + m_good_ack_data_size);
  const double drop_pct
    = (den == double(0))
        ? 0
        : (double(m_dropped_data_size) * double(100) / den);
  // Compute % of retransmitted vs. all sent DATA packets.
  den = double(m_sent_data_size);
  const double rexmit_pct
    = (den == double(0))
        ? 0
        : (double(m_sent_rexmitted_data_size) * double(100) / den);

  *os
    << "[snd] format of sizes: [bytes|packets]\n"

       "[snd] socket_lifetime [" << lifetime << "]\n"

       "[snd] lifetime_data_sent "
       "[" << util::to_mbit_per_sec<Float_seconds>(double(m_sent_data_size) / lifetime.count()) << " Mbit/s]\n"

       "[snd] sent_data [" << m_sent_data_size << '|' << m_sent_data_count << "]\n"

       "[snd]   acked [" << m_good_ack_data_size << '|' << m_good_ack_count << "] "
       "dropped [" << m_dropped_data_size << '|' << m_dropped_data_count << "]\n"

       "[snd]     observed_net_drop_rate = dropped / (acked + dropped) = [" << drop_pct << "%]\n"

       "[snd]   in_flight [" << (m_sent_data_size - m_good_ack_data_size - m_dropped_data_size)
    << '|' << (m_sent_data_count - m_good_ack_count - m_dropped_data_count) << "]\n"

       "[snd]   original [" << (m_sent_data_size - m_sent_rexmitted_data_size)
    << '|' << (m_sent_data_count - m_sent_rexmitted_data_count) << "] "
       "rexmitted [" << m_sent_rexmitted_data_size << '|' << m_sent_rexmitted_data_count << "]\n"

       "[snd]     rexmit_rate = rexmitted / sent = [" << rexmit_pct << "%]\n"

       "[snd] received_low_lvl_acks [" << m_received_low_lvl_ack_count << " packets]\n"

       "[snd]   rcv_wnd_update_only_acks [" << m_received_low_lvl_rcv_wnd_only_ack_count << " packets]\n"

       "[snd] received_acks [" << m_received_ack_count << " individual acknowledgments]\n"

       "[snd]   good [" << m_good_ack_count << " individual acknowledgments]\n"

       "[snd]   late_or_dupe [" << m_late_or_dupe_ack_count << " individual acknowledgments]\n"

       "[snd]   error [" << m_error_acks_data_count << " individual acknowledgments]\n"

       "[snd] loss_events [" << m_loss_events << "]\n"

       "[snd] drop_timeouts [" << m_drop_timeouts << "]\n"

       "[snd] idle_timeouts [" << m_idle_timeouts << "]\n"

       "[snd] remote_rcv_wnd_exhaustion_events [" << m_remote_rcv_wnd_exhaustion_events << "]\n"

       "[snd]   recovered [" << m_remote_rcv_wnd_recovery_events << "]\n"

       "[snd] max_buf_data_size [" << m_max_buf_data_size << " bytes]\n"
       "[snd] low_lvl_packet_send_requested ";

  Peer_socket_info::output_map_of_pkt_counts(os, m_low_lvl_packet_xfer_requested_count_by_type,
                                             static_cast<const map<type_index, uint64_t>*>(0));

  *os
    << "]\n" <<
       "[snd]   send_called ";

  Peer_socket_info::output_map_of_pkt_counts(os, m_low_lvl_packet_xfer_called_count_by_type,
                                             &m_low_lvl_packet_xfer_called_size_by_type);

  *os
    << "\n" <<
       "[snd]     send_completed ";

  Peer_socket_info::output_map_of_pkt_counts(os, m_low_lvl_packet_xfer_completed_count_by_type_and_result,
                                             &m_low_lvl_packet_xfer_completed_size_by_type_and_result);
}

std::ostream& operator<<(std::ostream& os, const Peer_socket_send_stats& stats)
{
  stats.output(&os);
  return os;
}

// Peer_socket_info implementations.

Peer_socket_info::Peer_socket_info() :
  m_low_lvl_max_buf_size(0),
  m_is_active_connect(false),
  m_rcv_buf_size(0),
  m_rcv_wnd(0),
  m_rcv_wnd_last_advertised(0),
  m_rcv_reassembly_q_data_size(0),
  m_rcv_packets_with_gaps(0),
  m_rcv_syn_rcvd_data_cumulative_size(0),
  m_rcv_syn_rcvd_data_q_size(0),
  m_snd_buf_size(0),
  m_snd_rcv_wnd(0),
  m_snd_cong_ctl_wnd_bytes(0),
  m_snd_cong_ctl_wnd_count_approx(0),
  m_snd_cong_ctl_in_flight_bytes(0),
  m_snd_cong_ctl_in_flight_count(0),
  m_snd_smoothed_round_trip_time(0),
  m_snd_round_trip_time_variance(0),
  m_snd_drop_timeout(0),
  m_snd_pacing_packet_q_size(0),
  m_snd_pacing_slice_period(0),
  m_snd_pacing_bytes_allowed_this_slice(0),
  m_snd_est_bandwidth_mbit_per_sec(0)
{
  // Nothing.
}

void Peer_socket_info::output(std::ostream* os) const
{
  using boost::chrono::milliseconds;
  using boost::chrono::round;

  const Fine_duration& srtt = m_snd_smoothed_round_trip_time;
  const Fine_duration& rtt_var = m_snd_round_trip_time_variance;
  const Fine_duration& dto = m_snd_drop_timeout;

  *os
    <<
    "--- Basic socket state ---\n"
    "Internal state: [" << m_int_state_str << "].\n"
    "Client or server: [" << (m_is_active_connect ? "client" : "server") << "].\n"
    // Also visible below in the options but show it separately for convenience.
    "Reliability mode: [" << (m_sock_opts.m_st_rexmit_on ? "reliable/rexmit on" : "unreliable/rexmit off") << "].\n"
    "--- Buffers/queues/lists ----\n"
    "Receive window (free space): [" << m_rcv_wnd << "]\n"
    "  = limit\n"
    "  - Receive buffer size: [" << m_rcv_buf_size << "]\n"
    "  - reassembly queue total data size: [" << m_rcv_reassembly_q_data_size << "] (0 if rexmit off)\n"
    "      " << (m_sock_opts.m_st_rexmit_on ? "Reassembly queue" : "Dupe check list") <<
    " length: [" << m_rcv_packets_with_gaps << "].\n"
    "  Last advertised: [" << m_rcv_wnd_last_advertised << "].\n";

  // This condition is hard to notice otherwise but can explain hard-to-debug refusal to send any data.
  if ((m_rcv_wnd == 0) || (m_rcv_wnd_last_advertised == 0))
  {
    *os << "  ^-- CAUTION!  No space in [advertised] Receive buffer + reassembly queue!  "
           "Peer may think they cannot send to us!\n";
  }

  *os
    <<
    "DATA packets in SYN_RCVD queue: "
    "[" << m_rcv_syn_rcvd_data_cumulative_size << '|' << m_rcv_syn_rcvd_data_q_size << "].\n"
    "Send buffer size: [" << m_snd_buf_size << "].\n"
    "--- Peer's buffers ---\n"
    "Receive window (free Receive buffer + reassembly queue space): [" << m_snd_rcv_wnd << "].\n";

  // This condition is hard to notice otherwise but can explain otherwise hard-to-debug refusal to send any data.
  if (m_snd_rcv_wnd == 0)
  {
    *os << "^-- CAUTION!  No space in peer Receive buffer + reassembly queue!  "
           "Is that true, or did we never receive an update from peer?\n";
  }

  *os
    <<
    "--- Traffic stats (outgoing) ---\n" << m_snd << "\n"
    "--- Traffic stats (incoming) ---\n" << m_rcv << "\n"

    "--- Congestion control status ---\n"
    "Window: "
    "[" << m_snd_cong_ctl_wnd_bytes << '|' << m_snd_cong_ctl_wnd_count_approx << "].\n"
    "In-flight: "
    "[" << m_snd_cong_ctl_in_flight_bytes << '|' << m_snd_cong_ctl_in_flight_count << "].\n"

  // Truncate results to millisecond representation for readability.
    "--- RTT/DTO data ---\n"
    "Smoothed RTT: [" << round<milliseconds>(srtt) << " = " << srtt << "].\n"
    "RTT variance: [" << round<milliseconds>(rtt_var) << " = " << rtt_var << "].\n"
    "Drop Timeout: [" << round<milliseconds>(dto) << " = " << dto << "].\n"

    "--- Send pacing status ---\n"
    "Queue size: [" << m_snd_pacing_packet_q_size << "].\n"
    "Current time "
    "slice [epoch+" << round<milliseconds>(m_snd_pacing_slice_start.time_since_epoch()) << " "
    "over " << round<milliseconds>(m_snd_pacing_slice_period) << "]; "
    "currently in slice? = "
    "[" << util::in_closed_range(m_snd_pacing_slice_start,
                                 Fine_clock::now(),
                                 m_snd_pacing_slice_start + m_snd_pacing_slice_period) << "].\n"
    "Bytes still allowed in slice: [" << m_snd_pacing_bytes_allowed_this_slice << "].\n"

    "--- Send bandwidth data ---\n"
    "Estimated outgoing available bandwidth for socket: "
    "[" << m_snd_est_bandwidth_mbit_per_sec << " Mbit/s].\n"

    "--- Config ---\n" << m_sock_opts << m_node_opts <<

    "--- Other info ---\n"
    "OS-reported UDP receive buffer size: " << m_low_lvl_max_buf_size << '.';
}

template<typename Key, typename Value>
void Peer_socket_info::output_map_of_pkt_counts(std::ostream* os,
                                                const std::map<Key, Value>& count_by_type,
                                                const std::map<Key, Value>* size_by_type) // Static.
{
  // Output two maps with the same key set (but 2nd map may not exist, if null).
  *os << '[';

  size_t idx = 0;
  for (const auto& type_id_and_count : count_by_type)
  {
    // Common key; value from first map.
    const Key& type_id = type_id_and_count.first;
    const Value& value = type_id_and_count.second;

    output_pkt_count_key(os, type_id);
    *os << ":";

    if (size_by_type)
    {
      *os << '[';

      // Value from second map.  Type can be a simple scalar or something more complex, so use this function.
      output_pkt_count_value(os, size_by_type->find(type_id)->second);
      *os << "b|";
    }

    // Value from first map.  Similar to above.
    output_pkt_count_value(os, value);

    if (size_by_type)
    {
      *os << ']';
    }

    if (idx != count_by_type.size() - 1)
    {
      *os << ' ';
    }
    ++idx;
  }

  *os << ']';
}

void Peer_socket_info::output_pkt_count_value(std::ostream* os, uint64_t value) // Static.
{
  *os << value;
}

void Peer_socket_info::output_pkt_count_value(std::ostream* os,
                                              const boost::array<uint64_t, 2>& value_by_delay_type) // Static.
{
  const auto& value_no_delay = value_by_delay_type[0];
  const auto& value_paced = value_by_delay_type[1];
  const char* const NO_DELAY = "NO_DELAY";
  const char* const PACED = "PACED";

  if ((value_no_delay == 0) && (value_paced == 0))
  {
    *os << '0';
  }
  else if (value_no_delay == 0)
  {
    *os << '[' << PACED << ':' << value_paced << ']';
  }
  else if (value_paced == 0)
  {
    *os << '[' << NO_DELAY << ':' << value_no_delay << ']';
  }
  else
  {
    *os << '[' << NO_DELAY << ':' << value_no_delay << ' ' << PACED << ':' << value_paced << ']';
  }
}

void Peer_socket_info::output_pkt_count_value(std::ostream* os,
                                              const std::map<Xfer_op_result, uint64_t>& value_by_op_result) // Static.
{
  // This value is itself a map, so use recursion, in a manner of speaking.
  output_map_of_pkt_counts<Xfer_op_result, uint64_t>(os, value_by_op_result, 0);
}

void Peer_socket_info::output_pkt_count_key(std::ostream* os, const std::type_index& type_id) // Static.
{
  *os << Low_lvl_packet::type_id_to_str(type_id);
}

void Peer_socket_info::output_pkt_count_key(std::ostream* os, Xfer_op_result op_result) // Static.
{
  const auto result_to_str = [&]() -> const char*
  {
    switch (op_result)
    {
      case Xfer_op_result::S_FULLY_XFERRED: return "FULLY_XFERRED";
      case Xfer_op_result::S_PARTIALLY_XFERRED: return "PARTIALLY_XFERRED";
      case Xfer_op_result::S_ERROR: return "ERROR";
    }
    assert(false && "Should be unreachable; compiler should warn if incomplete switch.");
    return nullptr;
  };

  *os << result_to_str();
}

// Free function implementations.

std::ostream& operator<<(std::ostream& os, const Peer_socket_info& stats)
{
  stats.output(&os);
  return os;
}

} // namespace flow::net_flow
