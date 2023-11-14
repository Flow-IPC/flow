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
#include "flow/net_flow/peer_socket.hpp"
#include <boost/weak_ptr.hpp>
#include <string>

namespace flow::net_flow
{
// Types.

/**
 * Utility class for use by Congestion_control_strategy implementations that implements congestion
 * window and slow start threshold storage and classic Reno-style "slow start" and "congestion
 * avoidance" algorithms.
 *
 * While all (most?) congestion control algorithms store a CWND value, how each one computes or stores it is
 * entirely up to the particular implementation.  Thus each Congestion_control_strategy subclass can
 * just store its own CWND data member, possibly an SSTHRESH data member, and re-implement slow
 * start and congestion avoidance as needed.
 *
 * However, many (most?) congestion control algorithms use the classic TCP congestion control (Reno)
 * slow start and congestion avoidance algorithms.  That is, upon acknowledgment, CWND increases
 * either linearly (if CWND >= SSTRESH) or exponentially (CWND < SSTHRESH); these are "congestion
 * avoidance" and "slow start" modes respectively.  Where most (all?) Reno-based algorithms (e.g.,
 * Westwood+) differ is what they do to CWND when loss is detected; they may halve SSTHRESH and CWND
 * or do some other thing.  However, in almost all cases, at least in some situations CWND grows
 * according to either SS or CA based on SSTHRESH.  Therefore, for code reuse among the various
 * Congestion_control_strategy implementations, this class stores CWND and SSTHRESH, increases the
 * former when asked to (based on CWND vs. SSTHRESH), and sets SSTHRESH and CWND to arbitrary given
 * values when asked to.
 *
 * The Linux TCP implementation is kind of based on the same idea, wherein a Reno-style `struct` storing CWND,
 * SSTHRESH, and acked-bytes-so-far values also has a pointer to a "polymorphic" `struct` corresponding to the
 * custom congestion control implementation (e.g., a Westwood-specific `struct` storing its bandwidth estimation
 * variables).  Then when Westwood CC is enabled, its event handlers are set to Reno functions when classic SS
 * and CA are to be performed and to Westwood functions when a Westwood-specific action is to be performed
 * (set CWND to the bandwidth estimate * RTT-min).  It's hard to put into words, but while the idea is the
 * same, it's not really executed quite as cleanly as it could be.  The Linux-equivalent implementation in our
 * framework would be to have Congestion_control_classic store CWND and SSTHRESH and implement classic SS/CA
 * as well as CWND/SSTHRESH halving, then add Congestion_control_classic_with_bandwidth_est deriving from this
 * and try to alter the superclass's behavior through inheritance (CWND setting to bandwidth-based value).
 * Instead of forcing this IS-A relationship (as Linux kind of does), it seems cleaner and more flexible to
 * use a HAS-A relationship:  Congestion_control_classic HAS-A Congestion_control_classic_data (and reuses all
 * of its code, and adds SSTHRESH/CWND halving on loss); Congestion_control_classic_with_bandwidth_est HAS-A
 * Congestion_control_classic_data (and reuses all of its code, plus adds setting SSTHRESH/CWND to a
 * bandwidth-based value).
 *
 * ### Documentation ###
 * Because this class explicitly implements Reno-style CWND growth, upon which any user class
 * must be able to rely exactly, this class is uncharacteristically white-boxy in terms of its API and API
 * documentation.  Each method documents exactly what happens to CWND and SSTHRESH; and no CWND/SSTHRESH
 * changes are allowed except as documented.
 *
 * ### Thread safety ###
 * Same as Congestion_control_strategy.
 */
class Congestion_control_classic_data :
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Constructors/destructor.

  /**
   * Constructs object by setting up logging and saving a pointer to the containing Peer_socket,
   * setting CWND to a reasonable initial value (also known as Initial Window or IW) per classic TCP RFCs,
   * and setting SSTHRESH to infinity (ditto).
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param sock
   *        The Peer_socket.
   */
  explicit Congestion_control_classic_data(log::Logger* logger_ptr, Peer_socket::Const_ptr sock);

  // Methods.

  /**
   * Return current stored CWND value in bytes.  This corresponds to
   * Congestion_control_strategy::congestion_window_bytes().
   *
   * @return Ditto.
   */
  size_t congestion_window_bytes() const;

  /**
   * Return current stored SSTHRESH value in bytes.  When congestion_window_bytes() >= this value,
   * it grows according to congestion avoidance mode (linearly).  Otherwise it grows according to
   * slow start mode (exponentially).
   *
   * @return See above.  Infinity is `"numeric_limits<size_t>::max()"`.
   */
  size_t slow_start_threshold_bytes() const;

  /**
   * The value to which the constructor set the stored CWND value (also known as Initial Window or IW).
   * This is provided, because some congestion control algorithms may want to manually set CWND back
   * to this value on events like Idle Timeout.
   *
   * @return See above.
   */
  size_t init_congestion_window_bytes() const;

  /**
   * Adjusts state, including potentially CWND, based on either "congestion avoidance" or "slow
   * start" algorithm, given that packets with the given amount of data were just converted from
   * In-flight to Acknowledged (i.e., were newly acknowledged).  CWND may change.  SSTHRESH will not
   * change.
   *
   * Specifically: If CWND < SSTHRESH, then each byte of acked data causes CWND to increase by that
   * many bytes.  If CWND >= SSTHRESH, then each CWND bytes of acked data causes CWND to be
   * increased by N * max-block-size bytes (in max-block-size-at-a-time increments), where N
   * is configurable by socket options (N = 1 is the value mandated by RFC 5681).  State is
   * internally maintained to enable the latter to work.  This is inspired by the classic TCP
   * congestion control combined with Appropriate Byte Counting (ABC).
   *
   * @param bytes
   *        Same meaning as for Congestion_Congestion_control_strategy::on_acks().
   */
  void on_acks(size_t bytes);

