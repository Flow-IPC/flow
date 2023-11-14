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
#include "flow/net_flow/options.hpp"
#include "flow/util/util.hpp"
#include "flow/log/log.hpp"
#include <map>
#include <iostream>
#include <boost/weak_ptr.hpp>

namespace flow::net_flow
{
// Types.

/**
 * The abstract interface for a per-socket module that determines the socket's congestion control
 * behavior.  Each Peer_socket is to create an instance of a concrete subclass of this class,
 * thus determining that socket's can-send policy.
 *
 * ### Congestion control ###
 * This term refers to the process of deciding when to send data, assuming data are available
 * to send.  In a magic world we could send all available data immediately upon it becoming
 * available, but in the real world doing so at a high volume or in poor networks will eventually
 * lead to packets being dropped somewhere along the way or at the destination.  To determine
 * whether we can send data (if available), we maintain `m_snd_flying_bytes` (how much we think is
 * currently In-flight, i.e. sent by us but not yet Acknowledged by the receiver or Dropped by the
 * network or receiver) and CWND (how much data we think the route, or pipe, can hold In-flight
 * without dropping too much).  Basically if Peer_socket::m_snd_flying_bytes < CWND, data could be sent.
 *
 * How to determine CWND though?  That is the main question in congestion control.  In `net_flow`,
 * the Peer_socket data member `Congestion_control_strategy sock->m_cong_ctl` provides the API that
 * returns CWND (how it does so depends on the real type implementing that interface).  In order to
 * compute/update CWND, Congestion_control_strategy and all subclasses have `const` access to `sock`.
 * Since it does not get its own thread, it needs to be informed of various events on `sock` (ACKs,
 * timeouts) so that it can potentially recompute its CWND.  Thus the interface consists of,
 * basically: congestion_window_bytes() (obtain CWND for comparison to In-flight bytes in
 * `can_send()`); and `on_...()` methods to effect change in the internally stored CWND.
 *
 * ### Object life cycle ###
 * There is a strict 1-to-1 relationship between one Congestion_control_strategy
 * instance and one Peer_socket.  A Congestion_control_strategy is created shortly after Peer_socket
 * is and is saved inside the latter.  Conversely a pointer to the Peer_socket is stored inside the
 * Congestion_control_strategy.  Many congestion control algorithms need (read-only) access to the
 * innards of a socket; for example they may frequently access Peer_socket::m_snd_smoothed_round_trip_time
 * (SRTT) for various calculations.  The containing Peer_socket must exist at all times while
 * Congestion_control_strategy exists.  Informally it's recommended that Peer_socket destructor or
 * other method deletes its Congestion_control_strategy instance when it is no longer needed.
 *
 * ### Functionality provided by this class ###
 * The main functionality is the aforementioned interface.
 *
 * Secondarily, this main functionality is implemented as do-nothing methods (as opposed to pure
 * methods).  Finally, the class stores a pointer to the containing Peer_socket, as a convenience
 * for subclasses.
 *
 * ### General design note ###
 * TCP RFCs like RFC 5681 tend to present a monolithic congestion
 * control algorithm.  For example, Fast Retransmit/Fast Recovery is described as one combined
 * algorithm in RFC 5681-3.2.  It doesn't describe loss detection (3 dupe-ACKs), retransmission
 * (send first segment found to be lost), congestion window adjustment and loss recovery
 * (CWND/SSTHRESH halving, CWND inflation/deflation) as belonging to separate modules working
 * together but rather as one monolithic algorithm.  The design we use here is an attempt to
 * separate those things into distinct modules (classes or at least methods) that work together but
 * are abstractly separated within reason.  This should result in cleaner, clearer, more
 * maintainable code.  Namely, retransmission and loss detection are (separate) in Node, while CWND
 * manipulations are in Congestion_control_strategy.  Congestion_control_strategy's focus is, as
 * narrowly as possible, to compute CWND based on inputs from Node.  This may run counter to the
 * how a given RFC is written, since the part of "Reno" involving counting dupe-ACKs is not at all
 * part of the Reno congestion control module (Congestion_control_classic) but directly inside Node
 * instead.
 *
 * ### Assumptions about outside worl###
 * To support an efficient and clean design, it's important to
 * cleanly delineate the level of abstraction involved in the Congestion_control_strategy class
 * hierarchy.  How stand-alone is the hierarchy?  What assumptions does it make about its underlying
 * socket's state?  The answer is as follows.  The hierarchy only cares about
 *
 *   1.  events (`on_...()` methods like on_acks()) that occur on the socket (it must know about EVERY
 *       occurrence of each event as soon as it occurs);
 *   2.  certain state of the socket (such as Peer_socket::m_snd_flying_bytes).
 *
 * The documentation for each `on_...()` method must therefore exactly specify what the event means.
 * Similarly, either the doc header for the appropriate method or the class doc header must
 * specify what, if any, Peer_socket state must be set and how.  That said, even with clean
 * documentation, the key point is that the Congestion_control_strategy hierarchy must work TOGETHER
 * with the Node and Peer_socket; there is no way to make the programmer of a congestion control
 * module be able to ignore the relevant details (like how ACKs are generated and
 * handled) of Node and Peer_socket.  The reverse is not true; congestion control MUST be a black
 * box to the Node code; it can provide inputs (as events and arguments to the `on_...()` calls; and
 * as read-only state in Peer_socket) to Congestion_control_strategy objects, but it must have no
 * access (read or write) to the latter's internals.  This philosophy is loosely followed in the
 * Linux kernel code, though in my opinion Linux kernel code (in this case) is not quite as
 * straightforward or clean as it could be (which I humbly tried to improve upon here).  Bottom line:
 *
 *   1.  Congestion_control_strategy and subclasses are a black box to Node/Peer_socket code (no
 *       access to internals; access only to constructors/destructor and API).
 *   2.  Congestion_control_strategy and subclasses have `const` (!) `friend` access to Peer_socket
 *       internals.
 *   3.  The programmer of any Congestion_control_strategy subclass must assume a certain event
 *       model to be followed by Node.  This model is to be explicitly explained in the doc headers
 *       for the various `on_...()` methods.  Node must call the `on_...()` methods as soon as it
 *       detects the appropriate events, and it should aim to detect them as soon as possible after
 *       they occur.
 *   4.  The programmer of any Congestion_control_strategy subclass may assume the existence
 *       and meaning of certain state of Peer_socket, which she can use to make internal
 *       computations.  Any such state (i.e., in addition to the on-event calls, and their
 *       arguments, in (3)) must be explicitly documented in the class or method doc headers.
 *
 * ### Choice of congestion window units ###
 * We choose bytes, instead of multiples of max-block-size (in
 * TCP, this would be maximum segment size [MSS]).  Either would have been feasible.  TCP RFCs use
 * bytes (even if most of the math involves incrementing/decrementing in multiples of MSS); so does
 * at least the canonical BSD TCP implementation (Stevens/Wright, TCP/IP Illustrated Vol. 2) from
 * the early 90s (not sure about modern version).  Linux TCP uses multiples of MSS, as do many
 * papers on alternative congestion control methods.  Reasoning for our choice of bytes: First, the
 * particular congestion control implementation can still do all internal math in multiples of
 * max-block-size (if desired) and then just multiply by that in congestion_window_bytes() at the
 * last moment.  Second, I've found that certain algorithms, namely Appropriate Byte Counting, are
 * difficult to perform in terms of multiples while staying true to the algorithm as written (one
 * either has to lose some precision or maintain fractional CWND parts which cancels out some of the
 * simplicity in using the multiples-of-MSS accounting method).  Thus it seemed natural to go with
 * the more flexible approach.  The only cost of that is that Node, when using
 * congestion_window_bytes(), must be ready for it to return a non-multiple-of-max-block-size value
 * and act properly.  This is not at all a major challenge in practice.
 *
 * ### Terminology ###
 * In doc comments throughout this class hierarchy, the terms "simultaneously,"
 * "immediately," and "as soon as" are to be interpreted as follows: Within a non-blocking amount of
 * time.  Note that that is not the same as literally "as soon as possible," because for efficiency
 * the Node implementation may choose to perform other non-blocking actions first.  For example,
 * on_acks() is to be called "as soon as" a packet acknowledgment is received, but the Node can and
 * should first accumulate all other acks that have already arrived, and only then call on_acks()
 * for all of them.  Thus, in practice, in the `net_flow` implementation, "immediately/as soon
 * as/simultaneously" is the same as "within the same boost.asio handler invocation," because each
 * handler is written to complete without blocking (sleeping).
 *
 * ### Thread safety ###
 * Unless stated otherwise, a Congestion_control_strategy object is to be accessed
 * from the containing Peer_socket's Node's thread W only.
 *
 * How to add a new Congestion_control_strategy subclass?  First write the code to above spec (using
 * existing strategies as a basis -- especially Congestion_control_classic, upon which most others
 * are usually based) and put it in the self-explanatory location.  Next, add it to the socket
 * option machinery, so that it can be programmatically selected for a given Node or Peer_socket,
 * or selected by the user via a config file or command line, if the application-layer programmer so
 * chooses.  To do so, add it to the following locations (by analogy with existing ones):
 *
 *   - `enum Peer_socket_options::Congestion_control_strategy_choice::Congestion_control_strategy_choice.`
 *   - Congestion_control_selector::S_ID_TO_STRATEGY_MAP `static` initializer.
 *   - Congestion_control_selector::S_STRATEGY_TO_ID_MAP `static` initializer.
 *   - Factory method Congestion_control_selector::create_strategy().
 *
 * Voila!  You can now use the new congestion control algorithm.
 *
 * @todo Tuck away all congestion control-related symbols into new `namespace cong_ctl`?
 */
class Congestion_control_strategy :
  public util::Null_interface,
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Methods.

