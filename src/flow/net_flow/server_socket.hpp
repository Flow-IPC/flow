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
#include "flow/net_flow/options.hpp"
#include "flow/log/log.hpp"
#include "flow/util/linked_hash_set.hpp"
#include "flow/util/util.hpp"
#include "flow/net_flow/error/error.hpp"
#include "flow/util/shared_ptr_alias_holder.hpp"
#include <boost/unordered_set.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <queue>

namespace flow::net_flow
{
// Types.

/**
 * A server socket able to listen on a single Flow port for incoming connections and return peer
 * sockets (Peer_socket objects) to the local user once such connections are established.
 *
 * ### Life cycle of a Server_socket ###
 * A given Server_socket can only arise by calling Node::listen().
 * Node generates a new Server_socket and returns it (factory pattern).  Server_socket is not
 * instantiable otherwise.  A Server_socket cannot be deleted explicitly by the user and will only
 * be returned via `boost::shared_ptr<>`; when both the Node and all user code no longer refers to
 * it, the Server_socket will be destroyed.
 *
 * Once a Flow-protocol user has a Server_socket object, that object represents a socket in one of the
 * following basic states:
 *
 *   - Listening.
 *     - accept() may or may not return peer socket ("Acceptable").
 *   - Closing.
 *     - accept() will never return peer socket (never Acceptable).
 *   - Closed.
 *     - accept() will never return peer socket (never Acceptable).
 *
 * Exactly the following state transitions are possible for a given Server_socket returned by Node:
 *
 *    - start => Listening
 *    - start => Closing
 *    - start => Closed
 *    - Listening => Closing
 *    - Listening => Closed
 *    - Closing => Closed
 *
 * Note, in particular, that Closed is final; socket cannot move from Closed to Listening.  If after
 * an error or valid closing you want to reestablish a server, obtain a new Server_socket
 * from Node's factory.  Same rationale as for equivalent design decision in Peer_socket.
 *
 * Closing means either this side or other side initiated the closing of this server socket for any
 * reason, but Node is still finishing up operations related to that in the background (such as
 * closing in-progress peer connections).  Closed means Node has finished any such operations and
 * has disowned the Server_socket.
 *
 * In either case the only operation Server_socket supports (accept() and derivatives thereof) is
 * impossible in Closing and Closed.  Therefore the two states are distinguished for
 * diagnostic/informational purposes only.  Generally one should only accept() when Acceptable, and
 * accept() will return an error if the state is Closing or Closed.
 *
 * Note that a Closing or Closed Server_socket does NOT mean that already accept()ed Peer_socket objects
 * will be in any way affected.
 *
 * @todo Implement `Server_socket::close()` functionality -- at least the equivalent of
 * Peer_socket::close_abruptly().
 *
 * ### Accept queue ###
 * Server_socket, like a TCP server socket, stores a queue of fully established peer
 * connections.  A Peer_socket is placed onto this internal queue only once its state() ==
 * State::S_OPEN (i.e., the connection handshake is finished/successful).  Peer_socket objects are then
 * obtained from this queue via the Server_socket::accept() call and its derivatives.
 *
 * ### Efficiently accept()ing ###
 * The sync_accept() method is efficient, in that it uses no processor
 * cycles until Acceptable is achieved (i.e., it sleeps until that point).  The non-blocking
 * accept() method doesn't sleep/block, however.  For a program using it to be efficient it should
 * sleep until Acceptable and only then call accept(), when a Peer_socket is certainly
 * available for immediate acceptance.  Moreover, a complex program is likely to want to perform
 * this sleep-and-conditional-wake on a set of several Server_sockets (and/or other sockets)
 * simultaneously (similarly to `select()`, `epoll*()`, etc.).  Use class Event_set for this purpose.
 *
 * ### Thread safety ###
 * Same as for Node.  (Briefly: all operations safe for simultaneous execution on separate or the same object.)
 *
 * @internal
 *
 * Implementation notes
 * --------------------
 * See Peer_socket documentation header.  Similar comments apply here.
 *
 * To prevent node.cpp from being unmanageably large (and also because it makes sense),
 * implementations for Node methods that deal only with an individual Server_socket objects reside in
 * server_socket.cpp (even though they are members of Node, since, again, the logic is all forwarded to Node).
 *
 * @todo Limit Server_socket listen queue/set length.
 *
 * @todo Rename `State` to `Phase`, as with similar to-do in class Peer_socket doc header.
 */
class Server_socket :
  public util::Null_interface,
  // Endow us with shared_ptr<>s ::Ptr and ::Const_ptr (syntactic sugar).
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Server_socket>>,
  // Allow access to Ptr(this) from inside Server_socket methods.  Just call shared_from_this().
  public boost::enable_shared_from_this<Server_socket>,
  public log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// State of a Server_socket.
  enum class State
  {
    /// Future or current accept()s may be possible.  A socket in this state may be Acceptable.
    S_LISTENING,
    /// No accept()s are or will be possible, but Node is still finishing up the closing operation.
    S_CLOSING,
    /// No accept()s are or will be possible, AND Node has disowned the Server_socket.
    S_CLOSED
  };

