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
#include "flow/util/shared_ptr_alias_holder.hpp"
#include <boost/enable_shared_from_this.hpp>
#include <set>

namespace flow::net_flow
{
// Types.

/**
 * Internal `net_flow` class that maintains the Drop Timer for DATA packet(s) to have been sent out
 * over a connection but not yet acknowledged by the receiver.  This is similar to the
 * Retransmission Timer of TCP (although the way that timer works is not solidly standardized,
 * though there are recommended setups in RFCs like RFC 6298).  However, this applies even with
 * retransmission disabled (which is allowed in NetFlow but not in TCP), so it would be incorrect to
 * call it `Rexmit_timer`; on the other hand when retransmission is enabled, Drop_timer will still be
 * an accurate name (as a packet has to be considered Dropped before we retransmit it).  `net_flow`
 * users should not use this class directly.
 *
 * The basic idea is: if you send a packet, and it's not acknowledged within some amount of time,
 * then consider that packet Dropped.  (At that point you can take it out of the In-flight window,
 * or retransmit it, or whatever else is appropriate.)  Beyond this concept the details are
 * ambiguous, and there is no one right/standard/accepted answer for how it should work. For
 * example, when to restart the timer?  What to do with the timer after it fires?  Etc.  Therefore
 * the class is reasonably configurable, so that we can experiment with different approaches.
 *
 * This class works together with Node, Peer_socket, and Server_socket and accordingly assumes
 * very specific algorithms are used in sending and handling acknowledgments.  However it
 * could pretty easily be swapped out with another implementationr
 *
 * A Drop_timer works as follows.  It keeps an internal timer that fires every DTO seconds, where
 * DTO is computed externally (from SRTT, etc.).  Certain events start or restart the timer (e.g.,
 * receiving an acknowledgment).  Other events stop the timer (namely having no more In-flight
 * packets).  Then when the timer fires, certain actions have to be taken, such as considering
 * packets Dropped and/or restarting the timer.  Additionally the DTO value itself can be written to
 * by Drop_timer (e.g., timer backoff).
 *
 * Therefore, the interface of Drop_timer is simple: you call `on_...()`, when a Drop
 * Timer-influencing event has occurred; and when some action needs to be taken due to a Drop Timer
 * firing, it calls a user-supplied callback which will perform that action.  The action callbacks
 * are supplied at construction.  Drop_timer internally takes care of everything in-between:
 * running/stopping the timer, catching the timer firing, etc.  All of the "policy" about how the
 * timer operates is also inside.  Calling done() means the socket is done, and no action callbacks
 * will subsequently fire.
 *
 * To schedule the timer, Drop_timer shares Node's main event queue (Node::m_task_engine).
 *
 * ### Thread safety ###
 * To be used in the containing Node's thread W only.
 *
 * Implementation notes
 * --------------------
 *
 * Having written this class, it looks over-engineered.  The code would be shorter (no `shared_ptr`,
 * callbacks, factory, `enable_shared_from_this`) if it were directly in Node and Peer_socket.  On the
 * other hand the Strategy Pattern setup used here is nice for segregating the Drop Timer logic and
 * for future modification, and it does make Node (which is huge) much easier to understand.  So could
 * go either way... rewrite in Node and Peer_socket or leave it alone.
 *
 * We use a `Ptr`-emitting factory, because wrapping the object in a `Ptr` helps with some internal
 * book-keeping and is useful to the user as well.
 *
 * ### Regarding events ###
 * You'll note the interface is extremely simple: essentially, outside code just registers whenever
 * packet becomes In-flight (a/k/a is sent over wire); and the opposite.  This simplicity comes at
 * an implementation cost, because it requires us to keep -- *internally* -- an ordered (by send time) set
 * of packets currently In-flight.  Why do this?  Because the true events we are interested in are slightly
 * more complex than what the Drop_timer API exposes: we want to detect when no packets are In-flight and
 * suddenly a packet is sent off; the reverse; the acknowledgment of a packet that is NOT the oldest
 * one currently In-flight; and the changing of the oldest currently In-flight packet from packet A to
 * packet B =/= A.  (That's as of this writing; perhaps this set of events could change.)
 *
 * One might wonder whether it would be -- overall -- simpler to just have the event API of Drop_timer (the set of
 * on_...() methods) just be those exact events I just listed.  Then outside code could just call them when detecting
 * those events.  After all, Peer_socket has complex structures keeping track all sorts of details including how
 * many and which packets are In-flight, so Peer_socket and Node code could easily detect all those situations.
 *
 * In fact, that IS how Drop_timer was implemented in the past.  Here is why I changed it: There is a small
 * caveat about what "In-flight" means.  Peer_socket's `m_snd_flying_pkts_...` data structures define "In-flight
 * packet" as "packet that has been determined to be fit to send over the wire, having passed the availability
 * and CWND constraints."  For Drop_timer's ideal operation, "In-flight packet" is defined as "packet that has
 * been sent over the wire."  The practical difference between the two definitions is that the former may be
 * awaiting certain send pacing conditions to actually be sent off; whereas the latter means it has been sent
 * off -- period.  (See `struct Send_pacing_data` and related code for info on pacing.  Essentially, though,
 * any packet that's definitely going to be sent out barring existential disaster may be delayed a bit in a pacing
 * queue to avoid bunching up packets in the pipe.)  However, we lack a data structure that specifically
 * stores packets to have been sent off over the wire, except indirectly in Peer_socket::m_snd_flying_pkts_by_sent_when
 * minus Send_pacing_data::m_packet_q.  The former is unwieldy for our purposes, let along when combined with the
 * other.  So it made more sense to just let outside code report truly In-flight packets appearing and disappearing
 * to us, then we store the (rather minimal) information internally.  Thus we separate concerns and divide
 * labor among modules in an accurate fashion instead of Drop_timer becoming ultra-dependent on other modules'
 * implementation details.  That said, there is probably some memory cost and (maybe) a little processor cycle
 * cost to this approach.  (One could also say that the pacing delay is so minor as to make it ignorable
 * here.  However, I decided I don't need the headache of wondering/worrying about that ad infinitum; and
 * solved the problem instead.)
 *
 * ### Update regarding events ###
 * In order to increase performance by smooshing together lots of little checks and
 * actions that might occur when handling lots of little events, namely received acknowledgments, the class
 * user must now bracket groups of `on_...()` method calls with start_contemporaneous_events() and
 * end_contemporaneous_events().  Thus each `on_...()` call performs only very basic/quick book-keeping; while
 * end_contemporaneous_events() is what compares the situation against what it was at start_contemporaneous_events()
 * and possibly detects the true events of interest that may actually trigger something (start timer, stop timer,
 * restart running timer).
 *
 * ### `Timer` vs util::schedule_task_from_now() ###
 * In other places we have tended to replace a `Timer` with the far simpler util::schedule_task_from_now() (etc.)
 * facility (which internally uses a `Timer` but hides its various annoyances and caveats).  Why not here?
 * Answer: We don't happen to use any of `Timer`'s advanced features that the simpler facility lacks; we just
 * schedule and cancel, and the other facility supports those.  So that just leaves the potential performance
 * effects as the reason not to use the simpler facility (as listed in util::schedule_task_from_now() doc header).
 * The logic concerning the delay before each scheduled task, and how often it might be canceled and/or
 * rescheduled is not simple.  It's hard to make iron-clad guarantees, therefore, that using a single repeatedly-reused
 * `Timer` is no better perf-wise than using multiple schedulings using the simpler facility.  So we do the
 * perf-conservative thing and use `Timer` despite having to worry about a couple of the associated corner cases.
 *
 * Or to put it concisely: The timer logic here is complicated, so the perf and flexibility aspects of directly using
 * the advanced `Timer` feature are suitable.
 *
 * @todo Drop_timer has data members that are references to non-`const`.  These should be pointers for max consistency
 * of style with other code in the project (and that style is encouraged for certain good reasons).  Scour rest of
 * code for other such data members as well.
 */
class Drop_timer :
  // Endow us with shared_ptr<>s ::Ptr and ::Const_ptr (syntactic sugar).
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Drop_timer>>,
  // Allow access to Ptr{this} from inside Drop_timer methods.  Just call shared_from_this().
  public boost::enable_shared_from_this<Drop_timer>,
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /**
   * Type to uniquely identify a packet sent over the wire in the socket to which this Drop_timer applies.
   * Caution: keep in sync with Peer_socket::order_num_t. That way Peer_socket
   * code can easily form a #packet_id_t from an `order_num_t` which, too, uniquely identifies a packet.
   * 0 is reserved, not to be used for any actual packets.
   */
  using packet_id_t = Sequence_number::seq_num_t;