  /**
   * Returns the maximal number of bytes (with respect to `m_data` field of DATA packets) that this
   * socket should allow to be In-flight at this time.  Bytes, if available, can be sent if and only
   * if this value is greater than the # of In-flight bytes at this time.
   *
   * This is pure.  Each specific congestion control strategy must implement this.
   *
   * @note For definition of In-flight bytes, see Peer_socket::m_snd_flying_pkts_by_sent_when.
   * @return See above.
   */
  virtual size_t congestion_window_bytes() const = 0;

  /**
   * Informs the congestion control strategy that 1 or more previously sent packets whose status was
   * In-flight just received acknowledgments, thus changing their state from In-flight to
   * Acknowledged.  For efficiency and simplicity of behavior, on_acks() should be called as few
   * times as possible while still satisfying the requirement in the previous sentence.  That is,
   * suppose acknowledgments for N packets were received simultaneously.  Then on_acks() must be
   * called one time, with the "packets" argument equal to N -- not, say, N times with `packets == 1`.
   *
   * The acknowledgments that led to this on_acks() call also results in 1 or more individual
   * on_individual_ack() calls covering each individual packet.  You MUST call
   * on_individual_ack() and THEN call on_acks().
   *
   * If the acknowledgment group that led to on_acks() also exposed the loss of some packets, i.e.,
   * if the criteria for on_loss_event() also hold, then you MUST call on_loss_event() and THEN call
   * on_acks().  (Informal reasoning: the ACKs are exposing drop(s) that occurred in the past,
   * chronologically before the ACKed packets arrived.  Thus the events should fire in that order.)
   *
   * You MUST call on_acks() AFTER Peer_socket state (Peer_socket::m_snd_flying_pkts_by_sent_when et al) has been
   * updated to reflect the acknowledgments being reported here.
   *
   * Assumptions about ACK sender (DATA receiver): It is assumed that:
   *
   *   - Every DATA packet is acknowledged at most T after it was received, where T is some
   *     reasonably small constant time period (see Node::S_DELAYED_ACK_TIMER_PERIOD).  (I.e., an
   *     ACK may be delayed w/r/t DATA reception but only up to a certain delay T.)
   *   - All received but not-yet-acked DATA packets are acknowledged as soon as there are at least
   *     Node::S_MAX_FULL_PACKETS_BEFORE_ACK_SEND * max-block-size bytes in the received but
   *     not-yet-acked DATA packets.  (I.e., every 2nd DATA packet forces an immediate ACK to be
   *     sent.)
   *   - If, after the DATA receiver has processed all DATA packets that were received
   *     simultaneously, at least one of those DATA packets has a higher sequence number than a
   *     datum the receiver has not yet received, then all received but not-yet-acked DATA packets
   *     are acknowledged.  (I.e., every out-of-order DATA packet forces an immediate ACK to be
   *     sent.)
   *
   * @note congestion_window_bytes() may return a higher value after this call.  You should check
   *       `can_send()`.
   * @note Acknowledgments of data that are not currently In-flight due to being Dropped (a/k/a
   *       late ACKs) or Acknowledged (i.e., duplicate ACKs) must NOT be passed to this method.
   * @note If an acknowledgment for packet P transmission N is received, while packet P transmission
   *       M != N is the one currently In-flight (i.e., packet was retransmitted, but the earlier
   *       incarnation was late-acked), such acknowledgments must NOT be passed to this method.
   *       We may reconsider this in the future.
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
   * @param packets
   *        The number of packets thus Acknowledged.
   */
  virtual void on_acks(size_t bytes, size_t packets);

