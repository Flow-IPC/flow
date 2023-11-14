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
#include "flow/net_flow/detail/cong_ctl/cong_ctl_classic.hpp"

namespace flow::net_flow
{
// Implementations.

Congestion_control_classic::Congestion_control_classic(log::Logger* logger_ptr, Peer_socket::Const_ptr sock) :
  Congestion_control_strategy(logger_ptr, sock),
  // Initialize the basics (CWND, SSTHRESH, etc.) in the usual way.
  m_classic_data(logger_ptr, sock)
{
  // Nothing.
}

size_t Congestion_control_classic::congestion_window_bytes() const // Virtual.
{
  return m_classic_data.congestion_window_bytes();
}

void Congestion_control_classic::on_acks(size_t bytes, [[maybe_unused]] size_t packets) // Virtual.
{
  /* We have received new (non-duplicate) individual acknowledgments for data we'd sent.  This may
   * affect (grow) CWND.  This is a standard Reno algorithm, so just forward it to m_classic_data
   * which takes care of all non-congestion-event-related CWND changing. */
  m_classic_data.on_acks(bytes); // Will log.
}

void Congestion_control_classic::on_loss_event(size_t bytes, [[maybe_unused]] size_t packets)
  // Virtual.
{
  using std::max;

  /* Node has detected at least some lost bytes (at least one dropped packet), or at least so it
   * thinks, but it's not a "major" (Drop Timeout-based) loss.  Follow classic Reno, RFC 5681:
   * guess that the available pipe (CWND) is half of what we thought it was; and set SSTHRESH to
   * that same value, so that we immediately begin congestion avoidance (slow linear increase in
   * CWND), and so that if we later enter slow start for whatever reason, it will end at this CWND
   * value (unless something else changes SSTHRESH before then of course).  In addition, we make the
   * decay configurable: instead of it being a 1/2 decay, it is settable.
   *
   * More accurately, set it to max(m_flying_bytes / 2, 2 * max-block-size), where m_flying_bytes is
   * the # of bytes we think are currently in the network (sent but not received by the other side
   * or dropped somewhere).
   *
   * Why m_flying_bytes and not CWND?  It used to be the latter until getting corrected at least
   * in RFC 5681 formula (4).  The explanation for the change given in the RFC is cryptic, but
   * basically I surmised it's based on the intuition that we may not be filling the available
   * pipe, yet congestion has been caused, so we shouldn't just change based on CWND, as that may
   * not even affect how many bytes we allow to be In-flight in the near future (e.g., say pipe is
   * only 10% full).  One of the writers of the RFC explains here:
   *
   *   http://www.postel.org/pipermail/end2end-interest/attachments/20060514/20ead524/attachment.ksh
   *
   * Note that the DCCP CCID 2 (similar to our protocol) RFC 4341 says "cwnd is halved," but there
   * is no formula, and they're probably just writing in semi-short-hand; let's follow RFC 5681.
   * In most cases m_flying_bytes and m_cong_wnd_bytes are equal or nearly equal. */

  const Peer_socket::Const_ptr sock = socket();
  const size_t& snd_flying_bytes = sock->m_snd_flying_bytes;

  /* RFC 5689-3.2.1 and formula (4).  The halving (or other reduction) of the window is computed in
   * congestion_window_decay().
   *
   * Subtlety: FlightSize in the RFC represents the # of bytes in the pipe *before* the loss event.
   * Well, in the RFC it's just (SND_NXT - SND_UNA), which is not changed by a dupe-ACK-exposed
   * loss.  However intuitively it's also correct: we want to reduce by N% the number of bytes which
   * "caused" the loss, and that # was in effect before we marked those packets Dropped.  Anyway,
   * since we must be called AFTER the Drop, and bytes contains the # dropped, we can compute the
   * pre-Drop total easily. */
  const unsigned int window_decay_percent = congestion_window_decay();
  const size_t new_wnd_bytes = max((snd_flying_bytes + bytes) * window_decay_percent / 100,
                                   2 * sock->max_block_size());

  FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: loss event; "
                 "set sl_st_thresh=cong_wnd to [" << window_decay_percent << "%] of "
                 "In-flight [" << sock->bytes_blocks_str(snd_flying_bytes + bytes) << "].");

  m_classic_data.on_congestion_event(new_wnd_bytes, new_wnd_bytes); // Will log but not enough (so log the above).