  // Constructors/destructor.

  /// Destructor.
  ~Drop_timer();

  // Methods.

  /**
   * Constructs Drop_timer and returns a ref-counted pointer wrapping it.  Saves the "action
   * callbacks" to call when various events fire in this Drop_timer.  The callbacks may be placed
   * onto `*node_task_engine` in the manner of `post(Task_engine&)`, at any future time until done()
   * is called.  At construction, the timer is guaranteed not to fire until the first `on_...()` call.
   *
   * After construction, call `on_...()` as events occur.  If an event described by the doc comment of
   * an `on_...()` method occurs, you MUST call it or results are undefined (the state machine breaks
   * down).
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param node_task_engine
   *        The containing Node's `&m_task_engine`.  Asyncronous callbacks will be placed on it
   *        (see below).
   * @param sock_drop_timeout
   *        Pointer to the containing Peer_socket's `m_snd_drop_timeout.`  Drop_timer may write to this
   *        member if it feels the need to modify Drop Timeout value (as in RFC 6298 section 5.5)!
   * @param sock
   *        Pointer to the containing Peer_socket.  Access is read-only.
   * @param timer_failure
   *        Function `F` with signature `void F(const Error_code& err_code)`.
   *        `F(<failure code>)` will be called if and only if there was an internal system timer
   *        failure.  Required action of `timer_failure()`: RST/close with the given error.
   * @param timer_fired
   *        Function `F` with signature `void F(bool drop_all_packets)`.  `F(drop_all_packets)` will be
   *        called if and only if the Drop Timer has fired.  Required action of `timer_fired()`: mark
   *        either all (if `drop_all_packets`) or just the earliest (otherwise) In-flight packet(s) as
   *        Dropped.
   * @return A ref-counted pointer wrapping a newly created Drop_timer as described above.
   */
  static Ptr create_drop_timer(log::Logger* logger_ptr,
                               util::Task_engine* node_task_engine,
                               Fine_duration* sock_drop_timeout,
                               Peer_socket::Const_ptr&& sock,
                               const Function<void (const Error_code& err_code)>& timer_failure,
                               const Function<void (bool drop_all_packets)>& timer_fired);

