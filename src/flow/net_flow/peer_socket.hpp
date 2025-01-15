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
#include "flow/net_flow/detail/net_flow_fwd.hpp"
#include "flow/net_flow/options.hpp"
#include "flow/net_flow/endpoint.hpp"
#include "flow/error/error.hpp"
#include "flow/net_flow/error/error.hpp"
#include "flow/net_flow/server_socket.hpp"
#include "flow/util/linked_hash_map.hpp"
#include "flow/util/util.hpp"
#include "flow/util/sched_task_fwd.hpp"
#include "flow/net_flow/detail/low_lvl_io.hpp"
#include "flow/net_flow/detail/socket_buffer.hpp"
#include "flow/net_flow/detail/stats/socket_stats.hpp"
#include "flow/util/shared_ptr_alias_holder.hpp"
#include <boost/utility.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/move/unique_ptr.hpp>
#include <type_traits>

namespace flow::net_flow
{
// Types.

/**
 * A peer (non-server) socket operating over the Flow network protocol, with optional stream-of-bytes and
 * reliability support.
 *
 * Reliability is enabled or disabled via a socket option, Peer_socket_options::m_st_rexmit_on,
 * at socket creation.  Use unreliable mode with care -- see send() method doc header for details.
 *
 * ### Life cycle of a Peer_socket ###
 * A given Peer_socket can arise either by connecting to
 * Server_socket on a Node (Node::connect() or Node::sync_connect()), or by listening on a
 * Node's Server_socket and accepting such a connection (Server_socket::accept() or
 * Server_socket::sync_accept()).  In all cases, Node or Server_socket generates a new Peer_socket
 * and returns it (factory pattern).  Peer_socket is not instantiable otherwise.  A Peer_socket
 * cannot be deleted explicitly by the user and will only be returned via `boost::shared_ptr<>`; when
 * both the Node and all user code no longer refers to it, the Peer_socket will be destroyed.
 *
 * Once a `net_flow` user has a Peer_socket object, that object represents a socket in one of the
 * following basic states:
 *
 *   - Open.
 *     - Sub-states:
 *       - Connecting.  (Never Writable, never Readable.)
 *       - Connected.  (May be Writable, may be Readable.)
 *       - Disconnecting.  (May be Readable, never Writable.)
 *   - Closed.
 *     - Socket can neither read nor write.
 *
 * Open.Connecting means means Node initiated a connect to the given server, and this is in
 * progress. Open.Connected means the connection to the other Node is fully functional.
 * Open.Disconnecting means either our side or the other side has initiated a clean or abrupt
 * disconnect, but it is not yet entirely finished (background handshaking is happening, you have
 * not read all available data or sent all queued data, etc.).
 *
 * In either case, reading and writing may or may not be possible at a given time, depending on the
 * state of the internal buffers and the data having arrived on the logical connection.  Thus all
 * Open sub-states can and often should be treated the same way in a typical Flow-protocol-using algorithm:
 * simply determine when the Peer_socket is Readable, and read; and similarly for Writable and
 * write.  Thus the sub-states are distinguished for informational/diagnostic purposes only, as user
 * reading/writing logic in these states should usually be identical.
 *
 * @todo Closing connection considerations.  May implement closing only via timeouts at first (as
 * opposed to explicit closing).  Below text refers to `close_final()` and `close_start()`, but those
 * are just ideas and may be replaced with timeout, or nothing.  At this time, the only closing
 * supported is abrupt close due to error or abrupt close via close_abruptly().
 *
 * Closed means that the Peer_socket has become disconnected, and no data can possibly be
 * received or sent, AND that Node has no more background internal operations to perform and has
 * disowned the Peer_socket.  In other words, a Closed Peer_socket is entirely dead.
 *
 * Exactly the following state transitions are possible for a given Peer_socket returned by Node:
 *
 *    - start => Closed
 *    - start => Open
 *    - Open => Closed
 *
 * Note, in particular, that Closed is final; socket cannot move from Closed to
 * Open.  If after an error or valid disconnection you want to reestablish a
 * connection, obtain a new Peer_socket from Node's factories.  Rationale (subject to change):
 * this cuts down on state having to be tracked inside a Peer_socket, while the interface becomes
 * simpler without much impact on usability.  Anti-rationale: contradicts BSD socket and boost.asio
 * established practices; potentially more resource-intensive/slower in the event of errors and
 * disconnects.  Why IMO rationale > anti-rationale: it's simpler, and the potential problems do not
 * appear immediately serious; added statefulness can be added later if found desirable.
 *
 * Receving, sending, and buffers: Peer_socket, like a TCP socket, has a Receive buffer (a/k/a
 * FIFO queue of bytes) of some maximum size and a Send buffer (a/k/a FIFO queue of bytes) of some
 * maximum size. They are typically not directly exposed via the interface, but their existence
 * affects documented behavior.  I formally describe them here, but generally they work similarly to
 * TCP socket Send/Receive buffers.
 *
 * The Receive buffer: Contains bytes asynchronously received on the connection that have not yet
 * been removed with a `*receive()` method.  Any bytes that asynchronously arrive on the connection are
 * asynchronously stored to the buffer on the other side of the buffer in a queued fashion.
 *
 * The Send buffer: Contains bytes intended to be asynchronously sent on the connection that have
 * been placed there by a `*send()` method but not yet sent on the connection.  Any bytes that are
 * asynchronously sent on the connection are asynchronously removed from the buffer on the other
 * side of the buffer in a queued fashion.
 *
 * With that in mind, here are the definitions of Readable and Writable while state is Open:
 *
 *   - Readable <=> Data available in internal Receive buffer, and user has not explicitly announced
 *                  via `close_final()` they're not interested in reading further.
 *   - Writable <=> Space for data available in internal Send buffer, and the state is Open.Connected.
 *
 * Note that neither definition really cares about the state of the network connection (e.g., could
 * bytes actually be sent over the network at the moment?).  There is one caveat: A
 * socket is not Writable until Open.Connecting state is transitioned away from; this prevents user
 * from buffering up send data before the connection is ready.  (Allowing that would not necessarily
 * be wrong, but I'm taking a cue from BSD socket semantics on this, as they seem to be convenient.)
 *
 * In Open, the following archetypal operations are provided.  (In Closed all
 * immediately fail; in Open.Disconnecting some immediately fail if `close*()` has been called.)  Let
 * R be the current size of data in the Receive buffer, and S be the available space for data in the
 * Send buffer.
 *
 *   - `receive(N)`.  If Readable, return to caller `min(N, R)` oldest data to have been received from
 *     the other side, and remove them from Receive buffer.  Otherwise do nothing.
 *   - `send(N)`.  If Writable, take from caller `min(N, S)` data to be appended to the Send
 *     buffer and, when possible, sent to the other side.  Otherwise do nothing.
 *   - `sync_receive(N)`.  If Readable, `receive(N)`.  Otherwise sleep until Readable, then `receive(N)`.
 *   - `sync_send(N)`.  If Writable, `send(N)`.  Otherwise sleep until Writable, then `send(N)`.
 *
 * These are similar to TCP Receive and Send APIs in non-blocking mode, and TCP Receive and Send APIs in
 * blocking mode, respectively.  There may be other similarly themed methods, but all use these as
 * semantic building blocks.
 *
 * To understand the order of events, one can think of a disconnect-causing event (like a graceful
 * close initiation from the remote socket) as a piece of data itself.  Thus, for example, if 5
 * bytes are received and placed into the Receive buffer without being read by the user, and then a
 * connection close is detected, the socket will be Readable until the 5 bytes have been
 * receive()ed, and the next receive() (or send()) would yield the error, since that's the order
 * things happened.  Similarly, suppose you've sent 5 bytes, but they haven't been yet
 * sent over the wire and are sitting in the Send buffer.  Then you trigger a graceful connection close.
 * First the 5 bytes will be sent if possible, and then the closing procedure will actually begin.
 *
 * Abrupt closes such as connection resets may force both buffers to be immediately emptied without
 * giving to the user or writing to the other side, so that the above rule does not have to apply.
 * Typically a connection reset means the socket is immediately unusable no matter what was in the
 * buffers at the time, per BSD socket semantics.
 *
 * ### Efficiently reading/writing ###
 * The `sync_*`() methods are efficient, in that they use no processor
 * cycles until Readable or Writable is achieved (i.e., they sleep until that point).  The
 * non-blocking versions don't sleep/block, however.  For a program using them to be efficient it
 * should sleep until Readable or Writable and only then call receive()/send(), when data are
 * certainly available for immediate reading or writing.  Moreover, a complex program is likely to
 * want to perform this sleep-and-conditional-wake on a set of several Peer_socket objects simultaneously
 * (similarly to `select()`, `epoll*()`, etc.).  Use class Event_set for this purpose.
 *
 * ### Thread safety ###
 * Same as for Node.  (Briefly: all operations safe for simultaneous execution on
 * separate or the same object.)
 *
 * @internal
 *
 * Implementation notes
 * --------------------
 *
 * While to a user a Peer_socket appears as a nearly self-sufficient object (i.e., you can do things
 * like `s->send()`, which means 'socket `s`, send some data!''), the most reasonable way to internally
 * implement this is to have Node contain the logic behind a Peer_socket (and how it works together
 * with other Peer_socket objects and other internal infrastructure).  Thus Node is the class with all of
 * the logic behind (for example) `s->send()`.  Peer_socket then, privately, is not too much more than a
 * collection of data (like a `struct` almost) to help Node.
 *
 * Therefore Peer_socket provides a clean object-oriented public interface to the user but, on the
 * implementation side, is basically a data store (with Node as `friend`) and forwards the logic to
 * the originating Node.  One idea to make this dichotomy more cleanly expressed (e.g., without
 * `friend`) was to make Peer_socket a pure interface and have Node produce `Peer_socket_impl`
 * objects, where `Peer_socket_impl` implements Peer_socket and is itself private to the user (a
 * classic factory pattern).  Unfortunately defining function templates such as `send<Buffers>()`
 * (where `Buffers` is an arbitrary `Buffers` concept model) as pure `virtual` functions is not really
 * possible in C++.  Since such a templated interface can be highly convenient (see boost.asio with
 * its seamless support for buffers and buffer sequences of most types, including scatter-gather),
 * the usefulness of the interface trumps implementation beauty.
 *
 * To prevent node.cpp from being unmanageably large (and also because it makes sense),
 * implementations for Node methods that deal only with an individual Peer_socket reside in
 * peer_socket.cpp (even though they are members of Node, since, again, the logic is all forwarded to Node).
 *
 * @todo Rename `State` and `Open_sub_state` to `Phase` and `Open_sub_phase` respectively; and
 * `Int_state` to `State`.  Explain difference between phases (application-layer, user-visible and used
 * close to application layer) and states (transport layer, internal).
 *
 * @todo Look into a way to defeat the need for boiler-plate trickery -- with low but non-zero perf cost --
 * involving `*_socket`-vs-`Node` circular references in method templates, such as the way
 * Peer_socket::send() and Peer_socket::receive() internally make `Function<>`s before forwarding to the core
 * in Node.  Can this be done with `.inl` files?  Look into how Boost internally uses `.inl` files; this could
 * inspire a solution... or not.
 */
class Peer_socket :
  public util::Null_interface,
  // Endow us with shared_ptr<>s ::Ptr and ::Const_ptr (syntactic sugar).
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Peer_socket>>,
  // Allow access to Ptr(this) from inside Peer_socket methods.  Just call shared_from_this().
  public boost::enable_shared_from_this<Peer_socket>,
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// State of a Peer_socket.
  enum class State
  {
    /// Future reads or writes may be possible.  A socket in this state may be Writable or Readable.
    S_OPEN,
    /// Neither future reads nor writes are possible, AND Node has disowned the Peer_socket.
    S_CLOSED
  };

  /// The sub-state of a Peer_socket when state is State::S_OPEN.
  enum class Open_sub_state
  {
    /**
     * This Peer_socket was created through an active connect (Node::connect() and the like), and
     * the connection to the remote Node is currently being negotiated by this socket's Node.
     * A socket in this state may be Writable but cannot be Readable.  However, except for
     * diagnostic purposes, this state should generally be treated the same as S_CONNECTED.
     */
    S_CONNECTING,

    /**
     * This Peer_socket was created through a passive connect (Node::accept() and the like) or an
     * active connect (Node::connect() and the like), and the connection is (as far this socket's
     * Node knows) set up and functioning.  A socket in this state may be Writable or Readable.
     */
    S_CONNECTED,

    /**
     * This Peer_socket was created through a passive connect (Node::accept() and the like) or an
     * active connect (Node::connect() and the like), but since then either an active close,
     * passive close, or an error has begun to close the connection, but data may still possibly
     * arrive and be Readable; also data may have been "sent" but still sitting in the Send buffer
     * and needs to be sent over the network.  A socket in this state may be Readable but cannot
     * be Writable.
     *
     * This implies that a non-S_CLOSED socket may be, at a lower level, disconnected.  For
     * example, say there are 5 bytes in the Receive buffer, and the other side sends a graceful
     * disconnect packet to this socket.  This means the connection is finished, but the user can
     * still receive() the 5 bytes (without blocking).  Then state will remain
     * S_OPEN.S_DISCONNECTING until the last of the 5 bytes is received (gone from the buffer); at
     * this point state may change to S_CLOSED (pending any other work Node must do to be able to
     * disown the socket).
     */
    S_DISCONNECTING
  }; // enum class Open_sub_state

  // Constructors/destructor.

  /// Boring `virtual` destructor.  Note that deletion is to be handled exclusively via `shared_ptr`, never explicitly.
  ~Peer_socket() override;

  // Methods.

  /**
   * Current State of the socket.
   *
   * @param open_sub_state
   *        Ignored if null.  Otherwise, if and only if State::S_OPEN is returned, `*open_sub_state` is set to
   *        the current sub-state of `S_OPEN`.
   * @return Current main state of the socket.
   */
  State state(Open_sub_state* open_sub_state = 0) const;

  /**
   * Node that produced this Peer_socket.
   *
   * @return Pointer to (guaranteed valid) Node; null if state is State::S_CLOSED.
   */
  Node* node() const;

  /**
   * Intended other side of the connection (regardless of success, failure, or current State).
   * For a given Peer_socket, this will always return the same value, even if state is
   * State::S_CLOSED.
   *
   * @return See above.
   */
  const Remote_endpoint& remote_endpoint() const;

  /**
   * The local Flow-protocol port chosen by the Node (if active or passive open) or user (if passive open) for
   * this side of the connection.  For a given Peer_socket, this will always return the same value,
   * even if state is State::S_CLOSED.  However, when state is State::S_CLOSED, the port may be unused or
   * taken by another socket.
   *
   * @return See above.
   */
  flow_port_t local_port() const;

  /**
   * Obtains the serialized connect metadata, as supplied by the user during the connection handshake.
   * If this side initiated the connection (Node::connect() and friends), then this will equal what
   * was passed to the connect_with_metadata() (or similar) method.  More likely, if this side
   * accepted the connection (Server_socket::accept() and friends), then this will equal what the
   * user on the OTHER side passed to connect_with_metadata() or similar.
   *
   * @note It is up to the user to deserialize the metadata portably.  One recommended convention is to
   *       use `boost::endian::native_to_little()` (and similar) before connecting; and
   *       on the other side use the reverse (`boost::endian::little_to_native()`) before using the value.
   *       Packet dumps will show a flipped (little-endian) representation, while with most platforms the conversion
   *       will be a no-op at compile time.  Alternatively use `native_to_big()` and vice-versa.
   * @note If a connect() variant without `_with_metadata` in the name was used, then the metadata are
   *       composed of a single byte with the zero value.
   * @param buffer
   *        A buffer to copy the metadata into.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   * @return The size of the copied metadata.
   */
  size_t get_connect_metadata(const boost::asio::mutable_buffer& buffer,
                              Error_code* err_code = 0) const;

