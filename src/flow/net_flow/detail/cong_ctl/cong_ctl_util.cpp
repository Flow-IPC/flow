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
#include "flow/net_flow/detail/cong_ctl/cong_ctl_util.hpp"
#include <limits>

namespace flow::net_flow
{
// Static initializations.

/* By setting this to infinity, we disable any limit on per-ack-group CWND incrementing.
 * For reasons why see on_acks(). */
const size_t Congestion_control_classic_data::S_MAX_SLOW_START_CWND_INCREMENT_BLOCKS
  = std::numeric_limits<size_t>::max();

// Implementations.

Congestion_control_classic_data::Congestion_control_classic_data(log::Logger* logger_ptr,
                                                                 Peer_socket::Const_ptr sock) :
  log::Log_context(logger_ptr, Flow_log_component::S_NET_FLOW_CC),
  m_sock(sock),
  // Initialize CWND in a standard way, or use the option's overriding value if provided.
  m_cong_wnd_bytes(sock->max_block_size() * // Initial CWND a multiple of max-block-size always.
                   ((sock->opt(sock->m_opts.m_st_cong_ctl_init_cong_wnd_blocks) == 0)
                      ? ((sock->max_block_size() > 2190) // Not option-specified; use RFC 5681.
                           ? 2 // In (2190, ...).
                           : ((sock->max_block_size() > 1095)
                                ? 3  // In (1095, 2190].
                                : 4)) // In [1, 1095].
                      : sock->opt(sock->m_opts.m_st_cong_ctl_init_cong_wnd_blocks))), // Option-specified.
  m_init_cong_wnd_bytes(m_cong_wnd_bytes),
  // SSTHRESH set to infinity as recommended by RFC 5681-3.1 and others.  Will be set to finite value on drop.
  m_slow_start_thresh_bytes(std::numeric_limits<size_t>::max()),
  m_acked_bytes_since_window_update(0)
{
  /* Initial CWND is chosen above according to RFC 5681-3.1 (for TCP) (unless it's overridden with
   * m_st_init_cond_wnd_blocks option).
   *
   * I'm blindly following the RFC here.  Whatever reasoning led to the above formula is probably
   * about right for us too; IP+UDP+Flow overhead should not be too much different from IP+TCP
   * overhead.  Anyway, the initial CWND is just a safe guess and will change quickly regardless. */

  log_state("construction");
}

size_t Congestion_control_classic_data::congestion_window_bytes() const
{
  return m_cong_wnd_bytes;
}

size_t Congestion_control_classic_data::init_congestion_window_bytes() const
{
  return m_init_cong_wnd_bytes;
}

size_t Congestion_control_classic_data::slow_start_threshold_bytes() const
{
  return m_slow_start_thresh_bytes;
}

void Congestion_control_classic_data::on_acks(size_t bytes)
{
  using std::min;

  log_state("ack-handling-before");

  const Peer_socket::Const_ptr sock = socket();

  if (congestion_window_at_limit())
  {
    /* Once we've maxed out CWND, some of the below calculations make no sense and wouldn't result
     * in an increased CWND anyway.  If we are in slow start, this is nothing more than a little
     * optimization.  If we are in congestion avoidance, then it's a bit more complex:
     * m_acked_bytes_since_window_update state remains unchaged which may seem like it might have
     * implications on when CWND is no longer at the limit in the future (does congestion avoidance
     * resume as if nothing happened or what?).  HOWEVER, any change in CWND that would lower it
     * will have to be done through congestion_event(), in which case
     * m_acked_bytes_since_window_update will reset to zero, so what we do with
     * m_acked_bytes_since_window_update at this point is immaterial.  Therefore it's safe to simply
     * do nothing here. */
    FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: cong_wnd already at limit.");
  }
  else if (in_slow_start())
  {
    /* We perform slow start CWND increase based on classic Reno TCP (RFC 5681) with the Appropriate
     * Byte Counting (ABC) modification (RFC 3465-2.2).  It says, simply, to increment CWND by the
     * number of bytes acknowledged.  By the way, ABC is the reason I ultimately chose to store CWND
     * in bytes, not max-blocks; implementing ABC with a multiples-of-max-block-size CWND is tricky.
     * (Linux TCP does it.  The code is difficult to understand.)
     *
     * Well, there is one difficulty.  The RFC also says to increase CWND as above but capped by a
     * maximum of L bytes, where L is 2 * max-block-size.
     *
     * The justification is: "A very large L could potentially lead to large line-rate bursts of
     * traffic in the face of a large amount of ACK loss or in the case when the receiver sends
     * 'stretch ACKs' (ACKs for more than the two full-sized segments allowed by the delayed ACK
     * algorithm)."  I don't know what the first part means, but let's deal with the second part.
     * For now, let's say we know for a fact that the receiver's net_flow is the same as our net_flow.
     * net_flow does not send stretch acknowledgments; it sends an ACK -- at the latest -- when it
     * has 2 un-acked DATA segments ready to acknowledge.  If we assume good behavior (which for now
     * we will), then "stretch ACKs" are not a concern.
     *
     * However, the main reason the limit of 2 makes no sense for us is the following.  The RFC,
     * when describing how to increment CWND based on an ACK, is describing a slightly different
     * situation than on_acks().  To illustrate the difference, suppose 10 segments happen to arrive
     * at the same time, each acknowledging a different data segment sent earlier (for this example,
     * suppose each one acks one MSS).  The RFC describes what to do for *each* of them.  Therefore,
     * for each one CWND will increase by 1 MSS (for 10 MSS total).  Now imagine the equivalent
     * situation in Flow protocol: 10 individual acknowledgments arrive at the same time (perhaps even in one
     * ACK, which can happen in high-bandwidth situations).  Based on the documented rule for
     * calling on_acks(), it will be called ONCE, with bytes = 10 * max-block-size.  If we were to
     * apply the limit of 2 we'd only increase CWND by 2 * max-block-size -- 5x less than even the
     * RFC would recommend!  Therefore we should disregard the limit part of the RFC, as it doesn't
     * apply to us.  We are perhaps vulnerable to our equivalent of "stretch ACKs" if the other
     * net_flow misbehaves, but let's not sweat it until it's a problem.
     *
     * ...That was difficult to explain in writing.  The bottom line is, I do allow for a limit
     * S_MAX_SLOW_START_CWND_INCREMENT_BLOCKS a-la RFC but recommend that, at least for now, we set
     * it to basically infinity, because the model the RFC assumes is not quite the model we use and
     * would thus lead to far-too-slow CWND growth during slow start in a high-bandwidth situation
     * like transfer over localhost.  I have seen that first-hand with the value set to 2. */

    const size_t limit = S_MAX_SLOW_START_CWND_INCREMENT_BLOCKS * sock->max_block_size();
    const size_t increment_bytes = min(bytes, limit);

    FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: [slow_start] cong_wnd incrementing "
                   "by [" << sock->bytes_blocks_str(increment_bytes) << "]; "
                   "acked [" << sock->bytes_blocks_str(bytes) << "]; "
                   "max increment [" << sock->bytes_blocks_str(limit) << "].");

    m_cong_wnd_bytes += increment_bytes;
    clamp_congestion_window(); // If exceeded limit, bring it to the limit.

    /* Take opportunity to ensure our arithmetic is working; this field should be zero outside of
     * congestion avoidance.  Thus if we just equalled or exceeded SSTRESH, thus entering
     * congestion avoidance, then this is a clean slate. */
    assert(m_acked_bytes_since_window_update == 0);
  }
  else // if (!in_slow_start())
  {
    /* In congestion avoidance, also follow classic Reno RFC 5681 with the ABC modification (RFC
     * 3465-2.1).  For every CWND bytes acked in a row, increase CWND by a block, and then decrease
     * the byte tally by the pre-increase CWND.  This causes a linear-ish growth in CWND while in
     * congestion avoidance.  We add the feature to make the increment N * block, where N is configurable
     * (the growth is still linear).
     *
     * As always there is a caveat that prevents the below code from literally following the
     * algorithm from RFC.  More precisely, we follow the RFC algorithm and, if after that
     * bytes_acked >= CWND still, we reapply the RFC algorithm.  We repeat until bytes_acked < CWND.
     *
     * How can bytes_acked match or exceed CWND after applying the RFC algorithm once?  I believe
     * it's impossible assuming the expected handling of Dropped packets in Node.  However I
     * cannot/don't want to prove this to myself, especially with edge cases with a very low CWND
     * yet somehow a quite large ack group (large "bytes" arg value).  Therefore, to be safe, I
     * added the handling for this case. */

    // Add the newly acked bytes to the tally.
    m_acked_bytes_since_window_update += bytes;

    /* Increase CWND if that has put us over the top.  Repeat if we're still over the top after
     * increasing CWND and accordingly decreasing the tally.  In the usual case
     * this loop will execute once, reducing the algorithm to the one in the RFC. */
    bool cwnd_changed = false;
    while ((m_acked_bytes_since_window_update >= m_cong_wnd_bytes)
           // Can't be true in 1st iteration (eliminated in if () above) but can become true after 1st iteration:
           && (!congestion_window_at_limit()))
    {
      // Have a full CWND's worth of acked bytes; increase CWND.

      if (!cwnd_changed)
      {
        cwnd_changed = true;
      }
      else // (already is true)
      {
        // This should be rare or impossible, but not an error.  An INFO is in order.
        FLOW_LOG_INFO("cong_ctl [" << sock << "] info: [cong_avoid] acknowledgment group brings tally to more "
                      "than one congestion window -- thought to be rare or impossible.");
      }

      /* Extend RFC 5681: in the RFC, this is simply 1.  If they specify special value 0, use that value.
       * Otherwise use the value they give. */
      unsigned int increment_blocks = sock->opt(sock->m_opts.m_st_cong_ctl_cong_avoidance_increment_blocks);
      if (increment_blocks == 0)
      {
        increment_blocks = 1;
      }
      const size_t increment_bytes = increment_blocks * sock->max_block_size();

      FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: [cong_avoid] cong_wnd incrementing "
                     "by [" << sock->bytes_blocks_str(increment_bytes) << "]; "
                     "acked [" << sock->bytes_blocks_str(bytes) << "].");

      // "Convert" the CWND's worth of acked bytes into N-block increase of CWND (as in RFC, though there N=1).

      m_acked_bytes_since_window_update -= m_cong_wnd_bytes;

      m_cong_wnd_bytes += increment_bytes;
      clamp_congestion_window(); // If exceeded limit, bring it to the limit.  (while() loop will exit in that case.)
    } // while ((bytes_acked >= CWND) && (CWND < max CWND)

