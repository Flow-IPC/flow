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

#include "flow/util/stat/stat_set.hpp"
#include "flow/util/stat/stat_set_list.hpp"
#include "flow/util/stat/histo.hpp"
#include "flow/common.hpp"
#include <boost/lexical_cast.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <iterator>
#include <vector>
#include <ostream>
#include <limits>
#include <type_traits>
#include <string>
#include <sstream>
#include <atomic>
#include <deque>

namespace flow::util::stat::test
{

namespace
{
  using boost::lexical_cast;
  using std::string;
  using std::vector;
  using std::cout;
  using std::flush;
  using std::is_same_v;
  using std::ostringstream;
  using std::atomic;
  using std::deque;

  using Counts = vector<Histogram_counter::count_t>;

  Counts load_counts(const Histogram_counter& histo)
  {
    Counts counts(histo.size());
    for (size_t idx = 0; idx != counts.size(); ++idx)
    {
      counts[idx] = histo.count_for_bucket(idx);
    }
    return counts;
  }
}

TEST(Stats_histogram_counter_test, Interface)
{
  // For ref: Histogram_counter(value_t n_buckets, value_t bucket0_sz, value_t bucket_sz = 1, value_t bucket0_val0 = 0)

#ifndef NDEBUG
  cout << "Death-testing basic bad Histogram_counter knobs.\n" << flush;
  {
    EXPECT_DEATH((Histogram_counter{0, 1, 1, 0}), "n_buckets > 1");
    EXPECT_DEATH((Histogram_counter{1, 1, 1, 0}), "n_buckets > 1");
    EXPECT_DEATH((Histogram_counter{0, 1}), "n_buckets > 1");
    EXPECT_DEATH((Histogram_counter{1, 1}), "n_buckets > 1");
    { // Check the count_for_bucket() bad-arg real-quick too.
      Histogram_counter histo{2, 1, 1, 0};
      EXPECT_EQ(histo.size(), 2);
      EXPECT_DEATH(histo.count_for_bucket(2), "idx < size\\(\\)");
      EXPECT_DEATH(histo.count_for_bucket(20000), "idx < size\\(\\)");
      [[maybe_unused]] auto ct = histo.count_for_bucket(0);
      ct = histo.count_for_bucket(1);
    }
    { // Same w/ default args.
      Histogram_counter histo{2, 1};
      EXPECT_EQ(histo.size(), 2);
      EXPECT_DEATH(histo.count_for_bucket(2), "idx < size\\(\\)");
      EXPECT_DEATH(histo.count_for_bucket(20000), "idx < size\\(\\)");
      [[maybe_unused]] auto ct = histo.count_for_bucket(0);
      ct = histo.count_for_bucket(1);
    }
    { [[maybe_unused]] Histogram_counter dummy{20000, 1, 1, 0}; }
    { [[maybe_unused]] Histogram_counter dummy{20000, 1}; }

    EXPECT_DEATH((Histogram_counter{2, 0, 1, 0}), "bucket0_sz > 0");
    { [[maybe_unused]] Histogram_counter dummy{2, 1, 1, 0}; }
    { [[maybe_unused]] Histogram_counter dummy{2, 20000, 1, 0}; }
    { [[maybe_unused]] Histogram_counter dummy{2, 1}; }
    { [[maybe_unused]] Histogram_counter dummy{2, 20000}; }

    EXPECT_DEATH((Histogram_counter{2, 1, 0, 0}), "bucket_sz > 0");
    { [[maybe_unused]] Histogram_counter dummy{2, 1, 1, 0}; }
    { [[maybe_unused]] Histogram_counter dummy{2, 1}; }
    { [[maybe_unused]] Histogram_counter dummy{2, 1, 20000, 0}; }
  }
#endif // #ifndef NDEBUG

  using value_t = Histogram_counter::value_t;
  const auto load_counts_by_outcome = [](const Histogram_counter& histo, value_t start, value_t inc) -> Counts
  {
    Counts counts(histo.size());
    size_t idx = 0;
    for (auto outcome = start; idx != counts.size();
         outcome += inc, ++idx)
    {
      counts[idx] = histo.count_for_bucket_containing_outcome(outcome);
    }
    return counts;
  };

  cout << "Basic test with 1-6 die, 6 buckets, no out-of-range events.\n" << flush;
  {
    constexpr size_t N = 6;

    Histogram_counter die{N, 1, 1, 1}; auto counts = load_counts(die);
    EXPECT_EQ(counts, (Counts{0, 0, 0, 0, 0, 0}));
    die.record_value(1); die.record_value(2); die.record_value(3);
    die.record_value(4); die.record_value(5); die.record_value(6); counts = load_counts(die);
    EXPECT_EQ(counts, (Counts{1, 1, 1, 1, 1, 1}));
    die.record_value(1); die.record_value(2); die.record_value(3);
    die.record_value(4); die.record_value(5); die.record_value(6); counts = load_counts(die);
    EXPECT_EQ(counts, (Counts{2, 2, 2, 2, 2, 2}));
    die.record_value(1); counts = load_counts(die);
    EXPECT_EQ(counts, (Counts{3, 2, 2, 2, 2, 2}));

    { // Output.
      Histogram_counter die2{die};
      die2.record_value(6); die2.record_value(6);
      die2.record_value(1); die2.record_value(1); die2.record_value(1); die2.record_value(1); die2.record_value(1);
      die2.record_value(1); die2.record_value(1); die2.record_value(1); die2.record_value(3);
      const auto counts2 = load_counts(die2);
      EXPECT_EQ(counts2, (Counts{11, 2, 3, 2, 2, 4}));
      EXPECT_EQ(lexical_cast<string>(die2), "..1..1[11]..2[2]..3[3]..4[2]..5[2]..6..[4]");
    }

    { // Copy.
      Histogram_counter die2{N, 1, 1, 1};
      die2.record_value(2); die2.record_value(4);
      Histogram_counter die3{die2};
      die2.record_value(4);
      die3.record_value(2); die3.record_value(6);
      auto counts2 = load_counts(die2); auto counts3 = load_counts(die3);
      EXPECT_EQ(counts2, (Counts{0, 1, 0, 2, 0, 0}));
      EXPECT_EQ(counts3, (Counts{0, 2, 0, 1, 0, 1}));
      Histogram_counter die4{N - 1, 1, 1, 1}; // This will be overwritten.
      die4.record_value(1); // This too.
      die4 = die3;
      die4.record_value(4); die4.record_value(4);
      die3.record_value(2); die3.record_value(6);
      auto counts4 = load_counts(die4); counts3 = load_counts(die3);
      EXPECT_EQ(counts4, (Counts{0, 2, 0, 3, 0, 1}));
      EXPECT_EQ(counts3, (Counts{0, 3, 0, 1, 0, 2}));
    }
    { // Move (equal to copy).
      Histogram_counter die2{N, 1, 1, 1};
      die2.record_value(2); die2.record_value(4);
      Histogram_counter die3{std::move(die2)};
      die2.record_value(4);
      die3.record_value(2); die3.record_value(6);
      auto counts2 = load_counts(die2); auto counts3 = load_counts(die3);
      EXPECT_EQ(counts2, (Counts{0, 1, 0, 2, 0, 0}));
      EXPECT_EQ(counts3, (Counts{0, 2, 0, 1, 0, 1}));
      Histogram_counter die4{N - 1, 1, 1, 1}; // This will be overwritten.
      die4.record_value(1); // This too.
      die4 = std::move(die3);
      die4.record_value(4); die4.record_value(4);
      die3.record_value(2); die3.record_value(6);
      auto counts4 = load_counts(die4); counts3 = load_counts(die3);
      EXPECT_EQ(counts4, (Counts{0, 2, 0, 3, 0, 1}));
      EXPECT_EQ(counts3, (Counts{0, 3, 0, 1, 0, 2}));
    }

    cout << "  High-count test (might hang for a bit; patience).\n" << flush;
    {
      /* Just test it to a high count (but not so high, such as with uint64_t/1, that this would hang for a long time).
       * In Linux + x86-64, at least, count_t is uint64_t as of this writing, so we'd never reach the max value.
       * (As of this writing Histogram_counter impl also foregoes protecting against overflow if indeed it is
       * uint64_t or wider.  Update: Due to certain internal atomic-related consideration, it now simply
       * static_assert()s that count_t is uint64_t essentially forever, and there is no overflow protection.) */
      using type_t = uint32_t; // We test the range of counts in this type, divided by RNG_DIVISOR.
      constexpr type_t RNG_DIVISOR = 10;
      static_assert(sizeof(type_t) <= sizeof(Histogram_counter::count_t), "Cannot count beyond the max count.");

      constexpr size_t IDX = 3;
      constexpr size_t MAX_CT = std::numeric_limits<type_t>::max() / RNG_DIVISOR;
      for (Histogram_counter::count_t ct = 2; ct != MAX_CT; ++ct)
      {
        die.record_value(IDX + 1);
      }
      counts = load_counts(die);
      EXPECT_EQ(counts, (Counts{3, 2, 2, MAX_CT, 2, 2}));
      /* Check the count-by-outcome accessor too (just here and below should be fine for now).
       * Since our buckets are each of length 1, the result should be the same. */
      EXPECT_EQ(load_counts_by_outcome(die, 1, 1), counts);

      die.record_value(IDX + 1); counts = load_counts(die);
#if 0 // As noted above, there is now no overflow protection, so forget all this.  Keeping for posterity just in case.
      if constexpr(is_same_v<type_t, Histogram_counter::count_t> && (RNG_DIVISOR == 1))
      {
        EXPECT_EQ(counts, (Counts{3, 2, 2, MAX_CT, 2, 2})); // Overflow reached: should not increment further.
      }
      else
      {
        EXPECT_EQ(counts, (Counts{3, 2, 2, MAX_CT + 1, 2, 2}));
      }
#else
      static_assert(sizeof(Histogram_counter::count_t) >= 8,
                    "We now assume Histogram_counter uses 64+-bit counters and need not contend with overflow.");
      EXPECT_EQ(counts, (Counts{3, 2, 2, MAX_CT + 1, 2, 2}));
#endif
      EXPECT_EQ(load_counts_by_outcome(die, 1, 1), counts);
    }
  } // Basic test with 1-6 die, 6 buckets, no out-of-range events.

  /* That tested various features of it; now test the basic thing (record_value()s, check count_for_bucket()s;
   * avoid exhaustive/repetitive testing like above) but with these complications not tested above:
   *   - Below-range outcomes go to bucket 0.
   *   - Above-range outcomes go to bucket N-1.
   *   - Bucket size > 1.
   *   - Bucket 0 size =/= bucket 1+ size.
   *   - Negative values (among buckets).
   *   - Negative values (among outcomes).
   *   - Overflow trouble, particularly when bucket0_val0 is negative, and an outcome is very positive. */

  // For ref: Histogram_counter(value_t n_buckets, value_t bucket0_sz, value_t bucket_sz = 1, value_t bucket0_val0 = 0)

  cout << "Testing more-advanced setups (out-of-range outcomes, bucket size 2+, negative outcomes/etc.).\n" << flush;
  {
    constexpr size_t N = 10;

    cout << "  Bucket 1 size = 2, after = 7, start at -15, N=[" << N << "].\n" << flush;
    // <-15 -14|-13..-7|-6..0|1..7|8..14|15..21|22..28|29..35|36..42|43..49>
    //  1       2        3    4    5     6      7      8      9      10
    Histogram_counter h1{N, 2, 7, -15}; auto counts1 = load_counts(h1);
    EXPECT_EQ(counts1, (Counts{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    h1.record_value(-20); h1.record_value(-20); h1.record_value(-16); h1.record_value(-16); h1.record_value(-15);
    h1.record_value(-14); h1.record_value(-14); h1.record_value(-14); h1.record_value(-14);
    h1.record_value(-13); h1.record_value(-13); h1.record_value(-13); counts1 = load_counts(h1);
    EXPECT_EQ(counts1, (Counts{9, 3, 0, 0, 0, 0, 0, 0, 0, 0}));
    h1.record_value(-13); h1.record_value(-12); h1.record_value(-11); h1.record_value(-10); h1.record_value(-9);
    h1.record_value(-8); h1.record_value(-7); h1.record_value(-6); h1.record_value(0);
    h1.record_value(1); h1.record_value(1); h1.record_value(2);
    h1.record_value(15); h1.record_value(22); h1.record_value(27); h1.record_value(42);
    h1.record_value(43); h1.record_value(48); h1.record_value(49); h1.record_value(50); counts1 = load_counts(h1);
    EXPECT_EQ(counts1, (Counts{9, 10, 2, 3, 0, 1, 2, 0, 1, 4}));

    cout << "  Bucket 1 size = 7, after = 2, start at -2, N=[" << N << "].\n" << flush;
    // <-2..4|5 6|7 8|9 10|11 12|13 14|15 16|17 18|19 20|21 22>
    //  1     2   3   4    5     6     7     8     9     10
    h1 = Histogram_counter{N, 7, 2, -2}; counts1 = load_counts(h1);
    EXPECT_EQ(counts1, (Counts{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    h1.record_value(-20); h1.record_value(-1);
    h1.record_value(21); h1.record_value(30000); counts1 = load_counts(h1);
    EXPECT_EQ(counts1, (Counts{2, 0, 0, 0, 0, 0, 0, 0, 0, 2}));
    EXPECT_EQ(lexical_cast<string>(h1), "..-2..4[2]..{7}..20[0]..22..[2]");
    h1.record_value(13); h1.record_value(13); h1.record_value(13);
    h1.record_value(15); h1.record_value(15); h1.record_value(16);
    h1.record_value(10); counts1 = load_counts(h1);
    EXPECT_EQ(counts1, (Counts{2, 0, 0, 1, 0, 3, 3, 0, 0, 2}));

    cout << "    Overflow-y stuff....\n" << flush;

    // Do a quick check against overflow trouble.  This one shouldn't be an issue but try it because why not.
    h1.record_value(std::numeric_limits<value_t>::min()); counts1 = load_counts(h1);
    EXPECT_EQ(counts1, (Counts{3, 0, 0, 1, 0, 3, 3, 0, 0, 2}));
    /* This one however can be problematic, if record_value() isn't careful about overflow math.
     * Internally it needs to do value_t.max - (-2), which could wrap around and record it in bucket 0 (wrong)
     * instead of last-bucket (correct). */
    h1.record_value(std::numeric_limits<value_t>::max()); counts1 = load_counts(h1);
    EXPECT_EQ(counts1, (Counts{3, 0, 0, 1, 0, 3, 3, 0, 0, 3}));
    EXPECT_EQ(lexical_cast<string>(h1), "..-2..4[3]..{1}..8[0]..10[1]..12[0]..14[3]..16[3]..{1}..20[0]..22..[3]");
  } // Testing more-advanced setups (out-of-range outcomes, bucket size 2+, negative outcomes/etc.).

  cout << "Testing 2-bucket histograms (payload-size-histogram-like configs).\n" << flush;
  {
    // Simulates max_payload_size=1, bucket_sz=1: bucket 0 = [0, 0], bucket 1 = [1, 1].
    cout << "  N = 2, bucket0_sz = 1, bucket_sz = 1, start = 0.\n" << flush;
    {
      Histogram_counter h{2, 1, 1, 0}; auto counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{0, 0}));
      h.record_value(0); counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{1, 0}));
      h.record_value(1); h.record_value(1); h.record_value(5); counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{1, 3}));
      EXPECT_EQ(lexical_cast<string>(h), "..0..0[1]..1..[3]");
    }
    // Simulates max_payload_size=500, bucket_sz=1024: bucket 0 = [0, 499], bucket 1 = [500, 1523].
    cout << "  N = 2, bucket0_sz = 500, bucket_sz = 1024, start = 0.\n" << flush;
    {
      Histogram_counter h{2, 500, 1024, 0}; auto counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{0, 0}));
      h.record_value(0); h.record_value(250); h.record_value(499); counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{3, 0}));
      h.record_value(500); h.record_value(500); h.record_value(1000); counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{3, 3}));
      EXPECT_EQ(lexical_cast<string>(h), "..0..499[3]..1523..[3]");
    }
    // Simulates max_payload_size=1024, bucket_sz=1024 (uniform): bucket 0 = [0, 1023], bucket 1 = [1024, 2047].
    cout << "  N = 2, bucket0_sz = 1024, bucket_sz = 1024, start = 0.\n" << flush;
    {
      Histogram_counter h{2, 1024, 1024, 0}; auto counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{0, 0}));
      h.record_value(0); h.record_value(512); h.record_value(1023); counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{3, 0}));
      h.record_value(1024); counts = load_counts(h);
      EXPECT_EQ(counts, (Counts{3, 1}));
      EXPECT_EQ(lexical_cast<string>(h), "..0..1023[3]..2047..[1]");
    }
  } // Testing 2-bucket histograms (payload-size-histogram-like configs).
} // TEST(Stats_histogram_counter_test, Interface)