  /**
   * Sends (adds to the Send buffer) the given bytes of data up to a maximum internal buffer size;
   * and asynchronously sends them to the other side.  The data given is copied into `*this`, in the order
   * given.  Only as many bytes as possible without the Send buffer size exceeding a certain max are
   * copied.
   *
   * The method does not block.  Data are then sent asynchronously (in the background).
   *
   * Method does nothing except possibly logging if there are no bytes in data.
   *
   * ### Error handling ###
   * These are the possible outcomes.
   *   1. There is no space in the Send buffer (usually due to network congestion).  Socket not
   *      Writable.  0 is returned; `*err_code` is set to success unless null; no data buffered.
   *   2. The socket is not yet fully connected (`S_OPEN+S_CONNECTING` state).  Socket not
   *      Writable.  0 is returned; `*err_code` is set to success unless null; no data buffered.
   *   3. There is space in the Send buffer, and socket connection is open (`S_OPEN+S_CONNECTED`).
   *      Socket Writable.  >= 1 is returned; `*err_code` is set to success; data buffered.
   *   4. The operation cannot proceed due to an error.  0 is returned; `*err_code` is set to the
   *      specific error unless null; no data buffered.  (If `err_code` null, Runtime_error thrown.)
   *
   * The semantics of -3- (the success case) are as follows.  N bytes will be copied into Send
   * buffer from the start of the Const_buffer_sequence data.  These N bytes may be spread across 1
   * or more buffers in that sequence; the subdivision structure of the sequence of bytes into
   * buffers has no effect on what will be buffered in Send buffer (e.g., "data" could be N+ 1-byte
   * buffers, or one N+-byte buffer -- the result would be the same).  N equals the smaller of: the
   * available space in the Send buffer; and `buffer_size(data)`.  We return N.
   *
   * ### Reliability and ordering guarantees: if the socket option rexmit-on is enabled ###
   * Reliability and ordering are guaranteed, and there is no notion of message boundaries.  There is no possibility
   * of data duplication.  In other words full stream-of-bytes functionality is provided, as in TCP.
   *
   * ### Reliability and ordering guarantees: if the socket option rexmit-on is NOT enabled ###
   * NO reliability guarantees are given, UNLESS *ALL* calls to send() (and other `*send`() methods)
   * satisfy the condition: '`buffer_size(data)` is a multiple of `sock->max_block_size()`'; AND all
   * calls to receive() (and other `*receive()` methods) on the OTHER side satisfy the condition:
   * '`buffer_size(target)` is a multiple of `sock->max_block_size()`.'  If and only if these guidelines
   * are followed, and there is no connection closure, the following reliability guarantee is made:
   *
   * Let a "block" be a contiguous chunk of bytes in a "data" buffer sequence immediately following
   * another "block," except the first "block" in a connection, which begins with the first byte of
   * the "data" buffer sequence passed to the first `*send()` call on that connection.  Then: Each
   * given block will either be available to `*receive()` on the other side exactly once and without
   * corruption; or not available to `*receive()` at all.  Blocks may arrive in a different order than
   * specified here, including with respect to other `*send()` calls performed before or after this
   * one.  In other words, these are guaranteed: block boundary preservation, protection against
   * corruption, protection again duplication.  These are not guaranteed: order preservation,
   * delivery.  Informally, the latter factors are more likely to be present on higher quality
   * network paths.
   *
   * @tparam Const_buffer_sequence
   *         Type that models the boost.asio `ConstBufferSequence` concept (see Boost docs).
   *         Basically, it's any container with elements convertible to `boost::asio::const_buffer`;
   *         and bidirectional iterator support.  Examples: `vector<const_buffer>`, `list<const_buffer>`.
   *         Why allow `const_buffer` instead of, say, `Sequence` of bytes?  Same reason as boost.asio's
   *         send functions: it allows a great amount of flexibility without sacrificing performance,
   *         since `boost::asio::buffer()` function can adapt lots of different objects (arrays,
   *         `vector`s, `string`s, and more -- composed of bytes, integers, and more).
   * @param data
   *        Buffer sequence from which a stream of bytes to add to Send buffer will be obtained.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   *        Error implies that neither this send() nor any subsequent `*send()` on this socket
   *        will succeeed.  (In particular a clean disconnect is an error.)
   * @return Number of bytes (possibly zero) added to buffer.  Always 0 if `bool(*err_code) == true` when
   *         send() returns.
   */
  template<typename Const_buffer_sequence>
  size_t send(const Const_buffer_sequence& data, Error_code* err_code = 0);

  /**
   * Blocking (synchronous) version of send().  Acts just like send(), except that if Socket is not
   * immediately Writable (i.e., send() would return 0 and no error), waits until it is Writable
   * (send() would return either >0, or 0 and an error) and returns `send(data, err_code)`.  If a
   * timeout is specified, and this timeout expires before socket is Writable, acts like send()
   * executed on an un-Writable socket.
   *
   * ### Error handling ###
   * These are the possible outcomes (assuming there are data in the argument `data`).
   *   1. There is space in the Send buffer, and socket connection
   *      is open (`S_OPEN+S_CONNECTED`).  Socket Writable.  >= 1 is returned; `*err_code` is set to
   *      success unless null; data buffered.
   *   2. The operation cannot proceed due to an error.  0 is returned; `*err_code` is set to the
   *      specific error unless null; no data buffered.  (If `err_code` null, Runtime_error thrown.)
   *      The code error::Code::S_WAIT_INTERRUPTED means the wait was interrupted
   *      (similarly to POSIX's `EINTR`).
   *   3. Neither condition above is detected before the timeout expires (if provided).
   *      Output semantics are the same as in 2, with the specific code error::Code::S_WAIT_USER_TIMEOUT.
   *
   * The semantics of -1- (the success case) equal those of send().
   *
   * Note that it is NOT possible to return 0 and no error.
   *
   * Tip: Typical types you might use for `max_wait`: `boost::chrono::milliseconds`,
   * `boost::chrono::seconds`, `boost::chrono::high_resolution_clock::duration`.
   *
   * @see The version of sync_send() with no timeout.
   * @tparam Rep
   *         See boost::chrono::duration documentation (and see above tip).
   * @tparam Period
   *         See boost::chrono::duration documentation (and see above tip).
   * @tparam Const_buffer_sequence
   *         See send().
   * @param data
   *        See send().
   * @param max_wait
   *        The maximum amount of time from now to wait before giving up on the wait and returning.
   *        `"duration<Rep, Period>::max()"` will eliminate the time limit and cause indefinite wait
   *        (i.e., no timeout).
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   *        Error, except `WAIT_INTERRUPTED` or `WAIT_USER_TIMEOUT`, implies that
   *        neither this send() nor any subsequent send() on this socket
   *        will succeeed.  (In particular a clean disconnect is an error.)
   * @return Number of bytes (possibly zero) added to Send buffer.  Always 0 if `bool(*err_code) == true`
   *         when sync_send() returns.
   */
  template<typename Rep, typename Period, typename Const_buffer_sequence>
  size_t sync_send(const Const_buffer_sequence& data,
                   const boost::chrono::duration<Rep, Period>& max_wait, Error_code* err_code = 0);

  /**
   * `sync_send()` operating in `nullptr_t` mode, wherein -- if Writable state is reached -- the actual data
   * are not moved out of any buffer, leaving that to the caller to do if desired.  Hence, this is a way of waiting
   * for Writable state that could be more concise in some situations than Event_set::sync_wait().
   *
   * ### Error handling ###
   * These are the possible outcomes:
   *   1. There is space in the Send buffer; and socket is fully connected
   *      (`S_OPEN+S_CONNECTED`).  Socket
   *      Writable.  `true` is returned; `*err_code` is set to success unless null.
   *   2. The operation cannot proceed due to an error.  `false` is returned; `*err_code` is set to the
   *      specific error unless null.  `*err_code == S_WAIT_INTERRUPTED` means the wait was
   *      interrupted (similarly to POSIX's `EINTR`).  (If `err_code` null, Runtime_error thrown.)
   *   3. Neither condition above is detected before the timeout expires (if provided).
   *      Output semantics are the same as in 2, with the specific code error::Code::S_WAIT_USER_TIMEOUT.
   *
   * Note that it is NOT possible to return `false` and no error.
   *
   * Tip: Typical types you might use for `max_wait`: `boost::chrono::milliseconds`,
   * `boost::chrono::seconds`, `boost::chrono::high_resolution_clock::duration`.
   *
   * @tparam Rep
   *         See other sync_send().
   * @tparam Period
   *         See other sync_send().
   * @param max_wait
   *        See other sync_receive().
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   *        Error, except `WAIT_INTERRUPTED` or `WAIT_USER_TIMEOUT`, implies that
   *        neither this nor any subsequent send() on this socket
   *        will succeeed.  (In particular a clean disconnect is an error.)
   * @return `true` if 1+ bytes are possible to add to Send buffer; `false` if either a timeout has occurred (bytes
   *         not writable), or another error has occurred.
   */
  template<typename Rep, typename Period>
  bool sync_send(nullptr_t,
                 const boost::chrono::duration<Rep, Period>& max_wait, Error_code* err_code = 0);

  /**
   * Equivalent to `sync_send(data, duration::max(), err_code)`; i.e., sync_send() with no timeout.
   *
   * @tparam Const_buffer_sequence
   *         See other sync_send().
   * @param data
   *        See other sync_send().
   * @param err_code
   *        See other sync_send().
   * @return See other sync_send().
   */
  template<typename Const_buffer_sequence>
  size_t sync_send(const Const_buffer_sequence& data, Error_code* err_code = 0);

  /**
   * Equivalent to `sync_send(nullptr, duration::max(), err_code)`; i.e., `sync_send(nullptr_t)`
   * with no timeout.
   *
   * @param err_code
   *        See other sync_receive().
   * @param tag
   *        Tag argument.
   * @return See other sync_receive().
   */
  bool sync_send(nullptr_t, Error_code* err_code = 0);

  /**
   * Receives (consumes from the Receive buffer) bytes of data, up to a given maximum
   * cumulative number of bytes as inferred from size of provided target buffer sequence.  The data
   * are copied into the user's structure and then removed from the Receive buffer.
   *
   * The method does not block.  In particular if there are no data already received from the other
   * side, we return no data.
   *
   * If the provided buffer has size zero, the method is a NOOP other than possibly logging.
   *
   * ### Error handling ###
   * These are the possible outcomes.
   *   1. There are no data in the Receive buffer.  Socket not Readable.  0 is returned;
   *      `*err_code` is set to success unless null; no data returned.
   *   2. The socket is not yet fully connected (`S_OPEN+S_CONNECTING`).  Socket not
   *      Readable.  0 is returned; `*err_code` is set to success unless null; no data returned.
   *   3. There are data in the Receive buffer; and socket is fully connected (`S_OPEN+S_CONNECTED`)
   *      or gracefully shutting down (`S_OPEN+S_DISCONNECTING`).  Socket Readable.  >= 1 is returned;
   *      *err_code is set to success; data returned.
   *   4. The operation cannot proceed due to an error.  0 is returned; `*err_code` is set to the
   *      specific error; no data buffered.  (If `err_code` null, Runtime_error thrown.)
   *
   * The semantics of -3- (the success case) are as follows.  N bytes will be copied from Receive
   * buffer beginning at the start of the `Mutable_buffer_sequence target`.  These N bytes may be
   * spread across 1 or more buffers in that sequence; the subdivision structure of the sequence of
   * bytes into buffers has no effect on the bytes, or order thereof, that will be moved from the
   * Receive buffer (e.g., `target` could be N+ 1-byte buffers, or one N+-byte buffer
   * -- the popped Receive buffer would be the same, as would be the extracted bytes).  N equals the
   * smaller of: the available bytes in the Receive buffer; and `buffer_size(target)`.  We return N.
   *
   * ### Reliability and ordering guarantees ###
   * See the send() doc header.
   *
   * @tparam Mutable_buffer_sequence
   *         Type that models the boost.asio `MutableBufferSequence` concept (see Boost docs).
   *         Basically, it's any container with elements convertible to `boost::asio::mutable_buffer`;
   *         and bidirectional iterator support.  Examples: `vector<mutable_buffer>`,
   *         `list<mutable_buffer>`.  Why allow `mutable_buffer` instead of, say, `Sequence` of bytes?
   *         Same reason as boost.asio's receive functions: it allows a great amount of flexibility
   *         without sacrificing performance, since `boost::asio::buffer()` function can adapt lots of
   *         different objects (arrays, `vector`s, `string`s, and more of bytes, integers, and more).
   * @param target
   *        Buffer sequence to which a stream of bytes to consume from Receive buffer will be
   *        written.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   *        Error implies that neither this receive() nor any subsequent receive() on this socket
   *        will succeeed.  (In particular a clean disconnect is an error.)
   * @return The number of bytes consumed (placed into `target`).  Always 0 if `bool(*err_code) == true`
   *         when receive() returns.
   */
  template<typename Mutable_buffer_sequence>
  size_t receive(const Mutable_buffer_sequence& target, Error_code* err_code = 0);

  /**
   * Blocking (synchronous) version of receive().  Acts just like receive(), except that if socket
   * is not immediately Readable (i.e., receive() would return 0 and no error), waits until it is
   * Readable (receive() would return either >0, or 0 and an error) and returns
   * `receive(target, err_code)`.  If a timeout is specified, and this timeout expires before socket is
   * Readable, it acts as if receive() produced error::Code::S_WAIT_USER_TIMEOUT.
   *
   * ### Error handling ###
   * These are the possible outcomes:
   *   1. There are data in the Receive buffer; and socket is fully connected
   *      (`S_OPEN+S_CONNECTED`) or gracefully shutting down (`S_OPEN+S_DISCONNECTING`).  Socket
   *      Readable.  >= 1 is returned; `*err_code` is set to success unless null; data returned.
   *   2. The operation cannot proceed due to an error.  0 is returned; `*err_code` is set to the
   *      specific error unless null; no data buffered.  `*err_code == S_WAIT_INTERRUPTED` means the wait was
   *      interrupted (similarly to POSIX's `EINTR`).  (If `err_code` null, Runtime_error thrown.)
   *   3. Neither condition above is detected before the timeout expires (if provided).
   *      Output semantics are the same as in 2, with the specific code error::Code::S_WAIT_USER_TIMEOUT.
   *
   * The semantics of -1- (the success case) equal those of receive().
   *
   * Note that it is NOT possible to return 0 and no error.
   *
   * Tip: Typical types you might use for `max_wait`: `boost::chrono::milliseconds`,
   * `boost::chrono::seconds`, `boost::chrono::high_resolution_clock::duration`.
   *
   * @see The version of sync_receive() with no timeout.
   * @tparam Rep
   *         See `boost::chrono::duration` documentation (and see above tip).
   * @tparam Period
   *         See `boost::chrono::duration` documentation (and see above tip).
   * @tparam Mutable_buffer_sequence
   *         See receive().
   * @param target
   *        See receive().
   * @param max_wait
   *        The maximum amount of time from now to wait before giving up on the wait and returning.
   *        `"duration<Rep, Period>::max()"` will eliminate the time limit and cause indefinite wait
   *        (i.e., no timeout).
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   *        Error, except `WAIT_INTERRUPTED` or `WAIT_USER_TIMEOUT`, implies that
   *        neither this receive() nor any subsequent receive() on this socket
   *        will succeeed.  (In particular a clean disconnect is an error.)
   * @return Number of bytes (possibly zero) added to target.  Always 0 if `bool(*err_code) == true` when
   *         sync_receive() returns.
   */
  template<typename Rep, typename Period, typename Mutable_buffer_sequence>
  size_t sync_receive(const Mutable_buffer_sequence& target,
                      const boost::chrono::duration<Rep, Period>& max_wait, Error_code* err_code = 0);

