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
#include "flow/net_flow/detail/stats/bandwidth.hpp"
#include "flow/util/util.hpp"

namespace flow::net_flow
{
// Implementations.

Send_bandwidth_estimator::Send_bandwidth_estimator(log::Logger* logger_ptr, Peer_socket::Const_ptr sock) :
  Log_context(logger_ptr, Flow_log_component::S_NET_FLOW),
  m_sock(sock), // Get a weak_ptr from a shared_ptr.
  m_bytes_per_time_smoothed(0), // Just to get the advertised answer in bandwidth_estimate_per_time() immediately.
  m_bytes_per_time_less_smoothed(0), // For cleanliness.
  m_bytes_this_sample(0), // on_acks() does assume this is already zero before it is first called.
  m_no_acks_yet(true), // As documented.
  m_no_samples_yet(true) // As documented.
{
  // Nothing.
}

Send_bandwidth_estimator::n_bytes_t Send_bandwidth_estimator::bandwidth_bytes_per_time() const
{
  return m_bytes_per_time_smoothed;
}

void Send_bandwidth_estimator::on_acks(size_t bytes)
{
  using boost::chrono::microseconds;
  using boost::chrono::round;
  using boost::chrono::ceil;
  using std::max;

  /* Received "bytes" bytes of clean acknowledgment(s).  Assuming both user and Node are sending
   * data at the maximum possible rate, try to estimate the bandwidth as the rate of sending.  Keep
   * passing the bandwidth measurements into a pair of filters to get the result.  Each sample is an
   * accumulation of acked bytes over the time period > SRTT.
   *
   * This implementation is a reasonably informed rip-off of tcp_westwood.c from the Linux kernel.
   * That implementation is by the creators of the Westwood+ algorithm, so it's probably
   * trustworthy (also we have tested it).
   *
   * The basic pattern is:
   *
   *   - on_acks(N) called the first time:
   *     - m_bytes_this_sample = 0; // True after construction.
   *     - m_this_sample_start_time = now();
   *     - m_bytes_this_sample += N; // Equivalent to m_bytes_this_sample = N;
   *   - ...Time passes....
   *   - on_acks(N) called subsequently:
   *     - since_sample_start = now() - m_this_sample_start_time;
   *     - if (since_sample_start > SRTT):
   *       - // Sample is long enough; update bandwidth estimate; start new sample.
   *       - bytes_per_time_this_sample = m_bytes_this_sample / since_sample_start
   *       - m_bytes_per_time_smoothed = filtered mix of m_bytes_per_time_smoothed and
   *         bytes_per_time_this_sample;
   *       - m_bytes_this_sample = 0; m_this_sample_start_time = now();
   *     - else:
   *       - // Sample not long enough.  Can't update bandwidth estimate yet.
   *     - m_bytes_this_sample += N;
   *   - ...Time passes....
   *   - on_acks(N) called subsequently (see above).
   *   - Etc.
   *
   * Some interesting observations about this:
   *
   * Note that m_bytes_this_sample is incremented AFTER the bandwidth estimation is recomputed (if
   * it is recomputed).  So the B/W computation does not include the newest acked data; the newest
   * acked data instead counts in the NEXT sample.  I am not exactly sure why they did it this way
   * and not in the opposite ("greedier") order, but it doesn't seem like it's worse.
   *
   * The start of the pattern has some subtlety to it.  m_bytes_this_sample and
   * m_this_sample_start_time are initialized to zero, as seems reasonable: we don't try to set
   * m_this_sample_start_time at construction or something, as that can be a long time ago and
   * bogus.  (tcp_westwood.c agrees with this.  Things are initialized in the .init callback, which
   * is called upon first TCP ACK receipt, not earlier during connection setup.)  Then
   * m_bytes_this_sample is incremented by N to N (0 + N == N).  The weird thing is that
   * tcp_westwood.c for some reason forces this increment in the first on_acks() equivalent to be 0.
   * If one traces the code, if (first_ack) they set "w->snd_una = tcp_sk(sk)->snd_una;", the
   * following if () probably does not trigger, and then the likeliest caller of that function
   * (westwood_fast_bw()) executes "w->bk += tp->snd_una - w->snd_una;", which increments w->bk by
   * 0.  w->bk is the exact equivalent of our m_bytes_this_sample.
   *
   * The comments in tcp_westwood.c about why the snd_una assignment happens there are not entirely
   * clear to me.  I think basically tp->snd_una may be incorrect for some reason (?) at connection
   * open, though I don't know why, so they just force the first part of the sample to be 0.  I am
   * not really sure.  I have chosen to deviate slightly and in fact perform that first increment.
   * Since it's just the first measurement, it is very very unlikely to matter, so I chose
   * simplicity... the "bytes" argument for us is given directly (no need to perform snd_una math),
   * and we have no reason to distrust it. */

  const Peer_socket::Const_ptr sock = socket();

  if (m_no_acks_yet)
  {
    // First on_acks() call (first acked bytes).  Begin first sample by marking the time.

    FLOW_LOG_TRACE("bw_est [" << sock << "] update: start first sample at 0 bytes.");

    m_no_acks_yet = false;
    m_this_sample_start_time = Fine_clock::now();
  }
  else // if (2nd on_acks() and later)
  {
    /* Standard on_acks() call.  Some acks were just received from the other side.  See if enough
     * time has passed since the sample was first started.  "Enough time" is defined as at least
     * SRTT (which is the estimate for the round trip time maintained elsewhere by measuring how
     * quickly packets are acked). */

    const Fine_duration since_sample_start = Fine_clock::now() - m_this_sample_start_time;
    assert(since_sample_start.count() >= 0);

    const Fine_duration& srtt = sock->m_snd_smoothed_round_trip_time;

    if (srtt == Fine_duration::zero())
    {
      /* A documented on_acks() requirement is that the bytes that were acked have already
       * affected SRTT.  Therefore srtt should not be zero.  We needn't overreact by assert()ing
       * though -- let's just keep building the sample then.... */

      FLOW_LOG_WARNING("bw_est [" << sock << "] update: received acks but have no SRTT yet!  This may be "
                       "a bug.  Adding [" << sock->bytes_blocks_str(bytes) << "] to current sample "
                       "of [" << sock->bytes_blocks_str(size_t(m_bytes_this_sample)) << "] bytes "
                       "started [" << since_sample_start << "] ago.");
    }
    else // if (srtt is valid)
    {
      /* As in tcp_westwood.cpp, if the SRTT is quite small (as in LAN), bandwidth calculations
       * may get crazy; so put a floor on the sample length, as specified in this socket option. */
      const Fine_duration sample_period_floor = sock->opt(sock->m_opts.m_st_snd_bandwidth_est_sample_period_floor);
      const Fine_duration min_sample_period = max(srtt, sample_period_floor);

      FLOW_LOG_TRACE("bw_est [" << sock << "] update: received acks; current sample "
                     "total [" << sock->bytes_blocks_str(size_t(m_bytes_this_sample)) << "]; "
                     "started [" << round<microseconds>(since_sample_start) << "] ago; "
                     "SRTT [" << round<microseconds>(srtt) << "]; "
                     "sample period floor [" << round<microseconds>(sample_period_floor) << "].");

      if (since_sample_start > min_sample_period)
      {
        /* Cool, enough time has passed since this sample was started; take a bandwidth sample over
         * that time period (just bytes/time).  As promised, our units are bytes per Time_unit(1),
         * so convert from Fine_duration to (the likely less fine) Time_unit before dividing.  We
         * use ceil() instead of truncation or round() to avoid rounding down to zero and
         * resulting in division by zero.  (Shouldn't really happen due to sample_period_floor,
         * but who knows what they've set it to?  Better safe than sorry.) */

        const n_bytes_t bytes_per_time_this_sample
          = m_bytes_this_sample / round<Time_unit>(since_sample_start).count();

        // B/W sample computed.  Shove into the filter(s), ultimately updating the main B/W estimate.

        if (m_no_samples_yet)
        {
          // First sample completed; simply set the estimates to that first sample.

          m_no_samples_yet = false;

          FLOW_LOG_TRACE("bw_est [" << sock << "] update: first complete sample; bw_est = bw_est_less_smoothed "
                         "= [" << bytes_per_time_this_sample << "] bytes per [" << Time_unit(1) << "]"
                         "= [" << util::to_mbit_per_sec<Time_unit>(bytes_per_time_this_sample) << " Mbit/s]; "
                         "start new sample at 0 bytes.");

          m_bytes_per_time_less_smoothed = bytes_per_time_this_sample;
          m_bytes_per_time_smoothed = m_bytes_per_time_less_smoothed;
        }
        else
        {
          /* 2nd or later sample collected.  Blend the existing value of the filters with the new
           * sample.
           *
           * I am not sure why there are two quantities maintained.  _less_smoothed I get; but
           * smoothing further to get _smoothed (which is the publicly available B/W estimate) I
           * don't.  I didn't see that in the Westwood/Westwood+ papers, nor is it really explained
           * in tcp_westwood.c except for one cryptic comment on the westwood->bw_ns_est struct
           * member.  Just do it.... */

          const n_bytes_t prev_bytes_per_time_less_smoothed = m_bytes_per_time_less_smoothed;
          const n_bytes_t prev_bytes_per_time_smoothed = m_bytes_per_time_smoothed;

          m_bytes_per_time_less_smoothed
            = apply_filter(m_bytes_per_time_less_smoothed, bytes_per_time_this_sample);
          m_bytes_per_time_smoothed
            = apply_filter(m_bytes_per_time_smoothed, m_bytes_per_time_less_smoothed);

          FLOW_LOG_TRACE("bw_est [" << sock << "] update: complete sample; "
                         "bw_est_less_smoothed "
                         "= filter[" << prev_bytes_per_time_less_smoothed << ", " << bytes_per_time_this_sample << "] "
                         "= [" << m_bytes_per_time_less_smoothed << "] units "
                         "= [" << util::to_mbit_per_sec<Time_unit>(m_bytes_per_time_less_smoothed) << " Mbit/s]; "
                         "bw_est "
                         "= filter[" << prev_bytes_per_time_smoothed << ", " << m_bytes_per_time_less_smoothed << "] "
                         "= [" << m_bytes_per_time_smoothed << "] units "
                         "= [" << util::to_mbit_per_sec<Time_unit>(m_bytes_per_time_smoothed) << " Mbit/s]; "
                         "units = bytes per [" << Time_unit(1) << "].");
        } // if (since_sample_start > min_sample_period)

        /* Start new sample.  Note that m_bytes_this_sample is about to get immediately incremented, as explained
         * at the top. */

        m_bytes_this_sample = 0;
        m_this_sample_start_time = Fine_clock::now();
      } // if (since_sample_start > min_sample_period)
    } // if (srtt is valid)
  } // if (2nd on_acks() and later)

  // As explained earlier, the actual acked bytes contribute to the next sample (if applicable), not preceding one.

  m_bytes_this_sample += bytes;
  FLOW_LOG_TRACE("bw_est [" << sock << "] update: received acks; sample "
                 "total now [" << sock->bytes_blocks_str(size_t(m_bytes_this_sample)) << "]; "
                 "after increment by [" << sock->bytes_blocks_str(bytes) << "].");
} // Send_bandwidth_estimator::on_acks()

Send_bandwidth_estimator::n_bytes_t Send_bandwidth_estimator::apply_filter(n_bytes_t prev_val,
                                                                           n_bytes_t new_sample_val) // Static.
{
  // Westwood filter: 7/8 * a + 1/8 * b.
  return ((7 * prev_val) + new_sample_val) / 8;
} // Send_bandwidth_estimator::apply_filter()

Peer_socket::Const_ptr Send_bandwidth_estimator::socket() const
{
  const Peer_socket::Const_ptr sock = m_sock.lock();

  // See comment in same spot in Congestion_control_strategy::socket().
  assert(sock);

  return sock;
}

} // namespace flow::net_flow