TEST(Stats_histogram_counter_test, Clear)
{
  cout << "clear() on a fresh histogram is a no-op (all counts already 0).\n" << flush;
  {
    constexpr size_t N = 6;
    Histogram_counter h{N, 1, 1, 1};
    EXPECT_EQ(load_counts(h), (Counts{0, 0, 0, 0, 0, 0}));
    h.clear();
    EXPECT_EQ(load_counts(h), (Counts{0, 0, 0, 0, 0, 0}));
    // clear() again: still fine.
    h.clear();
    EXPECT_EQ(load_counts(h), (Counts{0, 0, 0, 0, 0, 0}));
  }

  cout << "clear() zeroes counts, preserves bucket structure, and subsequent "
          "record_value()s route correctly.\n" << flush;
  {
    /* Non-trivial config:
     *   - N = 10, bucket 0 width = 2, bucket 1+ width = 7, start at -15.
     *     <-15 -14|-13..-7|-6..0|1..7|8..14|15..21|22..28|29..35|36..42|43..49> */
    constexpr size_t N = 10;
    Histogram_counter h{N, 2, 7, -15};

    // Record a variety -- including below-range and above-range outcomes.
    h.record_value(-20); h.record_value(-15); h.record_value(-14); // last two: bucket 0.
    h.record_value(-13); h.record_value(-7); // bucket 1.
    h.record_value(0); // bucket 2.
    h.record_value(49); h.record_value(1000); // last bucket.
    const auto counts_pre_clear = load_counts(h);
    EXPECT_EQ(counts_pre_clear, (Counts{3, 2, 1, 0, 0, 0, 0, 0, 0, 2}));

    h.clear();
    EXPECT_EQ(load_counts(h), (Counts{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));

    /* Bucket layout must survive clear(): re-record the same values and expect the
     * same counts as pre-clear. */
    h.record_value(-20); h.record_value(-15); h.record_value(-14);
    h.record_value(-13); h.record_value(-7);
    h.record_value(0);
    h.record_value(49); h.record_value(1000);
    EXPECT_EQ(load_counts(h), counts_pre_clear);
  }

  cout << "clear() + record via record_value() leaves to_ostream() reporting fresh counts "
          "with the original bucket labels.\n" << flush;
  {
    // From the Interface test: 2-bucket config, string rendering is deterministic.
    Histogram_counter h{2, 500, 1024, 0};
    h.record_value(100); h.record_value(600); h.record_value(600);
    EXPECT_EQ(lexical_cast<string>(h), "..0..499[1]..1523..[2]");

    h.clear();
    // Labels preserved; all counts 0.
    EXPECT_EQ(lexical_cast<string>(h), "..0..499[0]..1523..[0]");

    // Re-record differently; verify counts reflect only post-clear activity.
    h.record_value(0); h.record_value(499); h.record_value(500);
    EXPECT_EQ(lexical_cast<string>(h), "..0..499[2]..1523..[1]");
  }
} // TEST(Stats_histogram_counter_test, Clear)

/* The printing algorithm, verified deliberately (not merely via the opportunistic spot-checks in other TESTs
 * here): (1) the general look; (2) the 0-sequence compaction: any maximal all-zero-count bucket-run of
 * length `N + 1` -- never including bucket 0, but possibly ending at the last bucket -- prints as
 * `..{N}..x[0]` (`x` being the normal label of the run's last bucket; `N >= 1`); a length-1 zero-run prints
 * normally (no `{N}`).  Bucket 0 and the last bucket always print, whatever their counts. */
TEST(Stats_histogram_counter_test, Printing)
{
  /* The workhorse: 6 buckets <(-inf)..-1..1|2..6|7..11|12..16|17..21|22..26..(+inf)>; so the labels are:
   * bucket 0 = "..-1..1", middles = "..6", "..11", "..16", "..21", last = "..26..".
   * (Mirrors the worked example in Histogram_counter::to_ostream()'s impl comment, extended to 6 buckets.) */
  Histogram_counter h{6, 3, 5, -1};
  const auto set_counts = [&](const Counts& counts)
  {
    for (size_t idx = 0; idx != counts.size(); ++idx)
    {
      h.overwrite_count_for_bucket(idx, counts[idx]);
    }
  };

  // (1) The general look, no zeroes anywhere.
  set_counts({1, 2, 3, 4, 5, 6});
  EXPECT_EQ(lexical_cast<string>(h), "..-1..1[1]..6[2]..11[3]..16[4]..21[5]..26..[6]");

  /* (2) The compaction, corner case by corner case:
   * Length-1 zero-run (middle): printed normally; no {N}. */
  set_counts({1, 0, 3, 4, 5, 6});
  EXPECT_EQ(lexical_cast<string>(h), "..-1..1[1]..6[0]..11[3]..16[4]..21[5]..26..[6]");

  // Length-2 zero-run (middle): the minimal compaction, {1}.
  set_counts({1, 0, 0, 4, 5, 6});
  EXPECT_EQ(lexical_cast<string>(h), "..-1..1[1]..{1}..11[0]..16[4]..21[5]..26..[6]");

  // Length-3 zero-run (middle): {2}.
  set_counts({1, 0, 0, 0, 5, 6});
  EXPECT_EQ(lexical_cast<string>(h), "..-1..1[1]..{2}..16[0]..21[5]..26..[6]");

  // Two separate runs, one compactable and one not: each treated independently.
  set_counts({1, 0, 0, 4, 0, 6});
  EXPECT_EQ(lexical_cast<string>(h), "..-1..1[1]..{1}..11[0]..16[4]..21[0]..26..[6]");

  // Zero-run ending at the last bucket: the last bucket always prints -- it terminates the run.
  set_counts({1, 2, 0, 0, 0, 0});
  EXPECT_EQ(lexical_cast<string>(h), "..-1..1[1]..6[2]..{3}..26..[0]");

  // Bucket 0 is never compacted-into: a zero-run "starting" there really starts at bucket 1.
  set_counts({0, 0, 0, 4, 5, 6});
  EXPECT_EQ(lexical_cast<string>(h), "..-1..1[0]..{1}..11[0]..16[4]..21[5]..26..[6]");

  // All-zero: bucket 0 printed, the bucket-1-through-last run compacted, last bucket terminating it.
  set_counts({0, 0, 0, 0, 0, 0});
  EXPECT_EQ(lexical_cast<string>(h), "..-1..1[0]..{4}..26..[0]");

  // 2 buckets total => no middle => no compaction is ever possible; both always print.
  Histogram_counter h2{2, 3, 5, -1};
  EXPECT_EQ(lexical_cast<string>(h2), "..-1..1[0]..6..[0]");

  // The general-scale ctor form prints identically-formatted output (labels from its explicit bounds).
  Histogram_counter hg{{5, 8}, 3};
  hg.record_value(6); hg.record_value(9);
  EXPECT_EQ(lexical_cast<string>(hg), "..5..7[1]..10..[1]");
} // TEST(Stats_histogram_counter_test, Printing)

/* The general-scale (arbitrary-bucket-bounds) ctor form: Histogram_counter{vector-of-min-values, last-width}.
 * Cases: mixed widths (logarithmic-time path); all-equal widths via this ctor form (constant-time path --
 * different internally, same contract); the boundary semantics at each edge including underflow into bucket 0
 * and overflow into the last bucket; and the printout. */
TEST(Stats_histogram_counter_test, General_scale)
{
  // Buckets: <(-inf)..-14|-13..-7|-6..0|1..17|18..27|28..29|30..128|129..135|136..143|144..149+(inf)>
  Histogram_counter h{{-15, -13, -6, 1, 18, 28, 30, 129, 136, 144}, 6};
  EXPECT_EQ(h.size(), 10);
  EXPECT_EQ(load_counts(h), (Counts{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));

  // Bucket-0 = its range plus all underflow.
  h.record_value(-1000); h.record_value(-16); h.record_value(-15); h.record_value(-14);
  EXPECT_EQ(load_counts(h), (Counts{4, 0, 0, 0, 0, 0, 0, 0, 0, 0}));

  // Every boundary, from both sides.
  h.record_value(-13); h.record_value(-7); // Bucket 1: its min + its max.
  h.record_value(-6); h.record_value(0); // Bucket 2: ditto.
  h.record_value(1); h.record_value(17); // Bucket 3.
  h.record_value(18); h.record_value(27); // Bucket 4.
  h.record_value(28); h.record_value(29); // Bucket 5 (the narrowest: width 2).
  h.record_value(30); h.record_value(128); // Bucket 6 (the widest: width 99).
  h.record_value(129); h.record_value(135); // Bucket 7.
  h.record_value(136); h.record_value(143); // Bucket 8.
  EXPECT_EQ(load_counts(h), (Counts{4, 2, 2, 2, 2, 2, 2, 2, 2, 0}));

  // Last bucket = [min, min + last-width - 1] = [144, 149] -- plus all overflow.
  h.record_value(144); h.record_value(149); h.record_value(150); h.record_value(100000);
  EXPECT_EQ(load_counts(h), (Counts{4, 2, 2, 2, 2, 2, 2, 2, 2, 4}));

  // Printout: bucket-0 shows its formal min; last bucket its formal max; zero-runs compacted.
  cout << "  h = [" << h << "].\n" << flush;

  /* Same ctor form but all-equal widths: the ctor detects this and uses the constant-time (arithmetic)
   * record_value() path -- different machinery, same contract. */
  Histogram_counter hu{{0, 10, 20, 30}, 10}; // 4 buckets, uniform width 10, spanning [0, 39] + under/overflow.
  hu.record_value(-1); hu.record_value(0); hu.record_value(9); // All bucket 0.
  hu.record_value(10); hu.record_value(19); // Bucket 1.
  hu.record_value(35); hu.record_value(39); hu.record_value(40); hu.record_value(999); // Bucket 3 (2 overflow).
  EXPECT_EQ(load_counts(hu), (Counts{3, 2, 0, 4}));

  // Minimal size: 2 buckets.
  Histogram_counter h2{{5, 8}, 3}; // <..(-inf)..7|8..10+(inf)..>
  h2.record_value(4); h2.record_value(7); h2.record_value(8); h2.record_value(10); h2.record_value(11);
  EXPECT_EQ(load_counts(h2), (Counts{2, 3}));
} // TEST(Stats_histogram_counter_test, General_scale)

/* The remaining, smaller Histogram_counter aspects: copy ctor/assignment; operator-=;
 * overwrite_count_for_bucket(); count_for_bucket_containing_outcome(); record_period(). */
TEST(Stats_histogram_counter_test, Aux_interface)
{
  using boost::chrono::milliseconds;
  using boost::chrono::microseconds;
  using boost::chrono::seconds;

  Histogram_counter h{6, 1, 1, 1}; // Buckets: <..1|2|3|4|5|6..>.
  h.record_value(1); h.record_value(2); h.record_value(2); h.record_value(6);
  EXPECT_EQ(load_counts(h), (Counts{1, 2, 0, 0, 0, 1}));

  // Copy ctor and copy assignment: counts (and bucket structure) travel; the copies are independent.
  Histogram_counter h_copy{h};
  EXPECT_EQ(load_counts(h_copy), load_counts(h));
  Histogram_counter h_assigned{2, 1}; // Different structure entirely; assignment replaces it wholesale.
  h_assigned = h;
  EXPECT_EQ(h_assigned.size(), 6);
  EXPECT_EQ(load_counts(h_assigned), load_counts(h));
  h_copy.record_value(3);
  EXPECT_EQ(h.count_for_bucket(2), 0); // Original unaffected.

  // operator-=: per-bucket subtraction (the stats_since_reset_state() workhorse).
  Histogram_counter h_base{6, 1, 1, 1};
  h_base.record_value(2);
  h -= h_base;
  EXPECT_EQ(load_counts(h), (Counts{1, 1, 0, 0, 0, 1}));

  // overwrite_count_for_bucket(): count_for_bucket(idx) == new_count afterward, other buckets untouched.
  h.overwrite_count_for_bucket(2, 42);
  h.overwrite_count_for_bucket(5, 0);
  EXPECT_EQ(load_counts(h), (Counts{1, 1, 42, 0, 0, 0}));

  // count_for_bucket_containing_outcome(): the value-indexed read of the same counts.
  EXPECT_EQ(h.count_for_bucket_containing_outcome(3), 42);
  EXPECT_EQ(h.count_for_bucket_containing_outcome(-50), 1); // Underflow => bucket 0.
  EXPECT_EQ(h.count_for_bucket_containing_outcome(1000), 0); // Overflow => last bucket.

  // record_period(): converts (rounding to nearest) to the histogram's declared time-unit, then records.
  Histogram_counter ht{4, 100, 100, 0}; // Millisecond-scale: <..99|100..199|200..299|300..>.
  ht.record_period<milliseconds>(microseconds(50400)); // 50.4 ms -> 50 -> bucket 0.
  ht.record_period<milliseconds>(microseconds(150400)); // 150.4 ms -> 150 -> bucket 1.
  ht.record_period<milliseconds>(milliseconds(250)); // Same-unit passthrough -> bucket 2.
  ht.record_period<milliseconds>(seconds(2)); // 2000 ms -> overflow bucket.
  EXPECT_EQ(load_counts(ht), (Counts{1, 1, 1, 1}));
} // TEST(Stats_histogram_counter_test, Aux_interface)

// --- Element-op and Stat_set-wrangling tests (as opposed to the Histogram_counter tests above) ---

/* The Stat_set specimens, per the namespace doc header's pattern.  Deliberately diverse:
 *   - Test_stats: plain (non-atomic) members, ACCUMULATORs only, default-constructible.
 *   - Atomic_stats: all-atomic members + a histogram, every Stat_type represented -- and *no* default ctor
 *     (nothing in the stats_*() APIs may rely on one).
 *   - Plain_stats: every Stat_type represented but all members non-atomic (the single-threaded-collection
 *     use case) -- the stats_*() APIs must compile and behave for these too. */

struct Test_stats
{
  uint64_t m_msg_count = 0;
  uint64_t m_byte_count = 0;
};

template<typename Visitor>
void declare_stats(std::string name_prefix, const Test_stats* src_stats, Test_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_msg_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_byte_count, ACCUMULATOR);
}

struct Atomic_stats
{
  atomic<unsigned int> m_acc = 0;
  atomic<int> m_gauge = 0;
  atomic<int> m_hwm = 0;
  Histogram_counter m_histo{6, 1, 1, 1}; // Buckets: <..1|2|3|4|5|6..>.

  Atomic_stats([[maybe_unused]] size_t dummy) {} // No default ctor (nothing can rely on it in stats_*()).
};

template<typename Visitor>
void declare_stats(std::string name_prefix, const Atomic_stats* src_stats, Atomic_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_acc, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_gauge, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_hwm, m_gauge);
  FLOW_UTIL_STAT_DECLARE(m_histo, ACCUMULATOR);
}

