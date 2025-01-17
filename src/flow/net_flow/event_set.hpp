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

#include "flow/net_flow/net_flow_fwd.hpp"
#include "flow/net_flow/error/error.hpp"
#include "flow/util/traits.hpp"
#include "flow/util/shared_ptr_alias_holder.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/any.hpp>

namespace flow::net_flow
{
// Types.

/**
 * A user-set collection of sockets and desired conditions on those sockets (such as: "socket has data
 * to read"), with the ability to wait for those conditions to become true and signal the user when so.
 *
 * @note Important: With the somewhat more recent availability of the asio::Node hierarchy, which slickly adds
 *       boost.asio-event-loop-integration to the Node hierarchy, public use of this class Event_set should be avoided
 *       for those already using a boost.asio event loop.  It is both significantly simpler -- and a bit faster --
 *       to use Peer_socket::async_send(), Server_socket::async_accept(), etc. -- as well as a bit faster.
 *       IN PARTICULAR, the complex discussion in Event_set doc header regarding async_wait() with a glue socket
 *       is something one should skip over if one could just integrate directly into a boost.asio loop.
 * @note Advice: Even if you don't already use a boost.asio event loop, consider doing so before giving up and going for
 *       an Event_set glue pattern as described in this class doc header.  Your application may be sigificantly simpler
 *       and more maintainable as a result.
 * @note Advice: In general, please read the bird's eye view of the entire set of I/O ops in `net_flow`; this is found
 *       in asio::Node doc header.  This should help hugely in choosing the right type of operating mode in the context
 *       of a non-trivial application, possibly featuring other types of I/O in addition to the Flow protocol.
 * @note All that said, Event_set is a very important facility and is, at the very least, built-upon internally for all
 *       but the basic non-blocking ops.  Advanced users should understand it (even if they don't use it directly).
 *
 * This fulfills the same role as BSD sockets' `select()` (and its more powerful non-portable
 * equivalents like epoll, kqueue, etc.).  In addition to the feature set provided by functions such
 * as `select()`, Event_set provides a way to work together with the OS's built-in `select()`
 * equivalent; that is to say it provides a way to add Flow-protocol sockets into an event loop written in
 * terms of `select()` or spritiually similar facilities (epoll, kqueue, etc.).
 *
 * The simplest way to use Event_set is to not use it directly at all.  Instead, use the blocking
 * methods such as Peer_socket::sync_receive(), Peer_socket::sync_send(), Node::sync_connect(), and
 * Server_socket::sync_accept().  Such methods will internally construct an Event_set with the
 * target socket and perform work (namely some Event_set::sync_wait() calls) on it, hidden from the caller.
 * This mode of operation is analogous to BSD sockets' blocking functions (e.g., `recv()` in blocking
 * mode -- POSIX `O_NONBLOCK` bit set / WinSock `FIONBIO` mode is enabled).
 *
 * This is often insufficient, as in event loops of any complexity.  The next simplest way to use
 * Event_set is to use a synchronous wait.  To do so, construct an Event_set, add the desired
 * <em>(socket, desired event on that socket)</em> pairs to it (see the swap_wanted_sockets(), add_wanted_socket(),
 * remove_wanted_socket() methods), then perform sync_wait() (with an optional timeout).  Once that method
 * returns (which happens once either 1 or more events are active, there is a timeout, or wait is
 * interrupted via Node::interrupt_all_waits()), examine which events (if any) were found to be active, then perform
 * the appropriate operation (e.g., `serv->accept()` if Acceptable, `sock->send()` if Writable) on each active
 * socket/event pair.  This mode of operation is analogous to `FD_SET(); select(); FD_ISSET()` or
 * `epoll_ctl(); epoll_wait()`.  The key feature is that the sync_wait() (like `select()` or `epoll_wait()`) call blocks
 * the calling thread until there are events or a timeout or a global interrupt (the latter similar to POSIX `EINTR`
 * caused by a signal).  Additionally, the Event_set::poll() method is analogous to a `select()` call with a 0 timeout.
 *
 * The above is sufficient when writing an event loop that works *only* with Flow-protocol sockets.  However,
 * many practical applications will use an event loop that works with Flow-protocol sockets and other
 * resources (e.g., TCP sockets, pipes) simultaneously.  Then what?
 *   - If you have a boost.asio event loop, then (again) the best way to use Event_set is by not using it directly;
 *     instead use asio::Node hierarchy to use Flow-protocol sockets as 1st-class-citizen boost.asio-style I/O objects.
 *     Internally, again, an Event_set will be indirectly used; but one would not use it directly.
 *   - Otherwise -- if there is no boost.asio event loop, and it's impractical to convert to using one (generally a
 *     a good idea but may not be practical under time constraints), then direct use the core Event_set feature
 *     (namely Event_set::async_wait()) is just the ticket.
 *
 * We now discuss Event_set::async_wait().  The basic deficiency of sync_wait() is simply that it will only wake up
 * if there's a Flow-protocol event and knows nothing about anything
 * else.  Similarly, your `select()`-or-equivalent-of-choice
 * will know nothing of Flow-protocol events, since it can only work with standard file descriptors
 * (or whatever), not Flow sockets.  For this case Event_set provides asynchronous waiting.  As
 * before, you'll construct an Event_set and add some desired sockets/events.  Then, call
 * `async_wait(F)`, where `F()` is a function.  async_wait() will IMMEDIATELY return.  However, another,
 * unspecified, thread will wait for the desired condition(s) to become true.  Once that occurs,
 * that unspecified thread will call `F()`.  `F()` can do whatever you want (but see restrictions
 * documented on async_wait()); however in the use case described in this paragraph it will probably
 * want to do something that will wake up the `select()` (or `epoll_wait()`, or ...) of your main event
 * loop.  For example, you can set up a Unix domain socket or pipe and add it into your `select()` (or
 * equivalent's) event set; and then have `F()` write 1 byte to it.  Once this occurs, that `select()`
 * (etc.) will wake up; you can call `async_wait_finish()` and then check the active Flow-protocol events using
 * the same techniques as shown in the preceding paragraph, after having called sync_wait().
 *
 * The following illustrates the preceding paragraph.  In this example, we are interested in a single TCP
 * socket from which to read; and a single Flow socket from which to read.  This can be extended to more
 * native and Flow socket types, more sockets, and more sophisticated native mechanisms than `select()`;
 * but basically this is it.  (However, in case of `select()` and its peculiar `FD_SET()` semantics, please
 * note that this examples combines `sel_read_set` to handle both the special local "linking" socket `comm_sock`
 * and the TCP-readable socket `tcp_sock`.  If you want to check for another type of event than being "readable" --
 * e.g., "writable" -- then with `select()` you'd need to make a separate event set, perhaps named `sel_write_set`,
 * and it would not be handling `comm_sock`, in whose case we only care about readability.  Note also that
 * the example event loop iteration shown below models situation where `tcp_sock` and `flow_sock` happen to
 * both become readable just about at the same time, causing `select()` to return with 2 sockets in readable state
 * simultaneously for illustration purposes.  Realistically probably one would become readable; then in another
 * loop iteration the other would... but this would be annoyingly lengthy to show; and though this is less
 * probable, it's still possible.
 *
 *   ~~~
 *   // NOTE: ASCII art arrows |--------> (etc.) indicate thread execution.  "U" is the user thread.
 *   //       "K" is unspecified kernel "thread."  "W" is unspecified `net_flow` working thread.
 *   //       T (cont.) just means thread T was already running up to the point we are illustrating.
 *
 *   U (cont.)
 *   | Set up local socket (e.g., Unix domain socket or localhost TCP socket) comm_sock.
 *   | Set up select() native socket set sel_read_set, for native "readable" events.
 *   | Set up net_flow::Event_set flow_set.
 *   | FD_SET(comm_sock, sel_read_set);
 *   | Set up native TCP peer socket tcp_sock.
 *   | Set up Flow peer socket flow_sock.
 *   | FD_SET(tcp_sock, sel_read_set);
 *   | flow_set.add_wanted_socket(flow_sock, Event_type::S_PEER_SOCKET_READABLE);
 *   | Event loop begins here. <--------------------------------------+ K (cont.)             W (cont.)
 *   |   flow_set.async_wait(handle_flow_ev);                         | | ...                 | ...
 *   |     Inform W. ->                                               | | ...                 | flow_set -> WAITING.
 *   |   select(2, sel_read_set, NULL, NULL, ...positive-timeout...); | | ...                 | ...
 *   |     ...select() blocking...                                    | | ...                 | ...
 *   |     ...                                                        | | ...                 | ...
 *   |     ...                                                        | | tcp_sock readable!  | ...
 *   |     ...                                                        | | <- Tell select().   |
 *   |     K says tcp_sock readable! Get ready to return!             | | ...                 | flow_sock readable!
 *   |                                                                | |                     | handle_flow_ev():
 *   |                                                                | |                     |   Write 1 -> comm_sock.
 *   |                                                                | | comm_sock readable! |
 *   |                                                                | | <- Tell select().   |
 *   |     K says comm_sock readable, and that's it.  Return!         | | ...                 | ...
 *   |   flow_set.async_wait_finish();                                | v                     | ...
 *   |     Inform W. ->                                               |                       | ...
 *   |   if (FD_ISSET(tcp_sock, sel_read_set))                        |                       | flow_set -> INACTIVE.
 *   |     Non-blocking recv(tcp_sock) until exhausted!               |                       v
 *   |   if (FD_ISSET(comm_sock, sel_read_set))                       |
 *   |     In this example we've only 1 Flow socket and event, so: \  |
 *   |     flow_sock.async_receive() until exhausted!                 |
 *   | Event loop ends here. -----------------------------------------+
 *   v
 *   ~~~
 *
 * The above are informal suggestions for use.  Here is the more formal description of operation of
 * an Event_set.  Event_set is a simple state machine with three states: INACTIVE, WAITING, CLOSED.
 * After construction (which is done by Node in a factory fashion), Event_set is in INACTIVE state.
 * CLOSED means the Event_set has been Event_set::close()d, or the originating Node has been destroyed.  It is
 * not possible to perform any operations on a CLOSED Event_set, nor is it possible to exit the CLOSED
 * state.  A CLOSED Event_set stores no resources and will be deleted via `shared_ptr<>` mechanics
 * once all user references to it are gone.
 *
 * The rest of the time, Event_set switches back and forth between INACTIVE and WAITING.  In
 * INACTIVE, you may set and examine the desired sockets/events.  The following are supported (see also
 * Event_set::Event_type `enum` values)
 *
 *   - <em>Peer_socket Readable</em>: true if and only if `sock->receive()` with unlimited target buffer
 *     space would return either a non-zero number of bytes or indicate an error.  (Therefore "not Readable"
 *     means it would return 0 but no error.)
 *   - <em>Peer_socket Writable</em>: same but with `sock->send()`.  Note, however, that typically this is
 *     more likely to be immediately true on a given socket; if there's space in the Send buffer for a certain
 *     small amount of data, then the socket owning it is Writable; whereas data have to actually have arrived
 *     from the other side for it to be Readable.  Nevertheless, if network conditions are such that the
 *     Send buffer cannot be purged via sending its contents to the other side, then eventually it won't be
 *     Writable either (so don't count on writability without checking).
 *   - <em>Server_socket Acceptable</em>: true if and only if `serv->accept()` would return either a non-null
 *     Server_socket::Ptr or indicate an error.  (Therefore "not Acceptable" means it would return
 *     null but no error.)
 *
 * The desired events can be specified with swap_wanted_sockets(), add_wanted_socket(),
 * and remove_wanted_socket().  Note that all of these methods are efficient (no copying
 * of socket sets involved).  Also note that all the socket objects passed to an Event_set must come
 * from the same Node as that Event_set; else behavior is undefined.
 *
 * Also in INACTIVE, you may examine the results of the last wait, if any.  Do this using
 * emit_result_sockets(), etc.  Note that these methods are also efficient (no copying) and can
 * only be used once for each wait (all subsequent uses yield empty socket sets).
 *
 * In WAITING state, a wait for the specified events has started.  async_wait(), sync_wait(), and
 * poll() change state from INACTIVE to WAITING.  In WAITING state, all of the above methods return
 * errors.  The following pieces of code change state back to INACTIVE: async_wait_finish(),
 * sync_wait() (once an event is true, or timeout), poll() (immediately), and an unspecified
 * non-user thread if async_wait() was used, and an event has been asynchronously detected.
 *
 * In particular, if `Event_set e` is used by 1 thread, and that thread performs only sync_wait()s for
 * waiting, user code will never be able to observe the WAITING state, as sync_wait() will go
 * INACTIVE->WAITING->INACTIVE internally.
 *
 * ### Relationship between 2 different Event_set objects ###
 * They are entirely independent.  In particular, you
 * may put the same socket+event combo into 2 different Event_set objects and wait on both Event_sets
 * simultaneously.  If that socket/event holds for both Event_set objects at the same time, both will
 * be signalled.  I informally recommend against using this.  For example, if the event is "sock is
 * Readable," and you will `receive()` as a result, which of the two `receive()` calls gets what data?
 * However there may be a reasonable use case for the Acceptable event.
 *
 * ### Signals and interruption ###
 * Any POSIX blocking function, once it has started waiting, will exit with `errno == EINTR`
 * if a signal is delivered.  (Sometimes this is just annoying, but sometimes it's useful: if SIGTERM
 * handler sets some global terminate flag, then the signal delivery will immediately break out of
 * the blocking call, so the flag can be immediately checked by the main code (and then the program
 * can cleanly exit, for example).)  Event_set::sync_wait() and Event_set::async_wait() support
 * similar functionality.  Call Node::interrupt_all_waits() to interrupt any Event_set object(s) within
 * that Node currently in WAITING state.  They enter INACTIVE state, and it is indicated to the
 * user of each sync_wait() or async_wait() that the reason for the wait's finish was an interruption
 * (see those functions for details on how this is communicated).  Conceptually this is similar to
 * POSIX blocking `select()` or blocking `recv()` returning -1/`EINTR`.
 *
 * To actually cause signals to trigger Node::interrupt_all_waits() (as occurs by default,
 * conceptually, in POSIX programs w/r/t `EINTR`), the user has two basic options.  They can either
 * register signal handlers that'll explicitly invoke that method; or they can let Node do so
 * automatically for SIGINT and SIGTERM.  This is controlled by the Node option
 * Node_options::m_st_capture_interrupt_signals_internally.
 *
 * ### Thread safety ###
 * Same as for Peer_socket.  (Briefly: all operations safe for simultaneous execution
 * on separate or the same object.)  An informal recommendation, however, is to only use a given
 * Event_set in one thread.  Otherwise things will get confusing, quickly, with no practical benefit of which
 * I, at least, am aware.
 *
 * @internal
 *
 * ### Implementation notes ###
 * The crux of the implementation is in async_wait(), as all other wait types are built on it.
 * Therefore there is a giant comment inside that method that is required reading for understanding
 * this class's innards (which, IMO, are some of the trickiest logic in all of the library).
 *
 * As with Peer_socket and Server_socket, much of the implementation (as opposed to interface) of
 * Event_set functionality resides in class Node.  In particular anything to do with thread W is in
 * Node.  The actual code is in event_set.cpp, but since the logic involves Node state, and
 * Event_set is subservient to the Node, in terms of objects it makes more sense to keep it in
 * class Node.  However, when no Node state is involved, Event_set logic is actually typically
 * coded in Event_set methods, to a greater extent than in Peer_socket or Server_socket, which are
 * mere data stores in comparison.
 */
class Event_set :
  // Endow us with shared_ptr<>s ::Ptr and ::Const_ptr (syntactic sugar).
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Event_set>>,
  // Allow access to Ptr(this) from inside Event_set methods.  Just call shared_from_this().
  public boost::enable_shared_from_this<Event_set>,
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// A state of an Event_set.
  enum class State
  {
    /**
     * Default state; valid Event_set that is not currently waiting on events.  All user
     * operations are valid in this state.
     */
    S_INACTIVE,
    /**
     * Waiting state: valid Event_set that is currently waiting on previously described events. In
     * this state only async_wait_finish() may be called without resulting in an error.
     */
    S_WAITING,
    /// Node has disowned the Peer_socket; all further operations will result in error.
    S_CLOSED
  }; // enum class State

