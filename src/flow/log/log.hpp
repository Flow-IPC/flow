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
#include "flow/log/detail/log_fwd.hpp"
#include "flow/log/detail/thread_lcl_str_appender.hpp"
#include "flow/util/util.hpp"
#include "flow/util/detail/util.hpp"
#include "flow/util/uniq_id_holder.hpp"
#include <boost/chrono/chrono.hpp>
#include <string>
#include <typeinfo>

// Macros.  These (conceptually) belong to the flow::log namespace (hence the prefix for each macro).

/**
 * Logs a WARNING message into flow::log::Logger `*get_logger()` with flow::log::Component `get_log_component()`, if
 * such logging is enabled by that `Logger`.  Supplies context information
 * to be potentially logged with message, like current time, source file/line/function,
 * and thread ID/nickname info, in addition to the message, component, and severity.
 * The severity checked against (and potentially logged in its own right) is flow::log::Sev:S_WARNING.
 *
 * More precisely, checks whether logging warnings is currently enabled in the `Logger*` returned by
 * `get_logger()` in the macro invocation's context; if not does nothing; if so constructs and logs the
 * message as a warning via FLOW_LOG_WITHOUT_CHECKING().  Also, if `get_logger()` is null, then the effect is
 * the same as a `get_logger()->should_log()` returning `false` (meaning it is a no-op).
 *
 * `get_logger()` must exist, and if not null then `get_logger()` (returning
 * `Logger*`) and `get_log_component()` (returning `const Component&`)
 * must return a valid pointer and reference to `Logger` and `Component`, respectively.
 * Most of the time these are available due to most logging code being in classes deriving from flow::log::Log_context
 * which supplies those methods and takes (at construction) the values to return subsequently.
 * In situations where this is impossible (such as `static` members methods or in free functions) or insufficient (such
 * as when one wants to use a different component vs. the one returned by flow::log::Log_context::get_log_component()),
 * use FLOW_LOG_SET_CONTEXT().
 *
 * ### Final log output subleties ###
 * We assume for this discussion that the notion of order of final output (to device/file/network/whatever) exists,
 * and that the `get_logger()` indeed safely outputs message M1 entirely before M2, or vice versa, for every
 * pair of messages (in this context by message we mean message+metadata pair) ever passed to `do_log()`.  With that
 * assumption in effect (IF indeed it is):
 *   - In a given thread T, and assuming the same `get_logger()`, if `FLOW_LOG_*(M1);`
 *     is called before `FLOW_LOG_*(M2);`, and both are successfully output by logger, then the final output order must
 *     have M1 precede M2, not vice versa.
 *     - In addition, assuming monotonically increasing time stamps, as time flows (which is the case most of the time
 *       with exceptions due to computer-clock adjustments and similar), M1's time stamp will be earlier (ignoring
 *       rounding) than M2's.
 *   - If, instead, M1 is logged in thread T1, while M2 is logged in thread T2:
 *     - If the 2 `FLOW_LOG_*()` calls are chronologically disjoint, then again the final output must also have
 *       M1 precede M2 if M1 went first; and vice versa.
 *       - Time stamps (under a well behaved clock again), again, will match this order.
 *     - If the 2 `FLOW_LOG_*()` calls chronologically overlap, then either final output order is possible.
 *       - Moreover if M1 precedes M2 in the output, formally time stamps might be in the opposite order
 *         (even assuming, again, a well behaved clock).  (Informally, given how this is implemented internally,
 *         this is unlikely to be observed in practice, barring a wide absolute discrepancy between
 *         how long it takes to evaluate `ARG_stream_fragment` in M1 vs. M2.)
 *
 * The time stamp is obtained as soon as practically possible within the body of this macro.  Hence it reflects the time
 * at the moment just after the pre-logging statement finished, just before the log call site executes.  *After* this
 * is when `ARG_stream_fragment` is evaluated -- assuming the filter check passes -- and only after that might the
 * final output get queued (or possibly synchronously output, depending on nature of `get_logger()`).  This is why it's
 * technically possible that (even in the absence of system time going backwards) 2 messages from 2 different threads
 * might appear in the final output with slightly out-of-order time stamps.  Informally, this is unlikely, because
 * the `ARG_stream_fragment` evaluation would be on the order of a bunch of instructions that would complete in less
 * time than the microsecond resolution of time stamp output, in most cases, or maybe a handful of microseconds.
 * Anecdotally, I (ygoldfel) don't recall one instance of seeing this (out-of-order time stamps due to the
 * concurrency race implied by the above mechanism).
 * Nevertheless I mention it here for completeness, as well as to explain how it works.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WITHOUT_CHECKING().
 *
 * @todo We can avoid using macros for this and similar APIs by requiring the user to use commas instead of the usual
 * `<<` and by implementing this as a variadic function template (C++11 feature).
 *
 * Thus, while it'd be no longer possible to write
 *
 *   ~~~
 *   FLOW_LOG_WARNING("Result: [" << std::setfill('0') << num << "].");
 *   ~~~
 *
 * one would instead write
 *
 *   ~~~
 *   flow::log::warning("Result: [", std::setfill('0'), num, "].");
 *   ~~~
 *
 * which is fairly close and still reasonably readable.  However, one would need to be mindful of
 * performance; hopefully the optimizer would still inline everything
 * instead of adding a number of function calls compared to this macro implementation whose side benefit is
 * guaranteed inlining.  Generally, though, macros are bad and should be eliminated where possible; just don't mess
 * up speed in something as common as logging.  In addition, if it's NOT inlined, the number of functions generated
 * by the template's many instantiations would be rather large, though I suppose that's not necessarily *worse* than
 * full-on inlining thereof -- just *different*.  (Still, consider how many different configurations would pop up
 * as a result of all the different log messages!)  Also keep in mind that *fully* inlining all of this would require
 * the build engine to be capable of link-time optimization (FLTO), and the build script to enable FLTO.
 * *Unfortunately* it would be impossible for the non-macro to refer to a `get_logger()`
 * in the call's context, so it would be necessary for this to be passed in as an argument,
 * significantly lowering the ease of use of the API.  That said, flow::log::Logger itself could simply
 * implement all these APIs as class methods instead of their being free functions.
 * Then one could even (when desired) write such things as
 *
 *   ~~~
 *   Logger some_logger(...); // Some Logger that is not available through get_logger() as would be more typical.
 *   some_logger.warning("Error detected: [", err_num, "].");
 *   ~~~
 *
 * and therefore FLOW_LOG_SET_CONTEXT() (another "tricky" macro) could be eliminated due to lack of necessity.
 * Finally, flow::log::Log_context (in the current design, to be derived from by all logging classes) would be used
 * like this:
 *
 *   ~~~
 *   get_logger()->warning("Error detected: [", err_num, "].");
 *   ~~~
 *
 * It might also be advisable, for code brevity in such commonly referenced APIs, to add trivial forwarding methods
 * to flow::log::Log_context.  This is slightly questionable, as it's quite a bit of boiler-plate (needed every time one
 * might change this overall API) just to remove a few characters from each log call.  The above call would become:
 *
 *   ~~~
 *   log_warning("Error detected: [", err_num, "]."); // Invoke superclass Log_context's Logger::warning() method.
 *   ~~~
 *
 * which is a little more compact.  That can also be accomplished by having flow::log::Log_context implement
 * flow::log::Logger itself.  As a last note, `__LINE__` (etc.) can only be made useful via a macro, so one would still
 * be required to wrap around any API suggested above in a simple macro -- but this would be far superior (in this
 * particular dimension of avoiding macro insanity) to the level of macro-ness required at the moment.  All in all,
 * while I do hate macros, the present design seems reasonably strong, so the above rejiggering ideas don't feel
 * like no-brainers.
 */
#define FLOW_LOG_WARNING(ARG_stream_fragment) \
  FLOW_LOG_WITH_CHECKING(::flow::log::Sev::S_WARNING, ARG_stream_fragment)

/**
 * Logs a FATAL message into flow::log::Logger `*get_logger()` with
 * flow::log::Component `get_log_component()`, if such logging
 * is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING() but for the flow::log::Sev::S_FATAL severity.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_FATAL(ARG_stream_fragment) \
  FLOW_LOG_WITH_CHECKING(::flow::log::Sev::S_FATAL, ARG_stream_fragment)

/**
 * Logs an ERROR message into flow::log::Logger `*get_logger()` with
 * flow::log::Component `get_log_component()`, if such logging
 * is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING() but for the flow::log::Sev::S_ERROR severity.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_ERROR(ARG_stream_fragment) \
  FLOW_LOG_WITH_CHECKING(::flow::log::Sev::S_ERROR, ARG_stream_fragment)

/**
 * Logs an INFO message into flow::log::Logger `*get_logger()` with
 * flow::log::Component `get_log_component()`, if such logging
 * is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING() but for the flow::log::Sev::S_INFO severity.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_INFO(ARG_stream_fragment) \
  FLOW_LOG_WITH_CHECKING(::flow::log::Sev::S_INFO, ARG_stream_fragment)

/**
 * Logs a DEBUG message into flow::log::Logger `*get_logger()` with
 * flow::log::Component `get_log_component()`, if such logging
 * is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING() but for the flow::log::Sev::S_DEBUG severity.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_DEBUG(ARG_stream_fragment) \
  FLOW_LOG_WITH_CHECKING(::flow::log::Sev::S_DEBUG, ARG_stream_fragment)

/**
 * Logs a TRACE message into flow::log::Logger `*get_logger()` with
 * flow::log::Component `get_log_component()`, if such logging
 * is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING() but for the flow::log::Sev::S_TRACE severity.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_TRACE(ARG_stream_fragment) \
  FLOW_LOG_WITH_CHECKING(::flow::log::Sev::S_TRACE, ARG_stream_fragment)

/**
 * Logs a DATA message into flow::log::Logger `*get_logger()` with
 * flow::log::Component `get_log_component()`, if such logging
 * is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING() but for the flow::log::Sev::S_DATA severity.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_DATA(ARG_stream_fragment) \
  FLOW_LOG_WITH_CHECKING(::flow::log::Sev::S_DATA, ARG_stream_fragment)

/**
 * Logs a WARNING message into flow::log::Logger `*get_logger()` with
 * flow::log::Component `get_log_component()` regardless of
 * whether such logging is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING() but without checking for
 * whether it is enabled (you should do so yourself; see the following note on this topic).
 *
 * @note If `get_logger()` is null, this is a no-op.  In practice, though,
 *       this case should have been eliminated as part of heeding the following warning:
 *
 * @warning If invoking this directly, API user must manually ensure the severity is enabled in the logger.
 *          Not doing so breaks (unenforced but nevertheless mandatory) rules of logging system.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_WARNING_WITHOUT_CHECKING(ARG_stream_fragment) \
  FLOW_LOG_WITHOUT_CHECKING(::flow::log::Sev::S_WARNING, ARG_stream_fragment)

/**
 * Logs a FATAL message into flow::log::Logger `*get_logger()`
 * with flow::log::Component `get_log_component()` regardless of
 * whether such logging is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING_WITHOUT_CHECKING()
 * but for the flow::log::Sev::S_FATAL severity.
 *
 * @note Same warnings and notes as for FLOW_LOG_WARNING_WITHOUT_CHECKING().
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_FATAL_WITHOUT_CHECKING(ARG_stream_fragment) \
  FLOW_LOG_WITHOUT_CHECKING(::flow::log::Sev::S_FATAL, ARG_stream_fragment)

/**
 * Logs an ERROR message into flow::log::Logger `*get_logger()`
 * with flow::log::Component `get_log_component()` regardless of
 * whether such logging is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING_WITHOUT_CHECKING()
 * but for the flow::log::Sev::S_ERROR severity.
 *
 * @note Same warnings and notes as for FLOW_LOG_WARNING_WITHOUT_CHECKING().
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_ERROR_WITHOUT_CHECKING(ARG_stream_fragment) \
  FLOW_LOG_WITHOUT_CHECKING(::flow::log::Sev::S_ERROR, ARG_stream_fragment)

/**
 * Logs an INFO message into flow::log::Logger `*get_logger()`
 * with flow::log::Component `get_log_component()` regardless of
 * whether such logging is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING_WITHOUT_CHECKING()
 * but for the flow::log::Sev::S_INFO severity.
 *
 * @note Same warnings and notes as for FLOW_LOG_WARNING_WITHOUT_CHECKING().
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_INFO_WITHOUT_CHECKING(ARG_stream_fragment) \
  FLOW_LOG_WITHOUT_CHECKING(::flow::log::Sev::S_INFO, ARG_stream_fragment)

/**
 * Logs a DEBUG message into flow::log::Logger `*get_logger()`
 * with flow::log::Component `get_log_component()` regardless of
 * whether such logging is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING_WITHOUT_CHECKING()
 * but for the flow::log::Sev::S_DEBUG severity.
 *
 * @note Same warnings and notes as for FLOW_LOG_WARNING_WITHOUT_CHECKING().
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_DEBUG_WITHOUT_CHECKING(ARG_stream_fragment) \
  FLOW_LOG_WITHOUT_CHECKING(::flow::log::Sev::S_DEBUG, ARG_stream_fragment)

/**
 * Logs a TRACE message into flow::log::Logger `*get_logger()`
 * with flow::log::Component `get_log_component()` regardless of
 * whether such logging is enabled by the flow::log::Logger.  Analogous to FLOW_LOG_WARNING_WITHOUT_CHECKING()
 * but for the flow::log::Sev::S_TRACE severity.
 *
 * @note Same warnings and notes as for FLOW_LOG_WARNING_WITHOUT_CHECKING().
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_TRACE_WITHOUT_CHECKING(ARG_stream_fragment) \
  FLOW_LOG_WITHOUT_CHECKING(::flow::log::Sev::S_TRACE, ARG_stream_fragment)

/**
 * Logs a DATA message into flow::log::Logger `*get_logger()`
 * with flow::log::Component `get_log_component()` regardless of
 * whether such logging is enabled by the flow::log::Logger.  Analogous to
 * FLOW_LOG_WARNING_WITHOUT_CHECKING() but for the flow::log::Sev::S_DATA severity.
 *
 * @note Same warnings and notes as for FLOW_LOG_WARNING_WITHOUT_CHECKING().
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 */
#define FLOW_LOG_DATA_WITHOUT_CHECKING(ARG_stream_fragment) \
  FLOW_LOG_WITHOUT_CHECKING(::flow::log::Sev::S_DATA, ARG_stream_fragment)

