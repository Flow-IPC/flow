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
#include "flow/net_flow/detail/cong_ctl/cong_ctl_classic_bw.hpp"

namespace flow::net_flow
{
// Static initializations.

// Use conservative start value which will be overridden with the next ACK.  (Value taken from Linux tcp_westwood.c.)
const Send_bandwidth_estimator::Time_unit Congestion_control_classic_with_bandwidth_est::S_INIT_RTT_MIN
  = boost::chrono::seconds(20);

// Implementations.

Congestion_control_classic_with_bandwidth_est::Congestion_control_classic_with_bandwidth_est
  (log::Logger* logger_ptr, Peer_socket::Const_ptr sock) :

  Congestion_control_strategy(logger_ptr, sock),
  // Initialize the basics (CWND, SSTHRESH, etc.) in the usual way.
  m_classic_data(logger_ptr, sock),
  // See comment on S_INIT_RTT_MIN declaration inside class.
  m_rtt_min(S_INIT_RTT_MIN),
  // Trigger S_INIT_RTT_MIN to be disregarded as soon as we get an ACK.
  m_reset_rtt_min(true)
{
  // Nothing.
}

size_t Congestion_control_classic_with_bandwidth_est::congestion_window_bytes() const // Virtual.
{
  return m_classic_data.congestion_window_bytes();
}

void Congestion_control_classic_with_bandwidth_est::on_individual_ack
       (const Fine_duration& packet_rtt,
        [[maybe_unused]] const size_t bytes, [[maybe_unused]] const size_t sent_cwnd_bytes) // Virtual.
{
  using boost::chrono::ceil;
  using boost::chrono::round;
  using boost::chrono::milliseconds;

  const Peer_socket::Const_ptr sock = socket();

  /* As in the reference implementation (Linux's tcp_westwood.c), trivially maintain the lowest
   * known individual RTT.  The only deviation from that is certain events can cause the existing
   * minimum RTT to be invalidated, starting a new sequence.
   *
   * Another caveat concerns units.  Due to the math in congestion_window_adjust(), we store
   * m_rtt_min in Send_bandwidth_estimator-friendly units, likely less fine than those used in
   * Fine_duration but fine enough (see that alias's doc header for discussion).  In converting,
   * round up as a convention, if only to try to avoid 0.  @todo Why is that really needed?
   * There's no division by this number.... */

  const Send_bandwidth_estimator::Time_unit packet_rtt_conv = ceil<Send_bandwidth_estimator::Time_unit>(packet_rtt);

  if (m_reset_rtt_min)
  {
    m_reset_rtt_min = false;
    m_rtt_min = packet_rtt_conv;

    FLOW_LOG_TRACE("cong_ctl|bw_est [" << sock << "] update: RTT event; reset rtt_min "
                   "to [" << round<milliseconds>(m_rtt_min) << "].");
  }
  else
  {
    const milliseconds prev_rtt_min = round<milliseconds>(m_rtt_min);

    if (packet_rtt_conv < m_rtt_min)
    {
      m_rtt_min = packet_rtt_conv;
    }

    FLOW_LOG_TRACE("cong_ctl|bw_est [" << sock << "] update: RTT event; set rtt_min = "
                   "min[" << prev_rtt_min
                   << ", " << round<milliseconds>(packet_rtt_conv) << "] "
                   "= [" << round<milliseconds>(m_rtt_min) << "].");
  }
} // Congestion_control_classic_with_bandwidth_est::on_individual_ack()

void Congestion_control_classic_with_bandwidth_est::on_acks([[maybe_unused]] size_t bytes,
                                                            [[maybe_unused]] size_t packets) // Virtual.
{
  // Same as Congestion_control_classic; we are only different upon loss detection.
  m_classic_data.on_acks(bytes); // Will log.
}

void Congestion_control_classic_with_bandwidth_est::on_loss_event([[maybe_unused]] size_t bytes,
                                                                  [[maybe_unused]] size_t packets) // Virtual.
{
  /* Node has detected at least some lost bytes (at least one dropped packet), or at least so it
   * thinks, but it's not a "major" (Drop Timeout-based) loss.  At its core, we follow classic Reno,
   * reset the available pipe (CWND) to some probably lower value; and set SSTHRESH to that same
   * value, so that we immediately begin congestion avoidance (slow linear increase in CWND), and so
   * that if we later enter slow start for whatever reason, it will end at this CWND value (unless
   * something else changes SSTHRESH before then of course).
   *
   * Of course this is not Reno but instead "classic with bandwidth estimation."  The difference is
   * that we set CWND not to 1/2 (or any other constant) of anything but rather to our best guess on
   * what the pipe can currently take.  The standard formula is CWND = B * RTT, where B is the
   * available bandwidth, and RTT is the delay of the pipe.  RTT is taken to be the minimum RTT
   * observed on the pipe, which should represent the "true" delay.  B is estimated using
   * Send_bandwidth_estimator.  Altogether this is the Westwood+ algorithm, but the main part of it
   * is the B estimation, which has been separated into Send_bandwidth_estimator, because its
   * results may be useful outside of congestion control.  For more info on Westwood+ see that
   * class's doc header.
   *
   * As a reference implementation of the CWND = B * RTT formula, we use Linux kernel's
   * tcp_westwood.c, which was implemented by the inventors of the algorithm. */

  const size_t new_wnd_bytes = congestion_window_adjusted_bytes();

  const Peer_socket::Const_ptr sock = socket();
  FLOW_LOG_TRACE("cong_ctl|bw_est [" << sock << "] update: loss event; "
                 "set sl_st_thresh=cong_wnd to [" << sock->bytes_blocks_str(new_wnd_bytes) << "].");

  m_classic_data.on_congestion_event(new_wnd_bytes, new_wnd_bytes); // Will log but not enough (so log the above).

  // Beyond that, nothing else.  See Congestion_control_classic comments in same spot which apply equally.
} // Congestion_control_classic_with_bandwidth_est::on_loss_event()

void Congestion_control_classic_with_bandwidth_est::on_drop_timeout([[maybe_unused]] size_t bytes,
                                                                    [[maybe_unused]] size_t packets) // Virtual.
{
  /* Node has detected a major loss event in the form of a Drop Timeout.  As in on_loss_event(), do
   * the same thing as Congestion_control_classic does except that SSTHRESH is not 1/2 of anything
   * but rather selected based on the bandwidth estimation.
   *
   * Additionally, a major loss event implies that the properties of the pipe may have changed
   * (e.g., different IP route).  Therefore we (as the reference implementation, Linux's
   * tcp_westwood.c) also cause the obsolescence of m_rtt_min.  We don't invalidate it now, since we
   * don't have a better guess yet, but we do set m_reset_rtt_min, which will cause the next RTT
   * measurement to override any existing minimum.
   *
   * Should we also reset the bandwidth estimator somehow?  The reference implementation does not,
   * so I won't either.  As data transfer picks up, the estimator should pick up any changes in
   * bandwidth gradually anyway, it seems.
   *
   * Other comments from Congestion_control_classic's same spot apply here. */

  // Determine SSTHRESH.

  const size_t new_slow_start_thresh_bytes = congestion_window_adjusted_bytes();

  const Peer_socket::Const_ptr sock = socket();
  FLOW_LOG_TRACE("cong_ctl|bw_est [" << sock << "] update: DTO event; set sl_st_thresh "
                 "to [" << sock->bytes_blocks_str(new_slow_start_thresh_bytes) << "]; "
                 "cong_wnd to minimal value; schedule rtt_min reset.");

  // Now set SSTHRESH and let m_classic_data set CWND to a low value (common to most congestion control strategies).

  // Will log but not enough (so log the above).
  m_classic_data.on_drop_timeout(new_slow_start_thresh_bytes);

  m_reset_rtt_min = true; // As explained above.
} // Congestion_control_classic_with_bandwidth_est::on_drop_timeout()

size_t Congestion_control_classic_with_bandwidth_est::congestion_window_adjusted_bytes() const
{
  using std::max;

  using Time_unit = Send_bandwidth_estimator::Time_unit;
  using n_bytes_t = Send_bandwidth_estimator::n_bytes_t;

  const Peer_socket::Const_ptr sock = socket();

  /* The basic formula is CWND = B * RTTmin.  Units and arithmetic are as follows.  B is in units of
   * bytes per Time_unit{1}.  (The selection of Time_unit{1} is discussed in detail in that
   * alias's doc header.)  RTTmin, accordingly, is in units of Time_unit.  Therefore we can simply
   * multiply the two values.
   *
   * Range issues: B is returned in n_bytes_t, which is potentially larger than size_t due to
   * Send_bandwidth_estimator choosing to store safer internal values that way and return them in
   * that format also.  After the multiplication we convert the result to size_t; assuming
   * reasonable bandwidths, this number should not overflow even a 32-bit size_t.  However B can
   * also be zero (either unavailable or just a very low bandwidth).  As is standard, put a floor of
   * 2 max-block-sizes on it to avoid craziness. */

  const n_bytes_t bytes_per_time = sock->m_snd_bandwidth_estimator->bandwidth_bytes_per_time();

  const size_t floor_wnd_bytes = 2 * sock->max_block_size();
  const size_t new_wnd_bytes = max(size_t(bytes_per_time * m_rtt_min.count()),
                                   floor_wnd_bytes);

  FLOW_LOG_TRACE("cong_ctl|bw_est [" << sock << "] info: window calculation: wnd "
                 "= bw x rtt_min = [" << sock->bytes_blocks_str(size_t(bytes_per_time)) << "] bytes "
                 "per [" << Time_unit{1} << "] x [" << m_rtt_min << "] "
                 "(subject to floor [" << sock->bytes_blocks_str(floor_wnd_bytes) << "]) "
                 "= [" << sock->bytes_blocks_str(new_wnd_bytes) << "].");

  return new_wnd_bytes;
}

void Congestion_control_classic_with_bandwidth_est::on_idle_timeout() // Virtual.
{
  /* Node has detected that nothing has been sent out by us for a while.  (Note that this is
   * different from a Drop Timeout.  A Drop Timeout causes data to be sent (either retransmitted [if
   * used] or new data), which resets the state back to non-idle for a while.)
   *
   * Do the same thing as Congestion_control_classic.
   *
   * It might actually seem like we should perhaps reset m_rtt_min here or reset the bandwidth
   * estimation, but the reference implementation (tcp_westwood.c in Linux) does no such thing on
   * idle timeout.  For m_rtt_min it does make sense to me; there's no reason to necessarily think
   * that just because the user hasn't sent anything in a while that the properties of the pipe
   * have changed.  For the bandwidth estimator, it also seems like some guess is better than no
   * guess, and as traffic picks up the data should correct itself gradually, but it's iffier.
   * Anyway, go with the reference implementation for now. */

  m_classic_data.on_idle_timeout();
} // Congestion_control_classic_with_bandwidth_est::on_idle_timeout()

} // namespace flow::net_flow
