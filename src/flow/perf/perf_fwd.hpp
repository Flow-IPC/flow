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

#include "flow/perf/clock_type_fwd.hpp"

/**
 * Flow module containing tools for profiling and optimization.
 *
 * As of this writing (around the time the flow::perf Flow module was created) this centers on Checkpointing_timer,
 * a facility for measuring real and processor time elapsed during the arbitrary measured operation.  That said,
 * generally speaking, this module is meant to be a "kitchen-sink" set of facilities fitting the sentence at the
 * very top of this doc header.
 */
namespace flow::perf
{
// Types.

// Find doc headers near the bodies of these compound types.

class Checkpointing_timer;

/**
 * Short-hand for ref-counting pointer to Checkpointing_timer.  Original use case is to allow
 * Checkpointing_timer::Aggregator to generate and return Checkpointing_timer objects with minimal headaches for user.
 */
using Checkpointing_timer_ptr = boost::shared_ptr<Checkpointing_timer>;

// Free functions.

/**
 * Constructs a closure that times and executes `void`-returning `function()`, adding the elapsed time with
 * clock type `clock_type` -- as raw ticks of perf::Duration -- to `accumulator`.
 *
 * Consider other overload(s) and similarly named functions as well.  With this one you get:
 *   - `function()` is treated as returning `void` (any return value is ignored).
 *   - `function()` is a generally-used timed function: not necessarily a `boost.asio` or flow::async *handler*.
 *     Any associated executor (such as a `strand`) *will* be lost.  See timed_handler(), if you have a handler.
 *   - One specific perf::Clock_type, not some subset given as perf::Clock_types_subset.  For performance this may
 *     be significant, even though operations on the latter are still light-weight.
 *   - Accumulation (the plus-equals operation) done by performing `+=(duration_rep_t)`, where perf::duration_rep_t
 *     is -- as a reminder -- a raw integer type like `int64_t`.  If accumulation may occur in a multi-threaded
 *     situation concurrently, this can improve performance vs. using an explicit lock, if one
 *     uses `Accumulator` = `atomic<duration_rep_t>`.
 *   - Lack of `chrono`-style type safety: It is up to you to interpret the `*accumulator`-stored ticks as their
 *     appropriate units.
 *
 * ### Synopsis/examples ###
 * Time a function that happens to take a couple of args.  Don't worry about the timing also happening concurrenty:
 * not using `atomic`.
 *
 *   ~~~
 *   flow::perf::duration_rep_t accumulated_ticks{0};
 *   const auto timed_func
 *     = flow::perf::timed_function
 *         (flow::perf::Clock_type::S_CPU_THREAD_TOTAL_HI_RES, &accumulated_ticks,
 *          [](int x, int y) { for (auto i = 0; i < (x * y); ++i) {} });
 *   // ...
 *   // Later, run it -- this will add to accumulated_ticks.  Can do this many times but not concurrently.
 *   timed_func(7, 7); // Note it can only be called void-style.
 *   // ...
 *   // Later, here's the result.  Note the construction from type-unsafe ticks to type-safe Duration.
 *   const flow::perf::Duration total_dur{accumulated_ticks};
 *   // Can convert to whatever units type-safely now (duration_cast<> in this case allows for precision loss).
 *   const auto total_dur_us = chrono::duration_cast<chrono::microseconds>(total_dur);
 *   ~~~
 *
 * Same thing but with an `atomic` to support timing/execution occuring concurrently:
 *
 *   ~~~
 *   std::atomic<flow::perf::duration_rep_t> accumulated_ticks{0};
 *   const auto timed_func
 *     = flow::perf::timed_function
 *         (flow::perf::Clock_type::S_CPU_THREAD_TOTAL_HI_RES, &accumulated_ticks,
 *          [](int x, int y) { for (auto i = 0; i < (x * y); ++i) {} });
 *   // ...
 *   // Later, run it -- this will add to accumulated_ticks.  Can do this many times *and* concurrently in N threads.
 *   timed_func(7, 7); // Note it can only be called void-style.
 *   // ...
 *   // Later, here's the result.  Note the construction from type-unsafe ticks to type-safe Duration.
 *   const flow::perf::Duration total_dur{accumulated_ticks};
 *   // Can convert to whatever units type-safely now (duration_cast<> in this case allows for precision loss).
 *   const auto total_dur_us = chrono::duration_cast<chrono::microseconds>(total_dur);
 *   ~~~
 *
 * ### `Accumulator A` type requirements/recommendations ###
 * It must have `A += duration_rep_t(...)`.  This operation must be safe for concurrent execution with itself, if
 * timed_function() is potentially used concurrently.  In that case consider `atomic<duration_rep_t>`.  If concurrency
 * is not a concern, you can just use `duration_rep_t` to avoid the strict-ordering overhead involved in `atomic`
 * plus-equals operation.
 *
 * `Accumulator` is understood to store raw ticks of #Duration -- not actual #Duration -- for performance reasons
 * (to wit: so that `atomic` plus-equals can be made use of, if it exists).  If you need a #Duration
 * ultimately -- and for type safety you really *should* -- it is up to you to construct a #Duration from the
 * accumulated `duration_rep_t`.  This is trivially done via the `Duration{duration_rep_t}` constructor.
 *
 * @todo timed_function(), when operating on an `atomic<duration_rep_t>`, uses `+=` for accumulation which may be
 * lock-free but uses strict ordering; a version that uses `fetch_add()` with relaxed ordering may be desirable
 * for extra performance at the cost of not-always-up-to-date accumulation results in all threads.
 * As of this writing this can be done by the user by providing a custom type that defines `+=` as explicitly
 * using `fetch_add()` with relaxed ordering; but we could provide an API for this.
 *
 * @todo timed_function() overload exists for a single `Clock_type`, but simultaneous multi-clock timing using the
 * perf::Clock_types_subset paradigm (as used, e.g., in Checkpointing_timer) would be a useful and consistent API.
 * E.g., one could measure user and system elapsed time simultaneously.  As of this writing this only does not exist
 * due to time constraints: a perf-niggardly version targeting one clock type was necessary.
 *
 * @tparam Accumulator
 *         Integral accumulator of clock ticks.  See above for details.
 * @tparam Func
 *         A function that is called `void`-style taking any arbitrary number of args, possibly none.
 * @param clock_type
 *        The type of clock to use for timing `function()`.
 * @param accumulator
 *        The accumulator to add time elapsed when calling `function()` to.  See instructions above regarding
 *        concurrency, `atomic`, etc.
 * @param function
 *        The function to execute and time.
 * @return A closure that will time and execute `function()`, adding the elapsed time to `accumulator`.
 */
template<typename Accumulator, typename Func>
auto timed_function(Clock_type clock_type, Accumulator* accumulator, Func&& function);

/**
 * Constructs a closure that times and executes non-`void`-returning `function()`, adding the elapsed time with
 * clock type `clock_type` -- as raw ticks of perf::Duration -- to `accumulator`.  "Nvr" stands for
 * non-`void`-returning.
 *
 * Consider other overload(s) and similarly named functions as well.  With this one you get:
 *   - `function()` is treated as returning non-`void` (any return value returned by it is then returned
 *     by the returned closure accordingly).
 *     - Hence `function()` cannot be a `boost.asio` handler, which are always `void`-returning.
 *       So there is no timed_handler() counterpart to the present function.
 *   - Otherwise identical to the similar timed_function().
 *
 * ### Synopsis/examples ###
 * Similar to the 2nd example in timed_function() doc header: Time a function that happens to take a couple of args,
 * allowing for concurrency by using an `atomic`.  The difference: `timed_func()` returns a value.
 *
 *   ~~~
 *   std::atomic<flow::perf::duration_rep_t> accumulated_ticks{0};
 *   const auto timed_func
 *     = flow::perf::timed_function_nvr
 *         (flow::perf::Clock_type::S_CPU_THREAD_TOTAL_HI_RES, &accumulated_ticks,
 *          [](int x, int y) -> int { for (auto i = 0; i < (x * y); ++i) {} return i; });
 *   // ...
 *   // Later, run it -- this will add to accumulated_ticks.  Can do this many times *and* concurrently in N threads.
 *   const auto result = timed_func(7, 7); // Note it is called non-void-style, with the return value passed-through.
 *   // ...
 *   // Later, here's the result.  Note the construction from type-unsafe ticks to type-safe Duration.
 *   const flow::perf::Duration total_dur(accumulated_ticks);
 *   // Can convert to whatever units type-safely now (duration_cast<> in this case allows for precision loss).
 *   const auto total_dur_us = chrono::duration_cast<chrono::microseconds>(total_dur);
 *   ~~~
 *
 * ### `Accumulator A` type requirements/recommendations ###
 * See timed_function().
 *
 * @tparam Accumulator
 *         See timed_function().
 * @tparam Func
 *         A function that is called non-`void`-style taking any arbitrary number of args, possibly none.
 * @param clock_type
 *        The type of clock to use for timing `function()`.
 * @param accumulator
 *        The accumulator to add time elapsed when calling `function()` to.  See instructions above regarding
 *        concurrency, `atomic`, etc.
 * @param function
 *        The function to execute and time.
 * @return A closure that will time and execute `function()`, adding the elapsed time to `accumulator`.
 */
template<typename Accumulator, typename Func>
auto timed_function_nvr(Clock_type clock_type, Accumulator* accumulator, Func&& function);

/**
 * Identical to timed_function() but suitable for boost.asio-targeted handler functions.  In other words, if you want
 * to `post(handler)` or `async_...(handler)` in a boost.asio `Task_engine`, and you'd like to time `handler()` when
 * it is executed by boost.asio, then use `timed_handler(..., handler)`.
 *
 * Consider other overload(s) and similarly named functions as well.  With this one you get:
 *   - `handler()` is a `boost.asio` or flow::async *handler*.
 *   - Otherwise identical to the similar timed_function().
 *
 * @note This is suitable for using the Flow-recommended boost.asio wrapper/helper API, flow::async.
 * @warning Using `timed_function(handler)` would "work" too, in that it would compile and at a first glance appear to
 *          work fine.  The problem: If `handler` is bound to an executor -- most commonly a boost.asio strand
 *          (util::Strand) -- then using timed_function() would "unbind it."  So it it was bound to `Strand S`, meant
 *          to make certain `handler()` never executed concurrently with other handlers bound to `S`, then that
 *          constraint would (silently!) no longer be observed -- leading to terrible intermittent concurrency bugs.
 * @note boost.asio handlers always return `void` (meaning anything else they might return is ignored).  Hence there is
 *       no `timed_handler_nvr()`, even though there is a timed_function_nvr().
 *
 * ### Synopsis/examples ###
 * Similar to the 2nd example in timed_function() doc header: Time a function that happens to take a couple of args,
 * allowing for concurrency by using an `atomic`.  The difference: it is first bound to a strand.
 * In this case we `post()` the handler, so it takes no args in this example.  However, if used with, say,
 * `boost::asio::ip::tcp::socket::async_read_some()`, it would take args such as bytes-received and error code.
 *
 *   ~~~
 *   flow::util::Task_engine multi_threaded_engine; // boost.asio Task_engine later associated with 2+ threads.
 *   // ...
 *   // Strand guaranteeing non-concurrency for any handler functions bound to it, perhaps pertaining to HTTP request R.
 *   flow::util::Strand this_request_strand(multi_threaded_engine);
 *   std::atomic<flow::perf::duration_rep_t> accumulated_ticks{0};
 *   auto timed_hnd
 *     = flow::perf::timed_handler
 *         (flow::perf::Clock_type::S_CPU_THREAD_TOTAL_HI_RES, &accumulated_ticks,
 *          boost::asio::bind_executor(this_request_strand,
 *                                     []() { for (unsigned int i = 0; i < 1000000; ++i) {} });
 *   // Post it for ASAP execution -- *when* it asynchronously executed in some thread, will add to accumulated_ticks.
 *   // timed_hnd() is bound to this_request_strand, because the function we passed to timed_handler() was so bound.
 *   boost::asio::post(multi_threaded_engine, timed_hnd);
 *   // ...
 *   // Later, here's the result.  Note the construction from type-unsafe ticks to type-safe Duration.
 *   const flow::perf::Duration total_dur(accumulated_ticks);
 *   // Can convert to whatever units type-safely now (duration_cast<> in this case allows for precision loss).
 *   const auto total_dur_us = chrono::duration_cast<chrono::microseconds>(total_dur);
 *   ~~~
 *
 * ### `Accumulator A` type requirements/recommendations ###
 * See timed_function().
 *
 * @tparam Accumulator
 *         See timed_function().
 * @tparam Handler
 *         Handler meant to be `post()`ed or otherwise async-executed on a `Task_engine`. Can take any arbitrary number
 *         of args, possibly none.
 * @param clock_type
 *        See timed_function().
 * @param accumulator
 *        See timed_function().
 * @param handler
 *        The handler to execute and time.
 * @return A closure that will time and execute `handler()`, adding the elapsed time to `accumulator`; bound
 *         to the same executor (if any; e.g., a util::Strand) to which `handler` is bound.
 */
template<typename Accumulator, typename Handler>
auto timed_handler(Clock_type clock_type, Accumulator* accumulator, Handler&& handler);

/**
 * Prints string representation of the given `Checkpointing_timer` (whether with original data or an
 * aggregated-result timer) to the given `ostream`.  Note this is multi-line output that does *not* end in newline.
 *
 * @relatesalso Checkpointing_timer
 *
 * @param os
 *        Stream to which to write.
 * @param timer
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Checkpointing_timer& timer);

} // namespace flow::perf