struct Plain_stats
{
  uint64_t m_acc = 0;
  int m_gauge = 0;
  int m_hwm = 0;
  Histogram_counter m_histo{4, 1, 1, 1}; // Buckets: <..1|2|3|4..>.
};

template<typename Visitor>
void declare_stats(std::string name_prefix, const Plain_stats* src_stats, Plain_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_acc, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_gauge, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_hwm, m_gauge);
  FLOW_UTIL_STAT_DECLARE(m_histo, ACCUMULATOR);
}

/* For the composition test: Compo_stats exercises, all at once, every way a Stat_set can incorporate
 * stats -- see its declare_stats() -- plus a deliberately-undeclared member. */

struct Compo_leaf_stats
{
  uint64_t m_leaf_acc = 0;
  int m_leaf_gauge = 0;
};

template<typename Visitor>
void declare_stats(std::string name_prefix, const Compo_leaf_stats* src_stats, Compo_leaf_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_leaf_acc, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_leaf_gauge, GAUGE);
}

struct Compo_stats
{
  struct Nested // Organizational-only nested sub-struct: declared via dotted paths, not composition.
  {
    uint64_t m_n_events = 0;
  } m_nested;
  Compo_leaf_stats m_plain_leaf; // Real composition, empty prefix.
  Compo_leaf_stats m_sub_leaf; // Real composition, "sub." prefix (prefix = what disambiguates the names).
  uint64_t m_direct_acc = 0; // Normal direct member.
  int m_untracked = 7; // Not declared below: stats_*() ops must ignore it entirely.
};