/**
 * For the rest of the block within which this macro is instantiated, causes all `FLOW_LOG_...()`
 * invocations to log to `ARG_logger_ptr` with component `flow::log::Component(ARG_component_payload)`, instead of the
 * normal `get_logger()` and `get_log_component()`, if there even such things are available in the block.  This is
 * useful, for example, in `static` methods, where there is no `get_logger()` or `get_log_component()` function defined,
 * but a flow::log::Logger and component payload are available (for example) via parameters.  It's also useful if one
 * wants to log to a different `Logger*` for some reason and/or (perhaps more likely) a different component.
 * Note that this creates or changes the meaning of the identifiers `get_logger` and `get_log_component` for the rest of
 * the block; in fact you may call `get_logger()` or `get_log_component()` directly for whatever nefarious purposes you
 * require, though it is suspected to be rare compared to just using `FLOW_LOG_...()` normally.
 *
 * Example:
 *   ~~~
 *   // Suppose flow_node is a flow::Node object, which derives from Log_context.
 *   FLOW_LOG_SET_CONTEXT(flow_node.get_logger(), My_project_components::S_COOL_MODULE);
 *   // Logs with component S_COOL_MODULE to logger set above.
 *   FLOW_LOG_WARNING("Something horrible happened: [" << error_str << "].");
 *   ~~~
 *
 * @note It will not compile if used 2+ times in the same block at the same nesting level.
 *       (The same applies to mixing with FLOW_LOG_SET_COMPONENT() or FLOW_LOG_SET_LOGGER().)
 *       However, one can create sub-blocks to work around this (likely very infrequent) situation.
 *
 * @param ARG_logger_ptr
 *        `ARG_logger_ptr` will be used as the `Logger*` in subsequent `FLOW_LOG_...()`
 *        invocations in this block.
 * @param ARG_component_payload
 *        `Component(ARG_component_payload)`, a light-weight holder of a copy of `ARG_component_payload`, will be used
 *        as the `const Component&` in subsequent `FLOW_LOG_...()` invocations in this block.
 */
#define FLOW_LOG_SET_CONTEXT(ARG_logger_ptr, ARG_component_payload) \
  FLOW_LOG_SET_LOGGER(ARG_logger_ptr); \
  FLOW_LOG_SET_COMPONENT(ARG_component_payload);

/**
 * Equivalent to FLOW_LOG_SET_CONTEXT() but sets the `get_logger` only.
 *
 * @param ARG_logger_ptr
 *        See FLOW_LOG_SET_CONTEXT().
 */
#define FLOW_LOG_SET_LOGGER(ARG_logger_ptr) \
  [[maybe_unused]] \
    const auto get_logger \
      = [logger_ptr_copy = static_cast<::flow::log::Logger*>(ARG_logger_ptr)] \
          () -> ::flow::log::Logger* { return logger_ptr_copy; }

/**
 * Equivalent to FLOW_LOG_SET_CONTEXT() but sets the `get_log_component` only.
 *
 * @internal
 * ### Impl/perf discussion ###
 * If a free or `static` member function or lambda is frequently called, and it needs to log, it will make perf
 * aspects of this macro (and thus of FLOW_LOG_SET_CONTEXT()) significant to overall perf.  Hence let's unpack
 * what computation occurs.
 *   - When this macro (and thus FLOW_LOG_SET_CONTEXT()) is called (usually near the top of free/whatever function):
 *     It creates a lambda with one capture, a flow::log::Component, constructed via its 1-arg ctor form.
 *     That lambda object is, in fact, identical to a `struct` instance with a single `Component` member thus
 *     constructed.  As of this writing that constuctor: saves the pointer `&(typeid(T))`, where `T` is the particular
 *     `enum class` type of `ARG_component_payload`; and saves the integer payload of the `enum` value
 *     `ARG_component_payload`.  A pointer and an integer are light-weight to copy, indeed.  Additionally, in practice,
 *     both values are probably known at compile time; hence the compiler might further inline the "copy."
 *     Hence we conclude the copy operation is fast.
 *     - The quasi-`struct`'s two members are stored directly on the stack.  There is no heap involved.  Hence we
 *       conclude there is no hidden cost in messing with the heap by either using a lambda or whatever is stored
 *       inside the lambda.
 *   - When `get_log_component()` is called (at every log call site in the free/whatever function, probably):
 *     It calls the aforementioned lambda's compiler-generated `operator()()`.  This just returns a pointer to the
 *     stack-stored `struct`.  Compilers tend to auto-inline (short) lambda `operator()()` calls, so there's probably
 *     not even the overhead of a function call.  Hence we conclude this is pretty fast.
 *
 * Looks fine perf-wise (at least in theory).
 * @endinternal
 *
 * @param ARG_component_payload
 *        See FLOW_LOG_SET_CONTEXT().
 */
#define FLOW_LOG_SET_COMPONENT(ARG_component_payload) \
  [[maybe_unused]] \
    const auto get_log_component = [component = ::flow::log::Component(ARG_component_payload)] \
                                     () -> const ::flow::log::Component & \
  { \
    return component; \
  }

/**
 * Logs a message of the specified severity into flow::log::Logger `*get_logger()` with flow::log::Component
 * `get_log_component()` if such logging is enabled by said flow::log::Logger.  The behavior is identical
 * to that by FLOW_LOG_WARNING() and similar, but one specifies the severity as an argument instead of it being
 * hard-coded into the macro name itself.
 *
 * @note It is important that ARG_stream_fragment is actually evaluated only if Logger::should_log() is
 *       true.  Otherwise resources are wasted on constructing a message string that never gets
 *       logged.  That's a (the?) reason Logger:should_log() and Logger::do_log() are mutually decoupled in
 *       that interface.
 *
 * @param ARG_sev
 *        Severity (type log::Sev).
 * @param ARG_stream_fragment
 *        Same as in FLOW_LOG_WARNING().
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @internal
 * @todo In FLOW_LOG_WITH_CHECKING(), save `get_logger()` and `get_log_component()` return values in such a way as to be
 * reused by the FLOW_LOG_WITHOUT_CHECKING() invoked by the former macro if `should_log() == true`.  As it stands, they
 * are called again inside the latter macro.  In the most-common case, wherein Log_context is used for those
 * two expressions, this should get inline-optimized to be maximally fast anyway.  With `FLOW_LOG_SET_*()`, though,
 * it might be a bit slower than that.  Technically, one can make their own `get_logger` and `get_log_component`
 * identifiers that might do something slower still -- though it's unlikely (and as of this writing unprecedented).
 * So I would not call this pressing, but on the other hand... just do it!  The implementation code will be somewhat
 * hairier though.
 */
#define FLOW_LOG_WITH_CHECKING(ARG_sev, ARG_stream_fragment) \
  FLOW_UTIL_SEMICOLON_SAFE \
  ( \
    ::flow::log::Logger const * const FLOW_LOG_W_CHK_logger = get_logger(); \
    if (FLOW_LOG_W_CHK_logger && FLOW_LOG_W_CHK_logger->should_log(ARG_sev, get_log_component())) \
    { \
      FLOW_LOG_WITHOUT_CHECKING(ARG_sev, ARG_stream_fragment); \
    } \
  )

/**
 * Identical to FLOW_LOG_WITH_CHECKING() but foregoes the filter (Logger::should_log()) check.  No-op if
 * `get_logger()` returns null.  Internally, all other log-call-site macros ultimately build on top of this one
 * except FLOW_LOG_DO_LOG().
 *
 * Context information obtained and possibly logged is file/function/line, thread nickname/ID, time stamp, etc.
 * The message is given as the `<<`-using `ostream` fragment
 * in `ARG_stream_fragment` argument.  Example (note the 2nd argument containing a stream output fragment):
 *
 *   ~~~
 *   FLOW_LOG_WITHOUT_CHECKING(flow::log::Sev::S_WARNING,
 *                             "Failed [" << n_times << "] times; bailing out!");
 *   ~~~
 *
 * @note This macro is the lowest-level API that user should invoke to actually log, except FLOW_LOG_DO_LOG().
 *       Usually she'd invoke something higher-level like FLOW_LOG_WARNING(), etc.; but at times more control
 *       is desirable for performance or other reasons -- but even then one should not call anything below this
 *       level without an extremely excellent reason.  See FLOW_LOG_DO_LOG() for discussion of the latter.
 *
 * @warning If invoking this directly, API user must manually ensure the severity is enabled in the `Logger`.
 *          Not doing so breaks (unenforced but nevertheless mandatory) rules of logging system.
 *
 * ### Severity selection ###
 * Before selecting a severity for your log call site, please consider the discussion in the flow::log::Sev
 * doc header.
 *
 * @param ARG_sev
 *        Severity (type flow::log::Sev).
 * @param ARG_stream_fragment
 *        Fragment of code as if writing to a standard `ostream`.
 *        A terminating newline will be auto-appended to this eventually and therefore should generally not
 *        be included by the invoker.  (Such a terminating newline would manifest as a blank line, likely.)
 */
