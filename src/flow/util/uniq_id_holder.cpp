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
#include "flow/util/uniq_id_holder.hpp"
#include <boost/functional/hash/hash.hpp>
#include <boost/io/ios_state.hpp>
#include <iomanip>
#include <type_traits>

namespace flow::util
{

// Unique_id_holder implementations.

Unique_id_holder::Unique_id_holder() :
  m_id(create_unique_id())
{
  // Nothing else.
}

Unique_id_holder::Unique_id_holder(const Unique_id_holder&) :
  Unique_id_holder() // This (unusual) behavior is explained in class doc header.  Basically new obj = separate obj.
{
  // Nothing else.
}

const Unique_id_holder& Unique_id_holder::operator=(const Unique_id_holder&) const
{
  // Intentionally blank.  This (unusual) behavior is explained in class doc header.
  return *this;
}

Unique_id_holder::id_t Unique_id_holder::unique_id() const
{
  return m_id;
}

Unique_id_holder::id_t Unique_id_holder::create_unique_id() // Static.
{
  using std::atomic;
  using std::memory_order_relaxed;

  /* Get the next available ID in the universe.  Use large type for id_t to avoid overflow.
   * Use atomic<> to ensure thread safety.  Use pre-increment ++x equivalent, so that the smallest possible ID 0 is
   * reserved and never issued (might come in handy for debugging someday, identifying failure of
   * this feature).
   *
   * Use local static; in C++17 this is formally thread-safe; this way the init happens on-demand (no danger of
   * static-init mis-ordering). */
  static atomic<id_t> s_last_id{0};
  return s_last_id.fetch_add(1, memory_order_relaxed) + 1;
}

// Thread_token implementations.

Thread_token this_thread_unique_token()
{
  static_assert(std::is_same_v<uint64_t, Unique_id_holder::id_t>,
                "Thread_token wraps Unique_id_holder-supplied values; this check ensures no surprises: that "
                  "Unique_id_holder issues the type we expect.");

  /* Unique_id_holder::create_unique_id() never returns 0 (its first result is 1); hence default-cted
   * Thread_token (= 0 inside) = not-yet-assigned, as Thread_token doc header promises.
   * Both `static` (inside create_unique_id()) and this `thread_local` are trivially destructible and
   * constant-initialized => usable at any time including during static/TLS teardown as advertised. */
  thread_local Thread_token s_token;
  if (s_token == Thread_token{})
  {
    s_token = Thread_token{Unique_id_holder::create_unique_id()}; // Slow-path.
  }
  // else { Fast-path: no-op. }
  return s_token;
}

std::ostream& operator<<(std::ostream& os, Thread_token val)
{
  using boost::io::ios_all_saver;
  using std::hex;
  using std::setfill;
  using std::setw;

  ios_all_saver saver{os}; // Restore formatting at end of {block}.

  /* Print the low 32 bits only (cf. abbreviated git hashes): the value comes from a monotonic counter, so
   * the high bits are zeros until the 2^32nd ID is issued process-wide -- i.e., in practice always; printing
   * them would be pure noise.  (Display-truncation only: the stored value keeps all 64 bits, and code
   * needing the full value printed can cast: `os << Thread_token::id_t(val)`.) */
  static_assert(sizeof(uint32_t) == 4, "4 bytes <=> 8 hex-chars.");
  return os << "0x" << hex << setfill('0') << setw(8) << uint32_t(Thread_token::id_t(val));
}

size_t hash_value(Thread_token val)
{
  using boost::hash;
  return hash<Thread_token::id_t>()(val);
}

} // namespace flow::util