    if (!cwnd_changed)
    {
      // Don't have a full CWND's worth of acked bytes yet; leave CWND alone.
      FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: [cong_avoid] cong_wnd not yet incrementing; "
                     "acked [" << sock->bytes_blocks_str(bytes) << "].");
    }
  } // else if (!in_slow_start())

  log_state("ack-handling-after");
} // void Congestion_control_classic_data::on_acks()

void Congestion_control_classic_data::on_congestion_event(size_t new_slow_start_thresh_bytes,
                                                          size_t new_cong_wnd_bytes)
{
  log_state("event-handling-before");

  /* They are updating CWND and/or SSTHRESH due to some (unspecified) event.  For example the
   * classic Reno algorithm (more or less) halves CWND and sets SSTHRESH to this halved CWND,
   * whenever a non-timeout packet loss is detected.
   *
   * First figure out what to do about m_acked_bytes_since_window_update given this change. We may
   * have to reset it to zero.  This is fairly important, as it basically resets the congestion
   * avoidance state to that suitable to the beginning of the congestion avoidance phase (which may
   * then happen immediately or after one or more slow start phases, depending on the CWND vs.
   * SSTHRESH comparison).
   *
   * Let us consider the possibilities.  (It may help to draw a mental or physical graph of CWND and
   * SSTHRESH over time.)  In slow start, where CWND < SSTHRESH, CWND is increasing exponentially
   * (for every incoming full acknowledgment), and m_acked_bytes_since_window_update is not used.
   * In congestion avoidance, where CWND >= SSTHRESH, CWND is increasing linearly and
   * m_acked_bytes_since_window_update is used to keep track of this process.  Whenever congestion
   * avoidance ends (in favor of slow start or a different congestion avoidance phase at a lower [or
   * higher?] CWND), m_acked_bytes_since_window_update must be reset to 0.  So, the possibilities:
   *
   *   1. They're changing CWND (usually lower, e.g., Reno halves it).  This introduces
   *      "discontinuity" in the CWND value over time, so if any congestion avoidance phase was in
   *      progress, it is now over. Set to 0 to avoid affecting the next congestion avoidance phase
   *      (whether it happens now or after a slow start phase).  If no congestion avoidance phase
   *      was in progress, then it is already 0 and can be safely set to 0 again (or it can be left
   *      alone; makes no difference).
   *
   *   2. CWND is staying constant, but SSTHRESH is changing.  If we are in slow start, then we're
   *      not in congestion avoidance, so it is already set to 0 so no need to do anything.  If we
   *      are not in slow start, then we are in congestion avoidance.  If we are in congestion
   *      avoidance, and under the new SSTHRESH we remain in congestion avoidance, then the
   *      in-progress congestion avoidance increase can continue undisturbed.  Finally, if we are in
   *      congestion avoidance, and the new SSTHRESH would put us into slow start, then this
   *      congestion avoidance session is over, and we must set to 0 (to avoid affecting the next
   *      congestion avoidance phase, if there is one).
   *
   * Most of the above is uncontroversial and is consistent with various implementations of classic
   * TCP congestion control (Reno and off-shoots like Westwood+).  Certainly -1- is clear-cut.
   * -2- is generally atypical (usually SSTHRESH changes only when CWND changes), but let's consider
   * it.  If CWND stays constant, while SSTHRESH moves from at or below to above CWND, then even if
   * unlikely it clearly means congestion avoidance is over.  So that case is not controversial.
   * The remaining case is if SSTHRESH moves but stays below CWND and the decision to not touch
   * m_acked_bytes_since_window_update (i.e., let congestion avoidance continue).  I find this
   * decision acceptable <comment cuts out>.  @todo Where did the rest of the comment go?
   * why is the decision acceptable?  Didn't save? */
  if ((new_cong_wnd_bytes != m_cong_wnd_bytes) // Case 1 above.
      // Case 2 above:
      || ((!in_slow_start()) && in_slow_start(new_slow_start_thresh_bytes, new_cong_wnd_bytes)))
  {
    m_acked_bytes_since_window_update = 0;
  }

  // Now the easy part.
  m_slow_start_thresh_bytes = new_slow_start_thresh_bytes;
  m_cong_wnd_bytes = new_cong_wnd_bytes;

  // In case they gave a too-high value for congestion window.  (SSTHRESH can be arbitrarily high.)
  clamp_congestion_window();

  log_state("event-handling-after");
} // Congestion_control_classic_data::on_congestion_event()