  /**
   * `sync_receive()` operating in `nullptr_t` mode, wherein -- if Readable state is reached -- the actual data
   * are not moved into any buffer, leaving that to the caller to do if desired.  Hence, this is a way of waiting
   * for Readable state that could be more concise in some situations than Event_set::sync_wait().
   *
   * ### Error handling ###
   * These are the possible outcomes:
   *   1. There are data in the Receive buffer; and socket is fully connected
   *      (`S_OPEN+S_CONNECTED`) or gracefully shutting down (`S_OPEN+S_DISCONNECTING`).  Socket
   *      Readable.  `true` is returned; `*err_code` is set to success unless null.
   *   2. The operation cannot proceed due to an error.  `false` is returned; `*err_code` is set to the
   *      specific error unless null.  `*err_code == S_WAIT_INTERRUPTED` means the wait was
   *      interrupted (similarly to POSIX's `EINTR`).  (If `err_code` null, Runtime_error thrown.)
   *   3. Neither condition above is detected before the timeout expires (if provided).
   *      Output semantics are the same as in 2, with the specific code error::Code::S_WAIT_USER_TIMEOUT.
   *
   * Note that it is NOT possible to return `false` and no error.
   *
   * Tip: Typical types you might use for `max_wait`: `boost::chrono::milliseconds`,
   * `boost::chrono::seconds`, `boost::chrono::high_resolution_clock::duration`.
   *
   * @tparam Rep
   *         See other sync_receive().
   * @tparam Period
   *         See other sync_receive().
   * @param max_wait
   *        See other sync_receive().
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   *        Error, except `WAIT_INTERRUPTED` or `WAIT_USER_TIMEOUT`, implies that
   *        neither this nor any subsequent receive() on this socket
   *        will succeeed.  (In particular a clean disconnect is an error.)
   * @return `true` if there are 1+ bytes ready to read; `false` if either a timeout has occurred (no bytes ready), or
   *         another error has occurred.
   */
  template<typename Rep, typename Period>
  bool sync_receive(nullptr_t,
                    const boost::chrono::duration<Rep, Period>& max_wait, Error_code* err_code = 0);

  /**
   * Equivalent to `sync_receive(target, duration::max(), err_code)`; i.e., sync_receive()
   * with no timeout.
   *
   * @tparam Mutable_buffer_sequence
   *         See other sync_receive().
   * @param target
   *        See other sync_receive().
   * @param err_code
   *        See other sync_receive().
   * @return See other sync_receive().
   */
  template<typename Mutable_buffer_sequence>
  size_t sync_receive(const Mutable_buffer_sequence& target, Error_code* err_code = 0);

  /**
   * Equivalent to `sync_receive(nullptr, duration::max(), err_code)`; i.e., `sync_receive(nullptr_t)`
   * with no timeout.
   *
   * @param err_code
   *        See other sync_receive().
   * @param tag
   *        Tag argument.
   * @return See other sync_receive().
   */
  bool sync_receive(nullptr_t, Error_code* err_code = 0);

  /**
   * Acts as if fatal error error::Code::S_USER_CLOSED_ABRUPTLY has been discovered on the
   * connection.  Does not block.
   *
   * Post-condition: `state() == State::S_CLOSED`.  Additionally, assuming no loss on the
   * network, the other side will close the connection with error
   * error::Code::S_CONN_RESET_BY_OTHER_SIDE.
   *
   * Note: Discovering a fatal error on the connection would trigger all event waits on this socket
   * (sync_send(), sync_receive(), Event_set::sync_wait(), Event_set::async_wait()) to execute on-event
   * behavior (return, return, return, invoke handler, respectively).  Therefore this method will cause
   * just that, if applicable.
   *
   * Note: As a corollary, a socket closing this way (or any other way) does NOT cause that socket's
   * events (if any) to be removed from any Event_set objects.  Clearing an Event_set of all or some
   * sockets is the Event_set user's responsibility (the classic way being Event_set::close()).
   *
   * @warning The moment the other side is informed we have abruptly closed the connection, they
   * will no longer be able to receive() any of it (even if data had been queued up in
   * their Receive buffer).
   *
   * @todo Currently this close_abruptly() is the only way for the user to explicitly close one specified socket.
   * All other ways are due to error (or other side starting graceful shutdown, once we
   * implement that).  Once we implement graceful close, via `close_start()` and `close_final()`,
   * use of close_abruptly() should be discouraged, or it may even be deprecated (e.g.,
   * `Node`s lack a way to initiate an abrupt close for a specific socket).
   *
   * @todo close_abruptly() return `bool` (`false` on failure)?
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_NODE_NOT_RUNNING, or -- if socket already closed (`state() == State::S_CLOSED`) --
   *        then the error that caused the closure.
   */
  void close_abruptly(Error_code* err_code = 0);

  /**
   * Dynamically replaces the current options set (options()) with the given options set.
   * Only those members of `opts` designated as dynamic (as opposed to static) may be different
   * between options() and `opts`.  If this is violated, it is an error, and no options are changed.
   *
   * Typically one would acquire a copy of the existing options set via options(), modify the
   * desired dynamic data members of that copy, and then apply that copy back by calling
   * set_options().
   *
   * @param opts
   *        The new options to apply to this socket.  It is copied; no reference is saved.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.  Generated codes:
   *        error::Code::S_STATIC_OPTION_CHANGED, error::Code::S_OPTION_CHECK_FAILED,
   *        error::Code::S_NODE_NOT_RUNNING.
   * @return `true` on success, `false` on error.
   */
  bool set_options(const Peer_socket_options& opts, Error_code* err_code = 0);

  /**
   * Copies this socket's option set and returns that copy.  If you intend to use set_options() to
   * modify a socket's options, we recommend you make the modifications on the copy returned by
   * options().
   *
   * @todo Provide a similar options() method that loads an existing structure (for structure
   * reuse).
   *
   * @return See above.
   */
  Peer_socket_options options() const;

  /**
   * Returns a structure containing the most up-to-date stats about this connection.
   *
   * @note At the cost of reducing locking overhead in 99.999999% of the Peer_socket's operation,
   *       this method may take a bit of time to run.  It's still probably only 10 times or so slower than
   *       a simple lock, work, unlock -- there is a condition variable and stuff involved -- but this may
   *       matter if done very frequently.  So you probably should not.  (Hmmm... where did I get these estimates,
   *       namely "10 times or so"?)
   *
   * @todo Provide a similar info() method that loads an existing structure (for structure
   * reuse).
   *
   * @return See above.
   */
  Peer_socket_info info() const;

  /**
   * The maximum number of bytes of user data per received or sent packet on this connection.  See
   * Peer_socket_options::m_st_max_block_size.  Note that this method is ESSENTIAL when using the
   * socket in unreliable mode (assuming you want to implement reliability outside of `net_flow`).
   *
   * @return Ditto.
   */
  size_t max_block_size() const;

  /**
   * The error code that perviously caused state() to become State::S_CLOSED, or success code if state
   * is not CLOSED.  For example, error::code::S_CONN_RESET_BY_OTHER_SIDE (if was connected) or
   * error::Code::S_CONN_TIMEOUT (if was connecting)
   *
   * @return Ditto.
   */
  Error_code disconnect_cause() const;

protected:
  // Constructors/destructor.

  /**
   * Constructs object; initializes most values to well-defined (0, empty, etc.) but not necessarily
   * meaningful values.
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param task_engine
   *        IO service for the timer(s) stored as data member(s).
   * @param opts
   *        The options set to copy into this Peer_socket and use subsequently.
   */
  explicit Peer_socket(log::Logger* logger_ptr,
                       util::Task_engine* task_engine,
                       const Peer_socket_options& opts);

private:
  // Friends.

  /**
   * See rationale for `friend`ing Node in class Peer_socket documentation header.
   * @see Node.
   */
  friend class Node;
  /**
   * See rationale for `friend`ing Server_socket in class Peer_socket documentation header.
   * @see Server_socket.
   */
  friend class Server_socket;
  /**
   * For access to `Sent_pkt_by_sent_when_map` and Sent_packet types, at least.
   * (Drop_timer has no actual Peer_socket instance to mess with.)
   */
  friend class Drop_timer;
  /**
   * Stats modules have const access to all socket internals.
   * @see Send_bandwidth_estimator.
   */
  friend class Send_bandwidth_estimator;
  /**
   * Congestion control modules have const access to all socket internals.
   * @see Congestion_control_classic_data.
   */
  friend class Congestion_control_classic_data;
  /**
   * Congestion control modules have const access to all socket internals.
   * @see Congestion_control_classic.
   */
  friend class Congestion_control_classic;
  /**
   * Congestion control modules have const access to all socket internals.
   * @see Congestion_control_classic_with_bandwidth_est.
   */
  friend class Congestion_control_classic_with_bandwidth_est;

  // Types.

  /// Short-hand for `shared_ptr` to immutable Drop_timer (can't use Drop_timer::Ptr due to C++ and circular reference).
  using Drop_timer_ptr = boost::shared_ptr<Drop_timer>;

  /**
   * Short-hand for high-performance, non-reentrant, exclusive mutex used to lock #m_opts.
   *
   * ### Rationale ###
   * You might notice this seems tailor-made for shared/exclusive (a/k/a multiple-readers-single-writer) mutex.
   * Why a 2-level mutex instead of a normal exclusive mutex?  Because options can be accessed by
   * thread W and various user threads, in the vast majority of the time to read option values.  On
   * the other hand, rarely, #m_opts may be modified via set_options().  To avoid thread contention
   * when no one is writing (which is usual), we could use that 2-level type of mutex and apply the appropriate
   * (shared or unique) lock depending on the situation.  So why not?  Answer:
   * While a shared/exclusive mutex sounds lovely in theory -- and perhaps
   * if its implementation were closer to the hardware it would be lovely indeed -- in practice it seems its
   * implementation just causes performance problems rather than solving them.  Apparently that's why
   * it was rejected by C++11 standards people w/r/t inclusion in that standard.  The people involved
   * explained their decision here: http://permalink.gmane.org/gmane.comp.lib.boost.devel/211180.
   * So until that is improved, just do this.  I'm not even adding a to-do for fixing this, as that seems
   * unlikely anytime soon.  Update: C++17 added `std::shared_mutex`, and C++14 added a similar thing named
   * something else.  Seems like a good time to revisit this -- if not to materially improve #Options_mutex
   * performance then to gain up-to-date knowledge on the topic, specifically whether `shared_mutex` is fast now.
   * Update: Apparently as of Boost-1.80 the Boost.thread impl of `shared_mutex` is lacking in perf, and there
   * is a ticket filed for many years for this.  Perhaps gcc `std::shared_mutex` is fine.  However research
   * suggests it's less about this nitty-gritty of various impls and more the following bottom line:
   * A simple mutex is *very* fast to lock/unlock, and perf problems occur only if one must wait for a lock.
   * Experts say that it is possible but quite rare that there is enough lock contention to make it "worth it":
   * a shared mutex is *much* slower to lock/unlock sans contention.  Only when the read critical sections are
   * long and very frequently accessed does it become "worth it."
   */
  using Options_mutex = util::Mutex_non_recursive;

  /// Short-hand for lock that acquires exclusive access to an #Options_mutex.
  using Options_lock = util::Lock_guard<Options_mutex>;

  /**
   * Short-hand for reentrant mutex type.  We explicitly rely on reentrant behavior, so this isn't "just in case."
   *
   * @todo This doc header for Peer_socket::Mutex should specify what specific behavior requires mutex reentrance, so
   * that for example one could reevaluate whether there's a sleeker code pattern that would avoid it.
   */
  using Mutex = util::Mutex_recursive;

  /// Short-hand for RAII lock guard of #Mutex.
  using Lock_guard = util::Lock_guard<Mutex>;

  /// Type used for #m_security_token.
  using security_token_t = uint64_t;

  /// Short-hand for order number type.  0 is reserved.  Caution: Keep in sync with Drop_timer::packet_id_t.
  using order_num_t = Sequence_number::seq_num_t;

  /**
   * The state of the socket (and the connection from this end's point of view) for the internal state
   * machine governing the operation of the socket.
   *
   * @todo Peer_socket::Int_state will also include various states on way to a graceful close, once we implement that.
   */
  enum class Int_state
  {
    /// Closed (dead or new) socket.
    S_CLOSED,

    /**
     * Public state is OPEN+CONNECTING; user requested active connect; we sent SYN and are
     * awaiting response.
     */
    S_SYN_SENT,

    /**
     * Public state is OPEN+CONNECTING; other side requested passive connect via SYN; we sent
     * SYN_ACK and are awaiting response.
     */
    S_SYN_RCVD,

    /// Public state is OPEN+CONNECTED; in our opinion the connection is established.
    S_ESTABLISHED
  }; // enum class Int_state

  // Friend of Peer_socket: For access to private alias Int_state.
  friend std::ostream& operator<<(std::ostream& os, Int_state state);

  struct Sent_packet;

  /// Short-hand for #m_snd_flying_pkts_by_sent_when type; see that data member.
  using Sent_pkt_by_sent_when_map = util::Linked_hash_map<Sequence_number, boost::shared_ptr<Sent_packet>>;

  /// Short-hand for #m_snd_flying_pkts_by_sent_when `const` iterator type.
  using Sent_pkt_ordered_by_when_const_iter = Sent_pkt_by_sent_when_map::const_iterator;

  /// Short-hand for #m_snd_flying_pkts_by_sent_when iterator type.
  using Sent_pkt_ordered_by_when_iter = Sent_pkt_by_sent_when_map::iterator;

  /// Short-hand for #m_snd_flying_pkts_by_seq_num type; see that data member.
  using Sent_pkt_by_seq_num_map = std::map<Sequence_number, Sent_pkt_ordered_by_when_iter>;

  /// Short-hand for #m_snd_flying_pkts_by_seq_num `const` iterator type.
  using Sent_pkt_ordered_by_seq_const_iter = Sent_pkt_by_seq_num_map::const_iterator;

  /// Short-hand for #m_snd_flying_pkts_by_seq_num iterator type.
  using Sent_pkt_ordered_by_seq_iter = Sent_pkt_by_seq_num_map::iterator;

  struct Received_packet;

  /**
   * Short-hand for #m_rcv_packets_with_gaps type; see that data member.  `struct`s are stored via
   * shared pointers instead of as direct objects to minimize copying of potentially heavy-weight
   * data.  They are stored as shared pointers instead of as raw pointers to avoid having to
   * worry about `delete`.
   */
  using Recvd_pkt_map = std::map<Sequence_number, boost::shared_ptr<Received_packet>>;

  /// Short-hand for #m_rcv_packets_with_gaps `const` iterator type.
  using Recvd_pkt_const_iter = Recvd_pkt_map::const_iterator;

  /// Short-hand for #m_rcv_packets_with_gaps iterator type.
  using Recvd_pkt_iter = Recvd_pkt_map::iterator;

  struct Individual_ack;

  /**
   * Type used for #m_rcv_syn_rcvd_data_q.  Using `vector` because we only need `push_back()` and
   * iteration at the moment.  Using pointer to non-`const` instead of `const` because when we actually handle the
   * packet as received we will need to be able to modify the packet for performance (see
   * Node::handle_data_to_established(), when it transfers data to Receive buffer).
   */
  using Rcv_syn_rcvd_data_q = std::vector<boost::shared_ptr<Data_packet>>;