#define FLOW_LOG_WITHOUT_CHECKING(ARG_sev, ARG_stream_fragment) \
  FLOW_UTIL_SEMICOLON_SAFE \
  ( \
    using ::flow::util::Thread_id; \
    using ::flow::log::Logger; \
    using ::flow::log::Component; \
    using ::flow::util::String_view; \
    using ::flow::util::get_last_path_segment; \
    using ::boost::chrono::system_clock; \
    using ::std::string; \
    Logger* const FLOW_LOG_WO_CHK_logger = get_logger(); \
    if (!FLOW_LOG_WO_CHK_logger) /* Usually a preceding filter/should_log() check would've eliminated this but.... */ \
    { \
      break; \
    } \
    /* else */ \
    /* Important: This is from the time-of-day/calendar clock, which is not steady, monotonic, etc.; *but* it is */ \
    /* convertible to a UTC time with cosmic meaning to humands; that is invaluable. */ \
    auto const& FLOW_LOG_WO_CHK_time_stamp = system_clock::now(); \
    /* See Msg_metadata::m_msg_src_file doc. */ \
    /* @todo The __FILE/FUNCTION__ stuff was far more inlined, but gcc 5.4 hit an internal compiler error (bug)! */ \
    /* This verbose step-by-step way of doing it jiggled the bug away.  Put it back when bug goes away maybe. */ \
    /* Note either way it's optimized to the same thing, since the following 7 expressions all have constant */ \
    /* values at compile time as desired in m_msg_src_file doc header; and even formally guaranteed via constexpr. */ \
    /* Warning: Something about this is brittle, in that if one makes a false move the compiler bug comes back. */ \
    /* That's even when doing it this multi-step way.  Leaving this comment in as moral support for maintainers. */ \
    constexpr char const * FLOW_LOG_WO_CHK_file_ptr = __FILE__; \
    constexpr size_t FLOW_LOG_WO_CHK_file_sz = sizeof(__FILE__) - 1; \
    constexpr char const * FLOW_LOG_WO_CHK_func_ptr = __FUNCTION__; \
    constexpr size_t FLOW_LOG_WO_CHK_func_sz = sizeof(__FUNCTION__) - 1; \
    constexpr String_view FLOW_LOG_WO_CHK_full_file_str(FLOW_LOG_WO_CHK_file_ptr, FLOW_LOG_WO_CHK_file_sz); \
    /* Yes -- get_last_path_segment() is constexpr and will thus "execute" at compile time! */ \
    constexpr String_view FLOW_LOG_WO_CHK_file_str = get_last_path_segment(FLOW_LOG_WO_CHK_full_file_str); \
    constexpr String_view FLOW_LOG_WO_CHK_func_str(FLOW_LOG_WO_CHK_func_ptr, FLOW_LOG_WO_CHK_func_sz); \
    const Component& FLOW_LOG_WO_CHK_component = get_log_component(); \
    string FLOW_LOG_WO_CHK_call_thread_nickname; \
    Thread_id FLOW_LOG_WO_CHK_call_thread_id; \
    Logger::set_thread_info(&FLOW_LOG_WO_CHK_call_thread_nickname, &FLOW_LOG_WO_CHK_call_thread_id); \
    FLOW_LOG_DO_LOG(FLOW_LOG_WO_CHK_logger, FLOW_LOG_WO_CHK_component, ARG_sev, FLOW_LOG_WO_CHK_file_str, __LINE__, \
                    FLOW_LOG_WO_CHK_func_str, FLOW_LOG_WO_CHK_time_stamp, FLOW_LOG_WO_CHK_call_thread_nickname, \
                    FLOW_LOG_WO_CHK_call_thread_id, ARG_stream_fragment); \
    /* FLOW_LOG_WO_CHK_call_thread_nickname is now hosed. */ \
  ) /* FLOW_UTIL_SEMICOLON_SAFE() */

/**
 * Lowest-level logging API accessible to the user, this is identical to FLOW_LOG_WITHOUT_CHECKING() but expects all
 * pieces of metadata in addition to the message and log::Sev, plus the flow::log::Logger, to be supplied as macro
 * arguments.
 *
 * Internally, all other log-call-site macros ultimately build on top of this one.
 *
 * @note From public user's point of view: It's flow::log::Logger::should_log() that allows the message argument to be
 *       built using `ostream<<` semantics instead of having to instantiate an intermediate flow::util::String_ostream
 *       (which has performance overhead and is more verbose).  Why not just use a higher-level macro -- at least
 *       as high-level as FLOW_LOG_WITHOUT_CHECKING() -- instead?  Answer: In some cases there is a source of metadata,
 *       like file and line number, that comes from a different source than (e.g.) `__FILE__` and `__LINE__` at the log
 *       call site; e.g., when logging from another log API through flow::log.
 *
 * @warning If invoking this directly, API user must manually ensure the severity/component is enabled in the `Logger`.
 *          Not doing so breaks (unenforced but nevertheless mandatory) rules of logging system.
 * @warning If invoking this directly, API user must manually ensure `ARG_logger_ptr` is not null.  Otherwise behavior
 *          is undefined.
 *
 * @param ARG_logger_ptr
 *        A `Logger*` through which to log; not null.
 * @param ARG_component
 *        See Msg_metadata (reference copied into it).
 * @param ARG_sev
 *        See Msg_metadata (`enum` value copied into it).
 * @param ARG_file_view
 *        See Msg_metadata (`String_view` copied into it).  Reminder: Underlying memory may need to remain valid
 *        asynchronously (read: indefinitely); usually it's a literal in static storage.
 * @param ARG_line
 *        See Msg_metadata (integer copied into it).
 * @param ARG_func_view
 *        See Msg_metadata (`String_view` copied into it).  Reminder: Same as for `ARG_file_view`.
 * @param ARG_time_stamp
 *        See Msg_metadata (scalar copied into it).
 * @param ARG_call_thread_nickname_str_moved
 *        See Msg_metadata (this `std::string` is *moved* into it and thus made empty).
 * @param ARG_call_thread_id
 *        See Msg_metadata (scalar copied into it).
 * @param ARG_stream_fragment
 *        See FLOW_LOG_WITHOUT_CHECKING().
 *
 * @internal
 *
 * ### Implementation discussion ###
 *
 * The implementation here must be as performant as humanly possible.  Every single logged message (albeit
 * only assuming severity [or any other filter] checks have passed, meaning a message is IN FACT logged) will
 * execute this code.
 *
 * In this implementation, one keeps reusing a thread-local `string`, cleared each time this is invoked and then
 * written to.  (The clearing doesn't deallocate anything; it only updates an internal length integer to 0!)
 * If Logger::logs_asynchronously() is `false`, then the Logger synchronously outputs the message and has no need
 * to make some intemediate copy of either the message or the metadata (time stamp, etc.).  However, if
 * it is `true` (as for heavy-duty logger Async_file_logger), then Logger must make a copy of the aforementioned
 * thread-local message string, so that it can be asynchronously logged later (probably by some other worker thread
 * used by the Logger), and deallocated.  This implementation allows for both work-flows; also see below to-do.
 *
 * @todo An alternative copy-free implementation of the asynchronous FLOW_LOG_DO_LOG() work-flow
 * is possible.  The basic idea of using a thread-local non-reallocating work string is just fine in the
 * non-`logs_asynchronously()` (i.e., synchronous) Logger flow.  In the asynchronous flow, however, it involves
 * an added message copy.  Instead -- as done in certain major server software author is familiar with -- one
 * could (perhaps in the async flow only) allocate a new string in each FLOW_LOG_DO_LOG() (in rare cases
 * reallocating, even repeatedly, if more space is needed); then pass that pointer around, until it is asynchronously
 * written out by Logger impl; then deallocate it.  Thus, a copy is eliminated in the async workflow.  A complicating
 * factor is that the current system maintains a format state across separate log call sites in a given thread; this
 * change would (if naively implemented at least) eliminate that feature -- but this could be considered acceptable.
 * (Still, do realize that, for example, in today's feature set one can set the `chrono` I/O formatting to show short
 * unit names like `2ms` instead of `2 milliseconds`; and one need do it only once; but with this change one would
 * need to do it in every log call site.  That would be, I can attest, rather annoying.  Additionally, make sure the
 * behavior is consistent in the sync and async work-flows.)  A related wrinkle is that if we add special support for
 * `printf()`-style log call sites (a to-do in flow::log doc header as of this writing), then in that case since
 * there is no expectation of such format statefulness in the first place, in that flow that particular concern isn't
 * a concern.  (Sub-to-do: If one did this, an extra-optimized further idea is to avoid the repeated allocs/deallocs
 * by maintaining a pool of already-allocated buffers to be reused opportunistically.)  Bottom line: I claim the
 * existing thing is pretty good; the extra copy is IMO unlikely to affect real performance, because (1) it's only
 * one copy in the context of quite a bit of similar copying and other ops going on while writing out the string; and
 * (2) if the message is so verbose as to affect performance *at all*, then it will probably affect it regardless of
 * the extra copy (in other words, its verbosity must be increased, or the filter verbosity must be decreased --
 * avoiding this exta internal copy feels in my [ygoldfel] personal opinion like rearranging deck chairs on the
 * Titanic).  So, this to-do should probably be to-done at some point, but it doesn't feel urgent.  And since there are
 * quite a few subtleties involved, as shown above, it's natural to resist doing it until truly necessary.
 *
 * @todo Time stamp subtlety: It might not be crazy to log not just the time stamp of entry to this macro but also
 * some indication how long it took to evaluate the rest and finally output it to the ultimate device/whatever.
 * It could be an optional differential or something, like "timestamp+diff," with diff in microseconds or something.
 *
 * @todo Time stamp source: The current implementation uses the system clock to generate time stamps (a/k/a POSIX time),
 * but consider optionally or mandatorily using the high-resolution clock instead (or additionally?).
 * This has pros and cons; all or most time stamps elsewhere use the system clock also, so this allows for easy
 * cross-referencing against other systems and logs.  There's also the question of how to express an absolute time,
 * as usually the high-resolution clock starts at system startup -- not as humanly useful as a "calendar" (UTC) time,
 * which -- while useful humanly -- is *not* steady/monotonic/etc.  There is no reasonable conversion between
 * `Fine_clock::now()` and a calendar time (boost.chrono docs say this unequivocally which is a confirmation).
 * The pros include: (1) higher precision and resolution; (2) that time always flows forward and at a uniform rate
 * without possibility of time going back or forward due to human/otherwise clock sets or rare events like daylight
 * savings and leap seconds; or (3) to summarize, something more suited as a quick-and-dirty way to measure how long
 * things take in the program, like an always-on, log-integrated version of perf::Checkpointing_timer.
 * As of this writing all out-of-the-box log::Logger implementations and log::Config allow the output of human-readable
 * as well as sec.usec-from-Epoch time stamps.  One approach might be to replace the latter *only* with the high-rez
 * clock's time stamps, perhaps optionally, while leaving the human-readable one alone.  Note: There is an important
 * test to perform here, which is the time cost of obtaining either time stamp.  E.g., the high-rez time stamp might
 * be much slower -- or maybe the opposite!  To test this, (1) add the POSIX-time clock into the perf::Clock_type
 * `enum`, with all associated (fairly straightforward) changes in `flow::perf`; and (2) test the perf characteristics
 * of this new clock.  Certain code exists outside of Flow itself that already automatically tests all `Clock_type`s,
 * so it would quickly give the answer.  (Secondary to-do: Be less vague about where this program resides, in this
 * comment.  I, ygoldfel, have the answer at any rate and am only omitting it here for boring process reasons.)
 */