  /**
   * Type of event or condition of interest supported by class Event_set.  When specifying interest in some socket
   * reaching a certain condition, or when requesting the results of that interest, the user essentially specifies
   * a pair of data: an enumeration value of this type and a socket of a certain type (that type is specified in
   * the very name of the #Event_type).  For example, I may be interested in Event_type::S_PEER_SOCKET_READABLE
   * becoming true for Peer_socket::Ptr `sock`.  The precise meaning is documented for each enumeration value.
   *
   * @internal
   *
   * ### Implementation notes ###
   * You will note a few different structures, such as Event_set::m_want and Event_set::m_can, are keyed by this
   * type.  Doing it that way, instead of simply having a simpler Event_set::m_want-like structure (etc.) exist 3x,
   * has its plusses and minuses.  Historically, this was actually implemented that other way first, but I wanted
   * to play with `boost::any` to see if it makes the code more elegant.  It has certainly resulted in significantly
   * fewer methods and less code; and in code that is more generic (and, in principle, extensible if we somehow
   * come up with another event type of interest); and less bug-prone.  The code is also arguably more opaque and
   * harder to grok right off the bat, as it's more abstract.  Finally, from the user's point of view, it is slightly
   * harder to use in that when emitting results of an `Event_set::[a]sync_wait()`,
   * one must `any_cast<>` to the proper socket type -- easy but requires a small learning curve.  I'll
   * say overall it is better this way than the other way.
   *
   * In terms of extending this `enum`, which seems unlikely, the following would be involved.  Add a value for
   * the `enum`; then add clauses for it in Socket_as_any_hash::operator(), Socket_as_any_equals::operator(),
   * Event_set::sock_as_any_to_str().  Extend the structures in Event_set::empty_ev_type_to_socks_map() and
   * S_EV_TYPE_TO_IS_ACTIVE_NODE_MTD.  The latter will need a new method written in Node that checks for
   * whether the condition of this type currently holds for the given socket -- whose type, by the way, you will
   * need to decide and document in the doc header for the new `enum` value.  Finally, you'll need to find all
   * situations throughout the code where the condition may change from not holding to holding and possibly
   * save it into Node::m_sock_events when detected (for example, see when `m_sock_events[S_PEER_SOCKET_READABLE]`
   * is `insert()`ed into).
   */
  enum class Event_type
  {
    /**
     * Event type specifying the condition of interest wherein a target `Peer_socket sock` is such that
     * calling `sock->receive()` would yield either non-zero ("successfully dequeued
     * received data") or zero and an error (but not zero and NO error).  In other words, specifies
     * the condition where a Peer_socket is Readable.
     *
     * In Event_set::Sockets structures associated with this Event_set::Event_type, `boost::any` elements
     * wrap the type: Peer_socket::Ptr. `boost::any_cast<>` to that type to obtain the socket.
     */
    S_PEER_SOCKET_READABLE,

