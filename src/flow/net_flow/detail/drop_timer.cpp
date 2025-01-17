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
#include "flow/net_flow/detail/drop_timer.hpp"
#include "flow/util/util.hpp"

namespace flow::net_flow
{

// Implementations.

Drop_timer::Ptr Drop_timer::create_drop_timer
                  (log::Logger* logger_ptr,
                   util::Task_engine* node_task_engine,
                   Fine_duration* sock_drop_timeout,
                   Peer_socket::Const_ptr&& sock,
                   const Function<void (const Error_code& err_code)>& timer_failure,
                   const Function<void (bool drop_all_packets)>& timer_fired) // Static.
{
  // See doc comment for rationale.
  return Ptr(new Drop_timer(logger_ptr,
                            node_task_engine, sock_drop_timeout, std::move(sock),
                            timer_failure, timer_fired));
}

void Drop_timer::start_contemporaneous_events()
{
  FLOW_LOG_TRACE("Drop_timer [" << this << "]: contemporary events group: START.");

  assert(!m_in_events_group);
  m_in_events_group = true;

  m_at_events_start_oldest_flying_packet = m_flying_packets.empty() ? 0 : (*m_flying_packets.begin());
  m_during_events_newest_acked_packet = 0;
}

void Drop_timer::on_packet_in_flight(packet_id_t packet_id)
{
  using std::pair;
  using boost::prior;
  using std::set;

  FLOW_LOG_TRACE("Drop_timer [" << this << "]: packet [" << packet_id << "] now In-flight.");

  assert(m_in_events_group);

#ifndef NDEBUG
  const auto insert_result =
#endif
    m_flying_packets.insert(packet_id);
  // packet_id must never be reused (method contract).
  assert(insert_result.second);
  // packet_id must always be increasing (method contract).
  assert(insert_result.first == prior(m_flying_packets.end()));
}

void Drop_timer::on_ack(packet_id_t packet_id)
{
  using std::pair;
  using boost::prior;

  FLOW_LOG_TRACE("Drop_timer [" << this << "]: packet [" << packet_id << "] acknowledged.");

  assert(m_in_events_group);

  assert(util::key_exists(m_flying_packets, packet_id));

  // Keep track of the newest (right-most) acked packet. Starts off at 0 (reserved -- and also min. possible -- value).
  if (packet_id > m_during_events_newest_acked_packet)
  {
    FLOW_LOG_TRACE("Drop_timer [" << this << "]: acknowledged packet [" << packet_id << "] replaces "
                  "[" << m_during_events_newest_acked_packet << "] as right-most acknowledged packet.");
    m_during_events_newest_acked_packet = packet_id;
  }
}

void Drop_timer::on_packet_no_longer_in_flight(packet_id_t packet_id)
{
  FLOW_LOG_TRACE("Drop_timer [" << this << "]: packet [" << packet_id << "] now no longer In-flight.");

  assert(m_in_events_group);

#ifndef NDEBUG
  const size_t num_erased =
#endif
    m_flying_packets.erase(packet_id);
  assert(num_erased == 1);
}

void Drop_timer::on_no_packets_in_flight_any_longer()
{
  FLOW_LOG_TRACE("Drop_timer [" << this << "]: all packets now no longer In-flight.  Send pipe empty.");

  assert(m_in_events_group);

  /* As advertised. (This could be seen as unnecessarily pedantic... but algorithmic tightness is good. This is no
   * public API but is inner code instead.) */
  assert(!m_flying_packets.empty());

  m_flying_packets.clear();
}

void Drop_timer::end_contemporaneous_events()
{
  FLOW_LOG_TRACE("Drop_timer [" << this << "]: contemporary events group: END.");

  assert(m_in_events_group);
  m_in_events_group = false;

  /* Now that the events-tracking time period is done, we check for the various events of interest that affect
   * the timer itself. Essentially, we treat the on_*() calls since start_contemporaneous_events() up until now
   * as one operation. By comparing the situation "then" to "now," we detect the various conditions. */

  if (m_at_events_start_oldest_flying_packet == 0) // I.e., m_flying_packets.empty() was true at start_...() time.
  {
    if (!m_flying_packets.empty())
    {
      FLOW_LOG_TRACE("Drop_timer [" << this << "]: send pipe no longer empty (first packet In-flight).");

      /* Sent the first packet after nothing In-flight, so timer should be off.  Simply start it with
       * current DTO.  RFC 6298 and common sense support this.  Note, though, that we're slightly different, in that
       * we react to packet actually being sent off vs. merely being about to be sent off pending pacing. */
      start_timer();
    }
  }
  else if (m_flying_packets.empty()) // I.e., m_flying_packets.empty() went from false (then) to true (now).
  {
    FLOW_LOG_TRACE("Drop_timer [" << this << "]: send pipe empty.");

    /* Timer should be on, but last packet(s) have been acked or Dropped -- nothing else to wait
     * for; so just turn the timer off.  RFC 6298 and common sense support this. */
    disable_timer();
  }
  else // I.e., m_flying_packets.empty() was false (then) -- and still is (now).
  {
    const packet_id_t at_events_end_oldest_packet = *m_flying_packets.begin();

    if (at_events_end_oldest_packet != m_at_events_start_oldest_flying_packet)
    {
      // A different packet is oldest (now) vs. oldest packet (then). The latter must have been Acked or Dropped.

      FLOW_LOG_TRACE("Drop_timer [" << this << "]: send pipe's left edge moved to the right.");

      /* Timer should be on, but the earliest unacked packet has been Acknowledged or
       * Dropped; an In-flight packet remains.  In all reasonable algorithms the timer has to be
       * restarted in this situation.  RFC 6298 and common sense support this.
       *
       * (Otherwise suppose I've sent packets 1-10, one after the previous with 1 second between them.
       * Thus I started timer at time 0 (when packet 1 was sent, since it was the first packet to be
       * sent) and sent packet 10 ten seconds later.  Suppose DTO is 30 seconds, a second passes since
       * packet 10 is passed, and packet 1 is acked.  If I don't restart the timer now, then that
       * means (for example) packet 10 will have had ~10 seconds less (overall, relative to send time)
       * than packet 1 before being Dropped.  This violates the RFC 6299 section 5 rule that the
       * algorithm MUST allow at least a full RTO (DTO) since a given packet is sent before dropping
       * that packet.) */
      disable_timer();
      start_timer();
    }
    else if (m_during_events_newest_acked_packet != 0)
    {
      /* We have detected an out-of-order acknowledgment, which may mean we should restart time.
       * Let's discuss in detail.
       *
       * Firstly, note that unlike various cases covered so far, this situation is NOT mutually exclusive w/r/t
       * the pipe-left-edge-moved-right situation covered just above.  However, since the potential action
       * for both situations is to restart the timer, it would be pointless to check for the 2nd event, if the
       * 1st is already true. So that's why it's an 'else if.'  So, from now on, assert that in fact
       * at_events_end_oldest_packet == m_at_events_start_oldest_flying_packet.  In fact this makes the following
       * reasoning quite simple vs. the alternative (had we not asserted that).
       *
       * Now, for what did we just check? 0 != m_during_events_newest_acked_packet
       * means that, during the events group, the pipe's left edge did not move (previous paragraph again), but
       * _a_ packet in the pipe was acknowledged (it must therefore no longer be in pipe). In fact this packet
       * was "to the right of" (sent more recently than) that left-edge packet.
       * This means at least one packet was acknowledged, but it wasn't the one that's "due" for acknowledgment
       * (namely the packet m_at_events_start_oldest_flying_packet == at_events_end_oldest_packet).  So what do
       * we do based on this information?
       *
       * We can do one of two things here: (1) nothing (let timer run); or (2) restart the timer.
       * To be clear, (1) comes from a certain classic RFC mentioned just below. We can either follow that RFC's
       * recommendatio (restart timer here) or not (do nothing). Here are the arguments for each:
       *
       *   -1- Action: Let timer run (thus giving the earliest packet no recourse: do not follow RFC):
       *
       *   Pro: If we assume that, essentially, we're simulating a DTO for each individual packet,
       *        then having received acknowledgment for some packet P in no way means that an earlier
       *        packet P' was also acknowledged.  I.e., if our one timer is meant to apply to the
       *        next-to-be-dropped (earliest unacked) packet only -- since the other packets' implied
       *        timers can only fire after that one -- then a later packet being acked doesn't negate
       *        the fate that the earliest packet is still outstanding.  In other words restarting the
       *        timer here is more lax than the "Drop Timeout per individual packet" setup, thus we
       *        are being more precise w/r/t to the concept of the timer by letting it run.
       *   Pro: RFC 6298-5.3 says "[if] ACK is received that acknowledges new data, restart the
       *        retransmission timer."  This can be interpreted to mean a CUMULATIVE ACK that
       *        acknowledges new data, as opposed to an out-of-order SACK (which is what we have
       *        here).  I am not sure if the RFC means that.  But if it means the former (cumulative ack),
       *        then we're NOT going against the RFC, which is a pro... well, not a con at least.
       *   Con: Assuming the RFC section 5-RECOMMENDED algorithm *doest* mean that SACKs = acknowledging "new
       *        data" (quoting RFC) as well, then we are being MORE aggressive than the RFC algorithm -- and
       *        being more aggressive could be bad and/or wrong.
       *
       *   -2- Action: Restart the timer (thus giving the earliest packet a new lease on life: follow RFC):
       *
       *   Pro: RFC 6298-5.3 may mean that either an ACK or a SACK acknowledging any unacked
       *        packets (whether in-order or out-of-order) means we should restart the timer.  In that
       *        case we'd be directly following the RFC if restarting the timer.
       *   Pro: Even if not, by restarting the timer we are being LESS aggressive than the RFC, which
       *        is explicitly allowed by the RFC (we MUST not give any packet less than the DTO time,
       *        but we may give it more).
       *   Con: Restarting the timer is more lax than the "Drop Timeout per individual packet"
       *        setup, and the latter has an aesthetically pleasant logic to it.
       *
       * Therefore we make this configurable behavior.  When in doubt and without experimental backup,
       * do go for the less aggressive option (Restart the timer).
       *
       * @todo We didn't have to keep track of m_during_events_newest_acked_packet (the most-recently-sent
       * packet to be acked).  We could have just replaced it with bool m_during_events_ack_occurred, starting at false
       * but set to true in any on_ack().  Since a (valid) ack always subsequently removes that packet from
       * m_flying_packets, m_at_events_start_oldest_flying_packet still being in m_flying_packets implies that a packet
       * other than the left-most one was in fact acked... which is all we are lookig for in the first place.  However,
       * keeping the packet ID allows us to, at least, sanity-check some things via assert().  We can also log some
       * arguably useful packet IDs, e.g., in on_ack().  So arguably it's worth keeping.  The performance cost looks
       * minor (though this has not been profiled). */

      FLOW_LOG_TRACE("Drop_timer [" << this << "]: out of order acknowledgment detected.");

      // The following are all sanity checks to ensure situation is what we've reasoned out in comment just above.
      assert(at_events_end_oldest_packet == m_at_events_start_oldest_flying_packet);
      assert(at_events_end_oldest_packet < m_during_events_newest_acked_packet);
      assert(!util::key_exists(m_flying_packets, m_during_events_newest_acked_packet));

      /* An ACK acknowledged at least one packet, but the earliest (first) In-flight packet was NOT
       * acknowledged (and thus remains the earliest In-flight packet).
       *
       * We can do one of two things here: (1) nothing (let timer run); or (2) restart the timer.
       * Here are the arguments for each:
       *
       *   -1- Let timer run (thus giving the earliest packet no recourse):
       *
       *   Pro: RFC 6298-5.3 says "ACK is received that acknowledges new data, restart the
       *        retransmission timer."  This can be interpreted to mean a CUMULATIVE ACK that
       *        acknowledges new data, as opposed to an out-of-order SACK (which is what we have
       *        here).  I am not sure if the RFC means that.
       *   Pro: If we assume that, essentially, we're simulating a DTO for each individual packet,
       *        then having received acknowledgment for some packet P in no way means that an earlier
       *        packet P' was also acknowledged.  I.e., if our one timer is meant to apply to the
       *        next-to-be-dropped (earliest unacked) packet only -- since the other packets' implied
       *        timers can only fire after that one -- then a later packet being acked doesn't negate
       *        the fate that the earliest packet is still outstanding.  In other words restarting the
       *        timer here is more lax than the "Drop Timeout per individual packet" setup, thus we
       *        are being more precise w/r/t to the concept of the timer by letting it run.
       *   Con: Assuming the RFC section 5 RECOMMENDED algorithm means that SACKs = acknowledging new
       *        data as well, then we are being MORE aggressive than the RFC algorithm which could be
       *        bad/incorrect.
       *
       *   -2- Restart the timer (thus giving the earliest packet a new lease on life):
       *
       *   Pro: RFC 6298-5.3 may mean that either an ACK or a SACK acknowledging any unacked
       *        packets (whether in-order or out-of-order) means we should restart the timer.  In that
       *        case we'd be directly following the RFC if restarting the timer.
       *   Pro: Even if not, by restarting the timer we are being LESS aggressive than the RFC, which
       *        is explicitly allowed by the RFC (we MUST not give any packet less than the DTO time,
       *        but we may give it more).
       *   Con: Restarting the timer is more lax than the "Drop Timeout per individual packet"
       *        setup, and the latter has an aesthetically pleasant logic to it.
       *
       * Therefore we make this configurable behavior.  When in doubt and without experimental backup,
       * do go for the less aggressive option (Restart the timer). */
      if (m_sock->opt(m_sock->m_opts.m_st_out_of_order_ack_restarts_drop_timer))
      {
        disable_timer();
        start_timer();
      }
    } // else if (m_during_events_newest_acked_packet != 0)
  } // if (m_flying_packets.empty() was false -- and still is)
} // Drop_timer::end_contemporaneous_events()

Drop_timer::Drop_timer(log::Logger* logger_ptr,
                       util::Task_engine* node_task_engine,
                       Fine_duration* sock_drop_timeout,
                       Peer_socket::Const_ptr&& sock,
                       const Function<void (const Error_code& err_code)>& timer_failure,
                       const Function<void (bool drop_all_packets)>& timer_fired) :
  log::Log_context(logger_ptr, Flow_log_component::S_NET_FLOW),
  m_node_task_engine(*node_task_engine),
  m_sock_drop_timeout(*sock_drop_timeout),
  m_sock(std::move(sock)),
  m_timer(m_node_task_engine),
  m_timer_running(false),
  m_current_wait_id(0),
  m_done(false),
  m_timer_failure(timer_failure),
  m_timer_fired(timer_fired),
  m_at_events_start_oldest_flying_packet(0),
  m_during_events_newest_acked_packet(0),
  m_in_events_group(false)
{
  FLOW_LOG_TRACE("Drop_timer [" << this << "] created.");
}

Drop_timer::~Drop_timer()
{
  FLOW_LOG_TRACE("Drop_timer [" << this << "] destroyed.");
}

void Drop_timer::done()
{
  if (m_done)
  {
    return;
  }

  FLOW_LOG_TRACE("Drop_timer [" << this << "] permanently disabled.");

  if (m_timer_running)
  {
    // If timer is scheduled, expedite it firing.
    disable_timer();
  }
  // Short-circuit any subsequent callbacks to immediately return.
  m_done = true;

  /* These function "pointers" will be "pointing" to some function objects that are holding
   * arguments to the underlying methods to call, via labmda or bind().  These arguments will include
   * such resources as Peer_socket::Ptr.  Clear the "pointers" to delete the function objects, thus
   * letting those resources expire (in case of Ptr, let the underlying Peer_socket's ref-count get
   * decremented, avoiding a memory leak). */
  m_timer_failure.clear();
  m_timer_fired.clear();
}

void Drop_timer::start_timer()
{
  using log::Sev;
  using boost::asio::post;
  using boost::chrono::microseconds;
  using boost::chrono::milliseconds;
  using boost::chrono::round;

  // Explicitly documented pre-condition.
  assert(!m_timer_running);

  /* Determine when the timer will fire.  Due to the interface of Timer, we need either a
   * Fine_duration (for expires_after()) or a Fine_time_pt (for expires_at()): how long from now,
   * or when in absolute terms, respectively.
   *
   * The obvious first choice would be simply the DTO from now, so
   * expires_after(m_sock_drop_timeout), where the latter is a Fine_duration.
   *
   * A more complex setup we may want is: the timer firing in m_sock_drop_timeout time units since the time
   * of the earliest (first) current In-flight packet was sent.  Call that initial time point P.  (P is a
   * Fine_time_pt.)  So this is: expires_at(P + m_sock_drop_timeout), where the terms are Fine_time_pt
   * and Fine_duration, respectively.
   *
   * So which one should we use?  RFC 6298 says use RTO (our DTO) from the current time.  This is also the
   * less aggressive choice, towards which the RFC is generally biased.  On the other hand, if we want to give
   * each given packet no less and NO MORE than DTO before Dropping, then we'd use the other policy. So, it's
   * configurable.  However when in doubt and in the absence of experimental data go with the less aggressive
   * option, which is to just fire DTO from now, as opposed to from P. */

  /* The time point of firing is either duration from now(), or absolute time point.  The former is still
   * computed for logging even if not used directly. */
  Fine_duration fire_duration_vs_now; // Caution!  This is uninitialized.
  Fine_time_pt fire_time_pt;
  bool use_time_pt_over_duration;
  if (m_sock->opt(m_sock->m_opts.m_st_drop_packet_exactly_after_drop_timeout))
  {
    // The more aggressive choice.  Fire in DTO but adjust by the time that has passed since earliest packet sent.

    // Get time when earliest packet sent.
    assert(!m_sock->m_snd_flying_pkts_by_sent_when.empty()); // Timer must not be started when no packets In-flight.
    const Fine_time_pt& first_packet_sent_when
      = m_sock->m_snd_flying_pkts_by_sent_when.const_back().second->m_sent_when.back().m_sent_time;

    fire_time_pt = first_packet_sent_when + m_sock_drop_timeout;
    use_time_pt_over_duration = true;

    // Compute firing time for logging (skip it if logging macro will not actually log it though).
    auto const logger_ptr = get_logger();
    if (logger_ptr && logger_ptr->should_log(Sev::S_TRACE, get_log_component()))
    {
      fire_duration_vs_now = fire_time_pt - Fine_clock::now();
    }
  }
  else
  {
    // The less aggressive choice.  Just fire in DTO time units.
    fire_duration_vs_now = m_sock_drop_timeout;
    use_time_pt_over_duration = false;
  }

  FLOW_LOG_TRACE("Drop_timer [" << this << "] setting timer to "
                 "[" << round<milliseconds>(fire_duration_vs_now) << "] from now; "
                 "wait_id = [" << (m_current_wait_id + 1) << "].");

  /* Note that fire_duration_vs_now may be somewhat negative, particularly in the
   * m_st_drop_packet_exactly_after_drop_timeout case above after on_packet_no_longer_in_flight()'s
   * if-left-most-In-flight-packet-has-shifted case.
   * That's fine: timer will just fire as soon as m_task_engine gets a chance.  It's no different
   * from it just firing really soon due to a short timeout.  Since each timer firing will cause
   * Node to Drop one packet, this chain of events will finish in short order once it runs out of
   * packets for which it should be firing. */

  const auto this_ptr = shared_from_this();

  // Set the firing time decided above.
  use_time_pt_over_duration
    ? m_timer.expires_at(fire_time_pt)
    : m_timer.expires_after(fire_duration_vs_now);

  /* Call handle_timer_firing() asynchronously.  this_wait_id will be used to identify which start_timer() caused
   * that callback to fire.  this_ptr used as above. */
  const auto this_wait_id = ++m_current_wait_id;
  m_timer.async_wait([this, this_ptr, this_wait_id](const Error_code& sys_err_code)
  {
    handle_timer_firing(this_ptr, this_wait_id, sys_err_code);
  });

  m_timer_running = true;
} // Drop_timer::start_timer()

void Drop_timer::disable_timer()
{
  using boost::chrono::milliseconds;
  using boost::chrono::round;

  // Explicitly documented pre-condition.
  assert(m_timer_running);

  /* As discussed elsewhere, m_timer.cancel() is not enough to ensure handle_timer_firing() doesn't
   * get called; if the latter is already queued to fire right now then we won't be able stop it.
   * Moreover even if successful cancel() will still call handle_timer_firing() but with
   * operation_aborted Error_code.  That's why we set m_timer_running = false.
   * handle_timer_firing() will see that and do nothing, as if it had been canceled. */

  FLOW_LOG_TRACE("Drop_timer [" << this << "] disabling; was set to fire in "
                 "[" << round<milliseconds>(m_timer.expiry() - Fine_clock::now()) << "]; "
                 "wait_id = [" << m_current_wait_id << "].");

  // Try to cancel any pending waits (call ASAP with err_code = operation_aborted).
  m_timer.cancel();

  // Even if something was already queued, this will inform it that it was canceled.
  m_timer_running = false;
} // Drop_timer::disable_timer()

void Drop_timer::handle_timer_firing([[maybe_unused]] Ptr prevent_destruction,
                                     timer_wait_id_t wait_id,
                                     const Error_code& sys_err_code)
{
  using std::min;
  using boost::chrono::milliseconds;
  using boost::chrono::round;

  if (m_done // done() called.
      || (sys_err_code == boost::asio::error::operation_aborted) // disable_timer() called cancel() successfully.
      || (!m_timer_running) // cancel() was too late, but disable_timer() did try to cancel us nonetheless.
      || (m_current_wait_id != wait_id)) // Someone recently rescheduled timer, but we still fired from earlier.
  {
    FLOW_LOG_TRACE("Drop_timer [" << this << "] fired but obsolete/canceled; "
                   "done = [" << m_done << "]; "
                   "aborted = [" << (sys_err_code == boost::asio::error::operation_aborted) << "]; "
                   "timer_running = [" << m_timer_running << "]; "
                   "wait_id = [" << wait_id << "]; current_wait_id = [" << m_current_wait_id << "].");
    return;
  }
  // else

  FLOW_LOG_TRACE("Drop_timer [" << this << "] fired; wait_id = [" << wait_id << "].");

  if (sys_err_code)
  {
    FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log non-portable error.
    // Nothing else to do here.  We don't know what this means.  So just treat it as if timer was triggered.
  }

  // OK, timer triggered, and we're the active wait handler.  Decide what to do.

  /* RFC 6298 says that the RTO (DTO in our case) MUST be doubled each time the timeout expires.
   * We'll make this a knob for experimentation's sake.  */

  // Change the Peer_socket's DTO!

  // Get the option values.  Lock, obtain, unlock.  (A least overzealous -- but why not?)
  // @todo That last comment in parentheses is not grammatical, and I now don't know what it may have
  // meant, when I originally wrote it. Attempt to understand/clarify....
  unsigned int backoff_factor;
  Fine_duration ceiling;
  {
    Peer_socket::Options_lock lock(m_sock->m_opts_mutex);
    backoff_factor = m_sock->m_opts.m_dyn_drop_timeout_backoff_factor;
    ceiling = m_sock->m_opts.m_dyn_drop_timeout_ceiling;
  }

  if (backoff_factor != 1)
  {
    assert(backoff_factor != 0);
    m_sock_drop_timeout *= backoff_factor;
    m_sock_drop_timeout = min(m_sock_drop_timeout, ceiling);
    FLOW_LOG_TRACE("Increased DTO by factor [" << backoff_factor << "] with "
                   "ceiling [" << round<milliseconds>(ceiling) << "] to "
                   "[" << round<milliseconds>(m_sock_drop_timeout) << " = " << m_sock_drop_timeout << "].");
  }

  /* Certainly if the DTO was exceeded, at least the earliest (first) In-flight packet must be
   * considered Dropped.  (BTW it must exist; if there are no In-flight packets then they had to
   * have called disable_timer() or never called start_timer(), so we cannot be here.)
   *
   * Additionally, we could want to drop ALL In-flight packets.  RFC 6298 does not say to do that,
   * so doing so would be MORE aggressive than the RFC, which the RFC says MUST not be the done.
   * However, that's just more aggressive w/r/t their RECOMMENDED algorithm in section 5.  That's
   * not all however: it also explicitly violates the REQUIREMENT that no packet should be Dropped
   * for DTO reasons unless it was sent at least DTO ago.  E.g., if I sent packet 1 and then in 10
   * seconds packet 2, then DTO fires in in 1 second, then packet 2 was sent only 1 second ago yet
   * is Dropped, violating the requirement.
   *
   * I've seen certain lesser known custom protocols in fact Drop all packets in this case.
   * I didn't really know why, but eventually I realized they got that from the DCCP CCID 2
   * specification (RFC 4341).  DCCP is a UDP-based protocol similar to ours (especially similar
   * when retransmission is disabled).  That RFC says specifically that a DTO firing should cause
   * pipe to be set to zero, which means we should drop all packets.  No justification is given, but
   * this is serious Standards Track RFC, and the first author is Sally Floyd (who wrote the
   * original SACK RFC among others).  It does make some intuitive sense that whatever led to DTO
   * means all the data are gone from the pipe.  On the other hand, straight TCP RFCs like 6298
   * assume retransmission is involved, in which case my brain can't quite see how the pipe=0 move
   * is consistent with retransmission and cumulative ACKs, so maybe that's the source of the
   * difference between the two.
   *
   * Anyway, I make it a knob.  Given the DCCP CCID 2 instruction, I'd probably go with
   * m_st_drop_all_on_drop_timeout == true. */

  // Note: m_timer_running = true.

  // Instruct Node what to do with the packet(s) (at least drop one).  This happens SYNCHRONOUSLY (right here).
  m_timer_fired(m_sock->opt(m_sock->m_opts.m_st_drop_all_on_drop_timeout));

  /* They marked one or all In-flight packets Dropped just now.  It was also their
   * responsibility to call on_packet_no_longer_in_flight() accordingly. */
} // Drop_timer::handle_timer_firing()

void Drop_timer::timer_failure([[maybe_unused]] Ptr prevent_destruction, const Error_code& err_code)
{
  if (!m_done)
  {
    m_timer_failure(err_code);
  }
  // else they're not interested in the timer anymore, including any error, so don't call their failure callback.
}

} // namespace flow::net_flow