void Congestion_control_classic_data::on_drop_timeout(size_t new_slow_start_thresh_bytes)
{
  using std::max;

  const Peer_socket::Const_ptr sock = socket();

  /* Node has detected a major loss event in the form of a Drop Timeout.  In most congestion
   * control algorithms this causes CWND to be decreased to a low value, as inspired by classic Reno
   * (RFC 5681).  On the other hand SSTHRESH is set to a strategy-dependent value, which is that's
   * given to us as an argument.  Certainly the specific strategy may also reset other state. */

  // RFC 5689-3.1 (set CWND to a low value; note: not the initial CWND, which can be higher).
  const size_t new_cong_wnd_bytes
    = sock->opt(sock->m_opts.m_st_cong_ctl_cong_wnd_on_drop_timeout_blocks) * sock->max_block_size();
  // ^-- @todo Consistently with some other options, have value 0 mean "use RFC value," which is 1.

  on_congestion_event(new_slow_start_thresh_bytes, new_cong_wnd_bytes);
} // Congestion_control_classic_data::on_drop_timeout()

void Congestion_control_classic_data::on_idle_timeout()
{
  /* Node has detected that nothing has been sent out by us for a while.  (Note that this is
   * different from a Drop Timeout.  A Drop Timeout causes data to be sent (either retransmitted [if
   * used] or new data), which resets the state back to non-idle for a while.)  Per TCP
   * RFC 5681 (and DCCP CCID 2 RFC 4341), in this case we are to simply set CWND back to its initial
   * value (unless that would increase it [e.g., if we had recently hit Drop Timeout as well], in
   * which case just leave it alone).  SSTHRESH is to be left alone.  Thus we will slow-start much
   * as we would at the beginning of the connection, except that SSTHRESH stays constant and is not
   * set back to infinity.
   *
   * There is at least one RFC with a more moderate policy, but it's not standard, and let's not
   * get into it for now.
   *
   * Certainly the specific strategy may also reset other state. */

  const Peer_socket::Const_ptr sock = socket();
  if (m_cong_wnd_bytes <= m_init_cong_wnd_bytes)
  {
    FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: Idle Timeout event; "
                   "cong_wnd [" << sock->bytes_blocks_str(m_cong_wnd_bytes) << "] is "
                   "already <= initial cong_wnd [" << sock->bytes_blocks_str(m_init_cong_wnd_bytes) << "].");
  }
  else
  {
    // Below call will log details.
    FLOW_LOG_TRACE("cong_ctl [" << socket() << "] update: Idle Timeout event; set cong_wnd to initial cong_wnd.");

    // Don't change SSTHRESH.
    on_congestion_event(m_slow_start_thresh_bytes, m_init_cong_wnd_bytes);
  }
} // Congestion_control_classic_data::on_idle_timeout()