    /**
     * Event type specifying the condition of interest wherein a target `Peer_socket sock` is such that
     * calling `sock->send()` with a non-empty buffer would yield either non-zero ("successfully enqueued
     * data to be sent") or zero and an error (but not zero and NO error).  In other words, specifies
     * the condition where a Peer_socket is Writable.
     *
     * In Event_set::Sockets structures associated with this Event_set::Event_type, `boost::any` elements
     * wrap the type: Peer_socket::Ptr. `boost::any_cast<>` to that type to obtain the socket.
     */
    S_PEER_SOCKET_WRITABLE,

    /**
     * Event type specifying the condition of interest wherein a target `Server_socket serv` is such that
     * calling `serv->accept()` would yield either non-null ("successfully accepted a ready conneection")
     * or null and an error (but not null and NO error).  In other words, specifies
     * the condition where a Server_socket is Acceptable.
     *
     * In Event_set::Sockets structures associated with this Event_set::Event_type, `boost::any` elements
     * wrap the type: Server_socket::Ptr. `boost::any_cast<>` to that type to obtain the socket.
     */
    S_SERVER_SOCKET_ACCEPTABLE
  }; // enum class Event_type

  class Socket_as_any_hash;
  class Socket_as_any_equals;

  /**
   * A set of sockets of one type, used to communicate sets of desired and resulting events in various Event_set
   * APIs.  As a rule, a given #Sockets object will store sockets of one underlying type; meaning
   * the `boost::any`s stored inside one such set can ALL be `boost::any_cast<>` to the *same* type.  Which type that
   * is is usually determined by the associated #Event_type value, typically supplied alongside a #Sockets set
   * in another argument.  For example, if `ev_type == Event_type::S_PEER_SOCKET_READABLE`, then every `boost::any`
   * in the set can be decoded as follows: `Peer_socket::Ptr sock = any_cast<Peer_socket::Ptr>(sock_as_any)`, where
   * `sock_as_any` is an element in a #Sockets.
   *
   * As of this writing, the type is chronologically ordered; meaning sockets will be stored in order from latest to
   * oldest to be inserted into the structure.  E.g., emit_result_sockets() will produce a set containing of
   * readable sockets, in reverse chronological order in which they were detected as being readable.  This may or may
   * not be useful in debugging.
   *
   * @internal
   *
   * This type is also used internally extensively.
   *
   * @todo Is it necessary to have Event_set::Sockets be aliased to util::Linked_hash_set?  `unordered_set` would also
   * work and take somewhat less memory and computational overhead.  It would become unordered, instead of ordered
   * chronologically, but that seems like a price possibly worth paying.
   *
   * Note, also, the to-do in emit_result_sockets() doc header, regarding using `variant` instead of `any`.
   * Not only would that be faster and arguably safer, being compile-time in nature; but we'd get
   * hashing/equality for free -- hence the code would be much pithier (no need for Socket_as_any_hash or
   * Socket_as_any_equals).
   */
  using Sockets = util::Linked_hash_set<boost::any, Socket_as_any_hash, Socket_as_any_equals>;