  /**
   * Indicates the start of a series of zero or more contemporary `on_*()` event calls, to be marked as finished
   * via end_contemporaneous_events(). Contemporary means to be treated as occurring in that order but essentially
   * simultaneously. (It is recommended but not enforced that they be registered in the same boost.asio task.)
   * This call should be followed by the 0+ `on_*()` calls; and finally end_contemporaneous_events(). Grouping events
   * in this way allows for better performance than, for example, a series of one-event groups.
   */
  void start_contemporaneous_events();

  /**
   * Indicates that a packet identified by the given unique ID has just been
   * sent over the wire (the low-level transport, currently UDP). Subtlety: Peer_socket may consider a packet
   * In-flight before it has actually been sent out over the wire, namely if it is queued in the pacing
   * module. This should be called only once it is actually sent. Note, however, that IF `on_packet_in_flight(P)`
   * is called, then indeed the packet identified by `P` _must_ be considered In-flight in the Peer_socket
   * sense. See Peer_socket::m_snd_flying_pkts_by_sent_when for definition of In-flight.
   *
   * A contemporary event group must be in progress via start_contemporaneous_events(); or behavior is undefined.
   *
   * @param packet_id
   *        ID of the packet that was sent out. This can be any number as long as it increases with each
   *        subsequent call to this method for `*this Drop_timer` (in particular, it'll therefore also be unique
   *        per Drop_timer). Behavior is undefined if this rule is not followed.
   */
  void on_packet_in_flight(packet_id_t packet_id);

