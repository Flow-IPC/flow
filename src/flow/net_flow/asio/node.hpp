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
#include "flow/net_flow/node.hpp"
#include "flow/net_flow/asio/peer_socket.hpp"
#include "flow/net_flow/asio/server_socket.hpp"
#include "flow/util/sched_task.hpp"

namespace flow::net_flow::asio
{
/**
 * A subclass of net_flow::Node that adds the ability to easily and directly use `net_flow` sockets in general
 * boost.asio event loops.
 *
 * net_flow::Node, the superclass, provides the vast bulk of the functionality in this library, but
 * the sockets it generates directly or indirectly (net_flow::Peer_socket, net_flow::Server_socket) cannot be used
 * directly in a boost.asio event loop (see reference below for background).  It is possible to use the
 * vanilla net_flow::Node, etc., with *any* asynchronous event loop (boost.asio or native `epoll` or `select()`...) --
 * notably by using a glue socket, as explained in Event_set doc header.  However, for those using the boost.asio
 * `io_context` as the app's core main loop, this is not as easy as what one might desire.  (Side note:
 * I personally strongly recommend using boost.asio event loops instead of rolling one's own or the many alternatives
 * I have seen.  It is also on its way to ending up in the C++ standard library.  Lastly, such a loop is used
 * internally to implement `net_flow`, so you know I like it, for what that's worth.)
 *
 * Basically, if you use this asio::Node instead of net_flow::Node, then you get everything in the latter plus
 * the ability to use Flow-protocol sockets `{Server|Peer}_socket` with boost.asio as easily as and similarly to how
 * one uses `boost::asio::ip::tcp::acceptor::async_*()` and `boost::asio::ip::tcp::socket::async_*()`, respectively.
 *
 * The usage pattern is simple, assuming you understand tha basics of net_flow::Node, net_flow::Server_socket, and
 * net_flow::Peer_socket -- and especially if you are familiar with boost.asio's built-in `tcp::acceptor` and
 * `tcp::socket` (and, of course, `boost::asio::io_context` that ties them and many more I/O objects together).
 *
 *   - First, simply create an asio::Node using the same form of constructor as you would with net_flow::Node, plus
 *     an arg that's a pointer to the subsequently used target boost.asio flow::util::Task_engine, which is memorized.
 *     - All facilities from the regular net_flow::Node are available on this object (plus some minor boost.asio ones).
 *   - Upon obtaining a net_flow::Server_socket::Ptr via listen(), cast it to its true polymorphic
 *     equivalent asio::Server_socket::Ptr, via convenience method net_flow::asio::Server_socket::cast():
 *     `auto serv = asio::Server_socket::cast(node.listen(...));`.
 *     - All facilities from the superclass net_flow::Server_socket are available (plus some minor boost.asio ones).
 *   - Similarly cast any net_flow::Peer_socket::Ptr to `asio` equivalent for any peer socket returned
 *     by asio::Server_socket::accept() and asio::Node::connect() and similar.
 *     - All facilities from the superclass net_flow::Peer_socket are available (plus some minor boost.asio ones).
 *   - All Server_socket and Peer_socket objects created by the Node subsequently will inherit the `Task_engine*`
 *     supplied in Node constructor.  However, you may overwrite these (or remove them, by setting to null) at any
 *     time via Node::set_async_task_engine(), Server_socket::set_async_task_engine(),
 *     Peer_socket::set_async_task_engine().
 *     - The only rule is this: `net_flow::asio::X::async_task_engine()` must not be null whenever one calls
 *       `net_flow::asio::X::async_F()`, where X is Node, Server_socket, or Peer_socket; and `async_F()` is some async
 *       operation added on top of base class `net_flow::X` (which typically features
 *       `net_flow::X::F()` and `net_flow::X::sync_F()` -- the non-blocking and thread-blocking versions of F,
 *       respectively).
 *     - The automatic inheriting of `Task_engine` pointer is as follows:
 *       `Node -listen-> Server_socket -accept-> Peer_socket <-connect- Node`.
 *       - For each op inside an "arrow," I mean to include all forms of it
 *         (`F` includes: `F()`, `sync_F()`, `async_F()`).
 *       - This pattern is more practical/reasonable for us than the boost.asio pattern of each object memorizing its
 *         `io_context` reference in its explicit constructor.  That is because we fully embrace the
 *         factory-of-shared-pointers pattern, and boost.asio doesn't use that pattern.  We also provide an explicit
 *         mutator however.
 *       - We also allow for a null `Task_engine*` -- except when `async_*()` is actually performed.
 *         (boost.asio I/O objects seem to require a real flow::util::Task_engine, even if not planning to do anything
 *         async.  Note, though, that if you don't plan to do anything async with `net_flow`, then you can just use
 *         vanilla net_flow::Node.  Bottom line, you have (on this topic) *at least* as much flexibility as with
 *         boost.asio built-in I/O objects.)
 *
 * Having thus obtained asio::Server_socket and asio::Peer_socket objects (and assigned each their desired
 * boost.asio `io_context` loop(s) -- essentially as one does with, e.g., `tcp::acceptor` and `tcp::socket`
 * respectively) -- you many now use the full range of I/O operations.  Here is a bird's eye view of all operations
 * available in the entire hierarchy.  Note that conceptually this is essentially identical to boost.asio `tcp`
 * I/O objects, but nomenclature and API decisions are in some cases somewhat different (I contend: with good reason).
 *
 *   - Non-blocking operations:
 *     - net_flow::Node::listen() -> net_flow::Server_socket: Create an object listening for connections.
 *       - Equivalent: `tcp::acceptor` constructor that takes a local endpoint arg.
 *     - net_flow::Server_socket::accept() -> net_flow::Peer_socket: Return a fully connected peer socket object,
 *       if one is immediately ready due to the listening done so far.
 *       - Equivalent: `tcp::acceptor::accept()` after ensuring `tcp::acceptor::non_blocking(true)`.
 *         NetFlow null return value <-> TCP "would-block" error code.
 *     - net_flow::Node::connect() -> net_flow::Peer_socket: Return a connectING peer socket object immediately,
 *       as it tries to connected to the server socket on the remote side.
 *       - Equivalent: `tcp::socket::connect()` after ensuring `tcp::socket::non_blocking(true)`.
 *       - Variations: see also `*_with_metadata()` for an added feature without direct TCP equivalent.
 *     - net_flow::Peer_socket::send(): Queue up at least 1 of the N given bytes to send to peer ASAP; or
 *       indicate no bytes could be *immediately* queued.
 *       - Equivalent: `tcp::socket::send()` after ensuring `tcp::socket::non_blocking(true)`.
 *         NetFlow 0 return value <-> TCP "would-block" error code.
 *     - net_flow::Peer_socket::receive(): Dequeue at least 1 of the desired N bytes earlier received from peer; or
 *       indicate none were *immediately* available.
 *       - Equivalent: `tcp::socket::receive()` after ensuring `tcp::socket::non_blocking(true)`.
 *         NetFlow 0 return value <-> TCP "would-block" error code.
 *   - Blocking operations:
 *     - net_flow::Server_socket::sync_accept() -> net_flow::Peer_socket: Return a fully connected peer socket object,
 *       blocking thread if necessary to wait for a connection to come in and fully establish.
 *       - Equivalent: `tcp::acceptor::accept()` after ensuring `tcp::acceptor::non_blocking(false)`.
 *       - Variations: optional timeout can be specified.
 *       - Variations: `reactor_pattern` mode, where the accept() call itself is left to aftermath of sync_accept().
 *     - net_flow::Node::sync_connect() -> net_flow::Peer_socket: Return a connected peer socket object,
 *       blocking thread as needed to reach the remote server and fully establish connection.
 *       - Equivalent: `tcp::socket::connect()` after ensuring `tcp::socket::non_blocking(false)`.
 *       - Variations: see also `*_with_metadata()` as above.
 *     - net_flow::Peer_socket::sync_send(): Queue up at least 1 of the N given bytes to send to peer ASAP,
 *       blocking thread if necessary to wait for this to become possible (internally, as determined by
 *       such things as flow control, congestion control, buffer size limits).
 *       - Equivalent: `tcp::socket::send()` after ensuring `tcp::socket::non_blocking(false)`.
 *       - Variations: optional timeout can be specified.
 *       - Variations: `null_buffers` mode, where the `send()` call itself is left to aftermath of sync_send().
 *     - net_flow::Peer_socket::sync_receive(): Dequeue at least 1 of the desired N bytes from peer,
 *       blocking thread if necessary to wait for this to arrive from said remote peer.
 *       - Equivalent: `tcp::socket::receive()` after ensuring `tcp::socket::non_blocking(false)`.
 *       - Variations: optional timeout can be specified.
 *       - Variations: `null_buffers` mode, where the `receive()` call itself is left to aftermath of sync_receive().
 *   - Asynchronous operations (`asio::*` classes provide these):
 *     - asio::Server_socket::async_accept() -> asio::Peer_socket: Obtain a fully connected peer socket object,
 *       waiting as necessary in background for a connection to come in and fully establish,
 *       then invoking user-provided callback as if by `post(io_context&)`.
 *       - Equivalent: `tcp::acceptor::async_accept()`.
 *       - Variations: optional timeout can be specified.
 *       - Variations: `reactor_pattern` mode, where the `accept()` call itself is left to user handler.
 *     - asio::Node::async_connect() -> net_flow::Peer_socket: Return a fully connected peer socket object,
 *       waiting as necessary in background to reach the remote server and fully establish connection,
 *       then invoking user-provided callback as if by `post(io_context&)`.
 *       - Equivalent: `tcp::socket::async_connect()`.
 *       - Variations: optional timeout can be specified.
 *       - Variations: see also `*_with_metadata()` as above.
 *     - asio::Peer_socket::async_send(): Queue up at least 1 of the N given bytes to send to peer ASAP,
 *       waiting as necessary in background for this to become possible (as explained above),
 *       then invoking user-provided callback as if by `post(io_context&)`.
 *       - Equivalent: `tcp::socket::async_send()`.
 *       - Variations: optional timeout can be specified.
 *       - Variations: `null_buffers` mode, where the `send()` call itself is left to user handler.
 *     - asio::Peer_socket::async_receive(): Dequeue at least 1 of the desired N bytes from peer,
 *       waiting as necessary in background for this to arrive from said remote peer,
 *       then invoking user-provided callback as if by `post(io_context&)`.
 *       - Equivalent: `tcp::socket::async_receive()`.
 *       - Variations: optional timeout can be specified.
 *       - Variations: `null_buffers` mode, where the `receive()` call itself is left to user handler.
 *   - Awaiting socket events (status):
 *     - Note that this is both a fundamental building block making much of the above work and simultaneously
 *       best to avoid using directly.  In particular, boost.asio is (subjectively speaking) a way to write a
 *       cleaner event loop, and "Asynchronous operations" above allow Flow-protocol use with boost.asio proactors.
 *     - Event_set::add_wanted_socket(), Event_set::remove_wanted_socket(), Event_set::swap_wanted_sockets(),
 *       Event_set::clear_wanted_sockets(), and related: Specify the precise socket(s) for which to check on
 *       particular status; and the status(es) for which to wait (e.g., Readable, Writable, Acceptable).
 *       - Equivalent (`select()`): `FD_SET()`, `FD_CLR()`, `FD_ZERO()`, etc.
 *       - Equivalent (`poll()`): The input events array.
 *     - Event_set::sync_wait(): Synchronous I/O multiplexing, with indefinite blocking or finite timeout blocking.
 *       - Equivalent: `select()` with null timeout (indefinite) or non-null, non-zero timeout (finite).
 *       - Equivalent: `poll()` with -1 timeout (indefinite) or positive timeout (finite).
 *     - Event_set::poll(): Non-blocking I/O multiplexing, with an instant check for the socket status(es) of interest.
 *       - Equivalent: `select()` with non-null, zero timeout.
 *       - Equivalent: `poll()` with 0 timeout.
 *     - Event_set::async_wait(): Asynchronous I/O multiplixing, like sync_wait() but waits in background and
 *       executes user-provided callback from an unspecified thread.  (A typical callback might set a `future`
 *       result; `post(io_context&)` a task to some boost.asio event loop; perhaps set a flag that is
 *       periodically checked by some user thread; or send a byte over some quick IPC mechanism like a POSIX
 *       domain socket or loopback UDP socket -- or even a condition variable.)
 *       - Equivalents: none in POSIX, that I know of.  Windows "overlapped" async I/O sounds vaguely like a distant
 *         cousin.  boost.asio `tcp::socket::async_*` (with `null_buffers` buffer argument if relevant) can simulate
 *         it also, but that's like using a modern PC to emulate an old graphing calculator... it's only
 *         conceivably useful if you need to work with some old-school 3rd party I/O library perhaps.
 *
 * ### Thread safety ###
 * See net_flow::Node thread safety notes.
 *
 * The one exception to the "concurrent write access to one Node is thread-safe without added locking" is
 * for set_async_task_engine().  See doc headers for async_task_engine() and set_async_task_engine() for more.
 *
 * ### Thread safety of destructor ###
 * See net_flow::Node thready safety notes.  Note, in particular, however, that as of this writing you may *not* let
 * asio::Node destruct if there are outstanding `{Node|Server|Peer}_socket::async_*()` operations.  The ability to
 * early-destruct during `sync_*()` does not extend to `async_*()`.
 *
 * @see boost.asio doc: https://www.boost.org/doc/libs/1_63_0/doc/html/boost_asio.html.
 * @see Superclass net_flow::Node, companion classes asio::Peer_socket and asio::Server_socket.
 *
 * @todo To enable reactor-style `async_*()` operations, meaning waiting for readability/writability/etc. but *not*
 * performing the actual operation before calling the user handler, we provide a `null_buffers`-style interface;
 * like newer boost.asio versions we should deprecate this in favor of simpler `async_wait()` APIs.
 * This would apply to net_flow::asio::Peer_socket and net_flow::asio::Server_socket APIs.
 * Similarly consider doing this for the `sync_*()` operations in the non-`asio` superclasses net_flow::Peer_socket
 * and net_flow::Server_socket.  Note that Event_set::async_wait() and Event_set::sync_wait() already exist; they
 * are powerful but a bit complex to use in these simple situations.  Hence the following hypothetical wrappers would
 * be welcome replacements for the deprecated `null_buffers` and "reactor-style" APIs in these classes:
 * `net_flow::Peer_socket::sync_wait(Event_set::Event_type)`,
 * `net_flow::asio::Peer_socket::async_wait(Event_set::Event_type)`,
 * `net_flow::Server_socket::sync_wait()`,
 * `net_flow::Server_socket::async_wait()`.  (Timeout-arg versions would be desired also, as they exist now.)
 *
 * @internal
 *
 * The decision to make set_async_task_engine() thread-unsafe, by refusing to put locking around internal uses of
 * #m_target_task_engine -- and in contrast to the rest of the main classes, which are generally thread-safe in
 * this sense -- was taken for the following reasons.
 *   1. Less mandatory locking = avoids potential performance loss.
 *   2. Realistically, set_async_task_engine() is not commonly used; and if it *is* used, it's tough to contrive
 *      a situation where one needs to call it *while* other threads are carrying on async operations -- so potential
 *      performance loss would be to provide a guarantee that wouldn't be very useful.
 *   3. Furthermore, it seems wrong to encourage wildly changing around the service throughout the socket's life.
 *      Just don't.
 *
 * There are counter-arguments.
 *   1. Saying Node is safe for concurrent access is easier and cleaner than saying that EXCEPT certain methods.
 *      - Well, that's a shame, but it's only a comment ultimately.
 *   2. If one *wanted* to use set_async_task_engine() concurrently with the methods against which it is
 *      thread-unsafe, they couldn't: async operations would require access to async_task_engine() during their
 *      background work, and there's no good way to let user lock around those parts.
 *      In other words, we're not merely leaving locking to user; no user locking solution could possibly fully work.
 *      - That's a true concern, but on balance it is defeated by reasons 2 and 3 above.  (Realistically -- doubt anyone
 *        would care; and don't want to encourage them to do strange stuff.)
 *
 * @todo The `sync_*()` early-destruction functionality explained in "Thread safety of destructor" above
 * could be extended to `async_*()`.  This is not a huge deal but would be nice for consistency.  Not allowing it
 * currently is really a result of internal implementation concerns (it is easier to not account for this corner case).
 * Alternatively, maybe we should go the other way and not support the somewhat-weird corner case in `sync_*()`.
 */
class Node :
  public net_flow::Node
{
public:
  // Constructors/destructor.

  /**
   * Constructs Node.  All docs from net_flow::Node::Node super-constructor apply.
   *
   * The difference is you may supply a flow::util::Task_engine.  More details below.
   *
   * @param logger
   *        See net_flow::Node::Node().
   * @param target_async_task_engine
   *        Initial value for async_task_engine().
   *        For the effect this has see async_task_engine() doc header.
   * @param low_lvl_endpoint
   *        See net_flow::Node::Node().
   * @param net_env_sim
   *        See net_flow::Node::Node().
   * @param err_code
   *        See net_flow::Node::Node().
   * @param opts
   *        See net_flow::Node::Node().
   */
  explicit Node(log::Logger* logger, util::Task_engine* target_async_task_engine,
                const util::Udp_endpoint& low_lvl_endpoint,
                Net_env_simulator* net_env_sim = 0, Error_code* err_code = 0,
                const Node_options& opts = Node_options());

  // Methods.

  /**
   * Pointer (possibly null) for the flow::util::Task_engine used by any coming async I/O calls and inherited by any
   * subsequently generated Server_socket and Peer_socket objects.
   *
   * One, this is used by `Node::async_*()` I/O calls, namely Node::async_connect() and all its variants, both
   * synchronously and during the async phases of such calls, whenever placing a user-supplied handler routine
   * onto an `Task_engine`.  Whatever async_task_engine() returns at that time is the `Task_engine` used.
   *
   * Two, when listen() creates Server_socket, or connect() creates Peer_socket, at that moment this is used.
   * (Recall: `[a]sync_connect()` and variants will all call connect() internally.)  Hence, the initial value
   * returned by Peer_socket::async_task_engine() and Server_socket::async_task_engine() is thus inherited from
   * the factory Node's async_task_engine() result.
   *
   * ### Thread safety (operating on a given Node) ###
   *
   *   - set_async_task_engine() is the only way to change the returned value, upon Node construction.
   *   - It is not safe to call set_async_task_engine() at the same time as async_task_engine().
   *   - Define "any Node async op" as Node::async_connect() and all its variants, including actions it takes
   *     in the background (asynchronously).
   *     - async_task_engine() must return the same value -- and *not* null -- throughout "any Node async op."
   *   - Define "any Node async inheriting op" as any non-blocking Node method that creates a Peer_socket or
   *     Server_socket (namely, connect(), listen(), and any variants).
   *     - Note that `[a]sync_connect()` (and variants) each invokes this non-blocking connect() at the start.
   *     - async_task_engine() must return the same value -- possibly null -- throughout "any Node async inheriting
   *       op."
   *   - Put simply, a null `Task_engine` can be inherited by a generated socket, but a null cannot be used when
   *     performing an async op.  Furthermore, set_async_task_engine() is unsafe through any of those and through
   *     async_task_engine() itself.
   *
   * Informal tip: Typically you'd never call set_async_task_engine(), hence there is no problem.  If you DO need to
   * call it, even then normally it's easy to ensure one does this before any actual async calls are made.
   * Trouble only begins if one calls set_async_task_engine() "in the middle" of operating the socket.
   * There is no way to add outside locking to make that work, either, due to async "tails" of some calls.
   *
   * @return Null or non-null pointer to flow::util::Task_engine.
   */
  util::Task_engine* async_task_engine();

  /**
   * Overwrites the value to be returned by next async_task_engine().
   *
   * See async_task_engine() doc header before using this.
   *
   * @param target_async_task_engine
   *        See async_task_engine().
   */
  void set_async_task_engine(util::Task_engine* target_async_task_engine);

  /**
   * The boost.asio asynchronous version of sync_connect(), performing any necessary wait and connection in the
   * background, and queueing the user-provided callback on the given boost.asio flow::util::Task_engine.
   * Acts just like connect() but instead of returning a connecting socket immediately, it returns and asycnhronously
   * waits until the initial handshake either succeeds or fails, and then invokes the given callback (in manner
   * equivalent to boost.asio `post(Task_engine&)`), passing to it the connected socket or null, respectively,
   * along with a success #Error_code or otherwise, respectively.  Additionally, you can specify a timeout; not
   * completing the connection by that time is a specific #Error_code.
   *
   * See subtleties about connect timeouts in Node::sync_connect() doc header.
   *
   * The following are the possible outcomes, in ALL cases ending with an `on_result()` invocation `post()`ed
   * onto `Task_engine* async_task_engine()` as follows:
   * `on_result(const Error_code& err_code, asio::Peer_socket::Ptr)`, with the 2 args determined as follows.
   *   1. Connection succeeds before the given timeout expires (or succeeds, if no timeout given).
   *      Socket is at least Writable at time of return.  The new socket and a success code are passed to
   *      callback.
   *   2. Connection fails before the given timeout expires (or fails, if no timeout given).
   *      null is passed in, and a non-success #Error_code is set to reason for connection failure and passed in.
   *      (Note that a built-in handshake timeout -- NOT the given user timeout, if any -- falls under this category.)
   *      #Error_code error::Code::S_WAIT_INTERRUPTED means the wait was interrupted (similarly to POSIX's `EINTR`).
   *   3. A user timeout is given, and the connection does not succeed before it expires.  Reported similarly
   *      to 2, with specific code error::Code::S_WAIT_USER_TIMEOUT.
   *      (Rationale: consistent with Server_socket::sync_accept(),
   *      Peer_socket::sync_receive(), Peer_socket::sync_send() behavior.)
   *
   * On success, the `asio::Peer_socket::Ptr sock` passed to `on_result()` will have
   * `sock->async_task_engine() == N->async_task_engine()`, where N is the current Node.  You can overwrite it
   * subsequently if desired.
   *
   * Tip: Typical types you might use for `max_wait`: `boost::chrono::milliseconds`,
   * `boost::chrono::seconds`, `boost::chrono::high_resolution_clock::duration`.
   *
   * Corner case: `*this` Node must exist throughout the async op, including when `on_result()` is actually
   * executed in the appropriate thread.
   *
   * error::Code other than success passed to `on_result()`:
   * error::Code::S_WAIT_INTERRUPTED, error::Code::S_WAIT_USER_TIMEOUT, error::Code::S_NODE_NOT_RUNNING,
   * error::Code::S_CANNOT_CONNECT_TO_IP_ANY, error::Code::S_OUT_OF_PORTS,
   * error::Code::S_INTERNAL_ERROR_PORT_COLLISION,
   * error::Code::S_CONN_TIMEOUT, error::Code::S_CONN_REFUSED,
   * error::Code::S_CONN_RESET_BY_OTHER_SIDE, error::Code::S_NODE_SHUTTING_DOWN,
   * error::Code::S_OPTION_CHECK_FAILED.
   *
   * @tparam Rep
   *         See `boost::chrono::duration` documentation (and see above tip).
   * @tparam Period
   *         See `boost::chrono::duration` documentation (and see above tip).
   * @tparam Handler
   *         A type such that if `Handler h`, then a function equivalent to `{ h(err_code, sock); }` can
   *         be `post()`ed onto an `Task_engine`, with `const Error_code& err_code` and `asio::Peer_socket::Ptr sock`.
   * @param to
   *        See connect().
   * @param on_result
   *        Callback whose invocation as explained above to add to `*async_task_engine()` as if by `post()`.
   *        Note: Use `bind_executor(S, F)` to bind your handler to the util::Strand `S`.
   * @param max_wait
   *        The maximum amount of time from now to wait before giving up on the wait and invoking `on_result()`
   *        with a timeout error code.
   *        `"duration<Rep, Period>::max()"` will eliminate the time limit and cause indefinite wait
   *        -- however, not really, as there is a built-in connection timeout that will expire.
   * @param opts
   *        See connect().
   */
  template<typename Rep, typename Period, typename Handler>
  void async_connect(const Remote_endpoint& to,
                     const Handler& on_result,
                     const boost::chrono::duration<Rep, Period>& max_wait,
                     const Peer_socket_options* opts = 0);

  /**
   * A combination of async_connect() and connect_with_metadata() (asynchronously blocking connect, with supplied
   * metadata).
   *
   * @tparam Rep
   *         See async_connect().
   * @tparam Period
   *         See async_connect().
   * @tparam Handler
   *         See async_connect().
   * @param to
   *        See async_connect().
   * @param on_result
   *        See async_connect().
   * @param max_wait
   *        See async_connect().
   * @param serialized_metadata
   *        See connect_with_metadata().
   * @param opts
   *        See async_connect().
   */
  template<typename Rep, typename Period, typename Handler>
  void async_connect_with_metadata(const Remote_endpoint& to,
                                   const Handler& on_result,
                                   const boost::chrono::duration<Rep, Period>& max_wait,
                                   const boost::asio::const_buffer& serialized_metadata,
                                   const Peer_socket_options* opts = 0);

  /**
   * Equivalent to `async_connect(to, on_result, duration::max(), opts)`; i.e., async_connect()
   * with no user timeout.
   *
   * @tparam Handler
   *         See other async_connect().
   * @param to
   *        See other async_connect().
   * @param on_result
   *        See other async_connect().
   * @param opts
   *        See other async_connect().
   */
  template<typename Handler>
  void async_connect(const Remote_endpoint& to,
                     const Handler& on_result,
                     const Peer_socket_options* opts = 0);

  /**
   * Equivalent to `async_connect_with_metadata(to, on_result, duration::max(),
   * serialized_metadata, opts)`; i.e., async_connect_with_metadata() with no user timeout.
   *
   * @tparam Handler
   *         See other async_connect_with_metadata().
   * @param to
   *        See other async_connect_with_metadata().
   * @param on_result
   *        See other async_connect_with_metadata().
   * @param serialized_metadata
   *        See other async_connect_with_metadata().
   * @param opts
   *        See other async_connect_with_metadata().
   */
  template<typename Handler>
  void async_connect_with_metadata(const Remote_endpoint& to,
                                   const Handler& on_result,
                                   const boost::asio::const_buffer& serialized_metadata,
                                   const Peer_socket_options* opts = 0);

private:
  // Friends.

  /// Peer_socket must be able to forward to async_op(), etc.
  friend class Peer_socket;
  /// Server_socket must be able to forward to async_op(), etc.
  friend class Server_socket;

  // Types.

  /// Short-hand for the `Task_engine`-compatible connect `Handler` concrete type for class-internal code.
  using Handler_func = Function<void (const Error_code& err_code, Peer_socket::Ptr new_sock)>;

  // Methods.

  /**
   * Implements superclass API.
   *
   * @param opts
   *        See superclass API.
   * @return See superclass API.
   */
  net_flow::Peer_socket* sock_create(const Peer_socket_options& opts) override;

  /**
   * Implements superclass API.
   *
   * @param child_sock_opts
   *        See superclass API.
   * @return See superclass API.
   */
  net_flow::Server_socket* serv_create(const Peer_socket_options* child_sock_opts) override;

  /**
   * Implementation of core asynchronous transfer methods, namely asio::Peer_socket::async_send(),
   * asio::Peer_socket::async_receive(), and asio::Server_socket::async_accept(), once the asio::Node has been
   * obtain from socket in any one of those methods.
   *
   * It is inspired by design of net_flow::Node::sync_op().
   *
   * Note the precise nature of `on_result` (see below).
   *
   * @tparam Socket
   *         Underlying object of the transfer operation (asio::Peer_socket or asio::Server_socket).
   * @tparam Base_socket
   *         Superclass of `Socket`.
   * @tparam Non_blocking_func_ret_type
   *         The return type of the calling transfer operation (`size_t` or asio::Peer_socket::Ptr).
   * @param sock
   *        Socket on which user called `async_*()`.
   * @param non_blocking_func
   *        When this method believes it should attempt a non-blocking transfer op, it will execute
   *        `non_blocking_func(&err_code)` with some `Error_code err_code` existing.
   *        If `non_blocking_func.empty()`, do not call `non_blocking_func()` --
   *        invoke `on_result()` indicating no error so far, and let
   *        them do actual operation, if they want; we just tell them it should be ready for them.  This is known
   *        as `null_buffers` mode or reactor pattern mode.  Otherwise, do the operation and then
   *        invoke `on_result()` with the resulting error code, possibly success.  This is the proactor pattern mode
   *        and arguably more typical.
   * @param would_block_ret_val
   *        The value that `non_blocking_func()` returns to indicate it was unable to perform the
   *        non-blocking operation (i.e., no data/sockets available).
   * @param ev_type
   *        Event type applicable to the type of operation this is.  See Event_set::Event_type doc header.
   * @param wait_until
   *        See `max_wait` argument on the originating `async_*()` method.  This is absolute timeout time point
   *        derived from it; zero-valued if no timeout.
   * @param on_result
   *        A function that will properly `post()` the original `on_result()` from the originating `async_*()` method
   *        onto `*async_task_engine()`.  This must be done carefully, preserving any associated executor of that
   *        original `on_result` handler.
   */
  template<typename Socket, typename Base_socket, typename Non_blocking_func_ret_type>
  void async_op(typename Socket::Ptr sock,
                Function<Non_blocking_func_ret_type (Error_code*)>&& non_blocking_func,
                Non_blocking_func_ret_type would_block_ret_val,
                Event_set::Event_type ev_type,
                const Fine_time_pt& wait_until,
                Function<void (const Error_code&, Non_blocking_func_ret_type)>&& on_result);

  /**
   * Implementation core of `async_connect*()` that gets rid of templated or missing arguments thereof.
   *
   * E.g., the API would wrap this and supply a Fine_duration instead of generic `duration`; and supply
   * `Fine_duration::max()` if user omitted the timeout argument; and convert a generic function object
   * into a concrete `Function<>` object.  Code bloat and possible circular definition issues are among the
   * reasons for this "de-templating" pattern.
   *
   * @param to
   *        See connect().
   * @param max_wait
   *        See the public `async_connect(timeout)`.  `"duration<Rep, Period>::max()"` maps to the value
   *        `Fine_duration::max()` for this argument.
   * @param serialized_metadata
   *        See connect_with_metadata().
   * @param opts
   *        See connect().
   * @param on_result
   *        `handler_func(on_result)`, where `on_result` is the user's `async_*()` method arg.
   */
  void async_connect_impl(const Remote_endpoint& to, const Fine_duration& max_wait,
                          const boost::asio::const_buffer& serialized_metadata,
                          const Peer_socket_options* opts,
                          Handler_func&& on_result);

  /**
   * Returns a functor that essentially performs `post()` `on_result` onto `*async_task_engine()` in a way suitable
   * for a boost.asio-compatible async-op.
   *
   * ### Rationale ###
   * See asio::Peer_socket::handler_func().
   *
   * @tparam Handler
   *         See async_connect().
   * @param on_result
   *        See async_connect().
   * @return Function to call from any context that will properly `post()` `on_result();` onto `*async_task_engine()`.
   */
  template<typename Handler>
  Handler_func handler_func(Handler&& on_result);

  // Data.

  /// See async_task_engine().
  util::Task_engine* m_target_task_engine;
};

// Template implementations.

template<typename Socket, typename Base_socket, typename Non_blocking_func_ret_type>
void Node::async_op(typename Socket::Ptr sock,
                    Function<Non_blocking_func_ret_type (Error_code*)>&& non_blocking_func,
                    Non_blocking_func_ret_type would_block_ret_val,
                    Event_set::Event_type ev_type,
                    const Fine_time_pt& wait_until,
                    Function<void (const Error_code&, Non_blocking_func_ret_type)>&& on_result)
{
  using boost::shared_ptr;
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using boost::asio::bind_executor;

  // We are in user thread U != W.

  /* We create an Event_set with just the one event we care about (e.g., sock is Writable) and async_wait() for it.
   * The handler for the latter can then perform the non-blocking operation (since the socket should be ready
   * due to the active event).  Finally, we would place the user's async handler onto the
   * boost.asio Task_engine user provided to `sock`.  on_result(), by contract, shall do that if we execute it
   * from any context; hence when needed we simply do that below.  See our doc header for a brief note on the
   * requirements on on_result() which allow us to do that.
   *
   * The other cases to cover:
   *   - If reactor_pattern, do not perform the non-blocking op (user probably wants to do so in on_result();
   *     and we only inform it that they should now be able to).  non_blocking_func can be garbage.
   *   - If interrupted (a-la EINTR), on_result() with the error code indicating interruption, no call
   *     to non-blocking op.
   *   - Timeout, if provided in wait_until, means we start a timer race against the async_wait(); if that
   *     wins, then stop the async_wait() and on_result() indicating timeout occurred (would-block result).
   *     It it loses, that timer is canceled at time of loss (async_wait() handler executing). */

  /* Unlike for sync_op() (as of this writing), API contract demands Node exist through all of the potential
   * background wait.  Thus, `this` Node will continue existing throughout, and we can (for example) log without
   * fear even after the wait finishes.  There is a @todo in class header for making it more like sync_op() in
   * this sense, but so far it hasn't seemed important enough to worry about it, consistency being the main
   * reason to do it.
   *
   * Unlike in sync_op(), nothing is locked currently.  So sock could become CLOSED between lines executing
   * below.  Remember that in below processing. */

  util::Task_engine& task_engine = *(sock->async_task_engine());

  // First create Event_set that'll await our one socket+status of interest.

  Error_code err_code;
  const Event_set::Ptr event_set = event_set_create(&err_code);
  if (!event_set)
  {
    on_result(err_code, would_block_ret_val); // It post()s user's originally-passed-in handler.
    // E.g., err_code might indicate Node is not running().
    return;
  }
  // else event_set ready.  In particular this means running() is true (it cannot become false up to ~Node()).

  FLOW_LOG_TRACE("Begin async op (identified by Event_set [" << event_set << "]) of type [" << ev_type << "] on "
                 "object [" << sock << "].");

  // Caution: remember event_set must be destroyed when async_op() finishes, synchronously or asynchronously.

  // We care about just this event of type ev_type on this socket.  Upcast is required for our use of boost::any().
  if (!(event_set->add_wanted_socket<Base_socket>(Base_socket::ptr_cast(sock), ev_type, &err_code)))
  {
    on_result(err_code, would_block_ret_val); // It post()s user's originally-passed-in handler.
    // Node must have shut down or something.  Pretty weird.  Code already logged.
    return;
  }
  // else go ahead and asynchronously wait.

  /* Timeout might be finite or infinite (non-existent).  Latter case is much simpler, but for brevity we mix the code
   * paths of the two cases. */
  const bool timeout_given = wait_until != Fine_time_pt();

  /* Explanation of why Strand is used / locking discussion:
   *
   * In the timeout case, we will be racing between a timer and the async_wait().  Their respective callbacks need
   * to go on some Task_engine.  The 2 services around are superclass Node's thread W event loop;
   * and the user-provided one task_engine.  Thread W is not suitable for 2 reasons: (1) in terms of clean design,
   * it's dirty, as we'd be getting into the superclass's dirty laundry -- asio::Node should be a clean addendum
   * to net_flow::node, for basic OOP hygiene; (2) thread W is not allowed to make external net_flow API calls itself;
   * most code assumes thread U != W.  So that leaves user-provided task_engine.  Using it is ideal, because
   * typically our internally placed handler will soon execute on_handler(), which we advertised is post()ed on
   * task_engine.  Only one subtle caveat is introduced: conceptually one handler must win the race against the
   * other, meaning execute serially before it -- for synchronization.  But post()ed handlers are only guaranteed
   * to execute serially if the Task_engine is run()ning in one thread: user is allowed by boost.asio to run it from 2+
   * (distributing tasks across threads).  We have 2 choices to deal make them serial anyway: use explicit
   * synchronization, or use Task_engine::strand to have Task_engine guarantee they can only run serially (choosing
   * thread accordingly if applicable -- probably internally by using a mutex anyway).  I use strand for a few reasons.
   * One, it's a bit easier to use-or-not-use a strand depending on a Boolean, as opposed to use-or-not-use a mutex;
   * in particular conditionally creating or not creating a strand seems a bit prettier than same with a mutex.
   * Two, conceivably the strand mechanism will allow Task_engine to use a thread normally blocked by a mutex lock wait
   * for something else, maybe another unrelated task might run instead.
   *
   * There is one more synchronization challenge.  2+ async_*() ops can run simultaneously on one socket object.
   * We access no shared data in such a case except for the short moment when we actually run the non-blocking op, when
   * events are ready simultanously for the 2+ async ops.  No mutex is needed, however, because net_flow::*_socket
   * are advertised as thread-safe for simultaneous method calls on one object. */
  using Strand = util::Strand;

  // This stuff is unused if !timeout_given.
  struct Timeout_state // Collection of state allocated on heap until the (subject-to-timeout) op completes.
  {
    util::Scheduled_task_handle sched_task;
    Strand m_make_serial;
    Timeout_state(util::Task_engine& svc) : m_make_serial(svc) {}
  };
  using Timeout_state_ptr = shared_ptr<Timeout_state>; // Use a ref-counted pointer to ensure it's around until done.
  Timeout_state_ptr timeout_state;

  if (timeout_given)
  {
    if (wait_until <= Fine_clock::now()) // A little optimization of a corner case; why not?
    {
      on_result(error::Code::S_WAIT_USER_TIMEOUT, would_block_ret_val); // post()s user's originally-passed-in handler.
      // Same as if wait actually occurred and timed out.
      return;
    }
    // else

    FLOW_LOG_TRACE("Timeout timer begin for async op [" << event_set << "] in "
                   "period [" << round<milliseconds>(wait_until - Fine_clock::now()) << "].");

    // Start one of the racers: the timer that'll fire once timeout is finished.

    // All the stuff needed by timeout can now be created (if !timeout_given, we save resources by not doing this).
    timeout_state.reset(new Timeout_state(task_engine));

    // Performance note: cannot move(on_result) here, as we still need on_result for 2nd closure made below.  Copy.
    timeout_state->sched_task
      = util::schedule_task_at(get_logger(), wait_until,
                               true, // Single-threaded!
                               &task_engine,
                               bind_executor(timeout_state->m_make_serial,
                                             [this, timeout_state, on_result, would_block_ret_val, event_set]
                                               (bool)
    {
      // We are in thread V != W.  V may or may not be U.  V is any thread where user's Task_engine is run()ning.

      FLOW_LOG_TRACE("[User event loop] "
                     "Timeout fired for async op [" << event_set << "]; clean up and report to user.");

      Error_code dummy_prevents_throw;
      event_set->async_wait_finish(&dummy_prevents_throw);
      event_set->close(&dummy_prevents_throw);

      FLOW_LOG_TRACE("[User event loop] User handler execution begins for async op [" << event_set << "].");
      on_result(error::Code::S_WAIT_USER_TIMEOUT, would_block_ret_val); // post()s user's originally-passed-in handler.

      FLOW_LOG_TRACE("[User event loop] User handler execution ends for async op [" << event_set << "].");
    })); // timer.async_wait() callback.
  } // if (timeout_given)

  // If there's a timeout, timer started for it.  Now prepare the mandatory Event_set::async_wait() for readiness.

  /* This will run as post()ed task on user's given target Task_engine.
   * Caution!  non_blocking_func is potentially destroyed (by move()) by the following assignment.
   * Caution!  Same with on_result.  We don't need on_result for any more closures below, so this is OK. */
  auto on_async_wait_user_loop
    = [this, sock, timeout_state, would_block_ret_val, ev_type, wait_until, event_set,
       non_blocking_func = std::move(non_blocking_func),
       on_result = std::move(on_result)]
        (bool interrupted)
        /* The two Function<>s may be move()d themselves inside, so those closure type members cannot be const.
         * Caution!  This pattern also makes on_async_wait_user_loop() a one-time-use closure. */
        mutable
  {
    // We are in thread V != W.  V may or may not be U.  V is any thread where user's Task_engine is run()ning.

    // Only watching for one event, so either interrupted is true, or the event is active.  Can thus clean this now.
    Error_code dummy_prevents_throw;
    event_set->close(&dummy_prevents_throw);

    if (timeout_state
        &&
        // We were racing timeout timer, and we may have won.  Check if indeed we won.
        (!util::scheduled_task_cancel(get_logger(), timeout_state->sched_task)))
    {
      /* Couldn't cancel it, so it *will* have run very soon.  We lost.
       * I didn't burrow in hard to see if this is really possible, but conceptually why not?  Be ready for it. */
      FLOW_LOG_INFO("[User event loop] "
                    "Events-ready in async op [" << event_set << "], but timeout already expired recently.  "
                    "Interesting timing coincidence.");
      return;
    }
    // else

    // There is no timeout, or there is but has not expired before us.  Is socket active, or were we interrupted?

    if (interrupted)
    {
      // Conceptually like EINTR.  Log and done.
      Error_code err_code_val;
      const auto err_code = &err_code_val;
      FLOW_ERROR_EMIT_ERROR(error::Code::S_WAIT_INTERRUPTED);

      FLOW_LOG_TRACE("[User event loop] "
                     "Events-ready in async op [" << event_set << "]; user handler execution begins.");
      on_result(err_code_val, would_block_ret_val); // It post()s user's originally-passed-in handler.
    }
    else
    {
      assert(!interrupted);

      /* OK.  async_wait() reports event is ready (sock is active, e.g., Writable).  Try to perform
       * non-blocking operation (e.g., Peer_socket::send()).  Another async_op() may asynchronously run a
       * non-blocking op at the same time from different thread, but no mutex needed as explained in long-ish
       * comment above. */

      Non_blocking_func_ret_type op_result;
      Error_code op_err_code;
      const bool reactor_pattern = non_blocking_func.empty(); // How this mode is indicated.

      if (reactor_pattern)
      {
        // They want to presumably perform the op themselves in handler.  Skip; would-block-no-error as advertised.
        assert(!op_err_code);

        FLOW_LOG_TRACE("[User event loop] "
                       "Events-ready in async op [" << event_set << "]; reactor pattern mode on; "
                       "user handler execution begins.");
        on_result(op_err_code, would_block_ret_val); // It post()s user's originally-passed-in handler.
      }
      else
      {
        assert(!reactor_pattern);

        FLOW_LOG_TRACE("[User event loop] "
                       "Events-ready in async op [" << event_set << "]; reactor pattern mode off; "
                       "executing non-blocking operation now.");
        op_result = non_blocking_func(&op_err_code); // send(), receive(), accept(), etc.

        if (op_err_code)
        {
          // Any number of errors possible here; error on socket => socket is active.

          FLOW_LOG_TRACE("[User event loop] "
                         "Error observed in instant op; code [" << op_err_code << '/' << op_err_code.message() << "]; "
                         "in async op [" << event_set << "]; "
                         "user handler execution begins.");
          on_result(op_err_code, would_block_ret_val); // It post()s user's originally-passed-in handler.
        }
        else
        {
          assert(!op_err_code);
          // No error, but did we get a result, or is it would-block?

          if (op_result == would_block_ret_val)
          {
            FLOW_LOG_TRACE("[User event loop] "
                           "Instant op yielded would-block despite events-ready "
                           "in async op [" << event_set << "]; are there concurrent competing operations?  "
                           "Trying async op again.");
            assert(!reactor_pattern); // We can't know whether it worked in reactor_pattern mode.  User finds out.

            /* If timeout_given, then effectively this reduces the time left in timeout and tries again
             * (time has passed; wait_until has remained constant).
             * If !timeout_given, then this will just keep trying forever, until the jerk in another thread, or
             * wherever, taking our stuff even when we get active events, stops jerking us around. */
            async_op
              <Socket, Base_socket, Non_blocking_func_ret_type>
              (sock, std::move(non_blocking_func),
               would_block_ret_val, ev_type, wait_until, std::move(on_result));
            // on_result, non_blocking_func are now potentially destroyed, but we don't need them anymore:
            return;
          }
          // else got actual result!

          FLOW_LOG_TRACE("[User event loop] "
                         "Instant op yielded positive result [" << op_result << "] "
                         "in async op [" << event_set << "]; "
                         "user handler execution begins.");

          assert(op_result != would_block_ret_val);
          assert(!op_err_code);
          on_result(op_err_code, op_result); // It post()s user's originally-passed-in handler.
        } // else if (!op_err_code)
      } // else if (!reactor_pattern)
    }

    // Note all paths lead here, except the `return;` above for when we try again.
    FLOW_LOG_TRACE("[User event loop] User handler execution ends for async op [" << event_set << "].");
  }; //  on_async_wait_user_loop =

  /* Now begin the wait for readiness.
   * Aside: There are strong reasons not to do anything but post(), directly in the thread W-executed handler, and
   * they are discussed above.  However, in any case, note that async_wait() API doc says handler
   * must not itself call anything related to the entire Node from which the Event_set comes.
   * Our choosing to post() elsewhere is a very vanilla thing to do in an async_wait() handler per its docs. */

  if (timeout_given)
  {
    // As above, `mutable` so that the captured closure can be move()d inside (no more than once ever).
    event_set->async_wait([sock, timeout_state,
                           on_async_wait_user_loop = std::move(on_async_wait_user_loop)]
                          (bool interrupted) mutable
    {
      /* We are in thread W.  Put the handler into user's event loop, as discussed, but make it serial with timeout.
       * Note: if Task_engine E and Strand S(E), then post(E, b_e(S, f)) is an anti-pattern for subtle reasons, though
       * in our case "tie-breaking" order doesn't matter, hence at worst it would only be slower.  Still, use the proper
       * pattern: post(S, f).  Note on note: <some async op on I/O object associated with E>(..., b_e(S, f)) is just
       * fine and in fact we do that elsewhere in this function. */
      post(timeout_state->m_make_serial, [interrupted,
                                          on_async_wait_user_loop = std::move(on_async_wait_user_loop)]
                                           () mutable
      {
        on_async_wait_user_loop(interrupted); // (This call mutates on_async_wait_user_loop closure *itself*.)
      });
      // BTW strand docs say strand need only exist through the post() call itself.  So this async path is done w/ it.
    });
  }
  else
  {
    /* Simpler case; no race; so no strand to use.  Keeping comments light.
     * @todo Subtlety: There's no race, so in effect on_async_wait_user_loop() does some non-blocking stuff and
     * executes on_result() -- which actually post()s, through the proper executor, onto *async_task_engine().
     * If Event_set::async_wait() had a variant (maybe asio::Event_set::async_wait()?) that is a boost.asio-compatible
     * async-op, in that it respects any associated executor for its completion handler, then it would be arguably
     * a worth-while goal (for perf/to reduce asynchronicity) to grab the associated executor from the user's
     * original completion handler and apply it to the arg given to the async_wait().  The to-do is fairly ambitious,
     * definitely, but I (ygoldfel) leave it in as food for thought.  Note, though, that I am not suggesting
     * we do it in the timeout-race case above, as that involves scheduling a parallel timer... the implications of
     * letting their executor (strand, or who knows what else) control our internal algorithm -- too risky/unclear. */
    event_set->async_wait([sock, on_async_wait_user_loop = std::move(on_async_wait_user_loop)]
                            (bool interrupted) mutable
    {
      // No need to worry about making it serial with anything else.
      post(*sock->async_task_engine(), [interrupted,
                                        on_async_wait_user_loop = std::move(on_async_wait_user_loop)]
                                         () mutable
      {
        on_async_wait_user_loop(interrupted);
      });
    });
  }
} // Node::async_op()

template<typename Rep, typename Period, typename Handler>
void Node::async_connect(const Remote_endpoint& to,
                         const Handler& on_result,
                         const boost::chrono::duration<Rep, Period>& max_wait,
                         const Peer_socket_options* opts)
{
  using boost::asio::buffer;

  async_connect_impl(to, max_wait,
                     buffer(&S_DEFAULT_CONN_METADATA, sizeof(S_DEFAULT_CONN_METADATA)),
                     opts,
                     handler_func(on_result));
}

template<typename Handler>
void Node::async_connect(const Remote_endpoint& to,
                         const Handler& on_result,
                         const Peer_socket_options* opts)
{
  using boost::asio::buffer;

  async_connect_impl(to, Fine_duration::max(),
                     buffer(&S_DEFAULT_CONN_METADATA, sizeof(S_DEFAULT_CONN_METADATA)),
                     opts,
                     handler_func(on_result));
}

template<typename Rep, typename Period, typename Handler>
void Node::async_connect_with_metadata(const Remote_endpoint& to,
                                       const Handler& on_result,
                                       const boost::chrono::duration<Rep, Period>& max_wait,
                                       const boost::asio::const_buffer& serialized_metadata,
                                       const Peer_socket_options* opts)
{
  async_connect_impl(to, max_wait, serialized_metadata,
                     opts,
                     handler_func(on_result));
}

template<typename Handler>
void Node::async_connect_with_metadata(const Remote_endpoint& to,
                                       const Handler& on_result,
                                       const boost::asio::const_buffer& serialized_metadata,
                                       const Peer_socket_options* opts)
{
  async_connect_impl(to, Fine_duration::max(), serialized_metadata,
                     opts,
                     handler_func(on_result));
}

template<typename Handler>
Node::Handler_func Node::handler_func(Handler&& on_result)
{
  using boost::asio::post;
  using boost::asio::bind_executor;
  using boost::asio::get_associated_executor;

  /* This mirrors Peer_socket::handler_func() exactly (just different signature on on_result()).  Comments light.
   * @todo Maybe there's a way to generalize this with template param-packs or something. */

  return [this, on_result = std::move(on_result)]
           (const Error_code& err_code, Peer_socket::Ptr new_sock)
           mutable
  {
    const auto executor = get_associated_executor(on_result);
    post(*(async_task_engine()),
         bind_executor(executor,
                       [err_code, new_sock, on_result = std::move(on_result)]
    {
      on_result(err_code, new_sock);
    }));
  };
} // Node::handler_func()

} // namespace flow::net_flow::asio