  /**
   * The type for custom handler passed to async_wait(), which is executed when one or more active events detected, or
   * interrupted as if by signal.
   *
   * @internal
   *
   * @todo Perhaps async_wait() and other APIs of Event_set taking handlers should be templated on handler type
   * for syntactic sugar.  Though, currently, this would be no faster (it is internally stored as this type anyway and
   * must be so), nor would it actually improve call code which needs no explicit cast (an object of this type will
   * implicitly be substituted as a conversion from whatever compatible-with-this-signature construction they used).
   * So on balance, this currently appears superior.  After all writing non-template bodies is easier/nicer.
   */
  using Event_handler = Function<void (bool)>;

  // Constructors/destructor.

  /// Boring destructor.  Note that deletion is to be handled exclusively via `shared_ptr`, never explicitly.
  ~Event_set();

  // Methods.

  /**
   * Current State of the Event_set.  Note that this may change the moment the method returns.
   *
   * @return Ditto.
   */
  State state() const;

  /**
   * Node that produced this Event_set.  Note that this may change the moment the method returns
   * (but only to null).
   *
   * @return Pointer to (guaranteed valid) Node; null if state() is S_CLOSED.
   */
  Node* node() const;

  /**
   * Clears all stored resources (any desired events, result events, and any handler saved by
   * async_wait()) and moves state to State::S_CLOSED.  In particular Node will have disowned this object
   * by the time close() returns.
   *
   * You might call close() while state is State::S_WAITING, but if a timeout-less sync_wait() is executing
   * in another thread, it will NEVER return.  Similarly, if state is currently WAITING due to
   * async_wait(), the handler saved by async_wait() will NEVER be called.  So don't do that.
   * However, first closing one or more sockets being waited on by those calls and THEN calling
   * `this->close()` is perfectly safe, in that sync_wait() will exit, or the handler will be called.
   * (In fact when Node shuts down it does just that.)
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED.
   */
  void close(Error_code* err_code = 0);

  /**
   * Adds the given socket to the set of sockets we want to know are "ready" by the definition of
   * the given event type.  See individual Event_type enumeration members' doc comments for exact
   * definition of readiness for each #Event_type.  For example, Event_type::S_PEER_SOCKET_READABLE
   * means we want to know when `sock->receive()` would yield either some data or an error, but not
   * no data and no error.
   *
   * @tparam Socket type to which `ev_type` applies.  E.g., Peer_socket for PEER_SOCKET_READABLE.
   * @param sock
   *        Socket to add.  Must be from the same Node as the one originating this Event_set.
   * @param ev_type
   *        The condition we are interested in `sock` reaching.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_ALREADY_EXISTS, error::Code::S_EVENT_SET_CLOSED,
   *        error::Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING.
   * @return `true` if and only if no error occurred (`*err_code` is success).
   */
  template<typename Socket>
  bool add_wanted_socket(typename Socket::Ptr sock, Event_type ev_type, Error_code* err_code = 0);

  /**
   * Opposite of add_wanted_socket().
   *
   * @tparam See add_wanted_socket().
   * @param sock
   *        Socket to remove.
   * @param ev_type
   *        See add_wanted_socket().
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_EVENT_DOES_NOT_EXIST, error::Code::S_EVENT_SET_CLOSED,
   *        error::Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING.
   * @return `true` if and only if no error occurred (`*err_code` is success).
   */
  template<typename Socket>
  bool remove_wanted_socket(typename Socket::Ptr sock, Event_type ev_type, Error_code* err_code = 0);

  /**
   * Efficiently exchanges the current set of sockets we want to know are "ready" by the definiton of
   * the given event type.  See individual Event_type enumeration members' doc comments for exact
   * definition of readiness for each #Event_type.  For example, Event_type::S_PEER_SOCKET_READABLE
   * means we want to know when `sock->receive()` would yield either some data or an error, but not
   * no data and no error.  Use this to perform arbitrarily complex operations on the internal set
   * storing sockets of interest for the given event type `ev_type`, when the add_wanted_socket() and
   * remove_wanted_socket() methods are insufficient.  For example:
   *
   *   ~~~
   *   Event_set::Sockets socks;
   *   // Exchange of our empty `socks` with what's in `es`.
   *   es->swap_wanted_sockets(&socks, Event_type::S_PEER_SOCKET_READBLE);
   *   // ...Remove every 3rd socket from `socks`, and log `socks.size()`....
   *   // Now put the modified socket set back into `es`.
   *   es->swap_wanted_sockets(&socks, Event_type::S_PEER_SOCKET_READBLE);
   *   ~~~
   *
   * ### Rationale ###
   * The swap paradigm (precursor to the "move" paradigm added in C++11) allows
   * arbitrarily complex operations without sacrificing performance or thread safety.
   *
   * @param target_set
   *        Pointer to set of sockets to which to load the current internal set.  Currently
   *        contained sockets must be from the same Node that originated this Event_set.
   * @param ev_type
   *        See add_wanted_socket().
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED, error::Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING.
   * @return `true` if and only if no error occurred (`*err_code` is success).
   */
  bool swap_wanted_sockets(Sockets* target_set, Event_type ev_type, Error_code* err_code);