  /**
   * Indicates that a packet for which on_packet_in_flight() was called is now no longer considered In-flight
   * in the Peer_socket sense; see Peer_socket::m_snd_flying_pkts_by_sent_when for definition of In-flight.
   * Since `on_packet_in_flight(P)` must be called only if `P` was then In-flight, `on_packet_no_longer_in_flight(P)`
   * can be called at most once and only after the former call.
   *
   * A contemporary event group must be in progress via start_contemporaneous_events(); or behavior is undefined.
   *
   * @param packet_id
   *        ID of a packet for which on_packet_in_flight() has been called exactly once for `*this`.
   *        Otherwise behavior is undefined.
   */
  void on_packet_no_longer_in_flight(packet_id_t packet_id);

  /**
   * Equivalent to `on_packet_no_longer_in_flight(P)`, for all `P` currently In-flight as registered by
   * `on_packet_in_flight(P)`.  When you know it would be correct, call it instead of the former method many times
   * for better efficiency.  (Do not do "both."  Trying to register more than once that `P` is no longer In-flight leads
   * to undefined behavior.)
   *
   * Calling this when no packets are In-flight already leads to undefined behavior.
   *
   * A contemporary event group must be in progress via start_contemporaneous_events(); or behavior is undefined.
   */
  void on_no_packets_in_flight_any_longer();

  /**
   * Indicates that a packet for which on_packet_in_flight() was called has just been validly acked.
   * To be validly acked means that an acknowledgment was received while the packet was considered In-flight in
   * the Peer_socket sense. See Peer_socket::m_snd_flying_pkts_by_sent_when for definition of In-flight.
   *
   * Such a packet also requires an on_packet_no_longer_in_flight() call. You must call on_ack() first; and the latter
   * subsequently. Otherwise behavior is undefined.
   *
   * A contemporary event group must be in progress via start_contemporaneous_events(); or behavior is undefined.
   *
   * @param packet_id
   *        ID of a packet for which on_packet_in_flight() has been called exactly once for *this.
   *        Otherwise behavior is undefined.
   */
  void on_ack(packet_id_t packet_id);

  /**
   * Finishes the group started by start start_contemporaneous_events(). This may trigger various on-event behavior
   * such as starting, stopping, or restarting the Drop_timer.
   */
  void end_contemporaneous_events();

  /**
   * Causes the Drop_timer to guarantee none of the action callbacks provided at construction will
   * be called from this point on.  Call this once you have no need of the Drop_timer (probably upon
   * exiting Peer_socket::Int_state::S_ESTABLISHED).  Subsequent calls to done() are harmless.
   *
   * Behavior is undefined if calling any other methods after done().
   */
  void done();

private:

  // Types.

  /// The counter type used to distinguish a given start_timer() call from any other such call (for this object).
  using timer_wait_id_t = uint64_t;

  // Constructors.