  /**
   * Informs the congestion control strategy that 1 or more previously sent packets whose status was
   * In-flight have just been inferred to be Dropped by receiving acknowledgments of packets that
   * were sent later than the now-Dropped packets.  For efficiency and simplicity of behavior,
   * on_loss_event() should be called as few times as possible while still satisfying the requirement in
   * the previous sentence.  That is, suppose acknowledgments for N packets were received
   * simultaneously thus exposing M packets as dropped.  Then on_loss_event() must be called one
   * time, with the "packets" argument equal to M and not, say, M times with packets == 1.
   *
   * An important addendum to the above rule is as follows.  You MUST NOT call on_loss_event(), if
   * the Dropped packets which would have led to this call are part of the same loss event as those
   * in the preceding on_loss_event() call.  How is "part of the same loss event" defined?  This is
   * formally defined within the large comment header at the top of
   * Node::handle_accumulated_acks().  The informal short version: If the new Dropped
   * packets were sent roughly within an RTT of those in the previous on_loss_event(), then do not
   * call on_loss_event().  The informal reasoning for this is to avoid sharply reducing
   * congestion_window_bytes() value due to 2+ groups of acks that arrive close to each other but
   * really indicate just 1 loss event nevertheless repeatedly reducing CWND.
   *
   * on_loss_event() must be called BEFORE on_individual_ack() and on_acks() are called for the
   * ack group that exposed the lost packets.  See on_acks() and on_individual_ack().
   *
   * You MUST call on_loss_event() AFTER Peer_socket state (`m_snd_flying_pkts_by_sent_when` et al) has been
   * updated to reflect the drops being reported here.
   *
   * @note congestion_window_bytes() WILL NOT return a higher value after this call.  You need not
   *       call `can_send()`.
   * @note For definition of In-flight, Acknowledged, and Dropped bytes, see
   *       Peer_socket::m_snd_flying_pkts_by_sent_when and Peer_socket::m_snd_flying_pkts_by_sent_when.
   * @note This is analogous to the 3-dupe-ACKs part of the Fast Retransmit/Recovery algorithm in
   *       classic TCP congestion control (e.g., RFC 5681).
   *
   * @param bytes
   *        The sum of the number of bytes in the user data fields of the packets that have been
   *        Dropped.  Must not be zero.
   * @param packets
   *        The number of packets thus Dropped.
   */
  virtual void on_loss_event(size_t bytes, size_t packets);

