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

#include "flow/common.hpp"

/**
 * Namespace containing the net_flow module's extension of boost.system error conventions, so that Flow network
 * protocol API can return codes/messages from within its own new set of error codes/messages.
 *
 * ### Synopsis ###
 *
 *   ~~~
 *   // Assign a code from net_flow's custom error code set -- to a *general* Error_code.
 *   flow::Error_code code = flow::net_flow::error::Code::S_CONN_REFUSED;
 *   // code contains the int equivalent of S_CONN_REFUSED enum value; and a category pointer for your new
 *   // new code set to which S_CONN_REFUSED belonds.  So it's about the size of errno plus a pointer.
 *
 *   // Yet we have nice OO access to the equivalents of `errno` and `strerror(errno)`:
 *   std::cout << "General error value = [" << code.value() << "]; msg = [" << code.message() << "].\n";
 *   // And we can throw an std::exception (`catch{}` can log the exception's .what() which will contain
 *   // the numeric value, the human message, and the optional context string).
 *   throw flow::error::Runtime_error(code, "Additional context info here!");
 *   ~~~
 *
 * ### Discussion ###
 * The contents of this `namespace` is one instance (and model example) for how one
 * can easily create their own simple error code set `enum` (and corresponding string messages) and hook it into
 * the flexible, yet maximally fast, boost.system error facility.  Another example (a peer of ours) would be
 * boost.asio's error code set.  See flow::Error_code doc header for a top-down summary of the
 * Flow-used-and-recommended error reporting system.
 *
 * For boost.system experts wondering whether there is support for `error_condition` equivalence (i.e., the
 * ability to compare a given `flow::error::error_code` to a more general `boost::system::error_condition`),
 * currently the answer is no, there isn't.  We may add `error_condition`s as #Error_code usage patterns emerge
 * over time.
 *
 * @internal
 *
 * ### Summary of implementation ###
 * This summarizes how we got the Synopsis above to work.  Replicate this in other modules as desired.
 *   -# Define new code set `enum` in your own namespace (recommend `error` sub-namespace of your module) in
 *      a new header file (recommend name based on namespace name).  Say we call it E.hpp.
 *   -# Also in E.hpp: Declare `make_error_code()` prototype right next to it.
 *   -# Also in E.hpp: Define `is_error_code_enum<>` specialization in `boost::system` namespace.
 *   -# In E.cpp: Define a new, non-public, category class C,
 *      whose main part is a simple function mapping each `enum` value to its human message (`switch()`).
 *      Instantiate a single `static` instance of C, perhaps as a member: `C::S_C`.
 *   -# Also in E.cpp: Implement the tiny boiler-plate `make_error_code()` from step 2.
 *      It constructs a general `Error_code` with the `int` version of the custom-`enum`
 *      value passed to it; and with a reference to `C::S_C` from previous step, thus identifying both the
 *      code set and the individual code.
 *
 * Result: One can directly assign codes from your new `enum` to the general `Error_code`
 * (a/k/a `boost::system::error_code`); see Synopsis above.
 *
 * ### Implementation discussion ###
 * While *using* this error reporting system is simple and convenient, the internal implementation is
 * (IMO) cryptic albeit fairly concise.  I followed the guide:
 *
 *   http://blog.think-async.com/2010/04/system-error-support-in-c0x-part-4.html
 *
 * Note that this isn't some random web page but rather a detailed a walkthrough by the author of
 * boost.system (which includes `error_code`) itself.  In addition, its author wrote boost.asio, which we use
 * extensively in this implementation.  Finally, boost.asio's own extension of the `error_code`/etc. system --
 * for its own error reporting, which is API-wise quite similar to this one -- can be seen (in boost.asio's
 * largely headers-only implementation) to follow the above guide ~99% to the letter.
 *
 * What's rather odd is that boost.system's actual formal documentation, e.g.,
 *
 *   http://www.boost.org/doc/libs/1_50_0/libs/system/doc/index.html,
 *
 * does not in any way explain how to implement all this properly.  The above guide is not included in that
 * official doc, nor is it linked.  In fact, following the formal documentation without a supernatural (IMO)
 * level of understanding of the author's intentions, would likely lead to the wrong implementation.
 * Most visibly, for example, the docs say that `error_code` is for platform-dependent error codes; while
 * `error_condition` is for portable error codes, or something very, very close to that.
 * Well (I thought originally), Flow's errors ARE platform-independent, so I guess I should use
 * `error_condition`s then!  So originally, all the Flow APIs that reported errors took `Error_condition*`
 * arguments, not `Error_code*` arguments.  Only after some colleagues remarked that this appears wrong,
 * and pointed out the above guide, did I realize that I did that wrong and switched to `Error_code`.
 * In particular, the guide elucidates: `error_code` is to report a "low-level" or "system-level" specific
 * error (not necessarily computer platform-dependent, as the formal doc might seem to say), while
 * `error_condition` is something to (optionally) "check against" as a higher-level or more abstract piece
 * of error handling logic.  So in fact any original error should be an `error_code`, and `error_condition`
 * is an _optional_ layer one _might_ add to help working with `error_code`s.
 */