  /// Equivalent to Peer_socket::Ptr, but can't use that due to C++'s circular reference nonsense.
  using Peer_socket_ptr = boost::shared_ptr<Peer_socket>;

  // Constructors/destructor.

  /// Boring `virtual` destructor.  Note that deletion is to be handled exclusively via `shared_ptr`, never explicitly.
  ~Server_socket() override;

  // Methods.

  /**
   * Current State of the socket.
   * @return Current state of the socket.
   */
  State state() const;

  /**
   * Node that produced this Server_socket.
   * @return Pointer to (guaranteed valid) Node; 0 if state is S_CLOSED.
   */
  Node* node() const;

  /**
   * The local Flow-protocol port on which this server is or was listening.  For a given Server_socket, this
   * will always return the same value, even if CLOSED.  However, when CLOSED, the port may be
   * unused or taken by another socket.
   *
   * @return See above.
   */
  flow_port_t local_port() const;

  /**
   * Non-blocking accept: obtain socket for the least recently established not-yet-obtained peer
   * connection on this server.  There is a queue (FIFO) of not-yet-claimed connections, and this
   * returns the one at the front of the queue.
   *
   * If state() is State::S_CLOSING or State::S_CLOSED (i.e., not State::S_LISTENING), this will return null (and an
   * error), even if connections have been queued up.  Rationale: BSD sockets act similarly: cannot
   * succeed with `accept(s)`, if `s` is not listening; also internal implementation is simpler.
   * Anti-rationale: our API is much more amenable to allowing accept()s in that situation; and it
   * would mirror the fact that Peer_socket::receive() can succeed in `S_OPEN+S_DISCONNECTING` state.
   * Why rationale > anti-rationale: it's a judgment call, and I'm going with simplicity of
   * implementation at least for now.
   *
   * @todo Reconsider allowing successful accept() in State::S_CLOSING state?
   *
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   *        Note: no available connections is not, in itself, an error.  So it's quite possible for
   *        null to be returned but `*err_code` is success.
   * @return A Peer_socket `sock` with `sock->state() == Peer_socket::State::S_OPEN`.  If no
   *         connections are available (including if `bool(*err_code) == true`), returns null pointer.
   */
  Peer_socket_ptr accept(Error_code* err_code = 0);

