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

/// @cond
// -^- Doxygen, please ignore the following.
#define BOOST_FILESYSTEM_NO_DEPRECATED 1
/// @endcond
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

/**
 * Flow module providing logging functionality.  While originally intended to
 * be used from within the flow::net_flow module's implementation, it can be used by general user code as well.
 * All other Flow modules expose flow::log concepts when logging is relevant, so one way or another the user
 * is likely to have at least limited contact with flow::log if they use Flow at all.  (In particular, classes
 * and free/`static` functions in various Flow modules often take a Logger pointer as a constructor
 * or function argument respectively.  Hence to use such APIs one must instantiate a concrete Logger, the simplest
 * choice being probably Simple_ostream_logger.)
 *
 * (The general user [in their own, non-Flow-related code] may well
 * prefer to use another logging system directly instead; or use boost.log.
 * However, we humbly recommend taking a look at flow::log as a possibility, as it is highly usable, yet fast, yet
 * small and elegant enough to ensure complete visibility into its low-level behavior, including how that affects
 * performance.  Note also that this system easily integrates with others via log::Logger interface, so one can stack
 * this on top of perhaps a lower-level logging facility.  The performance cost of this is essentially 1-2
 * `virtual` pointer lookups per log-message call.)
 *
 * Log philosophy of Flow is as follows.  The following is a list of goals; and for each item, sub-item(s)
 * explain how this goal is accomplished.
 *
 * - Flow is a library or set of libraries -- not an application or set of applications -- so logging has to flexibly
 *   work with any logging system the user code may use.
 *   - The Logger interface provides a way to efficiently log to an arbitrary logging output; the flow::log
 *     user must provide a simple Logger implementation that fills in what Logger::should_log() and
 *     Logger::do_log() mean.
 *   - To ensure maximum simplicity and usability right out of the box,
 *     flow::log provides a Logger implementation, Simple_ostream_logger, which will log to standard `ostream`s
 *     (including `cout` and `cerr`).  The user can just use this out of the box and not even have to implement
 *     the (already trivial) Logger interface.  (Probably most larger projects would need to go beyond this.)
 * - The logging system has to provide both error messages and informational or trace messages, and these
 *   should be easily distinguishable once passed on to the user's supplied arbitrary logging output(s).
 *   - There are at least 3 message severities, and they are passed to the Logger implementation, so
 *     that this can be translated as necessary into the user's logging output(s).
 * - The logging system is not a replacement for error reporting by Flow APIs.
 *   - flow::Error_code and flow::error provide at least error code reporting (in addition to log calls)
 *     where possible and applicable.
 * - The logging should be possible to entirely enable, entirely disable, or some reasonable point
 *   between these extremes.
 *   - The Logger interface's Logger::should_log() method allows filtering messages by severity (e.g., allow
 *     only WARNING messages) in any desired fashion.  There is now also support for setting filtering config
 *     by "component" supplied essentially at the log message call site.
 * - If a given message is not ultimately logged due its severity, it should induce no performance or resource
 *   penalty (such as that incurred by assembling the message string) beyond the severity check itself.
 *   - The `FLOW_LOG_...()` macros work with the `Logger::should_log()`/`Logger::do_log()` APIs to only
 *     construct the ultimate message string if the resulting message would be logged.
 * - Logging should be simple without lots of extraneous features.
 *   - A few simple-to-use macros are the main way to log messages; see those first.
 *   - Every logged message just has a severity and component, which are `enum`s, and the text of the message.
 *     File/function/line and thread ID info are auto-obtained and maintained along with the
 *     messages themselves, via the `FLOW_LOG_...()` macros.  The user need not worry about such mundane but tricky
 *     details.
 *   - The `FLOW_LOG_...()` macros assume that `get_logger()` (in the context where the macro is expanded)
 *     returns a `Logger*` implementation, which is then used for the logging call(s) associated with the
 *     message -- typically first the Logger::should_log() check and then possibly the Logger::do_log()
 *     message output.  Deriving from the supplied Log_context utility class is an easy way to make this
 *     `get_logger()` available all over the Flow modules' implementations (and should indeed be a technique for the
 *     user's own logging code, if indeed they decided to use flow::log themselves).
 *
 * @internal
 *
 * log_fwd.hpp is separate from log.hpp due to C++ circular dependency nonsense.  See util_fwd.hpp for some
 * discussion on this same concept.
 *
 * @todo There are third-party logging systems including boost.log.  The to-do is to
 * investigate using boost.log, partially or entirely replacing the manually implemented system.
 * (To be honest I am rather partial to the simple but effective system already implemented, in particular
 * the performance-focused trickery.)  Boost is invaluable
 * in many ways, and Flow modules use it extremely extensively, but my intuition says that in this case it may be
 * irrelevant.  Meanwhile, boost.log can, of course, be used within user's Logger interface implementation,
 * but that's not what this to-do is about; it's about actually placing it all over Flow's implementation and
 * APIs *in place of* Logger, log::Sev, etc.  (Also, after a 5-minute glance at boost.log,
 * I am noticing that some of its ideas I seem to have independently and
 * unknowingly replicated here.  It is, however, simple stuff, not rocket science.)  My intuition is I hesitate to
 * force-marry the Flow user to boost.log; the very simple Logger (abstract) interface is seemingly a gentler
 * thing to force the Flow user into, at least relatively speaking.
 *
 * @todo Lacking feature: message IDs.  This is discussed a bit more in the log::Msg_metadata doc header.
 *
 * @todo Lacking feature: `printf()`-style logging call sites, in contrast to the currently supported `ostream`
 * fragment call sites.  So, like, `FLOW_LOG_WARNING_FMT("Hello, Mr. %s!", my_name.c_str())` could be used
 * analogously to `FLOW_LOG_WARNING("Hello, Mr " << my_name << "!!")` (with `std::string my_name`).
 * In terms of implementation, there is more to this than meets the eye perhaps; as `ostream`s potentially
 * store (formatting) state between totally separate invocations of `ostream<<`, whereas `printf()` style
 * functions normally do not.  Internally, this likely allows for specially optimized logging code.
 *
 * @todo Lacking feature: boost.format-style logging call sites.  This is conceptually similar to -- and possibly even
 * entirely subsuming in backwards-compatible fashion -- the to-do just above for `printf()`-style logging.
 * Likely both to-dos should be designed/implemented in one shot.  Moreover, it is possible (and would be
 * absolutely delightful if true) that boost.format's `format` class can be used entirely transparently on top
 * of the `ostream`-focused existing flow::log API (such as FLOW_LOG_WARNING() and buddies)!
 *
 * @todo Lacking feature: log message rate-limiting.  This could actually mean a few things.  One is being able
 * to rate-limit the messages produced a given log call site per unit time; this might be the minimum for this to-do.
 * This could be done by a dirty macro hack; or it could be done in more civilized fashion (while minding perf)
 * by configuring it by message ID (but message IDs are optional and not implemented as of this writing anyway).
 * Another rate-limiting thing is general controls on frequency of logging; though arguably that is more applicable
 * to individual `Logger` implementations rather than as a mandatory-general mechanism.
 *
 * @todo Lacking feature: compiler hints for optimizing away log filter checks.  This is inspired by a certain
 * other proprietary logging API in which we noticed attempts to give hints to the compiler as to how likely or
 * unlikely the verbosity check is to return a true or false value, seemingly so that the compiler might
 * optimize out the check and hence the entire log statement in some conditions; or *always* log others and
 * skip the check in the optimized code.  I omit any details here, but that's the general idea.  I don't know
 * how effective this is given that verbosities are configurable dynamically potentially; but look into it.
 */
