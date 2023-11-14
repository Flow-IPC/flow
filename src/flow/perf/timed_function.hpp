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
#include "flow/perf/clock_type_fwd.hpp"
#include "flow/perf/checkpt_timer.hpp"

namespace flow::perf
{
// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Accumulator, typename Func>
auto timed_function(Clock_type clock_type, Accumulator* accumulator, Func&& function)
{
  return [clock_type, accumulator, function = std::move(function)](auto&&... params) mutable
  {
    const auto start = Checkpointing_timer::now(clock_type);

    function(std::forward<decltype(params)>(params)...);

    /* If Accumulator is atomic<duration_rep_t>: this is fetch_add() with memory ordering arg = strict.
     * @todo A non-strict-ordering-using API may be desirable. */
    *accumulator += (Checkpointing_timer::now(clock_type) - start).count();
  };
}

template<typename Accumulator, typename Func>
auto timed_function_nvr(Clock_type clock_type, Accumulator* accumulator, Func&& function)
{
  // It's much like timed_function() -- just adding passing-through function() return value.  Comments kept light.

  return [clock_type, accumulator, function = std::move(function)](auto&&... params) mutable
  {
    const auto start = Checkpointing_timer::now(clock_type);

    // Copy elision should avoid copying the result here.
    const auto result = function(std::forward<decltype(params)>(params)...);

    *accumulator += (Checkpointing_timer::now(clock_type) - start).count();

    /* I (ygoldfel) *think* this will be copy/move-elided... but not sure.
     * If yes, then this is good.  If not, then should add std::move(), to make sure it doesn't get copied (ugh).
     * @todo Look into it, maybe experimentally! */
    return result;
  };
}

} // namespace flow::perf