  /* Methods: as few as possible.  Logic should reside in class Node as discussed (though
   * implementation may be in peer_socket.cpp).  See rationale in class Peer_socket
   * documentation header. */

  /**
   * Non-template helper for template send() that forwards the send() logic to Node::send().  Would
   * be pointless to try to explain more here; see code and how it's used.  Anyway, this has to be
   * in this class.
   *
   * @param snd_buf_feed_func
   *        Function that will perform and return `m_snd_buf->feed(...)`.  See send().
   * @param err_code
   *        See send().
   * @return Value to be returned by calling Node::send().
   */
  size_t node_send(const Function<size_t (size_t max_data_size)>& snd_buf_feed_func,
                   Error_code* err_code);

  /**
   * Same as sync_send() but uses a #Fine_clock-based #Fine_duration non-template type for
   * implementation convenience and to avoid code bloat to specify timeout.
   *
   * @tparam Const_buffer_sequence
   *         See sync_send().
   * @param data
   *        See sync_send().
   * @param wait_until
   *        See `sync_send(timeout)`.  This is the absolute time point corresponding to that.
   *        `"duration<Rep, Period>::max()"` maps to the value `Fine_time_pt()` (Epoch) for this argument.
   * @param err_code
   *        See sync_send().
   * @return See sync_send().
   */
  template<typename Const_buffer_sequence>
  size_t sync_send_impl(const Const_buffer_sequence& data, const Fine_time_pt& wait_until,
                        Error_code* err_code);

  /**
   * Helper similar to sync_send_impl() but for the `nullptr_t` versions of `sync_send()`.
   *
   * @param wait_until
   *        See sync_send_impl().
   * @param err_code
   *        See sync_send_impl().
   * @return See `sync_send(nullptr_t)`.  `true` if and only if Writable status successfuly reached in time.
   */
  bool sync_send_reactor_pattern_impl(const Fine_time_pt& wait_until, Error_code* err_code);

  /**
   * This is to sync_send() as node_send() is to send().
   *
   * @param snd_buf_feed_func_or_empty
   *        See node_send().  Additionally, if this is `.empty()` then `nullptr_t` a/k/a "reactor pattern" mode is
   *        engaged.
   * @param wait_until
   *        See sync_send_impl().
   * @param err_code
   *        See sync_send().
   * @return See sync_send().
   */
  size_t node_sync_send(const Function<size_t (size_t max_data_size)>& snd_buf_feed_func_or_empty,
                        const Fine_time_pt& wait_until,
                        Error_code* err_code);

  /**
   * Non-template helper for template receive() that forwards the receive() logic to
   * Node::receive().  Would be pointless to try to explain more here; see code and how it's used.
   * Anyway, this has to be in this class.
   *
   * @param rcv_buf_consume_func
   *        Function that will perform and return `m_rcv_buf->consume(...)`.  See receive().
   * @param err_code
   *        See receive().
   * @return Value to be returned by calling Node::receive().
   */
  size_t node_receive(const Function<size_t ()>& rcv_buf_consume_func,
                      Error_code* err_code);

  /**
   * Same as sync_receive() but uses a #Fine_clock-based #Fine_duration non-template type
   * for implementation convenience and to avoid code bloat to specify timeout.
   *
   * @tparam Block_sequence
   *         See sync_receive().
   * @param target
   *        See sync_receive().
   * @param wait_until
   *        See `sync_receive(timeout)`.  This is the absolute time point corresponding to that.
   *        `"duration<Rep, Period>::max()"` maps to the value `Fine_time_pt()` (Epoch) for this argument.
   * @param err_code
   *        See sync_receive().
   * @return See sync_receive().
   */
  template<typename Mutable_buffer_sequence>
  size_t sync_receive_impl(const Mutable_buffer_sequence& target,
                           const Fine_time_pt& wait_until, Error_code* err_code);

  /**
   * Helper similar to sync_receive_impl() but for the `nullptr_t` versions of `sync_receive()`.
   *
   * @param wait_until
   *        See sync_receive_impl().
   * @param err_code
   *        See sync_receive_impl().
   * @return See `sync_receive(nullptr_t)`.  `true` if and only if Readable status successfuly reached in time.
   */
  bool sync_receive_reactor_pattern_impl(const Fine_time_pt& wait_until, Error_code* err_code);

  /**
   * This is to sync_receive() as node_receive() is to receive().
   *
   * @param rcv_buf_consume_func_or_empty
   *        See node_receive().  Additionally, if this is `.empty()` then `nullptr_t` a/k/a "reactor pattern" mode is
   *        engaged.
   * @param wait_until
   *        See sync_receive_impl().
   * @param err_code
   *        See sync_receive().
   * @return See sync_receive().
   */
  size_t node_sync_receive(const Function<size_t ()>& rcv_buf_consume_func_or_empty,
                           const Fine_time_pt& wait_until,
                           Error_code* err_code);

  /**
   * Analogous to Node::opt() but for per-socket options.  See that method.
   *
   * Do NOT read option values without opt().
   *
   * @tparam Opt_type
   *         The type of the option data member.
   * @param opt_val_ref
   *        A reference (important!) to the value you want; this may be either a data member of
   *        this->m_opts or the entire this->m_opts itself.
   * @return A copy of the value at the given reference.
   */
  template<typename Opt_type>
  Opt_type opt(const Opt_type& opt_val_ref) const;

  /**
   * Returns the smallest multiple of max_block_size() that is >= the given option value, optionally
   * first inflated by a certain %.  The intended use case is to obtain a Send of Receive buffer max
   * size that is about equal to the user-specified (or otherwise obtained) value, in bytes, but is
   * a multiple of max-block-size -- to prevent fragmenting max-block-size-sized chunks of data unnecessarily -- and
   * to possibly inflate that value by a certain percentage for subtle flow control reasons.
   *
   * @param opt_val_ref
   *        A reference to a `size_t`-sized socket option, as would be passed to opt().  See opt().
   *        This is the starting value.
   * @param inflate_pct_val_ptr
   *        A pointer to an `unsigned int`-sized socket option, as would be passed to opt().  See
   *        opt().  This is the % by which to inflate opt_val_ref before rounding up to nearest
   *        max_block_size() multiple.  If null, the % is assumed to be 0.
   * @return See above.
   */
  size_t max_block_size_multiple(const size_t& opt_val_ref,
                                 const unsigned int* inflate_pct_val_ptr = 0) const;

  /**
   * Whether retransmission is enabled on this connection.  Short-hand for appropriate opt() call.
   * Note this always returns the same value for a given object.
   *
   * @return Ditto.
   */
  bool rexmit_on() const;

  /**
   * Helper that is equivalent to Node::ensure_sock_open(this, err_code).  Used by templated
   * methods which must be defined in this header file, which means they cannot access Node members
   * directly, as Node is an incomplete type.
   *
   * @param err_code
   *        See Node::ensure_sock_open().
   * @return See Node::ensure_sock_open().
   */
  bool ensure_open(Error_code* err_code) const;

  /**
   * Helper that, given a byte count, returns a string with that byte count and the number of
   * max_block_size()-size blocks that can fit within it (rounded down).
   *
   * @param bytes
   * @return See above.
   */
  std::string bytes_blocks_str(size_t bytes) const;

  // Data.

  /**
   * This socket's per-socket set of options.  Initialized at construction; can be subsequently
   * modified by set_options(), although only the dynamic members of this may be modified.
   *
   * Accessed from thread W and user thread U != W.  Protected by #m_opts_mutex.  When reading, do
   * NOT access without locking (which is encapsulated in opt()).
   */
  Peer_socket_options m_opts;

  /// The mutex protecting #m_opts.
  mutable Options_mutex m_opts_mutex;

  /// `true` if we connect() to server; `false` if we are to be/are `accept()`ed.  Should be set once and not modified.
  bool m_active_connect;

  /**
   * See state().  Should be set before user gets access to `*this`.  Must not be modified by non-W threads after that.
   *
   * Accessed from thread W and user threads U != W (in state() and others).  Must be protected
   * by mutex.
   */
  State m_state;

  /**
   * See state().  Should be set before user gets access to `*this`.  Must not be modified by non-W
   * threads after that.
   *
   * Accessed from thread W and user threads U != W (in state() and others).  Must be protected by
   * mutex.
   */
  Open_sub_state m_open_sub_state;

  /**
   * See node().  Should be set before user gets access to `*this` and not changed, except to null when
   * state is State::S_CLOSED.  Must not be modified by non-W threads.
   *
   * Invariant: `x->node() == y` if and only if `y->m_socks` contains `x`; otherwise `!x->node()`.
   * The invariant must hold by the end of the execution of any thread W boost.asio handler (but
   * not necessarily at all points within that handler, or generally).
   *
   * Accessed from thread W and user threads U != W (in node() and others).  Must be protected by
   * mutex.
   *
   * @todo `boost::weak_ptr<Node>` would be ideal for this, but of course then Node would have to
   * (only?) be available via shared_ptr<>.
   */
  Node* m_node;

  /**
   * For sockets that come a Server_socket, this is the inverse of Server_socket::m_connecting_socks: it is
   * the Server_socket from which this Peer_socket will be Server_socket::accept()ed (if that succeeds); or null if
   * this is an actively-connecting Peer_socket or has already been `accept()`ed.
   *
   * More formally, this is null if #m_active_connect; null if not the case but already accept()ed; and otherwise:
   * `((y->m_connecting_socks contains x) || (y->m_unaccepted_socks contains x))` if and only if
   * `x->m_originating_serv == y`.  That is, for a socket in state Int_state::S_SYN_RCVD, or in state
   * Int_state::S_ESTABLISHED, but before being accept()ed by the user, this is the server socket that spawned this
   * peer socket.
   *
   * ### Thread safety ###
   * This can be write-accessed simultaneously by thread W (e.g., when closing a
   * socket before it is accepted) and a user thread U != W (in Server_socket::accept()).  It is
   * thus protected by a mutex -- but it's Server_socket::m_mutex, not Peer_socket::m_mutex.  I
   * know it's weird, but it makes sense.  Basically Server_socket::m_unaccepted_socks and
   * Server_socket::m_originating_serv -- for each element of `m_unaccepted_socks` -- are modified together
   * in a synchronized way.
   *
   * @see Server_socket::m_connecting_socks and Server_socket::m_unaccepted_socks for the closely
   *      related inverse.
   */
  Server_socket::Ptr m_originating_serv;

  /**
   * The Receive buffer; Node feeds data at the back; user consumes data at the front.  Contains
   * application-layer data received from the other side, to be read by user via receive() and
   * similar.
   *
   * A maximum cumulative byte count is maintained.  If data are received that would exceed this max
   * (i.e., the user is not retrieving the data fast enough to keep up), these data are dropped (and
   * if we use ACKs would be eventually treated as dropped by the other side).
   *
   * Note that this is a high-level structure, near the application layer.  This does not store any
   * metadata, like sequence numbers, or data not ready to be consumed by the user (such as
   * out-of-order packets, if we implement that).  Such packets and data should be stored elsewhere.
   *
   * ### Thread safety ###
   * This can be write-accessed simultaneously by thread W (when receiving by Node)
   * and a user thread U != W (in receive(), etc.).  It is thus protected by a mutex.
   */
  Socket_buffer m_rcv_buf;

  /**
   * The Send buffer; user feeds data at the back; Node consumes data at the front.  Contains
   * application-layer data to be sent to the other side as supplied by user via send() and friends.
   *
   * A maximum cumulative byte count is maintained.  If data are supplied that would exceed
   * this max (i.e., the Node is not sending the data fast enough to keep up), send() will
   * inform the caller that fewer bytes than intended have been buffered.  Typically this happens if
   * the congestion control window is full, so data are getting buffered in #m_snd_buf instead of
   * being immediately consumed and sent.
   *
   * Note that this is a high-level structure, near the application layer.  This does not store any
   * metadata, like sequence numbers, or data not ready to be consumed by the user (such as
   * out-of-order packets, if we implement that).  Such packets and data should be stored elsewhere.
   *
   * Thread safety: Analogous to #m_rcv_buf.
   */
  Socket_buffer m_snd_buf;

  /**
   * The #Error_code causing disconnection (if one has occurred or is occurring)
   * on this socket; otherwise a clear (success) #Error_code.  This starts as success
   * and may move to one non-success value and then never change after that.  Graceful connection
   * termination is (unlike in BSD sockets, where this is indicated with receive() returning 0, not an
   * error) indeed counted as a non-success value for #m_disconnect_cause.
   *
   * Exception: if, during graceful close, the connection must be closed abruptly (due to error,
   * including error::Code::S_USER_CLOSED_ABRUPTLY), #m_disconnect_cause may change a second time (from "graceful close"
   * to "abrupt closure").
   *
   * As in TCP net-stacks, one cannot recover from a transmission error or termination on the socket
   * (fake "error" `EWOULDBLOCK`/`EAGAIN` excepted), which is why this can only go success ->
   * non-success and never change after that.
   *
   * How to report this to the user: attempting to `*receive()` when not Readable while
   * #m_disconnect_cause is not success => `*receive()` returns this #Error_code to the user; and
   * similarly for `*send()` and Writable.
   *
   * I emphasize that this should happen only after Receive buffer has been emptied; otherwise user
   * will not be able to read queued up received data after the Node internally detects connection
   * termination.  By the same token, if the Node can still reasonably send data to the other side,
   * and Send buffer is not empty, and #m_disconnect_cause is not success, the Node should only halt
   * the packet sending once Send buffer has been emptied.
   *
   * This should be success in all states except State::S_CLOSED and
   * State::S_OPEN + Open_sub_state::S_DISCONNECTING.
   *
   * ### Thread safety ###
   * Since user threads will access this at least via receive() and send(), while
   * thread W may set it having detected disconnection, this must be protected by a mutex.
   */
  Error_code m_disconnect_cause;

  /**
   * If `!m_active_connect`, this contains the serialized metadata that the user supplied on
   * the other side when initiating the connect; otherwise this is the serialized metadata that the user
   * supplied on this side when initiating the connect.  In either case (though obviously more
   * useful in the `!m_active_connect` case) it can be obtained via get_connect_metadata().
   * In the `m_active_connect` case, this is also needed if we must re-send the original SYN
   * (retransmission).
   *
   * ### Thread safety ###
   * Same as #m_snd_buf and #m_rcv_buf (protected by #m_mutex).  This would not be
   * necessary, since this value is immutable once user gets access to `*this`, and threads other than
   * W can access it, but sock_free_memory() does clear it while the user may be accessing it.  Due
   * to that caveat, we have to lock it.
   */
  util::Blob m_serialized_metadata;

  /**
   * This object's mutex.  The protected items are #m_state, #m_open_sub_state, #m_disconnect_cause,
   * #m_node, #m_rcv_buf, #m_snd_buf, #m_serialized_metadata.
   *
   * Generally speaking, if 2 or more of the protected variables must be changed in the same
   * non-blocking "operation" (for some reasonable definition of "operation"), they should probably
   * be changed within the same #m_mutex-locking critical section.  For example, if closing the
   * socket in thread W due to an incoming RST, one should lock #m_mutex, clear both buffers, set
   * #m_disconnect_cause, change `m_state = State::S_CLOSED`, and then unlock #m_mutex.  Then thread U != W
   * will observe all this state changed at the "same time," which is desirable.
   */
  mutable Mutex m_mutex;

  /// See remote_endpoint().  Should be set before user gets access to `*this` and not changed afterwards.
  Remote_endpoint m_remote_endpoint;

  /// See local_port().  Should be set before user gets access to `*this` and not changed afterwards.
  flow_port_t m_local_port;