namespace flow::log
{

// Types.

// Find doc headers near the bodies of these compound types.

class Async_file_logger;
class Buffer_logger;
class Component;
class Config;
class Logger;
class Log_context;
struct Msg_metadata;
class Ostream_log_msg_writer;
class Simple_ostream_logger;


/* (The @namespace and @brief thingies shouldn't be needed, but some Doxygen bug necessitated them.
 * See flow::util::bind_ns for explanation... same thing here.) */

/**
 * @namespace flow::log::fs
 * @brief Short-hand for `namespace boost::filesystem`.
 */
namespace fs = boost::filesystem;

/**
 * Enumeration containing one of several message severity levels, ordered from highest to
 * lowest.  Generally speaking, volume/verbosity is inversely proportional to severity, though
 * this is not enforced somehow.
 *
 * As the underlying type is `size_t`, and the values are guaranteed to be 0, 1, ... going from lowest
 * to highest verbosity (highest to lowest severity), you may directly use log::Sev values to index into arrays that
 * arrange one-to-one values in the same order.
 *
 * The supplied `ostream<<` operator, together with this `enum`, is suitable for util::istream_to_enum().
 * `ostream>>` operator is built on the latter and is also supplied.  Hence I/O of `Sev` is available out of the box;
 * this enables parsing in boost.program_options among other things.
 *
 * ### Formal semantics of log::Sev ###
 * From the point of view of all Flow code, the *formal* semantics of log::Sev are as follows:
 *   - Sev::S_NONE has value 0 and is a start-sentinel.
 *   - Non-sentinel severities follow, starting at 1 with no gaps.
 *     - Ordinal comparison is meaningful; lower is termed as more severe/less verbose; higher is therefore
 *       termed as less severe/more verbose.  These are, formally, only terminology.
 *   - Sev::S_END_SENTINEL is the end-sentinel.
 *   - Sev::S_WARNING is and always shall be considered the *least severe* "abnormal" condition.  Any additional
 *     "abnormal" conditions shall always have more-severe (numerically lower) values.
 *     As of this writing, within flow::log Flow module, this has only one practical meaning (but that could change):
 *     Simple_ostream_logger directs WARNING-and-more-severe messages to its configured *error* stream (`os_for_err`);
 *     while all the rest are directed to its configured other (regular) stream (`os`).  (For example these might
 *     be set to `std::cerr` and `std::cout` respectively; or both to `std::cout`.)
 *
 * ### Informal semantics of log::Sev ###
 * The following are not enforced by any code logic in the flow::log Flow module.  However we suggest users
 * understand and consider these, as a haphazard approach to severity selection for a given log call site can
 * cause significant operational problems in a high-volume production environment (at least).
 *
 * Generally, there are 2 types of log call sites: ones inside Flow; and ones outside (user code).  The former
 * (inside Flow) are deliberately minimalistic, so as to use as few severities as possible and thus make
 * severity selection maximally simple and, importantly, unambiguous.
 *
 * However, logging code outside Flow may require more sophisticated in-use severity sets.  Informally we *recommend*
 * keeping it as simple as inside-Flow's scheme... if it is sufficient.  If it is not sufficient, the other
 * severities may also be used.
 *
 * The log::Sev values used inside Flow (the *minimalistic* set) are as follows.  (Brief meanings accompany them;
 * see their individual doc headers for more detail.)  From most to least severe:
 *   - Sev::S_FATAL: the program will abort shortly due to the condition that is being logged.  Usually an
 *     `assert(false)` and/or `std::abort()` follows.  If you've disabled abort-on-assertion-trip (`NDEBUG` is defined),
 *     and there is no `std::abort()` or equivalent, then the program may continue, but subsequent behavior is
 *     undefined.
 *   - Sev::S_WARNING: abnormal condition is being logged, and its aggregate volume is not high enough to be classified
 *     as TRACE instead to avoid perf impact.  (Other than possibly FATAL -- used in extreme cases -- no
 *     other abnormal-condition `Sev` are in-use inside Flow nor will they be.)
 *   - Sev::S_INFO: non-abnormal condition is being logged, and it's not so frequent in practice that enabling
 *     this log level shall adversely impact performance.
 *   - Sev::S_TRACE: like INFO, but enabling this log level *can* adversely impact performance.  However, entire
 *     dumps of potentially large (in aggregate) data (such as packet contents) being processed are not included.
 *   - Sev::S_DATA: like TRACE, but entire contents of potentially large (in aggregate) data (such as packet contents)
 *     being processed are included in the message, which may lead to particularly large log output (if enabled).
 *
 * As of this writing the following log::Sev values are available for use (by user/non-Flow code) beyond the above
 * *minimalistic* set.  Potential subjective meanings are included, but user code can use whatever conventions that
 * suit them best.
 *   - Sev::S_ERROR: A non-FATAL abnormal condition subjectively more severe than WARNING.
 *   - Sev::S_DEBUG: A non-abnormal condition with, perhaps, non-perf-affecting verbosity (a-la INFO) but subjectively
 *     of less interest to a human glancing over a large-ish log snippet (a-la TRACE).
 */
enum class Sev : size_t
{
  /**
   * Sentinel log level that must not be specified for any actual message (at risk of undefined behavior such
   * as assert failure); but can be used for sentinel purposes such as specifying a log filter wherein
   * no messages (not even Sev::S_WARNING ones) are shown.
   */
  S_NONE = 0,