  /**
   * Informs the congestion control strategy that exactly 1 previously sent packet whose status was
   * In-flight is now known to have the given round trip time (RTT), via acknowledgment.  In other
   * words, this informs congestion control of each valid individual-packet acknowledgment of a
   * packet that was In-flight at time of acknowledgment.
   *
   * The acknowledgment that led to the given individual RTT measurement also results in a
   * consolidated on_acks() call that covers that packet and all other packets acked simultaneously;
   * you MUST call this on_individual_ack() and THEN call on_acks().  on_individual_ack()
   * should be called in the order of receipt of the containing ACK that led to the RTT measurement;
   * if two RTTs are generated from one ACK, the tie should be broken in the order of appearance
   * within the ACK.
   *
   * If the acknowledgment group that led to on_individual_ack() also exposed the loss of some packets,
   * i.e., if the criteria for on_loss_event() also hold, then you MUST call on_loss_event() and
   * THEN call on_individual_ack().  (Informal reasoning: the ACKs are exposing drop(s) that occurred in the
   * past, chronologically before the ACKed packets arrived.  Thus the events should fire in that
   * order.)
   *
   * You MUST call on_individual_ack() AFTER Peer_socket state (Peer_socket::m_snd_flying_pkts_by_sent_when
   * et al) has been updated to reflect the acknowledgments being reported here.
   *
   * Assumptions about RTT value: The RTT value is assumed to include only the time spent in transit
   * from sender to receiver plus the time the ACK spent in transit from receiver to sender.  Any
   * delay (such as ACK delay) adding to the total time from sending DATA to receiving ACK is *not*
   * to be included in the RTT.  RTT is meant to measure network conditions/capacity.
   *
   * @note congestion_window_bytes() may return a higher value after this call, but you should wait
   *       to query it until after calling on_acks() for the entire round of acknowledgments being
   *       handled.
   * @note Acknowledgments of data that is not currently In-flight due to being Dropped (a/k/a late
   *       ACKs) or Acknowledged (i.e., duplicate ACKs) must NOT be passed to this method.
   * @note If an acknowledgment for packet P transmission N is received, while packet P transmission
   *       M != N is the one currently In-flight (i.e., packet was retransmitted, but the earlier
   *       incarnation was late-acked), such acknowledgments must NOT be passed to this method.
   *       We may reconsider this in the future.
   * @note For definition of In-flight, Acknowledged, and Dropped bytes, see
   *       Peer_socket::m_snd_flying_pkts_by_sent_when and Peer_socket::m_snd_flying_pkts_by_sent_when.
   *
   * @param packet_rtt
   *        Round trip time of an individual packet.
   * @param bytes
   *        The number of bytes of user data corresponding to this RTT sample (i.e., # of bytes
   *        acknowledged in the acknowledged packet).
   * @param sent_cwnd_bytes
   *        congestion_window_bytes() when acked DATA packet was sent.
   */
  virtual void on_individual_ack(const Fine_duration& packet_rtt, const size_t bytes, const size_t sent_cwnd_bytes);