  /**
   * Identical to `swap_wanted_sockets(&sockets, ev_type, err_code)`, where originally `sockets` is empty and
   * is afterwards cleared; but more efficient.
   *
   * @param ev_type
   *        See swap_wanted_sockets().
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED, error::Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING.
   * @return `true` if and only if no error occurred (`*err_code` is success).
   */
  bool clear_wanted_sockets(Event_type ev_type, Error_code* err_code = 0);

  /**
   * Returns `true` if and only if at least one wanted event for at least one socket is registered
   * (via add_wanted_socket(), swap_wanted_sockets(), etc.).
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED.
   * @return `true` if there are wanted events; `false` if there are no wanted events (then `*err_code` is
   *         success) or there was an error (`*err_code` is failure; i.e., `bool(*err_code) == true`).
   */
  bool events_wanted(Error_code* err_code = 0) const;

  /**
   * Checks for all previously described events that currently hold, saves them for retrieval via
   * emit_result_sockets(), etc., and returns.  This is akin to a non-blocking sync_wait() (which does
   * not exist; poll() does), a/k/a a `select()` with a timeout of zero.
   *
   * In a non-error invocation, state() will be State::S_INACTIVE before and after the call.  In an error
   * invocation, state() will not change.
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED, error::Code::S_EVENT_SET_DOUBLE_WAIT_OR_POLL.
   * @return `true` if and only if no error occurred (`*err_code` is success).
   */
  bool poll(Error_code* err_code = 0);

  /**
   * Blocks indefinitely until one or more of the previously described events hold -- or the wait
   * is interrupted; saves them for retrieval via emit_result_sockets(), etc.; and returns.
   * This is akin to a `select()` call with no (i.e., infinite) timeout.
   *
   * The special case of Node::interrupt_all_waits() interrupting this wait -- which is conceptually similar
   * to `EINTR` in POSIX -- manifests itself as error::Code::S_WAIT_INTERRUPTED.  In this case,
   * emit_result_sockets() (etc.) should not be used, as one should assume no events are active due to the
   * interruption.
   *
   * In a non-error invocation, state() will be State::S_INACTIVE before and after the call, unless
   * underlying Node is destroyed, in which case the final state may be State::S_CLOSED.  In an error
   * invocation, state() will not change, and the method will return immediately.  Additionally, the
   * method will NEVER return, if another thread calls `this->close()` or `this->async_wait_finish()`
   * during this sync_wait() (so don't do that).
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_WAIT_INTERRUPTED, error::Code::S_EVENT_SET_NO_EVENTS,
   *        error::Code::S_EVENT_SET_DOUBLE_WAIT_OR_POLL, error::Code::S_EVENT_SET_CLOSED.
   * @return `true` if and only if no error occurred (`*err_code` is success).
   */
  bool sync_wait(Error_code* err_code = 0);

  /**
   * Same as the other sync_wait() but will stop waiting if the timeout given as argument
   * expires.  If the timeout expires, it is the error code error::Code::S_WAIT_USER_TIMEOUT.  This is akin
   * to a `select()` call with a finite timeout.
   *
   * An additional error situation is possible in addition to that described in the 2nd/3rd paragraphs of
   * the other sync_wait()'s doc header: if this is `close()`d during the wait, and the wait
   * times out, error::Code::S_EVENT_SET_CLOSED will be emitted when this timeout is detected.  On the
   * positive side, that means `sync_wait(timeout)` will eventually exit no matter what.
   *
   * Tip: Typical types you might use for `max_wait`: `boost::chrono::milliseconds`,
   * `boost::chrono::seconds`, `boost::chrono::high_resolution_clock::duration`.
   *
   * No guarantees are made as to the accuracy of the timeout timer, although you can optimistically
   * provide arbitrarily precise values for `max_wait`.
   *
   * @tparam Rep
   *         See `boost::chrono::duration` documentation (and see above tip).
   * @tparam Period
   *         See `boost::chrono::duration` documentation (and see above tip).
   * @param max_wait
   *        The maximum amount of time from now to wait before giving up on the wait and returning.
   *        `"duration<Rep, Period>::max()"` will eliminate the time limit and cause indefinite wait.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        Same as the other sync_wait() plus: error::Code::S_WAIT_USER_TIMEOUT.
   * @return `true` if and only if no error occurred (`*err_code` is success).  Timeout expiring IS
   *         an error, in particular.
   */
  template<typename Rep, typename Period>
  bool sync_wait(const boost::chrono::duration<Rep, Period>& max_wait, Error_code* err_code = 0);

