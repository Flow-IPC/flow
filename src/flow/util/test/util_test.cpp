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

#include "flow/util/util.hpp"
#include "flow/util/action_registry.hpp"
#include <boost/unordered_set.hpp>
#include <boost/thread/thread.hpp>
#include <gtest/gtest.h>
#include <optional>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <iostream>
#include <deque>
#include <vector>

namespace flow::util::test
{

namespace
{
using std::set;
using std::string;
using std::cout;
using std::flush;
using std::deque;
using std::vector;
} // Anonymous namespace

TEST(Util_scoped_setter, Interface)
{
  string str;
  int num = 0;
  EXPECT_TRUE(str.empty());
  EXPECT_EQ(num, 0);
  {
    Scoped_setter<string> set{&str, "abc"};
    Scoped_setter<int> set2{&num, 10};
    EXPECT_EQ(str, "abc");
    EXPECT_EQ(num, 10);
    {
      // Test out Scoped_setter move ctor... in 1-2 ways (return, direct).
      auto set2
        = ([&]() -> auto { return std::make_unique<Scoped_setter<int>>(&num, 5); })
          ();
      auto set
        = ([&]() -> auto { return std::make_unique<Scoped_setter<string>>(&str, "def"); })
          ();
      Scoped_setter<int> set2_a{std::move(*set2)};
      Scoped_setter<string> set_a{std::move(*set)};
      EXPECT_EQ(str, "def");
      EXPECT_EQ(num, 5);
      set2.reset(); // Destroy the moved-from guys -- this should have no effect.
      set.reset();
      EXPECT_EQ(str, "def");
      EXPECT_EQ(num, 5);
    }
    EXPECT_EQ(str, "abc");
    EXPECT_EQ(num, 10);
  }
  EXPECT_TRUE(str.empty());
  EXPECT_EQ(num, 0);
} // TEST(Util_scoped_setter, Interface)

TEST(Util_misc, Interface)
{
  // ostream_op_string() is built on top of vaious things, so this alone is a pretty decent check.  @todo More.
  EXPECT_EQ(ostream_op_string("abc[", 2, "] flag[", true, "]:", std::hex, 12),
            "abc[2] flag[1]:c");

  EXPECT_EQ(ceil_div(0, 1024), 0);
  EXPECT_EQ(ceil_div(1, 1024u), 1);
  EXPECT_EQ(ceil_div(2, 1024l), 1);
  EXPECT_EQ(ceil_div(1023, 1024ul), 1);
  EXPECT_EQ(ceil_div(1024, 1024ll), 1);
  EXPECT_EQ(ceil_div(1025, 1024ull), 2);
  EXPECT_EQ(ceil_div(1026, 1024ll), 2);
  EXPECT_EQ(ceil_div(2047, 1024ul), 2);
  EXPECT_EQ(ceil_div(2048, 1024l), 2);
  EXPECT_EQ(ceil_div(2049, 1024u), 3);
  EXPECT_EQ(ceil_div(2050, 1024), 3);
  EXPECT_EQ(ceil_div(8192, 1024u), 8);
  EXPECT_EQ(ceil_div(8193, 1024l), 9);

  EXPECT_EQ(round_to_multiple(0, 1024), 0 * 1024);
  EXPECT_EQ(round_to_multiple(1, 1024u), 1 * 1024);
  EXPECT_EQ(round_to_multiple(2, 1024l), 1 * 1024);
  EXPECT_EQ(round_to_multiple(1023, 1024ul), 1 * 1024);
  EXPECT_EQ(round_to_multiple(1024, 1024ll), 1 * 1024);
  EXPECT_EQ(round_to_multiple(1025, 1024ull), 2 * 1024);
  EXPECT_EQ(round_to_multiple(1026, 1024ll), 2 * 1024);
  EXPECT_EQ(round_to_multiple(2047, 1024ul), 2 * 1024);
  EXPECT_EQ(round_to_multiple(2048, 1024l), 2 * 1024);
  EXPECT_EQ(round_to_multiple(2049, 1024u), 3 * 1024);
  EXPECT_EQ(round_to_multiple(2050, 1024), 3 * 1024);
  EXPECT_EQ(round_to_multiple(8192, 1024u), 8 * 1024);
  EXPECT_EQ(round_to_multiple(8193, 1024l), 9 * 1024);
} // TEST(Util_misc, Interface)

TEST(Util_alignment, Is_power_of_two)
{
  // Powers of two.
  EXPECT_TRUE((is_power_of_two<1>()));
  EXPECT_TRUE((is_power_of_two<2>()));
  EXPECT_TRUE((is_power_of_two<4>()));
  EXPECT_TRUE((is_power_of_two<8>()));
  EXPECT_TRUE((is_power_of_two<16>()));
  EXPECT_TRUE((is_power_of_two<64>()));
  EXPECT_TRUE((is_power_of_two<1024>()));
  EXPECT_TRUE((is_power_of_two<(uint64_t(1) << 32)>()));
  EXPECT_TRUE((is_power_of_two<(uint64_t(1) << 63)>()));

  // Non-powers of two.
  EXPECT_FALSE((is_power_of_two<3>()));
  EXPECT_FALSE((is_power_of_two<5>()));
  EXPECT_FALSE((is_power_of_two<6>()));
  EXPECT_FALSE((is_power_of_two<7>()));
  EXPECT_FALSE((is_power_of_two<9>()));
  EXPECT_FALSE((is_power_of_two<15>()));
  EXPECT_FALSE((is_power_of_two<100>()));
  EXPECT_FALSE((is_power_of_two<1023>()));
  EXPECT_FALSE((is_power_of_two<1025>()));
  // static_assert(POSITIVE_INT > 0) prevents is_power_of_two<0>() from compiling, which is correct.
} // TEST(Util_alignment, Is_power_of_two)

TEST(Util_alignment, Max_align_sz)
{
  constexpr auto ALIGN = max_align_sz(); // Check it's constexpr.
  cout << "Most stringent alignment in this architecture: [" << ALIGN << "].\n" << flush;

  EXPECT_EQ(max_align_sz(), alignof(std::max_align_t));
  EXPECT_TRUE((is_power_of_two<alignof(std::max_align_t)>()));
  EXPECT_GE(max_align_sz(), sizeof(void*)); // Sanity: at least pointer-sized.
} // TEST(Util_alignment, Max_align_sz)

TEST(Util_alignment, Aligned_sz_of)
{
  // Alignment 1: no padding, just sizeof.
  EXPECT_EQ((aligned_sz_of<uint8_t, 1>()), sizeof(uint8_t));
  EXPECT_EQ((aligned_sz_of<uint32_t, 1>()), sizeof(uint32_t));
  EXPECT_EQ((aligned_sz_of<uint64_t, 1>()), sizeof(uint64_t));

  // Alignment to own size: trivial for fundamental types.
  EXPECT_EQ((aligned_sz_of<uint8_t, 1>()), 1);
  EXPECT_EQ((aligned_sz_of<uint16_t, 2>()), 2);
  EXPECT_EQ((aligned_sz_of<uint32_t, 4>()), 4);
  EXPECT_EQ((aligned_sz_of<uint64_t, 8>()), 8);

  // Padding scenarios.
  EXPECT_EQ((aligned_sz_of<uint8_t, 4>()), 4);  // 1 byte rounded up to 4.
  EXPECT_EQ((aligned_sz_of<uint8_t, 16>()), 16); // 1 byte rounded up to 16.
  EXPECT_EQ((aligned_sz_of<uint32_t, 16>()), 16); // 4 bytes rounded up to 16.

  // Exact fit: sizeof already a multiple.
  EXPECT_EQ((aligned_sz_of<uint64_t, 4>()), 8); // 8 is already a multiple of 4.

  // struct with odd size: array<uint8_t, 3> has size 3.
  using Three_bytes = std::array<uint8_t, 3>;
  static_assert(sizeof(Three_bytes) == 3, "");
  EXPECT_EQ((aligned_sz_of<Three_bytes, 1>()), 3);
  EXPECT_EQ((aligned_sz_of<Three_bytes, 2>()), 4);
  EXPECT_EQ((aligned_sz_of<Three_bytes, 4>()), 4);
  EXPECT_EQ((aligned_sz_of<Three_bytes, 8>()), 8);
  EXPECT_EQ((aligned_sz_of<Three_bytes, 16>()), 16);

  // Default ALIGN_SZ (max_align_sz()).
  EXPECT_EQ((aligned_sz_of<uint8_t>()), max_align_sz());
  EXPECT_EQ((aligned_sz_of<uint64_t>()), max_align_sz()); // 8 rounds up to 16 on x86-64.
  /* (Some stuff could fail if we target different arch, but that'll be self-correcting: we'll see it then and fix
   * the test to handle such differences.) */
} // TEST(Util_alignment, Aligned_sz_of)

TEST(Util_alignment, Prefix_before_and_after)
{
  // Allocate a buffer big enough to hold a prefix + data, both aligned.
  // Then verify roundtrip: aligned_prefix_before(data) -> prefix, after_aligned_prefix(prefix) -> data.

  constexpr size_t ALIGN = max_align_sz();
  using Prefix = uint64_t;
  using Data = uint32_t;
  constexpr size_t PFX_SZ = aligned_sz_of<Prefix, ALIGN>();
  constexpr size_t DATA_SZ = sizeof(Data);

  // Use aligned_alloc to guarantee alignment.
  const size_t total = PFX_SZ + DATA_SZ;
  auto* const buf = static_cast<uint8_t*>(std::aligned_alloc(ALIGN, total));
  ASSERT_NE(buf, nullptr);

  // Prefix at the start; data after the prefix area.
  auto* const pfx_ptr = reinterpret_cast<Prefix*>(buf);
  auto* const data_ptr = reinterpret_cast<Data*>(buf + PFX_SZ);

  // Roundtrip: from data_ptr, find the prefix; should be pfx_ptr.
  EXPECT_EQ((aligned_prefix_before<Prefix, Data, ALIGN>(data_ptr)), pfx_ptr);

  // Roundtrip: from pfx_ptr, find the data; should be data_ptr.
  EXPECT_EQ((after_aligned_prefix<Data, Prefix, ALIGN>(pfx_ptr)), data_ptr);

  // Full roundtrip composition.
  EXPECT_EQ((after_aligned_prefix<Data, Prefix, ALIGN>(aligned_prefix_before<Prefix, Data, ALIGN>(data_ptr))),
            data_ptr);
  EXPECT_EQ((aligned_prefix_before<Prefix, Data, ALIGN>(after_aligned_prefix<Data, Prefix, ALIGN>(pfx_ptr))),
            pfx_ptr);

  // Actually store a value in the prefix and read it back.
  *aligned_prefix_before<Prefix, Data, ALIGN>(data_ptr) = 0xDEADBEEF'CAFEBABE;
  EXPECT_EQ(*pfx_ptr, 0xDEADBEEF'CAFEBABE);

  // const overloads: same results.
  const auto* const const_data_ptr = const_cast<const Data*>(data_ptr);
  const auto* const const_pfx_ptr = const_cast<const Prefix*>(pfx_ptr);
  EXPECT_EQ((aligned_prefix_before<Prefix, Data, ALIGN>(const_data_ptr)), const_pfx_ptr);
  EXPECT_EQ((after_aligned_prefix<Data, Prefix, ALIGN>(const_pfx_ptr)), const_data_ptr);

  // void* variant: data_ptr as void*.
  auto* const data_void_ptr = static_cast<void*>(data_ptr);
  EXPECT_EQ((aligned_prefix_before<Prefix, void, ALIGN>(data_void_ptr)), pfx_ptr);
  EXPECT_EQ(static_cast<void*>(after_aligned_prefix<void, Prefix, ALIGN>(pfx_ptr)), data_void_ptr);

  // Default ALIGN_SZ (max_align_sz()) variant -- same as explicit ALIGN on this platform.
  EXPECT_EQ((aligned_prefix_before<Prefix>(data_ptr)),
            (aligned_prefix_before<Prefix, Data, ALIGN>(data_ptr)));
  EXPECT_EQ((after_aligned_prefix<Data>(pfx_ptr)),
            (after_aligned_prefix<Data, Prefix, ALIGN>(pfx_ptr)));

  std::free(buf);
} // TEST(Util_alignment, Prefix_before_and_after)

TEST(Util_alignment, Prefix_before_non_default_align)
{
  // Use ALIGN = 8, which differs from the default max_align_sz() (16 on x86-64).
  constexpr size_t ALIGN = 8;
  static_assert(ALIGN != max_align_sz(),
                "This test is trying to test not-the-most-stringent alignment; apparently for this arch our chosen "
                  "value *is* the most stringent.  Tweak test to cover relevant architectures.");

  using Prefix = std::array<uint8_t, 3>; // sizeof = 3, aligned_sz_of<Prefix, 8> = 8.
  using Data = uint64_t;
  constexpr size_t PFX_SZ = aligned_sz_of<Prefix, ALIGN>();
  EXPECT_EQ(PFX_SZ, size_t(8)); // 3 rounds up to 8.

  const size_t total = PFX_SZ + sizeof(Data);
  auto* const buf = static_cast<uint8_t*>(std::aligned_alloc(ALIGN, total));
  ASSERT_NE(buf, nullptr);

  auto* const pfx_ptr = reinterpret_cast<Prefix*>(buf);
  auto* const data_ptr = reinterpret_cast<Data*>(buf + PFX_SZ);

  // Non-const overloads.
  EXPECT_EQ(reinterpret_cast<uint8_t*>(aligned_prefix_before<Prefix, Data, ALIGN>(data_ptr)), buf);
  EXPECT_EQ((after_aligned_prefix<Data, Prefix, ALIGN>(pfx_ptr)), data_ptr);

  // const overloads.
  const auto* const const_data_ptr = const_cast<const Data*>(data_ptr);
  const auto* const const_pfx_ptr = const_cast<const Prefix*>(pfx_ptr);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(aligned_prefix_before<Prefix, Data, ALIGN>(const_data_ptr)), buf);
  EXPECT_EQ((after_aligned_prefix<Data, Prefix, ALIGN>(const_pfx_ptr)), const_data_ptr);

  std::free(buf);
} // TEST(Util_alignment, Prefix_before_non_default_align)

TEST(Util_action_registry, Single_arg_multi_action)
{
  /* Register 2 actions on a single-arg registry; execute(); verify both fired with the right value.
   * Then execute() again with a different value to confirm the registry persists. */

  struct Tag {};
  using Reg = Action_registry<Tag, int>;

  int result1 = 0;
  int result2 = 0;

  Reg::register_action([&](int val) { result1 = val; });
  Reg::register_action([&](int val) { result2 = val * 2; });

  Reg::execute(42);
  EXPECT_EQ(result1, 42);
  EXPECT_EQ(result2, 84);

  Reg::execute(10);
  EXPECT_EQ(result1, 10);
  EXPECT_EQ(result2, 20);
} // TEST(Util_action_registry, Single_arg_multi_action)

TEST(Util_action_registry, Multi_arg_with_pointers)
{
  /* Register 1 action on a 2-arg registry: a scalar and a pointer-to-const.
   * Verify both args arrive correctly. */

  struct Widget { int m_val; };
  struct Tag {};
  using Reg = Action_registry<Tag, int, const Widget*>;

  int got_n = 0;
  const Widget* got_w = nullptr;

  Reg::register_action([&](int n, const Widget* w) { got_n = n; got_w = w; });

  const Widget widget{77};
  Reg::execute(5, &widget);
  EXPECT_EQ(got_n, 5);
  ASSERT_NE(got_w, nullptr);
  EXPECT_EQ(got_w->m_val, 77);
} // TEST(Util_action_registry, Multi_arg_with_pointers)

TEST(Util_action_registry, No_actions_registered)
{
  /* execute() on a registry with nothing registered is a no-op (doesn't crash). */

  struct Tag {};
  using Reg = Action_registry<Tag, int>;

  Reg::execute(999); // Should not crash.
} // TEST(Util_action_registry, No_actions_registered)

/* this_thread_unique_token() (the thread-token): never-reused-for-the-process-life per-thread ID.
 * The distinctness-despite-Thread_id-recycling case at the end is the function's whole reason to exist. */
TEST(Util_this_thread_unique_token, Uniqueness)
{
  // Stable and non-"none" within a given thread.
  const auto token = this_thread_unique_token();
  EXPECT_NE(token, flow::util::Thread_token{});
  EXPECT_NE(token, 0); // Same thing, via the implicit int conversion.
  EXPECT_EQ(this_thread_unique_token(), token);
  { /* And the fixed-width-hex printing: "0x" + 8 zero-padded digits (low 32 bits only), always.
     * Also: stream formatting must be restored (42 must not print in hex).  Yes, o_o_s() uses an ostream; the
     * commas become <<es, so to speak, is all. */
    const auto str = ostream_op_string(token, '|', 42);
    EXPECT_EQ(str.size(), 2 + 8 + 1 + 2);
    EXPECT_EQ(str.rfind("0x", 0), 0);
    EXPECT_EQ(str.substr(2 + 8), "|42");
    cout << "Token looks like this: [" << token << "].\n" << flush;
  }

  // Distinct across concurrently-live threads (including from ours).
  constexpr size_t N_CONCURRENT = 4;
  vector<uint64_t> tokens(N_CONCURRENT);
  {
    vector<Thread> threads;
    for (size_t idx = 0; idx != N_CONCURRENT; ++idx)
    {
      threads.emplace_back([idx, &tokens]() { tokens[idx] = this_thread_unique_token(); });
    }
    for (auto& thread : threads)
    {
      thread.join();
    }
  }
  set<uint64_t> uniques{tokens.begin(), tokens.end()};
  uniques.insert(token);
  EXPECT_EQ(uniques.size(), N_CONCURRENT + 1);
  { // hash_value(): Thread_token works as a hashed-container key out of the box.
    boost::unordered_set<flow::util::Thread_token> token_set;
    for (const auto tok : tokens)
    {
      token_set.insert(flow::util::Thread_token{tok});
    }
    token_set.insert(token);
    token_set.insert(token); // Dupe: no effect.
    EXPECT_EQ(token_set.size(), N_CONCURRENT + 1);
  }

  /* The headline property -- the one Thread_id lacks: sequentially created-and-joined threads all get
   * distinct tokens, even when the OS recycles their Thread_ids (which glibc does readily: create-join-create
   * commonly yields the identical pthread_t).  Token distinctness is asserted; whether Thread_id recycling
   * actually occurred is recorded informationally (platform-dependent -- typically >= 1 on glibc). */
  constexpr size_t N_SEQUENTIAL = 8;
  set<Thread_id> thread_ids;
  size_t n_thread_id_reuses = 0;
  for (size_t idx = 0; idx != N_SEQUENTIAL; ++idx)
  {
    uint64_t seq_token = 0;
    Thread_id thread_id;
    Thread thread{[&]()
    {
      seq_token = this_thread_unique_token();
      thread_id = this_thread::get_id();
    }};
    thread.join();
    n_thread_id_reuses += (thread_ids.insert(thread_id).second ? 0 : 1);
    EXPECT_NE(seq_token, 0);
    EXPECT_TRUE(uniques.insert(seq_token).second); // Never seen before -- across all the threads above, too.
  }
  cout << "thread-token: sequential create-join threads = [" << N_SEQUENTIAL << "]; "
          "Thread_id reuses observed among them = [" << n_thread_id_reuses << "].\n";
} // TEST(Util_this_thread_unique_token, Uniqueness)

// For the Locked_proxy test: something with a const method, a non-const method, and public data.
struct Locked_widget
{
  int m_val = 0;

