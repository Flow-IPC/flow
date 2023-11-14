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

namespace flow::net_flow
{
// Types.

/**
 * A per-Peer_socket module that tries to estimate the bandwidth available to the outgoing flow.
 * That is, it tries to get at the # of bytes per unit time the outgoing empty pipe can support,
 * minus the # of bytes per unit time being transferred by all other flows in that pipe (Net-Flow or
 * otherwise).  This is useful primarily for certain congestion control strategies (like
 * Congestion_control_classic_with_bandwidth_est) but may also be good information to make available
 * to the application layer (e.g., if the user wants to inform a media player what is a suitable bit
 * rate to avoid rebuffering).  The latter is the reason why Send_bandwidth_estimator is not part of
 * a congestion control strategy but rather a separate class.
 *
 * How can we make this estimate?  As a black box, we care about just one event, `on_acks(N)`, where `N`
 * is the number of bytes that have been cleanly acknowledged by the receiver.  The `net_flow` engine
 * should inform this module about each such event.  The running bandwidth estimate is then
 * available via bandwidth_bytes_per_time().
 *
 * ### Object life cycle ###
 * There is a strict 1-to-1 relationship between one Send_bandwidth_estimator
 * instance and one Peer_socket.  A Send_bandwidth_estimator is created shortly after Peer_socket
 * is and is saved inside the latter.  Conversely a pointer to the Peer_socket is stored inside the
 * Send_bandwidth_estimator (for read-only access to stats such as SRTT).  The containing
 * Peer_socket must exist at all times while Send_bandwidth_estimator exists.  Informally it's
 * recommended that Peer_socket destructor or other method deletes its Send_bandwidth_estimator
 * instance when it is no longer needed.
 *
 * Relationship with outside world:
 *
 *   1.  Send_bandwidth_estimator is a black box to Node and Peer_socket code (no
 *       access to internals; access only to constructors/destructor and API).
 *   2.  Send_bandwidth_estimator has `const` (!) `friend` access to Peer_socket
 *       internals.
 *   3.  The programmer of Send_bandwidth_estimator must assume a certain event
 *       model to be followed by Node.  This model is to be explicitly explained in the doc headers
 *       for any `on_...()` methods.  Node must call the `on_...()` methods as soon as it
 *       detects the appropriate events, and it should aim to detect them as soon as possible after
 *       they occur.
 *   4.  The programmer of Send_bandwidth_estimator subclass may assume the existence
 *       and meaning of certain state of Peer_socket, which she can use to make internal
 *       computations.  Any such state (i.e., in addition to the on-event calls, and their
 *       arguments, in (3)) must be explicitly documented in the class or method doc headers.
 *   5.  Informally, it is assumed that the Node is sending data at a rate close to the available
 *       capacity of the pipe at all times (and thus the resulting on_acks() events reflect this
 *       available maximum possible speed).  (Of course this can only be a best effort and cannot be
 *       guaranteed; indeed Send_bandwidth_estimator itself can help the congestion control
 *       mechanism in use to maintain a sending rate close to the maximum possible, which in turn
 *       will feed good acknowledgment samples to Send_bandwidth_estimator.  It sounds like a
 *       circular technique, but it can work well.)
 *
 * Note that the assumption in (5) can also not hold if the sending rate (and thus the rate of
 * returning acknowledgments) is application-limited; that is the user is not sending data as fast
 * Node allows, or in particular simply not sending data.  In that case the bandwidth readings may
 * be inaccurately low (very much so possibly).  Therefore only rely on the results of this class when
 * not application-limited in this way.
 *
 * ### Units ###
 * bandwidth_bytes_per_time() returns the current bandwidth estimate per unit time U, as an
 * *integer* number of bytes (rounded down).  What is U?  U is given as `Time_unit(1)`, where
 * #Time_unit is a public alias of a boost.chrono duration type.  Be careful in any arithmetic done
 * with the value returned; both overflow and underflow can occur, if one does not take care to
 * sanity-check the arithmetic.  The justification for the value of the #Time_unit alias is given
 * in the #Time_unit doc header.
 *
 * ### Thread safety ###
 * Unless stated otherwise, a Congestion_control_strategy object is to be accessed
 * from the containing Peer_socket's Node's thread W only.
 *
 * Implementation notes
 * --------------------
 *
 * How do we estimate the available bandwidth?  Basically, we assume the sender -- whenever giving
 * us on_acks() events -- is sending data at the maximum possible rate.  Therefore, if we divide the
 * # of bytes reported in acknowledgments by the time period over which we've accumulated those
 * acknowledgments, then we know the available bandwidth over that time period.  Such bandwidth
 * samples can be blended with past samples using a low-pass filter to get a smoothed bandwidth
 * estimate.  Of course the question arises: what time period to choose, and what filter to use?  We
 * use the Westwood+ algorithm that answers these questions.  The basic idea is to maintain a byte
 * count that starts at 0 at the beginning of the sample period and keeps accumulating as the
 * acknowledgments come in; once the sample period > SRTT (smoothed round trip time of the socket),
 * the bytes/time bandwidth sample is taken and added into the running filter; and the new sample
 * begins with the byte count at 0.  The justification for these choices is given, partially, in the
 * code and, more completely, in the papers referenced below.
 *
 * Why not make this a strategy pattern (like Congestion_control_strategy hierarchy) instead of just
 * a single class, so that multiple ways of bandwidth estimation can exist?  Because, as far as I
 * know, Westwood+ is the best available technique, so I don't see the point of doing anything else.
 * If that changes, revisit this.
 *
 * @todo Look into Google BBR, a recent (as of 2016) state-of-the-art congestion control algorithm that
 * aims to estimate bandwidth among other things.  Depending on what what one decides, another bandwidth
 * estimator class could be written.
 *
 * @see Linux kernel's `tcp_westwood.c` for inspiration behind this implementation; the top comment in
 *      that file also cites the main papers that describe the Westwood+ bandwidth estimator.
 */
class Send_bandwidth_estimator :
  public log::Log_context,
  private boost::noncopyable
{
public:

  // Types.

  /**
   * Type that represents the number of bytes, either as an absolute amount or over some time
   * period.  To avoid any surprises we set its width explicitly instead of using `size_t`.
   */
  using n_bytes_t = uint64_t;

  /**
   * The primary time unit over which this class reports bandwidth.  So when
   * bandwidth_bytes_per_time() return the number N, that means its bandwidth estimate is N bytes
   * per `Time_unit(1)` time.
   *
   * ### Implementation notes ###
   * Why choose milliseconds?  There are two conflicting constraints on
   * #Time_unit.  In the basic bandwidth sample computation, the formula is B = N/D, where N is the
   * number of bytes acknowledged over time period D, and D is a #Fine_duration.  Note that B is an
   * integer (#n_bytes_t), as we try to avoid floating-point computations, so the division rounds
   * down.  #Fine_duration, we can assume, is at least nanoseconds (10^-9 seconds).
   *
   * If #Time_unit is something large, like seconds, then in the straightforward B = N/D formula
   * nanoseconds must first be converted (with integer math only!) to seconds.  This loses a ton of
   * precision in D, which will typically be something like .001-.2 seconds (up to about an RTT).
   * That's clearly unacceptable.  If #Time_unit is something tiny, like nanoseconds, then N/D's
   * minimum non-zero value (1) will represent 10^9 bytes per second (all lower bandwidths B
   * rounding down to zero), which is clearly absurd.  What about milliseconds?  RTTs are
   * classically given in milliseconds anyway, so the precision is acceptable (though #Fine_clock can
   * in fact give much more precise values -- but that precision is not necessary here).  Meanwhile,
   * the lowest value B = N/D = 1 represents 1000 bytes per second, which is definitely a very low
   * bandwidth and thus good enough.  So, we use milliseconds.
   *
   * Another important reason milliseconds is a reasonable unit is in the outside CWND = B * RTTmin
   * calculation (see Congestion_control_classic_with_bandwidth_est), where CWND is the congestion
   * window, B is the bandwidth estimate we generate, and RTTmin is some RTT.  Assuming RTTmin is
   * also in units of #Time_unit, and we use a very large unit for B, then the result of that
   * calculation may actually exceed 64 bits, which will cause overflow.  On the other hand if we
   * just express B in bytes/ms and RTTmin in ms, then the formula avoids that danger while being
   * quite precise enough.
   */
  using Time_unit = boost::chrono::milliseconds;

  // Constructors/destructor.

  /**
   * Constructs object by setting up logging and saving a pointer to the containing Peer_socket.
   * bandwidth_bytes_per_time() will return 0 until enough on_acks() events have occurred to make it
   * receive a higher value, but certainly at least until on_acks() is called once.
   *
   * Only a weak pointer of `sock` is stored: the `shared_ptr` itself is not saved, so the reference
   * count of `sock` does not increase.  This avoids a circular `shared_ptr` situation that would arise
   * from `*this` pointing to `sock`, and `sock` pointing to `*this` (the two objects *do* need access
   * to each other, as explained in class doc header).
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param sock
   *        The Peer_socket for which this module will estimate available outgoing bandwidth.
   */
  explicit Send_bandwidth_estimator(log::Logger* logger_ptr, Peer_socket::Const_ptr sock);

  // Methods.

  /**
   * Returns the current estimate of the available outgoing bandwidth per unit time for the
   * containing socket's connection, in units of bytes per `Time_unit(1)`.  This value may be zero if
   * either there is not enough information to make a reasonable estimate, or if the estimated
   * bandwidth is less than a certain low threshold.
   *
   * @return Ditto.
   */
  n_bytes_t bandwidth_bytes_per_time() const;

  /**
   * Informs the bandwidth estimator strategy that 1 or more previously sent packets whose status
   * was In-flight just received acknowledgments, thus changing their state from In-flight to
   * Acknowledged.  For efficiency and simplicity of behavior, on_acks() should be called as few
   * times as possible while still satisfying the requirement in the previous sentence.  That is,
   * suppose acknowledgments for N packets were received simultaneously.  Then on_acks() must be
   * called one time, not several times.
   *
   * on_acks() must be called *after* any RTT measurement based on the acked bytes has affected
   * the recorded SRTT in `sock` passed in constructor.
   *
   * Additional Peer_socket state used: Peer_socket::m_snd_smoothed_round_trip_time.
   *
   * Assumptions about ACK sender (DATA received): Same as Congestion_control_strategy::on_acks().
   * However Send_bandwidth_estimator does not strictly rely on those exact requirements; however it
   * generally expects quick acknowledgments in order to be effective, and
   * Congestion_control_strategy::on_acks()'s specific requirements happen to basically satisfy that
   * informal requirement.
   *
   * @note bandwidth_bytes_per_time() may return a different value after this call.  If something,
   *       like congestion control, relies on that value and can be reasonably be called right after
   *       or right before this on_acks(), recommend calling it right after this on_acks().
   * @note Acknowledgments of data that are not currently In-flight due to being Dropped (a/k/a late
   *       ACKs) or Acknowledged (i.e., duplicate ACKs) must NOT be passed to this method.
   * @note on_acks() makes no assumptions about how the reported individual packet acks were
   *       packaged by the ACK sender into actual ACK packets (how many ACKs there were, etc.).
   *       It just assumes every individual acknowledgment is reported to on_acks() as soon as
   *       possible and grouped into as few on_acks() calls as possible.
   * @note For definition of In-flight, Acknowledged, and Dropped bytes, see
   *       Peer_socket::m_snd_flying_pkts_by_sent_when and Peer_socket::m_snd_flying_pkts_by_seq_num.
   *
   * @param bytes
   *        The sum of the number of bytes in the user data fields of the packets that have been
   *        Acknowledged.  Must not be zero.
   */
  void on_acks(size_t bytes);

private:
  // Methods.

  /**
   * Applies the low-pass filter that takes the given previous result of the filter and blends in
   * the given new sample.  The values should be in the same units, which are presumably bytes per
   * `Time_unit(1)`.
   *
   * @param prev_val_per_time
   *        Previous result of this filter.
   * @param new_sample_val_per_time
   *        New sample to blend into that filter.
   * @return The blended value.
   */
  static n_bytes_t apply_filter(n_bytes_t prev_val_per_time, n_bytes_t new_sample_val_per_time);

  /**
   * Utility that returns a handle to the containing Peer_socket.  If somehow the containing
   * Peer_socket has been deleted, `assert()` trips.
   *
   * @return Ditto.
   */
  Peer_socket::Const_ptr socket() const;

  // Data.

  /**
   * The containing socket (read-only access).  Implementation may rely on various state stored
   * inside the pointed-to Peer_socket.
   *
   * Why `weak_ptr`?  See similar comment on Congestion_control_strategy::m_sock.
   */
  boost::weak_ptr<Peer_socket::Const_ptr::element_type> m_sock;

  /**
   * The current smoothed bandwidth estimate, to be returned by bandwidth_bytes_per_time(), in the
   * units described by that method's doc header.  Zero until the first sample is completed.
   */
  n_bytes_t m_bytes_per_time_smoothed;

  /**
   * In the same units as #m_bytes_per_time_smoothed, the less smoothed bandwidth estimate, which is
   * simply the progressive application of apply_filter() on each available bandwidth sample.  It's
   * not used by bandwidth_bytes_per_time() but instead is blended into #m_bytes_per_time_smoothed.
   *
   * I do not really understand why `tcp_westwood.c` (Linux kernel) does it that way; I get that it
   * adds an extra level of smoothing, but I did not catch any obvious references to this in the
   * papers.  Perhaps this is some elementary math that is just assumed to be understood.  Anyway, I
   * took it from `tcp_westwood.c`.  Their comment on the equivalent data member `bw_ns_est` says:
   * "first bandwidth estimation..not too smoothed 8)" which is not too helpful to me.
   */
  n_bytes_t m_bytes_per_time_less_smoothed;

  /**
   * The number of bytes acknowledged by receiver since #m_this_sample_start_time (the time when the
   * current sample began to be compiled).  When a sample is deemed sufficient, `m_bytes_this_sample
   * / (now() - m_this_sample_start_time)` is the individual bandwidth sample fed into the filter.
   */
  n_bytes_t m_bytes_this_sample;

  /// The time at which the currently ongoing bandwidth sample began to accumulate.
  Fine_time_pt m_this_sample_start_time;

  /// `true` until on_acks() called for the first time; `false` forever thereafter.  Used to begin the sample sequence.
  bool m_no_acks_yet;

  /**
   * `true` until a sample has been completed and fed into the filter; `false` forever thereafter.  Used to begin the
   * sample sequence.
   */
  bool m_no_samples_yet;
}; // class Send_bandwidth_estimator

} // namespace flow::net_flow