  /**
   * Current internal state of the socket.  Note this is a very central piece of information and is analogous
   * to TCP's "state" (ESTABLISHED, etc. etc.).
   *
   * This gains meaning only in thread W.  This should NOT be
   * accessed outside of thread W and is not protected by a mutex.
   */
  Int_state m_int_state;

  /**
   * The queue of DATA packets received while in Int_state::S_SYN_RCVD state before the
   * Syn_ack_ack_packet arrives to move us to Int_state::S_ESTABLISHED
   * state, at which point these packets can be processed normally.  Such
   * DATA packets would not normally exist, but they can exist if the SYN_ACK_ACK is lost or DATA
   * packets are re-ordered to go ahead of it.  See Node::handle_data_to_syn_rcvd() for more
   * detail.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W
   * and is not protected by a mutex.
   */
  Rcv_syn_rcvd_data_q m_rcv_syn_rcvd_data_q;

  /**
   * The running total count of bytes in the `m_data` fields of #m_rcv_syn_rcvd_data_q.  Undefined
   * when the latter is empty.  Used to limit its size.  This gains meaning only in thread W.  This
   * should NOT be accessed outside of thread W and is not protected by a mutex.
   */
  size_t m_rcv_syn_rcvd_data_cumulative_size;

  /**
   * The Initial Sequence Number (ISN) contained in the original Syn_packet or
   * Syn_ack_packet we received.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.  Useful at least in verifying the validity of duplicate SYNs and
   * SYN_ACKs.
   */
  Sequence_number m_rcv_init_seq_num;

  /**
   * The maximal sequence number R from the remote side such that all data with sequence numbers
   * strictly less than R in this connection have been received by us and placed into the Receive
   * buffer.  This first gains meaning upon receiving SYN and is the sequence number of that SYN,
   * plus one (as in TCP); or upon receiving SYN_ACK (similarly).  Note that received packets past
   * this sequence number may exist, but if so there is at least one missing packet (the one at
   * #m_rcv_next_seq_num) preceding all of them.
   *
   * @see #m_rcv_packets_with_gaps.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  Sequence_number m_rcv_next_seq_num;

  /**
   * The sequence-number-ordered collection of all
   * received-and-not-dropped-due-to-buffer-overflow packets such that at least
   * one unreceived-or-otherwise-unknown datum precedes all sequence numbers in this collection;
   * a/k/a the reassembly queue if retransmission is enabled.
   * With retransmission off, the only purpose of keeping this structure at all is to detect any
   * already-received-and-given-to-Receive-buffer packet coming in again; such a packet should be
   * ACKed but NOT given to the Receive buffer again (avoid data duplication).  With retransmission
   * on, this is additionally used as the reassembly queue (storing the non-contiguous data until
   * the gaps are filled in).
   *
   * The structure is best explained by breaking down the sequence number space.  I list the
   * sequence number ranges in increasing order starting with the ISN.  Let `last_rcv_seq_num` be the
   * sequence number of the last datum to have been received (and not dropped due to insufficient
   * Receive buffer space), for exposition purposes.
   *
   *   - #m_rcv_init_seq_num =
   *     - SYN or SYN_ACK
   *
   *   - [`m_rcv_init_seq_num + 1`, `m_rcv_next_seq_num - 1`] =
   *     - Largest possible range of sequence numbers such that each datum represented by this range
   *       has been received (and not dropped due to insufficient Receive buffer space) and copied to
   *       the Receive buffer for user retrieval.
   *
   *   - [#m_rcv_next_seq_num, `m_rcv_next_seq_num + N - 1`] =
   *     - The first packet after the ISN that has not yet been received (or has been received but has
   *       been dropped due to insufficient Receive buffer space).  `N` is the (unknown to us) length of
   *       that packet.  `N` > 0.  This can be seen as the first "gap" in the received sequence number
   *       space.
   *
   *   - [`m_rcv_next_seq_num + N`, `last_rcv_seq_num`] =
   *     - The remaining packets up to and including the last byte that has been received (and not
   *       dropped due to insufficient Receive buffer space).  Each packet in this range is one of the
   *       following:
   *       - received (and not dropped due to insufficient Receive buffer space);
   *       - not received (or received and dropped due to insufficient Receive buffer space).
   *
   *   - [`last_rcv_seq_num + 1`, ...] =
   *     - All remaining not-yet-received (or received but dropped due to insufficient Receive buffer
   *       space) packets.
   *
   * #m_rcv_packets_with_gaps contains all Received_packets in the range [`m_rcv_next_seq_num + N`,
   * `last_rcv_seq_num`], with each particular Received_packet's first sequence number as its key.  If
   * there are no gaps -- all received sequence numbers are followed by unreceived sequence numbers
   * -- then that range is empty and so is #m_rcv_packets_with_gaps.  All the other ranges can be
   * null (empty) as well.  If there are no received-and-undropped packets, then `m_rcv_init_seq_num
   * == m_rcv_next_seq_num`, which is the initial situation.
   *
   * The above is an invariant, to be true at the end of each boost.asio handler in thread W, at
   * least.
   *
   * Each received-and-undropped packet therefore is placed into #m_rcv_packets_with_gaps, anywhere
   * in the middle.  If retransmission is off, the data in the packet is added to Receive buffer.
   * If retransmission is on, the data in the packet is NOT added to Receive buffer but instead
   * saved within the structure for later reassembly (see next paragraph).
   *
   * If the [#m_rcv_next_seq_num, ...] (first gap) packet is received-and-not-dropped, then
   * #m_rcv_next_seq_num is incremented by N (the length of that packet), filling the gap.  Moreover,
   * any contiguous packets at the front of #m_rcv_packets_with_gaps, assuming the first packet's
   * sequence number equals #m_rcv_next_seq_num, must be removed from #m_rcv_packets_with_gaps, and
   * #m_rcv_next_seq_num should be incremented accordingly.  All of this maintains the invariant.  If
   * retransmission is on, the data in the byte sequence formed by this operation is to be placed
   * (in sequence number order) into the Receive buffer (a/k/a reassembly).
   *
   * Conceptually, this is the action of receiving a gap packet which links up following
   * already-received packets to previous already-received packets, which means all of these can go
   * away, as the window slides forward beyond them.
   *
   * If a packet arrives and is already in #m_rcv_packets_with_gaps, then it is a duplicate and is
   * NOT placed on the Receive buffer.  The same holds for any packet with sequence numbers
   * preceding #m_rcv_next_seq_num.
   *
   * The type used is a map sorted by starting sequence number of each packet.  Why?  We need pretty
   * fast middle operations, inserting and checking for existence of arriving packet.  We need fast
   * access to the earliest (smallest sequence number) packet, for when the first gap is filled.
   * `std::map` satisfies these needs: `insert()` and `lower_bound()` are <em>O(log n)</em>; `begin()` gives the
   * smallest element and is @em O(1).  Iteration is @em O(1) as well.  (All amortized.)
   *
   * ### Memory use ###
   * The above scheme allows for unbounded memory use given certain behavior from the
   * other side, when retransmission is off.  Suppose packets 1, 2, 3 are received; then packets 5,
   * 6, ..., 1000 are received.  Retransmission is off, so eventually the sender
   * may give up on packet 4 and consider it Dropped.  So the gap will forever exist; hence
   * #m_rcv_packets_with_gaps will always hold per-packet data for 5, 6, ..., 1000 (and any
   * subsequent packets).  With retransmission, packet 4 would eventually arrive, or the connection
   * would get RSTed, but without retransmission that doesn't happen.  Thus memory use will just
   * grow and grow.  Solution: come up with some heuristic that would quite conservatively declare
   * that packet 4 has been "received," even though it hasn't. This will plug the hole (packet 4)
   * and clear #m_rcv_packets_with_gaps in this example.  Then if packet 4 does somehow come in, it
   * will get ACKed (like any valid received packet) but will NOT be saved into the Receive buffer,
   * since it will be considered "duplicate" due to already being "received."  Of course, the
   * heuristic must be such that few if any packets considered "received" this way will actually get
   * delivered eventually, otherwise we may lose a lot of data.  Here is one such heuristic, that is
   * both simple and conservative: let N be some constant (e.g., N = 100).  If
   * `m_rcv_packets_with_gaps.size()` exceeds N (i.e., equals (N + 1)), consider all gap packets
   * preceding #m_rcv_packets_with_gaps's first sequence number as "received."  This will, through
   * gap filling logic described above, reduce `m_rcv_packets_with_gaps.size()` to N or less.  Thus it
   * puts a simple upper bound on #m_rcv_packets_with_gaps's memory; if N = 100 the memory used by
   * the structure is not much (since we don't store the actual packet data there [but this can get
   * non-trivial with 100,000 sockets all filled up]).  Is it conservative?  Yes.  100 packets
   * arriving after a gap are a near-guarantee those gap packets will never arrive (especially
   * without retransmission, which is the predicate for this entire problem).  Besides, the Drop
   * heuristics on the Sender side almost certainly will consider gap packets with 100 or near 100
   * Acknowledged packets after them as Dropped a long time ago; if the receiving side's heuristics
   * are far more conservative, then that is good enough.
   *
   * If retransmission is on, then (as noted) the sender's CWND and retransmission logic will ensure
   * that gaps are filled before more future data are sent, so the above situation will not occur.
   * However if the sender is a bad boy and for some reason sends new data and ignores gaps
   * (possibly malicious behavior), then it would still be a problem.  Since in retransmission mode
   * it's not OK to just ignore lost packets, we have no choice but to drop received packets when
   * the above situation occurs (similarly to when Receive buffer is exceeded).  This is basically a
   * security measure and should not matter assuming well-behaved operation from the other side.
   * Update: With retransmission on, this structure is now subject to overflow protection with a tighter
   * limit than with rexmit-off; namely, the limit controlling #m_rcv_buf overflow actually applies to
   * the sum of data being stored in #m_rcv_buf and this structure, together.  I.e., a packet is dropped
   * if the total data stored in #m_rcv_buf and #m_rcv_packets_with_gaps equal or exceed the configured
   * limit.  Accordingly, rcv-wnd advertised to other side is based on this sum also.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   *
   * @see #m_rcv_reassembly_q_data_size.
   *
   * @todo The memory use of this structure could be greatly optimized if, instead of storing each
   * individual received packet's metadata separately, we always merged contiguous sequence number
   * ranges.  So for example if packet P1, P2, P3 (contiguous) all arrived in sequence, after
   * missing packet P0, then we'd store P1's first sequence number and the total data size of
   * P1+P2+P3, in a single `struct` instance.  Since a typical pattern might include 1 lost packet
   * followed by 100 received packets, we'd be able to cut down memory use by a factor of about 100
   * in that case (and thus worry much less about the limit).  Of course the code would get more
   * complex and potentially slower (but not necessarily significantly).
   */
  Recvd_pkt_map m_rcv_packets_with_gaps;

  /**
   * With retransmission enabled, the sum of Received_packet::m_size over all packets stored in the
   * reassembly queue, #m_rcv_packets_with_gaps.  Stored for performance.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  size_t m_rcv_reassembly_q_data_size;

  /**
   * The received packets to be acknowledged in the next low-level ACK packet to be sent to the
   * other side, ordered in the chronological order they were received.  They are accumulated in a
   * data structure because we may not send each desired acknowledgment right away, combining
   * several together, thus reducing overhead at the cost of short delays (or even nearly
   * non-existent delays, as in the case of several DATA packets handled in one
   * NodeLLlow_lvl_recv_and_handle() invocation, i.e., having arrived at nearly at the same time).
   *
   * Any two packets represented by these Individual_ack objects may be duplicates of each other (same
   * Sequence_number, possibly different Individual_ack::m_received_when values).  It's up to the sender (receiver
   * of ACK) to sort it out.  However, again, they MUST be ordered chronologicaly based on the time
   * when they were received; from earliest to latest.
   *
   * Storing shared pointers to avoid copying of structs (however small) during internal
   * reshuffling; shared instead of raw pointers to not worry about delete.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  std::vector<boost::shared_ptr<Individual_ack>> m_rcv_pending_acks;

  /**
   * Helper state, to be used while inside either Node::low_lvl_recv_and_handle() or
   * async part of Node::async_wait_latency_then_handle_incoming(), set only at the beginning of either and equal to
   * `m_rcv_pending_acks.size()` at that time.  Because, for efficiency, individual acknowledgements are
   * accumulated over the course of those two methods, and an ACK with those acknowledgments is
   * sent at the end of that method (in perform_accumulated_on_recv_tasks()) at the earliest, this
   * member is used to determine whether we should start a delayed ACK timer at that point.
   *
   * This gains meaning only in thread W and only within Node::low_lvl_recv_and_handle()/etc.
   * and loses meaning after either method exits.  This should NOT
   * be accessed outside of thread W and is not protected by a mutex.
   */
  size_t m_rcv_pending_acks_size_at_recv_handler_start;

  /**
   * While Node::low_lvl_recv_and_handle() or async part of Node::async_wait_latency_then_handle_incoming() is running,
   * accumulates the individual acknowledgments contained in all incoming ACK low-level packets
   * received in those methods.  More precisely, this accumulates the elements of
   * `packet.m_rcv_acked_packets` for all packets such that `packet` is an Ack_packet.  They are accumulated
   * in this data structure for a similar reason that outgoing acknowledgments are accumulated in
   * `Peer_socket::m_rcv_pending_acks`.  The situation here is simpler, however, since the present
   * structure is always scanned and cleared at the end of the current handler and never carried
   * over to the next, as we always want to scan all individual acks received within a non-blocking
   * amount of time from receipt.  See Node::handle_ack_to_established() for details.
   *
   * This structure is empty, accumulated over the course of those methods, is used to finally scan
   * all individual acknowledgments (in the exact order received), and then cleared for the next
   * run.
   *
   * Storing shared pointers to avoid copying of structs (however small) during internal
   * reshuffling; shared instead of raw pointers to not worry about delete.
   *
   * This gains meaning only in thread W and only within Node::low_lvl_recv_and_handle()/etc.
   * and loses meaning after either method exits.  This should NOT
   * be accessed outside of thread W and is not protected by a mutex.
   */
  std::vector<Ack_packet::Individual_ack::Ptr> m_rcv_acked_packets;

  /**
   * While Node::low_lvl_recv_and_handle() or async part of Node::async_wait_latency_then_handle_incoming() is running,
   * contains the rcv_wnd (eventual #m_snd_remote_rcv_wnd) value in the last observed ACK low-level
   * packet received in those methods.  The reasoning is similar to #m_rcv_acked_packets.  See
   * Node::handle_ack_to_established() for details.
   *
   * This gains meaning only in thread W and only within Node::low_lvl_recv_and_handle()/etc.
   * and loses meaning after either method exits.  This should NOT
   * be accessed outside of thread W and is not protected by a mutex.
   */
  size_t m_snd_pending_rcv_wnd;

