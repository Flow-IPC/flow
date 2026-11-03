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
#include "flow/util/detail/thread_lcl_ptr_impl.hpp"
#include <boost/thread.hpp>
#include <cassert>
#include <cstddef>

namespace flow::util
{

// Implementations.

Thread_local_ptr_cache::Thread_local_ptr_cache() :
  m_per_tlp_key(Unique_id_holder::create_unique_id())
{
  // That's all.  Real action starts, potentially, in reset_release() and friends.
}

Thread_local_ptr_cache::~Thread_local_ptr_cache()
{
  release(); // It checks s_this_thread_global_tl_state_operative.

  /* Reminder: We only worry about *this* thread's release(); other threads' stuff is allowed to stay around,
   * until each thread's deinit phase executes. */
}

Thread_local_ptr_cache::Global_tl_state::Global_tl_state()
{
  // See s_this_thread_global_tl_state_operative doc header; it explains why we do this *here*.
  boost::this_thread::at_thread_exit([]()
  {
    s_this_thread_global_tl_state_operative = false;
  });
}

Thread_local_ptr_cache::Global_tl_state::~Global_tl_state()
{
  /* The following is clearly harmless and clearly not-wrong: If this dtor is running, then obviously m_tlp_states_map
   * cannot be accessed, so obviously the definitional condition "do not access Global_tl_state" becomes
   * true.  And if it is already `false`, then it's a no-op.
   *
   * Is it necessary?  It is moot, per preceding paragraph, but it can't hurt to think about it.
   *   - If the boost::thread-module at_thread_exit() + tsp-cleanup phase runs before thread_local deinit
   *     (boost::thread thread): Flag will already be false.  No-op.  Not necessary.
   *   - If the boost::thread-module at_thread_exit() + tsp-cleanup phase runs after thread_local deinit
   *     (std::thread thread): thread_local deinit might run some dtor which could, e.g., use
   *     Tlp::get() for some entry.  If this flag remained true here, but m_tlp_states_map is destroyed
   *     (which it will be just after this dtor {block} exits), then that Tlp::get() would try to read
   *     from an invalid map.  Undefined behavior.  So yes: necessary in this (contrived but possible) case.
   *     More to the point (contrived or not): would vanilla thread_specific_ptr happen to work OK?  Yes:
   *     it would.  (Relying on that is probably not so smart though -- in a boost::thread the tsp cleanup would
   *     have run already, so only for std::thread could it ever work -- but that's not our code; that's
   *     on the user.  We should just mirror tsp as promised.) */
  s_this_thread_global_tl_state_operative = false;
}


} // namespace flow::util
