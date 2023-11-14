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

#include <iostream>

namespace flow::log
{

// Types.

// Find doc headers near the bodies of these compound types.

class Verbosity_config;

// Free functions.

/**
 * Deserializes a Verbosity_config from a standard input stream by invoking `val.parse(is)`.
 * `val.last_result_message()` can be used to glean the success or failure of this operation.
 *
 * @relatesalso Verbosity_config
 *
 * @param is
 *        Stream from which to deserialize.
 * @param val
 *        Value to set.
 * @return `is`.
 */
std::istream& operator>>(std::istream& is, Verbosity_config& val);

/**
 * Serializes a Verbosity_config to a standard output stream.
 *
 * @relatesalso Verbosity_config
 *
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Verbosity_config& val);

/**
 * Checks for exact equality of two Verbosity_config objects.  (Note that this is not maximally clever, in that if
 * (`val1 == val2`), then they *definitely* produce the same Config all else being equal; but if
 * the reverse is true, then it is possible they differently expressed values producing the same actual result.
 * E.g., something like `ALL:INFO` differs from `ALL:WARN;ALL:INFO` -- yet they have the same effect.  However
 * component names are normalized internally when parsing, so that won't produce false inequality.)
 *
 * Only Verbosity_config::component_sev_pairs() is significant in this comparison; last_result_message() is not.
 *
 * @relatesalso Verbosity_config
 *
 * @param val1
 *        Object to compare.
 * @param val2
 *        Object to compare.
 * @return `true` if definitely equal; `false` if possibly not equal.
 */
bool operator==(const Verbosity_config& val1, const Verbosity_config& val2);

/**
 * Returns `!(val1 == val2)`.
 *
 * @relatesalso Verbosity_config
 *
 * @param val1
 *        Object to compare.
 * @param val2
 *        Object to compare.
 * @return See above.
 */
bool operator!=(const Verbosity_config& val1, const Verbosity_config& val2);

} // namespace flow::log