  int get_and_bump() { return m_val++; }
  int peek() const { return m_val; }
};

/* Locked_proxy (execute-around pointer): the lock is held exactly while a proxy exists -- per-expression in
 * the typical unnamed-temporary use -- and ownership travels via move, unlocking exactly once. */
TEST(Util_locked_proxy, Interface)
{
  using Proxy = Locked_proxy<Locked_widget, Mutex_non_recursive>;

  Mutex_non_recursive m;
  Locked_widget w;
  const auto w_locked = [&]() { return Proxy{&w, &m}; };

  /* Lock-state probe.  Must run in a separate thread: try_lock() from the mutex-holding thread itself is
   * formally undefined for a non-recursive mutex. */
  const auto locked = [&]() -> bool
  {
    bool result;
    boost::thread{[&]()
    {
      result = !m.try_lock();
      if (!result)
      {
        m.unlock();
      }
    }}.join();
    return result;
  };

  // The typical use: an unnamed temporary => locked for exactly the expression.
  EXPECT_EQ(w_locked()->get_and_bump(), 0);
  EXPECT_EQ(w_locked()->peek(), 1);
  EXPECT_FALSE(locked());

  { // A named proxy: locked while it lives.
    const auto px = w_locked();
    EXPECT_TRUE(locked());
    EXPECT_EQ((*px).m_val, 1); // operator*: reference access.
    px->m_val = 42;
  }
  EXPECT_FALSE(locked());
  EXPECT_EQ(w.m_val, 42);

  { // Move: ownership travels; the moved-from's destruction must NOT unlock; the moved-to's must (exactly once).
    std::optional<Proxy> px_moved_to;
    {
      auto px = w_locked();
      px->m_val = 43;
      px_moved_to.emplace(std::move(px));
    } // px (moved-from) destroyed here...
    EXPECT_TRUE(locked()); // ...and indeed the mutex remains locked, held by the moved-to.
    EXPECT_EQ((*px_moved_to)->m_val, 43);
    px_moved_to.reset();
  }
  EXPECT_FALSE(locked());

  // The read-only form: const Target => only const members accessible.
  const auto w_locked_const = [&]() { return Locked_proxy<const Locked_widget, Mutex_non_recursive>{&w, &m}; };
  EXPECT_EQ(w_locked_const()->peek(), 43);
  // (w_locked_const()->get_and_bump() would not compile -- which is the feature.)
} // TEST(Util_locked_proxy, Interface)

} // namespace flow::util::test