  /**
   * Informs the congestion control strategy that 1 or more previously sent packets whose status was
   * In-flight have just been inferred to be Dropped because of the Drop Timer expiring.  A formal
   * description of what "Drop Timer expiring" means is too large to put here, and there are many
   * different ways to do it.  See class Drop_timer and the code that uses it.  Formally, we expect
   * that one Drop Timer is running if and only if there is at least one packet In-flight, and that
   * that Drop Timer expiring implies the immediate conversion of at least one packet from In-flight
   * to Dropped.  We also expect that, informally, the Drop Timeout indicates serious loss events
   * and is the 2nd and last resort in detecting loss, the main (and more likely to trigger first)
   * one being on_loss_event().
   *
   * You MUST call on_drop_timeout() AFTER Peer_socket state (Peer_socket::m_snd_flying_pkts_by_sent_when
   * et al) has been updated to reflect the drops being reported here.
   *
   * @note congestion_window_bytes() WILL NOT return a higher value after this call.  You need not
   *       call `can_send()`.
   * @note For definition of In-flight, Acknowledged, and Dropped bytes, see
   *       Peer_socket::m_snd_flying_pkts_by_sent_when and Peer_socket::m_snd_flying_pkts_by_seq_num.
   * @note This is analogous to the Retransmit Timeout (RTO) algorithm in classic TCP congestion
   *       control (e.g., RFCs 5681 and 6298).
   *
   * @param bytes
   *        The sum of the number of bytes in the user data fields of the packets that have been
   *        Dropped with this Drop Timeout.  Must not be zero.
   * @param packets
   *        The number of packets thus Dropped.
   */
  virtual void on_drop_timeout(size_t bytes, size_t packets);

  /**
   * Informs the congestion control strategy that Node considers the connection to be "idle" by
   * virtue of no desired send activity on the part of the user application for some period of time.
   * Informally, this means "the user hasn't wanted to send anything for a while, so you may want to
   * update your CWND calculations based on that fact, as it's likely you have obsolete information
   * about the connection."  For example, if a connection has been idle for 5 minutes, then there
   * have been no ACKs for a while, and since ACKs typically are the tool used to gather congestion
   * data and thus compute CWND, the internal CWND may be reset to its default initial value within
   * on_idle_timeout().
   *
   * The formal definition of Idle Timeout is the one used in Node::send_worker().  Short version:
   * an Idle Timeout occurs T after the last packet to be sent out, where T is the Drop Timeout (see
   * on_drop_timeout()).
   *
   * @note congestion_window_bytes() WILL NOT return a higher value after this call.  You need not
   *       call `can_send()`.
   * @note This is analogous to the "Restarting Idle Connections" algorithm in classic TCP
   *       congestion control (RFC 5681-4.1).
   */
  virtual void on_idle_timeout();

protected:

  /**
   * Constructs object by setting up logging and saving a pointer to the containing Peer_socket.
   * Only a weak pointer of `sock` is stored: the `shared_ptr` itself is not saved, so the reference
   * count of `sock` does not increase.  This avoids a circular `shared_ptr` situation.
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param sock
   *        The Peer_socket for which this module will control congestion policy.
   */
  explicit Congestion_control_strategy(log::Logger* logger_ptr, Peer_socket::Const_ptr sock);

