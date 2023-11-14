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
#include "flow/net_flow/detail/low_lvl_packet.hpp"
#include "flow/util/linked_hash_set.hpp"

namespace flow::net_flow
{

/**
 * The current outgoing packet pacing state, including queue of low-level packets to be sent, for a
 * given Peer_socket.
 *
 * This structure is a data store and not an object.  It is a separate `struct`, instead of directly
 * inside Peer_socket, to make the complex Peer_socket structure's definition shorter and easier to
 * understand.  Accordingly the Node code dealing with pacing (and working on this structure and
 * related data) is in a separate file (low_lvl_io.cpp instead of peer_socket.cpp or
 * node.cpp).
 *
 * ### Background on packet pacing ###
 * Empirically we've seen that sending out many UDP datagrams at the same
 * time may cause extra -- sometimes quite major -- loss somewhere in the network.  To combat that,
 * we've seen simple packet pacing be effective (and I hear Linux kernel does it too).  The idea is
 * to keep datagrams being sent at below a certain dynamically adjusting rate.  More specifically,
 * if we want to (and, subject to the send window, are allowed to) send N equally sized available
 * packets over T msec, it may prevent loss to send them equally spaced T/N msec apart, as
 * opposed to sending a burst of N packets immediately.  In other words the total send rate over
 * time period T is the same in either case, but in the latter case this is accomplished smoothly
 * instead of burstily.  This is pacing: minimize burstiness without lowering overall throughput.
 * The pending packets that cannot be sent immediately are added to a queue to be sent later.
 *
 * Suppose we keep the pacing system as above; begin a pacing time slice of length T, and over that time
 * period send packets spaced at T/N msec apart; once T is over, recompute T and N and repeat.  Let T be
 * decided.  We shouldn't use N = [number of packets pending to send], since at any moment many more can come
 * in and get queued (causing backup of the queue).  The natural solution is to use T = round trip time (SRTT)
 * and N = congestion window (CWND), since those by definition set the maximum rate we will allow packets to
 * be sent out, because the network cannot support more.  Thus, the interval between packets is S = SRTT /
 * CWND.
 *
 * To make this system more responsive, S = SRTT / CWND can be recomputed after queued packet is
 * sent (instead of recomputing it every SRTT as mentioned above; as SRTT and CWND are continuously
 * computed as acknowledgments come in, much more frequently than every SRTT).
 *
 * More precisely, whenever we send a packet, start a timer for S = SRTT / CWND msec.  While the
 * timer is running any new packet available for sending should be added to the packet queue Q and
 * not sent.  Once tomer fires, send packet at top of Q, if any (and start the timer again,
 * since packet was sent).  If Q is empty, and the timer is not running, and a new packet is
 * available for sending, it should be sent immediately.  Finally, any packet available for sending
 * until SRTT is known (i.e., before the first acknowledgment comes in) should be sent immediately
 * (all packets after that are paced per above algorithm).
 *
 * There is a technical limitation that complicates the above.  The timer period S can easily be
 * quite low, e.g., S = 100 msec / 100 = 1 msec.  In the current implementation (boost.asio) there
 * is no timer mechanism with that precision (platform-dependent, but see comment on util::Timer alias)
 * (we can *measure* time with much better precision, but we cannot *schedule* actions).  Therefore we
 * cannot always schedule timer for S.  Suppose the timer has a minimum scheduling precision of < R.
 * Since we mathematically cannot avoid burstiness if S = SRTT / CWND < R, we must minimize it;
 * namely, in that case, set S = R and, instead of 1 packet max during that period,
 * allow N = R / (SRTT / CWND) packets during that period.  Thus the simple "queue packet each time
 * it is available, if timer is running" policy changes to the slightly more complex "start counter
 * K = N when timer starts; send packet and decrement K each time packet is available, until K is
 * down to zero, then queue packet."  Note that the latter reduces to the former when S >= R (i.e.,
 * timer precision is not a problem).
 *
 * Now to formally define the invariants guiding the algorithm: let [T, T + S) be a time slice,
 * where T is the time point at which the first packet is sent once an SRTT is known.  S =
 * max(floor(SRTT / CWND), R), where SRTT and CWND (which is in units of max-block-sizes) are
 * determined at time T.  Each packet should be sent out as soon as possible after it becomes
 * available for sending, subject to the following constraint: at most N = S / (SRTT / CWND)
 * full-sized blocks' worth of packets may be sent during the time slice [T, T + S); any packets
 * beyond that should be queued on packet queue Q (and sent based on the policy in his sentence, in
 * order of Q).  After the time slice is finished, the next time slice [T, T + S) is re-defined
 * at the time T the first packet available for sending appears (at which time S and N are
 * recomputed), and the above policy applies.  This continues indefinitely.
 *
 * ### Packets other than DATA ###
 * There is one final consideration.  The above discussion implicitly
 * treats all packets as DATA packets.  Since SYN/SYN_ACK/SYN_ACK_ACK packets all occur before the
 * first SRTT measurement is available (but see below to-do), they are not relevant.  However, there
 * are also ACK and RST packets.  First, let's deal with RST; an RST is by definition the very last
 * packet we can send on a connection.  We have no desire for it to be dropped any more than a DATA
 * packet; so it can be subject to pacing just like DATA and placed onto Q, etc.  Since it's the
 * last packet to be sent, there is nothing more to discuss.  However, that would mean the sending
 * of RST may occur after socket is in state CLOSED.  While we could make that work, it introduces
 * unnecessary complication; we prefer to consider the Peer_socket quite dead once it is in CLOSED.
 * Therefore I choose to not pace RSTs: send it immediately.  This MAY put the RST ahead of other
 * queued packets (ACK, DATA), but since RST indicates a sudden connection break, this is more or less
 * acceptable (and those other packets will not be sent, as we stop sending paced packets once in
 * CLOSED).  It may also increase the chance the RST is dropped, but I highly doubt it's significant
 * in reality.
 *
 * What about ACKs?  Probably we must place them onto Q, since sending them earlier would at least
 * mean changing the original order of Node's intended outgoing packet stream.  However, how many
 * bytes, in comparison to (N * max-block-size), is an ACK worth?  We could do this a couple of
 * ways.  It could be worth max-block-size, like a typical DATA packet; this is the conservative
 * choice.  However, it's probably not too realistic, since an ACK is much smaller than a full DATA.
 * It could be worth 0 bytes (i.e., send any ACK as soon as it's at the head of Q, regardless of
 * time slice), which is closer to the truth but not true (the other way).  Or it could be worth ~K
 * bytes, where K is the approximate size of its payload (not counting the common header data).  That
 * is most realistic, but would throw off a bit the appealing exact math when all DATA packets are
 * full-sized.  What to choose?  The answer is probably not too important, because in most
 * applications there will be no significant mixing of outgoing DATA and ACK packets (since either
 * one side or the other side is usually sending, not both simultaneously).  I choose to go with ACK
 * = 0 bytes, since it is fairly close to realistic yet also simple to implement.
 *
 * ### Thread safety ###
 * Meant to be accessed from thread W only.
 *
 * @todo One way to provide finer pacing, in the situation where we must send more than 1 packet
 * within the minimal schedulable timer period, is to rely on events other than timer clock ticks
 * that occur more frequently.  Namely, ACKs will possibly arrive faster that the minimal timer
 * schedulable ticks do.  Therefore, when a time slice begins, we can send no packets (instead of
 * all the packets that must be sent in that time slice, as is the case currently).  Then over the
 * course of the time slice we can opportunistically -- upon each ACK send the proper number of
 * queued packets up to that point into the time slice (this can be computed accurately, because we
 * can MEASURE time very accurately -- just can't schedule things as accurately).  Finally, once the
 * time slice does expire, send off any packets still remaining for that slice.  This way we can
 * opportunistically provide finer pacing, where the ACK clock allows it.
 *
 * @todo Obtain the first RTT measurement based on SYN-SYN_ACK or SYN_ACK-SYN_ACK_ACK interval; then
 * SRTT will be available from the start, and packet pacing can be enabled with the very first DATA
 * packet, avoiding the initial (albeit small) burst of DATA packets.  This is pretty easy.
 *
 * @todo As noted above we perform packet pacing but currently choose to assign a value of
 * 0 bytes to an ACK.  That is, while we do preserve the order of DATA and ACK packets -- if
 * both happen to be in the outgoing stream -- we do not delay the sending of the ACK once it is
 * the next packet to be sent out.  However, even so, an ACK's sending may be delayed by the
 * pacing applied to DATA packets intermixed with it.  Therefore the ACK delay measurement we
 * take here may be incorrect (too low) in that case.  This can cause overestimated RTTs on the
 * sender's side.  The to-do is to correct the ACK delay value in a given ACK by adding the
 * pacing delay (if any) of the ACK to the individual ACK delays within it.  Conceptually this
 * is similar to the `m_sent_when` value being set when choosing to send a DATA packet and then
 * corrected in the pacing module later.  This to-do is not important until we in practice start
 * mixing sending and receiving at the application layer... but still -- it's worth knowing that
 * there is a design bug here.
 *
 * @todo My quick look into Google BBR, the recent advanced congestion control algorithm, suggests
 * that the pacing algorithm can be used as part of congestion control rather than something merely
 * applied after congestion control has made the CWND decision.  I believe the idea would be this:
 * If we can somehow very reliably determine the available bandwidth, which Google BBR does purport to do,
 * then instead of controlling send rate via CWND, instead one could apply it to this pacing module.
 * It is a slight modification of the algorithm described above: instead of the rate of sending being
 * equal to CWND/SRTT, make it equal to the bandwidth determined through Google BBR.  If I understand
 * correctly, Google BBR does maintain a CWND as well, but this is a safety upper bound, in case the
 * computed bandwidth is too high to be practical (in this case CWND will slow down data being delivered
 * to the pacing module -- but, normally, this would not be the case, and the pacing module would be,
 * in practice, the thing controlling the sending rate).  Note this is a pretty big conceptual change;
 * in particular, it would make the queue Q grow much larger than what we see currently.
 *
 * ### `Timer` vs util::schedule_task_from_now() ###
 * In other places we have tended to replace a `Timer` with the far simpler util::schedule_task_from_now() (etc.)
 * facility (which internally uses a `Timer` but hides its various annoyances and caveats).  Why not here?
 * Answer: These tasks are potentially scheduled *very* frequently; and the firing periods can push the limits of how
 * small they can effectively be.  Therefore, the perf caveats (as listed in util::schedule_task_from_now() doc header)
 * apply in a big way -- moreover, exactly when the effects would be felt the most (namely at high throughputs -- which
 * in turn is likeliest to peg process use).  We should definitely not risk that in this case.
 */
struct Send_pacing_data :
  private boost::noncopyable
{
  // Types.

  /// Short-hand for FIFO of low-level packets.  `queue<>` dumbly has no `clear()`, so just use `deque<>` directly.
  using Packet_q = std::deque<Low_lvl_packet::Const_ptr>;

  // Data.

  /**
   * The time point at which the last pacing time slice began; or epoch if no packets sent so far
   * (i.e., no time slices yet => undefined).  The time period
   * [#m_slice_start, #m_slice_start + #m_slice_period) defines the range of time points in the time
   * slice.
   */
  Fine_time_pt m_slice_start;

  /**
   * The length of the current pacing time slice period; this depends on congestion window and SRTT
   * on the containing socket, at the time point #m_slice_start.  Undefined (`zero()`) if #m_slice_start
   * is undefined.
   */
  Fine_duration m_slice_period;

  /**
   * This many bytes worth of DATA packets may still be sent, at this time, within the
   * time slice defined by #m_slice_start and #m_slice_period; if this is less than the data size of
   * `m_packet_q.front()`, then no more can be sent, and any packets that need to be sent must be sent
   * in the next time slice (i.e., at time point `>= m_slice_start + m_slice_period`).  Undefined
   * (zero) if #m_slice_start is undefined.
   *
   * Why track it in bytes instead of multiples of max-block-size (as in the `struct` doc header)?
   * Since the DATA packets in the queue may be of varying sizes, this allows us to more precisely
   * (proportionally) count them against the #m_bytes_allowed_this_slice budget.
   */
  size_t m_bytes_allowed_this_slice;

  /**
   * Queue of low-level packets to be sent to the remote endpoint, in order in which they are to be
   * sent, with the head element the one to be sent next.  Each new pending packet is to be added at
   * the tail.  Invariant: `m_packet_q.front()`, if it exists, is of type DATA after any
   * Node::sock_pacing_new_packet_ready() or Node::sock_pacing_process_q() call returns.  (This is consistent
   * with the aforementioned "do not re-order non-DATA packets, but do not pace them" philosophy.)
   */
  Packet_q m_packet_q;

  /**
   * When running, #m_packet_q is non-empty, #m_bytes_allowed_this_slice < data size of
   * `m_packet_q.front()`, and the timer will fire when this time slice ends, and thus the head packet
   * in #m_packet_q must be sent.  In all other situations the timer is inactive.  (Note that if the
   * timer is not running, that does not mean any packet becoming available can be sent immediately;
   * if the queue is empty and `m_bytes_allowed_this_slice == N`, the timer is off, but if a packet
   * comes in with data size > `N` before `m_slice_start + m_slice_period` [within the current time
   * slice], then #m_slice_timer must be set to fire at time `m_slice_start + m_slice_period`.)
   */
  util::Timer m_slice_timer;

  // Constructors/destructor.

  /**
   * Initializes data to initial state (no active time slice).
   *
   * @param task_engine
   *        IO service for the timer stored as data member.
   */
  explicit Send_pacing_data(util::Task_engine* task_engine);
}; // struct Send_pacing_data

} // namespace flow::net_flow
