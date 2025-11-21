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

#include "flow/util/blob.hpp"
#include "flow/log/buffer_logger.hpp"
#include <gtest/gtest.h>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <optional>
#include <algorithm>
#include <ostream>
#include <memory>

namespace flow::util::test
{

namespace
{

using boost::asio::const_buffer;
using boost::asio::mutable_buffer;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::optional;
using std::all_of;
using std::cout;
using std::flush;
using std::vector;
using std::allocator_traits;
using std::fill_n;
namespace bipc = boost::interprocess;

/* Basic_blob supports SHM-friendly allocators, and we test this to some extent.  The testing just
 * tests the ability to use those allocators, by using boost.interprocess's `allocator` in some of the sub-runs;
 * as well as some specific stateful-allocator-based checks when a given blob type supports it (see `using Blob_t = `).
 *
 * We *could* actually fork a process and try sharing the memory realistically, but this isn't a SHM test per se;
 * rather it's about (1) SHM-*friendly* allocators being properly handled (mostlly not using raw pointers but
 * Allocator::pointer, inside Basic_blob that is); and (2) specifically *stateful* allocators (e.g.,
 * std::allocator and even Flow-IPC's Stateless_allocator are stateless -- we the former too, naturally).
 *
 * So we do use some SHM-pool-based bipc `allocator`s, but we don't go so far as to communicate between 2+ processes. */
using Shm_pool = bipc::managed_shared_memory;
using Shm_allocator = bipc::allocator<uint8_t, Shm_pool::segment_manager>;

using Vanilla_allocator = std::allocator<uint8_t>;

template<typename Blob_t, typename Allocator_t, typename... Ctor_args>
Blob_t make_blob([[maybe_unused]] const Allocator_t* alloc_if_applicable,
                 log::Logger* logger, Ctor_args&&... ctor_args)
{
  constexpr bool HAS_LOG_CTX = std::is_same_v<Blob_t, Blob_with_log_context<Blob_t::S_SHARING>>;

  if constexpr(HAS_LOG_CTX) // Also means does *not* take an Allocator arg (forces std::allocator).
  {
    return Blob_t{logger, std::forward<Ctor_args>(ctor_args)...};
  }
  else
  {
    assert(alloc_if_applicable);
    return Blob_t{std::forward<Ctor_args>(ctor_args)..., logger, *alloc_if_applicable};
  }
}

} // Anonymous namespace

// Yes... this is very cheesy... but this is a test, so I don't really care.
#define CTX ostream_op_string("Caller context [", FLOW_UTIL_WHERE_AM_I_STR(), "].")

TEST(Blob, Interface) // Note that other test-cases specifically test SHARING=true and fancy-allocator support.
{
  using std::swap; // This enables proper ADL.

  /* In this test-case we don't actually share memory between processes, but in some cases we do allocate
   * from a SHM-pool, so set one up (for this process only), ensuring it is cleaned-up at the start and the end. */
  struct Shm_remove
  {
    Shm_remove() { bipc::shared_memory_object::remove("FlowUnitTestBlobInterface");
                   bipc::shared_memory_object::remove("FlowUnitTestBlobInterface2"); }
    ~Shm_remove() { bipc::shared_memory_object::remove("FlowUnitTestBlobInterface");
                    bipc::shared_memory_object::remove("FlowUnitTestBlobInterface2"); }
  } remover;
  Shm_pool shm_pool{bipc::create_only, "FlowUnitTestBlobInterface", 64 * 1024 * 1024};
  Shm_pool shm_pool2{bipc::create_only, "FlowUnitTestBlobInterface2", 64 * 1024 * 1024};

  const auto test_type = [&](auto type_specimen)
  {
    /* Is either Basic_blob<...> or Blob_with_log_context<...>; and further variation is possible due to <...>
     * Much of the API is identical among them but not all of it.
     *
     * In most (not all) of this lambda we don't specifically test APIs enabled by SHARING=true, but the common API's
     * impl still somewhat differs internally depending on SHARING; so it makes sense to invoke us for both
     * situations, even if sharing itself is not being tested.
     *
     * Regarding logging: We don't intend to check the actual log output, if any, but we do want to ensure
     * that enabling logging (non-null Logger) doesn't cause stability problems for example.  There's further
     * differentiation logging-wise; if !HAS_LOG_CTX then some APIs (not just ctor) take Logger* which has effect
     * only ~within that method; else ctor takes Logger*, and it is memorized in that object.  We choose to test
     * both but "cheat" by knowing that HAS_LOG_CTX-case internally is implemented in terms of the !HAS_LOG_CTX
     * case (Blob_with_log_context derives from Basic_blob and mainly uses the aforementioned Logger*-taking APIs
     * from that point on, wrapping them).  Since we know that:
     *   - When constructing a Blob_t, we'll use the proper API and pass it a Logger.
     *     - If HAS_LOG_CTX, then indirectly the !HAS_LOG_CTX case is tested too, fairly fully -- not just
     *       the logging by the ctor.
     *   - When calling other APIs of Blob_t, we do *not* pass a Logger*, even if !HAS_LOG_CTX.  While formally
     *     this omits testing of that case, it still gets decent coverage in reality (as of this writing anyway)
     *     due to the preceding bullet.  Meanwhile the test-code is less tedious.  @todo Maybe reconsider. */
    using Blob_t = decltype(type_specimen);
    constexpr bool SHARING = Blob_t::S_SHARING;
    [[maybe_unused]] constexpr bool HAS_LOG_CTX = std::is_same_v<Blob_t, Blob_with_log_context<SHARING>>;
    constexpr bool SHM_ALLOC = !Blob_t::S_IS_VANILLA_ALLOC;
    using Allocator = typename Blob_t::Allocator_raw;

    cout << "Testing type [" << typeid(Blob_t).name() << "].\n" << flush;

    log::Config log_config;
    log::Buffer_logger logger{&log_config};

    optional<Allocator> alloc_v;
    if constexpr(SHM_ALLOC) { alloc_v.emplace(shm_pool.get_segment_manager()); } else { alloc_v.emplace(); }
    // For tests of 2 `Blob_t`s, each allocating with actually-different allocator (if SHM_ALLOC).
    optional<Allocator> alloc2_v;
    if constexpr(SHM_ALLOC) { alloc2_v.emplace(shm_pool2.get_segment_manager()); } else { alloc2_v.emplace(); }
    const Allocator* const alloc = &(*alloc_v);
    const Allocator* const alloc2 = &(*alloc2_v);

    constexpr size_t N_SM = 1024;
    ASSERT_EQ(N_SM % 2, size_t(0)) << "We shall be dividing it by 2; should probably be even.";
    constexpr size_t ZERO = 0;

    const uint8_t ONE = 1;

    constexpr auto CH = char(ONE);
    const string STRING(37, CH);
    const const_buffer STR_BUF{STRING.data(), STRING.size()};
    const string STRING_SM(14, '\0');
    const const_buffer STR_SM_BUF{STRING_SM.data(), STRING_SM.size()};

    const auto RNG_ZERO_FN = [](const auto& it1, const auto& it2) -> auto
    {
      return all_of(it1, it2, [](uint8_t x) -> auto { return x == uint8_t(0); });
    };
    const auto ALL_ZERO_FN = [&](const auto& blob) -> auto
    {
      return RNG_ZERO_FN(blob.begin(), blob.end());
    };
    const auto RNG_ONES_FN = [](const auto& it1, const auto& it2) -> auto
    {
      return all_of(it1, it2, [](uint8_t x) -> auto { return x == ONE; });
    };

    cout << "  General tests....\n" << flush;

    { // Null blobs.
      auto b1 = make_blob<Blob_t>(alloc, &logger, ZERO);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.resize(0);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.resize(0, Blob_t::S_UNCHANGED);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.resize(0, size_t(0));
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.resize(0, CLEAR_ON_ALLOC);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.resize(0, CLEAR_ON_ALLOC, Blob_t::S_UNCHANGED);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.resize(0, CLEAR_ON_ALLOC, size_t(0));
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.make_zero();
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.reserve(0);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.reserve(0, CLEAR_ON_ALLOC);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.start_past_prefix(0);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      b1.start_past_prefix_inc(0);
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));

      Blob_t b2{b1}; // Copy-ct.
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      EXPECT_TRUE(b2.zero()); EXPECT_TRUE(b2.empty());
      EXPECT_EQ(b2.size(), size_t(0)); EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), size_t(0));
      b1 = b2; // Copy-assign.
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      EXPECT_TRUE(b2.zero()); EXPECT_TRUE(b2.empty());
      EXPECT_EQ(b2.size(), size_t(0)); EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), size_t(0));
      Blob_t b3{std::move(b2)}; // Move-ct.
      EXPECT_TRUE(b3.zero()); EXPECT_TRUE(b3.empty());
      EXPECT_EQ(b3.size(), size_t(0)); EXPECT_EQ(b3.start(), size_t(0)); EXPECT_EQ(b3.capacity(), size_t(0));
      EXPECT_TRUE(b2.zero()); EXPECT_TRUE(b2.empty());
      EXPECT_EQ(b2.size(), size_t(0)); EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), size_t(0));
      b2 = std::move(b3); // Move-assign.
      EXPECT_TRUE(b3.zero()); EXPECT_TRUE(b3.empty());
      EXPECT_EQ(b3.size(), size_t(0)); EXPECT_EQ(b3.start(), size_t(0)); EXPECT_EQ(b3.capacity(), size_t(0));
      EXPECT_TRUE(b2.zero()); EXPECT_TRUE(b2.empty());
      EXPECT_EQ(b2.size(), size_t(0)); EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), size_t(0));
      swap(b2, b3);
      EXPECT_TRUE(b3.zero()); EXPECT_TRUE(b3.empty());
      EXPECT_EQ(b3.size(), size_t(0)); EXPECT_EQ(b3.start(), size_t(0)); EXPECT_EQ(b3.capacity(), size_t(0));
      EXPECT_TRUE(b2.zero()); EXPECT_TRUE(b2.empty());
      EXPECT_EQ(b2.size(), size_t(0)); EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), size_t(0));
    } // Null blobs.

    // We can now generally perhaps ignore null blobs when testing construct/assign ops.

    { // Copy-ct.
      auto b1 = make_blob<Blob_t>(alloc, &logger, N_SM, CLEAR_ON_ALLOC);
      EXPECT_FALSE(b1.zero()); EXPECT_FALSE(b1.empty());
      EXPECT_EQ(b1.size(), N_SM); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b1));
      Blob_t b2{b1};
      EXPECT_FALSE(b1.zero()); EXPECT_FALSE(b1.empty());
      EXPECT_EQ(b1.size(), N_SM); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b1));
      EXPECT_FALSE(b2.zero()); EXPECT_FALSE(b2.empty());
      EXPECT_EQ(b2.size(), N_SM); EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b2));
      EXPECT_NE(b1.data(), b2.data());

      b2.resize(b2.size() / 2, b2.size() / 2);
      Blob_t b3{b2};
      EXPECT_FALSE(b3.zero()); EXPECT_FALSE(b3.empty());
      EXPECT_EQ(b3.size(), N_SM / 2);
      // Attn: Only [.b(), .e()) range copied; start() shall be zero, and capacity() big-enough for size() only.
      EXPECT_EQ(b3.start(), size_t(0)); EXPECT_EQ(b3.capacity(), N_SM / 2);
      EXPECT_FALSE(b2.zero()); EXPECT_FALSE(b2.empty());
      EXPECT_EQ(b2.size(), N_SM / 2); EXPECT_EQ(b2.start(), N_SM / 2); EXPECT_EQ(b2.capacity(), N_SM);
      EXPECT_NE(b3.data(), b2.data());
      EXPECT_TRUE(ALL_ZERO_FN(b3));
      EXPECT_TRUE(ALL_ZERO_FN(b2));
    } // Copy-ct.

    { // Copy-assign et al.
      auto b1 = make_blob<Blob_t>(alloc, &logger, N_SM, CLEAR_ON_ALLOC);
      b1.resize(b1.size() / 2, b1.size() / 2);
      auto b2 = make_blob<Blob_t>(alloc, &logger, ZERO);
      b2 = b1; // Overwrite null blob.
      EXPECT_FALSE(b2.zero()); EXPECT_FALSE(b2.empty());
      EXPECT_EQ(b2.size(), N_SM / 2);
      EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), N_SM / 2); // Attn: same deal as with copy-ct.
      EXPECT_TRUE(ALL_ZERO_FN(b2));
      EXPECT_NE(b1.data(), b2.data());
      EXPECT_FALSE(b1.zero()); EXPECT_FALSE(b1.empty());
      EXPECT_EQ(b1.size(), N_SM / 2); EXPECT_EQ(b1.start(), N_SM / 2); EXPECT_EQ(b1.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b1));

      auto b3 = make_blob<Blob_t>(alloc, &logger, N_SM);
      fill_n(b3.begin(), ONE, b3.size());
      const size_t N_TN = 5;
      b3.resize(N_SM - N_TN, N_TN); // Structure: [N_TN][N_SM - N_TN][], all ONEs.  Terms: [prefix][body][postfix].
      EXPECT_TRUE(RNG_ONES_FN(b3.begin() - N_TN, b3.end())); // Ensure they're all ONEs in fact.
      EXPECT_EQ(b3.capacity(), N_SM); ASSERT_EQ(b2.capacity(), N_SM / 2) << "Sanity-check.";
      /* Overwrite non-null, larger-capacity blob, starting at buffer-start (not just past the leading [N_TN] area,
       * meaning not at .begin()).
       * I.e., it makes use of as much buffer as it can, as early in it as possible, meaning at its start.  .begin()
       * is adjusted accordingly. */
      b3 = b2; // So now it's [][N_SM / 2][N_SM - N_SN/2].
      EXPECT_FALSE(b3.zero()); EXPECT_FALSE(b3.empty());
      EXPECT_EQ(b3.start(), size_t(0)); EXPECT_EQ(b3.size(), N_SM / 2); EXPECT_EQ(b3.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b3)); // Copied-stuff a/k/a body should be as in b2.
      EXPECT_TRUE(RNG_ONES_FN(b3.end(), b3.begin() - b3.start() + b3.capacity())); // Postfix should be untouched.

      { // Copy blob over itself (no-op).
        const auto saved_dt = b3.data();
        const auto saved_start = b3.start(); const auto saved_size = b3.size(); const auto saved_cap = b3.capacity();
        b3 = static_cast<const Blob_t&>(b3); // Cast to avoid warning in some compilers (auto self-assignment).
        EXPECT_EQ(b3.data(), saved_dt);
        EXPECT_EQ(b3.start(), saved_start); EXPECT_EQ(b3.size(), saved_size); EXPECT_EQ(b3.capacity(), saved_cap);
      }
      auto b4 = make_blob<Blob_t>(alloc, &logger, ZERO);
      { // Copy null blob over itself (no-op).
        const auto saved_dt = b4.data();
        const auto saved_start = b4.start(); const auto saved_size = b4.size(); const auto saved_cap = b4.capacity();
        b4 = static_cast<const Blob_t&>(b4); // Cast to avoid warning in some compilers (auto self-assignment).
        EXPECT_EQ(b4.data(), saved_dt);
        EXPECT_EQ(b4.start(), saved_start); EXPECT_EQ(b4.size(), saved_size); EXPECT_EQ(b4.capacity(), saved_cap);
      }

      /* "Cheating" white-boxily in this reasoning: really the above assignments build on various directly-accessible
       * APIs including assign(), assign_copy(), emplace_copy(), resize(), and reserve().  Of these the "core" ones
       * are emplace_copy() (copy bytes from anywhere into a blob) and reserve() (allocate if needed).  So we've
       * tested stuff already.  Still throw in a few direct calls just in case (e.g., impl could change).  We can
       * really skip assign() though, as it is the same as copy-assignment. */
      // assign_copy().
      auto b5 = make_blob<Blob_t>(alloc, &logger, ZERO);
      EXPECT_EQ(b5.assign_copy(STR_BUF), STR_BUF.size()); // Made of ONEs.
      EXPECT_EQ(b5.start(), size_t(0)); EXPECT_EQ(b5.size(), STRING.size()); EXPECT_EQ(b5.capacity(), STRING.size());
      EXPECT_TRUE(RNG_ONES_FN(b5.begin(), b5.end()));
      EXPECT_EQ(b5.assign_copy(STR_SM_BUF), STR_SM_BUF.size()); // Made of zeroes.
      EXPECT_EQ(b5.start(), size_t(0)); EXPECT_EQ(b5.size(), STRING_SM.size()); EXPECT_EQ(b5.capacity(), STRING.size());
      EXPECT_TRUE(ALL_ZERO_FN(b5));
      EXPECT_TRUE(RNG_ONES_FN(b5.end(), b5.begin() + STRING.size()));
      // emplace_copy().
      b5.resize(b5.capacity());
      EXPECT_EQ(b5.start(), size_t(0)); EXPECT_EQ(b5.size(), STRING.size()); EXPECT_EQ(b5.capacity(), STRING.size());
      EXPECT_TRUE(RNG_ONES_FN(b5.begin() + STRING_SM.size(), b5.begin() + STRING.size()));
      EXPECT_EQ(b5.emplace_copy(b5.begin() + (STRING_SM.size() / 2), STR_SM_BUF),
                b5.begin() + (STRING_SM.size() / 2) + STRING_SM.size());
      // All these must remain unchanged; just bytes were copied into a sub-range of [.b(), .e()).
      EXPECT_EQ(b5.start(), size_t(0)); EXPECT_EQ(b5.size(), STRING.size()); EXPECT_EQ(b5.capacity(), STRING.size());
      EXPECT_TRUE(RNG_ZERO_FN(b5.begin(),
                              b5.begin() + (STRING_SM.size() / 2)));
      EXPECT_TRUE(RNG_ZERO_FN(b5.begin() + (STRING_SM.size() / 2),
                              b5.begin() + (STRING_SM.size() / 2) + STRING_SM.size()));
      EXPECT_TRUE(RNG_ONES_FN(b5.begin() + (STRING_SM.size() / 2) + STRING_SM.size(),
                              b5.end()));
      // Copy (non-overlappingly only) inside the blob.
      EXPECT_EQ(b5.emplace_copy(b5.begin() + 1,
                                const_buffer{b5.end() - 5, 4}),
                b5.begin() + 5);
      EXPECT_EQ(b5.start(), size_t(0)); EXPECT_EQ(b5.size(), STRING.size()); EXPECT_EQ(b5.capacity(), STRING.size());
      EXPECT_EQ(b5.front(), uint8_t(0));
      EXPECT_TRUE(RNG_ONES_FN(b5.begin() + 1,
                              b5.begin() + 1 + 4));
      EXPECT_TRUE(RNG_ZERO_FN(b5.begin() + 1 + 4,
                              b5.begin() + (STRING_SM.size() / 2)));
      EXPECT_TRUE(RNG_ZERO_FN(b5.begin() + (STRING_SM.size() / 2),
                              b5.begin() + (STRING_SM.size() / 2) + STRING_SM.size()));
      EXPECT_TRUE(RNG_ONES_FN(b5.begin() + (STRING_SM.size() / 2) + STRING_SM.size(),
                              b5.end()));
    } // Copy-assign et al.

    { // Move-ct, move-assign.
      const size_t N_TN = 5;

      auto b1 = make_blob<Blob_t>(alloc, &logger, N_SM, CLEAR_ON_ALLOC);
      EXPECT_FALSE(b1.zero()); EXPECT_FALSE(b1.empty());
      EXPECT_EQ(b1.size(), N_SM); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b1));
      auto saved_dt = b1.const_data();
      Blob_t b2{std::move(b1)};
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.capacity(), size_t(0));
      EXPECT_FALSE(b2.zero()); EXPECT_FALSE(b2.empty());
      EXPECT_EQ(b2.size(), N_SM); EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b2));
      EXPECT_EQ(b1.data(), nullptr);
      EXPECT_EQ(b2.data(), saved_dt);

      b2.resize(b2.size() / 2, b2.size() / 2);
      saved_dt = b2.const_data();
      Blob_t b3{std::move(b2)};
      EXPECT_TRUE(b2.zero()); EXPECT_TRUE(b2.empty());
      EXPECT_EQ(b2.size(), size_t(0)); EXPECT_EQ(b2.start(), size_t(0)); EXPECT_EQ(b2.capacity(), size_t(0));
      EXPECT_FALSE(b3.zero()); EXPECT_FALSE(b3.empty());
      EXPECT_EQ(b3.size(), N_SM / 2); EXPECT_EQ(b3.start(), N_SM / 2); EXPECT_EQ(b3.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b3));
      EXPECT_EQ(b2.data(), nullptr);
      EXPECT_EQ(b3.data(), saved_dt);

      auto b4 = make_blob<Blob_t>(alloc, &logger, N_TN, CLEAR_ON_ALLOC);
      EXPECT_FALSE(b4.zero()); EXPECT_FALSE(b4.empty());
      EXPECT_EQ(b4.size(), N_TN); EXPECT_EQ(b4.start(), size_t(0)); EXPECT_EQ(b4.capacity(), N_TN);
      b4 = std::move(b3); // Move-assign.
      EXPECT_TRUE(b3.zero()); EXPECT_TRUE(b3.empty());
      EXPECT_EQ(b3.size(), size_t(0)); EXPECT_EQ(b3.start(), size_t(0)); EXPECT_EQ(b3.capacity(), size_t(0));
      EXPECT_FALSE(b4.zero()); EXPECT_FALSE(b4.empty());
      EXPECT_EQ(b4.size(), N_SM / 2); EXPECT_EQ(b4.start(), N_SM / 2); EXPECT_EQ(b4.capacity(), N_SM);
      EXPECT_TRUE(ALL_ZERO_FN(b4));
      EXPECT_EQ(b3.data(), nullptr);
      EXPECT_EQ(b4.data(), saved_dt);
    } // Move-ct, move-assign.

    { // reserve(), make_zero().
      constexpr size_t N1 = 20;
      constexpr size_t N2 = 10;
      constexpr size_t N_BIG = 1024 * 1024; // Biggish so as to lower chance all-zeroes being already there by accident.
      auto b1 = make_blob<Blob_t>(alloc, &logger, ZERO);
      EXPECT_TRUE(b1.zero()); EXPECT_EQ(b1.capacity(), size_t(0));
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0));
      EXPECT_EQ(b1.begin(), nullptr); EXPECT_EQ(b1.begin(), b1.end()); EXPECT_EQ(b1.data(), nullptr);
      b1.make_zero(); // No-op.
      EXPECT_TRUE(b1.zero()); EXPECT_EQ(b1.capacity(), size_t(0));
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0));
      EXPECT_EQ(b1.begin(), nullptr); EXPECT_EQ(b1.begin(), b1.end()); EXPECT_EQ(b1.data(), nullptr);
      b1.reserve(N1);
      EXPECT_FALSE(b1.zero()); EXPECT_EQ(b1.capacity(), N1);
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0));
      EXPECT_NE(b1.begin(), nullptr); EXPECT_EQ(b1.begin(), b1.end()); EXPECT_NE(b1.data(), nullptr);
      b1.reserve(N2); // Smaller => no-op.
      EXPECT_FALSE(b1.zero()); EXPECT_EQ(b1.capacity(), N1);
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0));
      EXPECT_NE(b1.begin(), nullptr); EXPECT_EQ(b1.begin(), b1.end()); EXPECT_NE(b1.data(), nullptr);
      b1.make_zero(); // Dealloc here (ahead of destructor).
      EXPECT_TRUE(b1.zero()); EXPECT_EQ(b1.capacity(), size_t(0));
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0));
      EXPECT_EQ(b1.begin(), nullptr); EXPECT_EQ(b1.begin(), b1.end()); EXPECT_EQ(b1.data(), nullptr);
      b1.reserve(N_BIG, CLEAR_ON_ALLOC);
      EXPECT_FALSE(b1.zero()); EXPECT_EQ(b1.capacity(), N_BIG);
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0));
      EXPECT_NE(b1.begin(), nullptr); EXPECT_EQ(b1.begin(), b1.end()); EXPECT_NE(b1.data(), nullptr);
      RNG_ZERO_FN(b1.begin(), b1.begin() + b1.capacity());
      b1.reserve(N1); // Smaller => no-op.
      EXPECT_FALSE(b1.zero()); EXPECT_EQ(b1.capacity(), N_BIG);
      EXPECT_EQ(b1.size(), size_t(0)); EXPECT_EQ(b1.start(), size_t(0));
      EXPECT_NE(b1.begin(), nullptr); EXPECT_EQ(b1.begin(), b1.end()); EXPECT_NE(b1.data(), nullptr);
      RNG_ZERO_FN(b1.begin(), b1.begin() + b1.capacity());
      // Destructor deallocs.
    } // reserve(), make_zero().

    { // begin(), end(), front(), back(), et al.
      /* We've been using begin() and end() plenty.  We test some corner cases like when zero(); but beyond that
       * mainly we try to expose copy/paste errors like const_end() accidentally equalling begin() and such.
       * We implicitly assume that more or less if begin() and end() work on one vanilla example, they work for all
       * similar ones as opposed to being ultra-paranoid about secret bug-prone logic inside. */

      constexpr size_t INC = 5;

      // Sanity-check begin() and end() cores when zero() and not zero() but empty() (degenerate cases).
      auto b1 = make_blob<Blob_t>(alloc, &logger, ZERO);
      EXPECT_EQ(b1.begin(), nullptr); EXPECT_EQ(b1.end(), b1.begin());
      b1.reserve(N_SM, CLEAR_ON_ALLOC); ASSERT_EQ(b1.size(), size_t(0));
      EXPECT_NE(b1.begin(), nullptr); EXPECT_EQ(b1.end(), b1.begin());

      // Now for the mainstream situation (!empty(); also have non-empty prefix (start()) and postfix).
      b1.resize(b1.capacity() - INC - INC, INC);
      uint8_t* const b = b1.begin();
      uint8_t* const e = b1.end();
      const uint8_t* const c_b = static_cast<const Blob_t&>(b1).begin();
      const uint8_t* const c_e = static_cast<const Blob_t&>(b1).end();
      const uint8_t* const c_cb = b1.cbegin();
      const uint8_t* const c_ce = b1.cend();
      const uint8_t* const c_ccb = b1.const_begin();
      const uint8_t* const c_cce = b1.const_end();
      uint8_t* const d = b1.data();
      const uint8_t* const c_d = b1.const_data();

      EXPECT_EQ(int(e - b), int(b1.size()));
      EXPECT_TRUE(RNG_ZERO_FN(b - INC, e + INC)); // Should be all derefable (and zeroed) as opposed to possible crash.
      EXPECT_EQ(b, c_b); EXPECT_EQ(b, c_cb); EXPECT_EQ(b, c_ccb); EXPECT_EQ(b, d); EXPECT_EQ(b, c_d);
      EXPECT_EQ(e, c_e); EXPECT_EQ(e, c_ce); EXPECT_EQ(e, c_cce);

      uint8_t& fr = b1.front();
      uint8_t& bk = b1.back();
      const uint8_t& c_fr = static_cast<const Blob_t&>(b1).front();
      const uint8_t& c_bk = static_cast<const Blob_t&>(b1).back();
      const uint8_t& c_cfr = b1.const_front();
      const uint8_t& c_cbk = b1.const_back();

      EXPECT_EQ(b, &fr); EXPECT_EQ(e - 1, &bk);
      EXPECT_EQ(&fr, &c_fr); EXPECT_EQ(&fr, &c_cfr);
      EXPECT_EQ(&bk, &c_bk); EXPECT_EQ(&bk, &c_cbk);
    } // begin(), end(), front(), back(), et al.

    { // start_past_prefix[_inc](), clear().
      /* resize() is a big one, but we've been using it a lot already, so let's not be tedious.
       * start_past_prefix() is built on it, and start_past_prefix_inc() is built on the latter. */

      constexpr size_t N1 = 20;
      constexpr size_t N2 = 10;
      constexpr size_t INC = 5;

      auto b1 = make_blob<Blob_t>(alloc, &logger, N1);
      EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.size(), N1);
      b1.start_past_prefix(N2);
      EXPECT_EQ(b1.start(), N2); EXPECT_EQ(b1.size(), N1 - N2);
      b1.make_zero(); // Dealloc.
      b1.resize(N2, INC); // Alloc.
      EXPECT_EQ(b1.capacity(), N2 + INC);
      b1.make_zero(); // Dealloc.
      b1.reserve(N_SM); // Alloc: Much bigger play area.
      b1.resize(N2, INC);
      ASSERT_GT(b1.capacity(), N2 + INC) << "Sanity-check our own logic real quick.";
      EXPECT_EQ(b1.start(), INC); EXPECT_EQ(b1.size(), N2);
      ASSERT_GT(N1, N2 + INC) << "Sanity-check our own logic real quick.";
      b1.start_past_prefix(N1); // Requested start() > current start() + size() => size() becomes 0.
      EXPECT_EQ(b1.start(), N1); EXPECT_EQ(b1.size(), size_t(0));
      b1.start_past_prefix_inc(-1);
      EXPECT_EQ(b1.start(), N1 - 1); EXPECT_EQ(b1.size(), size_t(1));
      b1.start_past_prefix_inc(-5);
      EXPECT_EQ(b1.start(), N1 - 1 - 5); EXPECT_EQ(b1.size(), size_t(1) + 5);
      b1.start_past_prefix_inc(+2);
      EXPECT_EQ(b1.start(), N1 - 1 - 5 + 2); EXPECT_EQ(b1.size(), size_t(1) + 5 - 2);
      b1.start_past_prefix_inc(+5); // Push past original start().  size() itself is floored at 0.
      EXPECT_EQ(b1.start(), N1 + 1); EXPECT_EQ(b1.size(), size_t(0));
      b1.start_past_prefix_inc(-(N1 + 1));
      EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.size(), N1 + 1);
      // (Recall that clear() never actually deallocs.  It really just sets size() to 0; that's it.)
      b1.resize(N2, INC);
      EXPECT_EQ(b1.start(), INC); EXPECT_EQ(b1.size(), N2);
      b1.clear(); // Attn.
      EXPECT_FALSE(b1.zero()); EXPECT_TRUE(b1.empty()); // Empty but buffer is actually allocated still.
      EXPECT_EQ(b1.start(), INC); EXPECT_EQ(b1.size(), size_t(0)); // Empty but start() unchanged as advertised.
      b1.make_zero();
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.size(), size_t(0));
      b1.clear(); // No-op.
      EXPECT_TRUE(b1.zero()); EXPECT_TRUE(b1.empty());
      EXPECT_EQ(b1.start(), size_t(0)); EXPECT_EQ(b1.size(), size_t(0));
    } // start_past_prefix[_inc](), clear().

    { // erase().
      /* Historical note: there was a time when that method was horribly buggy!  So it's well worth testing.
       * Though the current impl is so simple that it's probably not gonna break if the basics work.  We digress.... */

      constexpr size_t N1 = 20;
      constexpr size_t INC = 5;

      auto b1 = make_blob<Blob_t>(alloc, &logger, N1, CLEAR_ON_ALLOC);

      b1.resize(b1.capacity() - INC, INC); // [INC][N1 - INC][], all 0.
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start(), b1.begin() - b1.start() + b1.capacity()));
      fill_n(b1.begin() + INC, ONE, INC); // [INC x 0][INC x 0, INC x 1, rest x 0][].
      ASSERT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start(),
                              b1.begin() - b1.start() + INC + INC)) << "Sanity-check selves.";
      ASSERT_TRUE(RNG_ONES_FN(b1.begin() - b1.start() + INC + INC,
                              b1.begin() - b1.start() + INC + INC + INC)) << "Sanity-check selves.";
      ASSERT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start() + INC + INC + INC,
                              b1.begin() + b1.size())) << "Sanity-check selves.";
      ASSERT_EQ(b1.start(), INC) << "Sanity-check selves.";
      EXPECT_EQ(b1.size(), N1 - INC) << "Sanity-check selves.";
      EXPECT_EQ(b1.erase(b1.begin() - b1.start() + INC + INC, // No-op.
                         b1.begin() - b1.start() + INC + INC),
                b1.begin() - b1.start() + INC + INC);
      EXPECT_EQ(b1.start(), INC); EXPECT_EQ(b1.size(), N1 - INC);
      EXPECT_EQ(b1.erase(b1.begin() - b1.start() + INC + INC, // No-op.
                         b1.begin() - b1.start() + INC + INC - 1),
                b1.begin() - b1.start() + INC + INC);
      EXPECT_EQ(b1.start(), INC); EXPECT_EQ(b1.size(), N1 - INC);
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start(),
                              b1.begin() - b1.start() + INC + INC));
      EXPECT_TRUE(RNG_ONES_FN(b1.begin() - b1.start() + INC + INC,
                              b1.begin() - b1.start() + INC + INC + INC));
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start() + INC + INC + INC,
                              b1.begin() + b1.size()));

      EXPECT_EQ(b1.erase(b1.begin() - b1.start() + INC + INC, // Erase the INCx1 area.  Only 0s remain all-over.
                         b1.begin() - b1.start() + INC + INC + INC),
                b1.begin() - b1.start() + INC + INC);
      EXPECT_EQ(b1.start(), INC); EXPECT_EQ(b1.size(), N1 - INC - INC);
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start(),
                              b1.begin() + b1.size()));
    } // erase().

    { // sub_copy().
      constexpr size_t N1 = 20;
      constexpr size_t INC = 10;
      vector<uint8_t> DIST_VEC(N1); // Don't use {} here due to stupid C++ ambiguity with initializer-list.
      ASSERT_TRUE(RNG_ZERO_FN(DIST_VEC.begin(), DIST_VEC.end()));

      auto b1 = make_blob<Blob_t>(alloc, &logger, N_SM, CLEAR_ON_ALLOC);
      b1.resize(b1.capacity() - INC, INC);
      fill_n(b1.begin() + INC, INC, ONE);
      EXPECT_EQ(b1.sub_copy(b1.begin() + INC, mutable_buffer{&(DIST_VEC.front()), 0}), // Degenerate case (no-op).
                b1.begin() + INC);
      EXPECT_TRUE(RNG_ZERO_FN(DIST_VEC.begin(), DIST_VEC.end()));
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start(),
                              b1.begin() + INC));
      EXPECT_TRUE(RNG_ONES_FN(b1.begin() + INC,
                              b1.begin() + INC + INC));
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() + INC + INC,
                              b1.begin() - b1.start() + b1.capacity()));
      EXPECT_EQ(b1.sub_copy(b1.begin() + INC, mutable_buffer{&(DIST_VEC.front()), INC}),
                b1.begin() + INC + INC);
      EXPECT_TRUE(RNG_ONES_FN(DIST_VEC.begin(),
                              DIST_VEC.begin() + INC));
      EXPECT_TRUE(RNG_ZERO_FN(DIST_VEC.begin() + INC,
                              DIST_VEC.end()));
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start(), // b1 shall be unchanged always from sub_copy()->external.
                              b1.begin() + INC));
      EXPECT_TRUE(RNG_ONES_FN(b1.begin() + INC,
                              b1.begin() + INC + INC));
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() + INC + INC,
                              b1.begin() - b1.start() + b1.capacity()));
      // Copy inside non-overlappingly.
      EXPECT_EQ(b1.sub_copy(b1.begin() + INC, mutable_buffer{b1.end() - INC - INC, INC}),
                b1.begin() + INC + INC);
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() - b1.start(),
                              b1.begin() + INC));
      EXPECT_TRUE(RNG_ONES_FN(b1.begin() + INC,
                              b1.begin() + INC + INC));
      EXPECT_TRUE(RNG_ZERO_FN(b1.begin() + INC + INC,
                              b1.end() - INC - INC));
      EXPECT_TRUE(RNG_ONES_FN(b1.end() - INC - INC,
                              b1.end() - INC));
      EXPECT_TRUE(RNG_ZERO_FN(b1.end() - INC,
                              b1.begin() - b1.start() + b1.capacity()));
      // This stuff should all be unchanged throughout.
      EXPECT_EQ(b1.start(), INC); EXPECT_EQ(b1.size(), N_SM - INC); EXPECT_EQ(b1.capacity(), N_SM);
    } // sub_copy().

    { // Allocators, multiple allocators.
      // (Please see comment near `using Shm_pool =` for some background.)

      /* We've heavily exercised general/basic allocator-involving code and APIs already.  Even when
       * !SHM_ALLOC (Allocator is std::allocator), Basic_blob et al takes an allocator instance to
       * to ctor, and we've been doing that; then naturally allocating/deallocating things with it.  So what we
       * test here, allocator-wise, is two-fold:
       *   - First, some boring checks involving get_allocator(): that this returns an object equal-by-value to
       *     what we gave to ctor.  So like with std::allocator it'll always be true, since it is stateless and
       *     equality is always true as a result; but if SHM_ALLOC then there can be actually-unequal allocator
       *     objects, so we can sanity-check that.
       *   - Second, though, there are more subtle tests of what happens w/r/t allocators when `Blob_t`s are
       *     copy/move-constructed; copy/move-assigned; and swapped.  There're the get_allocator() equality checks
       *     again -- but more saliently what if get_allocator() is replaced by some assignment, but there's already
       *     a buffer allocated within the receiving object?  That stuff.  P.S. Just getting it compile is itself
       *     a victory.  P.P.S. Again, when !SHM_ALLOC there's only one stateless allocator really, so
       *     those same tests just vacuously pass; but it's harmless to still do it. */

      /* Actually one basic check is to ensure the allocator is... used.  There could easily be an error causing
       * std::allocator to be used regardless of anything.  (As of this writing internally there are ~3 possible
       * smart-pointer types used inside; we could have chosen the wrong one, misused allocator APIs, etc.) */
      {
        const auto check_alloc = [&](const Blob_t& b, const string& ctx)
        {
          const auto p = b.const_data();
          EXPECT_TRUE(p) << ctx;
          if constexpr(SHM_ALLOC)
          {
            ASSERT_EQ(b.get_allocator(), *alloc) << "Sanity-check selves.  " << ctx;
            ASSERT_EQ(alloc->get_segment_manager(), shm_pool.get_segment_manager()) << "Sanity-check selves.  " << ctx;

            const auto pool_base = static_cast<const uint8_t*>(shm_pool.get_address());
            EXPECT_TRUE((p >= pool_base) && (p < (pool_base + shm_pool.get_size()))) << ctx;
          }
          // else { Not much we can check when it's just std::allocator. }
        };

        auto b1 = make_blob<Blob_t>(alloc, &logger, N_SM); check_alloc(b1, CTX); b1.make_zero();
        b1.reserve(N_SM); check_alloc(b1, CTX); b1.make_zero();
        b1.resize(N_SM); check_alloc(b1, CTX); b1.make_zero();

        auto b2 = make_blob<Blob_t>(alloc, &logger, N_SM, CLEAR_ON_ALLOC); check_alloc(b2, CTX); b2.make_zero();
        b2.reserve(N_SM, CLEAR_ON_ALLOC); check_alloc(b2, CTX); b2.make_zero();
        b2.resize(N_SM, CLEAR_ON_ALLOC); check_alloc(b2, CTX); b2.make_zero();

        b1.resize(N_SM);
        Blob_t b3{std::move(b1)}; check_alloc(b3, CTX);
        Blob_t b4{b3}; check_alloc(b4, CTX);
      }

      if constexpr(SHM_ALLOC)
      {
        ASSERT_NE(*alloc, *alloc2) << "Sanity-check.";
      }
      else
      {
        ASSERT_EQ(*alloc, *alloc2) << "Sanity-check.";
      }

      // get_allocator() value checks.

      auto b1 = make_blob<Blob_t>(alloc, &logger, ZERO);
      auto b2 = make_blob<Blob_t>(alloc2, &logger, ZERO);
      EXPECT_EQ(b1.get_allocator(), *alloc); EXPECT_EQ(b2.get_allocator(), *alloc2);
      b1.resize(N_SM); b2.resize(N_SM);
      EXPECT_EQ(b1.get_allocator(), *alloc); EXPECT_EQ(b2.get_allocator(), *alloc2);

      b1 = make_blob<Blob_t>(alloc, &logger, N_SM);
      b2 = make_blob<Blob_t>(alloc2, &logger, N_SM);
      EXPECT_EQ(b1.get_allocator(), *alloc); EXPECT_EQ(b2.get_allocator(), *alloc2);

      { // Copy-ct, copy-assign.  No buffer.
        auto b3 = make_blob<Blob_t>(alloc, &logger, ZERO);
        Blob_t b4{b3};
        EXPECT_EQ(b4.get_allocator(), *alloc); EXPECT_EQ(b3.get_allocator(), *alloc);
        auto b5 = make_blob<Blob_t>(alloc2, &logger, ZERO);
        EXPECT_EQ(b5.get_allocator(), *alloc2); EXPECT_EQ(b3.get_allocator(), *alloc);
        b3 = b5;
        EXPECT_EQ(b5.get_allocator(), *alloc2);
        static_assert(!allocator_traits<Allocator>::propagate_on_container_copy_assignment::value,
                      "Our test allocators, including the stateful ones, configured themselves to "
                        "*not* propagate on copy-assignment.  If this static-assert trips, something major changed "
                        "in boost.interprocess maybe?!");
        EXPECT_EQ(b3.get_allocator(), *alloc); // Copy-construction propagates... but not copy-assignment.
      }
      { // Copy-ct, copy-assign.  Yes buffer.
        auto b3 = make_blob<Blob_t>(alloc, &logger, N_SM);
        Blob_t b4{b3};
        EXPECT_EQ(b4.get_allocator(), *alloc); EXPECT_EQ(b3.get_allocator(), *alloc);
        EXPECT_EQ(b4.capacity(), N_SM); EXPECT_EQ(b3.capacity(), N_SM);
        auto b5 = make_blob<Blob_t>(alloc2, &logger, N_SM);
        EXPECT_EQ(b5.get_allocator(), *alloc2); EXPECT_EQ(b3.get_allocator(), *alloc);
        EXPECT_EQ(b5.capacity(), N_SM); EXPECT_EQ(b3.capacity(), N_SM);
        b3 = b5;
        EXPECT_EQ(b5.get_allocator(), *alloc2);
        EXPECT_EQ(b3.get_allocator(), *alloc); // Copy-construction propagates... but not copy-assignment.
        EXPECT_EQ(b5.capacity(), N_SM); EXPECT_EQ(b3.capacity(), N_SM);
      }

      { // Move-ct, move-assign, swap.  No buffer.
        auto b3 = make_blob<Blob_t>(alloc, &logger, ZERO);
        Blob_t b4{std::move(b3)};
        EXPECT_EQ(b4.get_allocator(), *alloc); EXPECT_EQ(b3.get_allocator(), *alloc);
        auto b5 = make_blob<Blob_t>(alloc2, &logger, ZERO);
        EXPECT_EQ(b5.get_allocator(), *alloc2); EXPECT_EQ(b3.get_allocator(), *alloc);
        b3 = std::move(b5);
        EXPECT_EQ(b5.get_allocator(), *alloc2);
        static_assert(!(SHM_ALLOC && allocator_traits<Allocator>::propagate_on_container_move_assignment::value),
                      "Our stateful test allocators configured themselves to "
                        "*not* propagate on move-assignment.  If this static-assert trips, something major changed "
                        "in boost.interprocess maybe?!");
        EXPECT_EQ(b3.get_allocator(), *alloc); // Move-construction propagates... but not move-assignment.
        swap(b3, b5);
        EXPECT_EQ(b5.get_allocator(), *alloc2); EXPECT_EQ(b3.get_allocator(), *alloc);
      }
      { // Move-ct, move-assign, swap.  Yes buffer.
        auto b3 = make_blob<Blob_t>(alloc, &logger, N_SM);
        Blob_t b4{std::move(b3)};
        /* (The b3's allocator is supposed to be moved-from, but in practice for allocators -- at the very least
         * the ones we have here, but also generally from what we know of allocators -- it is the same as being
         * copied-from.  Like, the moved-from allocator shouldn't get nullified or something; generally that is not
         * really a thing; that I (ygoldfel) know of.)  So we'll just still check the moved-from get_allocator(). */
        EXPECT_EQ(b4.get_allocator(), *alloc); EXPECT_EQ(b3.get_allocator(), *alloc);
        EXPECT_EQ(b4.capacity(), N_SM); EXPECT_EQ(b3.capacity(), size_t(0));
        auto b5 = make_blob<Blob_t>(alloc2, &logger, N_SM);
        EXPECT_EQ(b5.get_allocator(), *alloc2); EXPECT_EQ(b4.get_allocator(), *alloc);
        EXPECT_EQ(b5.capacity(), N_SM); EXPECT_EQ(b4.capacity(), N_SM);
        b4 = std::move(b5);
        EXPECT_EQ(b5.get_allocator(), *alloc2);
        EXPECT_EQ(b4.get_allocator(), *alloc); // Move-construction propagates... but not move-assignment.
        EXPECT_EQ(b5.capacity(), size_t(0)); EXPECT_EQ(b4.capacity(), N_SM);

        static_assert(!(SHM_ALLOC && allocator_traits<Allocator>::propagate_on_container_swap::value),
                      "Our stateful test allocators configured themselves to "
                        "*not* propagate on swap.  If this static-assert trips, something major changed "
                        "in boost.interprocess maybe?!");
        swap(b4, b5);
        EXPECT_EQ(b5.capacity(), N_SM); EXPECT_EQ(b4.capacity(), size_t(0)); // Buf pointers apparently swapped...
        EXPECT_EQ(b5.get_allocator(), *alloc2); // ...but not allocators.
        EXPECT_EQ(b4.get_allocator(), *alloc);
        b5.make_zero(); // Force deallocation now -- at least it shouldn't crash.

        /* Lastly let's do a full swap (of 2 buffers, 2 allocators), checking the buf ptr values before/after.
         * We consider this decently sufficient without doing the same with move-assignment alone. */
        constexpr size_t N_LG = N_SM * 2;
        auto b6 = make_blob<Blob_t>(alloc, &logger, N_SM);
        auto b7 = make_blob<Blob_t>(alloc2, &logger, N_LG);
        auto p6 = b6.begin(); auto p7 = b7.begin();
        ASSERT_EQ(b6.get_allocator(), *alloc); ASSERT_EQ(b7.get_allocator(), *alloc2);
        swap(b6, b7);
        EXPECT_EQ(b7.begin(), p6); EXPECT_EQ(b6.begin(), p7);
        EXPECT_EQ(b7.size(), N_SM); EXPECT_EQ(b6.size(), N_LG);
        EXPECT_EQ(b6.get_allocator(), *alloc); EXPECT_EQ(b7.get_allocator(), *alloc2);
        b6.make_zero(); b7.make_zero(); // No crash here hopefully.
        b6.resize(N_LG); b7.resize(N_SM); p6 = b7.begin(); p7 = b6.begin();
        // Invoke non-ADL-swap too (over-abundance of caution).
        b6.swap(b7);
        EXPECT_EQ(b6.begin(), p6); EXPECT_EQ(b7.begin(), p7);
        EXPECT_EQ(b6.size(), N_SM); EXPECT_EQ(b7.size(), N_LG);
        EXPECT_EQ(b6.get_allocator(), *alloc); EXPECT_EQ(b7.get_allocator(), *alloc2);
        b6.make_zero(); b7.make_zero(); // No crash here hopefully.
      } // Move-ct, move-assign, swap.  Yes buffer.
    } // Allocators, multiple allocators.

    { // Dealloc; sharing.
      /* We combine a couple things here; we've been deallocing left and right but never really checked whether
       * in fact dealloc occurred.  It's not that easy to check it, but with SHM_ALLOC we can do it by using
       * bipc's stats.  So that's thing 1.  Thing 2, relevant only if SHARING, is to test that share()-based
       * feature, wher buf is deallocated only once all co-sharing blobs have been destroyed or make_zero()ed.
       *
       * We use a shared_ptr handle in heap, so that the allocator is used only for the actual buffer. */

      auto b1 = make_shared<Blob_t>(make_blob<Blob_t>(alloc, &logger, ZERO));
      size_t N = 1024 * 1024; // Pretty bug, so that alloc-slack is minor compared to it.

      [[maybe_unused]] size_t n_base = 0;
      if constexpr(SHM_ALLOC)
      {
        n_base = shm_pool.get_free_memory();
      }

      const auto check_alloc_sz = [&]([[maybe_unused]] size_t n_or_0, [[maybe_unused]] const string& ctx)
      {
        if constexpr(SHM_ALLOC)
        {
          if (n_or_0 == 0)
          {
            EXPECT_EQ(shm_pool.get_free_memory(), n_base) << ctx;
          }
          else
          {
            ASSERT_GE(n_base, shm_pool.get_free_memory()) << ctx;
            const auto n_diff = static_cast<size_t>(n_base - shm_pool.get_free_memory());
            // Should be buf-size x N, plus some possibly some alloc slack.
            EXPECT_GE(n_diff, n_or_0 * N) << ctx;
            EXPECT_LT(n_diff, (n_or_0 + 1) * N) << ctx;
          }
        }
        // else { We cannot check anything about this.  Oh well. }
      };

      check_alloc_sz(0, CTX); // Sanity-check.
      b1->reserve(N); check_alloc_sz(1, CTX);
      b1->make_zero(); check_alloc_sz(0, CTX);
      b1->resize(N); check_alloc_sz(1, CTX);

      if constexpr(SHARING)
      {
        constexpr size_t INC = 15;

        const auto p = b1->const_data();

        auto b2 = make_shared<Blob_t>(b1->share()); check_alloc_sz(1, CTX); // No extra alloc.
        EXPECT_EQ(b1->data() - b1->start(), p); EXPECT_EQ(b1->capacity(), N);
        EXPECT_EQ(b1->size(), N); EXPECT_EQ(b1->start(), size_t(0));
        EXPECT_EQ(b2->data() - b2->start(), p); EXPECT_EQ(b2->capacity(), N);
        EXPECT_EQ(b2->size(), N); EXPECT_EQ(b2->start(), size_t(0));

        auto b3 = make_shared<Blob_t>(b2->share_after_split_left(INC)); check_alloc_sz(1, CTX);
        EXPECT_EQ(b1->data() - b1->start(), p); EXPECT_EQ(b1->capacity(), N);
        EXPECT_EQ(b1->size(), N); EXPECT_EQ(b1->start(), size_t(0));
        EXPECT_EQ(b2->data() - b2->start(), p); EXPECT_EQ(b2->capacity(), N);
        EXPECT_EQ(b2->size(), N - INC); EXPECT_EQ(b2->start(), INC);
        EXPECT_EQ(b3->data() - b3->start(), p); EXPECT_EQ(b3->capacity(), N);
        EXPECT_EQ(b3->size(), INC); EXPECT_EQ(b3->start(), size_t(0));

        auto b4 = make_shared<Blob_t>(b2->share_after_split_right(INC)); check_alloc_sz(1, CTX);
        EXPECT_EQ(b1->data() - b1->start(), p); EXPECT_EQ(b1->capacity(), N);
        EXPECT_EQ(b1->size(), N); EXPECT_EQ(b1->start(), size_t(0));
        EXPECT_EQ(b2->data() - b2->start(), p); EXPECT_EQ(b2->capacity(), N);
        EXPECT_EQ(b2->size(), N - INC - INC); EXPECT_EQ(b2->start(), INC);
        EXPECT_EQ(b3->data() - b3->start(), p); EXPECT_EQ(b3->capacity(), N);
        EXPECT_EQ(b3->size(), INC); EXPECT_EQ(b3->start(), size_t(0));
        EXPECT_EQ(b4->data() - b4->start(), p); EXPECT_EQ(b4->capacity(), N);
        EXPECT_EQ(b4->size(), INC); EXPECT_EQ(b4->start(), N - INC);

        b2->make_zero(); check_alloc_sz(1, CTX); // Still alive.
        b3.reset(); check_alloc_sz(1, CTX); // Ditto.
        b1.reset(); check_alloc_sz(1, CTX); // Ditto!  Even after killing the original.
        b4->clear(); ASSERT_EQ(b4->capacity(), N); check_alloc_sz(1, CTX); // Doesn't even dec the ref-count.
        *b4 = make_blob<Blob_t>(alloc, &logger, ZERO); check_alloc_sz(0, CTX); // Now the ref-count got to zero.

        /* Avoid tedium slightly; use one form of share_after_split_equally*().  We use another elsewhere in this
         * test.  @todo Technically should go through them all (~3), but they're all built on one core (cheating
         * white-boxily when we say this). */
        vector<bool> headless_v{false, true};
        for (auto headless : headless_v)
        {
          cout << "    Splitting sub-test with headless = [" << headless << "].\n" << flush;
          constexpr size_t COUNT = 16; constexpr size_t N_PER_CT = 65536; constexpr size_t REM = 123;
          constexpr size_t CAP = COUNT * N_PER_CT + REM;
          vector<shared_ptr<Blob_t>> blob_vec;

          N = CAP; // Soooo hacky... @todo Come on... this is sad.

          check_alloc_sz(0, CTX);
          auto b5 = make_shared<Blob_t>(make_blob<Blob_t>(alloc, &logger, CAP, CLEAR_ON_ALLOC));
          check_alloc_sz(1, CTX);

          const auto p = b5->const_data();
          b5->share_after_split_equally_emit_ptr_seq(N_PER_CT, headless, &blob_vec);

          EXPECT_EQ(blob_vec.size(), COUNT + 1);
          check_alloc_sz(1, CTX);

          auto rem_blob = std::move(blob_vec.back()); blob_vec.pop_back();
          size_t start = 0;
          for (const auto& b : blob_vec)
          {
            EXPECT_EQ(b->data() - b->start(), p); EXPECT_EQ(b->capacity(), CAP);
            EXPECT_EQ(b->size(), N_PER_CT); EXPECT_EQ(b->start(), start);
            start += N_PER_CT;
          }

          EXPECT_EQ(rem_blob->data() - rem_blob->start(), p); EXPECT_EQ(rem_blob->capacity(), CAP);
          EXPECT_EQ(rem_blob->size(), REM); EXPECT_EQ(rem_blob->start(), start);

          check_alloc_sz(1, CTX);
          blob_vec.clear();
          rem_blob.reset();

          EXPECT_TRUE(b5->empty());
          if (!headless)
          {
            check_alloc_sz(1, CTX);
            b5->make_zero();
          }
          EXPECT_TRUE(b5->zero());
          check_alloc_sz(0, CTX);
        } // for (auto headless : headless_v)
      } // if constexpr(SHARING)
    } // Dealloc; sharing.

    /* Now to test the many assert-trip undefined-behavior triggers we've avoided above.  This is arguably more
     * important here than usual, in the sense that a common user error (since other containers allow it, but we
     * *intentionally* do not) would be to perform an op that would require an allocation of a larger buffer,
     * when one is already allocated (!zero()).  So at least we need to test the s*** out of that.  (Ultimately,
     * internally, it comes down to reserve() which is called by 100% of code paths requiring allocation.) */
