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
#include "flow/log/detail/thread_lcl_str_appender.hpp"
#include <boost/move/make_unique.hpp>

namespace flow::log
{

// Static initializations.

boost::thread_specific_ptr<Thread_local_string_appender::Source_obj_to_appender_map>
  Thread_local_string_appender::s_this_thread_appender_ptrs;

// Implementations.

Thread_local_string_appender*
  Thread_local_string_appender::get_this_thread_string_appender(const util::Unique_id_holder& source_obj_id) // Static.
{
  /* It's just like an on-demand singleton -- but per thread.  Precisely because it is per thread, this code is
   * thread-safe unlike a regular on-demand singleton.  Then, after the thread lookup, another lookup obtains the
   * particular source object's dedicated appender (for the current thread).  (Reminder: source_obj_id is unique ID
   * of an object that wants to use appenders.  In theory even a mix of different classes can
   * co-exist in this system, since IDs are unique across all objects of all types.)
   *
   * The crux of it is that the .get() call just below returns a different pointer depending on the current thread;
   * followed by a presumably quick map lookup.  If the performance of .get() is higher than the performance of creating
   * a new string, inserter around string, and stream around inserter, then this whole thing is worth it.  Otherwise it
   * is not.
   *
   * Update: Now it might still be worth it, because the fact that each ostream thus stored maintains a
   * persistent yet independent-from-others formatter state (with the save_formatting_state_and_restore_prev() feature
   * further helping to manage formatting) guarantees convenient, predictable behavior for users of ostream formatters.
   * That is to say, this allows user to (say) set chrono::symbol_format formatter for this thread's logging of
   * durations; yet without concurrently affecting other threads' output of durations; yet also without having re-apply
   * that formatter repeatedly just to keep duration output looking a certain way.  So, desiring persistent formatting
   * for a given stream-writing entity OVER TIME means new-string/inserter-around-string/stream-around-insert being
   * done for each little stream-writing code snippet is insufficient, even if it were computationally as cheap
   * as the following lookup. */

  Source_obj_to_appender_map* appender_ptrs = s_this_thread_appender_ptrs.get();
  if (!appender_ptrs)
  {
    // Uncommon code path.

    appender_ptrs = new Source_obj_to_appender_map;
    s_this_thread_appender_ptrs.reset(appender_ptrs);

    assert(appender_ptrs == s_this_thread_appender_ptrs.get());

    /* Note about cleanup: `delete appender_ptrs;` will be executed by thread_specific_ptr when thread exits.
     * Therefore any per-thread memory is not leaked past the thread's lifetime.  However, for a given thread,
     * per-object-ID memory IS leaked even when the corresponding object disappears; we provide no mechanism
     * to free this.  See class doc header for discussion. */
  }

  Source_obj_to_appender_map& appender_ptrs_map = *appender_ptrs;

  const auto& id = source_obj_id.unique_id();
  auto& appender_ptr_ref = appender_ptrs_map[id]; // (Reference to) smart pointer directly in the map.
  if (!appender_ptr_ref)
  {
    /* Uncommon code path.  appender_ptr (inside the map) was just initialized to null smart pointer.  Un-null it.
     * Because it's a smart pointer, not a raw one, deleting the map will delete the object allocated here. */
    appender_ptr_ref.reset(new Thread_local_string_appender);
  }

  /* You may be rightly alarmed that .get() is used and even returned outside the current block.
   * In this case, it is definitely safe: The underlying pointer is deleted only on .reset() or destruction
   * of this pointer; .reset() is called only once and was just done above; and destruction occurs only
   * when the thread exits.  Since the returned object is to be, by definition (it's in the name!) used only
   * in the current thread, any attempt to use it in another thread is a blatant mis-use and need not be considered.
   * (Why not return a smart pointer anyway?  Well, why impose on outside caller the associated syntactic complexities,
   * when it would be done due to internal implementation details in the first place?  There are other reasons, but
   * that one is enough, even if they didn't exist.) */
  return appender_ptr_ref.get();
}

Thread_local_string_appender::Thread_local_string_appender() :
  m_target_appender_ostream_prev_os_state
    (boost::movelib::make_unique<boost::io::ios_all_saver>(m_target_appender_ostream.os()))
{
  // Nothing else.
}

std::ostream* Thread_local_string_appender::fresh_appender_ostream()
{
  m_target_appender_ostream.str_clear();
  return appender_ostream();
}

std::ostream* Thread_local_string_appender::appender_ostream()
{
  // @todo Maybe we should also expose ostream& instead of pointer, if only for consistency.
  return &(m_target_appender_ostream.os());
}

void Thread_local_string_appender::save_formatting_state_and_restore_prev()
{
  using boost::io::ios_all_saver;

  /* Due to smart pointer mechanics and function call expression evaluation order semantics, this does the following:
   * - Inside reset() parentheses, new ios_all_saver *X is created.
   *   - X->ios_all_saver() runs: the current state of the ostream is saved in *X.
   * - Inside reset() method, the unique_ptr<>'s internal saved pointer is replaced with X.
   *   - Since the previous saved pointer Y is replaced, *Y is destroyed.
   *     - Y->~ios_all_saver() runs: the stated saved at *Y's construction is restored to the ostream.
   *
   * The key insight is that X saved now will be the Y at the present method's next invocation.  Thus the inductive
   * chain is built.  The induction base is that *X is the clean just-constructed ostream's formatting. */
  m_target_appender_ostream_prev_os_state = boost::movelib::make_unique<ios_all_saver>(m_target_appender_ostream.os());
}

const std::string& Thread_local_string_appender::target_contents() const
{
  return m_target_appender_ostream.str();
}

} // namespace flow::log