  /**
   * Constructs Drop_timer as described in the factory constructor create_drop_timer().
   * Why have the factory method?  We guarantee that this won't get deleted before the timer
   * callback executes (causing a crash therein), by passing a `Ptr{this}` to
   * `basic_waitable_timer::async_wait()`.  However that can only work if all users of the object also
   * access it by a sharing `Ptr`.  Thus we only provide access to the outside via a `Ptr` (the
   * factory).
   *
   * @param logger
   *        See create_drop_timer().
   * @param node_task_engine
   *        See create_drop_timer().
   * @param sock_drop_timeout
   *        See create_drop_timer().
   * @param sock
   *        See create_drop_timer().
   * @param timer_failure
   *        See create_drop_timer().
   * @param timer_fired
   *        See create_drop_timer().
   */
  explicit Drop_timer(log::Logger* logger,
                      util::Task_engine* node_task_engine,
                      Fine_duration* sock_drop_timeout,
                      Peer_socket::Const_ptr&& sock,
                      const Function<void (const Error_code& err_code)>& timer_failure,
                      const Function<void (bool drop_all_packets)>& timer_fired);

  // Methods.

  /**
   * Starts a new wait on the timer, so that is it asynchronously triggered according to the current
   * DTO value.  Pre-condition: `!m_timer_running`.
   */
  void start_timer();

  /// Invalidates the running asynchronous wait on #m_timer.  Pre-condition: #m_timer_running.
  void disable_timer();

  /**
   * Called by boost.asio when the Drop Timer fires; disables timer and calls an outside action
   * callback (given in constructor).  If #m_done or `m_current_wait_id != wait_id` or `err_code` is
   * `operation_aborted` (meaning this invokation by boost.asio is obsolete and replaced by a
   * subsequent async_wait(), or canceled), does nothing.
   *
   * @param wait_id
   *        The value of #m_current_wait_id at the time that `m_timer.async_wait()` was called with
   *        this method invokation as an argument.
   * @param prevent_destruction
   *        `shared_from_this()` at the time of `m_timer.async_wait()`.  It assures `this` cannot be
   *        deleted until this handle_timer_firing() is called and returns.
   * @param sys_err_code
   *        boost.asio error code indicating the circumstances of the callback executing.  If this
   *        is `boost::error::operation_aborted`, method does nothing.
   */
  void handle_timer_firing(Ptr prevent_destruction,
                           timer_wait_id_t wait_id,
                           const Error_code& sys_err_code);

  /**
   * Called by boost.asio after we post it, in the event of some timer error.  If #m_done, does
   * nothing (meaning the object was turned off before this had a chance to be executed).  Otherwise
   * executes the `m_timer_failure()` callback.
   *
   * @param prevent_destruction
   *        `shared_from_this()` at the time of `m_timer.async_wait()`.  It assures `this` cannot be
   *        deleted until `this->handle_timer_firing()` is called and returns.
   * @param err_code
   *        The portable error to pass to the `m_timer_failure()`.
   */
  void timer_failure(Ptr prevent_destruction, const Error_code& err_code);

  // Data.

  /// Node::m_task_engine of the containing Node.  Used to schedule timer events.
  util::Task_engine& m_node_task_engine;

  /**
   * Reference to the containing Peer_socket's Peer_socket::m_snd_drop_timeout data member (= DTO, Drop Timeout,
   * RTO).  Why is this not `const`?  Because under certain policies this object is allowed to
   * CHANGE the value of the DTO.  E.g., RFC 6298 says that RTO MUST be doubled each time the
   * Retransmit Timer fires.  (However a subsequent RTT measurement and SRTT recalculation will
   * change it back to the normally computed value.)
   */
  Fine_duration& m_sock_drop_timeout;

  /**
   * The containing Peer_socket (note that this is read-only access).  This may be used
   * to, at least, get the time point at which a packet was originally sent, to schedule the timer,
   * as well to get socket options related to Drop Timer.
   */
  const Peer_socket::Const_ptr m_sock;

