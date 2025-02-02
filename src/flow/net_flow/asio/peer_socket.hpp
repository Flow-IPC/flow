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
#include "flow/net_flow/peer_socket.hpp"

namespace flow::net_flow::asio
{

/**
 * A net_flow::Peer_socket that adds integration with boost.asio.  See net_flow::Node for a discussion of the topic of
 * boost.asio integration.  A net_flow::asio::Peer_socket *is* a net_flow::Peer_socket with all that functionality, plus
 * APIs (such as async_send(), async_receive()) that accomplish boost.asio-style asynchronous operations with a
 * standard `boost::asio::io_context` (a/k/a flow::util::Task_engine).
 *
 * As of this writing, `asio::Peer_socket::Ptr`s are generated by `asio::Node::connect()`,
 * `asio::Server_socket::accept()`, and their derivatives.  The underlying `io_context` (a/k/a flow::util::Task_engine)
 * is carried forward from the factory object that generated and returned the `Ptr`.  It can be overwritten via
 * set_async_task_engine().
 */
class Peer_socket :
  public net_flow::Peer_socket
  // Note: can't derive util::Shared_ptr_alias_holder<>, because superclass already did it (doc header disallows this).
{
public:
  // Types.

  /// Short-hand for `shared_ptr` to Peer_socket.
  using Ptr = boost::shared_ptr<Peer_socket>;

  // Constructors/destructor.

  /// Boring `virtual` destructor as in superclass.  See notes there.
  ~Peer_socket() override;

  // Methods.

  /**
   * Pointer (possibly null) for the flow::util::Task_engine used by any coming `async_*()` I/O calls.
   *
   * ### Thread safety (operating on a given Node) ###
   *
   *   - set_async_task_engine() is the only way to change the returned value, upon construction.
   *   - It is not safe to call set_async_task_engine() at the same time as async_task_engine().
   *   - Define "any async op" as `async_*()` in this class, including actions it takes
   *     in the background (asynchronously).
   *     - async_task_engine() must return the same value -- and *not* null -- throughout "any async op."
   *   - Put simply, a null `Task_engine` is allowed, but a null cannot be used when
   *     performing an async op.  Furthermore, set_async_task_engine() is unsafe through any of those and through
   *     async_task_engine() itself.
   *
   * Informal tip: Same as in doc header for Node::async_task_engine().
   *
   * @return Null or non-null pointer to flow::util::Task_engine.
   */
  util::Task_engine* async_task_engine();

  /**
   * Read-only version of async_task_engine().
   *
   * @return Reference to read-only `*(async_task_engine())`.
   */
  const util::Task_engine& async_task_engine_cref() const;

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
   * boost.asio-style asynchronous version that essentially performs non-`nullptr_t`
   * net_flow::Peer_socket::sync_receive() in the background and invokes the given handler via the saved
   * `Task_engine *(async_task_engine())`, as if by `post(Task_engine&)`.
   *
   * The semantics are identical to the similar `sync_receive()`, except that the operation does not block the
   * calling thread; and the results are delivered to the supplied handler `on_result` instead of to the caller.
   *
   * @tparam Rep
   *         See net_flow::Peer_socket::receive().
   * @tparam Period
   *         See net_flow::Peer_socket::receive().
   * @tparam Mutable_buffer_sequence
   *         See net_flow::Peer_socket::receive().
   * @tparam Handler
   *         A type such that if `Handler h`, then a function equivalent to `{ h(err_code, n); }` can
   *         be `post()`ed onto an `Task_engine`, with `const Error_code& err_code` and `size_t n`.
   * @param target
   *        See net_flow::Peer_socket::receive().
   * @param on_result
   *        Handler to be executed asynchronously within the saved `Task_engine`.
   *        The error code and bytes-transmitted values passed to it, in that order, are identical
   *        to those out-arg/returned values in net_flow::Peer_socket::sync_receive().
   *        Note: This overload corresponds to the non-`nullptr_t` `sync_receive()` overload.
   *        Note: Use `bind_executor(S, F)` to bind your handler to the util::Strand `S`.
   * @param max_wait
   *        See net_flow::Peer_socket::sync_receive().
   */
  template<typename Rep, typename Period, typename Mutable_buffer_sequence, typename Handler>
  void async_receive(const Mutable_buffer_sequence& target,
                     const boost::chrono::duration<Rep, Period>& max_wait,
                     Handler&& on_result);

  /**
   * boost.asio-style asynchronous version that essentially performs a `nullptr_t`
   * net_flow::Peer_socket::sync_receive() in the background and invokes the given handler via the saved
   * `Task_engine *(async_task_engine())`, as if by `post(Task_engine&)`.
   *
   * The semantics are identical to the similar `sync_receive()`, except that the operation does not block the
   * calling thread; and the results are delivered to the supplied handler `on_result` instead of to the caller.
   *
   * @tparam Rep
   *         See net_flow::Peer_socket::receive().
   * @tparam Period
   *         See net_flow::Peer_socket::receive().
   * @tparam Handler
   *         A type such that if `Handler h`, then a function equivalent to `{ h(err_code, n); }` can
   *         be `post()`ed onto an `Task_engine`, with `const Error_code& err_code` and `size_t n`.
   * @param on_result
   *        Handler to be executed asynchronously within the saved `Task_engine`.
   *        The error code and bytes-transmitted values passed to it, in that order, are identical
   *        to those out-arg/returned values in net_flow::Peer_socket::sync_receive().
   *        Note: This overload corresponds to the `nullptr_t` `sync_receive()` overload.
   *        Therefore, the bytes-transmitted value passed to the handler is always 0.
   *        Note: Use `bind_executor(S, F)` to bind your handler to the util::Strand `S`.
   * @param max_wait
   *        See net_flow::Peer_socket::sync_receive().
   */
  template<typename Rep, typename Period, typename Handler>
  void async_receive(std::nullptr_t,
                     const boost::chrono::duration<Rep, Period>& max_wait,
                     Handler&& on_result);

  /**
   * Equivalent to `async_receive(target, duration::max(), on_result)`; i.e., `async_receive()`
   * with no timeout.
   *
   * @tparam Mutable_buffer_sequence
   *         See other async_receive().
   * @tparam Handler
   *         See other async_receive().
   * @param target
   *        See other async_receive().
   * @param on_result
   *        See other async_receive().
   */
  template<typename Mutable_buffer_sequence, typename Handler>
  void async_receive(const Mutable_buffer_sequence& target, Handler&& on_result);

  /**
   * Equivalent to `async_receive(nullptr, duration::max(), on_result)`; i.e., `async_receive(nullptr)`
   * with no timeout.
   *
   * @tparam Handler
   *         See other async_receive().
   * @param on_result
   *        See other async_receive().
   */
  template<typename Handler>
  void async_receive(std::nullptr_t, Handler&& on_result);

  /**
   * boost.asio-style asynchronous version that essentially performs non-`nullptr_t`
   * net_flow::Peer_socket::sync_send() in the background and invokes the given handler via the saved
   * `Task_engine *(async_task_engine())`, as if by `post(Task_engine&)`.
   *
   * The semantics are identical to the similar `sync_send()`, except that the operation does not block the
   * calling thread; and the results are delivered to the supplied handler `on_result` instead of to the caller.
   *
   * @tparam Rep
   *         See net_flow::Peer_socket::send().
   * @tparam Period
   *         See net_flow::Peer_socket::send().
   * @tparam Const_buffer_sequence
   *         See net_flow::Peer_socket::send().
   * @tparam Handler
   *         A type such that if `Handler h`, then a function equivalent to `{ h(err_code, n); }` can
   *         be `post()`ed onto an `Task_engine`, with `const Error_code& err_code` and `size_t n`.
   * @param source
   *        See net_flow::Peer_socket::send().
   * @param on_result
   *        Handler to be executed asynchronously within the saved `Task_engine`.
   *        The error code and bytes-transmitted values passed to it, in that order, are identical
   *        to those out-arg/returned values in net_flow::Peer_socket::sync_send().
   *        Note: This overload corresponds to the non-`nullptr_t` `sync_send()` overload.
   *        Note: Use `bind_executor(S, F)` to bind your handler to the util::Strand `S`.
   * @param max_wait
   *        See net_flow::Peer_socket::sync_send().
   */
  template<typename Rep, typename Period, typename Const_buffer_sequence, typename Handler>
  void async_send(const Const_buffer_sequence& source,
                  const boost::chrono::duration<Rep, Period>& max_wait,
                  Handler&& on_result);

  /**
   * boost.asio-style asynchronous version that essentially performs a `nullptr_t`
   * net_flow::Peer_socket::sync_send() in the background and invokes the given handler via the saved
   * `Task_engine *(async_task_engine())`, as if by `post(Task_engine&)`.
   *
   * The semantics are identical to the similar `sync_send()`, except that the operation does not block the
   * calling thread; and the results are delivered to the supplied handler `on_result` instead of to the caller.
   *
   * @tparam Rep
   *         See net_flow::Peer_socket::send().
   * @tparam Period
   *         See net_flow::Peer_socket::send().
   * @tparam Handler
   *         A type such that if `Handler h`, then a function equivalent to `{ h(err_code, n); }` can
   *         be `post()`ed onto an `Task_engine`, with `const Error_code& err_code` and `size_t n`.
   * @param on_result
   *        Handler to be executed asynchronously within the saved `Task_engine`.
   *        The error code and bytes-transmitted values passed to it, in that order, are identical
   *        to those out-arg/returned values in net_flow::Peer_socket::sync_send().
   *        Note: This overload corresponds to the `nullptr_t` `sync_send()` overload.
   *        Therefore, the bytes-transmitted value passed to the handler is always 0.
   *        Note: Use `bind_executor(S, F)` to bind your handler to the util::Strand `S`.
   * @param max_wait
   *        See net_flow::Peer_socket::sync_send().
   */
  template<typename Rep, typename Period, typename Handler>
  void async_send(std::nullptr_t,
                  const boost::chrono::duration<Rep, Period>& max_wait,
                  Handler&& on_result);

  /**
   * Equivalent to `async_send(target, duration::max(), on_result)`; i.e., `async_send()`
   * with no timeout.
   *
   * @tparam Const_buffer_sequence
   *         See other async_send().
   * @tparam Handler
   *         See other async_send().
   * @param source
   *        See other async_send().
   * @param on_result
   *        See other async_send().
   */
  template<typename Const_buffer_sequence, typename Handler>
  void async_send(const Const_buffer_sequence& source, Handler&& on_result);

  /**
   * Equivalent to `async_send(nullptr, duration::max(), on_result)`; i.e., `async_send(nullptr)`
   * with no timeout.
   *
   * @tparam Handler
   *         See other async_send().
   * @param on_result
   *        See other async_send().
   */
  template<typename Handler>
  void async_send(std::nullptr_t, Handler&& on_result);

  /**
   * Convenience method that polymorphically casts from `net_flow::Peer_socket::Ptr` to
   * subclass pointer `net_flow::asio::Peer_socket::Ptr`.
   * Behavior undefined if `sock` is not actually of the latter type.
   * With this, one needn't do verbose `static_[pointer_]cast<>` or similar conversions.
   *
   * @param sock
   *        Handle to a `sock` such that the true underlying concrete type is net_flow::asio::Peer_socket.
   * @return See above.
   */
  static Ptr cast(net_flow::Peer_socket::Ptr sock);

private:

  // Friends.

  /// Similarly to the equivalent `friend` in net_flow::Peer_socket.
  friend class Node;
  /// As of this writing, it is needed for Node::sock_create_forward_plus_ctor_args().
  friend class net_flow::Node;

  // Friend of Peer_socket: For access to private alias Int_state.
  friend std::ostream& operator<<(std::ostream& os, const Peer_socket* sock);

  // Types.

  /// Short-hand for the `Mutable_buffer_sequence` concrete type for class-internal code.
  using Target_bufs = std::vector<boost::asio::mutable_buffer>;
  /// Short-hand for a low-cost-copyable smart pointer of `Target_bufs`.
  using Target_bufs_ptr = boost::shared_ptr<Target_bufs>;
  /// Short-hand for the `Const_buffer_sequence` concrete type for class-internal code.
  using Source_bufs = std::vector<boost::asio::const_buffer>;
  /// Short-hand for a low-cost-copyable smart pointer of `Source_bufs`.
  using Source_bufs_ptr = boost::shared_ptr<Source_bufs>;
  /// Short-hand for the `Task_engine`-compatible send and receive `Handler` concrete type for class-internal code.
  using Handler_func = Function<void (const Error_code& err_code, size_t op_result)>;

  // Constructors/destructor.

  /**
   * Constructs object.
   *
   * @param logger_ptr
   *        See superclass.
   * @param task_engine
   *        See superclass.
   * @param opts
   *        See superclass.
   */
  explicit Peer_socket(log::Logger* logger_ptr,
                       util::Task_engine* task_engine,
                       const Peer_socket_options& opts);

  // Methods.

  /**
   * De-templated implementation of all `async_receive()` methods.
   *
   * @param target
   *        Ref-counted pointer to copy of `target` arg for non-`nullptr_t` `async_receive()` methods;
   *        or null in the case of `async_receive(nullptr_t)` methods.
   * @param on_result
   *        `handler_func(on_result)`, where `on_result` is the user's `async_*()` method arg.
   * @param wait_until
   *        See `max_wait` arg on the originating `async_receive()` method.  This is absolute timeout time point
   *        derived from it; zero-valued if no timeout.
   */
  void async_receive_impl(Target_bufs_ptr target, Handler_func&& on_result,
                          const Fine_time_pt& wait_until);

  /**
   * De-templated implementation of all `async_send()` methods.
   *
   * @param source
   *        Ref-counted pointer to copy of `source` arg for non-`nullptr_t` `async_send()` methods;
   *        or an empty sequence in the case of `async_send(nullptr_t)` methods.
   * @param on_result
   *        `handler_func(on_result)`, where `on_result` is the user's `async_*()` method arg.
   * @param wait_until
   *        See `max_wait` arg on the originating `async_send()` method.  This is absolute timeout time point
   *        derived from it; zero-valued if no timeout.
   */
  void async_send_impl(Source_bufs_ptr source, Handler_func&& on_result,
                       const Fine_time_pt& wait_until);

  /**
   * Helper that returns the `net_flow::asio::Node` that generated `*this`; unless `*this` is closed;
   * in which case it returns null after posting an error-case invocation of the user handler `on_result`.
   *
   * @param on_result
   *        `handler_func(on_result)`, where `on_result` is the user's `async_*()` method arg.
   *        This will be executed if and only if null is returned.  The reason for why the latter occurred is passed as
   *        the error code argument to `on_result()`.
   * @return Non-null pointer to owning Node, if and only if it is available.
   */
  Node* node_or_post_error(Handler_func&& on_result);

  /**
   * Returns a functor that essentially performs `post()` `on_result` onto `*async_task_engine()` in a way suitable
   * for a boost.asio-compatible async-op.
   *
   * ### Rationale ###
   * Why not simply create use `Handler_func(on_result)`?  After all they have the same signature, so if the
   * idea is to de-template our internal implementation of the various `async_*()` APIs, then that would be sufficient.
   * Answer:
   *
   * We are aiming to implement a boost.asio-compatible async-op, so one way or another we'll need to `post()`
   * a no-arg wrapper of `on_result(...);`.  handler_func() returning a function that does that alone would
   * be potentially useful in its own right.  That, however, is still insufficient -- though that may not be obvious
   * even to experienced boost.asio *users*.  `Handler` and `on_result` may be associated with an executor,
   * such as a util::Strand, which a proper boost.asio async-op API must respect when `post()`ing it onto the
   * util::Task_engine.  handler_func() takes care of that too (details omitted here; see body).
   *
   * Note: Failing to do the executor stuff will build, and work -- but it won't work "all the way," if `on_result`
   * is bound to an executor like a strand.  In the latter case it would result in executing `on_result()` outside
   * the associated strand -- and hence potential thread-unsafety.
   *
   * @tparam Handler
   *         See async_receive() and others.
   * @param on_result
   *        See async_receive() and others.
   * @return Function to call from any context that will properly `post()` `on_result();` onto `*async_task_engine()`.
   */
  template<typename Handler>
  Handler_func handler_func(Handler&& on_result);

  // Data.

  /// See async_task_engine().
  util::Task_engine* m_target_task_engine;
}; // class asio::Peer_socket

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Rep, typename Period, typename Mutable_buffer_sequence, typename Handler>
void Peer_socket::async_receive(const Mutable_buffer_sequence& target,
                                const boost::chrono::duration<Rep, Period>& max_wait,
                                Handler&& on_result)
{
  assert(target.begin() != target.end());
  async_receive_impl(Target_bufs_ptr(new Target_bufs(target.begin(), target.end())),
                     handler_func(on_result),
                     util::chrono_duration_from_now_to_fine_time_pt(max_wait));
}

template<typename Rep, typename Period, typename Handler>
void Peer_socket::async_receive(std::nullptr_t,
                                const boost::chrono::duration<Rep, Period>& max_wait,
                                Handler&& on_result)
{
  async_receive_impl(Target_bufs_ptr(), handler_func(on_result),
                     util::chrono_duration_from_now_to_fine_time_pt(max_wait));
}

template<typename Mutable_buffer_sequence, typename Handler>
void Peer_socket::async_receive(const Mutable_buffer_sequence& target,
                                Handler&& on_result)
{
  assert(target.begin() != target.end());
  async_receive_impl(Target_bufs_ptr(new Target_bufs(target.begin(), target.end())),
                     handler_func(on_result), Fine_time_pt());
}

template<typename Handler>
void Peer_socket::async_receive(std::nullptr_t, Handler&& on_result)
{
  async_receive_impl(Target_bufs_ptr(), handler_func(on_result), Fine_time_pt());
}

template<typename Rep, typename Period, typename Const_buffer_sequence, typename Handler>
void Peer_socket::async_send(const Const_buffer_sequence& source,
                             const boost::chrono::duration<Rep, Period>& max_wait,
                             Handler&& on_result)
{
  assert(source.begin() != source.end());
  async_send_impl(Source_bufs_ptr(new Source_bufs(source.begin(), source.end())),
                  handler_func(on_result),
                  util::chrono_duration_from_now_to_fine_time_pt(max_wait));
}

template<typename Rep, typename Period, typename Handler>
void Peer_socket::async_send(std::nullptr_t,
                             const boost::chrono::duration<Rep, Period>& max_wait,
                             Handler&& on_result)
{
  async_send_impl(Source_bufs_ptr(), handler_func(on_result),
                  util::chrono_duration_from_now_to_fine_time_pt(max_wait));
}

template<typename Const_buffer_sequence, typename Handler>
void Peer_socket::async_send(const Const_buffer_sequence& source,
                             Handler&& on_result)
{
  assert(source.begin() != source.end());
  async_send_impl(Source_bufs_ptr(new Source_bufs(source.begin(), source.end())),
                  handler_func(on_result), Fine_time_pt());
}

template<typename Handler>
void Peer_socket::async_send
                    (std::nullptr_t, Handler&& on_result)
{
  async_send_impl(Source_bufs_ptr(), handler_func(on_result), Fine_time_pt());
}

template<typename Handler>
Peer_socket::Handler_func Peer_socket::handler_func(Handler&& on_result)
{
  using boost::asio::post;
  using boost::asio::bind_executor;
  using boost::asio::get_associated_executor;

  /* Necessary background: See our doc header Rationale section.
   *
   * As stated there, we have 2 interrelated jobs:
   *   - post() on_result() onto *async_task_engine().  Note on_result() takes some args, whereas anything post()ed
   *     takes no args.
   *   - When doing said post(), do it through the executor associated with `on_result`.
   *     There are 2 common situations (though not the only ones possible) worth contemplating:
   *     - If on_result is a "vanilla" handler without anything fancy, get_associated_executor(on_result) will
   *       return boost::asio::system_executor.  boost.asio post() reference web page will clarify that
   *       post(E, F), where E is a Task_engine, and F is a handler associated with system_executor,
   *       will simply cause F() to be post()ed onto E (to execute ASAP on a threads doing E.run()).
   *       (We've also tested this empirically.)
   *     - If on_result is bound to a strand S, via bind_executor(S, F), get_associated_executor(on_result) will
   *       return the boost::asio::io_context::strand (flow::util::Strand) S.  boost.asio doc web page about strands
   *       explains post(E, bind_executor(S, F)) will post() F onto E, with the added constraint to not execute it
   *       concurrently with any other handler also associated with S.
   *     - Other executors (not only strands) are possible, though that's more obscure.
   *
   * Failing to post() through get_associated_executor(on_result) -- e.g., simply just calling on_result() inside
   * the post()ed body -- would work great in the "vanilla" case but would simply disregard the strand S in the
   * strand case above (or whatever other executor).
   */
  return [this, on_result = std::move(on_result)]
           (const Error_code& err_code, size_t op_result)
           mutable
  {
    // Not safe to rely on L->R arg evaluation below; get this 1st, when we know on_result hasn't been move()d.
    const auto executor = get_associated_executor(on_result); // Usually system_executor (vanilla) or a strand.
    post(*(async_task_engine()),
         bind_executor(executor,
                       [err_code, op_result, on_result = std::move(on_result)]
    {
      on_result(err_code, op_result);
    }));
  };
} // Peer_socket::handler_func()

} // namespace flow::net_flow::asio