#define FLOW_LOG_DO_LOG(ARG_logger_ptr, \
                        ARG_component, ARG_sev, ARG_file_view, ARG_line, ARG_func_view, \
                        ARG_time_stamp, ARG_call_thread_nickname_str_moved, \
                        ARG_call_thread_id, ARG_stream_fragment) \
  FLOW_UTIL_SEMICOLON_SAFE \
  ( \
    using ::flow::log::Thread_local_string_appender; \
    using ::flow::log::this_thread_sync_msg_metadata_ptr; \
    using ::flow::log::Logger; \
    using ::flow::log::Msg_metadata; \
    using ::flow::util::String_view; \
    using ::std::flush; \
    Logger* const FLOW_LOG_DO_LOG_logger = ARG_logger_ptr; /* Note: As advertised, it must NOT be null. */ \
    /* If first use this thread/logger combo, create necessary structures for writing stream fragment to a string. */ \
    /* If subsequent use with that combo, reuse already created structures to save cycles. */ \
    /* We could've just created string { locally } and used util::ostream_op_to_string(), which would */ \
    /* have been easier, but doing it this way should be faster, as it's just: a thread-local lookup, */ \
    /* a string clearing which should amount to a single length assignment internally, and finally a */ \
    /* write to an existing stream adapter, which would have been necessary regardless.  The alternative */ \
    /* would involve creating the string and the adapter machinery { locally } first -- every time. */ \
    /* Update: Also, this way we get the continuous but distinct ostream state as documented in class Logger doc. */ \
    /* Update 2: See to-do in FLOW_LOG_WITHOUT_CHECKING() doc header. */ \
    auto& FLOW_LOG_DO_LOG_appender \
      = *(Thread_local_string_appender::get_this_thread_string_appender(*FLOW_LOG_DO_LOG_logger)); \
    auto& FLOW_LOG_DO_LOG_os = *(FLOW_LOG_DO_LOG_appender.fresh_appender_ostream()); \
    FLOW_LOG_DO_LOG_os << ARG_stream_fragment << flush; \
    /* They gave us all the pieces of Msg_metadata, so we just put the object together. */ \
    Msg_metadata* FLOW_LOG_DO_LOG_msg_metadata_ptr; \
    if (FLOW_LOG_DO_LOG_logger->logs_asynchronously()) \
    { \
      /* () used to avoid nested-macro-comma trouble. */ \
      (FLOW_LOG_DO_LOG_msg_metadata_ptr \
         = new Msg_metadata{ ARG_component, ARG_sev, ARG_file_view, ARG_line, ARG_func_view, \
                             ARG_time_stamp, std::move(ARG_call_thread_nickname_str_moved), ARG_call_thread_id }); \
      /* FLOW_LOG_DO_LOG_msg_metadata_ptr will be async-deleted by Logger (see below). */ \
    } \
    else \
    { \
      FLOW_LOG_DO_LOG_msg_metadata_ptr = this_thread_sync_msg_metadata_ptr.get(); \
      if (!FLOW_LOG_DO_LOG_msg_metadata_ptr) \
      { /* This happens once per thread at most. */ \
        this_thread_sync_msg_metadata_ptr.reset(FLOW_LOG_DO_LOG_msg_metadata_ptr = new Msg_metadata); \
      } \
      /* () used to avoid nested-macro-comma trouble. */ \
      ((*FLOW_LOG_DO_LOG_msg_metadata_ptr) \
         = { ARG_component, ARG_sev, ARG_file_view, ARG_line, ARG_func_view, \
             ARG_time_stamp, std::move(ARG_call_thread_nickname_str_moved), ARG_call_thread_id }); \
      /* *FLOW_LOG_DO_LOG_msg_metadata_ptr will be overwritten next time in this thread, and again and again. */ \
    } \
    /* Time to log it finally and (if applicable) clean up. */ \
    /* Asynchronous path (see above `if`): */ \
    /*   target_contents() returns const reference that we pass through without copying. */ \
    /*   However, FLOW_LOG_DO_LOG_logger with true logs_asynchronously() knows it needs to make a copy of this to */ \
    /*   log that later, asynchronously. */ \
    /*   Also it specifically knows *it* must `delete` FLOW_LOG_DO_LOG_msg_metadata_ptr when done with it */ \
    /*   subsequently. */ \
    /* Synchronous path: */ \
    /*   This path is simpler.  Logger won't make a copy of the message, to which we still pass a ref; */ \
    /*   and it specifically knows it must NOT `delete` FLOW_LOG_DO_LOG_msg_metadata_ptr.  */ \
    /*   However, for an alleged perf bump (@todo verify!) we use a */ \
    /*   thread-local Msg_metadata to avoid making this thing on the stack and then destroying almost immediately. */ \
    FLOW_LOG_DO_LOG_logger->do_log(FLOW_LOG_DO_LOG_msg_metadata_ptr, \
                                   String_view(FLOW_LOG_DO_LOG_appender.target_contents())); \
  ) /* FLOW_UTIL_SEMICOLON_SAFE() */

namespace flow::log
{
// Types.

/**
 * A light-weight class, each object storing a *component* payload encoding an `enum` value from `enum` type of
 * user's choice, and a light-weight ID of that `enum` type itself.
 * A Component is supplied by the user, at every logging call site, along with the message.
 * Log_context (or FLOW_LOG_SET_CONTEXT() in relatively rare scenarios) makes this easier, so typically user need not
 * literally type out a component at every logging call site, meaning there is a "context" that already stores it
 * and need not be re-specified.
 *
 * A Component can be either empty, as when default-constructed, indicated by empty() returning `true`; or non-empty,
 * in which case it actually stores something interesting.  In the latter case, construct it with the templated
 * one-arg constructor.  The template arg `Payload` must, always, be user's own `enum class`.  In order to have this
 * `Payload` interpreted correctly, one can (and usually should) use the log::Config facility which is used by
 * out-of-the-box Logger implementations including Simple_ostream_logger (useful for console output) and
 * Async_file_logger (for heavy-duty file logging, including rotation support).  However, this is technically optional:
 * one can implement their own Logger which might not use the Config facility and thus deal with the meaning of
 * Component in some completely different way.  I'd start with an existing Logger implementation however; and if writing
 * one's own Logger, then still have it use log::Config, unless that system is somehow insufficient or inappropriate.
 *
 * Tip: Arguably the best way to see how to use all this together, just see flow::Flow_log_component and how Flow's own
 * code (which, itself, logs!) uses the log system.  This is discussed in more detail in the class Config doc header.
 * Lastly, some clarifying discussion may be found (as of this writing) in the Component::type_info() doc header.
 *
 * ### Thread safety, mutability ###
 * A Component is immutable as of this writing, except one can trivially overwrite a Component via an assignment
 * operator.  The latter write operation is not thread-safe w.r.t. a given `*this`, though in practice this is unlikely
 * to ever matter.
 *
 * @internal
 *
 * ### Implementation discussion ###
 * Again -- a Component conceptually must store only 2 things:
 *   - Encoding of an `enum` value.
 *   - Some ID of the `enum class` type from which this value originates.
 *
 * Why these 2 values?
 *   - When a Logger logs, it will likely want to print some string representation.  If it's just printing an integer,
 *     then it needs that integer.  If it's printing a string, it'll probably need to look up that string; for which
 *     it needs the ID of the per-logging-module component table, and of course the row within that table; as
 *     component 2 for module A is completely not the same component at all as component 2 for module B (and if it is,
 *     then it is a coincidence presumably).
 *   - When a Logger decides *whether* component C should indeed log message M (not even constructed yet at that point,
 *     for perf) at log::Sev S, it must look up whether the logging of component C messages at severity S is indeed
 *     enabled at present.  Some kind of data structure is required; for example a map (conceptually) from C to
 *     max severity S', to compare S vs. S'.  log::Config (technically optional, though all out-of-the-box `Logger`s
 *     do use it), in particular, has such a structure.
 *
 * The operations required to implement, then, are:
 *   - Construct a Component from an `enum class` value of arbitrary type (one such type per logging module).
 *   - Access the type ID of the latter type.
 *   - Access the `enum` value itself.
 *
 * The first version of Component did this by simply storing (and thus being, in terms of data, equal to) `boost::any`.
 * The constructor would simply load the `enum` value of type T as the `any` payload.  The accessors would
 * `any_cast<T>()` and then trivially access the value itself and/or its `typeid()`.
 *
 * This was delightful in terms of code simplicity.  However, due to the extreme prevalence of Logger::should_log()
 * checks in code -- at *every* log call site, including high-verbosity ones that do *not* typically result in
 * messages being logged, such as for Sev::S_TRACE -- certain perf aspects of `boost::any` involved non-trivial perf
 * costs.  Namely:
 *   - `any` constructor performs a `new` when storing the arbitrarily-typed value, even though in our case it's
 *     always just an `enum`.  Copying an `any` similarly will need to `new`.
 *   - The `any` accessors must do `virtual` lookups, to resolve to the particular `enum class` type.
 *
 * Hence the current solution optimized those issues away by making use of the fact we know the stored thing is
 * *always* an `enum class`.  Hence:
 *   - Its value can be stored as an integer.
 *   - Its type can be stored directly; and since the type is an `enum`, which is non-polymorphic, the `typeid()`
 *     operation does not even need to do the `virtual` resolution.  In fact its result is known at compile time.
 *     (This can even lead to further optimizations by the compiler.)
 *
 * Some discussion for perspective:
 *   - Using `any` was elegant in terms of code simplicity; it is analogous to runtime dynamic typing of languages
 *     like Python.
 *   - It is just that it was too slow given the frequency of the accessor operations (and, to a lesser but still
 *     potentially significant degree -- as with FLOW_LOG_SET_CONTEXT() -- for construction/copying).
 *   - Therefore it was replaced by a less-pithy hand-optimized solution that doesn't use Python-style dynamic typing
 *     but instead encodes the 2 bits of info directly by using the fact it's always an `enum`.
 *   - This does not mean `boost::any` is just too slow, because most operations are not done
 *     as frequently as should-log checks.  It *does* mean it may be too slow in production in this *particular*
 *     context.
 *     - It has been alleged that `std::any` (appearing first in C++17) in some or all gcc versions optimized-away these
 *       perf problems by storing certain values directly inside it, when those values were small enough to "fit."
 *       (Our `enum`s presumably would fit that bill, being glorified `int`s.)  This is believable: Many `std::string`
 *       impls will directly store strings of sufficiently small length; e.g., <=15 bytes would fit into the same area
 *       as the pointers and length data members required for arbitrarily-long string that do require heap allocation.
 *       Probably the same can be done for `any`.  So keep that in mind if using `any` functionality in other contexts.
 */
class Component
{
public:
  // Types.

  /// The type `Payload` must be `enum class Payload : enum_raw_t`: an `enum` type encoded via this integer type.
  using enum_raw_t = unsigned int;

  // Constructors/destructor.

  /**
   * Constructs a Component that stores no payload; meaning an unspecified-component Component that returns
   * `empty() == true`.  Every Logger and all other systems must accept a message with such a null Component.
   * However, one can configure a given Logger to ensure such messages not be logged (filtered out via `should_log()`).
   * Point is, any log call site that supplied a null Component must still work, meaning not cause undefined behavior.
   */
  Component();