#ifndef NDEBUG
    { // Death tests.
      cout << "  Assertions enabled -- performing death-tests.\n" << flush;

      { // General input checks.
        constexpr String_view RSRV_ERR_STR{"intentionally disallows reserving N>M>0.*make_zero.. first"};
        constexpr auto RSRV_ERR = RSRV_ERR_STR.data();

        // Various attempts to ultimately reserve(N), when `N > .capacity() > 0`.

        // reserve().
        constexpr size_t INC = 1;
        constexpr size_t N_LG = N_SM + INC;
        auto b1 = make_blob<Blob_t>(alloc, &logger, N_SM);
        b1.resize(0);
        ASSERT_EQ(b1.capacity(), N_SM); ASSERT_EQ(b1.size(), size_t(0));
        b1.reserve(N_SM);
        EXPECT_DEATH(b1.reserve(N_LG), RSRV_ERR);
        b1.make_zero();
        // resize().
        b1.reserve(N_LG);
        EXPECT_DEATH(b1.resize(N_LG - INC, INC + INC), RSRV_ERR);
        EXPECT_DEATH(b1.resize(N_LG, INC), RSRV_ERR);
        EXPECT_DEATH(b1.resize(0, N_LG + INC), RSRV_ERR);
        EXPECT_DEATH(b1.resize(N_LG - INC, CLEAR_ON_ALLOC, INC + INC), RSRV_ERR);
        EXPECT_DEATH(b1.resize(N_LG, CLEAR_ON_ALLOC, INC), RSRV_ERR);
        EXPECT_DEATH(b1.resize(0, CLEAR_ON_ALLOC, N_LG + INC), RSRV_ERR);
        b1.resize(N_LG, 0);
        b1.resize(0, N_LG);
        b1.resize(N_LG - INC, 0);
        b1.resize(N_LG - INC, INC);
        b1.resize(N_LG, CLEAR_ON_ALLOC, 0);
        b1.resize(0, CLEAR_ON_ALLOC, N_LG);
        b1.resize(N_LG - INC, CLEAR_ON_ALLOC, 0);
        b1.resize(N_LG - INC, CLEAR_ON_ALLOC, INC);
        // Copy-assign (also assign_copy(), emplace_copy() really, as of this writing white-boxily).
        auto b2 = make_blob<Blob_t>(alloc, &logger, N_SM);
        b1.resize(N_LG, 0); // A bit too big.
        EXPECT_DEATH(b2 = b1, RSRV_ERR);
        b2.clear();
        EXPECT_DEATH(b2 = b1, RSRV_ERR);
        b2.make_zero();
        b2 = b1;

        // Other input errors.

        // start_past_prefix_inc().
        b2.resize(10, 5);
        b2.start_past_prefix_inc(-1); ASSERT_EQ(b2.start(), size_t(4));
        b2.start_past_prefix_inc(-4); ASSERT_EQ(b2.start(), size_t(0));
        b2.start_past_prefix_inc(+4); ASSERT_EQ(b2.start(), size_t(4));
        EXPECT_DEATH(b2.start_past_prefix_inc(-5), "start.. >= size_type.-prefix_size_inc");

        { // emplace/sub_copy().
          string string_sm(14, '\0');
          const const_buffer STR_SM_BUF{string_sm.data(), string_sm.size()};
          const mutable_buffer STR_SM_BUF_MUT{string_sm.data(), string_sm.size()};

          // emplace_copy().
          b2.make_zero();
          b2.resize(N_SM);
          EXPECT_DEATH(b2.emplace_copy(b2.begin() - 1, const_buffer{string_sm.data(), 0}), "valid_iterator");
          EXPECT_DEATH(b2.emplace_copy(b2.end() + 1, const_buffer{string_sm.data(), 0}), "valid_iterator");
          EXPECT_DEATH(b2.emplace_copy(b2.begin() - 1, STR_SM_BUF), "valid_iterator");
          EXPECT_DEATH(b2.emplace_copy(b2.end() + 1, STR_SM_BUF), "valid_iterator");
          EXPECT_DEATH(b2.emplace_copy(b2.end(), STR_SM_BUF), "derefable_iterator");
          EXPECT_DEATH(b2.emplace_copy(b2.end() - 1, const_buffer{string_sm.data(), 2}),
                       "n. <= .const_end.. - dest");
          EXPECT_DEATH(b2.emplace_copy(b2.end() - 2, const_buffer{string_sm.data(), 3}),
                       "n. <= .const_end.. - dest");
          b2.emplace_copy(b2.end() - 1, const_buffer{string_sm.data(), 1});
          b2.emplace_copy(b2.end() - 2, const_buffer{string_sm.data(), 2});

          EXPECT_DEATH(b2.emplace_copy(b2.begin(), const_buffer{b2.begin() + 2, 3}),
                       ".dest_it \\+ n. <= src_data");
          b2.emplace_copy(b2.begin(), const_buffer{b2.begin() + 2, 2});
          EXPECT_DEATH(b2.emplace_copy(b2.begin() + 2, const_buffer{b2.begin(), 3}),
                       ".src_data \\+ n. <= dest_it");
          b2.emplace_copy(b2.begin() + 2, const_buffer{b2.begin(), 2});

          // sub_copy().
          EXPECT_DEATH(b2.sub_copy(b2.begin() - 1, mutable_buffer{string_sm.data(), 0}), "valid_iterator");
          EXPECT_DEATH(b2.sub_copy(b2.end() + 1, mutable_buffer{string_sm.data(), 0}), "valid_iterator");
          EXPECT_DEATH(b2.sub_copy(b2.begin() - 1, STR_SM_BUF_MUT), "valid_iterator");
          EXPECT_DEATH(b2.sub_copy(b2.end() + 1, STR_SM_BUF_MUT), "valid_iterator");
          EXPECT_DEATH(b2.sub_copy(b2.end(), STR_SM_BUF_MUT), "derefable_iterator");
          EXPECT_DEATH(b2.sub_copy(b2.end() - 1, mutable_buffer{string_sm.data(), 2}),
                       "n. <= .const_end.. - src");
          EXPECT_DEATH(b2.sub_copy(b2.end() - 2, mutable_buffer{string_sm.data(), 3}),
                       "n. <= .const_end.. - src");
          b2.sub_copy(b2.end() - 1, mutable_buffer{string_sm.data(), 1});
          b2.sub_copy(b2.end() - 2, mutable_buffer{string_sm.data(), 2});

          EXPECT_DEATH(b2.sub_copy(b2.begin(), mutable_buffer{b2.begin() + 2, 3}),
                       ".src \\+ n. <= dest_data");
          b2.sub_copy(b2.begin(), mutable_buffer{b2.begin() + 2, 2});
          EXPECT_DEATH(b2.sub_copy(b2.begin() + 2, mutable_buffer{b2.begin(), 3}),
                       ".dest_data \\+ n. <= src");
          b2.sub_copy(b2.begin() + 2, mutable_buffer{b2.begin(), 2});
        } // emplace/sub_copy().

        b1.make_zero();
        b1.resize(N_SM, 0);
        { // .erase()
          EXPECT_DEATH(b1.erase(b1.begin() - 1, b1.end()), "derefable_iterator.first");
          EXPECT_DEATH(b1.erase(b1.end(), b1.end()), "derefable_iterator.first");
          EXPECT_DEATH(b1.erase(b1.end() + 1, b1.end()), "derefable_iterator.first");
          EXPECT_DEATH(b1.erase(b1.begin(), b1.end() + 1), "valid_iterator.past_last");
          EXPECT_DEATH(b1.erase(b1.begin(), b1.begin() - 1), "valid_iterator.past_last");
          b1.erase(b1.begin(), b1.end()); ASSERT_TRUE(b1.empty());
          EXPECT_DEATH(b1.erase(b1.begin(), b1.end()), "derefable_iterator.first"); // Because empty.
        }

        // front(), back(), et al.
        ASSERT_TRUE(b1.empty());
        EXPECT_DEATH(b1.front(), "!empty");
        EXPECT_DEATH(b1.back(), "!empty");
        // @todo ^-- This'll skip checking the const overloads.
        EXPECT_DEATH(b1.const_front(), "!empty");
        EXPECT_DEATH(b1.const_back(), "!empty");
        b1.resize(1);
        EXPECT_EQ(b1.front(), b1.back());
        EXPECT_EQ(b1.const_front(), b1.const_back());
        {
          uint8_t& fr = b1.front();
          uint8_t& bk = b1.back();
          const uint8_t& c_fr = static_cast<const Blob_t&>(b1).front();
          const uint8_t& c_bk = static_cast<const Blob_t&>(b1).back();
          const uint8_t& c_cfr = b1.const_front();
          const uint8_t& c_cbk = b1.const_back();
          ASSERT_EQ(&fr, &bk);
          ASSERT_EQ(&fr, &c_fr); ASSERT_EQ(&fr, &c_cfr);
          ASSERT_EQ(&bk, &c_bk); ASSERT_EQ(&bk, &c_cbk);
        }
      } // General input checks.

      if constexpr(SHARING)
      {
        cout << "    A few sharing-specific death-tests for this type....\n" << flush;
        auto b3 = make_blob<Blob_t>(alloc, &logger, N_SM);
        auto b4 = b3.share_after_split_right(5);
        ASSERT_TRUE(b4.size() == 5); ASSERT_TRUE(b3.size() == N_SM - 5);
        ASSERT_TRUE(b3.size() > b4.size());
        // It's small enough... but copy-assign/etc. between shared blobs = not OK.
        EXPECT_DEATH(b4 = b3, "!blobs_sharing");
        EXPECT_DEATH(b3 = b4, "!blobs_sharing");
        EXPECT_DEATH(b4.assign(b3), "!blobs_sharing");
        EXPECT_DEATH(b3.assign(b4), "!blobs_sharing");
        b4.clear();
        EXPECT_DEATH(b4 = b3, "!blobs_sharing");
        EXPECT_DEATH(b3 = b4, "!blobs_sharing");
        EXPECT_DEATH(b4.assign(b3), "!blobs_sharing");
        EXPECT_DEATH(b3.assign(b4), "!blobs_sharing");
        b4.make_zero();
        b4 = b3;

        b4.make_zero();
        EXPECT_DEATH(b4.share(), "!zero");
        EXPECT_DEATH(b4.share_after_split_right(5), "!zero");
        EXPECT_DEATH(b4.share_after_split_left(5), "!zero");

        b4.reserve(N_SM); ASSERT_TRUE(b4.empty());
        vector<Blob_t> blobs;
        EXPECT_DEATH(b4.share_after_split_equally_emit_seq(0, true, &blobs), "size != 0");
        EXPECT_DEATH(b4.share_after_split_equally_emit_seq(0, false, &blobs), "size != 0");
        EXPECT_DEATH(b4.share_after_split_equally_emit_seq(1, true, &blobs), "!empty");
        EXPECT_DEATH(b4.share_after_split_equally_emit_seq(1, false, &blobs), "!empty");
        b4.resize(b4.capacity());
        EXPECT_DEATH(b4.share_after_split_equally_emit_seq(0, true, &blobs), "size != 0");
        EXPECT_DEATH(b4.share_after_split_equally_emit_seq(0, false, &blobs), "size != 0");
        b4.share_after_split_equally_emit_seq(1, true, &blobs); ASSERT_TRUE(blobs.size() == N_SM);
      } // if constexpr(SHARING)
    } // Death tests.
#endif // NDEBUG

    [[maybe_unused]] auto b1 = make_blob<Blob_t>(alloc, &logger, size_t(3), CLEAR_ON_ALLOC);
    if constexpr(SHM_ALLOC)
    {
      [[maybe_unused]] auto b2 = make_blob<Blob_t>(alloc2, &logger, size_t(6));
    }
  }; // const auto test_type =

  /* As noted earlier makes sense to test all permutations, even if only a "core" API were tested here.
   * Again though we do various type-specific APIs via `if constexpr` too. */
  Shm_allocator dummy_shm_alloc{shm_pool.get_segment_manager()};
  test_type(Basic_blob<Vanilla_allocator, false>());
  test_type(Basic_blob<Vanilla_allocator, true>());
  test_type(Basic_blob<Shm_allocator, false>(dummy_shm_alloc));
  test_type(Basic_blob<Shm_allocator, true>(dummy_shm_alloc));
  test_type(Blob_with_log_context<true>()); // Recall that for these it'll always use Vanilla_allocator.
  test_type(Blob_with_log_context<false>());
} // TEST(Blob, Interface)

} // namespace flow::util::test
