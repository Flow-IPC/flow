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

#include "flow/log/detail/log_fwd.hpp"
#include "flow/util/string_ostream.hpp"
#include "flow/util/uniq_id_holder.hpp"
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/io/ios_state.hpp>
#include <boost/move/unique_ptr.hpp>
#include <boost/unordered_map.hpp>
#include <boost/thread.hpp>
#include <string>

namespace flow::log
{

/**
 * Internal flow::log class that facilitates a more efficient way to get util::ostream_op_to_string() behavior
 * by allowing each thread to repeatedly reuse the structures that function creates from scratch on stack each time it
 * is invoked; furthermore each logging entity is allotted a separate such set of structures to enable each entity to
 * not affect the streams of other entities.
 *
 * The problem statement arises from util::ostream_op_to_string()'s only obvious flaw which is performance.
 * One invokes that function and passes a target `std::string` and a sequence of `ostream`-writing operations.  Usually
 * the `string` has just been newly created `{` locally `}`; and the function itself will ultimately create some Boost
 * `iostreams` machinery `{` locally `}` and then use it to efficiently write through that machinery into the `string`.
 * That's all fine, but if the function is very often invoked, it would be nice
 * to instead reuse a global `string` *and* associated machinery between invocations.  (`ostream_op_to_string()` can
 * reuse an existing `string`, but even then it'll need to re-create the `iostreams` machinery each time.)
 * In particular, logging machinery (`FLOW_LOG_*()` especially) indeed very often invokes such behavior and could thus
 * benefit (hence the inspiration behind this class).
 *
 * Well, using global structures is not OK: multiple threads will corrupt any such structures by writing and/or
 * reading concurrently to/from them.  However, if one creates a structure per thread -- a/k/a a thread-local
 * structure -- then that danger goes away completely.  That is the principle behind this class.  Firstly, from the
 * class user's perspective, the class itself is a singleton per thread.  Call get_this_thread_string_appender() to
 * access the current thread's only instance of Thread_local_string_appender.  Now you have a pointer to that
 * object.  Further use is very simple: fresh_appender_ostream() clears the internally stored target string and
 * returns a pointer to an `ostream` object.  Write to this `ostream` like any other `ostream`.  Once done doing so,
 * call target_contents(), which will return a reference to the read-only string which has been appended to with
 * the aforementioned `ostream` writing.  Repeat each time you'd like to write to a string, and then quickly access
 * that string.
 *
 * Update: A further level of indirection/lookup was added when I realized that, for each thread, it's not desirable
 * that logically separate stream writers (typically each allotted its own writing object, e.g., a Logger) all write
 * to one common stream.  Even if their writes are not interleaved within an "atomic" sequence of characters,
 * state-changing `ostream` formatters (`std::hex` and the like) would cause one writer to affect the formatting of
 * another, which is not desired (though maybe not immediately obvious due to formatting being a relatively rarely
 * used feature for many).  Therefore, each writing "entity" is allotted a separate instance of this class, and
 * lookup is (implicitly) via thread ID and (explicitly) via a source object ID which uniquely (over all time)
 * identifies each object.
 *
 *   ~~~
 *   // If you frequently do this, from one or from multiple threads:
 *   class Distinct_writer
 *   {
 *     ...
 *       string output;
 *       // Expensive structure setup occurs before the stream writing can execute.  (Terminating `flush` is assumed.)
 *       util::ostream_op_to_string(&output, "The answer is: [", std::hex, 42, "].");
 *       log_string(output); // Suppose log_string() takes const std::string&.
 *       log_chars(output.c_str()); // Or suppose log_chars() takes const char*.
 *       // std::hex formatting is forgotten, as each util::ostream_op_to_string() creates a new everything.
 *     ...
 *   }
 *
 *   // Then consider replacing it with this (for better performance over time):
 *   class Distinct_writer : private util::Unique_id_holder
 *   {
 *     ...
 *       // Note lookup by `this`: we are also a Unique_id_holder which means this can get our unique ID.
 *       auto const appender = log::Thread_local_string_appender::get_this_thread_string_appender(*this);
 *       // Note added explicit `flush`.
 *       *(appender->fresh_appender_ostream()) << "The answer is: [" << std::hex << 42 << "]." << std::flush;
 *       log_string(appender->target_contents());
 *       log_chars(appender->target_contents().c_str());
 *       // Additional feature: std::hex formatting will persist for further such snippets, for the current thread.
 *     ...
 *   }
 *   ~~~
 *
 * Note, once again, that because get_this_thread_string_appender() returns a thread-local object, it is by definition
 * impossible to corrupt anything inside it due to multiple threads writing to it.  (That is unless, of course, you
 * try passing that pointer to another thread and writing to it there, but that's basically malicious behavior;
 * so don't do that.)
 *
 * ### Thread safety ###
 * Well, see above.  If you use class as prescribed, then it's safe to read and write without locking around
 * object of this class.
 *
 * ### Memory use ###
 * The object itself is not large, storing some streams and ancillary stream-related obejcts.
 * The lookup table just indexes by object ID and thread ID, so a few integers per writing entity per writing thread.
 * The implementation we use ensures that once a given thread disappears, all the data for that thread ID are freed.
 * Let us then discuss, from this point forth, what happens while a given thread is alive.  Namely:
 *
 * There is no way to remove an appender from this table once it has been added which can be seen as a
 * memory leak.  In practice, the original use case of Logger means the number of lookup table entries is not likely
 * to grow large enough to matter -- but extreme scenarios contradicting this estimation may be contrived or occur.
 *
 * In terms of implementation, getting cleanup to work is extremely difficult and possibly essentially impossible.
 * (This is true because it's not possible, without some kind of undocumented and possibly un-portable hackery, to
 * enumerate all the threads from which a given object has created a Thread_local_string_appender.  Even if one could,
 * it is similarly not really possible to do anything about it without entering each such thread, which is quite hard
 * to do elegantly and unintrusively w/r/t to calling code.  Even if one could enter each such thread, some
 * kind of locking would probably need to be added, eliminating the elegance of the basic premise of the class which
 * is that of a thread-local singleton per source object.)  For this reason, adding cleanup is not even listed as a
 * to-do.
 *
 * Since util::Unique_id_holder is unique over all time, not just at any given time, there is no danger that the reuse
 * of some dead object's ID will cause a collision.  Historically this was a problem when we used `this` pointers
 * as IDs (as once an object is gone, its `this` value can be reused by another, new object).
 *
 * ### Implementation notes ###
 * We use boost.thread's `thread_specific_ptr` to implement a lazily initialized per-thread singleton (in which
 * a per-object-ID sub-table of Thread_local_string_appender objects lies).  An alternative implementation would
 * be C++11's built-in `thread_local` keyword, probably with a `unique_ptr` to wrap each sub-table (to allow for
 * lazy initialization instead of thread-startup initialization).  The main reason I chose to keep `thread_specific_ptr`
 * even upon moving from C++03 to C++1x is that we use `boost::thread` -- not `std::thread`.  Experiments show
 * `thread_local` behaves appropriately (crucially, including cleanup on thread exit) even with
 * `boost::thread`, but I don't see this documented anywhere (which doesn't mean it isn't documented), and without
 * that it could be an implementation coincidence as opposed to a formal guarantee.  A secondary reason -- which can
 * be thought of the straw that broke the camel's back in this case, as it is fairly minor -- is that
 * `thread_specific_ptr` provides lazy initialization by default, without needing a `unique_ptr` wrapper;
 * a given thread's `p.get()` returns null the first time it is invoked; and executes `delete p.get();` at thread exit
 * (one need not supply a deleter function, although one could if more complex cleanup were needed).  This is
 * what a default-constructed `unique_ptr` would give us, but we get it for "free" (in the sense that no added code
 * is necessary to achieve the same behavior) with `thread_specific_ptr`.
 */
class Thread_local_string_appender :
  private boost::noncopyable
{
public:
  // Methods.

  /**
   * Returns a pointer to the exactly one Thread_local_string_appender object that is accessible from
   * the current thread for the given source object.  The source object is given by its ID.  The source object
   * can store or contain (or be otherwise mapped to) its own util::Unique_id_holder; and thus it can be
   * any object (e.g., a Logger, in original use case) that desires the use of one distinct (from other
   * objects) but continuous (meaning any stream state including characters output will persist over time)
   * `ostream`.  (The `ostream` is not returned directly but rather as the wrapping Thread_local_string_appender
   * object for that stream.)  Ultimately there is exactly one `ostream` (and Thread_local_string_appender) per
   * (invoking thread T, `source_obj_id`) pair that has invoked the present method so far, including the current
   * invocation itself.
   *
   * @param source_obj_id
   *        An ID attached in a 1-to-1 (over all time until program exit) fashion to the entity (typically, class
   *        instance of any type, e.g., Logger) desiring its own Thread_local_string_appender.
   * @return See above.
   */
  static Thread_local_string_appender* get_this_thread_string_appender(const util::Unique_id_holder& source_obj_id);

  /**
   * Clears the internally stored string (accessible for reading via target_contents()), and returns an
   * output stream writing to which will append to that string.  You must `flush` (or flush any other way like
   * `endl`) the stream in order to ensure characters are actually written to the string.  (I am fairly sure flushing
   * is in fact THE thing that actually writes to the string.)
   *
   * The pointer's value never changes for `*this` object.  This fact is critical when it comes to the logic
   * of sequential save_formatting_state_and_restore_prev() calls, as well as (for example) the `ostream` continuity
   * semantics described in class Logger header (that class uses the present class).
   *
   * Behavior is undefined if, at the time the present method is called, the last `flush` of
   * `*fresh_appender_ostream()` precedes the last writing of actual data to same.  In other words,
   * always `flush` the stream immediately after writing to it, or else who knows what will happen
   * with any un-flushed data when one subsequently calls fresh_appender_ostream(), clearing the string?
   *
   * Behavior is undefined if you call this from any thread other than the one in which
   * get_this_thread_string_appender() was called in order to obtain `this`.
   *
   * @see save_formatting_state_and_restore_prev() is a valuable technique to enable usability in
   *      user-facing APIs that use Thread_local_string_appender within the implementation (notably logging
   *      APIs).  See its doc header before using this method.
   *
   * @return Pointer to `ostream` writing to which (followed by flushing) will append to string that
   *         is accessible via target_contents().
   */
  std::ostream* fresh_appender_ostream();

  /**
   * Same as fresh_appender_ostream() but does not clear the result string, enabling piecemeal writing to
   * that string between clearings of the latter.
   *
   * @return Identical to fresh_appender_ostream()'s return value.
   */
  std::ostream* appender_ostream();

  /**
   * Saves the formatting state of the `ostream` returned by appender_ostream() and sets that same `ostream`
   * to the state saved last time this method was called on `*this`, or at its construction, whichever happened
   * later.  Examples of formatting state are `std::hex` and locale imbuings.
   *
   * This is useful if you tend to follow a pattern like the following
   * macro definition that takes `user_supplied_stream_args` macro argument:
   *
   *   ~~~
   *   *(appender->fresh_appender_ostream())
   *     << __FILE__ << ':' << __LINE << ": " // First standard prefix to identify source of log line...
   *     << user_supplied_stream_args         // ...then user-given ostream args (token may expand to multiple <<s)...
   *     << '\n'                              // ...then any terminating characters...
   *     << std::flush;                       // ...and ensure it's all flushed into `string target_contents()`.
   *   ~~~
   *
   * If this is done repeatedly, then `user_supplied_stream_args` might include formatting changes like `std::hex`.
   * In this example, there is the danger that such an `std::hex` from log statement N would then "infect"
   * the `__LINE__` output from log statement (N + 1), the line # showing up in hex form instead of decimal.
   * However, if you use the present feature, then the problem will not occur.
   * However, to avoid similarly surprising the user, restore their formatting with the same call.
   * The above example would become:
   *
   *   ~~~
   *     auto& os = *appender->fresh_appender_ostream();
   *     os.save_formatting_state_and_restore_prev(); // Restore pristine formatting from constuction time.
   *     os << __FILE__ << ':' << __LINE << ": "; // Can log prefix without fear of surprising formatting.
   *     // (Note that if apply formatters here for more exotic output, undo them before the following call.)
   *     os.save_formatting_state_and_restore_prev(); // Restore formatting from previous user_supplied_stream_args.
   *     os << user_supplied_stream_args
   *        << '\n' // Formatting probably doesn't affect this, so no need to worry about restoring state here.
   *        << std::flush;
   *   ~~~
   *
   * @note Reminder: All access to a `*this` must occur in one thread by definition.  Therefore there are no
   *       thread safety concerns to do with the suggested usage patterns.
   * @note Detail: The formatting features affected are as described in documentation for
   *       `boost::io::basic_ios_all_saver`.  This can be summarized as everything except user-defined formatters.
   */
  void save_formatting_state_and_restore_prev();

  /**
   * Read-only accessor for the contents of the `string`, as written to it since the last fresh_appender_ostream()
   * call, or object construction, whichever occurred later.
   *
   * The reference's value never changes for `*this` object.  The string's value may change depending on
   * whether the user writes to `*fresh_appender_ostream()` (which writes to the string) or calls that
   * method (which clears it).
   *
   * Behavior is undefined if you call this from any thread other than the one in which
   * get_this_thread_string_appender() was called in order to obtain `this`.
   *
   * ### Rationale ###
   * Why return `const string&` instead of util::String_view?  Answer: Same as in doc header of String_ostream::str().
   *
   * @return Read-only reference to `string`.
   */
  const std::string& target_contents() const;

private:

  // Types.

  /**
   * Short-hand for map of a given thread's appender objects indexed by the IDs of their respective source objects.
   * Smart pointers are stored to ensure the Thread_local_string_appender is deleted once removed from such a map.
   * Thus once a per-thread map disappears at thread exit, all the stored objects within are freed also.
   */
  using Source_obj_to_appender_map = boost::unordered_map<util::Unique_id_holder::id_t,
                                                          boost::movelib::unique_ptr<Thread_local_string_appender>>;

  // Constructors/destructor.

  /**
   * Initializes object with an empty string and the streams machinery available to write to that string.
   * Note this is not publicly accessible.
   */
  explicit Thread_local_string_appender();

  // Data.

  /**
   * Thread-local storage for each thread's map storing objects of this class (lazily set to non-null on 1st access).
   * Recall `delete s_this_thread_appender_ptrs.get();` is executed at each thread's exit; so if that is non-null
   * for a given thread, this map is freed at that time.  Since smart pointers to Thread_local_string_appender
   * are stored in the map, the Thread_local_string_appender objects thus stored are also freed at that time.
   */
  static boost::thread_specific_ptr<Source_obj_to_appender_map> s_this_thread_appender_ptrs;

  /// The target string wrapped by an `ostream`.  Emptied at construction and in fresh_appender_ostream() *only*.
  util::String_ostream m_target_appender_ostream;

  /**
   * Stores the `ostream` formatter state from construction time or time of last
   * save_formatting_state_and_restore_prev(), whichever occurred later; clearing (including as part of reassigning)
   * this pointer will invoke the destructor `~ios_all_saver()`, restoring that state to #m_target_appender_ostream.
   *
   * A pointer is used in order to be able to re-construct this at will: `ios_all_saver` does not have a "re-do
   * construction on existing object" API (not that I'm saying it should... less state is good, all else being equal...
   * but I digress).
   *
   * @see save_formatting_state_and_restore_prev() for explanation of the feature enabled by this member.
   */
  boost::movelib::unique_ptr<boost::io::ios_all_saver> m_target_appender_ostream_prev_os_state;
}; // class Thread_local_string_appender

} // namespace flow::log
