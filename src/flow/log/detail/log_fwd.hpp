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

#include "flow/log/log_fwd.hpp"
#include <boost/thread.hpp>

namespace flow::log
{
// Types.

// Find doc headers near the bodies of these compound types.

class Serial_file_logger;

// Globals.

/**
 * Thread-local Msg_metadata object used by FLOW_LOG_WITHOUT_CHECKING() for an alleged perf bonus in the
 * synchronous-Logger code path.  It is allocated at the first log call site for a given thread; and auto-deallocated
 * when thread exits.
 */
extern boost::thread_specific_ptr<Msg_metadata> this_thread_sync_msg_metadata_ptr;

} // namespace flow::log
