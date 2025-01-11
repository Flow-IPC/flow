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

#include "flow/detail/common.hpp"
/* These are just so commonly used, that I (ygoldfel) decided to just shove them here for everyone because of the
 * following boost.chrono I/O thing being nearby anyway. */
#include <boost/chrono/chrono.hpp>
#include <boost/chrono/ceil.hpp>
#include <boost/chrono/round.hpp>
/* boost.chrono I/O: This is subtle.  chrono.hpp doesn't include I/O (including the invaluable ostream<< output of
 * chrono::duration and chrono::time_point, especially when the latter is from chrono::system_clock).  (Probably that
 * is because std::chrono I/O support is spotty-to-non-existent, including lacking solid ostream<<.)  One must include
 * chrono/chrono_io.hpp... except, as it stands, this loads v1 -- not v2.  (Looking inside the header shows it's based
 * on some BOOST_ #define jungle, but the bottom line is every time I (ygoldfel) see Boost built, it loads v1, not v2.)
 * However loading v2 is easy (and correct): #include the following 2 files instead; chrono_io.hpp would do so itself if
 * the BOOST_ #define were set accordingly.
 *
 * This should be transparent, and transparently a good thing, for all `#include`ing devs.  There are some subtleties
 * however.  In particular these bear mentioning:
 *   - Output of a time_point from system_clock (e.g., chrono::system_clock::now()) to an ostream will (in v2) yield a
 *     nice, predictably formatted, human-readable date/time string that includes the UTC +0000 specifier.
 *     (v1 said something fairly useless -- which v2 still, by necessity, does for non-system_clock values -- like
 *     "28374928742987 nanoseconds since Jan 1, 1970".)  This is used directly by flow::log for example.
 *     With chrono::time_fmt(chrono::timezone::local) formatter applied to the ostream<<, it'll be in local time
 *     with the appropriate +xxxx specifier, instead.  (There is also custom formatting available but never mind.)
 *   - Breaking change: `chrono::duration_{short|long}` don't work (v1); use chrono::{symbol|name}_format instead (v2).
 *     (This switches between, e.g., "5 ns" and "5 nanoseconds.")
 *   - As of Boost 1.76 at least, v2 includes some deprecated Boost headers which results in a #pragma message (not
 *     a warning/error but verbose and unsightly).  -DBOOST_ALLOW_DEPRECATED_HEADERS will make it go away.
 *   - v1 and v2 cannot co-exist (some things, like ostream<<, become defined 2x).  Therefore do not attempt to
 *     #include <boost/chrono/chrono_io.hpp>.  At best it's redundant; at worst it will fail to compile. */
#include <boost/chrono/io/duration_io.hpp>
#include <boost/chrono/io/time_point_io.hpp>
#include <functional>
// For certains compile-time checks below.
#ifdef __APPLE__
#  include <TargetConditionals.h>
#endif

/**
 * @mainpage
 *
 * Welcome to the Flow project.  The documentation you're viewing has been generated using Doxygen directly from Flow
 * source code and comments.  I recommend reading the documentation of `namespace` ::flow as the starting point; then
 * that will direct one to the documentation of one or more of its sub-namespaces, depending on which Flow module(s)'s
 * functionality interests that person.
 */

/* We build in C++17 mode ourselves (as of this writing), but linking user shouldn't care about that so much.
 * The APIs and header-inlined stuff (templates, constexprs, possibly explicitly-inlined functions [though we avoid
 * those]), however, also requires C++17 or newer; and that applies to the linking user's `#include`ing .cpp file(s)!
 * Therefore enforce it by failing compile unless compiler's C++17 or newer mode is in use.
 *
 * Note this isn't academic; as of this writing there's at least a C++14-requiring constexpr feature in use in one
 * of the headers.  So at least C++14 has been required for ages in actual practice.  Later, notched it up to C++17
 * by similar logic.
 *
 * Update: We continue to target C++17 (by default) in our build -- but now also support (and, outside
 * the source code proper, test) C++20 mode build, albeit without using any C++20-only language or STL features.  It is
 * conceivable that at some point in the future we'll target C++20 by default (and drop support for C++17 and older).
 * Naturally at that point we'd begin using C++20 language/STL features when convenient. */
#if (!defined(__cplusplus)) || (__cplusplus < 201703L)
// Would use static_assert(false), but... it's C++11 and later only.  So.
#  error "To compile a translation unit that `#include`s any flow/ API headers, use C++17 compile mode or later."
#endif

// Macros.  These (conceptually) belong to the `flow` namespace (hence the prefix for each macro).

#ifdef FLOW_DOXYGEN_ONLY // Compiler ignores; Doxygen sees.

/// Macro that is defined if and only if the compiling environment is Linux.
#  define FLOW_OS_LINUX

/// Macro that is defined if and only if the compiling environment is Mac OS X or higher macOS (not iOS and such!).
#  define FLOW_OS_MAC

/// Macro that is defined if and only if the compiling environment is Windows.
#  define FLOW_OS_WIN

