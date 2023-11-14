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

#include "flow/perf/perf_fwd.hpp"
#include "flow/perf/timed_function.hpp"
#include <boost/asio/associated_executor.hpp>
#include <utility>

namespace flow::perf
{
// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Accumulator, typename Handler>
auto timed_handler(Clock_type clock_type, Accumulator* accumulator, Handler&& handler)
{
  using boost::asio::bind_executor;
  using boost::asio::get_associated_executor;

  const auto executor = get_associated_executor(handler); // Do this before `handler` is move()d away!
  return bind_executor(executor,
                       timed_function(clock_type, accumulator, std::move(handler)));
}

} // namespace flow::perf