template<typename Visitor>
void declare_stats(std::string name_prefix, const Compo_stats* src_stats, Compo_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_nested.m_n_events, ACCUMULATOR);
  declare_stats(name_prefix,
                src_stats ? &src_stats->m_plain_leaf : nullptr,
                target_stats ? &target_stats->m_plain_leaf : nullptr,
                visitor);
  declare_stats(name_prefix + "sub.",
                src_stats ? &src_stats->m_sub_leaf : nullptr,
                target_stats ? &target_stats->m_sub_leaf : nullptr,
                visitor);
  FLOW_UTIL_STAT_DECLARE(m_direct_acc, ACCUMULATOR);
}

/* The element-level ops -- the vocabulary in which the stats_*() Stat_set-level ops (and users' own
 * stat-keeping code) are written.  Each has a plain-T overload and (except load()/exchange() which differ
 * in kind) an atomic<T> overload; the atomic mutators return the pre-op value, the plain ones return void. */
TEST(Stats_element_ops_test, Interface)
{
  // Plain T.
  int x = 5;
  EXPECT_EQ(load(x), 5);
  store(&x, 7);
  EXPECT_EQ(x, 7);
  fetch_add(&x, 3);
  EXPECT_EQ(x, 10);
  fetch_sub(&x, 4);
  EXPECT_EQ(x, 6);
  update_hi_wmark(&x, 5); // Lower: no effect.
  EXPECT_EQ(x, 6);
  update_hi_wmark(&x, 6); // Equal: no effect.
  EXPECT_EQ(x, 6);
  update_hi_wmark(&x, 9); // Higher: raises.
  EXPECT_EQ(x, 9);
  reset(&x, 42);
  EXPECT_EQ(x, 42);

  // atomic<T>.
  atomic<int> a{5};
  EXPECT_EQ(load(a), 5);
  store(&a, 7);
  EXPECT_EQ(load(a), 7);
  EXPECT_EQ(fetch_add(&a, 3), 7); // Returns pre-op value.
  EXPECT_EQ(load(a), 10);
  EXPECT_EQ(fetch_sub(&a, 4), 10); // Ditto.
  EXPECT_EQ(load(a), 6);
  EXPECT_EQ(exchange(&a, 100), 6); // Ditto.
  EXPECT_EQ(load(a), 100);
  update_hi_wmark(&a, 99);
  EXPECT_EQ(load(a), 100);
  update_hi_wmark(&a, 101);
  EXPECT_EQ(load(a), 101);

  /* reset(Histogram_counter*): the dedicated overload *clears* the target; the fresh-value's own counts are
   * deliberately irrelevant (an all-zero histogram is the only sane fresh state; see its doc header). */
  Histogram_counter h{4, 1, 1, 1};
  h.record_value(2);
  Histogram_counter fresh_bait{4, 1, 1, 1};
  fresh_bait.record_value(3); // Bait: must be ignored...
  reset(&h, fresh_bait);
  EXPECT_EQ(load_counts(h), (Counts{0, 0, 0, 0})); // ...and indeed: cleared, not overwritten-with-bait.
} // TEST(Stats_element_ops_test, Interface)