  /* At this point m_classic_data will just start congestion avoidance.  Classic Reno here would do
   * a special phase called Fast Recovery, first.  Understanding no such special phase is necessary
   * in our case requires intuitive understanding of Fast Recovery in context.  Fast Recovery
   * (following a retransmit) is basically trying to inflate CWND with each duplicate cumulative ACK
   * to compensate for the fact there is no separate "pipe" (bytes In-flight) variable, and that the
   * normal measure of "pipe" (SND_NXT - SND_UNA) is inaccurate when packets in the [SND_UNA,
   * SND_NXT) range are lost, which is the case after a loss event. So to allow more data (including
   * the retransmitted segment) to enter the pipe, instead of increasing "pipe," Fast Recovery
   * increases CWND.  Once all of the lost packets have been ACKed (after retransmission, as
   * needed), Fast Recovery ends, and regular congestion avoidance begins; at this point (SND_UNA -
   * SND_NXT) again accurately represents "pipe," so CWND is deflated to what it "should" be ("pipe"
   * / 2).
   *
   * So why don't we do this?  Because we have full selective ACK information.  Therefore we have an
   * explicit pipe variable (m_snd_flying_bytes), which fully accounts for any dropped packets, and
   * we needn't mess with CWND to compensate for anything.  There is also no ambiguity as to what
   * the duplicate ACK means, since we have acknowledgments for each individual packet.  So we
   * basically do what the SACK RFC 3517 recommends.  The only real difference is that while RFC 3517
   * still is written in the context of Fast Recovery being a separate phase (between loss and
   * congestion avoidance) -- which the RFC 3517 algorithm replaces -- which ends once the gaps in
   * the pipe are filled, we use the scoreboard principle (with "pipe", etc.) for our entire
   * operation.  Therefore the phase between loss and congestion avoidance is not special, or even a
   * different phase, and is just regular congestion avoidance.
   *
   * This reasoning is validated by DCCP CCID 2 RFC 4341-5, which specifies just this course of
   * action. */
} // Congestion_control_classic::on_loss_event()

void Congestion_control_classic::on_drop_timeout(size_t bytes, [[maybe_unused]] size_t packets) // Virtual.
{
  using std::max;

  /* Node has detected a major loss event in the form of a Drop Timeout.  Follow classic Reno, RFC
   * 5681: guess that the available pipe (CWND) is half of what we thought it was, but only set
   * SSTHRESH to this halved value, and begin slow start at a low CWND, so that it can slow-start
   * until reaching this new SSTHRESH and then enter congestion avoidance.  Since CWND is set
   * basically to its initial value, the subtleties of on_loss_event() (Fast Recovery and all that)
   * don't apply; we basically just start over almost as if it's a new connection, except SSTHRESH
   * is not infinity. */

  // Determine SSTHRESH.

  const Peer_socket::Const_ptr sock = socket();
  const size_t& snd_flying_bytes = sock->m_snd_flying_bytes;

  /* RFC 5689-3.1 and formula (4).  The halving (or other reduction) of the window is computed in
   * congestion_window_decay().
   *
   * See also subtlety in on_loss_event() at this point.  It matters even more here, as a Drop
   * Timeout may well set snd_flying_bytes to zero (and we're called after registering the Drop in
   * snd_flying_bytes)! */
  const unsigned int window_decay_percent = congestion_window_decay();
  const size_t new_slow_start_thresh_bytes = max((snd_flying_bytes + bytes) * window_decay_percent / 100,
                                                 2 * sock->max_block_size());

  FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: DTO event; set sl_st_thresh "
                 "to [" << window_decay_percent << "%] of "
                 "cong_wnd [" << sock->bytes_blocks_str(snd_flying_bytes + bytes) << "]; "
                 "cong_wnd to minimal value.");

  // Now set SSTHRESH and let m_classic_data set CWND to a low value (common to most congestion control strategies).

  // Will log but not enough (so log the above).
  m_classic_data.on_drop_timeout(new_slow_start_thresh_bytes);
} // Congestion_control_classic::on_drop_timeout()

unsigned int Congestion_control_classic::congestion_window_decay() const
{
  const Peer_socket::Const_ptr sock = socket();
  unsigned int window_decay_percent = sock->opt(sock->m_opts.m_st_cong_ctl_classic_wnd_decay_percent);
  assert(window_decay_percent <= 100); // Should have checked this during option application.
  if (window_decay_percent == 0) // 0 is special value meaning "use RFC 5681 classic value," which is 1/2 (50%).
  {
    window_decay_percent = 50;
  }
  return window_decay_percent;
}

void Congestion_control_classic::on_idle_timeout() // Virtual.
{
  /* Node has detected that nothing has been sent out by us for a while.  (Note that this is
   * different from a Drop Timeout.  A Drop Timeout causes data to be sent (either retransmitted [if
   * used] or new data), which resets the state back to non-idle for a while.)
   *
   * This is handled similarly in most congestion control strategies; let m_classic_data handle it
   * (will set CWND to initial window and not touch SSTHRESH). */

  m_classic_data.on_idle_timeout();
} // Congestion_control_classic::on_idle_timeout()

} // namespace flow::net_flow