  /**
   * Message indicates a "fatally bad" condition, such that the program shall imminently abort, typically due
   * to immediately-following `assert(false)` and possibly `std::abort()`; or if aborts are disabled (such as
   * via defining `NDEBUG`), and there is no explicit `std::abort()` or equivalent, then further
   * program behavior is undefined.
   *
   * @note This severity *is* part of the *minimalistic* (in-use within Flow) severity set as discussed
   *       in log::Sev doc header, "Informal semantics" section.
   */
  S_FATAL,

  /**
   * Message indicates a "bad" condition with "worse" impact than that of Sev::S_WARNING.
   *
   * @note If it's "bad" but frequent enough for TRACE, we strongly recommend to make it TRACE -- not ERROR.
   *
   * @note This severity *is NOT* part of the *minimalistic* (in-use within Flow) severity set as discussed
   *       in log::Sev doc header, "Informal semantics" section.  Informally we recommend projects
   *       do not use it unless it is holistically necessary.
   */
  S_ERROR,

  /**
   * Message indicates a "bad" condition that is not frequent enough to be of severity Sev::S_TRACE.
   * These typically should occur with less frequency than INFO messages; however, it's not a hard rule.
   *
   * @note If it's "bad" but frequent enough for TRACE, it's TRACE by definition -- not WARNING.
   *
   * @note This severity *is* part of the *minimalistic* (in-use within Flow) severity set as discussed
   *       in log::Sev doc header, "Informal semantics" section.
   */
  S_WARNING,