  /**
   * Constructs a Component with the given payload of arbitrary type, so long as that type is an
   * `enum class : Component::enum_raw_t`.  (#enum_raw_t is an unsigned integer type.)
   * The resulting Component will return `empty() == false`.
   *
   * @tparam Payload
   *         The type of the component value stored inside `*this`.  This is required to be a type satisfying
   *         the requirements in the doc header for #enum_raw_t.
   * @param payload
   *        The payload value copied into `*this` and whenever a Component itself is copied (or moved).
   */
  template<typename Payload>
  Component(Payload payload);

  /**
   * Copies the source Component into `*this`.  This involves a single payload copy (and not even that if
   * `src.empty()`).
   *
   * @param src
   *        Object to copy.
   */
  Component(const Component& src);

  /**
   * Constructs `*this` equal to `src_moved`.  In this implementation it is equivalent to the copy constructor.
   *
   * @param src_moved
   *        Object to move.
   */
  Component(Component&& src_moved);

  // Methods.

  /**
   * Overwrites `*this` with a copy of `src`.
   * @param src
   *        Object to copy.
   * @return `*this`.
   */
  Component& operator=(const Component& src);

  /**
   * Equivalent to `operator=(Component<Payload>(new_payload))` modulo possibly minor perf differences.
   *
   * @param new_payload
   *        See non-default constructor.
   * @return `*this`.
   */
  template<typename Payload>
  Component& operator=(Payload new_payload);

  /**
   * Makes `*this` equal to `src_moved`.  In this implementation it is equivalent to copy assignment.
   * @param src_moved
   *        Object to move.
   * @return `*this`.
   */
  Component& operator=(Component&& src_moved);

  /**
   * Returns `true` if `*this` is as if default-constructed (a null Component); `false` if as if
   * constructed via the 1-arg constructor (a non-null Component).
   *
   * @return See above.
   */
  bool empty() const;

  /**
   * Returns reference to immutable payload stored in `*this`; undefined behavior if `empty() == true`.
   *
   * @tparam Payload
   *         See one-arg ctor doc header.
   * @return See above.
   */
  template<typename Payload>
  Payload payload() const;

  /**
   * Returns `typeid(Payload)`, where `Payload` was the template param used when calling the
   * originating one-arg constructor or equivalent; undefined behavior if `empty() == true`.
   *
   * flow::log user that relies fully on an out-of-the-box Logger, or on a custom Logger that
   * nevertheless fully uses the log::Config facility, is unlikely to need this information.
   * (It is used internally by log::Config.)  However, a custom Logger that decided to use
   * an alternative Component mechanism (as opposed to how log::Config and reliant `Logger`s do)
   * can use this payload_type() method to distinguish between potential source `enum`s that were
   * passed to the Logger at each given log call site.
   *
   * For example, consider that Flow itself, for its own logging, uses the flow::Flow_log_component
   * `enum` at all log call sites.  Imagine you, the user, have decided to generate your own
   * flow::log messages and always use your own `enum class cool_project::Cool_log_component` at your
   * own logging call sites.  Then, a typical component specification will look like this respectively:
   *
   *   ~~~
   *   class flow::net_flow::Node : public Log_context
   *   {
   *     Node(...) :
   *       Log_context(..., Flow_log_component::S_NET_FLOW) // This class will usually use the NET_FLOW component.
   *       ...
   *
   *   class cool_project::Cool_class_about_widgets : public Log_context
   *   {
   *     Cool_class_about_widgets(...) :
   *       Log_context(..., Cool_log_component::S_WIDGETRY) // Ditto for your own code.  Use your own enum.
   *   ~~~
   *
   * Now, `this->get_log_component().payload_type()` will equal that of `Flow_log_component` and `Cool_log_component`,
   * respectively, within those 2 classes.  In particular, a custom Logger implementation can use `payload_type()` --
   * and in particular the derived `payload_type_index()` -- to interpret the `Component`s of values from the 2
   * entirely disparate `enum`s in different ways.  More in particular, the out-of-the-box `Logger`s use `Config`
   * to do all of that without your having to worry about it (but your own Logger would potentially have to worry
   * about it particularly if not using log::Config... though we suggest that you should, barring excellent
   * design counter-reasons).
   *
   * @return See above.
   */
  const std::type_info& payload_type() const;

  /**
   * Convenience accessor that returns `std::type_index(payload_type())`, which can be used most excellently to store
   * things in associative containers (like `std::map`) keyed by disparate Component payload `enum` types.  (E.g., such
   * a map might have one entry for Flow's own flow::Flow_log_component; one for the example
   * `cool_project::Cool_log_component` enumeration mentioned in the payload_type() doc header; and so on for any
   * logging modules in your process.  Again, log::Config will do that for you, if you choose to use it in your
   * custom Logger.)
   *
   * @internal
   * ### Impl/perf discussion ###
   * I (ygoldfel) considered *not* storing a `type_info` -- and hence not even providing payload_type() API -- but
   * directly storing `type_index` *only* (and hence *only* providing payload_type_index()).  The plus would have been
   * eliminating computation in `type_index()` construction, in the payload_type_index() accessor which is executed very
   * frequently.  The minus would have been the lack of `type_info` access, so for instance the name of the `enum` type
   * could not be printed.
   *
   * I almost did this; but since `type_index()` ctor is `inline`d, and all possible `type_info`s are actually
   * generated at compile time before the program proper executes, the accessor likely reduces to a constant anyway
   * in optimized code.  That said, if profiler results contradict this expectation, we can perform this optimization
   * anyway.  However -- that would be a breaking change once a Flow user uses payload_type() publicly.
   * @endinternal
   *
   * @return See above.
   */
  std::type_index payload_type_index() const;

  /**
   * Returns the numeric value of the `enum` payload stored by this Component, originating in the one-arg constructor;
   * undefined behavior if `empty() == true`.
   *
   * @return See above.  Specifically that's: `static_cast<enum_raw_t>(payload)`, where `payload` was passed to
   *         originating 1-arg ctor.
   */
  enum_raw_t payload_enum_raw_value() const;

private:
  // Data.

  /**
   * The `typeid()` of the `Payload` passed to the 1-arg constructor; if 0-arg ctor was used (empty() is `true`) then
   * it is null.
   *
   * ### Rationale ###
   * Why is it a pointer and not something else?  Answer:
   * It cannot be a direct member: `type_info` is not copyable.  It cannot be a non-`const` reference for the
   * same reason.  It cannot be a `const` reference, because Component is mutable via assignment.
   * (In any case, all possible `type_info` objects are known before program start and are 1-1 with all possible
   * types; hence the desire to store a "copy" is wrong-headed perf-wise or otherwise; there is just no reason for it.)
   */
  std::type_info const * m_payload_type_or_null;

  /**
   * The internally stored integer representation of the `enum` value passed to the 1-arg constructor; meaningless
   * and ignored if 0-arg ctor was used (empty() is `true`).
   */
  enum_raw_t m_payload_enum_raw_value;
}; // class Component

/**
 * Simple data store containing all of the information generated at every logging call site by flow::log, except
 * the message itself, which is passed to Logger::do_log() assuming Logger::should_log() had returned `true`.
 * User only need to worry about this when dealing with the internals of a Logger implementation.  Copying is to be
 * avoided, as there are some non-trivial data stored here; though it is not too bad.
 *
 * @todo Add support in Msg_metadata for a message ID which could more straightforwardly help the human log reader
 * to map a log line to the originating log call site in source code.  One approach, then, might be to output
 * that message ID when available; else output #m_msg_src_file, #m_msg_src_line, #m_msg_src_function; or maybe
 * both.
 */
struct Msg_metadata
{
  // Types.

  /**
   * The time-stamp type with a precision exceeding (but ideally exactly equal to) the fine-grainedness of the
   * time-of-day info obtainable from the system.
   *
   * @internal
   * ### Rationale ###
   * In the past this was a `duration` and in fact manually set to `microseconds`, because we were forced to deal
   * with boost.date_time to get the required time components with enough precision in such a way as to be
   * possible to output with microsecond precision.  Finally, though, boost.chrono gave it to us for free with its
   * `system_clock::time_point`-supporting `ostream<<` implementation; so we can drop all these complexities and just
   * store a damned `system_clock::time_point`.
   */
  using Time_stamp = boost::chrono::system_clock::time_point;

  // Data.

  /// Component of message, as of this writing coming from either Log_context constructor or FLOW_LOG_SET_CONTEXT().
  Component m_msg_component;

  /**
   * Severity of message, typically determined by choice of macro (e.g., FLOW_LOG_WARNING() vs. FLOW_LOG_INFO()) at the
   * call site; though it can also be supplied manually via `FLOW_LOG_WITH[OUT]_CHECKING()` macro arg.
   */
  Sev m_msg_sev;

  /**
   * Pointer/length into static-storage string that would have come from built-in `__FILE__` macro which is
   * auto-invoked by all `FLOW_LOG_*()` logging call sites.  Formally this should be the pointer/length representing
   * the substring of `__FILE__` that you wish to be logged in its entirely, no more and no less.  For example
   * the pointer might be to the first character, the lengh equal to `strlen()`; or more practically it might be
   * one past the right-most dir separator character (and the length decremented accordingly).
   *
   * To be explicit: The underlying memory must never be deallocated, in that it should never have been allocated on the
   * heap but in static storage.
   *
   * ### Perf notes ###
   * We store a util::String_view, not a `const char*`, which means not only a pointer but a length is stored here
   * internally.  That's fine (we discuss trade-offs just below); it should barely affect the perf of copying
   * Msg_metadata.  However the length must be initialized.  To initialize it in the most optimized way, recognize
   * that it will come from `__FILE__` which is really a string literal substituted by preprocessor; therefore
   * the length can be obtained at compile time via `sizeof()`.  Hence use the 2-arg `String_view` ctor which takes
   * the pointer *and* the length instead of figuring the latter out via `strlen()` which is linear-time.
   * Update: The util::String_view in use as of this writing declares the 1-arg ctor as `constexpr` which indicates
   * it might be able to do the `strlen()` (or equivalent) at compile-time.  Nevertheless, this feels (and I say this
   * with some related experience) like something that may or may not actually be implemented by a given compiler.
   * So it's probably safer (in terms of portability) to still follow the 2-arg ctor advice above.
   *
   * If it is desired (as suggested in an example above) to represent a mere substring of that, then as long as
   * the computation of the appropriate first character past index 0 satisfies `constexpr` requirements (meaning, it
   * will be computed at compile time, not runtime) -- as does the according decrement of the length -- then you're
   * still fine.  If it is done at runtime, then that's a hit to perf, so avoid it if at all possible.
   *
   * If indeed you do want ultimately to output a substring of `__FILE__`, then in order to guarantee or at least
   * increase the chance of compile-time computation of that substring you should in fact do it as early as possible
   * at the log call site as opposed to later/nearer the time of final output to device/whatever by Logger.  In other
   * words in that case do try to set this value to the substring from the start; don't leave it to the Logger.
   *
   * ### Perf rationale: Why not use `const char*` instead? ###
   * (This is arguably very nitpicky anyway, but these objects are generated and passed around at every single log call
   * site, so we should try to save every little bit.)  Let's stipulate that the cost of storing (RAM; and copying)
   * the length is negligible.  The next concern is initializing the length; the above shows that's free in practice.
   * Finally, when output to the final device/whatever occurs within Logger impl, there are two possibilities of how
   * it might realistically act.  One, it might search for the NUL char anyway -- in which case we've changed nothing
   * perf-wise -- as it copies for output purposes.  Two, it might require the length of the string and hence use
   * `String_view::size()` and then perform the copy; in that case we have saved a linear search.  (For example, if
   * the Logger is printing to an `ostream`, then there exists an `operator<<(ostream, String_view)` that'd be
   * automatically used and would of course use the saved length properly.)  So, it's a win or at worst a tie.
   */
  util::String_view m_msg_src_file;