  /**
   * The Drop Timer itself.  This class maintains it (including based on input events reported by
   * the user) and informs the user when it expires and what to do about it if so.
   */
  util::Timer m_timer;

  /**
   * Is there an active (non-obsolete, not-canceled) asynchronous wait in progress on #m_timer?  More
   * precisely, this is `true` if the latest start_timer() call was later than the latest
   * disable_timer() or handle_timer_firing(), or if start_timer() has been called but
   * disable_timer() or handle_timer_firing() has not.
   *
   * #m_timer_running should not be checked unless `!m_done`.
   */
  bool m_timer_running;

  /**
   * Unique (within this object) identifier of the last start_timer() call.  Each time start_timer()
   * calls `boost::asio::basic_waitable_timer::async_wait()`, it passes `++m_current_wait_id` to the timer
   * firing callback.  Then when that callback runs, it can check whether this passed `wait_id` is
   * equal to #m_current_wait_id; if not then it knows the callback is called for an obsolete wait
   * and should do nothing.  This is to help deal with the fact that a `basic_waitable_timer` callback can
   * fire after it is `cancel()`ed if the callback was already queued by the time `cancel()` is called
   * (see `boost::asio::basic_waitable_timer` documentation).
   *
   * For example, if I schedule wait 1, and it is queued to fire 5 seconds later, but at that point
   * something causes me to cancel and schedule wait 2 for another 5 seconds later, then wait 1's
   * callback, though soon called, will detect that `wait_id < m_current_wait_id` and do nothing, as
   * desired.  Another 5 seconds later, wait 2's callback will see that `wait_id == m_current_wait_id`
   * and do stuff, as desired.
   *
   * This must only change via increment and start at zero.  Wrap-around is impossible due to the
   * range of #timer_wait_id_t and the time periods practically involved.
   *
   * #m_current_wait_id should not be checked unless #m_timer_running.
   */
  timer_wait_id_t m_current_wait_id;

  /// `true` if and only if done() has been called.  Starts at `false`, can only change to `true`.
  bool m_done;

  /// Called on error.  See Drop_timer constructor.
  Function<void (const Error_code& err_code)> m_timer_failure;

  /// Called on Drop Timeout.  See Drop_timer constructor.
  Function<void (bool drop_all_packets)> m_timer_fired;

  /**
   * Packet IDs of packets to have been sent over wire and still considered In-flight, ordered from
   * earliest to latest to have been sent out.  These are packet IDs for which on_packet_in_flight() but not
   * yet on_packet_no_longer_in_flight() has been called.
   *
   * Since the iterator ordering is increasing numerical order (ordered set of `packet_id`s), and `packet_id`s passed to
   * on_packet_in_flight() must be increasing from call to call, the aforementioned ordering (by time sent) is
   * expressed via the iterator ordering.  Thus the earliest-sent packet is easily accessible via `begin()`; this is
   * needed for certain drop-timer situations.
   *
   * Logarithmic-time erasure for on_packet_no_longer_in_flight() is also available.
   */
  std::set<packet_id_t> m_flying_packets;

  /**
   * The packet ID of the least recently sent In-flight packet at last
   * start_contemporaneous_events() call; or 0 if `m_flying_packets.empty()` at the time.
   */
  packet_id_t m_at_events_start_oldest_flying_packet;

  /**
   * During the time period starting with the last start_contemporaneous_events() call and ending with the subsequent
   * end_contemporaneous_events() call, if any -- in other words, during the last
   * contemporary events group, finished or otherwise -- this is the ID of the most-recently-sent packet (highest
   * packet ID) such that that packet was acknowledged during that time period.  0 if none were acknowledged.
   */
  packet_id_t m_during_events_newest_acked_packet;

  /**
   * `true` if and only if the last start_contemporaneous_events() call exists, and either end_contemporaneous_events()
   * hasn't been called at all or was called before the former call.
   */
  bool m_in_events_group;
}; // class Drop_timer

} // namespace flow::net_flow