  /**
   * Moves object to State::S_WAITING state, saves the given handler to be executed later (in a different,
   * unspecified thread), when one or more of the previously described events hold; and immediately
   * returns.  State will go back to State::S_INACTIVE when the handler fires; or when async_wait_finish() is
   * called by the user (whichever happens first).  State may also change to State::S_CLOSED if `this->close()` is
   * called, or the Node is destroyed.  The saved handler will be forgotten at that time.  Once
   * INACTIVE is entered, emit_result_sockets(), etc., are to be used to access the detected
   * events.  on_event() must take one bool argument.  If this argument is `false`, use emit_result_sockets(),
   * etc., to access the detected events.  If this argument is `true`, then Node::interrupt_all_waits() was
   * invoked and has interrupted this wait (conceptually similar to `EINTR` in POSIX).  In the latter case,
   * do no use emit_result_sockets(), etc., as no events are active due to the interruption.
   *
   * In a non-error invocation, state() will be INACTIVE before the call and WAITING after it.
   * In an error invocation, state() will not change, and the method will return immediately.
   * Additionally, on_event() will NEVER be called, if another thread calls `this->close()` before
   * async_wait_finish() is called, and before the Node is destroyed.  So don't do that.
   *
   * Restrictions on what `on_event()` is allowed to do: It is allowed to do anything except make any
   * `net_flow` call related to the net_flow::Node originating the current Event_set; doing so results in
   * undefined behavior.  Informally, it also must not block; spending significant time in
   * `on_event()` will disrupt the functionality of the Node.  Even more informally, the goal of
   * `on_event()` should be to quickly signal the user's thread(s) that the events hold (using the
   * technique of the user's choice) and then return -- beyond that, the handling of the ready events
   * should be in the user's thread(s).
   *
   * Tip: Call async_wait_finish() before checking the saved results, using emit_result_sockets().  Doing so BEFORE
   * any events are detected will finish this asynchronous wait.  Doing so AFTER any events are
   * detected will be a harmless NOOP.
   *
   * Tip: Use lambdas (or `bind()`) to make async_wait() asynchronously call any arbitrary function
   * or method with any arbitrary arguments -- NOT just a free `void` function with 1 argument.  Outside the scope
   * of discussion here, but if this doesn't ring a bell, please look into lambdas (`bind()` as a backup).
   *
   * Rationale for no timeout argument: Since the caller of async_wait() retains flow control
   * without blocking, the user code can enforce its own timeout logic, if necessary, and simply
   * call async_wait_finish() when desired.  In fact that is just what sync_wait() (with timeout argument) does
   * internally.
   *
   * @param on_event
   *        The function to call as soon as as one or more events previously described hold, AND
   *        `this->state()` is still State::S_WAITING.  (Also see above tip.)
   *        `on_event(bool was_interrupted)` will be called.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_DOUBLE_WAIT_OR_POLL, error::Code::S_EVENT_SET_CLOSED.
   * @return `true` if and only if no error occurred (`*err_code` is success).
   */
  bool async_wait(const Event_handler& on_event, Error_code* err_code = 0);

  /**
   * Moves object from State::S_WAITING to State::S_INACTIVE, and forgets any handler saved by async_wait(), or does
   * nothing if state is already INACTIVE.  Use this to cut short an asynchronous wait started by
   * async_wait().  After return, emit_result_sockets(), etc., can be used to check the active
   * events (if any) detected during the last wait.
   *
   * In a non-error invocation, state() will be State::S_INACTIVE or State::S_WAITING before the call and
   * State::S_INACTIVE after it.  In an error invocation, state() will not change.
   *
   * You might call async_wait_finish() while another thread is executing a timeout-less sync_wait()
   * (which is also invoked by blocking methods like Peer_socket::sync_receive()).  However that
   * will cause that sync_wait() to NEVER return.  So don't do that.
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED.
   * @return `true` if and only if no error occurred (`*err_code` is success).  In particular, state()
   *         being State::S_INACTIVE when the method starts is not an error.
   */
  bool async_wait_finish(Error_code* err_code = 0);

  /**
   * Returns `true` if and only if the last wait, if any, detected at least one event.  In other
   * words, returns `true` if and only if emit_result_sockets() would currently emit at least one
   * socket, if tried with all possible Event_type.
   *
   * One can still use `emit_result_sockets()` to get the specific events after calling this.
   *
   * @note Conceptually, this is a bit like when `select()` returns 0 or higher; and one uses the check
   *       of whether its return value is 0 or non-zero.  Non-zero is actually some complex index thing,
   *       but often that detail is not necessary (much like `emit_result_sockets()` is unnecessary, analogously),
   *       as the mere presence or absence of 1+ events is enough information.  For example, if only one event
   *       for one socket is being waited on, one can check this and confidently perform the appropriate I/O
   *       operation for that one socket, if and only if this returns `true` -- or `select()` would return
   *       non-zero.  Slightly more wastefully, but still not bad at all, is when (say) 2 event types are being
   *       waited on, but for only 1 socket.  In that case `true` return => just perform both I/O operations;
   *       one OR both of them should yield something (and the one that doesn't hardly uses any resources).
   *       Similarly, even if you're waiting on a few sockets, if it's a limited number (like, say, 2-3), then
   *       indiscriminately trying all possible I/O on all 2-3 sockets is only slightly wasteful: and the code
   *       is quite a bit shorter than carefully checking `emit_result_sockets()` (or doing `FD_ISSET()`, etc., in
   *       the analogous `select()` code).
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED, error::Code::S_EVENT_SET_RESULT_CHECK_WHEN_WAITING.
   * @return `true` if there are active events; `false` if there are no active events (then `*err_code` is
   *         success) or there was an error (`*err_code` is failure; i.e., `bool(*err_code) == true`).
   */
  bool events_detected(Error_code* err_code = 0) const;

  /**
   * Gets the sockets that satisfy the condition of the given Event_type detected during the last wait.
   * More precisely, moves all sockets satisfying that condition detected during the last wait (if any)
   * into the set provided by the user.  Because it is a move (for efficiency among other reasons), the
   * subsequent calls to the same method will yield empty sets (until the next wait operation).
   * Calling before any wait will also yield an empty set.
   *
   * Note that the accumulated sockets are NOT preserved across waits.  That is, if you
   * start a wait, the preceding wait's results are wiped out.
   *
   * ### Rationale ###
   * Making the method a one-off that returns nothing after the first invocation (per
   * wait) allows for thread safety without sacrificing efficiency by adding set copy.
   *
   * ### How to use ###
   * First, prepare a (usually empty) target socket set structure.  Second, call emit_result_sockets() to transfer
   * the results to it.  Third, for each socket of interest, `any_cast<>` it from `boost::any` to the
   * socket pointer of the appropriate type.  (Recall that `boost::any` stores an object of any type
   * inside it, so to get access to that you must know to what to cast it; but this is easy, since by
   * specifying `ev_type` you are implying a certain socket type.)  Example:
   *
   *   ~~~
   *   Event_set::Sockets readable_socks; // Note this contains boost::any's as elements, regardless of `ev_type`.
   *   event_set->emit_result_sockets(&readable_socks, Event_set::Event_type::S_PEER_SOCKET_READABLE);
   *   for (const auto& readable_sock_as_any : readable_socks)
   *   {
   *     // Since we asked for S_PEER_SOCKET_READABLE, we know results are all Peer_sockets.  Cast to that type:
   *     const Peer_socket::Ptr readable_sock = boost::any_cast<Peer_socket::Ptr>(readable_sock_as_any);
   *     // PEER_SOCKET_READABLE indicates readiness to receive() to yield data or an error.  We can now do so!
   *     readable_sock->receive(...); // Details of this call left to reader.
   *   }
   *   ~~~
   *
   * @param target_set
   *        Pointer to set of sockets to which to load the current internal set of result sockets
   *        of type `ev_type` found during the last wait.  Any elements here at call time will be removed.
   * @param ev_type
   *        The condition we are interested in which sockets have reached during the last wait.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED, error::Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING.
   * @return `true` if and only if no error occurred (`*err_code` is success).
   *
   * @todo Event_set::emit_result_sockets() sets a #Sockets structure which stores `boost:any`s each of which
   *       stores either a `Peer_socket::Ptr` or a `Server_socket::Ptr`; #Sockets should be changed to store
   *       C++17 `std::variant`s.  Performance, both internally and externally, would improve by using this
   *       type-safe compile-time mechanism (which is akin to `union`s but much more pleasant to use).
   *       At the time this feature was written, Flow was in C++11, so `variant`s were not available, and
   *       the author wanted to play around with `any`s instead of haxoring old-school `union`s.
   *       `variant` is much nicer, however, and the dynamic nature of `any` is entirely unnecessary here.
   */
  bool emit_result_sockets(Sockets* target_set, Event_type ev_type, Error_code* err_code = 0);