  /**
   * The last rcv_wnd value sent to the other side (in an ACK).  This is used to gauge how much the
   * true rcv_wnd has increased since the value that the sender probably (assuming ACK was not lost)
   * knows.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  size_t m_rcv_last_sent_rcv_wnd;

  /**
   * `true` indicates we are in a state where we've decided other side needs to be informed that
   * our receive window has increased substantially, so that it can resume sending data (probably
   * after a zero window being advertised).
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  bool m_rcv_in_rcv_wnd_recovery;

  /**
   * Time point at which #m_rcv_in_rcv_wnd_recovery was last set to true.  It is only used when the
   * latter is indeed true.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  Fine_time_pt m_rcv_wnd_recovery_start_time;

  /**
   * When #m_rcv_in_rcv_wnd_recovery is `true`, this is the scheduled task to possibly
   * send another unsolicited rcv_wnd-advertising ACK to the other side.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  util::Scheduled_task_handle m_rcv_wnd_recovery_scheduled_task;

  /**
   * Timer started, assuming delayed ACKs are enabled, when the first Individual_ack is placed onto
   * an empty #m_rcv_pending_acks; when it triggers, the pending individual acknowledgments are packed
   * into as few as possible ACKs and sent to the other side.  After the handler exits
   * #m_rcv_pending_acks is again empty and the process can repeat starting with the next received valid
   * packet.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   *
   * ### Implementation notes ###
   * In other places we have tended to replace a `Timer` with the far simpler util::schedule_task_from_now() (etc.)
   * facility (which internally uses a `Timer` but hides its various annoyances and caveats).  Why not here?
   * Answer: This timer is scheduled and fires often (could be on the order of every 1-500 milliseconds) and throughout
   * a given socket's existence; hence the potential performance effects aren't worth the risk (or at least mental
   * energy spent on evaluating that risk, originally and over time).  The conservative thing to do is reuse a single
   * `Timer` repeatedly, as we do here.
   */
  util::Timer m_rcv_delayed_ack_timer;

  /// Stats regarding incoming traffic (and resulting outgoing ACKs) for this connection so far.
  Peer_socket_receive_stats_accumulator m_rcv_stats;

  /**
   * The Initial Sequence Number (ISN) used in our original SYN or SYN_ACK.  Useful at least in re-sending the
   * original SYN or SYN_ACK if unacknowledged for too long.
   *
   * @see #m_snd_flying_pkts_by_seq_num and #m_snd_flying_pkts_by_sent_when.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  Sequence_number m_snd_init_seq_num;

  /**
   * The sequence number for the start of the data in the next new DATA packet to be sent out.  By
   * "new" I mean not-retransmitted (assuming retransmission is even enabled).
   *
   * @todo Possibly #m_snd_next_seq_num will apply to other packet types than DATA, probably anything to do with
   * connection termination.
   *
   * @see #m_snd_flying_pkts_by_seq_num and #m_snd_flying_pkts_by_sent_when.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  Sequence_number m_snd_next_seq_num;

  /**
   * The collection of all In-flight packets, indexed by sequence number and ordered from most to
   * least recently sent (including those queued up to wire-send in pacing module).  See also
   * #m_snd_flying_pkts_by_seq_num which is a similar map but in order of sequence number.
   * That map's keys are again sequence numbers, but its values are iterators into the present map
   * to save memory and avoid having to sync up data between the two (the only thing we must sync between them
   * are their key sets). The two maps together can be considered to be the sender-side "scoreboard."
   *
   * These are all the packets that have been sent but not Acknowledged that we have not yet considered
   * Dropped.  (With retransmission on, packets are never considered
   * permanently Dropped, but they are considered Dropped until retransmitted.)  With retransmission
   * off, the ultimate goal of having this structure at all is to handle ACKs, the ultimate goal of
   * which is, in turn, for the In-flight vs. Congestion Window comparison for congestion control.
   * With retransmission on, the structure additionally stores the data in the In-flight packets, so
   * that they can be retransmitted if we determine they were probably dropped.
   *
   * With retransmission on, this is NOT the retransmission queue itself -- i.e., this does NOT
   * store packet data that we know should be retransmitted when possible but rather only the data
   * already In-flight (whether from first attempt or from retransmission).
   *
   * Please see #m_snd_flying_pkts_by_seq_num for a breakdown of the sequence number space.
   * Since that structure contains iterators to exactly the values in the present map, that comment will
   * explain which packets are in the present map.
   *
   * #m_snd_flying_pkts_by_sent_when contains In-flight Sent_packet objects as values, with each particular
   * Sent_packet's first sequence number as its key.  If there are no In-flight Sent_packet objects, then
   * `m_snd_flying_pkts_by_sent_when.empty()`.
   *
   * The above is an invariant, to be true at the end of each boost.asio handler in thread W, at
   * least.
   *
   * Each sent packet therefore is placed into #m_snd_flying_pkts_by_sent_when, at the front (as is standard
   * for a Linked_hash_map, and as is expected, since they are ordered by send time).  (Note, however,
   * that being in this map does not mean it has been sent; it may only be queued up to be sent and
   * waiting in the pacing module; however, pacing does not change the order of packets but merely
   * the exact send moment, which cannot change the position in this queue.)
   * When a packet is Acknowledged, it is removed from #m_snd_flying_pkts_by_sent_when -- could be from anywhere
   * in the ordering.   Similarly to Acknowledged packets, Dropped ones are also removed.
   *
   * The type used is a map indexed by starting sequence number of each packet but also in order of
   * being sent out.  Lookup by sequence number is near constant time; insertion near the end is
   * near constant time; and iteration by order of when it was sent out is easy/fast, with iterators
   * remaining valid as long as the underlying elements are not erased.  Why use this particular
   * structure?  Well, the lookup by sequence number occurs all the time, such as when matching up
   * an arriving acknowledgment against a packet that we'd sent out.  We'd prefer it to not invalidate
   * iterators when something is erased, so Linked_hash_map is good in that way
   * also.  So finally, why order by time it was queued up for sending (as opposed to by sequence
   * number, as would be the case if this were an std::map)?  In truth, both are needed, which is why
   * #m_snd_flying_pkts_by_seq_num exists.  This ordering is needed particularly for the
   * `m_acks_after_me` logic, wherein we count how many times packets that were sent after a given packet
   * have been acknowledged so far; by arranging the packets in that same order, that value can be
   * easily and quickly accumulated by walking back from the most recently sent packet.  On the other
   * hand, some operations need sequence number ordering, which is why we have #m_snd_flying_pkts_by_seq_num;
   * note (again) that the two maps have the same key set, with one's values being iterators pointing into
   * the other.
   *
   * Whenever a packet with `m_sent_when.back().m_sent_time == T` is acknowledged, we need to (by definition of
   * Sent_packet::m_acks_after_me) increment `m_acks_after_me` for each packet with
   * `m_sent_when.back().m_sent_time < T`.  So, if we
   * find the latest-sent element that satisfies that, then all packets appearing to the right
   * (i.e., "sent less recently than") and including that one, in this ordering, should have `m_acks_after_me`
   * incremented.  Using a certain priority queue-using algorithm (see Node::handle_accumulated_acks())
   * we can do this quite efficiently.
   *
   * Note that this means Sent_packet::m_acks_after_me is strictly increasing as one walks this map.
   *
   * Since any packet with `m_acks_after_me >= C`, where `C` is some constant, is considered Dropped and
   * removed from #m_snd_flying_pkts_by_seq_num and therefore this map also, we also get the property that
   * if we find a packet in this map for which that is true, then it is also true for all packets
   * following it ("sent less recently" than it) in this map.  This allows us to more quickly determine which
   * packets should be removed from #m_snd_flying_pkts_by_sent_when, without having to walk the entire structure(s).
   *
   * ### Memory use ###
   * This structure's memory use is naturally bounded by the Congestion Window.
   * Congestion control will not let it grow beyond that many packets (bytes really, but you get the point).
   * At that point blocks will stay on the Send buffer, until that fills up too.  Then send() will refuse to enqueue
   * any more packets (telling the user as much).
   *
   * ### Thread safety ###
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   *
   * @see Sent_when and Sent_packet::m_sent_when, where if `X` is the the last element of the latter sequence, then
   *      `X.m_sent_time` is the value by which elements in the present map are ordered.  However, this only
   *      happens to be the case, because by definition an element
   *      is always placed at the front of the present map (Linked_hash_map), and this order is inductively maintained;
   *      AND MEANWHILE A Sent_when::m_sent_time's constructed value can only increase over time (which is a guaranteed
   *      property of the clock we use (::Fine_clock)).
   * @see #m_snd_flying_bytes, which must always be updated to be accurate w/r/t
   *      #m_snd_flying_pkts_by_sent_when.  Use Node::snd_flying_pkts_updated() whenever
   *      #m_snd_flying_pkts_by_sent_when is changed.
   * @see #m_snd_flying_pkts_by_seq_num, which provides an ordering of the elements of
   *      #m_snd_flying_pkts_by_sent_when by sequence number.  Whereas the present structure is used to
   *      determine `m_acks_after_me` (since logically "after" means "sent after"), `..._by_seq_num`
   *      is akin to the more classic TCP scoreboard, which is used to subdivide the sequence number
   *      space (closely related to #m_snd_next_seq_num and such).  With
   *      retransmission off, "after" would simply mean "having higher sequence number," so
   *      #m_snd_flying_pkts_by_sent_when would already provide this ordering, but with retransmission on
   *      a retransmitted packet with a lower number could be sent after one with a higher number.
   *      To make the code simpler, we therefore rely on a separate structure in either situation.
   */
  Sent_pkt_by_sent_when_map m_snd_flying_pkts_by_sent_when;

  /**
   * The collection of all In-flight packets (including those queued up to send in pacing module),
   * indexed AND ordered by sequence number.  See also #m_snd_flying_pkts_by_sent_when which is a similar map
   * but in order of time sent.  Our map's keys are sequence numbers again, but its values are iterators
   * into #m_snd_flying_pkts_by_sent_when to save memory and avoid having to sync up data between the two
   * (only keys are in sync).  The two maps together can be considered to be the "scoreboard," though in fact
   * the present structure alone is closer to a classic TCP scoreboard.
   *
   * The key sets of the two maps are identical.  The values in this map are iterators to exactly all
   * elements of #m_snd_flying_pkts_by_sent_when.  One can think of the present map as essentially achieving an
   * alternate ordering of the values in the other map.
   *
   * That said, the structure's contents and ordering are closely related to a breakdown of the sequence
   * number space.  I provide this breakdown here.  I list the
   * sequence number ranges in increasing order starting with the ISN.  Let `first_flying_seq_num
   * = m_snd_flying_pkts_by_seq_num.begin()->first` (i.e., the first key Sequence_number in
   * #m_snd_flying_pkts_by_seq_num) for exposition purposes.
   *
   *   - #m_snd_init_seq_num =
   *     - SYN or SYN_ACK
   *
   *   - [`m_snd_init_seq_num + 1`, `first_flying_seq_num - 1`] =
   *     - Largest possible range of sequence numbers such that each datum represented by this range
   *       has been sent and either:
   *       - Acknowledged (ACK received for it); or
   *       - Dropped (ACK not received; we consider it dropped due to some factor like timeout or
   *         duplicate ACKs);
   *
   *   - [`first_flying_seq_num`, `first_flying_seq_num + N - 1`] =
   *     - The first packet that has been sent that is neither Acknowledged nor Dropped.  `N` is length
   *       of that packet.  This is always the first packet, if any, to be considered Dropped in the
   *       future.  This packet is categorized In-flight.
   *
   *   - [`first_flying_seq_num + N`, `m_snd_next_seq_num - 1`] =
   *       - All remaining sent packets.  Each packet in this range is one of the following:
   *         - Acknowledged;
   *         - not Acknowledged and not Dropped = categorized In-flight.
   *
   *   - [#m_snd_next_seq_num, ...] =
   *     - Unsent packets, if any.
   *
   * #m_snd_flying_pkts_by_sent_when and #m_snd_flying_pkts_by_seq_num contain In-flight Sent_packet objects as values
   * (though the latter indirectly via iterator into the former) with each particular Sent_packet's first sequence
   * number as its key in either structure.  If there are no In-flight Sent_packet objects, then
   * `m_snd_flying_pkts_by_{sent_when|seq_num}.empty()` and hence `first_flying_seq_num` above does not
   * exist. Each of the [ranges] above can be null (empty).
   *
   * Each sent packet therefore is placed into #m_snd_flying_pkts_by_seq_num, at the back (if it's a new packet) or
   * possibly elsewhere (if it's retransmitted) -- while it is also placed into #m_snd_flying_pkts_by_sent_when but
   * always at the front (as, regardless of retransmission or anything else, it is the latest packet to be SENT).  When
   * packet is Acknowledged, it is removed from `m_snd_flying_pkts_by_*` -- could be from anywhere in
   * the ordering.  Similarly to Acknowledged packets, Dropped ones are also removed.
   *
   * Why do we need this map type in addition to `Linked_hash_map m_snd_flying_pkts_by_sent_when`?  Answer: Essentially,
   * when an acknowledgment comes in, we need to be able to determine where in the sequence number space this is.
   * If packets are ordered by send time -- not sequence number -- and the sequence number does not match
   * exactly one of the elements here (e.g., it erroneously straddles one, or it is a duplicate acknowledgement,
   * which means that element isn't in the map any longer), then a tree-sorted-by-key map is invaluable
   * (in particular: to get `upper_bound()`, and also to travel to the previous-by-sequence-number packet
   * from the latter).  So logarithmic-time upper-bound searches and iteration by sequence number are what we want and
   * get with this added ordering on top of #m_snd_flying_pkts_by_sent_when.
   *
   * ### Memory use ###
   * This structure's memory use is naturally bounded the same as #m_snd_flying_pkts_by_sent_when.
   *
   * ### Thread safety ###
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   *
   * @see #m_snd_flying_pkts_by_sent_when. There's a "see also" comment there that contrasts these two
   *      important structures.
   */
  Sent_pkt_by_seq_num_map m_snd_flying_pkts_by_seq_num;

  /**
   * The number of bytes contained in all In-flight packets, used at least for comparison against
   * the congestion window (CWND).  More formally, this is the sum of all Sent_packet::m_size values
   * in #m_snd_flying_pkts_by_sent_when.  We keep this, instead of computing it whenever needed, for
   * performance.  In various TCP and related RFCs this value (or something spiritually similar, if
   * only cumulative ACKs are used) is called "pipe" or "FlightSize."
   *
   * Though in protocols like DCCP, where CWND is stored in packets, instead of bytes, "pipe" is
   * actually just `m_snd_flying_pkts_by_sent_when.size()`.  Not for us though.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   *
   * @see #m_snd_flying_pkts_by_sent_when, which must always be updated to be accurate w/r/t
   *      #m_snd_flying_bytes.  Use Node::snd_flying_pkts_updated() whenever #m_snd_flying_pkts_by_sent_when
   *      is changed.
   */
  size_t m_snd_flying_bytes;