  /**
   * Blocking (synchronous) version of accept().  Acts just like accept(), except that if `*this` is
   * not immediately Acceptable (i.e., accept() would return null and no error), waits until it is
   * Acceptable (accept() would return either non-null, or null and an error) and returns
   * `accept(err_code)`.  In `reactor_pattern` mode (see arg doc), if it were to otherwise return a non-null
   * `Peer_socket::Ptr`, it instead leaves the ready peer socket available for subsequence acceptance and
   * returns null.
   *
   * If a timeout is specified, and this timeout expires before socket is
   * Acceptable, acts like accept() executed on an un-Acceptable server socket.
   *
   * ### Error handling ###
   * These are the possible outcomes.
   *   1. Connection succeeds before the given timeout expires (or succeeds, if no timeout given).
   *      Socket is at least Writable at time of return.  If `!reactor_pattern` then the new socket is returned,
   *      and no error is returned via `*err_code`; otherwise the socket is left available for acceptance, while
   *      null is returned, and (similarly to non-reactor-pattern mode) no error is returned via `*err_code`.
   *   2. Connection fails before the given timeout expires (or fails, if no timeout given).  null
   *      is returned, `*err_code` is set to reason for connection failure unless null.
   *      (If `err_code` null, Runtime_error thrown.)  The code error::Code::S_WAIT_INTERRUPTED means the
   *      wait was interrupted (similarly to POSIX's `EINTR`).
   *   3. A user timeout is given, and the connection does not succeed before it expires.
   *      Output semantics are the same as in 2, with the specific code error::Code::S_WAIT_USER_TIMEOUT.
   *      (Rationale: consistent with sync_receive(), sync_send() behavior.)
   *
   * Note that -- if `!reactor_pattern` -- it is NOT possible to return null and no error.
   *
   * Tip: Typical types you might use for `max_wait`: `boost::chrono::milliseconds`,
   * `boost::chrono::seconds`, `boost::chrono::high_resolution_clock::duration`.
   *
   * @see The version of sync_accept() with no timeout.
   * @tparam Rep
   *         See boost::chrono::duration documentation (and see above tip).
   * @tparam Period
   *         See boost::chrono::duration documentation (and see above tip).
   * @param max_wait
   *        The maximum amount of time from now to wait before giving up on the wait and returning.
   *        `"duration<Rep, Period>::max()"` will eliminate the time limit and cause indefinite wait
   *        (i.e., no timeout).
   * @param reactor_pattern
   *        If and only if `true`, and the call successfully waited until server socket became Acceptable, then
   *        we return a null pointer and leave the peer socket ready to be accepted by the caller.  If `false`, then
   *        in that same situation the socket is accepted and returned.  The parameter doesn't affect any other
   *        situations.
   * @param err_code
   *        See flow::Error_code docs for error reporting semantics.
   * @return Reference-counted pointer to new Server_socket; or an empty pointer (essentially null).
   *         Reminder that in `reactor_pattern` mode this may be null, yet indeed a socket is Acceptable
   *         (the presence or lack of an error indicates whether that's the case).
   */
  template<typename Rep, typename Period>
  Peer_socket_ptr sync_accept(const boost::chrono::duration<Rep, Period>& max_wait,
                              bool reactor_pattern = false,
                              Error_code* err_code = 0);

  /**
   * Equivalent to `sync_accept(duration::max(), reactor_pattern, err_code)`; i.e., sync_accept() with no user
   * timeout.
   *
   * @param err_code
   *        See other sync_accept().
   * @param reactor_pattern
   *        See other sync_accept().
   * @return See other sync_accept().
   */
  Peer_socket_ptr sync_accept(bool reactor_pattern = false, Error_code* err_code = 0);

  /**
   * The error code that perviously caused state() to become State::S_CLOSED, or success code if state
   * is not CLOSED.  Note that once it returns non-success, the returned value subsequently will always be the same.
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
   * @param logger
   *        The Logger implementation to use subsequently.
   * @param child_sock_opts
   *        Pointer to a per-socket options `struct` to copy and store; for each Peer_socket resulting
   *        from this Server_socket, the options will be a copy of this copy.  If null pointer, then
   *        instead the enclosing Node's global per-socket options will be used to produce the
   *        copy.
   */
  explicit Server_socket(log::Logger* logger, const Peer_socket_options* child_sock_opts);

private:
  // Friends.

  /**
   * See rationale for `friend`ing Node in class Server_socket documentation header.
   * @see Node.
   */
  friend class Node;

  // Types.

  /**
   * Short-hand for reentrant mutex type.  We explicitly rely on reentrant behavior, so this isn't "just in case."
   *
   * @todo This doc header for Server_socket::Mutex should specify what specific behavior requires mutex reentrance, so
   * that for example one could reevaluate whether there's a sleeker code pattern that would avoid it.
   */
  using Mutex = util::Mutex_recursive;

  /// Short-hand for RAII lock guard of #Mutex.
  using Lock_guard = util::Lock_guard<Mutex>;

  /* Methods: as few as possible.  Logic should reside in class Node as discussed (though
   * implementation may be in server_socket.cpp).  See rationale in class Server_socket
   * documentation header. */

  /**
   * Same as sync_accept() but uses a #Fine_clock-based #Fine_duration non-template type
   * for implementation convenience and to avoid code bloat to specify timeout.
   *
   * @param wait_until
   *        See `sync_accept(timeout)`.  This is the absolute time point corresponding to that.
   *        `"duration<Rep, Period>::max()"` maps to the value `Fine_time_pt()` (Epoch) for this argument.
   * @param reactor_pattern
   *        See sync_accept().
   * @param err_code
   *        See sync_accept().
   * @return See sync_accept().
   */
  Peer_socket_ptr sync_accept_impl(const Fine_time_pt& wait_until, bool reactor_pattern, Error_code* err_code);

  // Data.