  /**
   * Message indicates a not-"bad" condition that is not frequent enough to be of severity Sev::S_TRACE.
   *
   * @note That is, it's identical to WARNING *except* doesn't represent a "bad" situation.
   *
   * @note This severity *is* part of the *minimalistic* (in-use within Flow) severity set as discussed
   *       in log::Sev doc header, "Informal semantics" section.
   */
  S_INFO,

  /**
   * Message indicates a condition with, perhaps, no significant perf impact if enabled (like Sev::S_INFO)
   * but of subjectively less interest to a human reader than INFO (hence, like Sev::S_TRACE in that respect).
   *
   * @note This severity *is NOT* part of the *minimalistic* (in-use within Flow) severity set as discussed
   *       in log::Sev doc header, "Informal semantics" section.  Informally we recommend projects
   *       do not use it unless it is holistically necessary.
   */
  S_DEBUG,

  /**
   * Message indicates any condition that may occur with great frequency (thus verbose if logged).
   * The line between Sev::S_INFO or Sev::S_WARNING and TRACE is as follows: The former is *not* allowed to classify
   * messages such that in realistic scenarios they would degrade performance, from processor cycles or log file I/O.
   *
   * @see Sev::S_DATA for an even-more-verbose severity, when it's frequent and potentially large in size in aggregate.
   *
   * @warning One MUST be able to set max severity level to INFO and confidently count that logging will not
   * affect performance.
   *
   * @note This severity *is* part of the *minimalistic* (in-use within Flow) severity set as discussed
   *       in log::Sev doc header, "Informal semantics" section.
   */
  S_TRACE,