  /**
   * Helper data structure to store the packet IDs of packets that are marked Dropped during a single run
   * through accumulated ACKs; it is a data member instead of local variable for performance only.  The pattern is
   * to simply `clear()` it just before use, then load it up with stuff in that same round of ACK handling;
   * and the same thing each time we need to handle accumulated ACKs.  Normally one would just create one
   * of these locally within the code `{` block `}` each time instead.
   * Not doing so avoids unnecessary various internal-to-`vector` buffer
   * allocations.  Instead the internal buffer will grow as large as it needs to and not go down from there, so
   * that it can be reused in subsequent operations.  (Even `clear()` does not internally shrink the buffer.)
   * Of course some memory is held unnecessarily, but it's a small amount; on the other hand the performance
   * gain may be non-trivial due to the frequency of the ACK-handling code being invoked.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  std::vector<order_num_t> m_snd_temp_pkts_marked_to_drop;

  /**
   * For the Sent_packet representing the next packet to be sent, this is the value to assign to
   * `m_sent_when.back().first`.  In other words it's an ever-increasing number that is sort of like
   * a sequence number but one per packet and represents time at which sent, not order in the byte stream.
   * In particular the same packet retransmitted will have the same sequence number the 2nd time but
   * an increased order number.  Starts at 0.
   *
   * This is only used for book-keeping locally and never transmitted over network.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  order_num_t m_snd_last_order_num;

  /**
   * If retransmission is on, this is the retransmission queue.  It's the queue of packets determined to have
   * been dropped and thus to be retransmitted, when Congestion Window allows this.  Packet in Sent_packet::m_packet
   * field of element at top of the queue is to be retransmitted next; and the element itself is to be inserted into
   * #m_snd_flying_pkts_by_sent_when while popped from the present structure.  The packet's Data_packet::m_rexmit_id
   * should be incremented before sending; and the Sent_packet::m_sent_when `vector` should be appended with the
   * then-current time (for future RTT calculation).
   *
   * If retransmission is off, this is empty.
   *
   * Why use `list<>` and not `queue<>` or `deque<>`?  Answer: I'd like to use `list::splice()`.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  std::list<boost::shared_ptr<Sent_packet>> m_snd_rexmit_q;

  /// Equals `m_snd_rexmit_q.size().`  Kept since `m_snd_rexmit_q.size()` may be @em O(n) depending on implementation.
  size_t m_snd_rexmit_q_size;

  /**
   * The congestion control strategy in use for this connection on this side.  Node informs
   * this object of events, such as acknowedgments and loss events; conversely this object informs
   * (or can inform if asked) the Node whether or not DATA packets can be sent, by means of
   * providing the Node with the socket's current Congestion Window (CWND) computed based on the
   * particular Congestion_control_strategy implementation's algorithm (e.g., Reno or Westwood+).
   * Node then determines whether data can be sent by comparing #m_snd_flying_bytes (# of bytes we think
   * are currently In-flight) to CWND (# of bytes the strategy allows to be In-flight currently).
   *
   * ### Life cycle ###
   * #m_snd_cong_ctl must be initialized to an instance before user gains access to this
   * socket; the pointer must never change subsequently except back to null (permanently).  The
   * Peer_socket destructor, at the latest, will delete the underlying object, as #m_snd_cong_ctl
   * will be destructed.  (Note `unique_ptr` has no copy operator or
   * constructor.)  There is a 1-to-1 relationship between `*this` and #m_snd_cong_ctl.
   *
   * ### Visibility between Congestion_control_strategy and Peer_socket ###
   * #m_snd_cong_ctl gets read-only (`const`!) but otherwise complete
   * private access (via `friend`) to the contents of `*this` Peer_socket.  For example, it can
   * read `this->m_snd_smoothed_round_trip_time` (the SRTT) and use it
   * for computations if needed.  Node and Peer_socket get only strict public API access to
   * #m_snd_cong_ctl, which is a black box to it.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  boost::movelib::unique_ptr<Congestion_control_strategy> m_snd_cong_ctl;

  /**
   * The receive window: the maximum number of bytes the other side has advertised it would be
   * willing to accept into its Receive buffer if they'd arrived at the moment that advertisement
   * was generated by the other side.  This starts as 0 (undefined) and is originally set at SYN_ACK
   * or SYN_ACK_ACK receipt and then subsequently updated upon each ACK received.  Each such update
   * is called a "rcv_wnd update" or "window update."
   *
   * #m_snd_cong_ctl provides congestion control; this value provides flow control.  The socket's
   * state machine must be extremely careful whenever either this value or
   * `m_snd_cong_ctl->congestion_window_bytes()` may increase, as when that occurs it should call
   * Node::send_worker() in order to possibly send data over the network.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  size_t m_snd_remote_rcv_wnd;

  /**
   * The outgoing available bandwidth estimator for this connection on this side.  Node informs
   * this object of events, namely as acknowedgments; conversely this object informs
   * (or can inform if asked) the Node what it thinks is the current available bandwidth for
   * user data in DATA packets.  This can be useful at least for some forms of congestion control
   * but possibly as information for the user, which is why it's an independent object and not
   * part of a specific congestion control strategy (I only mention this because the mechanics of such
   * a bandwidth estimator typically originate in service of a congestion control algorithm like Westwood+).
   *
   * ### Life cycle ###
   * It must be initialized to an instance before user gains access to this
   * socket; the pointer must never change subsequently except back to null (permanently).  The
   * Peer_socket destructor, at the latest, will delete the underlying object, as #m_snd_bandwidth_estimator
   * is destroyed along with `*this`.  The only reason it's a pointer is that it takes a Const_ptr in the constructor,
   * and that's not available during Peer_socket construction yet.  (Note `unique_ptr` has no copy operator or
   * constructor.)  There is a 1-to-1 relationship between `*this` and #m_snd_bandwidth_estimator.
   *
   * ### Visibility between Send_bandwidth_estimator and Peer_socket ###
   * The former gets read-only (`const`!) but otherwise complete private access (via `friend`) to the contents of
   * `*this` Peer_socket.  For example, it can read #m_snd_smoothed_round_trip_time (the SRTT) and use it
   * for computations if needed.  Node and Peer_socket get only strict public API access to
   * #m_snd_bandwidth_estimator, which is a black box to it.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  boost::movelib::unique_ptr<Send_bandwidth_estimator> m_snd_bandwidth_estimator;

  /**
   * Estimated current round trip time of packets, computed as a smooth value over the
   * past individual RTT measurements.  This is updated each time we make an RTT measurement (i.e.,
   * receive a valid, non-duplicate acknowledgment of a packet we'd sent).  The algorithm to compute
   * it is taken from RFC 6298.  The value is 0 (not a legal value otherwise) until the first RTT
   * measurement is made.
   *
   * We use #Fine_duration (the high fine-grainedness and large bit width corresponding to #Fine_clock) to
   * store this, and the algorithm we use to compute it avoids losing digits via unnecessary
   * conversions between units (e.g., nanoseconds -> milliseconds).
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  Fine_duration m_snd_smoothed_round_trip_time;

  /// RTTVAR used for #m_snd_smoothed_round_trip_time calculation.  @see #m_snd_smoothed_round_trip_time.
  Fine_duration m_round_trip_time_variance;

  /**
   * The Drop Timer engine, which controls how In-flight (#m_snd_flying_pkts_by_sent_when) packets are
   * considered Dropped due to being unacknowledged for too long.  Used while #m_int_state is
   * Int_state::S_ESTABLISHED.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  Drop_timer_ptr m_snd_drop_timer;

  /**
   * The Drop Timeout: Time period between the next time #m_snd_drop_timer schedules a Drop Timer and that timer
   * expiring.  This is updated each time #m_snd_smoothed_round_trip_time is updated, and the Drop_timer
   * itself may change it under certain circumstances.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  Fine_duration m_snd_drop_timeout;

  /**
   * The state of outgoing packet pacing for this socket; segregated into a simple `struct` to keep
   * Peer_socket shorter and easier to understand.  Packet pacing tries to combat the burstiness of
   * outgoing low-level packet stream.
   *
   * @see `struct Send_pacing_data` doc header for much detail.
   */
  Send_pacing_data m_snd_pacing_data;

  /**
   * The last time that Node has detected a packet loss event and so informed #m_snd_cong_ctl by calling
   * the appropriate method of class Congestion_control_strategy.  Roughly speaking, this is used to
   * determine whether the detection of a given dropped packet is part of the same loss event as the
   * previous one; if so then #m_snd_cong_ctl is not informed again (presumably to avoid dropping CWND
   * too fast); if not it is informed of the new loss event.  Even more roughly speaking, if the new
   * event is within a certain time frame of #m_snd_last_loss_event_when, then they're considered in the
   * same loss event.  You can find detailed discussion in a giant comment in
   * Node::handle_accumulated_acks().
   *
   * Before any loss events, this is set to its default value (zero time since epoch).
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  Fine_time_pt m_snd_last_loss_event_when;

  /**
   * Time at which the last Data_packet low-level packet for this connection was sent.  We
   * use this when determining whether the connection is in Idle Timeout (i.e., has sent no traffic
   * for a while, which means there has been no data to send).  It's used for congestion control.
   *
   * Before any packets are sent, this is set to its default value (zero time since epoch).
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   *
   * ### Pacing ###
   * See Send_packet_pacing #m_snd_pacing_data.  See pacing-relevant note on Sent_packet::m_sent_when
   * which applies equally to this data member.
   */
  Fine_time_pt m_snd_last_data_sent_when;

  /// Stats regarding outgoing traffic (and resulting incoming ACKs) for this connection so far.
  Peer_socket_send_stats_accumulator m_snd_stats;

  /**
   * Random security token used during SYN_ACK-SYN_ACK_ACK.  For a given connection handshake, the
   * SYN_ACK_ACK receiver ensures that #m_security_token it received is equal to the original one it
   * had sent in SYN_ACK.  This first gains meaning upong sending SYN_ACK and it does not change afterwards.
   * It is not used unless `!m_active_connect`.  See #m_active_connect.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  security_token_t m_security_token;

  /**
   * Connection attempt scheduled task; fires if an individual connection request packet is not answered with a reply
   * packet in time.  It is readied when *any* SYN or SYN_ACK packet is sent, and fired if that packet has gone
   * unacknowledged with a SYN_ACK or SYN_ACK_ACK (respectively), long enough to be retransmitted.
   *
   * Connection establishment is aborted if it fires too many times, but #m_connection_timeout_scheduled_task is how
   * "too many times" is determined.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   *
   * @see #m_connection_timeout_scheduled_task which keeps track of the entire process timing out, as opposed to the
   *      individual attempts.
   */
  util::Scheduled_task_handle m_init_rexmit_scheduled_task;

  /**
   * If currently using #m_init_rexmit_scheduled_task, this is the number of times the timer has already fired
   * in this session.  So when the timer is readied the first time it's zero; if it fires and is
   * thus readied again it's one; again => two; etc., until timer is canceled or connection is
   * aborted due to too many retries.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   */
  unsigned int m_init_rexmit_count;

  /**
   * Connection timeout scheduled task; fires if the entire initial connection process does not complete within a
   * certain amount of time.  It is started when the SYN or SYN_ACK is sent the very first time (NOT counting
   * resends), canceled when SYN_ACK or SYN_ACK_ACK (respectively) is received in response to ANY SYN or
   * SYN_ACK (respevtively), and fired if the the latter does not occur in time.
   *
   * This gains meaning only in thread W.  This should NOT be accessed outside of thread W and is
   * not protected by a mutex.
   *
   * @see #m_init_rexmit_scheduled_task which keeps track of *individual* attempts timing out, as opposed to the
   *      entire process.
   */
  util::Scheduled_task_handle m_connection_timeout_scheduled_task;

  /**
   * This is the final set of stats collected at the time the socket was moved to S_CLOSED #m_state.
   * If it has not yet moved to that state, this is not applicable (but equals Peer_socket_info()).
   * It's used by info() to get at the final set of stats, before the source info is purged by the
   * resource cleanup in sock_free_memory().
   */
  Peer_socket_info m_info_on_close;
}; // class Peer_socket


/**
 * @private
 *
 * Metadata (and data, if retransmission is on) for a packet that has been sent one (if
 * retransmission is off) or possibly more (if on) times.  This is purely a data store, not a
 * class.  It is not copyable, and moving them around by smart Sent_packet::Ptr is encouraged.
 */
struct Peer_socket::Sent_packet :
  // Endow us with shared_ptr<>s ::Ptr and ::Const_ptr (syntactic sugar).
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Peer_socket::Sent_packet>>,
  private boost::noncopyable
{
  // Types.

  struct Sent_when;

  /**
   * Type used for #m_acks_after_me.  Use a small type to save memory; this is easily large enough,
   * given that that we drop a packet after #m_acks_after_me exceeds a very small value (< 10), and
   * a given ACK can *conceivably* hold a few thousand individual acknowledgments (most likely many
   * fewer).  In fact `uint8_t` is probably enough, but it seemed easier to not worry about
   * overflows when doing arithmetic with these.
   */
  using ack_count_t = uint16_t;

  // Data.

  /// Number of bytes in the Data_packet::m_data field of the sent packet.
  const size_t m_size;

  /**
   * Time stamps, order numbers, and other info at the times when the different attempts (including
   * original sending and retransmissions) to send the packet were given to the UDP net-stack. These are
   * arranged in the order they were sent (0 = original transmission, 1 = first retransmission, 2 = second
   * retransmission, ...).  If retransmission is off, this only ever has 1 element (set via
   * constructor).
   *
   * Along with each time stamp, we also store an order number.  It is much like a sequence
   * number, except (a) we never send it anywhere (internal bookkeeping only), (b) it specifies
   * the order in which the packets were sent off.  Why use that and not a `m_sent_time` timestamp?  Because
   * `m_sent_time` can, in theory, have ties (i.e., it's kind of possible for 2 packets to be sent
   * within the same high-resolution clock tick).  This gives us a solid way to order packets by send time
   * in certain key algorithms.
   *
   * Note: for reasons explained in the pacing module this should be the time stamp when we
   * actually send it off to boost.asio UDP net-stack, not when we queue it into the pacing module (which can be
   * quite a bit earlier, if pacing is enabled).  Subtlety: Note that "time stamp when we actually send it off
   * to boost.asio UDP net-stack" IS the same as "time stamp when boost.asio UDP net-stack actually performs the
   * `sendto()` call" in practice -- a good thing for accurate RTT measurements.
   */
  std::vector<Sent_when> m_sent_when;

  /**
   * The number of times any packet with `m_sent_when.back().m_order_num > this->m_sent_when.back().m_order_num`
   * has been acknowledged.  Reworded slightly: start at 0; *IF* a packet, other than this one, is In-flight and then
   * acknowledged, and that packet was sent out (or, equivalently, scheduled to be sent out -- packets do not
   * change order once marked for sending -- a/k/a became In-flight) chronologically AFTER same happened to
   * `*this` packet *THEN* add 1.  After adding up all such acks, the resulting value = this member.
   *
   * This is used to determine when `*this` packet should be considered Dropped,
   * as in TCP's Fast Retransmit heuristic (but better, since time-when-sent -- not sequence number, which has
   * retransmission ambiguities -- is used to order packets).
   *
   * Note, again, that "after" has little to do with *sequence* number but rather with send time, as ordered via
   * the *order* number.  E.g., due to retransmission, `*this` may have sequence number greater than another
   * packet yet have the lesser order number.  (This dichotomy is mentioned a lot, everywhere, and if I'm overdoing
   * it, it's only because sometimes TCP cumulative-ack habits die hard, so I have to keep reminding myself.)
   * (If retransmission is disabled, however, then sequence number order = order number order.)
   */
  ack_count_t m_acks_after_me;

  /**
   * If retransmission is on, this is the DATA packet itself that was sent; otherwise null.  It is
   * stored in case we need to retransmit it.  If we don't (it is acknowledged), the entire
   * Sent_packet is erased and goes away, so #m_packet's ref-count decreases to zero, and it
   * disappears.  If we do, all we need to do to retransmit it is increment its Data_packet::m_rexmit_id and
   * place it into the retransmission queue for sending, when CWND space is available.
   *
   * Why store the entire packet and not just its Data_packet::m_data?  There's a small bit of memory overhead, but
   * we get Data_packet::m_rexmit_id for free and don't have to spend time re-creating the packet to retransmit.
   * So it's just convenient.
   *
   * If retransmission is off, there is no need to store this, as it will not be re-sent.
   */
  const boost::shared_ptr<Data_packet> m_packet;

  // Constructors/destructor.

  /**
   * Constructs object with the given values and #m_acks_after_me at zero.  If rexmit-on option enabled, the
   * packet is stored in #m_packet; otherwise #m_packet is initialized to null.  Regardless, #m_size
   * is set to `packet.m_data.size()`.
   *
   * @param rexmit_on
   *        True if and only if `sock->rexmit_on()` for the containing socket.
   * @param packet
   *        The packet that will be sent.  Used for #m_size and, if `sock->rexmit_on()`, saved in #m_packet.
   * @param sent_when
   *        #m_sent_when is set to contain one element equalling this argument.
   */
  explicit Sent_packet(bool rexmit_on, boost::shared_ptr<Data_packet> packet, const Sent_when& sent_when);
}; // struct Peer_socket::Sent_packet