  /**
   * Either null or the pointer to a copy of the template Peer_socket_options intended for resulting
   * Peer_socket objects.  Null means Peer_socket should use Node::options() as the template instead.
   *
   * Must be deleted in destructor if not null.
   */
  Peer_socket_options const * const m_child_sock_opts;

  /// See state().  Should be set before user gets access to `*this`.  Must not be modified by non-W threads.
  State m_state;

  /**
   * See node().  Should be set before user gets access to `*this` and not changed, except to 0 when
   * state is S_CLOSED.  Must not be modified by non-W threads.
   */
  Node* m_node;

  /**
   * See local_port().  Should be set before user gets access to `*this` and not changed afterwards.
   * @todo Make #m_local_port `const`?
   */
  flow_port_t m_local_port;

  /**
   * Queue of passively opened sockets in Peer_socket::Int_state::S_ESTABLISHED internal state that have not yet been
   * claimed by the user via `*accept()`.  `back()` is the next socket to be accepted (i.e., the one
   * that established connection longest ago).  This is not the more restricted `queue<>`,
   * because sometimes we want to remove things from the middle of it on error.  It is not a `list<>`, because
   * we sometimes (on error) need to erase items from the middle which requires a lookup by stored #Peer_socket_ptr
   * value which would be linear-time in a plain `list<>`.
   *
   * Write-accessible from thread W and user threads (in `accept()`) and must be protected by a mutex.
   *
   * @see Peer_socket::m_originating_serv for the closely related inverse.
   */
  util::Linked_hash_set<Peer_socket_ptr> m_unaccepted_socks;

  /**
   * The #Error_code causing this server's move from LISTENING state (if this has occurred); otherwise a
   * clear (success) #Error_code.  This starts as success and may move to one non-success value
   * and then never change after that.  Graceful closing of the server via `close()` is indeed counted as a
   * non-success value for #m_disconnect_cause.
   *
   * As in TCP net-stacks, one cannot recover from a transmission error or termination on the socket
   * (the "error" `EWOULDBLOCK`/`EAGAIN` does not count), which is why this can only go from success ->
   * non-success and never change after that.  (boost.asio error would be `would_block` or `try_again` when
   * I said `EWOUDLBLOCK`/`EAGAIN` informally there.)
   *
   * How to report this to the user: attempting to `*accept()` or other operations while
   * #m_disconnect_cause is not success => the operation returns this #Error_code to the user.
   * Note that even already queued acceptable sockets (#m_unaccepted_socks) will no longer be
   * available for acceptance.
   *
   * This should be success in LISTENING state and failure otherwise.
   *
   * ### Thread safety ###
   * Since user threads will access this at least via accept(), while thread W may
   * set it when implementing the server socket close, this must be protected by a mutex.
   */
  Error_code m_disconnect_cause;

  /**
   * This object's mutex.  The protected items are #m_state, #m_node,
   * #m_unaccepted_socks, `sock->m_originating_serv` for each `sock` in #m_unaccepted_socks, and
   * #m_disconnect_cause.
   */
  mutable Mutex m_mutex;

  /**
   * Set of passively opening sockets in pre-ESTABLISHED (so SYN_RCVD?) internal state (and thus
   * are not yet ready to be given out via `*accept()`).  This gains meaning only in thread W.  This
   * should NOT be accessed outside of thread W and is not protected my a mutex.
   *
   * Once a socket is acceptable (ESTABLISHED), it is moved from this to #m_unaccepted_socks, where
   * it can be claimed by the user.  Thus the user never accesses this, and it is maintained by
   * thread W only.
   *
   * @see Peer_socket::m_originating_serv for the closely related inverse.
   */
  boost::unordered_set<Peer_socket_ptr> m_connecting_socks;
}; // class Server_socket

// Free functions: in *_fwd.hpp.

// However the following refer to inner type(s) and hence must be declared here and not _fwd.hpp.

/**
 * Prints string representation of given socket state to given standard `ostream` and returns the
 * latter.
 *
 * @relatesalso Server_socket
 *
 * @param os
 *        Stream to print to.
 * @param state
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, Server_socket::State state);

// Template implementations.

template<typename Rep, typename Period>
Server_socket::Peer_socket_ptr Server_socket::sync_accept(const boost::chrono::duration<Rep, Period>& max_wait,
                                                          bool reactor_pattern, Error_code* err_code)
{
  assert(max_wait.count() > 0);
  return sync_accept_impl(util::chrono_duration_from_now_to_fine_time_pt(max_wait), reactor_pattern, err_code);
}

} // namespace flow::net_flow