  /**
   * Identical to `emit_result_sockets(&sockets, ev_type, err_code)`, where originally `sockets` is empty and
   * is afterwards cleared; but more efficient.
   *
   * @param ev_type
   *        See emit_result_sockets().
   * @param err_code
   *        Same.
   * @return Same.
   */
  bool clear_result_sockets(Event_type ev_type, Error_code* err_code = 0);

  /**
   * Forgets all sockets stored in this object in any fashion.
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_EVENT_SET_CLOSED, error::Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING.
   * @return true if and only if no error occurred (*err_code is success).
   */
  bool clear(Error_code* err_code = 0);

private:
  // Friends.

  /**
   * See rationale for `friend`ing Node in class Event_set documentation header.
   * @see Node.
   */
  friend class Node;

  // Types.

  /**
   * Short-hand for reentrant mutex type.  We explicitly rely on reentrant behavior, so this isn't "just in case."
   * (One shouldn't use reentrant mutexes "just in case"; it should be entirely conscious and on-purpose; one should
   * use non-reentrant mutexes, all else being equal.)
   *
   * @todo This doc header for Event_set::Mutex should specify what specific behavior requires mutex reentrance, so that
   * for example one could reevaluate whether there's a sleeker code pattern that would avoid it.
   */
  using Mutex = util::Mutex_recursive;

  /// Short-hand for RAII lock guard of #Mutex.  Use instead of `boost::lock_guard` for `release()` at least.
  using Lock_guard = util::Lock_guard<Mutex>;

  /**
   * Short-hand for type storing a set of socket sets -- one per possible #Event_type `enum` value.
   * In practice, the key set for a value of this type is all Event_type members; use
   * empty_ev_type_to_socks_map() to create a maximally empty such structure.
   */
  using Ev_type_to_socks_map = util::Linked_hash_map<Event_type, Sockets>;

  // Constructors.

  /**
   * Constructs object; initializes all values to well-defined but possibly meaningless values (0,
   * empty, etc.).
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   */
  explicit Event_set(log::Logger* logger_ptr);

  // Methods.

  /**
   * Helper that ensures the state of `*this` is such that one may modify the #m_can and #m_want
   * socket sets.  Pre-condition: #m_mutex is locked.
   *
   * @param err_code
   *        If `false` returned, sets this to the reason why socket sets are not OK to modify.
   *        Otherwise sets it to success.  Possible errors:
   *        error::Code::S_EVENT_SET_CLOSED, error::Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING.
   * @return `true` if OK to modify; `false` if not OK to modify.
   */
  bool ok_to_mod_socket_set(Error_code* err_code) const;

  /**
   * Same as the public `sync_wait(max_wait)` but uses a `Fine_clock`-based `Fine_duration` non-template
   * type for implementation convenience and to avoid code bloat in specifying timeout.
   *
   * @param max_wait
   *        See the public sync_wait() *with* a `max_wait` argument.  `"duration<Rep, Period>::max()"` maps to the value
   *        `Fine_duration::max()` for this argument.
   * @param err_code
   *        See sync_wait().
   * @return See sync_wait().
   */
  bool sync_wait_impl(const Fine_duration& max_wait, Error_code* err_code);

  /**
   * Creates a maximally empty #Ev_type_to_socks_map: it will have all possible Event_type as keys
   * but only empty #Sockets sets as values.
   *
   * @return Ditto.
   */
  static Ev_type_to_socks_map empty_ev_type_to_socks_map();

  /**
   * Helper that clears each #Sockets set inside an #Ev_type_to_socks_map.  Thus,
   * it is equivalent to `*ev_type_to_socks_map = empty_ev_type_to_socks_map()`, but perhaps faster.
   *
   * @param ev_type_to_socks_map
   *        Target map.
   */
  static void clear_ev_type_to_socks_map(Ev_type_to_socks_map* ev_type_to_socks_map);

  /**
   * Functional helper that checks whether a given `pair` in an #Ev_type_to_socks_map contains an empty
   * set of #Sockets or not.
   *
   * @param ev_type_and_socks
   *        A value (key and mapped, not just mapped) from an #Ev_type_to_socks_map.
   * @return `true` if and only if the socket set is empty.
   */
  static bool ev_type_to_socks_map_entry_is_empty(const Ev_type_to_socks_map::Value& ev_type_and_socks);

  /**
   * Helper that returns a loggable string summarizing the sizes of the socket sets, by type, stored in
   * an #Ev_type_to_socks_map.
   *
   * @param ev_type_to_socks_map
   *        A valid such map.
   * @return Loggable string.
   */
  static std::string ev_type_to_socks_map_sizes_to_str(const Ev_type_to_socks_map& ev_type_to_socks_map);

  /**
   * Helper that returns a loggable string representing the socket stored in the given `boost::any` that
   * stores a value allowed by the members of #Event_type `enum`.
   *
   * @param sock_as_any
   *        See above.  `sock_as_any.empty()` is also allowed.
   * @return Loggable string.
   */
  static std::string sock_as_any_to_str(const boost::any& sock_as_any);

  // Constants.

  /**
   * Mapping from each possible #Event_type to the Node method that determines whether the condition defined by
   * that #Event_type is currently true for the socket wrapped in the `boost::any` argument passed to the method.
   *
   * E.g., `bool Node::sock_is_readable(const boost::any sock_as_any) const` is to return true if and only if
   * `sock = boost::any_cast<Peer_socket::Ptr>(sock_as_any)` -- which is a Peer_socket::Ptr -- would yield non-zero
   * or zero and error if one were to execute `sock->receive()`.  Note that the latter condition is defined by the
   * doc comment on Event_type::S_PEER_SOCKET_READABLE.
   */
  static const boost::unordered_map<Event_type, Function<bool (const Node*, const boost::any&)>>
                 S_EV_TYPE_TO_IS_ACTIVE_NODE_MTD;
  // Data.

  /// See state().  Should be set before user gets access to `*this`.  Must not be modified by non-W threads after that.
  State m_state;