/**
 * @private
 *
 * Data store to keep timing related info when a packet is sent out.  Construct via direct member initialization.
 * It is copy-constructible (for initially copying into containers and such) but not assignable to discourage
 * unneeded copying (though it is not a heavy structure).  Update: A later version of clang does not like
 * this technique and warns about it; to avoid any such trouble just forget the non-assignability stuff;
 * it's internal code; we should be fine.
 */
struct Peer_socket::Sent_packet::Sent_when
{
  // Data.

  /**
   * Order number of the packet.  This can be used to compare two packets sent out; the one with the
   * higher order number was sent out later.  See Peer_socket::m_snd_last_order_num.  The per-socket next-value
   * for this is incremented each time another packet is sent out.
   *
   * @see Peer_socket::Sent_packet::m_sent_when for more discussion.
   *
   * @internal
   * @todo Can we make Sent_when::m_order_num and some of its peers const?
   */
  // That @internal should not be necessary to hide the @todo in public generated docs -- Doxygen bug?
  const order_num_t m_order_num;

  /**
   * The timestamp when the packet is sent out.  This may be "corrected" when actually sent *after* pacing delay
   * (hence not `const`).
   *
   * @see Fine_clock.  Recall that, due to latter's properties, `Fine_clock::now()` results monotonically increase
   *      over time -- a property on which we rely on, particularly since this data member is only assigned
   *      `Fine_clock::now()` at various times.
   *
   * @see Peer_socket::Sent_packet::m_sent_when for more discussion.
   */
  Fine_time_pt m_sent_time;

  /**
   * The congestion window size (in bytes) that is used when the packet is sent out.
   * We store this to pass to the congestion control module when this packet is acked in the future.
   * Some congestion control algorithms compute CWND based on an earlier CWND value.
   * Not `const` for similar reason as #m_sent_time.
   *
   * @internal
   * @todo Why is #m_sent_cwnd_bytes in Sent_when `struct` and not directly in `Sent_packet`?  Or maybe it should
   * stay here, but Sent_when should be renamed `Send_attempt` (or `Send_try` for brevity)?  Yeah, I think that's
   * it.  Then Sent_packet::m_sent_when -- a container of Sent_when objects -- would become `m_send_tries`, a
   * container of `Send_try` objects.  That makes more sense (sentce?!) than the status quo which involves the
   * singular-means-plural strangeness.
   *
   * @todo Can we make #m_sent_cwnd_bytes and some of its peers `const`?
   */
  // That @internal should not be necessary to hide the @todo in public generated docs -- Doxygen bug?
  size_t m_sent_cwnd_bytes;
}; // struct Peer_socket::Sent_packet::Sent_when

/**
 * @private
 *
 * Metadata (and data, if retransmission is on) for a packet that has been received (and, if
 * retransmission is off, copied to Receive buffer).  This is purely a data store, not a class.
 * It is not copyable, and moving them around by smart Received_packet::Ptr is encouraged.
 */
struct Peer_socket::Received_packet :
  // Endow us with shared_ptr<>s ::Ptr and ::Const_ptr (syntactic sugar).
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Peer_socket::Received_packet>>,
  private boost::noncopyable
{
  // Data.

  /// Number of bytes in the Data_packet::m_data field of that packet.
  const size_t m_size;

  /**
   * Byte sequence equal to that of Data_packet::m_data of the packet.  Kept `empty()` if retransmission is off,
   * since in that mode any received packet's `m_data` is immediately moved to Receive buffer.  With
   * retransmission on, it is stored until all gaps before the packet are filled (a/k/a
   * reassembly).
   */
  util::Blob m_data;

  // Constructors/destructor.

  /**
   * Constructs object by storing size of data and, if so instructed, the data themselves.
   *
   * @param logger_ptr
   *        The Logger implementation to use subsequently.
   * @param size
   *        #m_size.
   * @param src_data
   *        Pointer to the packet data to be moved into #m_data; or null if we shouldn't store it.
   *        This should be null if and only if retransmission is off.  If not null, for
   *        performance, `*src_data` is CLEARED by this constructor (its data moved, in constant
   *        time, into #m_data).  In that case, `src_data.size() == size` must be true at entrance to
   *        constructor, or behavior is undefined.
   */
  explicit Received_packet(log::Logger* logger_ptr, size_t size, util::Blob* src_data);
};

/**
 * @private
 *
 * Metadata describing the data sent in the acknowledgment of an individual received packet.  (A
 * low-level ACK packet may include several sets of such data.)  This is purely a data store, not
 * a class.  It is not copyable, and moving them around by smart Individual_ack::Ptr is encouraged.
 * Construct this by direct member initialization.
 */
struct Peer_socket::Individual_ack
  // Cannot use boost::noncopyable or Shared_ptr_alias_holder, because that turns off direct initialization.
{
  // Types.

  /// Short-hand for ref-counted pointer to mutable objects of this class.
  using Ptr = boost::shared_ptr<Individual_ack>;

  /// Short-hand for ref-counted pointer to immutable objects of this class.
  using Const_ptr = boost::shared_ptr<const Individual_ack>;

  // Data.

  /// Sequence number of first datum in packet.
  const Sequence_number m_seq_num;

  /**
   * Retransmit counter of the packet (as reported by sender).  Identifies which attempt we are
   * acknowledging (0 = initial, 1 = first retransmit, 2 = second retransmit, ...).  Always 0
   * unless retransmission is on.
   */
  const unsigned int m_rexmit_id;

  /// When was it received?  Used for supplying delay before acknowledging (for other side's RTT calculations).
  const Fine_time_pt m_received_when;

  /// Number of bytes in the packet's user data.
  const size_t m_data_size;

  /// Make us noncopyable without breaking aggregateness (direct-init).
  [[no_unique_address]] util::Noncopyable m_nc{};
}; // struct Peer_socket::Individual_ack
// Note: Some static_assert()s about it currently in peer_socket.cpp in a function {} (for boring reasons).

// Free functions: in *_fwd.hpp.

// However the following refer to inner type(s) and hence must be declared here and not _fwd.hpp.

/**
 * @internal
 *
 * @todo There are a few guys like this which are marked `@internal` (Doxygen command) to hide from generated
 * public documentation, and that works, but really they should not be visible in the publicly-exported
 * (not in detail/) header source code; so this should be reorganized for cleanliness.  The prototypes like this one
 * can be moved to a detail/ header or maybe directly into .cpp that uses them (for those where it's only one).
 *
 * Prints string representation of given socket state to given standard `ostream` and returns the
 * latter.
 *
 * @param os
 *        Stream to print to.
 * @param state
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, Peer_socket::Int_state state);

// Template implementations.

template<typename Const_buffer_sequence>
size_t Peer_socket::send(const Const_buffer_sequence& data, Error_code* err_code)
{
  namespace bind_ns = util::bind_ns;
  using bind_ns::bind;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, Peer_socket::send<Const_buffer_sequence>, bind_ns::cref(data), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in user thread U != W.

  Lock_guard lock(m_mutex); // Lock m_node; also it's a pre-condition for Node::send().

  /* Forward the rest of the logic to Node.
   * Now, what I really want to do here is simply:
   *     m_node->send(this, data, err_code);
   * I cannot, however, due to C++'s circular reference BS (Node needs Peer_socket, Peer_socket needs Node,
   * and both send()s are templates so cannot be implemented in .cpp).  So I perform the following
   * voodoo.  The only part that must be defined in .hpp is the part that actually uses template
   * parameters.  The only part that uses template parameters is m_snd_buf->feed_bufs_copy().
   * Therefore I prepare a canned call to m_snd_buf->feed_bufs_copy(data, ...) and save it in a
   * function pointer (Function) to an untemplated, regular function that makes this canned
   * call.  Finally, I call an untemplated function in .cpp, which has full understanding of Node,
   * and pass the pre-canned thingie into that.  That guy finally calls Node::send(), passing along
   * the precanned thingie.  Node::send() can then call the latter when it needs to feed() to the
   * buffer.  Therefore Node::send() does not need to be templated.
   *
   * Yeah, I know.  I had to do it though.  Logic should be in Node, not in Peer_socket.
   *
   * Update: As of this writing I've added a formal @todo into Node doc header.  .inl tricks a-la-Boost might
   * help avoid this issue. */
  const auto snd_buf_feed_func = [this, &data](size_t max_data_size)
  {
    return m_snd_buf.feed_bufs_copy(data, max_data_size);
  };
  // ^-- Important to capture `&data`, not `data`; else `data` (albeit not actual payload) will be copied!

  /* Let this untemplated function, in .cpp, deal with Node (since .cpp knows what *m_node really is).
   * Note: whatever contraption lambda generated above will be converted to a Function<...> with ... being
   * an appropriate signature that node_send() expects, seen in the above explanatory comment. */
  return node_send(snd_buf_feed_func, err_code);
} // Peer_socket::send()

template<typename Const_buffer_sequence>
size_t Peer_socket::sync_send(const Const_buffer_sequence& data, Error_code* err_code)
{
  return sync_send_impl(data, Fine_time_pt(), err_code); // sync_send() with infinite timeout.
}

template<typename Rep, typename Period, typename Const_buffer_sequence>
size_t Peer_socket::sync_send(const Const_buffer_sequence& data,
                              const boost::chrono::duration<Rep, Period>& max_wait,
                              Error_code* err_code)
{
  assert(max_wait.count() > 0);
  return sync_send_impl(data,
                        util::chrono_duration_from_now_to_fine_time_pt(max_wait),
                        err_code);
}

template<typename Rep, typename Period>
bool Peer_socket::sync_send(nullptr_t,
                            const boost::chrono::duration<Rep, Period>& max_wait,
                            Error_code* err_code)
{
  assert(max_wait.count() > 0);
  return sync_send_reactor_pattern_impl(util::chrono_duration_from_now_to_fine_time_pt(max_wait),
                                        err_code);
}

template<typename Const_buffer_sequence>
size_t Peer_socket::sync_send_impl(const Const_buffer_sequence& data, const Fine_time_pt& wait_until,
                                   Error_code* err_code)
{
  namespace bind_ns = util::bind_ns;
  using bind_ns::bind;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, Peer_socket::sync_send_impl<Const_buffer_sequence>,
                                     bind_ns::cref(data), bind_ns::cref(wait_until), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in user thread U != W.

  Lock_guard lock(m_mutex); // Lock m_node; also it's a pre-condition for Node::send().

  /* Forward the rest of the logic to Node.
   * Now, what I really want to do here is simply:
   *     m_node->sync_send(...args...);
   * I cannot, however, due to <see same situation explained in Peer_socket::send()>.
   * Therefore I do the following (<see Peer_socket::send() for more comments>). */

  const auto snd_buf_feed_func = [this, &data](size_t max_data_size)
  {
    return m_snd_buf.feed_bufs_copy(data, max_data_size);
  };
  // ^-- Important to capture `&data`, not `data`; else `data` (albeit not actual payload) will be copied!

  lock.release(); // Let go of the mutex (mutex is still LOCKED).
  /* Let this untemplated function, in .cpp, deal with Node (since .cpp knows what *m_node really is).
   * Note: whatever contraption lambda generated above will be converted to a Function<...> with ... being
   * an appropriate signature that node_sync_send() expects, seen in the above explanatory comment. */
  return node_sync_send(snd_buf_feed_func, wait_until, err_code);
} // Peer_socket::sync_send_impl()

template<typename Mutable_buffer_sequence>
size_t Peer_socket::receive(const Mutable_buffer_sequence& target, Error_code* err_code)
{
  namespace bind_ns = util::bind_ns;
  using bind_ns::bind;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, Peer_socket::receive<Mutable_buffer_sequence>, bind_ns::cref(target), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in user thread U != W.

  Lock_guard lock(m_mutex);  // Lock m_node/m_state; also it's a pre-condition for Node::receive().

  /* Forward the rest of the logic to Node.
   * Now, what I really want to do here is simply:
   *     m_node->receive(this, target), err_code);
   * I cannot, however, due to <see same situation explained in Peer_socket::send()>.
   * Therefore I do the following (<see Peer_socket::send() for more comments>). */

  const auto rcv_buf_consume_func = [this, &target]()
  {
    // Ensure that if there are data in Receive buffer, we will return at least 1 block as
    // advertised.  @todo: I don't understand this comment now.  What does it mean?  Explain.
    return m_rcv_buf.consume_bufs_copy(target);
  };
  // ^-- Important to capture `&target`, not `target`; else `target` (albeit no actual buffers) will be copied!

  /* Let this untemplated function, in .cpp, deal with Node (since .cpp knows what *m_node really is).
   * Note: whatever contraption lambda generated above will be converted to a Function<...> with ... being
   * an appropriate signature that node_receive() expects, seen in the above explanatory comment. */
  return node_receive(rcv_buf_consume_func, err_code);
}

template<typename Mutable_buffer_sequence>
size_t Peer_socket::sync_receive(const Mutable_buffer_sequence& target, Error_code* err_code)
{
  return sync_receive_impl(target, Fine_time_pt(), err_code); // sync_receive() with infinite timeout.
}

template<typename Rep, typename Period, typename Mutable_buffer_sequence>
size_t Peer_socket::sync_receive(const Mutable_buffer_sequence& target,
                                 const boost::chrono::duration<Rep, Period>& max_wait, Error_code* err_code)
{
  assert(max_wait.count() > 0);
  return sync_receive_impl(target,
                           util::chrono_duration_from_now_to_fine_time_pt(max_wait),
                           err_code);
}

template<typename Rep, typename Period>
bool Peer_socket::sync_receive(nullptr_t,
                               const boost::chrono::duration<Rep, Period>& max_wait,
                               Error_code* err_code)
{
  assert(max_wait.count() > 0);
  return sync_receive_reactor_pattern_impl(util::chrono_duration_from_now_to_fine_time_pt(max_wait), err_code);
}

template<typename Mutable_buffer_sequence>
size_t Peer_socket::sync_receive_impl(const Mutable_buffer_sequence& target,
                                      const Fine_time_pt& wait_until, Error_code* err_code)
{
  namespace bind_ns = util::bind_ns;
  using bind_ns::bind;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(size_t, Peer_socket::sync_receive_impl<Mutable_buffer_sequence>,
                                     bind_ns::cref(target), bind_ns::cref(wait_until), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in user thread U != W.

  Lock_guard lock(m_mutex); // Lock m_node; also it's a pre-condition for Node::send().

  /* Forward the rest of the logic to Node.
   * Now, what I really want to do here is simply:
   *     m_node->sync_receive(...args...);
   * I cannot, however, due to <see same situation explained in Peer_socket::send()>.
   * Therefore I do the following (<see Peer_socket::send() for more comments>). */

  const auto rcv_buf_consume_func = [this, &target]()
  {
    return m_rcv_buf.consume_bufs_copy(target);
  };
  // ^-- Important to capture `&target`, not `target`; else `target` (albeit no actual buffers) will be copied!

  lock.release(); // Let go of the mutex (mutex is still LOCKED).
  /* Let this untemplated function, in .cpp, deal with Node (since .cpp knows what *m_node really is).
   * Note: whatever contraption lambda generated above will be converted to a Function<...> with ... being
   * an appropriate signature that node_sync_receive() expects, seen in the above explanatory comment. */
  return node_sync_receive(rcv_buf_consume_func, wait_until, err_code);
} // Peer_socket::sync_receive()

template<typename Opt_type>
Opt_type Peer_socket::opt(const Opt_type& opt_val_ref) const
{
  // Similar to Node::opt().
  Options_lock lock(m_opts_mutex);
  return opt_val_ref;
}

} // namespace flow::net_flow

