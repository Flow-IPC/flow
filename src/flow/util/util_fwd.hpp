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

#include "flow/util/detail/util_fwd.hpp"
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/thread/null_mutex.hpp>
#include <iostream>
#include <memory>

/**
 * Flow module containing miscellaneous general-use facilities that don't fit into any other Flow module.
 *
 * Each symbol therein is typically used by at least 1 other Flow module; but all public symbols (except ones
 * under a detail/ subdirectory) are intended for use by Flow user as well.
 *
 * @todo Since Flow gained its first users beyond the original author, some Flow-adjacent code has been written from
 * which Flow can benefit, including additions to flow::util module.  (Flow itself continued to be developed, but some
 * features were added elsewhere for expediency; this is a reminder to factor them out into Flow for the benefit of
 * all.)  Some features to migrate here might be: conversion between boost.chrono and std.chrono types; (add more
 * here).
 */
namespace flow::util
{
// Types.

// Find doc headers near the bodies of these compound types.

template<typename Key, typename Mapped, typename Hash = boost::hash<Key>, typename Pred = std::equal_to<Key>>
class Linked_hash_map;
template<typename Key, typename Hash = boost::hash<Key>, typename Pred = std::equal_to<Key>>
class Linked_hash_set;

class Null_interface;

class Rnd_gen_uniform_range_base;
template<typename range_t>
class Rnd_gen_uniform_range;
template<typename range_t>
class Rnd_gen_uniform_range_mt;

template<typename Value>
class Scoped_setter;

template<typename Target_ptr,
         typename Const_target_ptr
           = typename std::pointer_traits<Target_ptr>::template rebind<typename Target_ptr::element_type const>>
class Shared_ptr_alias_holder;

class String_ostream;

class Unique_id_holder;

/**
 * Short-hand for standard thread class.
 * We use/encourage use of boost.thread threads (and other boost.thread facilities) over std.thread counterparts --
 * simply because it tends to be more full-featured.  However, reminder: boost.thread IS (API-wise at least)
 * std.thread, plus more (advanced?) stuff.  Generally, boost.thread-using code can be converted to std.thread
 * with little pain, unless one of the "advanced?" features is used.
 */
using Thread = boost::thread;

/* (The @namespace and @brief thingies shouldn't be needed, but some Doxygen bug necessitated them.
 * See flow::util::bind_ns for explanation... same thing here.) */

/**
 * @namespace flow::util::this_thread
 * @brief Short-hand for standard this-thread namespace. Paired with util::Thread.
 */
namespace this_thread = boost::this_thread;

/// Short-hand for an OS-provided ID of a util::Thread.
using Thread_id = Thread::id;

/**
 * Short-hand for boost.asio event service, the central class of boost.asio.
 *
 * ### Naming ###
 * The reasons for the rename -- as opposed to simply calling it `Io_service` (historically it was indeed so named) --
 * are as follows.  To start, a later version of Boost (to which we eventually moved) realigned
 * boost.asio's API to match the feedback boost.asio received when submitted as a candidate for inclusion into
 * the official STL (sometime after C++17); in this reorg they renamed it from `io_service` to `io_context`.
 * That's not the actual reason but more of a symptom of similar reasoning to the following:
 *   - "Service" sounds like something that has its own thread(s) perhaps, which `io_service` actually doesn't;
 *     it *can* (and typically is) used to take over at least one thread, but that's not a part of its "essence," so
 *     to speak.  I've noticed that when boost.asio neophytes see "service" they intuitively draw incorrect assumptions
 *     about what it does, and then one must repeatedly dash those assumptions.
 *   - Furthermore (though apparently boost.asio guys didn't agree or didn't agree strongly enough) "I/O" implies
 *     an `io_service` is all about working with various input/output (especially networking).  While it's true
 *     that roughly 50% of the utility of boost.asio is its portable sockets/networking API, the other 50% is about
 *     its ability to flexibly execute user tasks in various threads; and, indeed, `io_service` *itself* is more about
 *     the latter 50% rather than the former.  Its two most basic and arguably oft-used features are simply
 *     `io_service::post()` (which executes the given task ASAP); and the `basic_waitable_timer` set of classes
 *     (which execute task at some specified point in the future).  Note these have nothing to do with networking or
 *     I/O of any kind.  The I/O comes from the "I/O object" classes/etc., such as `tcp::socket`, but those
 *     are *not* `io_service`; they merely work together with it to execute the user-specified success/failure handlers.
 *
 * So, neither "I/O" nor "service" is accurate.  To fix both, then, we used this rename reasoning:
 *   - "I/O" can be replaced by the notion of it executing "tasks."  boost.asio doesn't use or define the "task" term,
 *     sticking to "handler" or "functor" or "function," but we feel it's a reasonable term in this context.
 *     (Function, etc., doesn't cover all possibilities.  Handler is OK, but when it's not in response to any event --
 *     as with vanilla `post()` -- what is it handling?)
 *   - "Service" is trickier to replace.  It's a battle between "too generic" and "too specific and therefore long to
 *     type."  Plus there's the usual danger of accidentally overloading a term already used elsewhere nearby.
 *     Some ideas considered were: manager, engine, execution engine, queue, queues, context (which boost.asio chose),
 *     and various abbreviations thereof.  Ultimately it came down to context vs. engine, and I chose engine because
 *     context is just a little vague, or it suggests an arbitrary grouping of things -- whereas "engine" actually
 *     implies action: want to express this thing provides logical/algorithmic value as opposed to just bundling
 *     stuff together as "context" objects often do (e.g., "SSL context" in openssl).  Indeed `io_service` does provide
 *     immense algorithmic value.
 *     - Manager: generic but not bad; clashes with "Windows task manager" and such.
 *     - Execution engine: good but long.
 *     - Queue: early candidate, but at the very least it's not a *single* queue; if there are multiple threads
 *       `io_context::run()`ning then to the extent there are any queues there might be multiple.
 *     - Queues, qs: better... but having a thing called `Task_qs` -- of which there could be multiple (plural) --
 *       was clumsy.  (An early proof-of-concept used `Task_qs`; people were not fans.)
 */
using Task_engine = boost::asio::io_context;

/// Short-hand for boost.asio strand, an ancillary class that works with #Task_engine for advanced task scheduling.
using Strand = boost::asio::strand;

/**
 * boost.asio timer.  Can schedule a function to get called within a set amount of time or at
 * a certain specific time relative to some Epoch.
 *
 * @see schedule_task_from_now() (and friends) for a simpler task-scheduling API (which internally uses this
 *      `Timer).  The notes below still apply however.  Please read them even if you won't use `Timer` directly.
 *
 * Important: While one can schedule events using very precise units using #Fine_duration, the
 * timer may not actually have that resolution.  That is, I may schedule something to happen in 1
 * millisecond and then measure the time passed, using a high-res clock like #Fine_clock, and discover that
 * the wait was actually 15 milliseconds.  This is not a problem for most timers.  (In Flow itself, the most advanced
 * use of timers by far is in flow::net_flow.  Timers used in flow::net_flow -- delayed ACK timer, Drop Timer, simulated
 * latency timer -- typically are for `n x 50` milliseconds time periods or coarser.)  However that may not be the
 * case for all timers.  (In particular, flow::net_flow packet pacing may use finer time periods.)
 *
 * @note The flow::net_flow references in the present doc header speak of internal implementation details -- not
 *       of interest to the `net_flow` *user*.  I leave them in this public doc header for practical purposes, as
 *       examples needed to discuss some Inside-Baseball topics involved.
 *
 * So I looked into what boost.asio provides.  `deadline_timer` uses the system clock
 * (`universal_time()`) as the time reference, while `basic_waitable_timer<Fine_clock>` uses the
 * high-resolution clock (see #Fine_clock).  I tested both (using `wait()`s of various lengths
 * and using #Fine_clock to measure duration) and observed the following resolutions on certain OS
 * (not listing the hardware):
 *
 *   - Linux (2.6.x kernel): sub-2 millisecond resolution, with some variance.
 *     (Note: This Linux's `glibc` is too old to provide the `timerfd` API, therefore Boost falls back to
 *     using `epoll_wait()` directly; on a newer Linux this may get better results.)
 *   - Windows 7: 15 millisecond resolution, with little variance.
 *   - macOS: untested.  @todo Test macOS timer fidelity.
 *
 * These results were observed for BOTH `deadline_timer` and `basic_waitable_timer<Fine_clock>`
 * in Boost 1.50.  Thus, there was no resolution advantage to using the latter -- only an interface advantage.
 * Conclusion: we'd be well-advised not to rely on anything much smaller than 20 milliseconds when
 * scheduling timer events.  One technique might be, given any time T < 20 ms, to assume T = 0
 * (i.e., execute immediately).  This may or may not be appropriate depending on the situation.
 *
 * However, using `basic_waitable_timer<Fine_clock>` may get an additional nice property: #Fine_clock
 * always goes forward and cannot be affected by NTP or user changes.  `deadline_timer` may or may not
 * be affected by such tomfoolery (untested, but the potential is clearly there).  Therefore we do the best
 * we can by using the #Fine_clock-based timer.
 *
 * @todo Upgrade to newer Boost and keep testing timer resolutions on the above and other OS versions.
 * Update: As of this writing we are on Boost 1.74 now.  Needless to say, it is time to reassess the above, as it
 * has been years since 1.50 -- and we had been on 1.63 and 1.66 for long period of time, let alone 1.74.
 * Update: No changes with Boost 1.75, the newest version as of this writing.
 *
 * @note One annoying, though fully documented, "feature" of this and all boost.asio timers is that `cancel()`ing it
 * does not always cause the handler to fire with `operation_aborted` error code.  Sometimes it was already "just
 * about" to fire at `cancel()` time, so the handler will run with no error code regardless of the `cancel()`.
 * Personally I have never once found this behavior useful, and there is much code currently written to work
 * around this "feature."  Furthermore, even a "successful" cancel still runs the handler but with `operation_aborted`;
 * 99.999% of the time one would want to just not run it at all.  Finally, in many cases, even having to create
 * a timer object (and then the multiple steps required to actually schedule a thing) feels like more boiler-plate
 * than should be necessary.  To avoid all of these usability issues, see schedule_task_from_now() (and similar),
 * which is a facility that wraps `Timer` for a boiler-plate-free experience sufficient in the majority of practical
 * situations.  `Timer` is to be used directly when that simpler facility is insufficient.
 *
 * @note `b_w_t<Fine_clock>` happens to equal `boost::asio::high_resolution_timer`. We chose to alias
 *       in terms of #Fine_clock merely to ensure that #Fine_duration is one-to-one compatible with it, as it
 *       is defined in terms of #Fine_clock also.
 */
using Timer = boost::asio::basic_waitable_timer<Fine_clock>;

/// Helper type for setup_auto_cleanup().
using Auto_cleanup = boost::shared_ptr<void>;

/// Short-hand for the UDP endpoint (IP/port) type.
using Udp_endpoint = boost::asio::ip::udp::endpoint;
/// Short-hand for the IPv4 address type.
using Ip_address_v4 = boost::asio::ip::address_v4;
/// Short-hand for the IPv6 address type.
using Ip_address_v6 = boost::asio::ip::address_v6;

/// Short-hand for non-reentrant, exclusive mutex.  ("Reentrant" = one can lock an already-locked-in-that-thread mutex.)
using Mutex_non_recursive = boost::mutex;

/// Short-hand for reentrant, exclusive mutex.
using Mutex_recursive = boost::recursive_mutex;

/**
 * Short-hand for non-reentrant, shared-or-exclusive mutex.
 * When locking one of these, choose one of:
 * `Lock_guard<Mutex_shared_non_recursive>`, `Shared_lock_guard<Mutex_shared_non_recursive>`.
 * The level of locking acquired (shared vs. exclusive) depends on which you chose and is thus highly significant.
 *
 * @todo Consider changing util::Mutex_shared_non_recursive from `boost::shared_mutex` to `std::shared_mutex`, as
 * the former has a long-standing unresolved ticket about its impl being slow and/or outdated (as of Boost-1.80).
 * However see also the note on util::Mutex_shared_non_recursive that explains why it might be best to avoid
 * this type of mutex altogether in the first place (in most cases).
 *
 * ### When to use versus #Mutex_non_recursive ###
 * Experts suggest fairly strongly that one should be very wary about using a shared mutex, over a simple
 * non-recursive exclusive mutex, in almost any practical application.  The reason there's a trade-off at all is
 * that a #Mutex_non_recursive is extremely fast to lock and unlock; the entire perf cost is in waiting for
 * another thread to unlock first; so without contention it's almost *free*.  In contrast apparently any real
 * impl of #Mutex_shared_non_recursive is much slower to lock/unlock.  The trade-off is against allowing
 * reads to proceed in parallel; but experts say this is "worth it" only when these read critical sections
 * are lengthy and very frequently invoked.  So, for example, if the lock/unlock is around an `unordered_map`
 * lookup with a quick hashing function, it would be quite difficult to induce enough lock contention
 * to make a shared mutex better than an exclusive one.  (Note this is assuming even no perf issue with
 * `boost::shared_mutex` specifically.)
 */
using Mutex_shared_non_recursive = boost::shared_mutex;

/**
 * Short-hand for a mutex type equivalent to util::Mutex_shared_non_recursive -- except that the lock/unlock mutex ops
 * all do nothing.  One can parameterize templates accordingly; so that an algorithm can be generically written to work
 * in both single- and multi-threaded contexts without branching into 2 code paths, yet avoid the unnecessary actual
 * locking in the former case.
 */
using Mutex_noop_shared_non_recursive = boost::null_mutex;

/**
 * Short-hand for advanced-capability RAII lock guard for any mutex, ensuring exclusive ownership of that mutex.
 * Note the advanced API available for the underlying type: it is possible to relinquish ownership without unlocking,
 * gain ownership of a locked mutex; and so on.
 *
 * @see To grab shared-level ownership of a shared-or-exclusive mutex: use #Shared_lock_guard.
 *
 * @tparam Mutex
 *         A non-recursive or recursive mutex type.  Recommend one of:
 *         #Mutex_non_recursive, #Mutex_recursive, #Mutex_shared_non_recursive, #Mutex_noop_shared_non_recursive.
 */
template<typename Mutex>
using Lock_guard = boost::unique_lock<Mutex>;

/**
 * Short-hand for *shared mode* advanced-capability RAII lock guard, particuarly for #Mutex_shared_non_recursive
 * mutexes.
 *
 * @tparam Shared_mutex
 *         Typically #Mutex_shared_non_recursive, but any mutex type with that both-exclusive-and-shared API will work.
 */
template<typename Shared_mutex>
using Shared_lock_guard = boost::shared_lock<Shared_mutex>;

/**
 * (Deprecated given C++1x) Short-hand for advanced-capability RAII lock guard for #Mutex_non_recursive mutexes.
 *
 * @todo #Lock_guard_non_recursive is deprecated, now that C++1x made the more flexible
 * `Lock_guard<Mutex_non_recursive>` possible.  Remove it and all (outside) uses eventually.
 */
using Lock_guard_non_recursive = boost::unique_lock<Mutex_non_recursive>;

/**
 * (Deprecated given C++1x) Short-hand for advanced-capability RAII lock guard for #Mutex_recursive mutexes.
 *
 * @todo #Lock_guard_recursive is deprecated, now that C++1x made the more flexible `Lock_guard<Mutex_recursive>`
 * possible.  Remove it and all (outside) uses eventually.
 */
using Lock_guard_recursive = boost::unique_lock<Mutex_recursive>;

/**
 * (Deprecated given C++1x) Short-hand for *shared mode* advanced-capability RAII lock guard for
 * #Mutex_shared_non_recursive mutexes.
 *
 * @todo #Lock_guard_shared_non_recursive_sh is deprecated, now that C++1x made the more flexible
 * `Shared_lock_guard<Mutex_shared_non_recursive>` possible.  Remove it and all (outside) uses eventually.
 */
using Lock_guard_shared_non_recursive_sh = boost::shared_lock<Mutex_shared_non_recursive>;

/**
 * (Deprecated given C++1x) Short-hand for *exclusive mode* advanced-capability RAII lock guard for
 * #Mutex_shared_non_recursive mutexes.
 *
 * @todo #Lock_guard_shared_non_recursive_ex is deprecated, now that C++1x made the more flexible
 * `Lock_guard<Mutex_shared_non_recursive>` possible.  Remove it and all (outside) uses eventually.
 */
using Lock_guard_shared_non_recursive_ex = boost::unique_lock<Mutex_shared_non_recursive>;

/**
 * (Deprecated given C++1x) Equivalent to #Lock_guard_shared_non_recursive_sh but applied to
 * #Mutex_noop_shared_non_recursive.
 *
 * @todo #Lock_guard_noop_shared_non_recursive_sh is deprecated, now that C++1x made the more flexible
 * `Shared_lock_guard<Mutex_noop_shared_non_recursive>` possible.  Remove it and all (outside) uses eventually.
 */
using Lock_guard_noop_shared_non_recursive_sh = boost::shared_lock<Mutex_noop_shared_non_recursive>;

/**
 * (Deprecated given C++1x) Equivalent to #Lock_guard_shared_non_recursive_ex but applied to
 * #Mutex_noop_shared_non_recursive.
 *
 * @todo #Lock_guard_noop_shared_non_recursive_ex is deprecated, now that C++1x made the more flexible
 * `Lock_guard<Mutex_noop_shared_non_recursive>` possible.  Remove it and all (outside) uses eventually.
 */
using Lock_guard_noop_shared_non_recursive_ex = boost::unique_lock<Mutex_noop_shared_non_recursive>;

// Free functions.

/**
 * Equivalent to `val1.swap(val2)`.
 *
 * @relatesalso Linked_hash_map
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
template<typename Key, typename Mapped, typename Hash, typename Pred>
void swap(Linked_hash_map<Key, Mapped, Hash, Pred>& val1, Linked_hash_map<Key, Mapped, Hash, Pred>& val2);

/**
 * Equivalent to `val1.swap(val2)`.
 *
 * @relatesalso Linked_hash_set
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
template<typename Key, typename Hash, typename Pred>
void swap(Linked_hash_set<Key, Hash, Pred>& val1, Linked_hash_set<Key, Hash, Pred>& val2);

/**
 * Get the current POSIX (Unix) time as a duration from the Epoch time point.
 *
 * This is the amount of time -- according to the user-settable system clock time -- to have passed since the
 * POSIX (Unix) Epoch -- January 1st, 1970, 00:00:00 UTC -- not counting leap seconds to have been inserted or deleted
 * between then and now.
 *
 * The `boost::chrono` duration type is chosen so as to support the entire supported resolution of the
 * OS-exposed system clock (but probably no more than that).
 *
 * ### Known use cases, alternatives ###
 * - Logging of time stamps.  Output the raw value; or use boost.locale to output `ceil<seconds>()` in the desired
 *   human-friendly form, splicing in the left-over microseconds where desired (boost.locale lacks formatters for
 *   sub-second-resolution time points).  However see below for a typically-superior alternative.
 * - By subtracting return values of this at various points in time from each other, as a crude timing mechanism.
 *   (Various considerations make it just that -- crude -- and best replaced by flow::Fine_clock and the like.
 *   Moreover see flow::perf::Checkpointing_timer.)
 * - It's a decent quick-and-dirty random seed.
 *
 * ### Update/subtleties re. time stamp output ###
 * Using this for time stamp output is no longer needed or convenient, as a much nicer way presents itself
 * when combined with boost.chrono I/O-v2 `time_point`-outputting `ostream<<` overload.  Just grab `time_point`
 * `boost::chrono::system_clock::now()` (or another `system_clock`-originated value); its default-formatted
 * `ostream<<` output will include date, time with microsecond+ precision, and time-zone specifier (one can choose
 * UTC or local time).
 *
 * However, as of this writing, it is not possible to directly obtain *just* the microsecond+
 * part of this, in isolation, according boost.chrono docs.  (One could hack it by taking a substring.)
 * Quote: "Unfortunately there are no formatting/parsing sequences which indicate fractional seconds." From:
 * https://www.boost.org/doc/libs/1_76_0/doc/html/chrono/users_guide.html#chrono.users_guide.tutorial.i_o.system_clock_time_point_io)
 *
 * @return A duration representing how much time has passed since the Epoch reference point
 *         (could be negative if before it).
 */
boost::chrono::microseconds time_since_posix_epoch();

/**
 * Writes a multi- or single-line string representation of the provided binary data to an output stream, complete with
 * a printable and hex versions of each byte.
 *   - Single-line mode is chosen by setting `bytes_per_line` to special value `-1`.  In this mode, the output
 *     consists of `indentation`, followed by a pretty-printed hex dump of every byte in `data` (no matter how many),
 *     followed by a pretty-printed ASCII-printable (or '.' if not so printable) character for each byte; that's it.
 *     No newline is added, and `indentation` is included only the one time.  It is recommended to therefore print
 *     "in-line" inside log messages (etc.) without surrounding newlines, etc., when using this mode.
 *   - Multi-line mode is chosen by setting `bytes_per_line` to a positive value; or 0 which auto-selects a decent
 *     default positive value.  Then, each line represents up to that many bytes, which are pretty-printed similarly
 *     to single-line mode, in order of appearance in `data` (with the last such line representing the last few bytes,
 *     formatting nicely including accounting for potentially fewer than the desired # of bytes per line).
 *     Every line starts with `indentation` and ends with a newline -- including the last line.
 *     Therefore, it is recommended to precede this call with an output of a newline and to avoid doing so
 *     after that call (unless blank line is desired).
 *
 * Example with a single contiguous memory area, multi-line mode:
 *
 *   ~~~
 *   array<uint8_t, 256 * 256> bytes(...);
 *
 *   // Output to cout.
 *   cout
 *     << "Buffer contents: [\n";
 *   buffers_to_ostream(cout,
 *                      boost::asio::buffer(bytes), // Turn a single memory array into a buffer sequence.
 *                      "  ");
 *     << "]."; // This will be on its own line at the end.
 *   ~~~
 *
 * See also buffers_dump_string() which returns a string and can thus be more easily used directly inside
 * FLOW_LOG_DATA() and similar log macros.
 *
 * ### Performance ###
 * This thing is slow... it's not trying to be fast and can't be all that fast anyway.  As usual, though, if used
 * in FLOW_LOG_DATA() (etc.) its slowness will only come into play if the log filter passes which (esp. for
 * log::Severity::S_DATA) it usually won't.
 *
 * That said buffers_dump_string() is even slower, because it'll use an intermediate `ostream` independent of whatever
 * `ostream` you may or may not be placing the resulting string.  However it's easier to use with `FLOW_LOG_...()`,
 * and since in that case perf is typically not an issue, it makes sense to use the easier thing typically.
 *
 * However, if you do want to avoid that extra copy and need to *also* use buffers_to_ostream() directly inside
 * `FLOW_LOG_...()` then the following technique isn't too wordy and works:
 *
 *   ~~~
 *   const array<uint8_t, 256 * 256> bytes(...);
 *
 *   // Log with a flow::log macro.
 *   const flow::Function<ostream& (ostream&)> os_manip = [&](ostream& os) -> ostream&
 *   {
 *     return buffers_to_ostream(os, boost::asio::buffer(bytes), "  ");
 *   };
 *   FLOW_LOG_INFO("Buffer contents: [\n" << os_manip << "].");
 *
 *   // Above is probably more performant than:
 *   FLOW_LOG_INFO("Buffer contents: [\n"
 *                 << buffers_dump_string(boost::asio::buffer(bytes), "  ") // Intermediate+copy, slow....
 *                 << "].");
 *   // flow::util::ostream_op_string() is a bit of an improvement but still. :-)
 *   ~~~
 *
 * ### Rationale ###
 * The reason it returns `os` and takes a reference-to-mutable instead of the customary (for this project's
 * style, to indicate modification potential at call sites) pointer-to-mutable is in order to be `bind()`able
 * in such a way as to make an `ostream` manipulator.  In the example above we use a lambda instead of `bind()`
 * however.
 *
 * @tparam Const_buffer_sequence
 *         Type that models the boost.asio `ConstBufferSequence` concept (see Boost docs) which represents
 *         1 or more scattered buffers in memory (only 1 buffer in a sequence is common).
 *         In particular `boost::asio::const_buffer` works when dumping a single buffer.
 * @param os
 *        The output stream to which to write.
 * @param data
 *        The data to write, given as an immutable sequence of buffers each of which is essentially a pointer
 *        and length.  (Reminder: it is trivial to make such an object from a single buffer as well;
 *        for example given `array<uint8_t, 256> data` (an array of 256 bytes), you can just pass
 *        `boost::asio::buffer(data)`, as the latter returns an object whose type satisfies requirements of
 *        `Const_buffer_sequence` despite the original memory area being nothing more than a single
 *        byte array.)
 * @param indentation
 *        The indentation to use at the start of every line of output.
 * @param bytes_per_line
 *        If 0, act as-if a certain default positive value were passed; and then: If `-1`, single-line mode is invoked.
 *        If a positive value, multi-line mode is invoked, with all lines but the last one consisting of
 *        a dump of that many contiguous bytes of `data`.  (Yes, multi-line mode is still in force, even if there
 *        are only enough bytes in `data` for one line of output anyway.)
 * @return `os`.
 */
template<typename Const_buffer_sequence>
std::ostream& buffers_to_ostream(std::ostream& os, const Const_buffer_sequence& data, const std::string& indentation,
                                 size_t bytes_per_line = 0);

/**
 * Identical to buffers_to_ostream() but returns an `std::string` instead of writing to a given `ostream`.
 *
 * @see buffers_to_ostream() doc header for notes on performance and usability.
 *
 * @tparam Const_buffer_sequence
 *         See buffers_to_ostream().
 * @param data
 *        See buffers_to_ostream().
 * @param indentation
 *        See buffers_to_ostream().
 * @param bytes_per_line
 *        See buffers_to_ostream().
 * @return Result string.
 */
template<typename Const_buffer_sequence>
std::string buffers_dump_string(const Const_buffer_sequence& data, const std::string& indentation,
                                size_t bytes_per_line = 0);

/**
 * Utility that converts a bandwidth in arbitrary units in both numerator and denominator to the same bandwidth
 * in megabits per second.  The input bandwidth is given in "items" per `Time_unit(1)`; where `Time_unit` is an
 * arbitrary boost.chrono `duration` type that must be explicitly provided as input; and an "item" is defined
 * as `bits_per_item` bits.  Useful at least for logging.  It's probably easiest to understand by example; see below;
 * rather than by parsing that description I just wrote.
 *
 * To be clear (as C++ syntax is not super-expressive in this case) -- the template parameter `Time_unit` is
 * an explicit input to the function template, essentially instructing it as to in what units `items_per_time` is.
 * Thus all uses of this function should look similar to:
 *
 *   ~~~
 *   // These are all equal doubles, because the (value, unit represented by value) pair is logically same in each case.
 *
 *   // We'll repeatedly convert from 2400 mebibytes (1024 * 1024 bytes) per second, represented one way or another.
 *   const size_t MB_PER_SEC = 2400;
 *
 *   // First give it as _mebibytes_ (2nd arg) per _second_ (template arg).
 *   const double mbps_from_mb_per_sec
 *     = flow::util::to_mbit_per_sec<chrono::seconds>(MB_PER_SEC, 1024 * 8);
 *
 *   // Now give it as _bytes_ (2nd arg omitted in favor of very common default = 8) per _second_ (template arg).
 *   const double mbps_from_b_per_sec
 *     = flow::util::to_mbit_per_sec<chrono::seconds>(MB_PER_SEC * 1024 * 1024);
 *
 *   // Now in _bytes_ per _hour_.
 *   const double mbps_from_b_per_hour
 *     = flow::util::to_mbit_per_sec<chrono::hours>(MB_PER_SEC * 1024 * 1024 * 60 * 60);
 *
 *   // Finally give it in _bytes_ per _1/30th-of-a-second_ (i.e., per frame, when the frame rate is 30fps).
 *   const double mbps_from_b_per_30fps_frame
 *     = flow::util::to_mbit_per_sec<chrono::duration<int, ratio<1, 30>>(MB_PER_SEC * 1024 * 1024 / 30);
 *   ~~~
 *
 * @note Megabit (1000 x 1000 = 10^6 bits) =/= mebibit (1024 x 1024 = 2^20 bits); but the latter is only about 5% more.
 * @todo boost.unit "feels" like it would do this for us in some amazingly pithy and just-as-fast way.  Because Boost.
 * @tparam Time_unit
 *         `boost::chrono::duration<Rep, Period>` for some specific `Rep` and `Period`.
 *         See `boost::chrono::duration` documentation.
 *         Example types: `boost::chrono::milliseconds`; `boost::chrono::seconds`; see example use code above.
 * @tparam N_items
 *         Some (not necessarily integral) numeric type.  Strictly speaking, any type convertible to `double` works.
 * @param items_per_time
 *        The value, in items per `Time_unit(1)` (where there are `bits_per_item` bits in 1 item) to convert to
 *        megabits per second.  Note this need not be an integer.
 * @param bits_per_item
 *        Number of bits in an item, where `items_per_time` is given as a number of items.
 * @return See above.
 */
template<typename Time_unit, typename N_items>
double to_mbit_per_sec(N_items items_per_time, size_t bits_per_item = 8);

/**
 * Returns the result of the given non-negative integer divided by a positive integer, rounded up to the nearest
 * integer.  Internally, it avoids floating-point math for performance.
 *
 * @tparam Integer
 *         A signed or unsigned integral type.
 * @param dividend
 *        Dividend; non-negative or assertion trips.
 * @param divisor
 *        Divisor; positive or assertion trips.
 * @return Ceiling of `(dividend / divisor)`.
 */
template<typename Integer>
Integer ceil_div(Integer dividend, Integer divisor);

/**
 * Provides a way to execute arbitrary (cleanup) code at the exit of the current block.  Simply
 * save the returned object into a local variable that will go out of scope when your code block
 * exits.  Example:
 *
 *   ~~~
 *   {
 *     X* x = create_x();
 *     auto cleanup = util::setup_auto_cleanup([&]() { delete_x(x); });
 *     // Now delete_x(x) will be called no matter how the current { block } exits.
 *     // ...
 *   }
 *   ~~~
 *
 * @todo setup_auto_cleanup() should take a function via move semantics.
 *
 * @tparam Cleanup_func
 *         Any type such that given an instance `Cleanup_func f`, the expression `f()` is valid.
 * @param func
 *        `func()` will be called when cleanup is needed.
 * @return A light-weight object that, when it goes out of scope, will cause `func()` to be called.
 */
template<typename Cleanup_func>
Auto_cleanup setup_auto_cleanup(const Cleanup_func& func);

/**
 * Returns `true` if and only if the given value is within the given range, inclusive.
 *
 * @param min_val
 *        Lower part of the range.
 * @param val
 *        Value to check.
 * @param max_val
 *        Higher part of the range.  Must be greater than or equal to `min_val`, or behavior is undefined.
 * @tparam T
 *         A type for which the operation `x < y` is defined and makes sense.  Examples: `double`, `char`,
 *         `unsigned int`, #Fine_duration.
 * @return `true` if and only if `val` is in [`min_val`, `max_val`].
 */
template<typename T>
bool in_closed_range(T const & min_val, T const & val, T const & max_val);

/**
 * Returns `true` if and only if the given value is within the given range, given as a [low, high)
 * pair.
 *
 * @param min_val
 *        Lower part of the range.
 * @param val
 *        Value to check.
 * @param max_val
 *        Higher part of the range.  Must be greater than `min_val`, or behavior is undefined.
 * @tparam T
 *         A type for which the operation `x < y` is defined and makes sense.  Examples: `double`, `char`,
 *         `unsigned int`, #Fine_duration.
 * @return `true` if and only if `val` is in [`min_val`, `max_val`), i.e., `min_val` <= `val` < `max_val`.
 */
template<typename T>
bool in_closed_open_range(T const & min_val, T const & val, T const & max_val);

/**
 * Returns `true` if and only if the given value is within the given range, given as a (low, high]
 * pair.
 *
 * @param min_val
 *        Lower part of the range.
 * @param val
 *        Value to check.
 * @param max_val
 *        Higher part of the range.  Must be greater than `min_val`, or behavior is undefined.
 * @tparam T
 *         A type for which the operation `x < y` is defined and makes sense.  Examples: `double`, `char`,
 *         `unsigned int`, #Fine_duration.
 * @return `true` if and only if `val` is in (`min_val`, `max_val`], i.e., `min_val` < `val` <= `max_val`.
 */
template<typename T>
bool in_open_closed_range(T const & min_val, T const & val, T const & max_val);

/**
 * Returns `true` if and only if the given value is within the given range, given as a (low, high)
 * pair.
 *
 * @param min_val
 *        Lower part of the range.
 * @param val
 *        Value to check.
 * @param max_val
 *        Higher part of the range.  Must be at least 2 greater than `min_val`, or behavior is undefined.
 * @tparam T
 *         A type for which the operation `x < y` is defined and makes sense.  Examples: `double`, `char`,
 *         `unsigned int`, #Fine_duration.
 * @return `true` if and only if `val` is in (`min_val`, `max_val`), i.e., `min_val` < `val` < `max_val`.
 */
template<typename T>
bool in_open_open_range(T const & min_val, T const & val, T const & max_val);

/**
 * Returns `true` if and only if the given key is present at least once in the given associative
 * container.
 *
 * @tparam Container
 *         Associative container type (`boost::unordered_map`, `std::set`, etc.).
 *         In particular must have the members `find()`, `end()`, and `key_type`.
 * @param container
 *        Container to search.
 * @param key
 *        Key to find.
 * @return See above.
 */
template<typename Container>
bool key_exists(const Container& container, const typename Container::key_type& key);

/**
 * Performs `*minuend -= subtrahend`, subject to a floor of `floor`.  Avoids underflow/overflow to the extent
 * it's reasonably possible, but no more.  The return value indicates whether
 * the floor was hit; this allows one to chain high-performance subtractions like this:
 *
 *   ~~~
 *   double t = 44;
 *   int x = rnd(); // Suppose x == 123, for example.
 *   // Avoids the 2nd, 3rd computation altogether, as the first detects that x >= 44, and thus t == 0 regardless.
 *   subtract_with_floor(&t, x) &&
 *     subtract_with_floor(&t, long_computation()) &&
 *     subtract_with_floor(&t, another_long_computation());
 *   ~~~
 *
 * @tparam Minuend
 *         Numeric type.
 * @tparam Subtrahend
 *         Numeric type, such that given `Subtrahend s`, `Minuend(s)` is something reasonable for all `s` involved.
 * @param minuend
 *        `*minuend` is set to either `(*minuend - subtrahend)` or `floor`, whichever is higher.
 * @param subtrahend
 *        Ditto.
 * @param floor
 *        Ditto.  Negatives are OK.  Typically it's best to keep the magnitude of this small.
 * @return `true` if `*minuend == floor` at function exit; `false` if `*minuend > floor`.
 */
template<typename Minuend, typename Subtrahend>
bool subtract_with_floor(Minuend* minuend, const Subtrahend& subtrahend, const Minuend& floor = 0);

/**
 * Answers the question *what's the smallest integer number of `To`s sufficient to verbatim store the given number of
 * `From`s?*, where `From` and `To` are POD types.
 *
 * For example, one needs 1 `uint64_t` to store 1, 2, 3, or 4 `uint16_t`s, hence
 * `size_unit_convert<uint16_t, uint64_t>(1 or 2 or 3 or 4) == 1`.  Similarly, 5 or 6 or 7 or 8 -> 2.
 * It works in the opposite direction, too; if we are storing `uint64_t`s in multiples of `uint16_t`, then
 * 1 -> 4, 2 -> 8, 3 -> 12, etc.
 *
 * To be clear, when `From` bit width is smaller than `To` bit width, some of the bits will be padding and
 * presumably unused.  For example, raw data buffers of arbitrary bytes are often arranged in multi-byte "words."
 *
 * @tparam From
 *         The POD type of the values that must be encoded in `From`s.
 * @tparam To
 *         The POD type of the array that would store the `From`s.
 * @param num_froms
 *        How many `From`s does one want to encode in an array of `To`s?
 * @return How many `To`s are sufficient to encode `num_from` `From`s verbatim?
 */
template<typename From, typename To>
size_t size_unit_convert(From num_froms);

/**
 * Given a generic sequence (integer -> object) generates a generic map (object -> integer) providing inverse
 * lookup.  See the 3-arg overload if you want to provide a more complex lookup function to store something else based
 * on each index.
 *
 * A naive way of implementing lookups otherwise would be a linear search for the object; using this will
 * use RAM to avoid the slow searches.
 *
 * @tparam Sequence
 *         Sequence such as `std::vector<T>` or `std::array<T>`.
 *         Informally, `T` should be something light-weight and hence usable as a key type for a map type (`Map`).
 * @tparam Map
 *         Map that maps `T` from `Sequence` to `size_t`.  Example: `std::map<T, size_t>`.
 * @param src_seq
 *        Input sequence.
 * @param target_map
 *        Output map.  Note it will *not* be pre-cleared; informally, this means one can shove 2+ lookup maps
 *        into one.  If null behavior undefined (assertion may trip).
 */
template<typename Map, typename Sequence>
void sequence_to_inverted_lookup_map(Sequence const & src_seq, Map* target_map);

/**
 * Similar to the 2-arg overload of sequence_to_inverted_lookup_map() but with the ability to store a value based
 * on the index into the input sequence instead of that index itself.  See the 2-arg overload.
 *
 * @tparam Sequence
 *         Sequence such as `std::vector<T>` or `std::array<T>`.
 *         Informally, `T` should be something light-weight and hence usable as a key type for a map type (`Map`).
 * @tparam Map
 *         Map that maps `T` from `Sequence` to another type `X`.  Example:
 *         `unordered_map<T, X>`, where an `X` can be computed from a `size_t` index.
 * @param src_seq
 *        See 2-arg overload.
 * @param target_map
 *        See 2-arg overload.
 * @param idx_to_map_val_func
 *        Given an index `idx` into `src_seq`, `(*target_map)[]` shall contain `idx_to_map_val_func(idx)`.
 *        Use this arg to instead perform a second lookup before storing a value in `*target_map`.
 *        Use the 2-arg overload if you'd like to store the index itself.
 */
template<typename Map, typename Sequence>
void sequence_to_inverted_lookup_map
       (Sequence const & src_seq, Map* target_map,
        const Function<typename Map::mapped_type (size_t)>& idx_to_map_val_func);

/**
 * Writes to the specified string, as if the given arguments were each passed, via `<<` in sequence,
 * to an `ostringstream`, and then the result were appended to the aforementioned string variable.
 *
 * Tip: It works nicely, 99% as nicely as simply `<<`ing an `ostream`; but certain language subtleties mean
 * you may have to fully qualify some template instances among `ostream_args`.  Do so if you receive a
 * "deduced incomplete pack" (clang) or similar error, as I have seen when using, e.g., `chrono::symbol_format`
 * formatter (which the compile error forced me to qualify as: `symbol_format<char, ostream::traits_type>`).
 *
 * @see log::Thread_local_string_appender for an even more efficient version of this for some applications that can
 *      also enable a continuous stream across multiple stream-writing statements over time.
 *
 * @tparam ...T
 *         Each type `T` is such that `os << t`, with types `T const & t` and `ostream& os`, builds and writes
 *         `t` to `os`, returning lvalue `os`.
 *         Usually in practice this means the existence of `ostream& operator<<(ostream&, T const &)` or
 *         `ostream& operator<<(ostream&, T)` overload, the latter usually for basic types `T`.
 *         See also tip above, if compiler is unable to deduce a given `T` (even when it *would* deduce it
 *         in `os << t`).
 * @param target_str
 *        Pointer to the string to which to append.
 * @param ostream_args
 *        One or more arguments, such that each argument `arg` is suitable for `os << arg`, where
 *        `os` is an `ostream`.
 */
template<typename ...T>
void ostream_op_to_string(std::string* target_str, T const &... ostream_args);

/**
 * Equivalent to ostream_op_to_string() but returns a new `string` by value instead of writing to the caller's
 * `string`.  This is useful at least in constructor initializers, where it is not possible to first
 * declare a stack variable.
 *
 * With the C++11-y use of move semantics in STL it should be no slower than using
 * `ostream_op_to_string()` -- meaning, it is no slower, period, as this library now requires C++11.
 *
 * @tparam ...T
 *         See ostream_op_to_string().
 * @param ostream_args
 *        See ostream_op_to_string().
 * @return Resulting `std::string`.
 */
template<typename ...T>
std::string ostream_op_string(T const &... ostream_args);

/**
 * "Induction step" version of variadic function template that simply outputs arguments 2+ via
 * `<<` to the given `ostream`, in the order given.
 *
 * @tparam ...T_rest
 *         See `...T` in ostream_op_to_string().
 * @param remaining_ostream_args
 *        See `ostream_args` in ostream_op_to_string().
 * @tparam T1
 *         Same as each of `...T_rest`.
 * @param ostream_arg1
 *        Same as each of `remaining_ostream_args`.
 * @param os
 *        Pointer to stream to which to sequentially send arguments for output.
 */
template<typename T1, typename ...T_rest>
void feed_args_to_ostream(std::ostream* os, T1 const & ostream_arg1, T_rest const &... remaining_ostream_args);

/**
 * "Induction base" for a variadic function template, this simply outputs given item to given `ostream` via `<<`.
 *
 * @tparam T
 *         See each of `...T` in ostream_op_to_string().
 * @param os
 *        Pointer to stream to which to sequentially send arguments for output.
 * @param only_ostream_arg
 *        See each of `ostream_args` in ostream_op_to_string().
 */
template<typename T>
void feed_args_to_ostream(std::ostream* os, T const & only_ostream_arg);

/**
 * Deserializes an `enum class` value from a standard input stream.  Reads up to but not including the next
 * non-alphanumeric-or-underscore character; the resulting string is then mapped to an `Enum`.  If none is
 * recognized, `enum_default` is the result.  The recognized values are:
 *   - "0", "1", ...: Corresponds to the underlying-integer conversion to that `Enum`.  (Can be disabled optionally.)
 *   - Case-[in]sensitive string encoding of the `Enum`, as determined by `operator<<(ostream&)` -- which must exist
 *     (or this will not compile).  Informally we recommend the encoding to be the non-S_-prefix part of the actual
 *     `Enum` member; e.g., `"WARNING"` for log::Sev::S_WARNING.
 * If the scanned token does not map to any of these, or if end-of-input is encountered immediately (empty token),
 * then `enum_default` is returned.
 *
 * Error semantics: There are no invalid values or exceptions thrown; `enum_default` returned is the worst case.
 * Do note `*is_ptr` may not be `good() == true` after return.
 *
 * Tip: It is convenient to implement `operator>>(istream&)` in terms of istream_to_enum().  With both
 * `>>` and `<<` available, serialization/deserialization of the `enum class` will work; this enables a few key things
 * to work, including parsing from config file/command line via and conversion from `string` via `lexical_cast`.
 *
 * Informal convention suggestion: `S_END_SENTINEL` should be the sentinel member of `Enum`.  E.g., see log::Sev.
 *
 * @tparam Enum
 *         An `enum class` which must satisfy the following requirements or else risk undefined behavior (if it
 *         compiles): An element, `enum_lowest`, has a non-negative integer value.
 *         Subsequent elements are strictly monotonically increasing with increment 1, up to and including
 *         `enum_sentinel`.  (Elements outside [`enum_lowest`, `enum_sentinel`] may exist, as long as their numeric
 *         values don't conflict with those in-range, but informally we recommend against this.)
 *         `ostream << Enum` exists and works without throwing for all values in range
 *         [`enum_lowest`, `enum_sentinel`).  Each `<<`-serialized string must be distinct from the others.
 *         Each `<<`-serialized string must start with a non-digit and must consist only of alphanumerics and
 *         underscores.  Exception: digit-leading is allowed if and only if `!accept_num_encoding`, though
 *         informally we recommend against it as a convention.
 *
 * @param is_ptr
 *        Stream from which to deserialize.
 * @param enum_default
 *        Value to return if the token does not match either the numeric encoding (if enabled) or the `<<` encoding.
 *        `enum_sentinel` is a sensible (but not the only sensible) choice.
 * @param enum_sentinel
 *        `Enum` value such that all valid deserializable values have numeric conversions strictly lower than it.
 * @param accept_num_encoding
 *        If `true`, a numeric value is accepted as an encoding; otherwise it is not (and will yield `enum_default`
 *        like any other non-matching token).
 * @param case_sensitive
 *        If `true`, then the token must exactly equal an `ostream<<` encoding of a non-sentinel `Enum`;
 *        otherwise it may equal it modulo different case.
 * @param enum_lowest
 *        The lowest `Enum` value.  Its integer value is very often 0, sometimes 1.
 *        Behavior undefined if it is negative.
 * @return See above.
 */
template<typename Enum>
Enum istream_to_enum(std::istream* is_ptr, Enum enum_default, Enum enum_sentinel,
                     bool accept_num_encoding = true, bool case_sensitive = false,
                     Enum enum_lowest = Enum(0));

/**
 * Sets certain `chrono`-related formatting on the given `ostream` that results in a consistent, desirable output
 * of `duration`s and certain types of `time_point`s.  As of this writing this includes enabling short unit format
 * (e.g., "ms" instead of "milliseconds") and avoiding Unicode characters (the Greek letter for micro becomes
 * a similar-looking "u" instead).
 *
 * @see log::beautify_chrono_logger_this_thread() to affect a Logger directly.
 * @see flow::async in which new threads are set to use this formatting automatically.  However you'll want to
 *      do this explicitly for the startup thread.
 *
 * @param os
 *        The stream to affect.
 */
void beautify_chrono_ostream(std::ostream* os);

/**
 * Estimate of memory footprint of the given value, including memory allocated on its behalf -- but
 * excluding its shallow `sizeof`! -- in bytes.
 *
 * @param val
 *        Value.
 * @return See above.
 */
size_t deep_size(const std::string& val);

// Macros.

/**
 * Expands to an `ostream` fragment `X` (suitable for, for example: `std::cout << X << ": Hi!"`) containing
 * the file name, function name, and line number at the macro invocation's context.
 *
 * It's a functional macro despite taking no arguments to convey that it mimics a free function sans args.
 *
 * @internal
 *
 * ### Performance ###
 * The items `<<`ed onto the target `ostream` are each evaluated at compile-time.
 * This includes the expression involving flow::util::get_last_path_segment() which is `constexpr`.
 */
#define FLOW_UTIL_WHERE_AM_I() \
  FLOW_UTIL_WHERE_AM_I_FROM_ARGS(::flow::util::get_last_path_segment \
                                   (::flow::util::String_view(__FILE__, sizeof(__FILE__) - 1)), \
                                 ::flow::util::String_view(__FUNCTION__, sizeof(__FUNCTION__) - 1), \
                                 __LINE__)

/**
 * Same as FLOW_UTIL_WHERE_AM_I() but evaluates to an `std::string`.  It's probably a bit slower as
 * well.  Update: In fact, it's probably significantly slower as of this writing, as
 * FLOW_UTIL_WHERE_AM_I() evaluates the `<<`ed items at compile-time, while this cannot due to how it's implemented.
 *
 * @internal
 *
 * flow::util::get_where_am_i_str() is too clever to become a `constexpr` function, even if it were radically rewritten,
 * so this macro evaluates to a partially runtime-computed expression.  Hence the note just above about perf.
 *
 * @todo See if FLOW_UTIL_WHERE_AM_I_STR() can be coaxed into a compile-time expression after all.  It is used in
 * quite a lot of frequently executed code, namely at the top of most Flow (and Flow-like) APIs that can emit errors.
 * See notes inside flow::util::get_where_am_i_str() on this as of this writing.
 */
#define FLOW_UTIL_WHERE_AM_I_STR() \
  ::flow::util::get_where_am_i_str(::flow::util::get_last_path_segment \
                                     (::flow::util::String_view(__FILE__, sizeof(__FILE__) - 1)), \
                                   ::flow::util::String_view(__FUNCTION__, sizeof(__FUNCTION__) - 1), \
                                   __LINE__)

/**
 * Use this to create a semicolon-safe version of a "void" functional macro definition consisting of at least two
 * statements; or of one statement that would become two statements by appending a semicolon.
 *
 * There may be other use cases where it might be useful, though none more has come to mind as of this writing.
 * Loosely speaking, IF your sub-statements are always blocks, and you have a "void" functional macro to be used in
 * the typical way (every use is a statement that looks like a function call), and the macro's definition is anything
 * *other* than an expression with an intentionally missing trailing semicolon, THEN you should probably wrap
 * that would-be macro definition in a FLOW_UTIL_SEMICOLON_SAFE().  However, read on if you want to know rationale
 * and/or formalities.
 *
 * ### Formal use case requirements ###
 * The following is assumed about every *use* of this macro; behavior is undefined otherwise.
 *
 * - The macro's invocation is the *entire definition* of another macro, M.
 * - M is a functional macro taking 0 or more arguments (possibly variadic).
 * - The following is true about every *use* of the macro M:
 *   - The invocation is written as if M is a free function in the C language, and it is being "called"
 *     as the *entirety* of the statement containing that "call."  That is, it must look like this:
 *
 *   ~~~
 *   M(...arg 1 value..., ...arg 2 value..., ...more...); // (Or however many args M actually allows.)
 *   ~~~
 *
 * ### Known use case 1: Macro definition is 2+ statements ###
 * Suppose your macro's value is a series of two or more statements executing in series without braces `{}`
 * around the whole thing.  Then, in some contexts, invoking that macro as intended might result in
 * unexpected runtime misbehavior that the compiler wouldn't detect.  A simple example with two
 * expressions as individual statements:
 *
 *   ~~~
 *   #define DOUBLE_AND_PRINT(arg) \
 *     arg *= 2;    // Statement 1. \
 *     cout << arg  // Statement 2.  Note the lacking trailing semicolon (invoker must supply it after "call"). \
 *   DOUBLE_AND_PRINT(x); // No problem. Both statements execute.
 *   if (some_flag)
 *     DOUBLE_AND_PRINT(x); // UNINTENDED BEHAVIOR!  Statement 2 executes regardless of some_flag!
 *   ~~~
 *
 * Granted, if the invoker used braces appropriately around the `if` statement body, there'd be no issue, but one
 * cannot always rely on invoker's good style, especially if the macro is part of a public library API.
 * Solution:
 *
 *   ~~~
 *   #define DOUBLE_AND_PRINT(arg) \
 *     FLOW_UTIL_SEMICOLON_SAFE \
 *     ( \
 *       arg *= 2;    // Statement 1. \
 *       cout << arg; // Statement 2.  Note necessity for trailing semicolon. \
 *     )
 *   DOUBLE_AND_PRINT(x); // No problem.  Both statements execute.
 *   if (some_flag)
 *     DOUBLE_AND_PRINT(x); // No problem.  Both statements execute; or neither one does.
 *   ~~~
 *
 * One might ask: Why not simply surround the 2+ statements with braces `{}` to eliminate the problem more simply?
 * Answer: This actually would turn the use case into the following other problematic use case.
 *
 * ### Known use case 2: Required semicolon `;` trailing macro invocation leads to syntax error ###
 * Recall that the premise *requires* invoker to call your macro M like `M();`, meaning they will *always* put
 * a semicolon after the invocation.  Now suppose that doing so will turn one statement (which is M's definition)
 * into two.  Another way of putting it: Suppose M's body is already a complete statement, even without the required
 * trailing `;` (and there is no way to make it otherwise; usually [always?] because it ends with a `}`).
 * Then in some contexts it'll cause a syntax error.  Example:
 *
 *   ~~~
 *   #define CHECK_AND_LOG(msg) \
 *     if (filter()) \
 *     { \
 *       log(msg); \
 *     } // Semicolon will always be added right here.
 *   CHECK_AND_LOG("Hello."); // No problem.  The empty ; statement introduced is harmless.
 *   if (terminating)
 *     CHECK_AND_LOG(msg); // SYNTAX ERROR!  The empty ; will make the `else` illegal.
 *   else
 *     exit();
 *   ~~~
 *
 * Removing the `;` from the invocation is no solution.  Firstly it'll confuse many editors that will think the
 * "function call" (really macro invocation) is missing the usual semicolon.  This might cause incorrect
 * auto-indentation in the file and who knows what else.  Code analyzer tools may also be affected.
 * Even ignoring issues where the code is fed into a non-compiler, there are possible flow control problems.
 * In this example it'll attach the `else` in the invoking code to the inner `if` (from the macro)
 * instead of the outer `if` (in the invoking code) as intended.  Wrapping the body of `CHECK_AND_LOG()` in
 * the proposed wrapper will resolve all stated problems.  (Solution code omitted for brevity and obviousness
 * at this point.)
 *
 * @note Adding this wrapper would slightly adversely affect performance in the complete absence of compiler
 *       optimization, but even the most basic optimizer (one capable of eliminating `if (false)` and similar)
 *       should entirely eliminate any such performance degradation.
 *
 * @param ARG_func_macro_definition
 *        The intended value of a functional macro, such that the "function" it approximates would have
 *        return type `void`.  Behavior is undefined if the value of this parameter ends in a semicolon, or
 *        if the value contains a statement that includes another statement within it that is not a block
 *        (`{ ... }`).  (The rules in the last [English] statement may or may not be strictly necessary,
 *        but requiring these allows me to generalize/reason more straightforwardly.  In any case, one rule is
 *        assumed good style anyway, while the other is simply practical in this context.)
 * @return Code to be used as the entire definition of a macro whose definition would have been
 *         `ARG_func_macro_definition` if not for the resulting compile error or unexpected flow control
 *         effects detailed above.
 */
#define FLOW_UTIL_SEMICOLON_SAFE(ARG_func_macro_definition) \
  do \
  { \
    ARG_func_macro_definition \
  } \
  while (false)

} // namespace flow::util
