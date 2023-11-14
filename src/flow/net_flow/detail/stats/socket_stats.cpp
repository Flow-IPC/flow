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
#include "flow/net_flow/detail/stats/socket_stats.hpp"
#include <cassert>

namespace flow::net_flow
{

// Peer_socket_receive_stats implementations.

void Peer_socket_receive_stats_accumulator::total_data_packet(size_t data)
{
  ++m_total_data_count;
  m_total_data_size += data;
}

void Peer_socket_receive_stats_accumulator::good_data_packet(size_t data)
{
  ++m_good_data_count;
  m_good_data_size += data;
}

void Peer_socket_receive_stats_accumulator::good_data_dropped_buf_overflow_packet(size_t data)
{
  ++m_good_data_dropped_buf_overflow_count;
  m_good_data_dropped_buf_overflow_size += data;
}

void Peer_socket_receive_stats_accumulator::good_data_dropped_reassembly_q_overflow_packet(size_t data)
{
  ++m_good_data_dropped_reassembly_q_overflow_count;
  m_good_data_dropped_reassembly_q_overflow_size += data;
}

void Peer_socket_receive_stats_accumulator::good_data_accepted_packet(size_t data)
{
  ++m_good_data_accepted_count;
  m_good_data_accepted_size += data;
}

void Peer_socket_receive_stats_accumulator::good_data_delivered_packet(size_t data)
{
  ++m_good_data_delivered_count;
  m_good_data_delivered_size += data;
}

void Peer_socket_receive_stats_accumulator::good_data_first_qd_packet(size_t data)
{
  ++m_good_data_first_qd_count;
  m_good_data_first_qd_size += data;
}

void Peer_socket_receive_stats_accumulator::error_data_packet(size_t data)
{
  ++m_error_data_count;
  m_error_data_size += data;
}

void Peer_socket_receive_stats_accumulator::late_or_dupe_data_packet(size_t data)
{
  ++m_late_or_dupe_data_count;
  m_late_or_dupe_data_size += data;
}

void Peer_socket_receive_stats_accumulator::presumed_dropped_data(size_t data)
{
  m_presumed_dropped_data_size += data;
}

void Peer_socket_receive_stats_accumulator::total_to_send_ack_packet(size_t data)
{
  ++m_total_to_send_acks_count;
  m_total_to_send_acks_data_size += data;
}

void Peer_socket_receive_stats_accumulator::good_to_send_ack_packet(size_t data)
{
  ++m_good_to_send_acks_count;
  m_good_to_send_acks_data_size += data;
}

void Peer_socket_receive_stats_accumulator::late_or_dupe_to_send_ack_packet(size_t data)
{
  ++m_late_or_dupe_to_send_acks_count;
  m_late_or_dupe_to_send_acks_data_size += data;
}

void Peer_socket_receive_stats_accumulator::current_pending_to_ack_packets(size_t count)
{
  if (count == 0)
  {
    m_current_pending_acks_count = 0;
  }
  else
  {
    assert(count >= m_current_pending_acks_count); // Explicit pre-condition.

    /* If there were N pending individual acks at the end of the last handler (i.e., non-blocking
     * time period), and now there are N + M such pending individual acks (at the end of this
     * handler), then that means M more individual acks are being withheld from sending (a/k/a
     * delayed).  The N previous ones have already been counted in a preceding invocation. */

    m_delayed_acks_count += (count - m_current_pending_acks_count);
    m_current_pending_acks_count = count;
  }
}

void Peer_socket_receive_stats_accumulator::sent_individual_ack()
{
  ++m_sent_individual_acks_count;
}

void Peer_socket_receive_stats_accumulator::sent_low_lvl_ack_packet(bool rcv_wnd_update_only)
{
  ++m_sent_low_lvl_acks_count;
  if (rcv_wnd_update_only)
  {
    ++m_sent_low_lvl_rcv_wnd_only_acks_count;
  }
}

void Peer_socket_receive_stats_accumulator::rcv_wnd_recovery_event_start()
{
  ++m_rcv_wnd_recovery_count;
}

void Peer_socket_receive_stats_accumulator::rcv_wnd_recovery_event_finish(bool success)
{
  success ? (++m_rcv_wnd_recovery_success_count) : (++m_rcv_wnd_recovery_timeout_count);
}

void Peer_socket_receive_stats_accumulator::buffer_fed(size_t size)
{
  (m_max_buf_data_size >= size) || (m_max_buf_data_size = size);
}

void Peer_socket_receive_stats_accumulator::low_lvl_packet(const std::type_info& type, size_t size)
{
  using std::type_index;

  ++m_low_lvl_packet_count_by_type[type_index(type)];
  m_low_lvl_packet_size_by_type[type_index(type)] += size;
}

const Peer_socket_receive_stats& Peer_socket_receive_stats_accumulator::stats() const
{
  // Give them read-only access to the data portion of ourselves.
  return static_cast<const Peer_socket_receive_stats&>(*this);
}

// Peer_socket_send_stats_accumulator implementations.

void Peer_socket_send_stats_accumulator::data_sent(size_t data, bool rexmission)
{
  ++m_sent_data_count;
  m_sent_data_size += data;
  if (rexmission)
  {
    ++m_sent_rexmitted_data_count;
    ++m_sent_rexmitted_data_size += data;
  }
}

void Peer_socket_send_stats_accumulator::dropped_data(size_t data, size_t count)
{
  m_dropped_data_count += count;
  m_dropped_data_size += data;
}

void Peer_socket_send_stats_accumulator::received_low_lvl_ack_packet(bool rcv_wnd_update_only)
{
  ++m_received_low_lvl_ack_count;
  if (rcv_wnd_update_only)
  {
    ++m_received_low_lvl_rcv_wnd_only_ack_count;
  }
}

void Peer_socket_send_stats_accumulator::received_ack()
{
  ++m_received_ack_count;
}

void Peer_socket_send_stats_accumulator::updated_rcv_wnd(bool wnd_exhausted)
{
  if (wnd_exhausted == m_rcv_wnd_exhausted)
  {
    return;
  }
  // else

  m_rcv_wnd_exhausted = wnd_exhausted;
  wnd_exhausted ? (++m_remote_rcv_wnd_exhaustion_events) : (++m_remote_rcv_wnd_recovery_events);
}

void Peer_socket_send_stats_accumulator::good_ack(size_t data)
{
  ++m_good_ack_count;
  m_good_ack_data_size += data;
}

void Peer_socket_send_stats_accumulator::late_or_dupe_ack()
{
  ++m_late_or_dupe_ack_count;
}

void Peer_socket_send_stats_accumulator::error_ack()
{
  ++m_error_acks_data_count;
}

void Peer_socket_send_stats_accumulator::loss_event()
{
  ++m_loss_events;
}

void Peer_socket_send_stats_accumulator::drop_timeout()
{
  ++m_drop_timeouts;
}

void Peer_socket_send_stats_accumulator::idle_timeout()
{
  ++m_idle_timeouts;
}

void Peer_socket_send_stats_accumulator::buffer_fed(size_t size)
{
  (m_max_buf_data_size >= size) || (m_max_buf_data_size = size);
}

void Peer_socket_send_stats_accumulator::low_lvl_packet_xfer_requested(const std::type_info& type)
{
  using std::type_index;

  ++m_low_lvl_packet_xfer_requested_count_by_type[type_index(type)];
}

void Peer_socket_send_stats_accumulator::low_lvl_packet_xfer_called(const std::type_info& type, bool delayed_by_pacing,
                                                                    size_t size)
{
  using std::type_index;

  const auto type_idx = type_index(type);
  const size_t idx = delayed_by_pacing ? 1 : 0;
  ++m_low_lvl_packet_xfer_called_count_by_type[type_idx][idx];
  m_low_lvl_packet_xfer_called_size_by_type[type][idx] += size;
}

void Peer_socket_send_stats_accumulator::low_lvl_packet_xfer_completed(const std::type_info& type,
                                                                       size_t size_expected, size_t size_xferred)
{
  using std::numeric_limits;
  using std::type_index;

  const auto type_idx = type_index(type);
  const Xfer_op_result op_result
    = (size_expected == numeric_limits<size_t>::max())
        ? Xfer_op_result::S_ERROR
        : ((size_expected == size_xferred)
             ? Xfer_op_result::S_FULLY_XFERRED
             : Xfer_op_result::S_PARTIALLY_XFERRED);
  ++m_low_lvl_packet_xfer_completed_count_by_type_and_result[type_idx][op_result];
  m_low_lvl_packet_xfer_completed_size_by_type_and_result[type_idx][op_result] += size_expected;
}

const Peer_socket_send_stats& Peer_socket_send_stats_accumulator::stats() const
{
  // Give them read-only access to the data portion of ourselves.
  return static_cast<const Peer_socket_send_stats&>(*this);
}

} // namespace flow::net_flow