#else // if !defined(FLOW_DOXYGEN_ONLY)

// Now the actual definitions compiler sees (Doxygen ignores).

/* Used this delightful page for this logic:
 *   https://web.archive.org/web/20140625123925/http://nadeausoftware.com/articles/2012/01/c_c_tip_how_use_compiler_predefined_macros_detect_operating_system */
#  ifdef __linux__
#    define FLOW_OS_LINUX
#  elif defined(__APPLE__) && (TARGET_OS_MAC == 1)
#    define FLOW_OS_MAC
#  elif defined(_WIN32) || defined(_WIN64)
#    define FLOW_OS_WIN
#  endif

#endif // elif !defined(FLOW_DOXYGEN_ONLY)

/**
 * Catch-all namespace for the Flow project: A collection of various production-quality modules written in modern
 * C++17, originally by ygoldfel.  (The very first version was in Boost-y C++03, back around 2010.  Later, ~2019, it
 * moved to C++14 in style and substance; and in 2022 to C++17 -- a relatively minor upgrade.)
 * While the modules are orthogonal to each other in terms of functionality
 * provided, they all share a common set of stylistic conventions and happen to use each other internally; hence
 * they are distributed together as of this writing.
 *
 * From the user's perspective, one should view this namespace as the "root," meaning it consists of two parts:
 *   - Sub-namespaces (like flow::log, flow::util, flow::async, flow::net_flow),
 *     each of which represents a *Flow module* providing certain
 *     grouped functionality.  Each module is self-contained from the point of view of the user, meaning:
 *     While the various modules may use each other internally (and hence cannot be easily distributed separately), from
 *     user's perspective each one can be directly `include`d/referenced without directly `include`ing/referring to the
 *     others.  E.g., one can directly reference `namespace log` *and/or* `namespace util` *and/or* `namespace net_flow`
 *     *and/or* ....  Further documentation can be found in the doc headers for each individual sub-namespace.
 *   - Symbols directly in `flow`: The absolute most basic, commonly used symbols (such as `uint32_t` or `Error_code`).
 *     There should be only a handful of these, and they are likely to be small.
 *     - In particular this includes `enum class Flow_log_component` which defines the set of possible
 *       flow::log::Component values logged from within all modules of Flow (again, including flow::util, flow::async,
 *       flow::net_flow, etc.).  See end of common.hpp.
 *
 * Reiterating: Non-`namespace` symbols directly inside `namespace flow` are to be only extremely ubiquitous items
 * such as the basic integer types and the log component `enum`.  Anything beyond that should go into a sub-`namespace`
 * of `flow`; if it is something miscellaneous then put it into flow::util.
 *
 * Here we summarize topics relevant to all of Flow.  It is recommend one reads this before using any individual module,
 * for topics like file organization and style conventions, topics arguably not of huge practical value right away.
 * However, all the actual functionality is in the sub-modules, so once you're ready to actually do stuff, I reiterate:
 * See the list of sub-namespaces of `flow` and start with the doc header of the one(s) of interest to you.
 *
 * Documentation / Doxygen
 * -----------------------
 *
 * All code in the project proper follows a high standard of documentation, almost solely via comments therein
 * (as opposed to ancillary doc files/READMEs/etc.).  Additionally, a subset of comments are Doxygen-targeted,
 * meaning the comment starts with a special character sequence to cause the Doxygen utility to treat that comment
 * as a doc header for a nearby symbol (class, method, etc.).  (If you're not familiar with Doxygen: It's like Javadoc
 * but better -- although mostly compatible with Javadoc-targeted comments as well -- not that we care about that.)
 *
 * From the same source code (the entire Flow tree) 2 (two) web docs are generated by Doxygen utility -- depending on
 * which of the 2 Doxygen configuration files is chosen.  You can determine which web docs you're reading currently
 * (if indeed you are doing that, as opposed to reading raw code) via the wording of the title/header in every web page:
 *   - *Flow project: Full implementation reference*: This scans every header and translation unit file, including
 *     those in detail/ sub-directories at all levels; and all Doxygen-targeted comments (including this one)
 *     are fully scanned.  Full browsable source code is included in the output also.  As a result, it's basically
 *     an ultra-nice viewer of the entire source code (implementation included), with special emphasis on
 *     doc headers such as this one.
 *   - *Flow project: Public API*: Only headers are scanned; and all detail/ sub-directories at all levels are ignored.
 *     Additionally, any text following the (optional) `"@internal"` Doxygen command within each given Doxygen comment
 *     is ignored.  Browsable source code is omitted.  As a result, a clean public API documentation is generated with
 *     no "fat."
 *
 * While an important (achieved) goal is to keep the documentation of the library (internal and external, as noted)
 * self-contained -- within the source code or auto-generated by taking that source code as input -- nevertheless some
 * satellite documentation is likely to exist; for example to cover such things as the logistical state of the project,
 * its test harness, perhaps the license, etc.  Look outside the root src/flow directory.
 *
 * @todo As of this writing the *exact* nature of where the project will permanently reside (and who will maintain it
 * vs. use it) is in flux.  Therefore for now I have removed the section covering certain topics and replaced it with
 * the to-do you're reading.  This should be undone when things settle down (obviously ensuring the brought-back section
 * is made accurate).  The topics to cover: `"@author"` (including contact info); GitHub/other address indicating where
 * to browse the project source; link(s) to the latest auto-generated web docs (if any); a section on the
 * history of the project; and licensing info (if deemed needed) or pointer to it.  (Reminder: Also update any
 * similar sections of the historically significant net_flow::Node doc header.)
 *
 * @todo Since Flow gained its first users beyond the original author, some Flow-adjacent code has been written from
 * which Flow can benefit, including a potential `io` module/namespace for general networking/local I/O.
 * (Flow itself continued to be developed, but some features were added
 * elsewhere for expediency; this is a reminder to factor them out into Flow for the benefit of
 * all.)  Some features to migrate here might be: boost.asio extensions to UDP receive APIs to obtain receipt time
 * stamps and destination IP (`recvmsg()` with ancillary data extensions) and to receive multiple datagrams in one
 * call (`recvmmsg()`); boost.asio-adjacent facility to add custom socket options similarly to how boost.asio does it
 * internally; boost.asio support for (local) transmission of native socket handles and security data over stream
 * sockets (`SCM_RIGHTS`, etc.).
 *
 * Using Flow modules
 * ------------------
 *
 * This section discusses usability topics that apply to all Flow modules including hopefully any future ones but
 * definitely all existing ones as of this writing.
 *
 * ### Error reporting ###
 * Similarly to boost.asio, all public methods that may return errors can either do so via an
 * error code or an exception encapsulating that same error code.  If user passes in non-null pointer to a
 * flow::Error_code variable, the latter is set to success (falsy) or a specific failure (truthy `enum` value).  If
 * user passes in null pointer, an exception `exc` is thrown only in case of error and will encapsulate that error code
 * (accessible via `exc.code()`).
 *
 * For details about error reporting, see doc headers for flow::Error_code (spoiler alert: a mere alias to
 * `boost::system::error_code`) and `namespace` flow::error.
 *
 * ### Logging ###
 * The flow::log namespace (see especially log::Logger and the various `FLOW_LOG_...*()` macros in
 * log/log.hpp) provides a logging facility -- used by Flow modules' often-extensive logging, and equally available
 * to the Flow user.  Ultimately, you may tweak the log level and then observe the given Flow module's internal behavior
 * to whatever level of detail you desire.  Similarly, as the user, you may use this system for your own logging,
 * even if you use no Flow module other than flow::log itself.  Either way, you can hook up flow::log to log via your
 * own log output device in arbitrary fashion (e.g., save the log messages in a database, if that's what you want).
 *
 * For details about logging, see doc header for `namespace` flow::log.
 *
 * @internal
 *
 * Implementation notes
 * --------------------
 *
 * There is a high standard of consistency and style, as well as documentation, in Flow.  Before making changes to
 * the code, you must read doc-coding_style.cpp.
 *
 * ### Source file tree organization ###
 * See doc-coding_style.cpp which covers this topic.
 *
 * ### Libraries used; Boost ###
 * We use STL and several Boost libraries (notably boost.asio) extensively.
 * See doc-coding_style.cpp which covers this topic.
 *
 * ### Exceptions ###
 * For reasons of speed (hearsay or reputational though they may be) we avoid exception-throwing
 * boost.asio routines inside the modules' implementations, even though using them would make the code a bit simpler
 * in many areas.  Similarly, we avoid throwing our own exceptions only to catch them higher in the back-trace.
 * In both cases, we typically use error codes/return values.  These are not absolute/hard rules, as exceptions may be
 * used where minuscule speed improvements are immaterial (like the initialization and shutdown of a long-lived object
 * such as net_flow::Node).
 *
 * After a recent look at performance implications of exceptions, the following is the basic conclusion: Exceptions have
 * no processor cycle cost, unless actually thrown, in which case the perf cost can be non-trivial.  Since we consider
 * internal performance of paramount importance, we generally avoid throwing exceptions -- only to catch them
 * internally -- as noted in the preceding paragraph.  Arguably such situations are rare anyway, so using an exception
 * internally wouldn't actually slow anything down, but we haven't performed a code audit to verify that stipulation;
 * and in the meantime the consistent avoidance of using internally caught exceptions has proved to be a very reasonable
 * policy.  (For some modules this may be arguably mostly moot: Consider flow::net_flow specifically: Since --
 * internally -- mostly boost.asio `async_*()` methods are used, and those
 * by definition report errors as codes passed into async handler functions; whether to use an exception or not is
 * an ill-formed question in all such situations.)  We recommend this practice continue, until there's a specific reason
 * to reconsider.
 *
 * See doc-coding_style.cpp for more discussion of error handling and exceptions.
 *
 * Code style guidelines
 * ---------------------
 *
 * ### General coding style requirements ###
 * The formal guidelines are in doc-coding_style.cpp; read that file please.
 *
 * ### Documentation guidelines ###
 * The standard for documentation of Flow modules is that someone reading the source code, and nothing else, would
 * be able to understand that code (modulo having the intellectual sophistication/experience w/r/t the subject
 * matter, of course).  Simple but quite a task given how much code there is and the complexity.  We also
 * produce Doxygen output (2 web pages, as of this writing) by running the code through Doxygen.
 *
 * @see More on the technicalities of how we run Doxygen, and the philosophy behind all of that, can be found
 * in a `doc/` or similar directory outside src/flow.  It's rare something pertinent to the source code
 * is not IN the source code (i.e., right here somewhere), but the README explains why that rare choice is
 * made (among many more practical/interesting things).  This is worth reading if you'll be contributing to the
 * code.
 *
 * The actual guidelines are, as above, in doc-coding_style.cpp; read that file please.
 *
 * @todo Possibly document exceptions thrown explicitly via the Doxygen keyword meant for this purpose: `"@throws"`.
 * Currently when we document explicitly throwing an exception, it is ALWAYS a flow::error::Runtime_error
 * encapsulating a flow::Error_code (which is an `int`-like error code).  This is very explicitly documented,
 * but technically Doxygen has a keyword which will generate a special little readout for the exception
 * (similarly as for each parameter, etc.).  We don't use that keyword.  We probably should, though this
 * isn't totally cut-and-dried.  Consider that we already document the exception on the `err_code` parameter
 * in every case; so no information would really be gained (only arguably nicer formatting).  On the other hand,
 * the code would be somewhat more verbose (and boiler-platey, since each already boiler-platey `err_code`
 * comment snippet would essentially grow in size).  Furthermore, if we document this explicit exception, one might
 * say it behooves us to now document all the other possible sources of exceptions such as `std::bad_alloc` when
 * running out of heap memory.  Perhaps then we have to talk about constructor-throwing-exception behavior and
 * other C++ technicalities to do with exceptions.  Do we really want to enter that land?  I think maybe not;
 * consider just leaving it alone.  Though, maybe I'm over-dramatizing the impact of adding a `"@throws"`
 * section on our various flow::error::Runtime_error-throwing methods.  Well, it's a to-do; decide later.
 *
 * To-dos and future features
 * --------------------------
 *
 * @todo The comments (as of this writing, all written by me, ygoldfel) in this library could use an edit to make
 * them briefer.  (I've found even a self-edit by me, with that express purpose, often does wonders.)  Background:
 * I write very extensive comments.  I am quite convinced this is superior (far superior even) to next-to-no comments;
 * and to the average amount of comments one tends to see in production code.  *That said*, the top code review
 * feedback I receive (not here specifically but in general) is that my comments tend to be too "discursive" (consisting
 * of discourse) and/or at times unnecessarily in-depth.  Discursive = as if I am talking to the reader (many prefer
 * a terser, more formal style).  Too in-depth = tends to go into history, related issues, future work, etc.
 * (these elements can remain but can be cut down significant without losing much substance).
 * In other words, there should be a happy middle in terms of comment volume, and this can
 * be achieved by a comment edit run by Yuri or someone else (if reviewed by Yuri).  To be clear, I believe this middle
 * ground is to be closer to the status quo than to the average amount of comments in other projects.
 *
 * @todo Be more specific (cite date) when writing "as of this writing."
 * I use a rhetorical trick when commenting the state of something that may not continue to be the case.
 * Though I do avoid writing such things, often it is necessary; in that case I usually write "as of this writing" or
 * something very similarly worded.  That's fine and essentially the best one can do.  It means technically the
 * statement won't become false, even if the "sub-statement" (the thing that was true when written) does become false.
 * However, obviously, to the reader of the comment at that later time, that's little consolation: they're still reading
 * a possibly false statement and will want to know what the situation is THEN, or "as of the reading," to to speak.
 * In order to at least try to be helpful, in those cases a date (numeric month/year -- like 4/2017 -- should be
 * sufficient in most cases) should be supplied.  The to-do is to convert all "as of this writing" instances -- and
 * to always add a date when writing new instances of "as of this writing."  The to-do can be removed once the
 * conversion is completed.  Example: this to-do has not been completed as of this writing (11/2017).
 * (Side note: possibly goes without saying, but one is free to explain to an arbitrary degree of detail why something
 * is true as of that writing, and how/why/when it might change.  This to-do covers those cases where no such
 * explanation is written.  It would be impractically verbose to get into speculative detail for every
 * as-of-this-writing instance; so at least a date should thus be inserted.)
 *
 * @todo There are some boost.thread "interruption points" throughout the code, so we should
 * investigate whether we must catch `boost::thread_interrupted` in those spots, or what...?
 *
 * @todo Inline things:  Or just use `gcc -O3` (and non-`gcc` equivalents) for prettier/faster-to-compile
 * code?  The latter is definitely tempting if it works sufficiently well.  So far we've been using
 * `gcc -O3` and equivalents, and it seems to be working well (turning off inlining results in huge
 * performance losses).  Still, I am not sure if it would be better to explicitly `inline` functions
 * instead.  Not having to do so definitely simplifies the code, so it is my great hope that the answer is
 * no, and we can keep using `gcc -O3` and equivalents.  In that case delete this paragraph.
 * (To be clear: `gcc -O3` means that it ignores `inline` keyword and anything similar, including inlined
 * method bodies inside `class {}` and `struct {}`.  Instead it determines what to inline based on its own
 * ideas on what will generate the fastest code (with reasonable code size).  `gcc -O2`, on the other
 * hand, will mostly inline things explicitly declared as such in code (again, via `inline` or inlining inside
 * class bodies or other techniques).)  Update: Now using clang (not gcc) with maximum auto-inlining AND FLTO
 * (link-time optimization will allow inlining across object file boundaries) in at least some platforms.
 * This should be close to as good as possible.  Update: gcc auto-inlining+FLTO also works.
 *
 * @todo One space after period, not two:
 * For some reason in this project I've been following the convention -- in comments and (I think) log
 * messages -- of two spaces after a sentence-ending punctuator (usually period) before the next sentence's
 * first character.  I now regret trying this annoying convention.  Go back to one space. (For the record,
 * it does look sort of nice, but that's a matter of opinion, and single space looks fine too... AND doesn't
 * confuse various editors' auto-formatting facilityies, among other problem.)
 *
 * @todo We use `0` instead of `NULL` or `nullptr` when needing a null pointer; perhaps we should use the latter.
 * `NULL` is an anachronism from C, so we shouldn't use it.  `nullptr` is at least no worse than `0`,
 * however, other than being less concise.  However, the main reason it exists --
 * to avoid ambiguities in function overloading (e.g., when something could
 * take either an `int` or a `char*`, `nullptr` would resolve to the latter, while `0` probably unintentionally
 * to the former) -- is not a situation our style ever invokes, to my knowledge, so using `nullptr` would not
 * solve any actual problems.  However, it could be argued that using it more readily calls attention to the use
 * of a pointer, as opposed to an integer, in the particular context at play.  So it's something to consider
 * (but, no doubt, the conversion process would be laborious, as there's no simple search-replace that would
 * work).
 *
 * @todo `= default` for copy constructors and copy operators is now used in a few places; consider spreading
 * this C++11 feature everywhere it's being done implicitly due to C++03 rules (good documentation practices suggest
 * declaring them explicitly but of course leave the implementation to the compiler default, gaining best of both
 * worlds -- proper class API docs yet maintenance-friendly default body).
 *
 * @todo Consider PIMPL and related topics.  Recommend scouring Boost docs, particularly for the smart pointer library,
 * which discuss how to potentially use smart pointers for easy PIMPLing.  In general, research the state of the art
 * on the topic of library interface vs. implementation/hiding.
 *
 * @todo `std::string_view` is a way to pass things around similarly to `const std::string&` without requiring
 * that a `string` be created just for that purpose; it has a highly similar API but can be constructed from any
 * character sequence in memory and internally stores nothing more than a pointer and length; we should use it wherever
 * possible (within reason) instead of `const std::string&`.  Much code now uses `String_view`; the remaining to-do
 * is: scour the rest of the code for possible `const string&`s to convert and indeed convert those to
 * util::String_view.
 *
 * @todo Return-by-copy binary operators of the form `T operatorBLAH(const T& x1, const Some_type& x2)` should be
 * written as free functions instead of `T` members.  I don't recall at this point why, but this tends to be recommended
 * and done in STL and Boost.  Maybe check the Meyer Effective C++ book on the theory; and if it makes sense find all
 * such operators written as members and change them to be free functions.  Should this be avoided if it requires
 * `friend` though?  Lastly, for Doxygen, use the `relatesalso T` command to link the free function to the class `T`
 * in the documentation.
 *
 * @todo In many (most) cases we pass `shared_ptr`s (and their aliases) by value when it would be more performant to
 * do so by `const` reference; at times possibly better to pass by raw pointer.  Scott Meyer/Herb Sutter have opined
 * on this to basically indicate that (1) it is often best to use a raw pointer, unless actual copying/ownership status
 * is expected; but failing that (2) it is often best to use a `const&` when safe; and failing that passing by
 * value is fine.  This need not be a dogmatic change, but we should be more mindful than simply always passing by
 * value.  When searching for instances to potentially change, check for `shared_ptr`, `Ptr`, and `_ptr` tokens.
 */