  /**
   * Message satisfies Sev::S_TRACE description AND contains variable-length structure (like packet, file) dumps.
   * If these are allowed to be logged, resulting log file might be roughly similar to (or even larger than)
   * the data being transmitted by the logging code.  Packet dumps are obvious examples.
   *
   * Note that just because it's a variable-length structure dump doesn't mean it's for Sev::S_DATA severity.
   * If it's not frequent, it's fine for it to even be INFO.  E.g., if I decide to dump every
   * RST packet, it's probably okay as Sev::S_INFO, since RSTs are practically rare;
   * that need not be Sev::S_DATA.  On the other hand, if I dump every DATA packet
   * as anything except Sev::S_DATA severity, then I should be arrested and convicted.
   *
   * When this level is disabled, consider logging a TRACE message instead but summarize the contents you'd
   * dump; e.g., log a hash and/or a size and/or the few first bytes instead of the entire thing.  Something is
   * often better than nothing (but nothing is still safer than the whole thing).
   *
   * @note This severity *is* part of the *minimalistic* (in-use within Flow) severity set as discussed
   *       in log::Sev doc header, "Informal semantics" section.
   */
  S_DATA,

  /// Not an actual value but rather stores the highest numerical payload, useful for validity checks.
  S_END_SENTINEL
}; // enum class Sev

// Free functions.

/**
 * Deserializes a log::Sev from a standard input stream.  Reads up to but not including the next
 * non-alphanumeric-or-underscore character; the resulting string is then mapped to a log::Sev.  If none is
 * recognized, Sev::S_NONE is the result.  The recognized values are:
 *   - "0", "1", ...: Corresponds to the `int` conversion of that log::Sev (e.g., 0 being NONE).
 *   - Case-insensitive encoding of the non-S_-prefix part of the actual log::Sev member; e.g.,
 *     "warning" (or "Warning" or "WARNING" or...) for `S_WARNING`.
 * This enables a few key things to work, including parsing from config file/command line via and conversion from
 * `string` via `boost::lexical_cast`.
 *
 * @param is
 *        Stream from which to deserialize.
 * @param val
 *        Value to set.
 * @return `is`.
 */
std::istream& operator>>(std::istream& is, Sev& val);
// @todo - `@relatesalso Sev` makes Doxygen complain; maybe it doesn't work with `enum class`es like Sev.

/**
 * Serializes a log::Sev to a standard output stream.  The output string is compatible with the reverse
 * `istream>>` operator.
 *
 * @param os
 *        Stream to which to serialize.
 * @param val
 *        Value to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, Sev val);
// @todo - `@relatesalso Sev` makes Doxygen complain; maybe it doesn't work with `enum class`es like Sev.

/**
 * Log_context ADL-friendly swap: Equivalent to `val1.swap(val2)`.
 * @param val1
 *        Object.
 * @param val2
 *        Object.
 */
void swap(Log_context& val1, Log_context& val2);

/**
 * Sets certain `chrono`-related formatting on the given Logger in the current thread that results in a consistent,
 * desirable output of `duration`s and certain types of `time_point`s.  The effect is that of
 * util::beautify_chrono_ostream().
 *
 * @see flow::async in which new threads are set to use this formatting automatically.  However you'll want to
 *      do this explicitly for the startup thread.
 *
 * @param logger_ptr
 *        The Logger to affect in this thread.  Null is allowed; results in no-op.
 */
void beautify_chrono_logger_this_thread(Logger* logger_ptr);

/**
 * Estimate of memory footprint of the given value, including memory allocated on its behalf -- but
 * excluding its shallow `sizeof`! -- in bytes.
 *
 * @param val
 *        Value.
 * @return See above.
 */
size_t deep_size(const Msg_metadata& val);

} // namespace flow::log