bool Congestion_control_classic_data::congestion_window_at_limit() const
{
  const Peer_socket::Const_ptr sock = socket();
  return m_cong_wnd_bytes
         >= sock->opt(sock->m_opts.m_st_cong_ctl_max_cong_wnd_blocks) * sock->max_block_size();
}

void Congestion_control_classic_data::clamp_congestion_window()
{
  const Peer_socket::Const_ptr sock = socket();
  const size_t limit = sock->opt(sock->m_opts.m_st_cong_ctl_max_cong_wnd_blocks) * sock->max_block_size();
  if (m_cong_wnd_bytes > limit)
  {
    FLOW_LOG_TRACE("cong_ctl [" << sock << "] update: "
                   "clamping cong_wnd [" << sock->bytes_blocks_str(m_cong_wnd_bytes) << "] "
                   "to [" << sock->bytes_blocks_str(limit) << "].");
    m_cong_wnd_bytes = limit;
  }
}

bool Congestion_control_classic_data::in_slow_start() const
{
  return in_slow_start(m_cong_wnd_bytes, m_slow_start_thresh_bytes);
}

bool Congestion_control_classic_data::in_slow_start(size_t cong_wnd_bytes, size_t slow_start_thresh_bytes) // Static.
{
  return cong_wnd_bytes < slow_start_thresh_bytes;
}

void Congestion_control_classic_data::log_state(const std::string& context) const
{
  using std::numeric_limits;

  const Peer_socket::Const_ptr sock = socket();
  FLOW_LOG_TRACE("cong_ctl [" << sock << "] info [" << context <<
                 '|' << (in_slow_start() ? "slow_start" : "cong_avoid") << "]: "
                 "cong_wnd [" << sock->bytes_blocks_str(m_cong_wnd_bytes) << "]; "
                 "sl_st_thresh "
                 "[" << ((m_slow_start_thresh_bytes == numeric_limits<size_t>::max())
                           ? "inf" : sock->bytes_blocks_str(m_slow_start_thresh_bytes)) << "] "
                 "cong_avoid_acked "
                 "[" << sock->bytes_blocks_str(m_acked_bytes_since_window_update) << "].");
}

Peer_socket::Const_ptr Congestion_control_classic_data::socket() const
{
  const Peer_socket::Const_ptr sock = m_sock.lock();
  assert(sock);
  return sock;
}

} // namespace flow::net_flow