namespace flow::net_flow::error
{

// Types.

/**
 * All possible errors returned (via flow::Error_code arguments) by flow::net_flow functions/methods.
 * These values are convertible to flow::Error_code (a/k/a `boost::system::error_code`) and thus
 * extend the set of errors that flow::Error_code can represent.
 *
 * @internal
 *
 * When you add a value to this `enum`, also add its description to
 * net_flow_error.cpp’s Category::message().  This description must be identical to the description
 * in the /// comment below, or at least as close as possible.
 *
 * If, when adding a new revision of the code, you add a value to this `enum`, add it to the end.
 * If, when adding a new revision of the code, you deprecate a value in this `enum`, do not delete
 * it from this `enum`.  Instead mark it as deprecated here and then remove it from
 * Flow_category::message().
 *
 * Errors that indicate apparent logic bugs (in other words, assertions that we were too afraid
 * to write as actual `assert()`s) should be prefixed with `S_INTERNAL_ERROR_`, and their messages
 * should also indicate "Internal error: ".
 *
 * @todo Consider adding specific subclasses of flow::error::Runtime_error
 * for the key codes, like `WAIT_INTERRUPTED` and `USER_TIMEOUT`.
 * In doing so, make sure there's an automated syntactic-sugary way of doing this, so that one need only add
 * a code<->class pair into some internal centralized mapping, automatically making system emit the proper
 * exception class, defaulting to plain Runtime_error if not specially-mapped.
 */
enum class Code
{
  /// Node not running.
  S_NODE_NOT_RUNNING = 1,
  /// Cannot ask to connect to "any" IP address.  Use specific IP address.
  S_CANNOT_CONNECT_TO_IP_ANY,
  /// Flow port number is not in the valid service port number range.
  S_INVALID_SERVICE_PORT_NUMBER,
  /// Flow port already reserved.
  S_PORT_TAKEN,
  /**
   * Internal error:  Tried to return Flow port which had not been reserved.
   *
   * @internal
   *
   * @note By its definition (see above brief description), it can only be triggered by Port_space::return_port().
   *       If one looks at all the calls to said method in practice, they'll note
   *       that it returning a truthy `*err_code` of any kind is unexpected to the point where it is an
   *       `assert()` failure, as least as of this writing.  Therefore, `S_..._PORT_NOT_TAKEN` is indeed deserving of
   *       having the `INTERNAL_ERROR` prefix (meaning, it occurring implies a programming error inside library).
   *       As of this writing, unless `assert()`s are compiled out, it's not even possible for the error to get to
   *       the user (but that may change by removing some or all of those `assert()`s -- in which case the error's
   *       naming matters more.  Bottom line, it is an `..._INTERNAL_ERROR_...` until some condition in practice
   *       makes it possible that Port_space::return_port() failing that way is not a result of a programming error.
   */
  S_INTERNAL_ERROR_PORT_NOT_TAKEN,
  /// No more ephemeral Flow ports available.
  S_OUT_OF_PORTS,
  /// Internal error:  Ephemeral port double reservation allowed.
  S_INTERNAL_ERROR_PORT_COLLISION,
  /// Connection reset because of unexpected/illegal behavior by the other side.
  S_CONN_RESET_BAD_PEER_BEHAVIOR,
  /// Other side refused connection.
  S_CONN_REFUSED,
  /// Internal error:  System error:  Something went wrong with boost.asio timer subsystem.
  S_INTERNAL_ERROR_SYSTEM_ERROR_ASIO_TIMER,
  /// Other side did not complete connection handshake within the allowed time; perhaps no one is listening.
  S_CONN_TIMEOUT,
  /// Other side reset an established connection.
  S_CONN_RESET_BY_OTHER_SIDE,
  /// Connection reset because a packet has been retransmitted too many times.
  S_CONN_RESET_TOO_MANY_REXMITS,
  /// Other side has sent packets with inconsistent sequence numbers.
  S_SEQ_NUM_ARITHMETIC_FAILURE,
  /// Other side has sent packet with sequence number that implies a port collision between two connections over time.
  S_SEQ_NUM_IMPLIES_CONNECTION_COLLISION,
  /// Node shutting down.
  S_NODE_SHUTTING_DOWN,
  /// User code on this side abruptly closed connection; other side may be informed of this.
  S_USER_CLOSED_ABRUPTLY,
  /// Attempted operation on an event set, when that event set was closed.
  S_EVENT_SET_CLOSED,
  /// Attempted to write to an event set, while a wait operation was pending on that event set.
  S_EVENT_SET_IMMUTABLE_WHEN_WAITING,
  /// Attempted to add an event into an event set, but that event already exists.
  S_EVENT_SET_EVENT_ALREADY_EXISTS,
  /// Attempted to work with an event that does not exist in the event set.
  S_EVENT_SET_EVENT_DOES_NOT_EXIST,
  /// Attempted to wait on or poll an event set while already waiting on that event set.
  S_EVENT_SET_DOUBLE_WAIT_OR_POLL,
  /// Attempted to wait on an event set without specifying event on which to wait.
  S_EVENT_SET_NO_EVENTS,
  /// Attempted to check wait results while still waiting.
  S_EVENT_SET_RESULT_CHECK_WHEN_WAITING,
  /// During connection user supplied metadata that is too large.
  S_CONN_METADATA_TOO_LARGE,
  /// When setting options, tried to set an unchangeable (static) option.
  S_STATIC_OPTION_CHANGED,
  /// When setting options, at least one option's value violates a required condition on that option.
  S_OPTION_CHECK_FAILED,
  /// A blocking (`sync_`) or background-blocking (`async_`) operation was interrupted, such as by a signal.
  S_WAIT_INTERRUPTED,
  /// A blocking (`sync_`) or background-blocking (`async_`) operation timed out versus user-supplied time limit.
  S_WAIT_USER_TIMEOUT
}; // enum class Code

// Functions.

/**
 * Given a `Code` `enum` value, creates a lightweight flow::Error_code (a/k/a boost.system `error_code`)
 * representing that error.  This is needed to make the
 * `boost::system::error_code::error_code<Code>()` template implementation work.  Or, slightly more in English,
 * it glues the (completely general) flow::Error_code to the (net_flow-specific) error code set
 * flow::net_flow::error::Code, so that one can implicitly covert from the latter to the former.
 *
 * @param err_code
 *        The `enum` value.
 * @return A corresponding flow::Error_code.
 */
Error_code make_error_code(Code err_code);

} // namespace flow::net_flow::error

/// We may add some ADL-based overloads into this namespace outside `flow`.
namespace boost::system
{

// Types.

/**
 * Ummm -- it specializes this `struct` to -- look -- the end result is boost.system uses this as
 * authorization to make `enum` `Code` convertible to `Error_code`.  The non-specialized
 * version of this sets "value" to false, so that random arbitary `enum`s can't just be used as
 * `Error_code`s.  Note that this is the offical way to accomplish that, as (confusingly but
 * formally) documented in boost.system docs.
 */
template<>
struct is_error_code_enum<::flow::net_flow::error::Code>
{
  /// Means `Code` `enum` values can be used for flow::Error_code.
  static const bool value = true;
  /* ^-- Could instead derive publicly from true_type, but then we'd have to find a true_type
   * in some header we could use. At any rate this works just the same, and boost.asio does it this way, so if
   * it's good enough for them (the author of boost.asio = the author of boost.system [error_code, etc.]),
   * I say it's good enough for us. */
};

} // namespace boost::system