  /**
   * Utility for subclasses that returns a handle to the containing Peer_socket.  If somehow the
   * containing Peer_socket has been deleted, `assert()` trips.
   *
   * @return Ditto.
   */
  Peer_socket::Const_ptr socket() const;

private:

  // Data.

  /**
   * The containing socket (read-only access).  Implementation may rely on various state stored
   * inside the pointed-to Peer_socket.
   *
   * Why `weak_ptr`?  If we stored a `shared_ptr` (Peer_socket::Const_ptr) then something would have to
   * delete this Congestion_control_strategy object before the pointee Peer_socket's ref-count could
   * drop to zero and it too could be deleted; this is undesirable since the guy that would want to
   * delete this Congestion_control_strategy is Peer_socket's destructor itself (standard circular
   * `shared_ptr` problem).  If we stored a raw const `Peer_socket*` pointer instead, that would be
   * fine.  However, using a `weak_ptr` allows us to `assert()` in a civilized way if the underlying
   * Peer_socket had been deleted (instead of crashing due to accessing deleted memory as we would
   * with a raw pointer).  This isn't really a big deal, since hopefully our code will be written
   * properly to avoid this, but this is just a little cleaner.
   */
  boost::weak_ptr<Peer_socket::Const_ptr::element_type> m_sock;
}; // class Congestion_control_strategy

/**
 * Namespace-like class that enables an `enum`-based selection of the Congestion_control_strategy
 * interface implementation to use for a given socket (for programmatic socket options) and
 * facilitates stream I/O of these enums (allowing parsing and outputting these socket options).
 *
 * ### Provided facilities ###
 *   - Create Congestion_control_strategy subclass instance based on `enum` value;
 *   - Return set of possible text IDs, each representing a distinct `enum` value;
 *   - Read such an ID from an `istream` into an `enum` value; write an ID from an `enum` value to an `ostream`.
 */
class Congestion_control_selector :
  private boost::noncopyable
{
public:
  // Types.

  /// Short-hand for Peer_socket_options::Congestion_control_strategy.
  using Strategy_choice = Peer_socket_options::Congestion_control_strategy_choice;

  // Methods.

  /**
   * Factory method that, given an `enum` identifying the desired strategy, allocates the appropriate
   * Congestion_control_strategy subclass instance on the heap and returns a pointer to it.
   *
   * @param strategy_choice
   *        The type of strategy (congestion control algorithm) to use.
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param sock
   *        The Peer_socket for which this module will control congestion policy.
   * @return Pointer to newly allocated instance.
   */
  static Congestion_control_strategy* create_strategy(Strategy_choice strategy_choice,
                                                      log::Logger* logger_ptr, Peer_socket::Const_ptr sock);

  /**
   * Returns a list of strings, called IDs, each of which textually represents a distinct
   * Congestion_control_strategy subclass.  You can output this in socket option help text as the
   * possible choices for congestion control strategy.  They will match the possible text inputs to
   * `operator>>(istream&, Strategy_choice&)`.
   *
   * @param ids
   *        The `vector` of strings to clear and fill with the above.
   */
  static void get_ids(std::vector<std::string>* ids);

private:
  // Friends.

  // Friend of Congestion_control_selector: For access to Congestion_control_selector::S_ID_TO_STRATEGY_MAP and so on.
  friend std::istream& operator>>(std::istream& is,
                                  Congestion_control_selector::Strategy_choice& strategy_choice);
  // Friend of Congestion_control_selector: For access to Congestion_control_selector::S_STRATEGY_TO_ID_MAP and so on.
  friend std::ostream& operator<<(std::ostream& os,
                                  const Congestion_control_selector::Strategy_choice& strategy_choice);

  // Data.

  /// Maps each ID to the corresponding #Strategy_choice `enum` value.
  static const std::map<std::string, Strategy_choice> S_ID_TO_STRATEGY_MAP;
  /// The inverse of #S_ID_TO_STRATEGY_MAP.
  static const std::map<Strategy_choice, std::string> S_STRATEGY_TO_ID_MAP;

  // Privacy stubs.

  /// Forbid all instantiation.
  Congestion_control_selector() = delete;
}; // class Congestion_control_selector

} // namespace flow::net_flow