// stats_to_ostream() + the print() sugar.
TEST(Stats_stat_set_test, Pretty_print)
{
  Test_stats stats;
  stats.m_msg_count = 42;
  stats.m_byte_count = 1024;

  // Test stats_to_ostream() directly.
  ostringstream os;
  stats_to_ostream(os, stats);

  const auto result = os.str();
  cout << "stats_to_ostream() output: [" << result << "].\n" << flush;

  EXPECT_NE(result.find("msg_count=[42]"), string::npos);
  EXPECT_NE(result.find("byte_count=[1024]"), string::npos);

  // Test print() proxy -- uses stats_to_ostream() internally; just verify chaining works.
  ostringstream os2;
  os2 << "before [" << print(stats) << "] after";

  const auto result2 = os2.str();
  cout << "print() output: [" << result2 << "].\n" << flush;

  EXPECT_EQ(result2.substr(0, String_view{"before ["}.size()), "before [");
  EXPECT_NE(result2.find("] after"), string::npos);
  EXPECT_NE(result2.find("msg_count=[42]"), string::npos);

  // All-default (zeroed) struct.
  Test_stats defaults;

  ostringstream os3;
  stats_to_ostream(os3, defaults);

  const auto result3 = os3.str();
  cout << "stats_to_ostream() default output: [" << result3 << "].\n" << flush;

  EXPECT_NE(result3.find("msg_count=[0]"), string::npos);
  EXPECT_NE(result3.find("byte_count=[0]"), string::npos);
} // TEST(Stats_stat_set_test, Pretty_print)

/* stats_field_names(): reflection off declare_stats() -- names in declaration (= print) order; and the
 * ctor-args form for a Stat_set lacking a default ctor. */
TEST(Stats_stat_set_test, Field_names)
{
  EXPECT_EQ(stats_field_names<Test_stats>(), (vector<string>{"msg_count", "byte_count"}));
  EXPECT_EQ(stats_field_names<Plain_stats>(), (vector<string>{"acc", "gauge", "hwm", "histo"}));
  // Atomic_stats has no default ctor: the args are forwarded to whatever public ctor exists.
  EXPECT_EQ(stats_field_names<Atomic_stats>(size_t{0}), (vector<string>{"acc", "gauge", "hwm", "histo"}));
} // TEST(Stats_stat_set_test, Field_names)

/* stats_reset() (per-Stat_type semantics, including the histogram-clears special) and stats_assign()
 * (the copy that works despite non-copyable atomic members). */
TEST(Stats_stat_set_test, Reset_and_assign)
{
  Atomic_stats a{{}};
  store(&a.m_acc, 7);
  store(&a.m_gauge, 3);
  store(&a.m_hwm, 12);
  a.m_histo.record_value(2);

  // stats_assign() first (while `a` is interesting): everything copies, element-wise.
  Atomic_stats b{{}};
  stats_assign(&b, a);
  EXPECT_EQ(load(b.m_acc), 7);
  EXPECT_EQ(load(b.m_gauge), 3);
  EXPECT_EQ(load(b.m_hwm), 12);
  EXPECT_EQ(load_counts(b.m_histo), (Counts{0, 1, 0, 0, 0, 0}));

  /* stats_reset(): ACCUMULATOR := fresh value; GAUGE untouched (the measured state is the measured state);
   * HI_WMARK := its gauge (new measurement period: current == highest -- note it may thus *decrease*);
   * histogram-ACCUMULATOR: cleared via the ADL reset() overload -- the fresh histogram's counts are
   * irrelevant (bait below proves it). */
  Atomic_stats fresh{{}};
  store(&fresh.m_acc, 100); // Nonzero fresh ACC: legal, copied.
  fresh.m_histo.record_value(5); // Bait: must be ignored.
  stats_reset(&a, fresh);
  EXPECT_EQ(load(a.m_acc), 100);
  EXPECT_EQ(load(a.m_gauge), 3);
  EXPECT_EQ(load(a.m_hwm), 3); // Decreased from 12: correct (max-since-reset, and the reset is now).
  EXPECT_EQ(load_counts(a.m_histo), (Counts{0, 0, 0, 0, 0, 0}));
} // TEST(Stats_stat_set_test, Reset_and_assign)

/* stats_aggregate_one(), stats_aggregate(), stats_sum() -- and their differing treatment of the types:
 * ACCUMULATORs always sum; HI_WMARKs max under aggregation but *sum* under summing; GAUGEs sum under
 * aggregation-one and summing, but aggregate() finishes them with /= n (mean across the peers). */
