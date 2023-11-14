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
#include "flow/util/util_fwd.hpp"
#include <boost/asio.hpp>
#include <boost/unordered_set.hpp>
#include <ostream>

namespace flow::net_flow
{
// Types.

/**
 * Represents the remote endpoint of a Flow-protocol connection; identifies the UDP endpoint of
 * the remote Node and the logical Flow-protocol port within that Node.  In particular when performing
 * Node::connect(), one must supply a Remote_endpoint.  Constuct this via direct member initialization.
 *
 * When performing Node::listen(), on the other hand, a Remote_endpoint is unnecessary; the UDP
 * endpoint has already been supplied when starting the overall Node, so only the Flow-protocol port
 * number is needed by Node::listen().
 *
 * Since the components of a Remote_endpoint should be freely readable and modifiable, I forewent
 * the usual accessors/mutators and simply made those components public data members.
 *
 * @see As of this writing, class Node documentation header has a lengthy discussion about whether
 *      Remote_endpoint needs #m_flow_port at all; if it does not, then Remote_endpoint becomes isomorphic
 *      to util::Udp_endpoint and can be thus aliased or eliminated.  See the to-dos section of that large doc
 *      header.
 *      (That discussion has implications for all of `net_flow`, so it belongs there, not here on this humble
 *      `struct`.)
 *
 * @internal
 *
 * @todo The respected Scott Meyers ("Effective C++") would recommend that Remote_endpoint::hash() and
 * Remote_endpoint::operator==() be written as free functions instead of as `struct` members (see
 * http://www.drdobbs.com/cpp/how-non-member-functions-improve-encapsu/184401197).  The same would apply
 * to a potentially great number of other non-`virtual` methods (including operators) of other `struct`s and classes
 * that could be implemented without `friend`, so really pursuing this approach could touch quite a few things.
 * I won't repeat Meyers' reasoning; see the link.  I find the reasoning mostly compelling.
 * To summarize his final suggestions: An operation on a type should be a free function if ALL of the
 * following holds: it is not `virtual` by nature; AND: it is `<<` or `>>`, and/or its would-be left operand would
 * require type conversions, and/or it can be *implemented* entirely via the type's publicly exposed interface.
 * We already follow the advice for `<<` and `>>` (if only because we apply it to stream ops, and for that it just
 * makes sense, `<<` especially, since the stream is the left-most operand there, so it *has* to be a free function
 * in that case -- and thus also for `>>` for consistency).  The type conversion is not a common
 * thing for this library; so that leaves non-`virtual` operations (which is most of them) that can be implemented via
 * public APIs only (which is probaby common for `struct`s like Remote_endpoint, though we generally try not to
 * have lots of `struct` methods in the first place... usually).  Before rushing headlong into this project, consider
 * a few more things, though.  1, Doxygen wouldn't pick up the relation between a non-friend free function dealing
 * with `struct` or class `C` and `C` itself; so verbosity/error-proneness would increase by having to add a Doxygen
 * `relatesalso` special command to each free function; otherwise documentation becomes less readable (and there's no
 * way to enforce this by having Doxygen fail without this being done, somehow).  2, in the
 * `C`-returning-static-member-of-`C` pattern, usually in our code it would call a private `C` constructor,
 * meaning it would require `friend` to make it a free function, meaning it breaks Meyers' own rule and thus should
 * be left a member.  3, Meyers tends to place very little value on brevity as its own virtue.  If you find that
 * following the above rule in some case seems to be significantly increasing code complexity/size, maybe it's best
 * to leave it alone.  (I am thinking of Low_lvl_packet and its sub-types like Ack_packet: they are `struct`s but
 * not typical ones, with complex APIs; making those APIs free function when non-`virtual` sounds like a rather hairy
 * change that "feels" like it would reduce simplicity and may increase size, at least due to all the necessary
 * 'splaining in the comments.)  All that said, this is perhaps worth pursuing (in the pursuit of stylistic
 * perfection) -- but surgically and first-do-no-harm-edly.  Oh, also, constructors don't count for this and should
 * continue to remain constructors and not some free functions stuff (should be obvious why, but just in case you get
 * any ideas...).
 *
 * @todo There is a sub-case of the immediately preceding to-do that may be performed first without much
 * controversy.  That is the case where there is some member method of some type, that is then called by an
 * extremely thin wrapper free function (not even a `friend`).  In fact, `hash_value()` and Remote_endpoint::hash()
 * are such a pair.  Assuming one does not forget Doxygen's `relatesalso` command, it would be easy and concise
 * to just get rid of the member and simply move its implementation directly into the free function.  After all,
 * people are meant to use the free function anyway, so why the middle-man method?  In this example,
 * `hash_value()` would directly compute the hash, and Remote_endpoint::hash() would not exist; but the former
 * would include a Doxygen `relatesalso Remote_endpoint` command to ensure properly linked generated documentation.
 */
struct Remote_endpoint
{
  // Data.

  /// UDP address (IP address/UDP port) where the Node identified by this endpoint bound its low-level UDP socket.
  util::Udp_endpoint m_udp_endpoint;
  /// The logical Flow port within the Node for the particular connection identified by this endpoint.
  flow_port_t m_flow_port = 0;

  // Methods.

  /**
   * Hash value of this Remote_endpoint for `unordered_*<>`.
   *
   * @return Ditto.
   */
  size_t hash() const;
}; // class Remote_endpoint

// Free functions: in *_fwd.hpp.

} // namespace flow::net_flow