  /**
   * Sets internally stored SSHTRESH and CWND to the given values; appropriately resets internal
   * state so that on_acks() CWND accounting will continue to work properly.  See details inside the
   * method if needed, but basically since the value of (CWND < SSTHRESH) may be changed by this
   * assignment, we may move from congestion avoidance to slow start.  If CWND changes at all, then
   * a new congestion avoidance session begins if one is in progress.  On the other hand if neither
   * is true, then congestion avoidance or slow start will continue undisturbed in the next
   * on_acks() call.
   *
   * @param new_slow_start_thresh_bytes
   *        New value for SSHTRESH.  (May be unchanged.)
   * @param new_cong_wnd_bytes
   *        New value for CWND.  (May be unchanged.)
   */
  void on_congestion_event(size_t new_slow_start_thresh_bytes, size_t new_cong_wnd_bytes);

  /**
   * Adjust state, including CWND and SSTHRESH, assuming a Drop Timeout just occurred.
   * This is typically treated similarly in most (all?) congestion control strategies, so it is
   * factored out into a method.  (The caller can of course choose to perform additional steps
   * before or after.)  Both CWND and SSTHRESH will change.
   *
   * Specifically: Sets CWND to a low value, since Drop Timeout indicates a major loss event and
   * possibly major congestion; sets SSTHRESH to the given value (which is highly dependent on the
   * caller congestion control strategy).  The CWND part is taken from classic TCP congestion
   * control.
   *
   * Logging: logs the before/after states of the change, but caller is encouraged to explain what
   * it is about to do (e.g., halve SSTHRESH in case of classic Reno).
   *
   * @param new_slow_start_thresh_bytes
   *        New value for SSHTRESH.  (May be unchanged.)
   */
  void on_drop_timeout(size_t new_slow_start_thresh_bytes);

  /**
   * Adjust state, namely CWND, assuming an Idle Timeout just occurred.
   * This is typically treated similarly in most (all?) congestion control strategies, so it is
   * factored out into a method.  (The caller can of course choose to perform additional steps
   * before or after.)  CWND will change.  SSTHRESH will not change.
   *
   * Specifically: Sets CWND to a low value (the initial window), thus in effect restarting the
   * connection (except SSTHRESH is left alone).  The policy is taken from classic TCP
   * congestion control.
   *
   * Logs fully.
   */
  void on_idle_timeout();

private:

  // Methods.

  /**
   * Return `true` if and only if CWND < SSTHRESH.
   *
   * @return Ditto.
   */
  bool in_slow_start() const;

  /**
   * Helper that returns `true` if and only if the given CWND < the given SSTHRESH.
   *
   * @param cong_wnd_bytes
   *        CWND.
   * @param slow_start_thresh_bytes
   *        SSTHRESH.
   * @return See above.
   */
  static bool in_slow_start(size_t cong_wnd_bytes, size_t slow_start_thresh_bytes);

  /**
   * Returns `true` if and only if the stored CWND is >= a certain constant maximum value.
   *
   * @return Ditto.
   */
  bool congestion_window_at_limit() const;

  /// If congestion_window_at_limit(), sets CWND to the limit value.
  void clamp_congestion_window();

  /**
   * Logs, in a TRACE message, all internal state values.
   *
   * @param context
   *        A brief description of where the caller is.  Preferably this should be lower-case
   *        letters with dashes separating words.  We add this to the log message.  Example:
   *        `"ack-before"`.
   */
  void log_state(const std::string& context) const;

  /**
   * Analogous to Congestion_control_strategy::socket().
   *
   * @return Ditto.
   */
  Peer_socket::Const_ptr socket() const;

  // Constants.

  /**
   * The constant that limits how much CWND can grow in slow start for a single ack group.  In
   * on_acks(), while in_slow_start(), each byte acknowledged increments CWND by 1 byte.  However,
   * if the total number of such bytes exceeds this constant, CWND only increments by this constant
   * times max-block-size.
   */
  static const size_t S_MAX_SLOW_START_CWND_INCREMENT_BLOCKS;

  // Data.

  /// Same meaning as in Congestion_control_strategy.
  boost::weak_ptr<Peer_socket::Const_ptr::element_type> m_sock;

  /// Current value of CWND.
  size_t m_cong_wnd_bytes;

  /// Initial value of CWND in constructor.
  const size_t m_init_cong_wnd_bytes;

  /// Current value of SSTRESH.
  size_t m_slow_start_thresh_bytes;

  /**
   * While in congestion avoidance mode (`!in_slow_start()`), the # of acked bytes reported to
   * on_acks() that have not yet been accounted for in a CWND increase.  This corresponds to the
   * `bytes_acked` variable in TCP RFC 3465-2.1.  When in_slow_start(), this value is zero and unused.
   *
   * More formally, the congestion avoidance (CA) algorithm with Appropriate Byte Counting (ABC), as
   * implemented here, works as follows.  When CA is entered, #m_acked_bytes_since_window_update = 0.
   * Then, in every on_acks() call, it is incremented by `bytes`.  Once it exceeds CWND, CWND bytes
   * are "converted" into a CWND increase of N * max-block-size bytes, by performing said CWND increment
   * and subtracting the pre-increment CWND value from #m_acked_bytes_since_window_update.  The
   * remainder of this operation is less than CWND and is in #m_acked_bytes_since_window_update. The
   * latter is then incremented in the next on_acks(), and so on, until CA ends (something causes
   * in_slow_start()).
   */
  size_t m_acked_bytes_since_window_update;
}; // class Congestion_control_classic_data

} // namespace flow::net_flow