TEST(Stats_stat_set_test, Aggregate_and_sum)
{
  deque<Atomic_stats> srcs; // (deque<>: various vector<> paths would need Atomic_stats movability.)
  srcs.emplace_back(int{});
  srcs.emplace_back(int{});
  auto& s1 = srcs[0];
  auto& s2 = srcs[1];
  store(&s1.m_acc, 10);
  store(&s1.m_gauge, 4);
  store(&s1.m_hwm, 9);
  s1.m_histo.record_value(1);
  s1.m_histo.record_value(2);
  store(&s2.m_acc, 30);
  store(&s2.m_gauge, 8);
  store(&s2.m_hwm, 5);
  s2.m_histo.record_value(4);

  { // stats_aggregate_one(): += for ACC/GAUGE (and histo); max for HWM.
    Atomic_stats t{{}};
    stats_assign(&t, s1);
    stats_aggregate_one(&t, s2);
    EXPECT_EQ(load(t.m_acc), 40);
    EXPECT_EQ(load(t.m_gauge), 12);
    EXPECT_EQ(load(t.m_hwm), 9);
    EXPECT_EQ(load_counts(t.m_histo), (Counts{1, 1, 0, 1, 0, 0}));
  }
  { // stats_aggregate(): as above across the range -- then GAUGE /= n: the mean.
    Atomic_stats t{{}};
    stats_aggregate(&t, srcs.begin(), srcs.end());
    EXPECT_EQ(load(t.m_acc), 40);
    EXPECT_EQ(load(t.m_gauge), 6); // (4 + 8) / 2.
    EXPECT_EQ(load(t.m_hwm), 9);
    EXPECT_EQ(load_counts(t.m_histo), (Counts{1, 1, 0, 1, 0, 0}));
  }
  { // stats_aggregate() over a 1-element range: assign + the (here-trivial) /= 1.
    Atomic_stats t{{}};
    stats_aggregate(&t, srcs.begin(), std::next(srcs.begin()));
    EXPECT_EQ(load(t.m_acc), 10);
    EXPECT_EQ(load(t.m_gauge), 4);
    EXPECT_EQ(load(t.m_hwm), 9);
  }
  { // stats_sum(): += for *everything* -- notably the HWMs too (peers' maxima add, not max).
    Atomic_stats t{{}};
    stats_sum(&t, srcs.begin(), srcs.end());
    EXPECT_EQ(load(t.m_acc), 40);
    EXPECT_EQ(load(t.m_gauge), 12);
    EXPECT_EQ(load(t.m_hwm), 14); // 9 + 5: the sum/aggregate distinction, asserted exactly.
    EXPECT_EQ(load_counts(t.m_histo), (Counts{1, 1, 0, 1, 0, 0}));
  }
} // TEST(Stats_stat_set_test, Aggregate_and_sum)

/* stats_aggregate_shards() + stats_reset_shard_aggregate(): the shard-consumption pattern (see
 * stats_aggregate_shards() doc header).  Both their non-degenerate paths and the degenerate empty-range
 * (fresh_stats_from_0_shards) paths -- entirely separate code -- are covered. */
