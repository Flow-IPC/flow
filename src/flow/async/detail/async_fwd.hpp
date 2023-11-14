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

#include <stdint.h>

namespace flow::async
{
// Types.

// Find doc headers near the bodies of these compound types.

class Task_qing_thread;

// Free functions.

/**
 * Returns the 0-based processor *logical* (not hardware) core index of the core executing the calling thread
 * presently.  Since the value of this can change from statement to statement in the same thread, probably this
 * is not to be used except for logging/reporting.
 *
 * @return See above.
 */
uint16_t cpu_idx();

} // namespace flow::async
