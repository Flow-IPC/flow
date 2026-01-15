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
#include "flow/net_flow/error/error.hpp"
#include "flow/net_flow/detail/port_space.hpp"
#include "flow/util/util.hpp"

namespace flow::net_flow::error
{

// Types.

/**
 * The boost.system category for errors returned by the `net_flow` Flow module.  Think of it as the polymorphic
 * counterpart of error::Code, and it kicks in when, for `flow::Error_code ec`, something
 * like `ec.message()` is invoked.
 *
 * Note that this class's declaration is not available outside this translation unit (.cpp
 * file), and its logic is accessed indirectly through standard boost.system machinery
 * (`Error_code::name()` and `Error_code::message()`).  This class enables those basic
 * features (e.g., `err_code.message()` returns the string description of the error `err_code`) to
 * work.
 */
class Category :
  public boost::system::error_category
{
public:
  // Constants.

  /// The one Category.
  static const Category S_CATEGORY;

  // Methods.

  /**
   * Implements superclass API: returns a `static` string representing this `error_category` (which,
   * for example, shows up in the `ostream` representation of any Category-belonging
   * flow::Error_code).
   *
   * @return A `static` string that's a brief description of this error category.
   */
  const char* name() const noexcept override;

  /**
   * Implements superclass API: given the integer error code of an error in this category, returns a description of that
   * error (similarly in spirit to `std::strerror()`).  This is the "engine" that allows `ec.message()`
   * to work, where `ec` is a flow::Error_code with an error::Code value.
   *
   * @param val
   *        Error code of a Category error (realistically, an error::Code `enum` value cast to
   *        `int`).
   * @return String describing the error.
   */
  std::string message(int val) const override;

private:
  // Constructors.

  /// Boring constructor.
  explicit Category();
}; // class Category

// Static initializations.

const Category Category::S_CATEGORY;
/* ^-- @todo Note that things will go bad, if something triggers an error before this is actually instantiated, AND
 * the error is accessed in such a way as to need the error_category -- e.g., accessing its error message for
 * logging.  This isn't very likely, but if some other static initialization code triggers an error, then there
 * is a random chance it would be before this static initialization, hence the likely problem of the seg-fault
 * type.  We can fight this by using a local static variable trick, but then that isn't thread-safe by default.
 * This is a well known C++ dilemma without a perfect solution.  In this case we think it's reasonable to
 * expect Flow to not be used until main() runs, so we'll live with the risk of this.  Revisit/reevaluate,
 * especially if we upgrade to C++0x.  Update: We are now compiling with C++11.  http://cppreference.com does
 * not mention any relevant improvements in 11 or 14 or 17.  So we are stuck for now. */

// Implementations.

Error_code make_error_code(Code err_code)
{
  /* Assign Category as the category for net_flow::error::Code-cast error_codes;
   * this basically glues together Category::name()/message() with the Code enum. */
  return Error_code{static_cast<int>(err_code), Category::S_CATEGORY};
}

Category::Category() = default;

const char* Category::name() const noexcept // Virtual.
{
  return "flow";
}

std::string Category::message(int val) const // Virtual.
{
  using std::string;

  // KEEP THESE STRINGS IN SYNC WITH COMMENT IN net_flow_error.hpp ON THE INDIVIDUAL ENUM MEMBERS!

  /* Just create a string (unless compiler is smart enough to do this only once and reuse later)
   * based on a static char* which is rapidly indexed from val by the switch() statement.
   * Since the error_category interface requires that message() return a string by value, this
   * is the best we can do speed-wide... but it should be fine.
   *
   * Some error messages can be fancier by specifying outside values (e.g., see
   * S_INVALID_SERVICE_PORT_NUMBER). */
  switch (static_cast<Code>(val))
  {
  case Code::S_NODE_NOT_RUNNING:
    return "Node not running.";
  case Code::S_CANNOT_CONNECT_TO_IP_ANY:
    return "Cannot ask to connect to \"any\" IP address.  Use specific IP address.";
  case Code::S_INVALID_SERVICE_PORT_NUMBER:
  {
    string str;
    util::ostream_op_to_string(&str,
                               "Flow port number is not in the valid service port number range [",
                               Port_space::S_FIRST_SERVICE_PORT, ", ",
                               Port_space::S_FIRST_SERVICE_PORT + Port_space::S_NUM_SERVICE_PORTS - 1, "].");
    return str;
  }
  case Code::S_PORT_TAKEN:
    return "Flow port already reserved.";
  case Code::S_INTERNAL_ERROR_PORT_NOT_TAKEN:
    return "Internal error:  Tried to return Flow port which had not been reserved.";
  case Code::S_OUT_OF_PORTS:
    return "No more ephemeral Flow ports available.";
  case Code::S_INTERNAL_ERROR_PORT_COLLISION:
    return "Internal error:  Ephemeral port double reservation allowed.";
  case Code::S_CONN_RESET_BAD_PEER_BEHAVIOR:
    return "Connection reset because of unexpected/illegal behavior by the other side.";
  case Code::S_CONN_REFUSED:
    return "Other side refused connection.";
  case Code::S_CONN_TIMEOUT:
    return "Other side did not complete connection handshake within the allowed time; perhaps no one is listening.";
  case Code::S_CONN_RESET_BY_OTHER_SIDE:
    return "Other side reset an established connection.";
  case Code::S_CONN_RESET_TOO_MANY_REXMITS:
    return "Connection reset because a packet has been retransmitted too many times.";
  case Code::S_SEQ_NUM_ARITHMETIC_FAILURE:
    return "Other side has sent packets with inconsistent sequence numbers.";
  case Code::S_SEQ_NUM_IMPLIES_CONNECTION_COLLISION:
    return "Other side has sent packet with sequence number that implies a port collision between "
           "two connections over time.";
  case Code::S_NODE_SHUTTING_DOWN:
    return "Node shutting down.";
  case Code::S_USER_CLOSED_ABRUPTLY:
    return "User code on this side abruptly closed connection; other side may be informed of this.";
  case Code::S_EVENT_SET_CLOSED:
    return "Attempted operation on an event set, when that event set was closed.";
  case Code::S_EVENT_SET_IMMUTABLE_WHEN_WAITING:
    return "Attempted to write to an event set, while a wait operation was pending on that event set.";
  case Code::S_EVENT_SET_EVENT_ALREADY_EXISTS:
    return "Attempted to add an event into an event set, but that event already exists.";
  case Code::S_EVENT_SET_EVENT_DOES_NOT_EXIST:
    return "Attempted to work with an event that does not exist in the event set.";
  case Code::S_EVENT_SET_DOUBLE_WAIT_OR_POLL:
    return "Attempted to wait on or poll an event set while already waiting on that event set.";
  case Code::S_EVENT_SET_NO_EVENTS:
    return "Attempted to wait on an event set without specifying event on which to wait.";
  case Code::S_EVENT_SET_RESULT_CHECK_WHEN_WAITING:
    return "Attempted to check wait results while still waiting.";
  case Code::S_CONN_METADATA_TOO_LARGE:
    return "During connection user supplied metadata that is too large.";
  case Code::S_STATIC_OPTION_CHANGED:
    return "When setting options, tried to set an unchangeable (static) option.";
  case Code::S_OPTION_CHECK_FAILED:
    return "When setting options, at least one option's value violates a required condition on that option.";
  case Code::S_WAIT_INTERRUPTED:
    return "A blocking synchronous or asynchronous operation was interrupted, such as by a signal.";
  case Code::S_WAIT_USER_TIMEOUT:
    return "A blocking synchronous or asynchronous operation reached user-specified timeout without succeeding.";
  }
  assert(false);
  return "";
} // Category::message()

} // namespace flow::net_flow::error