TEST(Stats_stat_set_test, Aggregate_shards)
{
  Atomic_stats agg{{}};
  deque<Atomic_stats> shards; // (deque<>: various vector<> paths would need Atomic_stats movability.)
  shards.emplace_back(int{});
  shards.emplace_back(int{});
  auto& s1 = shards[0];
  auto& s2 = shards[1];

  store(&s1.m_acc, 12);
  store(&s1.m_gauge, -3);
  s1.m_histo.record_value(1);
  s1.m_histo.record_value(4);
  store(&s2.m_acc, 44);
  store(&s2.m_gauge, 8);
  s2.m_histo.record_value(2);
  s2.m_histo.record_value(4);

  ASSERT_EQ(agg.m_acc, 0); ASSERT_EQ(agg.m_gauge, 0); ASSERT_EQ(agg.m_hwm, 0);
  ASSERT_EQ(load_counts(agg.m_histo), (Counts{0, 0, 0, 0, 0, 0}));

  stats_aggregate_shards<Atomic_stats>(&agg, shards.begin(), shards.end(), nullptr);
  cout << "\n[" << print(s1) << "]\n +\n[" << print(s2) << "]\n =>\n[" << print(agg) << "].\nHWM init-set.\n" << flush;
  EXPECT_EQ(agg.m_acc, 56) << "ACC summed."; EXPECT_EQ(agg.m_gauge, 5) << "GAUGE also summed (shards)";
  EXPECT_EQ(agg.m_hwm, 5) << "HWM<GAUGE, prev value 0, now 5.";
  EXPECT_EQ(load_counts(agg.m_histo), (Counts{1, 1, 0, 2, 0, 0})) << "HISTO also summed.";

  store(&s1.m_acc, 11); // Not too realistic for an ACC to be stored-to, let alone decrease, but so what?
  store(&s1.m_gauge, -4);
  s1.m_histo.record_value(1);
  s1.m_histo.record_value(4);
  store(&s2.m_acc, 43);
  store(&s2.m_gauge, 7);
  s2.m_histo.record_value(2);
  s2.m_histo.record_value(4);

  stats_aggregate_shards<Atomic_stats>(&agg, shards.begin(), shards.end(), nullptr);
  EXPECT_EQ(agg.m_acc, 54) << "ACC re-summed."; EXPECT_EQ(agg.m_gauge, 3) << "GAUGE also re-summed (shards)";
  EXPECT_EQ(agg.m_hwm, 5) << "HWM>=GAUGE, prev value 5, stays 5.";
  EXPECT_EQ(load_counts(agg.m_histo), (Counts{2, 2, 0, 4, 0, 0})) << "HISTO inc-ed; also re-summed.";

  store(&s1.m_acc, 11);
  store(&s1.m_gauge, -2);
  s1.m_histo.record_value(2);
  s1.m_histo.record_value(4);
  store(&s2.m_acc, 43);
  store(&s2.m_gauge, 9);
  s2.m_histo.record_value(2);
  s2.m_histo.record_value(4);

  stats_aggregate_shards<Atomic_stats>(&agg, shards.begin(), shards.end(), nullptr);
  EXPECT_EQ(agg.m_acc, 54) << "ACC re-re-summed."; EXPECT_EQ(agg.m_gauge, 7) << "GAUGE also re-re-summed (shards)";
  EXPECT_EQ(agg.m_hwm, 7) << "HWM<GAUGE, prev value 5, now 7.";
  EXPECT_EQ(load_counts(agg.m_histo), (Counts{2, 4, 0, 6, 0, 0})) << "HISTO re-inc-ed; also re-re-summed.";

  /* stats_reset_shard_aggregate(): a reset in the sharded world.  Per its doc header: each *shard's*
   * ACCUMULATORs (including histogram) reset; shard GAUGEs untouched (a reset never affects a gauge); the
   * target's gauges get the current sums, and its HWMs := those sums (new measurement period -- and so an
   * HWM may decrease, as here).  (The target's own ACCs are explicitly dont-care per the doc header: the
   * next aggregate_shards() overwrites them; hence no assert on agg.m_acc here.) */
  store(&s1.m_gauge, -3);
  stats_reset_shard_aggregate(&agg, shards.begin(), shards.end(), Atomic_stats{{}});
  cout << "\n[" << print(s1) << "]\n +\n[" << print(s2) << "]\n =>\n[" << print(agg) << "].\nDid reset.\n" << flush;
  EXPECT_EQ(s1.m_acc, 0); EXPECT_EQ(s2.m_acc, 0);
  EXPECT_EQ(load_counts(s1.m_histo), (Counts{0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(load_counts(s2.m_histo), (Counts{0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(s1.m_gauge, -3); EXPECT_EQ(s2.m_gauge, 9);
  EXPECT_EQ(agg.m_gauge, 6) << "Summed as-if consumed (see doc header).";
  EXPECT_EQ(agg.m_hwm, 6) << "HWM := summed gauge; decreased from 7: new measurement period.";

  // And the consume after the reset: everything fresh, gauges live on.
  stats_aggregate_shards<Atomic_stats>(&agg, shards.begin(), shards.end(), nullptr);
  EXPECT_EQ(agg.m_acc, 0);
  EXPECT_EQ(agg.m_gauge, 6);
  EXPECT_EQ(agg.m_hwm, 6);
  EXPECT_EQ(load_counts(agg.m_histo), (Counts{0, 0, 0, 0, 0, 0}));

  /* The degenerate paths: an empty shard-range with fresh_stats_from_0_shards -- e.g., stats consumed
   * before any mutation thread has existed.  ACC/GAUGE := fresh (histogram: cleared -- the fresh one's
   * counts are bait below); HWM := max(its prior value, the gauge): it survives the shardless stretch. */
  Atomic_stats fresh{{}};
  store(&fresh.m_acc, 5);
  store(&fresh.m_gauge, 2);
  fresh.m_histo.record_value(3); // Bait: must be ignored.

  Atomic_stats t{{}};
  store(&t.m_acc, 77);
  store(&t.m_gauge, 50);
  store(&t.m_hwm, 100);
  t.m_histo.record_value(1);
  stats_aggregate_shards<Atomic_stats>(&t, shards.end(), shards.end(), &fresh);
  EXPECT_EQ(t.m_acc, 5);
  EXPECT_EQ(t.m_gauge, 2);
  EXPECT_EQ(t.m_hwm, 100) << "Prior HWM higher than gauge: persists.";
  EXPECT_EQ(load_counts(t.m_histo), (Counts{0, 0, 0, 0, 0, 0}));

  store(&t.m_hwm, 1);
  stats_aggregate_shards<Atomic_stats>(&t, shards.end(), shards.end(), &fresh);
  EXPECT_EQ(t.m_hwm, 2) << "Prior HWM lower than gauge: raised to it.";

  stats_reset_shard_aggregate(&t, shards.end(), shards.end(), fresh);
  EXPECT_EQ(t.m_gauge, 2);
  EXPECT_EQ(t.m_hwm, 2) << "Reset: HWM := gauge, even shardless.";
} // TEST(Stats_stat_set_test, Aggregate_shards)

/* stats_since_reset_state() + stats_mark_reset_state(): the consume-relative-to-reset machinery.
 * The scenarios walk the HI_WMARK induction: the reset-state's HWM slot carries the
 * highest-sampled-gauge-so-far, each consume folds the newly-observed gauge into it (both into the
 * consumed-out value and, for next time, back into the reset-state); mark_reset_state() restarts the
 * induction at the current gauge.  ACCUMULATORs (including histogram, via operator-=) subtract the reset-state
 * baseline; GAUGEs pass through untouched; the reset-state's own ACC baseline is the caller's business
 * (it is not auto-updated). */
TEST(Stats_stat_set_test, Since_reset_state)
{
  Atomic_stats rst{{}};

  { // Consume 1: baseline all-zero; gauge 150 observed => HWM 150 both out and into the induction.
    Atomic_stats tgt{{}};
    store(&tgt.m_acc, 50);
    store(&tgt.m_gauge, 150);
    store(&tgt.m_hwm, 999); // Garbage-in by contract: since_reset_state() computes it.
    tgt.m_histo.overwrite_count_for_bucket(0, 11);
    tgt.m_histo.overwrite_count_for_bucket(2, 22);
    tgt.m_histo.overwrite_count_for_bucket(4, 33);
    stats_since_reset_state(&tgt, &rst);
    EXPECT_EQ(load(tgt.m_acc), 50);
    EXPECT_EQ(load(tgt.m_gauge), 150);
    EXPECT_EQ(load(tgt.m_hwm), 150);
    EXPECT_EQ(load_counts(tgt.m_histo), (Counts{11, 0, 22, 0, 33, 0}));
    EXPECT_EQ(load(rst.m_hwm), 150);
    EXPECT_EQ(load(rst.m_acc), 0); // The baseline is caller-managed; untouched.
  }
  { // Consume 2: higher gauge => HWM rises (and sticks in the induction).
    Atomic_stats tgt{{}};
    store(&tgt.m_acc, 51);
    store(&tgt.m_gauge, 300);
    store(&tgt.m_hwm, 999);
    tgt.m_histo.overwrite_count_for_bucket(0, 13);
    stats_since_reset_state(&tgt, &rst);
    EXPECT_EQ(load(tgt.m_hwm), 300);
    EXPECT_EQ(load(rst.m_hwm), 300);
  }
  { // Consume 3: lower gauge => HWM holds at the induction's max.
    Atomic_stats tgt{{}};
    store(&tgt.m_acc, 55);
    store(&tgt.m_gauge, 125);
    store(&tgt.m_hwm, 999);
    stats_since_reset_state(&tgt, &rst);
    EXPECT_EQ(load(tgt.m_gauge), 125);
    EXPECT_EQ(load(tgt.m_hwm), 300);
    EXPECT_EQ(load(rst.m_hwm), 300);
  }

  // A reset: baseline captured (caller stores the raws) + mark_reset_state() restarts the HWM induction.
  store(&rst.m_acc, 56);
  store(&rst.m_gauge, 101);
  store(&rst.m_hwm, 999); // Garbage-in, as above.
  rst.m_histo.overwrite_count_for_bucket(0, 14);
  rst.m_histo.overwrite_count_for_bucket(2, 25);
  rst.m_histo.overwrite_count_for_bucket(4, 36);
  stats_mark_reset_state(&rst);
  EXPECT_EQ(load(rst.m_hwm), 101) << "mark: HWM := own gauge; nothing else touched.";
  EXPECT_EQ(load(rst.m_acc), 56);

  { // Consume 4: ACCs (including histogram) now relative to the baseline; HWM inducts from the mark.
    Atomic_stats tgt{{}};
    store(&tgt.m_acc, 59);
    store(&tgt.m_gauge, 100);
    store(&tgt.m_hwm, 999);
    tgt.m_histo.overwrite_count_for_bucket(0, 20);
    tgt.m_histo.overwrite_count_for_bucket(2, 25);
    tgt.m_histo.overwrite_count_for_bucket(4, 39);
    stats_since_reset_state(&tgt, &rst);
    EXPECT_EQ(load(tgt.m_acc), 3); // 59 - 56.
    EXPECT_EQ(load(tgt.m_gauge), 100);
    EXPECT_EQ(load(tgt.m_hwm), 101) << "Marked-at gauge still the max.";
    EXPECT_EQ(load_counts(tgt.m_histo), (Counts{6, 0, 0, 0, 3, 0})); // Per-bucket minus baseline.
    EXPECT_EQ(load(rst.m_hwm), 101);
  }
  { // Consume 5: gauge exceeds the marked max => HWM moves once more.
    Atomic_stats tgt{{}};
    store(&tgt.m_acc, 59);
    store(&tgt.m_gauge, 190);
    store(&tgt.m_hwm, 509090);
    tgt.m_histo.overwrite_count_for_bucket(0, 20);
    tgt.m_histo.overwrite_count_for_bucket(2, 26);
    tgt.m_histo.overwrite_count_for_bucket(4, 39);
    stats_since_reset_state(&tgt, &rst);
    EXPECT_EQ(load(tgt.m_acc), 3);
    EXPECT_EQ(load(tgt.m_hwm), 190);
    EXPECT_EQ(load_counts(tgt.m_histo), (Counts{6, 0, 1, 0, 3, 0}));
    EXPECT_EQ(load(rst.m_hwm), 190);
  }
} // TEST(Stats_stat_set_test, Since_reset_state)

/* Every stats_*() op instantiated + sanity-asserted against an all-non-atomic Stat_set: the
 * single-threaded-collection use case must compile (the ops' plain-overload code paths differ from the
 * atomic ones) and behave identically. */
TEST(Stats_stat_set_test, Plain_members)
{
  vector<Plain_stats> srcs(2); // Copyable: plain vector<> is fine here.
  auto& p1 = srcs[0];
  auto& p2 = srcs[1];
  p1.m_acc = 4;
  p1.m_gauge = 2;
  p1.m_hwm = 6;
  p1.m_histo.record_value(1);
  p2.m_acc = 6;
  p2.m_gauge = 4;
  p2.m_hwm = 3;
  p2.m_histo.record_value(4);

  { // print + field-names.
    ostringstream os;
    os << print(p1);
    EXPECT_NE(os.str().find("acc=[4]"), string::npos);
    EXPECT_EQ(stats_field_names<Plain_stats>(), (vector<string>{"acc", "gauge", "hwm", "histo"}));
  }
  { // assign; aggregate_one.
    Plain_stats t;
    stats_assign(&t, p1);
    EXPECT_EQ(t.m_acc, 4);
    stats_aggregate_one(&t, p2);
    EXPECT_EQ(t.m_acc, 10);
    EXPECT_EQ(t.m_gauge, 6);
    EXPECT_EQ(t.m_hwm, 6);
    EXPECT_EQ(load_counts(t.m_histo), (Counts{1, 0, 0, 1}));
  }
  { // aggregate (gauge-mean); sum (HWM-sum).
    Plain_stats t;
    stats_aggregate(&t, srcs.begin(), srcs.end());
    EXPECT_EQ(t.m_gauge, 3); // (2 + 4) / 2.
    EXPECT_EQ(t.m_hwm, 6);
    Plain_stats u;
    stats_sum(&u, srcs.begin(), srcs.end());
    EXPECT_EQ(u.m_gauge, 6);
    EXPECT_EQ(u.m_hwm, 9); // 6 + 3.
  }
  { // aggregate_shards + reset_shard_aggregate, non-degenerate and empty-range.
    Plain_stats t;
    stats_aggregate_shards<Plain_stats>(&t, srcs.begin(), srcs.end(), nullptr);
    EXPECT_EQ(t.m_acc, 10);
    EXPECT_EQ(t.m_gauge, 6);
    EXPECT_EQ(t.m_hwm, 6);
    EXPECT_EQ(load_counts(t.m_histo), (Counts{1, 0, 0, 1}));

    stats_reset_shard_aggregate(&t, srcs.begin(), srcs.end(), Plain_stats{});
    EXPECT_EQ(p1.m_acc, 0);
    EXPECT_EQ(p2.m_acc, 0);
    EXPECT_EQ(load_counts(p1.m_histo), (Counts{0, 0, 0, 0}));
    EXPECT_EQ(t.m_gauge, 6);
    EXPECT_EQ(t.m_hwm, 6);

    Plain_stats fresh;
    fresh.m_acc = 9;
    fresh.m_gauge = 5;
    Plain_stats t2;
    t2.m_hwm = 8;
    stats_aggregate_shards<Plain_stats>(&t2, srcs.end(), srcs.end(), &fresh);
    EXPECT_EQ(t2.m_acc, 9);
    EXPECT_EQ(t2.m_gauge, 5);
    EXPECT_EQ(t2.m_hwm, 8);
    stats_reset_shard_aggregate(&t2, srcs.end(), srcs.end(), fresh);
    EXPECT_EQ(t2.m_hwm, 5);
  }
  { // reset; since_reset_state + mark_reset_state.
    Plain_stats a;
    a.m_acc = 7;
    a.m_gauge = 3;
    a.m_hwm = 12;
    a.m_histo.record_value(2);
    stats_reset(&a, Plain_stats{});
    EXPECT_EQ(a.m_acc, 0);
    EXPECT_EQ(a.m_gauge, 3);
    EXPECT_EQ(a.m_hwm, 3);
    EXPECT_EQ(load_counts(a.m_histo), (Counts{0, 0, 0, 0}));

    Plain_stats rst;
    Plain_stats tgt;
    tgt.m_acc = 20;
    tgt.m_gauge = 7;
    tgt.m_histo.record_value(1);
    tgt.m_histo.record_value(1);
    stats_since_reset_state(&tgt, &rst);
    EXPECT_EQ(tgt.m_acc, 20);
    EXPECT_EQ(tgt.m_hwm, 7);
    EXPECT_EQ(load_counts(tgt.m_histo), (Counts{2, 0, 0, 0}));
    EXPECT_EQ(rst.m_hwm, 7);
    stats_mark_reset_state(&rst);
    EXPECT_EQ(rst.m_hwm, rst.m_gauge);
  }
} // TEST(Stats_stat_set_test, Plain_members)

/* Composition (see the namespace doc header's Composition section): no stats_*() op knows about it -- it
 * must simply fall out of declare_stats() forwarding.  Spot-checked here via Compo_stats, which combines a
 * dotted-path-declared nested sub-struct, two really-composed sub-`Stat_set`s (one empty-prefixed, one
 * "sub."-prefixed -- prefixes being what keeps the names distinct), a direct member -- and an undeclared
 * member, which every op must ignore (per the doc header: undeclared = merely invisible to the ops). */
TEST(Stats_stat_set_test, Composition)
{
  // Reflection sees the composed whole, prefixes applied, in declaration order.
  EXPECT_EQ(stats_field_names<Compo_stats>(),
            (vector<string>{"nested.n_events", "leaf_acc", "leaf_gauge",
                            "sub.leaf_acc", "sub.leaf_gauge", "direct_acc"}));

  Compo_stats a;
  a.m_nested.m_n_events = 5;
  a.m_plain_leaf.m_leaf_acc = 10;
  a.m_plain_leaf.m_leaf_gauge = 4;
  a.m_sub_leaf.m_leaf_acc = 20;
  a.m_sub_leaf.m_leaf_gauge = 6;
  a.m_direct_acc = 3;
  a.m_untracked = 42;

  { // Printing: the composed whole, exactly; and no trace of the undeclared member.
    ostringstream os;
    os << print(a);
    EXPECT_EQ(os.str(),
              "nested.n_events=[5] leaf_acc=[10] leaf_gauge=[4] sub.leaf_acc=[20] sub.leaf_gauge=[6] "
              "direct_acc=[3]");
  }

  { // One mutating op through the composition: aggregation recurses into the leaves like anything else.
    Compo_stats b;
    b.m_nested.m_n_events = 1;
    b.m_plain_leaf.m_leaf_acc = 2;
    b.m_plain_leaf.m_leaf_gauge = 8;
    b.m_sub_leaf.m_leaf_acc = 3;
    b.m_sub_leaf.m_leaf_gauge = 10;
    b.m_direct_acc = 4;
    b.m_untracked = 99;

    vector<Compo_stats> srcs{a, b};
    Compo_stats t;
    stats_aggregate(&t, srcs.begin(), srcs.end());
    EXPECT_EQ(t.m_nested.m_n_events, 6);
    EXPECT_EQ(t.m_plain_leaf.m_leaf_acc, 12);
    EXPECT_EQ(t.m_plain_leaf.m_leaf_gauge, 6); // (4 + 8) / 2.
    EXPECT_EQ(t.m_sub_leaf.m_leaf_acc, 23);
    EXPECT_EQ(t.m_sub_leaf.m_leaf_gauge, 8); // (6 + 10) / 2.
    EXPECT_EQ(t.m_direct_acc, 7);
    EXPECT_EQ(t.m_untracked, 7); // Undeclared: untouched -- still its initial value, 42s and 99s notwithstanding.
  }
} // TEST(Stats_stat_set_test, Composition)

// For the Global_stats tests: distinct tag types = distinct singletons.
struct Test_tag_one {};
struct Test_tag_two {};

/* Stat_set_list: the array-of-Stat_sets sugar -- accessor consistency, copy semantics, reset(). */
TEST(Stats_stat_set_list_test, Interface)
{
  Stat_set_list<Plain_stats, 3> list;
  EXPECT_EQ(list.S_N, 3);

  // All accessor forms address the same storage.
  EXPECT_EQ(&list.stats_default(), &list.stats<0>());
  EXPECT_EQ(&list.stats_default(), &list.stats_at(0));
  EXPECT_EQ(&list.stats_mutable_default(), &list.stats_mutable<0>());
  EXPECT_EQ(&list.stats_mutable<2>(), &list.stats_mutable_at(2));

  // Mutations land in the right slots, visible via the const forms.
  list.stats_mutable_default().m_acc = 10;
  list.stats_mutable<1>().m_acc = 20;
  list.stats_mutable_at(2).m_acc = 30;
  EXPECT_EQ(list.stats<0>().m_acc, 10);
  EXPECT_EQ(list.stats_at(1).m_acc, 20);
  EXPECT_EQ(list.stats<2>().m_acc, 30);

  // It really is an array: copyable iff Stat_set is; and the copy is independent.
  auto list2 = list;
  list.stats_mutable<1>().m_acc = 21;
  EXPECT_EQ(list2.stats<1>().m_acc, 20);

  // reset() = stats_reset(&s, {}) per slot: ACCs to fresh-zero, GAUGEs persist, HWMs re-seed to gauges.
  auto& s0 = list.stats_mutable_default();
  s0.m_gauge = 3;
  s0.m_hwm = 9;
  s0.m_histo.record_value(2);
  list.reset();
  EXPECT_EQ(list.stats<0>().m_acc, 0);
  EXPECT_EQ(list.stats<1>().m_acc, 0);
  EXPECT_EQ(list.stats<0>().m_gauge, 3);
  EXPECT_EQ(list.stats<0>().m_hwm, 3);
  EXPECT_EQ(load_counts(list.stats<0>().m_histo), (Counts{0, 0, 0, 0}));
} // TEST(Stats_stat_set_list_test, Interface)

// Global_stats: the on-demand singleton holder -- one Stat_set_list per (Tag, Stat_set, N) tuple.
TEST(Stats_stat_set_list_test, Global_singletons)
{
  auto& g1 = Global_stats<Test_tag_one, Plain_stats, 2>::get();
  EXPECT_EQ((&Global_stats<Test_tag_one, Plain_stats, 2>::get()), &g1) << "Same tuple => same object, always.";

  // Distinct tag -- or same tag but distinct N -- each yield their own singleton.
  EXPECT_NE(static_cast<const void*>(&Global_stats<Test_tag_two, Plain_stats, 2>::get()),
            static_cast<const void*>(&g1));
  EXPECT_NE(static_cast<const void*>(&Global_stats<Test_tag_one, Plain_stats, 1>::get()),
            static_cast<const void*>(&g1));

  // Mutations persist across get()s (it is the same object; also: independent from other-tag singletons).
  g1.stats_mutable_default().m_acc = 42;
  EXPECT_EQ((Global_stats<Test_tag_one, Plain_stats, 2>::get().stats_default().m_acc), 42);
  EXPECT_EQ((Global_stats<Test_tag_two, Plain_stats, 2>::get().stats_default().m_acc), 0);
} // TEST(Stats_stat_set_list_test, Global_singletons)

} // namespace flow::util::stat::test