namespace flow
{

// Types.  They're outside of `namespace ::flow::util` for brevity due to their frequent use.

// Integer short-hands and specific-bit-width types.

/// Byte.  Best way to represent a byte of binary data.  This is 8 bits on all modern systems.
using uint8_t = unsigned char;
/// Signed byte.  Prefer to use `uint8_t` when representing binary data.  This is 8 bits on all modern systems.
using int8_t = signed char;

// Time-related short-hands.

/**
 * Clock used for delicate time measurements, such that the `now()` method gets the current time
 * relative to some unknown but constant epoch (reference point).  Used to measure durations of
 * things.  It has the following properties:
 *
 *   - Steady: time cannot go backwards (e.g., via user time change, NTP); time values increment at
 *     a rate proportional to real time (no leap seconds for example).
 *   - High-resolution: the increments of time at which the clock runs are as small as supported
 *     by the OS+hardware.  This should be at most a handful of microseconds in practice.
 *
 * So basically it's a precise clock with no surprises (which is more than can be said for stuff people
 * tend to be forced to use, like `gettimeofday()`).
 */
using Fine_clock = boost::chrono::high_resolution_clock;

/// A high-res time point as returned by `Fine_clock::now()` and suitable for precise time math in general.
using Fine_time_pt = Fine_clock::time_point;

/// A high-res time duration as computed from two `Fine_time_pt`s.
using Fine_duration = Fine_clock::duration;

/**
 * Short-hand for a boost.system error code (which basically encapsulates an integer/`enum` error
 * code and a pointer through which to obtain a statically stored message string); this is how Flow modules
 * report errors to the user; and we humbly recommended all C++ code use the same techniques.
 *
 * @note It is not inside flow::error namespace due to its (`Error_code`'s) ubiquity.
 *       Very few other symbols should follow suit.  We may decide to move it there after all.
 *
 * ### Basic error-emitting API semantics ###
 *
 * All error-reporting Flow APIs follow the following pattern of error reporting semantics.
 * Each API looks something like:
 *
 *   ~~~
 *   return_type Some_class::some_op(..., flow::Error_code* err_code)
 *   ~~~
 *
 * Then, there are two possibilities.  If you pass in non-null `err_code`, then after return `*err_code` is
 * success (falsy) or a truthy `enum`-like value, representing a specific error.  If, instead, you pass in null,
 * then a flow::error::Runtime_error() `exc` is thrown if and only if `*err_code` would have been set to truthy value
 * `e_c` had a non-null `err_code` been passed in.  If such an exception is thrown, `Error_code e_c` is
 * encapsulated in exception object `exc`.  If and only if no exception is thrown, there was no error (`*err_code` would
 * have been falsy).
 *
 * Thus, you get the best of both worlds: you can get the simplicity and performance
 * of an error code; or the various features of an exception (including access to the error code via
 * `exc.code()` if desired), with the same API signature.  (boost.asio
 * follows a similar concept, though it requires two API signatures for each operation, one without
 * an `Error_code` argument, and one with non-`const` `Error_code&` out-arg.  The above convention is more compact;
 * plus we provide certain tools to reduce boiler-plate in connection with this.)
 *
 * ### Intro to `Error_code`, a/k/a boost.system `error_code` ###
 * (I am restating boost.system documentation here, but that particular set of docs is notoriously
 * formal-but-reader-unfriendly.)
 *
 * A truthy `Error_code` is a very lightweight -- `errno`-like in that regard -- value indicating
 * the error that occurred.  It stores an `int` code and a "category" pointer (basically, thing specifying to what code
 * set this belongs).  The `int` is to be converted from the error code set of choice, whereas the category pointer is
 * internally magically determined based on the type of the error code value being converted to `Error_code`.
 *
 * An `Error_code` itself can be serialized into `ostream`s (and thus `string`s via `lexical_cast`, etc.) easily for
 * logging purposes/etc.  You can access both the numeric code and a human explanation of the error.
 * Any and all error code sets are supported by this boost.system type.  POSIX `errno`s are one possible set of codes;
 * boost.asio has its own code set; and other modules in `flow` may introduce their own code sets.  All are compatible
 * for equality/assignment/etc. with this general `Error_code` type.
 *
 * As stated, all error-emitting Flow public APIs (regardless of module) use the above-described error-reporting
 * conventions.  In addition, we humbly recommend Flow *user* code adopt the same battle-tested conventions.  However
 * that is absolutely not required and is entirely independent of the fact that Flow modules use them.  Do note this
 * convention is battle-tested in boost.asio as well; though Flow's version is more compact; by using a pointer (which
 * can be null) instead of a reference it cuts the number of error-emitting API functions in half.
 *
 * For each function (including each publicly exported error-reporting function within Flow) that indeed agrees to
 * use the above convention, follow these instructions:
 *
 * To reduce boiler-plate, within reason, it is incumbent on each error-reporting method to use the following
 * technique:
 *
 * - The method signature should be similar to the above (including naming it `err_code`) and use the above semantics.
 *   - Use FLOW_ERROR_EXEC_AND_THROW_ON_ERROR() (and/or nearby similar utilities in flow/error/error.hpp) for minimal
 *     boiler-plate that implements these semantics.  See doc header for that macro for details.
 * - You may or may not indicate the lack or presence of an error condition via some additional non-exception technique
 *   such as a `bool` return value.
 * - The error behavior documentation should be *confined entirely* to the documentation of `err_code` parameter, so
 *   that the above semantics need not be repetitively restated a million times.
 *   The text of the parameter's doc should usually be as follows (you may copy/paste to start).  In this example
 *   the function returns codes from the `net_flow::error::Code` code set `enum`; but please substitute your code set of
 *   choice (again; `errno` and boost.asio error codes are 2 possible other examples of code sets).  Here we go:
 *
 *   ~~~
 *   // param err_code
 *   //       See flow::Error_code docs for error reporting semantics.  net_flow::error::Code generated:
 *   //       net_flow::error::Code::S_(...) (optional comment), ...more... , net_flow::error::Code::S_(...)
 *   //       (optional comment).
 *   ~~~
 *
 * @see The doc header (and code inside) `namespace` flow::net_flow::error is a good primer showing how to create
 *      an `Error_code`-compatible set of error codes.  This is easier to understand than boost.asio's counterpart
 *      for example.
 *
 * @note boost.system at some point -- I (ygoldfel) am fairly sure after I designed the above ages ago -- introduced
 *       an alternate idiom for passing an #Error_code out-arg that is to be ignored in favor of throwing an exception
 *       if omitted.  We use the idiom: `Error_code*` out-arg, throw if null.  They, instead propose:
 *       `Error_code&` out-arg, throw if it equals `boost::system::throws()`.  That's great, too, but actually our
 *       idiom hews to another bit of the Flow coding style/guide, wherein out-args should be pointers, not
 *       non-`const` references -- and is otherwise very similar.  So it's fine.  Note that their idiom vs. ours =
 *       orthogonal to the main difficulty which is the boiler-plate associated with actually throwing vs. non-throwing;
 *       this would be required regardless of the API idiom chosen.  The above (involving
 *       FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(), etc.) is really the main crux of it.
 */
using Error_code = boost::system::error_code;

// See just below.
template<typename Signature>
class Function;

/**
 * Intended as the polymorphic function wrapper of choice for Flow, internally and externally; to be used
 * instead of `std::function` or `boost::function`.  Due to ubiquitous use of such function-object wrappers,
 * this is one of the very few direct non-`namespace` members of the outer namespace `::flow`.
 *
 * In reality it *is* `std::function`, with a couple of added APIs (no data) to make it more similar to
 * `boost::function` API-wise.
 *
 * ### Rationale ###
 * By far the main reason this exists is: I (ygoldfel) conducted an investigation in 2022, with a not-too-old gcc
 * in Linux, in C++17 mode, with Boost 1.78 (but built seemingly in C++03-supporting config) about the
 * performance behavior of lambdas objects and `boost::function<>` and `std::function<>` wrappers thereof.
 * I noticed these things:
 *   - Say one constructs a `boost::function` from a lambda that has 1 or more by-value captures `[x = std::move(x)]`,
 *     with those captures' types having move ctors different and faster than copy ctors.  I found that this not only
 *     *copies* each of those captures (invokes their copy ctors), but it even does so several times!
 *     - However, constructing an `std::function` identically never invokes copy ctors -- only move ctors --
 *       and fewer times at that.
 *   - Say one constructs a `boost::function` from a lambda that has 1 or more by-value captures `[x = std::move(x)]`,
 *     with those captures' types having move ctors but `= delete`d copy ctors.  I.e., say at least 1 capture is
 *     movable but not copyable; `unique_ptr` being a classic and practical example.  Well: This does not compile;
 *     gcc complains `boost::function` needs `unique_ptr` to have a copy ctor, but it is `delete`d.
 *     In the case of `unique_ptr`, one can do `[x = shared_ptr(std::move(x))]` to get it work, though it's a bit
 *     more code, reduces perf by adding ref-counting, and reduces smart-ptr safety inside the lambda body.
 *     - However, constructing an `std::function` identically compiles and works fine.
 *
 * So, long story short, at least in that environment, `std::function` is just plain faster than `boost::function`,
 * avoiding copying of captures; and it's easier to use with movable-not-copyable capture types.
 *
 * So we could have just done circa `using Function = std::function;`.  Why the subclass?  Answer: `std::function`
 * lacks a couple of commonly used `boost::function` APIs that code tends to rely on (in the past we used
 * `boost::function`).  These are empty() and clear() as of this writing.  See their doc headers (nothing amazing).
 *
 * Lastly: In the aforementioned environment, possibly because of having to support C++03 (which lacked
 * param packs) -- unlike `std::function` which was introduced in C++11 to begin with (and
 * probably conceptually based on `boost::function`) -- `boost::function`
 * supports up to 10 args and does not compile when used with 11+ args.  `std::function`, and therefore Function,
 * lacks this limitation.  It can be used with any number of args.
 *
 * @tparam Result
 *         See `std::function`.
 * @tparam Args
 *         See `std::function`.
 */
template<typename Result, typename... Args>
class Function<Result (Args...)> :
  public std::function<Result (Args...)>
{
public:
  // Types.

  /// Short-hand for the base.  We add no data of our own in this subclass, just a handful of APIs.
  using Function_base = std::function<Result (Args...)>;

  // Ctors/destructor.

  /// Inherit all the constructors from #Function_base.  Add none of our own.
  using Function_base::Function_base;

  // Methods.

  /**
   * Returns `!bool(*this)`; i.e., `true` if and only if `*this` has no target.
   *
   * ### Rationale ###
   * Provided due to ubuiquity of code that uses `boost::function::empty()` which `std::function` lacks.
   *
   * @return See above.
   */
  bool empty() const noexcept;

  /**
   * Makes `*this` lack any target; i.e., makes it equal to a default-constructed object of this type, so that
   * `empty() == true` is a post-condition.
   *
   * ### Rationale ###
   * Provided due to ubuiquity of code that uses `boost::function::clear()` which `std::function` lacks.
   */
  void clear() noexcept;
}; // class Function<Result (Args...)>

#ifdef FLOW_DOXYGEN_ONLY // Actual compilation will ignore the below; but Doxygen will scan it and generate docs.

/**
 * The flow::log::Component payload enumeration comprising various log components used by Flow's own internal logging.
 * Internal Flow code specifies members thereof when indicating the log component for each particular piece of
 * logging code.  Flow user specifies it, albeit very rarely, when configuring their program's logging
 * such as via flow::log::Config::init_component_to_union_idx_mapping() and flow::log::Config::init_component_names().
 *
 * If you are reading this in Doxygen-generated output (likely a web page), be aware that the individual
 * `enum` values are not documented right here, because flow::log auto-generates those via certain macro
 * magic, and Doxygen cannot understand what is happening.  However, you will find the same information
 * directly in the source file `log_component_enum_declare.macros.hpp` (if the latter is clickable, click to see
 * the source).
 *
 * ### Details regarding overall log system init in user program ###
 *
 * The following is a less formal reiteration of flow::log::Config documentation and is presented here -- even
 * though technically in the present context Flow itself is nothing more than yet another module that uses
 * flow::log for its own logging -- for your convenience.  Flow's own logging can be seen as the canonical/model
 * use of flow::log, so other flow::log users can read this to learn the basics of how to configure loggingg.
 * That's why we re-explain this info here, in brief form:
 *
 * Your program -- that uses the present library -- can register this `enum` in order for these components
 * (and particularly the log messages that specify them via flow::log::Log_context or
 * FLOW_LOG_SET_CONTEXT()) to be logged properly in that program, co-existing correctly with other code bases
 * that use flow::log for logging.  Typically one constructs a `flow::log::Config C` and then at some point
 * before logging begins:
 *   - For each `enum class X_log_component` (note that `Flow_log_component` is only one such `enum class`):
 *     -# `C.init_component_to_union_idx_mapping<X_log_component>(K)`, where `K` is a distinct numeric offset,
 *         maybe multiple of 1000.
 *     -# `C.init_component_names<X_log_component>(S_X_LOG_COMPONENT_NAME_MAP, ..., "X-")`.
 *        - Note the "X-" prefix, allowing one to prepend a namespace-like string prefix to avoid any output and config
 *          clashing.
 *     -# `C.configure_default_verbosity(Sev::S)`, where `S` is some default max severity.
 *     -# For each component `M` for which one desires a different max severity `S`:
 *        - `C.configure_component_verbosity<X_log_component>(Sev::S, X_log_component::M)`. OR:
 *        - `C.configure_component_verbosity_by_name(Sev::S, "X-M")`.
 *   - Apply `C` to the flow::log::Logger or `Logger`s you want to affect.
 *   - Pass the `Logger` or `Logger`s to appropriate APIs that want to log.
 *
 * One could make changes after logging has begun, but that's a separate topic.
 */
enum class Flow_log_component
{
  /**
   * CAUTION -- see flow::Flow_log_component doc header for directions to find actual members of this
   * `enum class`.  This entry is a placeholder for Doxygen purposes only, because of the macro magic involved
   * in generating the actual `enum class`.
   */
  S_END_SENTINEL
};

/**
 * The map generated by flow::log macro magic that maps each enumerated value in flow::Flow_log_component to its
 * string representation as used in log output and verbosity config.  Flow user specifies, albeit very rarely,
 * when configuring their program's logging via flow::log::Config::init_component_names().
 *
 * As a Flow user, you can informally assume that if the component `enum` member is called `S_SOME_NAME`, then
 * its string counterpart in this map will be auto-computed to be `"SOME_NAME"` (optionally prepended with a
 * prefix as supplied to flow::log::Config::init_component_names()).  This is achieved via flow::log macro magic.
 *
 * @see flow::Flow_log_component first.
 */
extern const boost::unordered_multimap<Flow_log_component, std::string> S_FLOW_LOG_COMPONENT_NAME_MAP;

#endif // FLOW_DOXYGEN_ONLY

// Template implementations.

template<typename Result, typename... Args>
bool Function<Result (Args...)>::empty() const noexcept
{
  return !*this;
}

template<typename Result, typename... Args>
void Function<Result (Args...)>::clear() noexcept
{
  *this = {};
}

} // namespace flow
