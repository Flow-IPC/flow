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
#include <ostream>

namespace flow::net_flow
{
// Types.

// Find doc headers near the bodies of these compound types.

struct Ack_packet;
class Congestion_control_classic;
class Congestion_control_classic_data;
class Congestion_control_classic_with_bandwidth_est;
class Congestion_control_strategy;
class Congestion_control_selector;
struct Data_packet;
class Drop_timer;
struct Low_lvl_packet;
class Peer_socket_receive_stats_accumulator;
class Peer_socket_send_stats_accumulator;
class Port_space;
struct Rst_packet;
class Send_bandwidth_estimator;
struct Send_pacing_data;
class Sequence_number;
class Socket_buffer;
struct Syn_packet;
struct Syn_ack_packet;
struct Syn_ack_ack_packet;

// Free functions.

/**
 * Applies the given `ostream` manipulator function to the given `ostream` -- just like standard streams already
 * allow, but with the function given as a flow::Function object instead of a raw function pointer.
 * Without this, the flow::Function is usually converted to a `bool` or something and just printed out as
 * `"1"` -- which is not useful.
 *
 * Much like with standard function manipulators (`std::endl`, for example), the idea is for this to work:
 *
 *   ~~~
 *   cout << print_something_to_ostream << ":" << xyz;
 *   // ...
 *   // But in this case, print_something_to_stream is of type: Function<ostream& (ostream&)>.
 *   // E.g., ahead of the `cout` this could have been done, assuming: ostream& func(ostream& os, bool some_flag):
 *   flow::Function<ostream& (ostream&)> print_something_to_stream
 *     = [](ostream& os) -> ostream& { return func(os, false); };
 *   // Or with a non-static class member function: class Some_class { ostream& func(ostream& os, bool some_flag); }.
 *   Some_class some_instance;
 *   flow::Function<ostream& (ostream&)> print_something_to_stream
 *     = [some_instance](ostream& os) -> ostream& { return some_instance->func(os, false); };
 *   ~~~
 *
 * @warning It is important for this to be in namespace `flow::net_flow`, not higher (such as `flow`) or lower.
 *          Otherwise the places needing this overload to be used won't, and it will either not compile;
 *          or compile but invoke some other, useless overload.  Unfortunately placing it into a containing
 *          namespace makes it not work from within a contained namespace.
 *
 * @note You may note the above technique is not used for argument-bearing `std` stream manipulators; for example
 *       `std::setw()`.  The technique used to make that work is that `setw()` would return some internal `struct`
 *       (of unknown-to-user type `T`), in which it has stored the argument to `setw()`; and an `operator<<(ostream, T)`
 *       overload is defined; this overload would actually set the field width (or
 *       whatever `setw()` is supposed to do) on the `ostream` -- using the arg value
 *       stored in the `T`.  That works, and it may be somewhat more performant than our solution; but our solution
 *       is *much* more elegant and concise; it lets one build stream manipulators out of any function at all,
 *       the only requirements on it being that it returns an `ostream&` and takes same as *an* argument.
 *       The `std` technique allows the same, but we can do it with a single and expressive `bind()` call instead of
 *       a hidden internal `struct type`, explicitly storing arguments, documenting that stuff, etc.
 *       (We can change individual cases to use the `std`-style solution, if performance needs call for it, but so far
 *       there has been no such needs.)  To be fair, the `std` solution is ALSO easier/more expressive at the
 *       actual call site.  That is, in the above examples, one could just do: `cout << func(false) << ":" << xyz;` or
 *       `cout << some_instance.func(false) << ":" << xyz;`, respectively.  That's easier to read than the `bind()`
 *       constructs above; but the code elsewhere that makes those `func()`s work is a huge mess that our solution
 *       completely avoids.  For us, all that is needed behind the scenes is the present `operator<<` overload.
 *
 * @param os
 *        Stream to write to/mutate.
 * @param os_manip
 *        A flow::Function object that points to a manipulator, which is a function that takes a mutable
 *        `ostream`, writes something to it and/or acts on it in some other non-`const` way; and returns the original
 *        reference to mutable `ostream`.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Function<std::ostream& (std::ostream&)>& os_manip);

/**
 * Prints a printable representation of the data in `sock_buf` to the given standard `ostream`.  Exactly
 * one line per block is used.  This implementation is slow; use only in DATA logging, or where
 * performance does not matter.
 *
 * @param os
 *        Stream to which to write.
 * @param sock_buf
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Socket_buffer& sock_buf);

/**
 * Prints given sequence number to given `ostream`.
 *
 * set_metadata()'s effects are felt here; see Sequence_number class doc header for details.
 *
 * @param os
 *        Stream to which to print.
 * @param seq_num
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Sequence_number& seq_num);

/**
 * Free function that returns `seq_num.hash()`; has to be a free function named `hash_value`
 * for boost.hash to pick it up.
 *
 * @relatesalso Sequence_number
 *
 * @param seq_num
 *        Object to hash.
 * @return `seq_num.hash()`.
 */
size_t hash_value(const Sequence_number& seq_num);

} // namespace flow::net_flow