  /**
   * Copy of integer that would have come from built-in `__LINE__` macro which is auto-invoked by all `FLOW_LOG_*()`
   * logging call sites.
   */
  unsigned int m_msg_src_line;

  /// Analogous to #m_msg_src_file but coming from `__FUNCTION__`, not `__FILE__`.  See #m_msg_src_file perf notes.
  util::String_view m_msg_src_function;

  /// Time stamp from as close as possible to entry into the log call site (usually `FLOW_LOG_WARNING()` or similar).
  Time_stamp m_called_when;

  /**
   * Thread nickname, as for Logger::this_thread_set_logged_nickname(), of the thread from which the
   * log call was invoked; or an empty string if the thread had no such nickname set at the time.
   *
   * @see #m_call_thread_id
   *
   * ### Perf note ###
   * Setting this involves an `std::string` copy; the cost of this is worth considering given that this is done
   * for every single log call site, if the nickname is indeed set.  See performance note in doc header of
   * Logger::this_thread_set_logged_nickname() for the recommendation and details.  (Long story short, if you keep
   * it at N `char`s or fewer, the cost of a non-empty #m_call_thread_nickname becomes equal to that of an
   * empty one.  N might be 15 in gcc 5.)
   */
  std::string m_call_thread_nickname;

  /**
   * Thread ID of the thread from which the log call was invoked; or a default-constructed (no-thread) such
   * thread ID if the thread has a nickname, meaning `!m_call_thread_nickname.empty()`.  The working assumption is
   * that (1) both members are met for direct log output only and no other logic; and (2) the nickname is preferable
   * when set, the thread ID being the fallback.  (If this sounds meh, consider that it's entirely reasonable to make
   * the nickname contain some nice info *and* the original thread ID as well in string form.  However, might
   * the length -- the Performance Note in #m_call_thread_nickname doc header.)
   */
  util::Thread_id m_call_thread_id;
}; // struct Msg_metadata

/**
 * Interface that the user should implement, passing the implementing Logger into logging classes
 * (Flow's own classes like net_flow::Node; and user's own logging classes) at construction (plus free/`static`
 * logging functions).  The class (or function) will then implicitly
 * use that Logger in the logging apparatus (such as `FLOW_LOG_...()` macros) to log messages
 * into the user's preferred logging output(s).  One can think of the class implementing this interface
 * as the glue between Flow logging and either the user's lower-level logging system or some output device
 * (like console, files) directly.
 *
 * The reason should_log() and do_log() are decoupled like this is for a fast
 * implementation of the `FLOW_LOG_...()` macros (see FLOW_LOG_WITH_CHECKING() in particular).
 *
 * There is also a small set of app-wide (`static`) utilities, including the ability to nickname any
 * given thread, which causes that nickname (instead of the thread ID) to be logged in subsequent log
 * messages.
 *
 * ### Stream continuity (and threads) ###
 * As slightly advanced `ostream` users know, an `ostream` carries state that can be set via explicit method
 * calls such as `ostream::setf()` and via "printing" manipulators such as `std::hex` to a given `ostream`.
 * Regular values printed to `ostream` will come out differently depending on the current state (e.g., `<< 14` may
 * show up as a decimal or a hex integer depending on preceding formatters).  If you use these state-affecting features,
 * be aware of the following semantics:
 *
 * - The system works as if there is exactly one `ostream` for every distinct combination (`logger`, T), where:
 *   - `logger` is a distinct Logger object, as identified by the address in its `Logger::this` pointer.
 *   - T is a distinct thread such that the API user has previously, at least once, invoked a logging API that works
 *     with Logger `ostream`s -- from that thread.  (The APIs that qualify are essentially various APIs that actually
 *     log messages or attempt to set formatting for subsequent messages or are used to subsequently do either.)
 * - One can think of a given (`logger`, T) combo's `ostream` as being created on-demand the first time a given
 *   Logger `logger` attempts to do something logging-related from a given thread T.  Subsequently, with that combo
 *   used again and again, the stream is internally looked up with high performance.  The on-demand creation is slower
 *   but relatively rare given the low expected count of threads and `Logger`s.
 * - Any stream state change carried by a user API call for a given distinct (as defined above) stream S:
 *   - *will* persistently affect the subsequent output of any subsequent log message payload on stream S,
 *     including the same log message following the given state change, and including any subsequent log messages;
 *   - *will not* affect any stream other than S;
 *   - *will not* affect any intra-message prefixes or suffixes (such as originating file, function, line that may
 *     be inserted before every log message automatically) added by the Logger API implementation.
 * - There are essentially two ways to affect state of stream S.  Either one must be invoked from the thread T
 *   pertaining to stream S, and the Logger object involved must also be the one pertaining to S.
 *   - The state change may be part of the sequence of `operator<<()` operands passed to a logging API.
 *     However, note that for it to take effect, it must not be filtered out by a Sev filter check (basically,
 *     the containing log message must actually end up in the log, not filtered away).
 *     - In this case, the state change must be via a manipulator formatter like `std::setfill`, `std::hex`,
 *       `boost::chrono::seconds`, etc.
 *   - The state change may be a direct call invoked on a given `ostream S`, where to get access to `S` one would
 *     call Logger::this_thread_ostream() to receive the pointer `&S', for the current thread and
 *     the Logger on which the method is executed.  Once you have `S`, you may call state manipulators such as
 *     `S.setf();` or, again, manipulators via `S << std::hex;` and similar.
 *     - Using this_thread_ostream() for any purpose other than to change stream state (i.e., to output actual
 *       characters, like `S << "Hello, world!";`) will result in undefined behavior.  Do not.  Use logging APIs for
 *       that.
 * - Even if the Logger implementation uses `ostream` for ultimate output of characters (and it absolutely does not
 *   have to do any such thing), that `ostream` is totally orthogonal and irrelevant to the one being discussed here.
 *   E.g., Simple_ostream_logger may print to `std::cout`, but the formatters we are discussing affect a totally
 *   different, internal stream, not that `std::cout`.  In fact, the internal stream is in memory, not
 *   file/console/whatever.  I point this out only to avoid confusion.
 *
 * ### Basic suggested strategy for implementing a Logger ###
 * The simplicity of the Logger interface, and the lack of mandatory rigidity in how one might configure it
 * (particularly w/r/t per-`Component` verbosity and output format), assures the sky is the limit for how one
 * implements their own custom Logger.  However, in the absence of great reasons not to, we suggest one follows
 * the lead of out-of-the-box existing `Logger`s in Flow, which adds a degree of rigidity as to the implementation
 * but also seems to provide all the features seemingly commonly desired in practice.  Namely, like
 * (say) Simple_ostream_logger, Buffer_logger, and Async_file_logger, do this:
 *   - Take a Config pointer at constructor and save it (do not copy the Config).  (There are thread safety
 *     implications.)
 *   - Internally use some kind of `ostream`-subclass member to a target device, if at all possible.
 *     (boost.asio will let you even write to network this way; but at least console output, file output, and
 *     memory string output are 100% practical via `ostream`s.  Existing `Logger`s provide examples.)
 *   - Internally use an Ostream_log_msg_writer to write to said `ostream`, the formatting thereof being configurable
 *     in a uniform way via the saved Config.
 *   - Forward should_log() logic to the saved Config (Config::output_whether_should_log()), so that verbosity is
 *     flexibly but uniformly set via Config.
 *   - It is up to the user, now, to set up the Config appropriately when passing it to your `Logger` subclass
 *     constructor.  The user would simply follow the documentation for Config, and you need neither re-implement
 *     nor re-document configurability of your Logger.
 *
 * Reminder: Config and Ostream_log_msg_writer are optional to use, and one can use one, both, or neither (though
 * the latter itself does expect one of the former; of course you can just load up your own new Config, in case
 * you don't want to use a user Config taken via your constructor API).  However, unless there's something insufficient
 * about them, various benefits abound in using both.  Furthermore, if something is lacking, consider extending that
 * system to the benefit of all/most other `Logger`s as opposed to throwing it away and re-implementing something new
 * for just your new Logger.
 *
 * Having decided on that stuff, you also need to decide whether you will write to the output device/whatever
 * synchronously or asynchronously.  To wit, the thread safety discussion:
 *
 * ### Thread safety ###
 * The degree of thread safety for either of the 2 main operations is completely up to the subclass implementer.
 * Informally, we suggest here that you think about this topic carefully.  In particular, without locking,
 * do_log() may run concurrently with itself from multiple threads; depending on the medium to which
 * it is writing, this may result in corruption or ugly output or turn out fine, depending on how you define
 * "fine."  Whether this is a concern or not is up to you.  Note, however, that in practice as of this writing
 * *at least* one Flow module (flow::net_flow) will definitely potentially execute should_log() and do_log()
 * concurrently with themselves from multiple threads on the same Logger.  In general that should be expected in all but
 * the simplest single-threaded apps.
 *
 * Implementation suggestions for Logger subclasses with respect to thread safety: There are 2 likeliest patterns one
 * can use.
 *   -# One can use a mutex lock around actual writing to the target device.  There's nothing inherently un-performant
 *      about this, in an of itself, and the implementation is incredibly simple.  For example see
 *      Simple_ostream_logger.  However, there *is* a significant performance danger if the device-writing itself can
 *      be both synchronous and slow.  In particular, a file write (when flushing any buffer such as the internal
 *      one in `FILE*` or `ofstream`), which is likely after each message, is usually synchronous and can be
 *      sporadically slow (consider having to spin up a sleeping hard drive for instance).  That would not only block
 *      the log-call-site thread but also any competing logging threads at that time.  That is probably fine in many
 *      non-production scenarios, but in a production heavy-duty server it's not OK.
 *      - An informal suggestion: It is fine for console output, probably (standard out, standard err).  Otherwise,
 *        particularly with files and synchronous networking, don't.  Use the following instead:
 *   -# One can avoid any such lock; instead the log-call-site thread can save the stuff to log (including the message
 *      and Msg_metadata passed to do_log()) and pass it to a dedicated thread (or pool thereof, etc.) in charge of
 *      asynchronously queueing up stuff to write to device and actually writing it in the order received at some
 *      later time (though typically fairly soon and definitely ASAP in most cases).  This is a bit more complex, but
 *      with flow::async it's really pretty easy still.  See the heavy-duty Async_file_logger for example.
 *
 * Both general patterns are formally supported.  To use pattern 1, have your Logger implementation's
 * logs_asynchronously() return `false`.  To use pattern 2, have it use `true`.  The precise implications are
 * documented in doc header of do_log().
 *
 * ### Order of output requirements ###
 * We assume for this discussion that the notion of order of final output (to device/file/network/whatever) exists,
 * and that the Logger implementation indeed safely outputs message M1 entirely before M2, or vice versa, for every
 * pair of messages (in this context by message we mean message+metadata pair) ever passed to do_log().  With that
 * assumption in effect (IF indeed it is), the present text *requires* that the following is guaranteed:
 *   - In a given thread T, for a given Logger L, if `L.do_log(M1);` is called before `L.do_log(M2)`, and both
 *     are successfully output by L, then the final output order must have M1 precede M2, not vice versa.
 *   - If, instead, M1 is logged in thread T1, while M2 is logged in thread T2:
 *     - If the 2 do_log() calls are chronologically disjoint, then again the final output must also have
 *       M1 precede M2 if M1 went first; and vice versa.
 *     - If the 2 do_log() calls chronologically overlap, then either final output order is acceptable.
 *       (Ideally, it *should* reflect the order of entry to do_log() in the 2 threads, but it's formally optional.
 *       Informally this typically will happen anyway under most basic algorithms imaginable.)
 *
 * Note that this is only tangentially related to any time stamps one might see in the final output.
 *
 * See also notes on log output order in FLOW_LOG_WARNING() doc header (which applies to all log call sites).
 * The behavior described there is a function of the underlying Logger following the above formal requirements for
 * thread-safe Logger implementations.
 */
class Logger :
  public util::Null_interface,
  public util::Unique_id_holder,
  private boost::noncopyable // Though it's an interface, this just seems like a prudent rule.  @todo Reconsider?
{
public:
  // Methods.

  /**
   * Given attributes of a hypothetical message that would be logged, return `true` if that message
   * should be logged and `false` otherwise (e.g., if the verbosity of the message is above the
   * current configured verbosity threshold for the Component specified).
   *
   * The convenience macros `FLOW_LOG_...()` combine this with do_log() to make it so that
   * a message is only built if `should_log() == true`, and (if so) builds and logs it; otherwise
   * almost no computing or storage resources are spent, and the message is neither built nor logged logged.
   * The verb "to build message" here more formally means "to execute the `ostream<<` fragment passed to macro by
   * writing characters to some internally maintained `ostream` accordingly."
   *
   * @param sev
   *        Severity of the message.
   * @param component
   *        Component of the message.  Reminder: `component.empty() == true` is allowed; which isn't to say it will
   *        or won't result in this method returning `true`, but that it will return and not act in undefined fashion.
   * @return `true` if it should be logged; `false` if it should not.
   */
  virtual bool should_log(Sev sev, const Component& component) const = 0;

  /**
   * Must return `true` if do_log() at least sometimes logs the given message and metadata (e.g., time stamp) after
   * do_log() returns; `false` if this never occurs (i.e., it logs synchronously, always).  do_log() doc header
   * formally describes the implications of this.
   *
   * This must always return the same value, for a given `*this`.
   *
   * This method is intended for internal use by the flow::log system; informally it is not expected the user
   * will call it.  Technically there is no harm in doing so.  (It would have been `private` but cannot due to certain
   * C++ limitations, and certain contrived ways to do it are just not worth the trouble.)
   *
   * @internal
   * The "certain C++ limitations" are that FLOW_LOG_WITHOUT_CHECKING() must access it, but it is a macro and cannot
   * be involved in `friend`ship.  A contrived way to resolve it would be to make some `detail/` helper free function
   * the `friend`, which would call this method, and invoke that free function from the macro.  All that just to move a
   * harmless thing into `private` -- I think not worth it, but an argument could be made.
   * @endinternal
   *
   * @see do_log().
   * @return See above.
   */
  virtual bool logs_asynchronously() const = 0;

  /**
   * Given a message and its severity, logs that message and possibly severity WITHOUT checking whether it should be
   * logged (i.e., without performing logic that should_log() performs).  The logging is guaranteed to be synchronous
   * if `!logs_asynchronously()`; but may be asynchronous otherwise (more on this below).
   *
   * The convenience macros `FLOW_LOG_...()` combine this with should_log() to make it so that a
   * message is only constructed if `should_log() == true`, and if so, constructs and logs it; otherwise
   * almost no computing or storage resources are spent, and the message is not logged.
   *
   * Expectations of what should or should not be included in `msg` are of some importance.
   * They are as follows.  To summarize, `msg` should include the message (as specified typically as the
   * `ARG_stream_fragment` arg to FLOW_LOG_WARNING() and buddies); and `*msg_metadata` should include everything
   * else.  This design provides maximum flexibility to the Logger::do_log() implementation to structure the final
   * output's contents (in the log file, or console, or wherever) as it sees fit, cosmetically and size-wise.
   *
   * Note on trailing newline(s): `msg` must include any trailing newline(s) that are *required* to be output.
   * By convention, do_log() itself will print a message terminator (often in fact a newline) after each message, if
   * applicable.  Hence typically there is no trailing newline at the end of most `msg`s, and one would include N of
   * them if and only if one intentionally desires N trailing blank lines (possible but atypical).
   *
   * ### Precise meaning of logs_asynchronously() ###
   * Let ASYNC = the value logs_asynchronously() returns (note for a given `*this` it must always be the same value by
   * that API's contract).  Then do_log() must act as follows:
   *   - If ASYNC is `false`: Do not make a copy of `msg`; output it synchronously.  Do not make a copy of `*metadata`.
   *     Do not `delete metadata`.  Output any of its contents synchronously.
   *   - If ASYNC is `true`: Optionally make a copy of `msg`, unless you are able to output it synchronously, in which
   *     case there is no need.  Do not make a copy of `*metadata`.  You *must* `delete metadata` at some point;
   *     failing to do so *will* leak it.  (Note: We are intentionally avoiding using `shared_ptr` or even `unique_ptr`,
   *     for the perf savings, since logging is ubiquitous.)  Output `*metadata` contents either synchronously or
   *     asynchronously.
   *
   * The only formal requirement, however, is simply: You must `delete metadata;` at some future point <=iff=>
   * ASYNC is `true`.  The other requirements just above are informal but of no less import.
   *
   * @see Msg_metadata which includes stuff not to include in `msg`.
   *
   * @param metadata
   *        All information to potentially log in addition to `msg`.
   * @param msg
   *        The message.  See details above.  Short version: exclude anything from `metadata`; exclude any ever-present
   *        terminating newline.
   */
  virtual void do_log(Msg_metadata* metadata, util::String_view msg) = 0;

  /**
   * Sets or unsets the current thread's logging-worthy string name; optionally sets the OS thread name (such as
   * visible in `top` output).  The logging-worthy name is always set or unset; the OS name is modified only if
   * `also_set_os_name == true` arg is set.  `thread_nickname` can thus be set to something more descriptive than
   * any default, such as: "native_main" or "worker_port_2231."
   *
   *   - The logging-worthy thread string name is accessed as follows:
   *     - `FLOW_LOG_*()` macros pass it automatically to the appropriate log::Logger.  All out-of-the-box `Logger`s
   *       will then log either the name set here (if not blank) or the thread ID (if blank).  (Custom `Logger`s
   *       can ignore these data and not log them; but presumably most will at least optionally log them.)
   *     - It can also be obtained directly via this_thread_logged_name_os_manip(), set_thread_info(), or
   *       set_thread_info_in_msg_metadata().
   *   - The OS thread name can be accessed in various ways; including in Linux:
   *     - `ps H -C $cmd -o pid\ tid\ cmd\ comm  # The comm column will be the thread name; set $proc = process name.`
   *     - `top -H` (thread mode -- or just `top` and press H key for the same effect).
   *
   * More precisely, there are 3 states for each thread: before this_thread_set_logged_nickname() is called;
   * after it is called with blank name; and after it's called non-blank name.  The semantics are as follows:
   *   - Logging-worthy thread string name:
   *     - Initial: As-if this_thread_set_logged_nickname() was already called with blank name (next bullet).
   *     - Blank: The thread string name becomes conceptually null; and the thread ID shall be used instead.
   *     - Non-blank: The thread string name becomes equal to `thread_nickname`.
   *   - OS thread name:
   *     - Initial: `ps`, `top`, etc. will show the thread name as equal to the process name.
   *     - Blank: `ps`, `top`, etc. will show the thread name as the thread ID (truncated to N characters, though
   *       this is unlikely to be exceeded by real thread IDs).
   *     - Non-blank: `ps`, `top`, etc. will show the thread name as equal to `thread_nickname` (truncated to N
   *       characters).
   *     - Note: Because "Initial" and "Blank" are necessarily not the same, it is recommended to call
   *       this_thread_set_logged_nickname() around thread creation time even if `thread_nickname` is blank.
   *       Then `ps`, `top`, etc. output will still be useful and possible to cross-reference with log output, say.
   *     - Note: Truncation will be reverse, meaning -- if necessary -- *leading* characters will be eliminated.
   *       This, in practice, tends to at least help disambiguate in case of truncation.
   *     - Note: N is documented in `man pthread_setname_np` as 15 not counting the NUL terminator.
   *       Therefore ideally keep the `thread_nickname.size()` to at most N.
   *
   * ### Performance ###
   * Subsequently obtaining the nickname involves a linear string copy;
   * the cost of this is worth considering given that this is potentially
   * done for every single log call site, if the nickname is indeed set.  However, most `string` implementations
   * provide an optimization that uses a `union` (or equivalent) technique to store short strings in the same place
   * as the data members (pointer, size, capacity) required for long strings; meaning such short `string`s cost no
   * more to copy than an *empty* one does.  In gcc 5, this is 15 bytes or `char`s, but implementations vary.
   *
   * Therefore, it is *actively encouraged* that you keep the length of `thread_nickname` low.  "Low" depends on the
   * compiler, but keeping it somewhere at or under 15 characters is likely good.  If you do so, the effective cost
   * (at a log call site) will be the same as if `thread_nickname.empty()`, so one could not do better.
   * Note, also, that the OS-name (in Linux) max length happens to also be 15 (see N discussion above), so there is
   * convenient synergy there.
   *
   * ### Thready safety ###
   * This call is safe w/r/t concurrent execution with itself and this_thread_logged_name_os_manip() in other
   * thread(s) for a given `*this`.  It is thread-local in nature.
   *
   * ### Naming rationale ###
   * this_thread_set_logged_nickname() is an incomplete name, in that it (optionally) also affects the OS thread name.
   * The function was not renamed for compatibility reasons, as the technically incomplete name is in my (ygoldfel)
   * opinion still acceptably descriptive.
   *
   * @param thread_nickname
   *        New nickname of thread; or "" to request the thread to not be nicknamed (thread ID will be used).
   *        In the "" case, no particular thread ID format should ever be assumed; it may be OS-dependent;
   *        but it can be informally assumed to be useful for thread identification purposes (probably unique per thread
   *        and somewhat readable, etc.).  The non-blank value is copied and saved.
   * @param logger_ptr
   *        If non-null, this `Logger` will be used to log an INFO-severity message indicating the
   *        thread ID and new nickname (conceptually: [INFO] T...nickname...: Thread ...ID... has new nickname.).
   *        If null, nothing is logged.
   * @param also_set_os_name
   *        If and only `true`, `thread_nickname` (whether blank or not) also affects the current OS thread name.
   *        Otherwise it is not affected.
   *
   * @todo this_thread_set_logged_nickname() could take multiple `Logger`s, since it is an app-wide function
   * and potentially would want to be reflected in multiple loggers.  It could take a pair of iterators
   * or a container (in both cases, of template argument type).  Arguably, though, this is overkill; as (1) most code
   * paths deal with only one `get_logger()`-owning object each; (2) naming of a thread is obvious enough in subsequent
   * logs (hence this message only calls attention to that operation plus notes the thread ID before the nickname
   * assignment, not necessarily the most critical of information).  Certainly this zero-to-one-`Logger` version must
   * continue to be available for syntactic-sugary convenience, even if the to-do is performed.
   */
  static void this_thread_set_logged_nickname(util::String_view thread_nickname = util::String_view(),
                                              Logger* logger_ptr = 0,
                                              bool also_set_os_name = true);

  /**
   * `ostream` manipulator function that, if output via `operator<<` to an `ostream`, will cause the current
   * thread's logging-worthy string name to be output to that stream.  See this_thread_set_logged_nickname()
   * for details of what this string will actually be.
   *
   * Recall that an `ostream` manipulator is invoked in the style of `endl` and `flush`; for example: `cout << endl;`.
   * It is atypical to call it directly as opposed to via the overloaded "shift" operator.
   *
   * Note that typically this is not invoked directly by the user but rather used in the `FLOW_LOG_...()` macros'
   * implementation guts which is the original use case and hence reason for its existence.
   * However, there's no rule against direct uses, and it could prove useful at some point.  Any use beyond logging or
   * debugging is not recommended however (in particular, do not use to make any algorithmic decisions).
   *
   * This call is safe w/r/t concurrent execution with itself and this_thread_set_logged_nickname() in other
   * thread(s).
   *
   * @param os
   *        Stream to which to write thread's name.
   * @return `os`.
   */
  static std::ostream& this_thread_logged_name_os_manip(std::ostream& os);

  /**
   * Loads `msg_metadata->m_call_thread_nickname` (if set) or else `msg_metadata->m_call_thread_id`, based
   * on whether/how this_thread_set_logged_nickname() was last called in the current thread.
   * The two members should be set to ther default-constructed values on entry to the present function.
   *
   * @todo It would be more consistent to rename set_thread_info_in_msg_metadata() to
   * this_thread_set_info_in_msg_metadata(), since it operates in thread-local fashion.
   * This was a naming convention oversight.
   *
   * @param msg_metadata
   *        Non-null pointer to structure to modify.  See above.
   */
  static void set_thread_info_in_msg_metadata(Msg_metadata* msg_metadata);

  /**
   * Same as set_thread_info_in_msg_metadata() but targets the given two variables as opposed to a
   * Msg_metadata.
   *
   * @todo It would be more consistent to rename set_thread_info() to
   * this_thread_set_info(), since it operates in thread-local fashion.
   * This was a naming convention oversight.
   *
   * @param call_thread_nickname
   *        Non-null pointer to value to modify.  See above.
   * @param call_thread_id
   *        Non-null pointer to value to modify.  See above.
   */
  static void set_thread_info(std::string* call_thread_nickname,
                              flow::util::Thread_id* call_thread_id);

  /**
   * Returns the stream dedicated to the executing thread and `this` Logger, so that the caller can apply
   * state-setting formatters to it.  If you write characters to it, or otherwise do anything othen than set
   * formatting state, or try to affect buffering behavior, behavior is undefined.  Usage example:
   *
   *   ~~~
   *   get_logger()->this_thread_ostream()->setf(std::fixed | std::right);
   *   *(get_logger()->this_thread_ostream()) << std::setw(2);
   *   // *get_logger()'s subsequent messages (such as the following) from the current thread will use above formatting.
   *   FLOW_LOG_WARNING("Let's print a number with some formatting: " << 0.5);
   *   ~~~
   *
   * Note that you could just as well apply the intended formatters via regular log statements.
   * However, there are disadvantages to that approach -- but they do not always apply.  The disadvantages are listed
   * below; but the short version is you should typically use the present method if and only if you are attempting to
   * affect subsequent logging at large, not a particular fragment of a particular message.
   *
   * Formally, the disadvantages of affecting formatting state of the underlying stream via log macros:
   * - If the log statement is ignored due to failing a filter check,
   *   then any formatters therein will also be ignored.  (However, you could use a macro that bypasses such a check.
   *   On the other hand, stylistically one would typically only do that after checking the severity manually
   *   for performance of combining several log statements with equal severities.  Using it *just* to apply formatters
   *   is stylistically dubious.)
   * - If you are trying to affect subsequent logging at large, you'd hypothetically use something like
   *   `FLOW_LOG_INFO_WITHOUT_CHECKING(...formatter... << ...formatter... << ...);`.  This is stylistically dubious,
   *   because the lack of characters being output means the severity (INFO in this case) is disregarded and is
   *   a dummy value.
   * - While one can pass many formatters as `<<` operator arguments, there are others than do not use that syntax.
   *   For example, `ostream::setf()` is a method *of* `std::ostream`.  Thus the log macros do not help.
   *
   * Recall that the stream involved is completely orthogonal to any underlying stream that may be ultimately output
   * to by Logger for the actual, ultimate output of characters.  E.g., if Logger happens to be Simple_ostream_logger
   * targeted at `std::cout`, the above snippet would in no way touch `std::cout` formatting.  In fact, Logger may
   * not even use streams for output; that is an orthogonal implementation detail.
   *
   * @return Pointer to stream; always the same value for a given thread and different among all distinct threads.
   */
  std::ostream* this_thread_ostream() const;

private:
  // Data.

  /// Thread-local storage for each thread's logged name (null pointer, which is default, means no nickname stored).
  static boost::thread_specific_ptr<std::string> s_this_thread_nickname_ptr;
}; // class Logger

/**
 * Convenience class that simply stores a Logger and/or Component passed into a constructor; and returns this
 * Logger and Component via get_logger() and get_log_component() public accessors.  It's extremely
 * useful (almost mandatory in conventional practice) for classes that want to log, as they can simply
 * derive from it (passing in the desired `Logger*` and Component payload (an `enum`
 * value) into the Log_context superclass constructor),
 * at which point the get_logger() and get_log_component() functions the `FLOW_LOG_...()` macros expect automatically
 * become available without any additional code having to be written in the logging class.  Here is how:
 *
 *   ~~~
 *   class I_like_to_have_fun_and_log_about_it :
 *     public flow::log::Log_context
 *   {
 *   public:
 *     I_like_to_have_fun_and_log_about_it() :
 *       // Glue FLOW_LOG_*() macros to the following simple logger and component FUN_HAVER.
 *       Log_context(&m_logger, My_cool_components::S_FUN_HAVER),
 *       // Initialize stdout logger that logs INFO-or-higher-severity messages.
 *       m_logger(true, std::cout, std::cout, flow::log::Sev::S_INFO),
 *       // ... other initializers and superclass constructors, if any ...
 *     {
 *       FLOW_LOG_INFO("I can log right from the constructor and throughout *this lifetime!");
 *       // ... other code ...
 *     }
 *
 *   private:
 *     void do_fun_stuff()
 *     {
 *       // This macro works, because Log_context superclass defines get_logger() which returns m_logger,
 *       // and component() returns My_cool_components::S_FUN_HAVER.
 *       // But we need not ever worry about such details.
 *       FLOW_LOG_INFO("I am about to do something cool and fun: " << 42 << "!");
 *       // ... more code ...
 *     }
 *
 *     // I will use a simple stdout/stderr logger for my logging. It's passed to Log_context in constructor.
 *     flow::log::Simple_ostream_logger m_logger;
 *   }; // class I_like_to_have_fun_and_log_about_it
 *   ~~~
 *
 * Note that the `operator=()` allows one to change the underlying Logger anytime after
 * construction (e.g., `existing_log_context = Log_context(&some_logger, Some_enum::S_SOME_COMPONENT);`).
 *
 * ### Implementation notes ###
 * The code could be shorter by getting rid of non-copy constuctor in favor of direct member initialization by user;
 * and by simply omitting the API for the auto-generated copy constructor and assignment.  However, in this case,
 * I wanted to clearly document the API; and since there are more than 1 constructors, it seemed better to explicitly
 * declare them all instead of replacing some with implicitly required direct initialization (again to make API more
 * clearly documented).
 *
 * ### Thread safety ###
 * The only operation of interest w/r/t threads is the aforementioned implicit assignment operator.  Thread safety is
 * the same as for any `struct` with no locking done therein.
 */
class Log_context
{
public:
  // Constructors/destructor.