  /// See node().  Should be set before user gets access to `*this`.  Must not be modified by non-W threads after that.
  Node* m_node;

  /**
   * The sockets, categorized by #Event_type of interest, to check for "ready" status (as defined in the doc header
   * for each #Event_type), in the next wait (State::S_WAITING state).
   */
  Ev_type_to_socks_map m_want;

  /**
   * The sockets, categorized by #Event_type of interest, that were found to be "ready" (as defined in the doc header
   * for each #Event_type), during the last wait (State::S_WAITING state).  For each WAITING period, each set in
   * #m_can following that period must be a subset of the corresponding #m_want set when entering that period.
   *
   * E.g., `m_can[S_PEER_SOCKET_READABLE]` after WAITING is a subset of `m_want[S_PEER_SOCKET_READABLE]` when just
   * starting WAITING.
   */
  Ev_type_to_socks_map m_can;

  /**
   * During State::S_WAITING, stores the handler (a `void` function with 1 `bool` argument) that will be
   * called once one or more events are found to hold.  `m_on_event.empty()` at all other times.
   */
  Event_handler m_on_event;

  /**
   * While in State::S_WAITING, if this is `true`, an exhaustive check of all desired events is yet to
   * be performed (in thread W); if `false`, it has alredy been performed (in thread W), and only
   * "delta" checks need to be checked from now on during this wait.  See details in giant comment
   * block inside async_wait().
   */
  bool m_baseline_check_pending;

  /// Mutex protecting ALL data in this object.
  mutable Mutex m_mutex;
}; // class Event_set

/**
 * Hasher class used in storing various sockets of types wrapped as `boost::any`s in the #Sockets type.
 * We do not simply define `hash_value(any)`, because our implementation specifically works for `any` objects
 * that store various sockets and won't work for all other possibilities; and we don't want to force this
 * on every user of `hash<any>`.
 */
class Event_set::Socket_as_any_hash
{
public:
  /**
   * Returns hash value of the given object which must be stored in a #Sockets object.
   *
   * @param sock_as_any
   *        Object wrapping one the socket objects permitted by `enum` members of Event_type.
   * @return Hash.
   */
  size_t operator()(const boost::any& sock_as_any) const;
};

/**
 * Equality predicate class used in storing various sockets of types wrapped as `boost::any`s in the #Sockets type.
 * We do not simply define `operator==(any, any)`, because our implementation specifically works for `any` objects
 * that store various sockets and won't work for all other possibilities; and we don't want to force this
 * on every user of `hash<any>`.
 */
class Event_set::Socket_as_any_equals
{
public:
  /**
   * Returns whether the two objects, which must be stored in #Sockets objects, are equal by value.
   *
   * @param sock_as_any1
   *        Object wrapping one the socket objects permitted by `enum` members of Event_type.
   * @param sock_as_any2
   *        Object wrapping one the socket objects permitted by `enum` members of Event_type.
   * @return `true` if and only if the two are equal by value.
   */
  bool operator()(const boost::any& sock_as_any1, const boost::any& sock_as_any2) const;
};

// Free functions: in *_fwd.hpp.

// However the following refer to inner type(s) and hence must be declared here and not _fwd.hpp.

/**
 * Prints string representation of given Event_set state to given standard `ostream` and returns the
 * latter.
 *
 * @relatesalso Event_set
 *
 * @param os
 *        Stream to print to.
 * @param state
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, Event_set::State state);

/**
 * Prints string representation of given event type to given standard `ostream` and returns the
 * latter.
 *
 * @relatesalso Event_set
 *
 * @param os
 *        Stream to print to.
 * @param ev_type
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, Event_set::Event_type ev_type);

// Template implementations.

template<typename Socket>
bool Event_set::add_wanted_socket(typename Socket::Ptr sock, Event_type ev_type, Error_code* err_code)
{
  using boost::any;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, add_wanted_socket<Socket>, sock, ev_type, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U != W.

  // Accessing m_state, socket sets, etc. which may be written by other threads at any time.  Must lock.
  Lock_guard lock(m_mutex);

  FLOW_LOG_TRACE("Object [" << sock << "] wanted for event type [" << ev_type << "] in Event_set [" << this << "].");

  if (!ok_to_mod_socket_set(err_code)) // Ensure we can modify want_set in this state.
  {
    // *err_code is set.
    return false; // Closed; do not set *target_set (*want_set is blank anyway).
  }
  // else *err_code is success.
  assert(!*err_code);

  /* Note that this method is a template solely because of the following `any(sock)`.  We could've made the user
   * perform this conversion themselves and taken a `boost::any` into a non-template method.  We give them a little
   * sugar though. */
  static_assert(util::Container_traits<Sockets>::S_CONSTANT_TIME_INSERT,
                "Expecting amortized constant time insertion sockets container."); // Ensure fastness.
  if (!m_want[ev_type].insert(any(sock)).second)
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_EVENT_ALREADY_EXISTS);
  }
  // else { *err_code is still success. }

  return !*err_code;
}

template<typename Socket>
bool Event_set::remove_wanted_socket(typename Socket::Ptr sock, Event_type ev_type, Error_code* err_code)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, remove_wanted_socket<Socket>, sock, ev_type, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  using boost::any;

  // We are in thread U != W.

  // Accessing m_state, the sets, etc. which may be written by other threads at any time.  Must lock.
  Lock_guard lock(m_mutex);

  FLOW_LOG_TRACE("Object [" << sock << "] no longer wanted for event type [" << ev_type << "] in "
                 "Event_set [" << this << "].");

  if (!ok_to_mod_socket_set(err_code)) // Ensure we can modify want_set in this state.
  {
    // *err_code is set.
    return false; // Closed; do not set *target_set (*want_set is blank anyway).
  }
  // else *err_code is success.

  Sockets& want_set = m_want[ev_type];
  /* Note that this method is a template solely because of the following assignment.  We could've made the user
   * perform this conversion themselves and taken a `boost::any` into an non-template method.  We give them a little
   * sugar though. */
  const any sock_as_any = sock;

  const bool did_erase = want_set.erase(sock_as_any) == 1;
  if (!did_erase)
  {
    FLOW_ERROR_EMIT_ERROR(error::Code::S_EVENT_SET_EVENT_DOES_NOT_EXIST);
    return false;
  }
  // else *err_code is success.

  return true;
} // Event_set::remove_wanted_socket()

template<typename Rep, typename Period>
bool Event_set::sync_wait(const boost::chrono::duration<Rep, Period>& max_wait, Error_code* err_code)
{
  assert(max_wait.count() > 0);
  return sync_wait_impl(util::chrono_duration_to_fine_duration(max_wait), err_code);
}

} // namespace flow::net_flow
