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
#include "flow/net_flow/detail/cong_ctl.hpp"
#include "flow/net_flow/detail/cong_ctl/cong_ctl_util.hpp"
#include "flow/net_flow/detail/stats/bandwidth.hpp"

namespace flow::net_flow
{
// Types.

/**
 * Classic congestion control but with backoff to bandwidth estimate-based pipe size.
 * Implements Congestion_control_strategy abstract API.
 *
 * The basic idea is the same as in Congestion_control_classic.  See that class's doc header first.
 * Except where noted below, the algorithm is identical.
 *
 * The difference is that instead of halving (or otherwise lowering by an arbitrary constant factor)
 * the window, we set the window to a pipe width based on our estimate of the available bandwidth
 * and the inherent latency in the pipe.  The latter is tracked by keeping the minimum individual
 * packet RTT observed thus far; the former (bandwidth estimate) is obtained from
 * Send_bandwidth_estimator.  Normally bandwidth estimation would be inside the congestion control
 * strategy class, but since its results may be useful independently of congestion control, it is a
 * separate module.
 *
 * The specifics of the implementation are based on Westwood/Westwood+.  The part about computing
 * the pipe width based on B * RTTmin comes from the original Westwood paper.  The bandwidth
 * estimation algorithm is based on Westwood+ and is discussed in further detail in class
 * Send_bandwidth_estimator.  The reference implementation is Linux kernel's Westwood+
 * implementation; see Send_bandwidth_estimator doc header for references.
 *
 * The strength of this algorithm is that it is potentially less timid in the face of random loss as
 * opposed to true congestion, when compared to Congestion_control_classic (Reno).  It won't just halve
 * CWND at the slightest provocation.  On the other hand bandwidth estimation seems to, in my tests
 * at least, kind of measure the rate at which we're sending data and less the actual available
 * pipe.  Still, at least at lower latencies and loss rates it can greatly outperform
 * Congestion_control_classic; though at higher such values it can actually be worse (at least in my
 * crude tests -- more formal testing with real simulation TBD).
 *
 * @see Congestion_control_classic, Send_bandwidth_estimator.
 */
class Congestion_control_classic_with_bandwidth_est :
  public Congestion_control_strategy
{
public:
  // Constructors/destructor.

  /**
   * Constructs object by setting up logging and saving a pointer to the containing Peer_socket.
   * Congestion window is chosen to be some small initial value, and an exponential congestion
   * window growth is set up to continue indefinitely, until a loss event, drop timeout, or idle
   * timeout.
   *
   * Only a weak pointer of `sock` is stored: the `shared_ptr` itself is not saved, so the reference
   * count of `sock` does not increase.  This avoids a circular `shared_ptr` situation that would arise
   * from `*this` pointing to `sock`, and `sock` pointing to `*this` (the two objects *do* need access
   * to each other, as explained in class Congestion_control_strategy doc header).
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param sock
   *        The Peer_socket for which this module will control congestion policy.
   */
  Congestion_control_classic_with_bandwidth_est(log::Logger* logger_ptr, Peer_socket::Const_ptr sock);

  // Methods.

  /**
   * Implements Congestion_control_strategy::congestion_window_bytes() API.
   * @return See Congestion_control_strategy::congestion_window_bytes().
   */
  size_t congestion_window_bytes() const override;

  /**
   * Implements Congestion_control_strategy::on_individual_ack() API.  This results in no
   * congestion_window_bytes() change but may affect its later values.
   *
   * Internally this gauges the "true" latency of the pipe.
   *
   * @param packet_rtt
   *        See Congestion_control_strategy::on_individual_ack().
   * @param bytes
   *        See Congestion_control_strategy::on_individual_ack().
   * @param sent_cwnd_bytes
   *         See Congestion_control_strategy::on_individual_ack().
   */
  void on_individual_ack(const Fine_duration& packet_rtt, const size_t bytes, const size_t sent_cwnd_bytes) override;

  /**
   * Implements Congestion_control_strategy::on_acks() API.  Congestion window grows either linearly
   * (in congestion avoidance mode) or exponentially (in slow start mode).  There is no congestion
   * avoidance mode until at least one of on_loss_event() or on_drop_timeout() occurs; at least
   * until then slow start is in effect.
   *
   * Additional Peer_socket state used: none.
   *
   * @param bytes
   *        See Congestion_control_strategy::on_acks().
   * @param packets
   *        See Congestion_control_strategy::on_acks().
   */
  void on_acks(size_t bytes, size_t packets) override;

  /**
   * Implements Congestion_control_strategy::on_loss_event() API.  Congestion window changes
   * (probably drops) to a value based on the estimation of the available outgoing bandwidth, and
   * the next slow start (if any, e.g., on a later Drop Timeout) will end once the congestion window
   * exceeds this value.  Thus after this call congestion avoidance mode begins anew.
   *
   * Additional Peer_socket state used: none.
   *
   * @param bytes
   *        See Congestion_control_strategy::on_loss_event().
   * @param packets
   *        See Congestion_control_strategy::on_loss_event().
   */
  void on_loss_event(size_t bytes, size_t packets) override;

  /**
   * Implements Congestion_control_strategy::on_drop_timeout() API.  Congestion window is set to a
   * low value, while the next slow start session is set to end at a pipe width based on the
   * estimation of the available outgoing bandwidth.  Thus a slow start session, most
   * likely, begins.
   *
   * Internally this also may affect our gauge of the "true" latency of the pipe.
   *
   * Additional Peer_socket state used: none.
   *
   * @param bytes
   *        See Congestion_control_strategy::on_drop_timeout().
   * @param packets
   *        See Congestion_control_strategy::on_drop_timeout().
   */
  void on_drop_timeout(size_t bytes, size_t packets) override;

  /**
   * Implements Congestion_control_strategy::on_idle_timeout() API.  Congestion window is set to a
   * low value.  Thus a slow start session begins.
   *
   * Additional Peer_socket state used: none.
   */
  void on_idle_timeout() override;

private:
  // Methods.

  /**
   * Returns the best bandwidth/latency-based guess for what the current window (pipe) size should
   * be.
   *
   * @return A CWND/SSTHRESH-suitable value.
   */
  size_t congestion_window_adjusted_bytes() const;

  // Constants.

  /**
   * The initial value for #m_rtt_min, until one RTT measurement comes in.  Initialize to
   * conservatively large value.  I considered making this configurable, but it doesn't seem worth
   * it, as we'll get a real RTT sample with the very first ACK that arrives. Also the Linux
   * `tcp_westwood.c` reference implementation does the same thing (a hard constant).
   */
  static const Send_bandwidth_estimator::Time_unit S_INIT_RTT_MIN;

  // Data.

  /// The Reno CWND/SSTHRESH-changing engine and CWND/SSTHRESH storage.
  Congestion_control_classic_data m_classic_data;

  /**
   * The minimum individual packet's RTT observed so far; it should be reset to the next sample if
   * #m_reset_rtt_min is `true`, which may happen if our algorithm thinks the pipe has changed
   * drastically, making past measurements useless.
   */
  Send_bandwidth_estimator::Time_unit m_rtt_min;

  /**
   * If `true`, then when the next RTT measurement comes in, any past measurement should be
   * disregarded, and #m_rtt_min should be set to the new measurement.  Do this if the pipe may have
   * drastically changed, obsoleting any prior observations.
   */
  bool m_reset_rtt_min;
}; // class Congestion_control_classic_with_bandwidth_est

} // namespace flow::net_flow