  /**
   * Constructs Log_context by storing the given pointer to a Logger and a null Component.
   *
   * @param logger
   *        Pointer to store.  Rationale for providing the null default: To facilitate subclass `= default` no-arg
   *        ctors.
   */
  explicit Log_context(Logger* logger = 0);

  /**
   * Constructs Log_context by storing the given pointer to a Logger and a new Component storing the
   * specified generically typed payload (an `enum` value).  For more background on Component see its
   * doc header.
   *
   * @tparam Component_payload
   *         See Component constructor doc header: `Payload` template arg specifically.
   * @param logger
   *        Pointer to store.
   * @param component_payload
   *        See Component constructor doc header: `payload` arg specifically.
   */
  template<typename Component_payload>
  explicit Log_context(Logger* logger, Component_payload component_payload);

  /**
   * Copy constructor that stores equal `Logger*` and Component values as the source.
   *
   * This is `explicit`, even though an unintentional copy (e.g., in a `bind` sans `cref` or `ref`) would just
   * internally involve the copying a pointer (as of this writing).  The reason is that it's unlikely one wants
   * to blithely copy these objects or objects of a sub-type; most likely (at least in scenarios seen so far, as of
   * this writing) a `cref` or `ref` is in order instead.  (I am open to counter-examples and thus removing this
   * `explicit` keyword if convinced by one.)
   *
   * @param src
   *        Source object.
   */
  explicit Log_context(const Log_context& src);

