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

namespace flow::net_flow
{
// Types.

/**
 * Classic congestion control, based on Reno (TCP RFC 5681), with congestion avoidance, slow start,
 * and congestion window halving (etc.) upon a loss event.  Implements Congestion_control_strategy
 * abstract API.
 *
 * The basic idea is: start with a narrow pipe.  With each acknowledgment, grow the width of the
 * pipe exponentially (a/k/a "slow start").  Eventually this will either hit the maximum width or
 * cause loss. Once loss is detected, guess that the "steady-state" pipe width is about half of that
 * at the point where loss occurred, set the pipe to that width and increase the pipe width but
 * slowly (linearly) as acknowledgments arrive (a/k/a "congestion avoidance").  Eventually this
 * will cause loss again: again guess pipe is half that width and start at that width and increase
 * linearly (congestion avoidance again).  Continue indefinitely.  This results in the
 * characteristic CWND vs. time graph, where CWND first explodes up, then goes up diagonally and
 * vertically down, up, down, up, down, like a saw edge.
 *
 * Additionally, on major loss (Drop Timeout), guess the steady state pipe width as above but reset
 * the actual pipe width to its narrowest value.  This will cause exponential growth again until the
 * steady state pipe width is reached, then linear growth.
 *
 * On idle timeout (when connection is seen as idle in the send direction), reset the pipe to its
 * original narrow width but make no change to the steady-state pipe width.
 *
 * The strength of this algorithm is that it's time-tested and saved the Internet from collapsing.
 * The weakness is that once it encounters loss -- which may well be random loss and not a sign of
 * congestion -- it backs off very conservatively (half).  This means in a somewhat lossy
 * environment one flow like this will fail to take advantage of the available bandwidth.
 *
 * @note The above refers to "half" the pipe width being used when backing off due to loss, as in classic Reno,
 *       but actually we make it configurable through socket options.
 *
 * @see Congestion_control_classic_with_bandwidth_est that acts similarly but instead of backing
 *      off conservatively to half (etc.) the pipe, backs off to an estimate as to the available
 *      pipe width based on a bandwidth estimator.
 */
class Congestion_control_classic :
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
  explicit Congestion_control_classic(log::Logger* logger_ptr, Peer_socket::Const_ptr sock);

  // Methods.

  /**
   * Implements Congestion_control_strategy::congestion_window_bytes() API.
   * @return See Congestion_control_strategy::congestion_window_bytes().
   */
  size_t congestion_window_bytes() const override;

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
   * Implements Congestion_control_strategy::on_loss_event() API.  Congestion window changes to some
   * configurable constant fraction (like half) of its current value, and the next slow start (if
   * any, e.g., on a later Drop Timeout) will end once the congestion window exceeds this value.
   * Thus after this call congestion avoidance mode begins anew.
   *
   * Additional Peer_socket state used: Peer_socket::m_snd_flying_bytes (the # of bytes still In-flight
   * after the loss event).
   *
   * @param bytes
   *        See Congestion_control_strategy::on_loss_event().
   * @param packets
   *        See Congestion_control_strategy::on_loss_event().
   */
  void on_loss_event(size_t bytes, size_t packets) override;

  /**
   * Implements Congestion_control_strategy::on_drop_timeout() API.  Congestion window is set to a
   * low value, while the next slow start session is set to end at roughly half (or some
   * configurable constant fraction) of the current (pre-decrease) congestion window.  Thus a slow
   * start session begins.
   *
   * Additional Peer_socket state used: Peer_socket::m_snd_flying_bytes (the # of bytes still In-flight
   * after the loss event implied by the timeout).
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
   * Returns the decay (as a percentage) to apply to the congestion window upon encounterling loss.
   * RFC 5681-3.2.1 and formula (4) say this should 50 (i.e., halve it); but we make it configurable
   * by a socket option.
   *
   * @return A percentage in the range [1, 100].
   */
  unsigned int congestion_window_decay() const;

  // Data.

  /// The Reno CWND/SSTHRESH-changing engine and CWND/SSTHRESH storage.
  Congestion_control_classic_data m_classic_data;
}; // class Congestion_control_classic

} // namespace flow::net_flow