  /**
   * Move constructor that makes this equal to `src`, while the latter becomes as-if default-constructed.
   *
   * @param src
   *        Source object.
   */
  Log_context(Log_context&& src);

  // Methods.

  /**
   * Assignment operator that behaves similarly to the copy constructor.
   *
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Log_context& operator=(const Log_context& src);

  /**
   * Move assignment operator that behaves similarly to the move constructor.
   *
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Log_context& operator=(Log_context&& src);

  /**
   * Swaps Logger pointers and Component objects held by `*this` and `other`.  No-op if `this == &other`.
   *
   * @param other
   *        Other object.
   */
  void swap(Log_context& other);

  /**
   * Returns the stored Logger pointer, particularly as many `FLOW_LOG_*()` macros expect.
   *
   * @note It's public at least so that FLOW_LOG_SET_CONTEXT() works in all reasonable contexts.
   *
   * @return See above.
   */
  Logger* get_logger() const;

  /**
   * Returns reference to the stored Component object, particularly as many `FLOW_LOG_*()` macros expect.
   *
   * @note It's public at least so that FLOW_LOG_SET_CONTEXT() works in all reasonable contexts.
   *
   * @return See above.
   */
  const Component& get_log_component() const;

private:
  // Data.

  /// The held Logger pointer.  Making the pointer itself non-`const` to allow `operator=()` to work.
  Logger* m_logger;
  /// The held Component object.  Making the object non-`const` to allow `operator=()` to work.
  Component m_component;
}; // class Log_context

// Free functions: in *_fwd.hpp.

// Template implementations.

// Template implementations.

template<typename Payload>
Component::Component(Payload payload)
{
  // Currently uninitialized.  Now:
  operator=(payload);
}

template<typename Payload>
Payload Component::payload() const
{
  // Zero processor cycles (an `enum` value *is* internally its integer value).
  return static_cast<Payload>(m_payload_enum_raw_value);
}

template<typename Payload>
Component& Component::operator=(Payload new_payload)
{
  static_assert(std::is_enum_v<Payload>, "Payload type must be an enum.");
  static_assert(std::is_same_v<typename std::underlying_type_t<Payload>, enum_raw_t>,
                "Payload enum underlying type must equal enum_raw_t.");

  // Might be optimized-out to storing, essentially, a constant at the call site of this assignment (or ctor).
  m_payload_type_or_null = &(typeid(Payload));
  // Zero processor cycles (an `enum` value *is* internally its integer value).
  m_payload_enum_raw_value = static_cast<enum_raw_t>(new_payload);

  return *this;
}

template<typename Component_payload>
Log_context::Log_context(Logger* logger, Component_payload component_payload) :
  m_logger(logger),
  m_component(component_payload)
{
  // Nothing.
}

} // namespace flow::log
