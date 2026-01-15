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

#include "flow/log/log.hpp"
#include "flow/log/simple_ostream_logger.hpp"
#include "flow/log/detail/component_cfg.hpp" // Yes, detail/ -- we do what we must.
#include "flow/test/test_logger.hpp"
#include "flow/test/test_config.hpp"
#include "flow/perf/checkpt_timer.hpp"
#include "flow/util/string_view.hpp"
#include <gtest/gtest.h>
#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string.hpp>
#include <vector>
#include <array>
#include <map>
#include <random>
#include <type_traits>

namespace flow::log::test
{

namespace
{
using flow::test::Test_logger;
using util::String_view;
using Timer = perf::Checkpointing_timer;

using cfg_t = int;
using Dict_ptr_tree_map = Component_payload_type_dict_by_ptr_via_tree_map<cfg_t>;
using Dict_ptr_s_hash_map = Component_payload_type_dict_by_ptr_via_s_hash_map<cfg_t>;
using Dict_ptr_b_hash_map = Component_payload_type_dict_by_ptr_via_b_hash_map<cfg_t>;
using Dict_ptr_array = Component_payload_type_dict_by_ptr_via_array<cfg_t>;
using Dict_ptr_sorted_array = Component_payload_type_dict_by_ptr_via_sorted_array<cfg_t>;
using Dict_val_tree_map = Component_payload_type_dict_by_val_via_tree_map<cfg_t>;
using Dict_val_s_hash_map = Component_payload_type_dict_by_val_via_s_hash_map<cfg_t>;
using Dict_val_b_hash_map = Component_payload_type_dict_by_val_via_b_hash_map<cfg_t>;
using Dict_val_array = Component_payload_type_dict_by_val_via_array<cfg_t>;
using Dict_val_sorted_array = Component_payload_type_dict_by_val_via_sorted_array<cfg_t>;

namespace n1::n1 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n2::n2 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n3::n3 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n4::n4 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n5::n5 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n6::n6 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n7::n7 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n8::n8 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n9::n9 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace n0::n0 { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
namespace nX::nX { enum class Cmps : Component::enum_raw_t { S_COMP_A, S_COMP_B }; }
constexpr auto S_CMPS_SZ = static_cast<Component::enum_raw_t>(nX::nX::Cmps::S_COMP_B) + 1;

String_view dict_type_printable(const std::type_index& type, bool brief = false)
{
  /* A dict_type_printable<Dict>() is perfectly doable via `if constexpr()`; but we need runtime result in some
   * cases below, and perf is irrelevant in our context; so this runtime guy is fine. */

  const auto impl = [&]() -> String_view
  {
    using Type_idx = std::type_index;
    // @todo A map<> would be a bit nicer.  It's just test code though.
    if (type == Type_idx(typeid(Dict_ptr_tree_map)))     { return "idx=ptr|impl=sorted-map"; }
    if (type == Type_idx(typeid(Dict_ptr_s_hash_map)))   { return "idx=ptr|impl=hash-map-std"; }
    if (type == Type_idx(typeid(Dict_ptr_b_hash_map)))   { return "idx=ptr|impl=hash-map-boost"; }
    if (type == Type_idx(typeid(Dict_ptr_array)))        { return "idx=ptr|impl=linear-array"; }
    if (type == Type_idx(typeid(Dict_ptr_sorted_array))) { return "idx=ptr|impl=bin-search-array"; }
    if (type == Type_idx(typeid(Dict_val_tree_map)))     { return "idx=val|impl=sorted-map"; }
    if (type == Type_idx(typeid(Dict_val_s_hash_map)))   { return "idx=val|impl=hash-map-std"; }
    if (type == Type_idx(typeid(Dict_val_b_hash_map)))   { return "idx=val|impl=hash-map-boost"; }
    if (type == Type_idx(typeid(Dict_val_array)))        { return "idx=val|impl=linear-array"; }
    if (type == Type_idx(typeid(Dict_val_sorted_array))) { return "idx=val|impl=bin-search-array"; }
    assert(false && "Missed a spot above?"); // ASSERT_*() does not work in non-void.
    return "what the";
  };

  auto ret = impl();
  if (brief)
  {
    ret.remove_prefix(String_view{"idx=...|impl="}.size());
  }
  return ret;
}

String_view dict_type_printable(const std::type_info& type, bool brief = false)
{
  return dict_type_printable(std::type_index(type), brief);
}

template<typename... Dict> // So you can give it a list of types, and the little lambda will be repeated for each.
void dict_map_death_test()
{
  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Flow_log_component::S_UNCAT);

  (([&]()
  {
    FLOW_LOG_INFO("Testing dict-type [" << dict_type_printable(typeid(Dict)) << "].");
    Dict dict;
    dict.insert(typeid(n1::n1::Cmps), 1);
    dict.insert(typeid(n0::n0::Cmps), 0);
    EXPECT_DEATH(dict.insert(typeid(n0::n0::Cmps), 0), "duplicate insertion is disallowed");
    EXPECT_DEATH(dict.insert(typeid(n0::n0::Cmps), -1), "duplicate insertion is disallowed");
    dict.insert(typeid(n2::n2::Cmps), 2);
    cfg_t cfg;
    EXPECT_TRUE(dict.lookup(typeid(n0::n0::Cmps), &cfg)); // Sanity checks.
    EXPECT_TRUE(dict.lookup(typeid(n1::n1::Cmps), &cfg));
    EXPECT_TRUE(dict.lookup(typeid(n2::n2::Cmps), &cfg));
    EXPECT_FALSE(dict.lookup(typeid(n3::n3::Cmps), &cfg));
  })(), ...);
}

template<typename... Dict>
void dict_test()
{
  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Flow_log_component::S_UNCAT);

  (([&]()
  {
    FLOW_LOG_INFO("Testing dict-type [" << dict_type_printable(typeid(Dict)) << "].");
    {
      Dict dict;
      dict.insert(typeid(n1::n1::Cmps), 1);
      dict.insert(typeid(n0::n0::Cmps), 0);
      dict.insert(typeid(n2::n2::Cmps), 2);
      dict.insert(typeid(nX::nX::Cmps), 100);
      cfg_t cfg{-1};
      EXPECT_FALSE(dict.lookup(typeid(n3::n3::Cmps), &cfg));
      EXPECT_EQ(cfg, -1);
      EXPECT_TRUE(dict.lookup(typeid(n0::n0::Cmps), &cfg));
      EXPECT_EQ(cfg, 0);
      EXPECT_TRUE(dict.lookup(typeid(n1::n1::Cmps), &cfg));
      EXPECT_EQ(cfg, 1);
      EXPECT_TRUE(dict.lookup(typeid(n2::n2::Cmps), &cfg));
      EXPECT_EQ(cfg, 2);
      EXPECT_FALSE(dict.lookup(typeid(n3::n3::Cmps), &cfg));
      EXPECT_EQ(cfg, 2);
      EXPECT_TRUE(dict.lookup(typeid(n0::n0::Cmps), &cfg));
      EXPECT_EQ(cfg, 0);
      EXPECT_TRUE(dict.lookup(typeid(nX::nX::Cmps), &cfg));
      EXPECT_EQ(cfg, 100);
      /* @todo It is tough to test _by_val_ dudes where &typeid(X) yields different addrs at different times;
       * probably needs to be done across a shared-object boundary at least.  Could contrive something though.
       * For now there is no unit test for it, so code review was doubly important. */

      // Weird stuff.
      cfg = -1;
      EXPECT_FALSE(dict.lookup(typeid(int), &cfg));
      EXPECT_FALSE(dict.lookup(typeid(dict), &cfg));
      EXPECT_EQ(cfg, -1);
    }
    {
      Dict dict; // Deal with degenerate case of an empty dict.
      cfg_t cfg{-1};
      EXPECT_FALSE(dict.lookup(typeid(n0::n0::Cmps), &cfg));
      EXPECT_EQ(cfg, -1);
    }
  })(), ...);
} // dict_test()

template<typename... Dict>
void dict_benchmark(size_t n_cfgs)
{
  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Flow_log_component::S_UNCAT);
  log::beautify_chrono_logger_this_thread(&logger); // Nice short-form duration printouts.

  const bool do_not_fail_benchmarks = flow::test::Test_config::get_singleton().m_do_not_fail_benchmarks;

  using Ti = const std::type_info*;
  using Type_idx = std::type_index;
  using std::string;
  using std::vector;

  constexpr size_t N_SAMPLES = 100 * 1000;
  const auto results_unfinding = std::make_unique<std::array<cfg_t, N_SAMPLES>>();
  const auto results_finding = std::make_unique<std::array<cfg_t, N_SAMPLES>>();

  constexpr auto SIGNIFICANT_MULTIPLE_THRESHOLD = 1.2f;

  struct Type_rec { Ti m_type; cfg_t m_cfg; };
  vector<Type_rec> all_types =
  {
    Type_rec{ &typeid(n1::n1::Cmps), 1 }, Type_rec{ &typeid(n2::n2::Cmps), 2 },
    Type_rec{ &typeid(n3::n3::Cmps), 3 }, Type_rec{ &typeid(n4::n4::Cmps), 4 },
    Type_rec{ &typeid(n5::n5::Cmps), 5 }, Type_rec{ &typeid(n6::n6::Cmps), 6 },
    Type_rec{ &typeid(n7::n7::Cmps), 7 }, Type_rec{ &typeid(n8::n8::Cmps), 8 },
    Type_rec{ &typeid(n9::n9::Cmps), 9 }, Type_rec{ &typeid(n0::n0::Cmps), 0 }
  };
  EXPECT_TRUE(n_cfgs <= all_types.size());
  all_types.resize(n_cfgs);

  std::random_device rd; // Seed source.
  std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dist_to_n_cfgs(0, n_cfgs - 1);
  perf::Clock_types_subset clocks;
  /* Important (in our benchmarking context) discussion -- what clock to measure?  Normally I (ygoldfel) by far
   * prefer REAL_HI_RES.  It is extremely accurate and itself very low-cost.  When it's a local environment without
   * a real possibility of irrelevant stuff happening simultaneously, it's great.  Here, though, we've got a brittle
   * situation: the thing being timed in is in the nanoseconds, so the slightest disruption throws various ratios
   * below out of whack and triggers failures and confusion.  So in this case it seems best to count processor cycles;
   * and just in case multithreading (though there shouldn't be any in our context) becomes an issue, let's count
   * this thread's cycles.
   * Update: On 2nd thought, failing tests due to this benchmark appears too draconian, at least in automated
   * setups like GitHub Actions; so we allow for --do-not-fail-benchmarks flag to override that.  Hence
   * let's use the (IMO) nicer experience of REAL_HI_RES, since we will basically run without --do-not-fail-benchmarks
   * only locally, where we can control our environment and not worry about spurious craziness that much. */
  constexpr auto CLK_TYPE = size_t(perf::Clock_type::S_REAL_HI_RES);
  clocks.set(CLK_TYPE);

  /* Maps Dict_* type to total (findable lookup + unfindable lookup) benchmark result for that dictionary impl.
   * Note: Empirically we see (as of this writing in the ~3 types of hardware tried) that the failed-lookup and
   * successful-lookup-for-randomly-chosen-findable-key ops take similar amount of time (probably because
   * n_cfgs is so small (all_types.size() at most)).  Using a sum therefore seemed a decent approach in terms of
   * being a single number, for a given n_cfgs, assessing the perf of a Dict_* choice. */
  using Timing_map = std::map<Type_idx, Fine_duration>;
  Timing_map by_ptr_timing, by_ptr_timing1; // The Dict_ptr_* types.
  Timing_map by_val_timing, by_val_timing1; // The Dict_val_* types.
  /* We use the sum of items in by_{ptr|val}_timing to roughly ensure that, indeed, the Dict_ptr_* guys are
   * faster than the Dict_val_* guys.  We know it's better -- lookup by ptr comparison/etc. will be better than
   * by string comparison/hashing/etc. -- but this should sanity-check it decently.  These ops are very fast,
   * and especially for very low n_cfgs there can be some occasional counterintuitive results; but summing it
   * should deal with such occasional caveats and avoid false failures while sanity-checking decently. */
  Fine_duration by_ptr_time_sum = Fine_duration::zero();
  Fine_duration by_val_time_sum = Fine_duration::zero();

  (([&]()
  {
    const auto run_str = util::ostream_op_string("n_cfgs=", n_cfgs, '|', dict_type_printable(typeid(Dict)));
    Timer::Aggregator timer_agg(nullptr, string(run_str), N_SAMPLES);

    for (size_t idx = 0; idx != N_SAMPLES; ++idx)
    {
      std::shuffle(all_types.begin(), all_types.end(), gen);

      const Ti unfindable_type = &typeid(nX::nX::Cmps);
      const auto& findable_type_rec = all_types[dist_to_n_cfgs(gen)];
      const Ti findable_type = findable_type_rec.m_type;
      const auto findable_cfg = findable_type_rec.m_cfg;
      cfg_t cfg_found1{-1};
      cfg_t cfg_found2{-1};

      Dict dict;
      for (const auto& type : all_types)
      {
        dict.insert(*type.m_type, type.m_cfg);
      }

      const auto timer = boost::make_shared<Timer>(nullptr, "benchiez", clocks, 2);
      dict.lookup(*unfindable_type, &cfg_found1);
      timer->checkpoint("unfindable");
      dict.lookup(*findable_type, &cfg_found2);
      timer->checkpoint("findable");

      timer_agg.aggregate(timer);

      (*results_unfinding)[idx] = cfg_found1;
      (*results_finding)[idx] = cfg_found2;
      EXPECT_EQ(cfg_found1, -1);
      EXPECT_EQ(cfg_found2, findable_cfg);
    }

    // This is cool but too wordy due to being general/etc.:
#if 0
    timer_agg.log_aggregated_results(&logger, false, true, 0);
#endif
    // We can print out a pithy thing ourselves.  (Also see above re. why we choose to use the sum.)
    const auto total_timer = timer_agg.create_aggregated_result(nullptr, false, 1);
    const Fine_duration time = total_timer->since_start().m_values[CLK_TYPE];
    const Fine_duration time1 = total_timer->checkpoints()[0].m_since_last.m_values[CLK_TYPE];
#if 0 // We'll summarize even more pithily below by showing: (time) = (time1) + (time - time1).
    FLOW_LOG_INFO("Lookup time for [" << run_str << "]: [" << time << "] = "
                   "unfindable [" << time1 << "] + "
                   "findable [" << total_timer->checkpoints()[1].m_since_last.m_values[CLK_TYPE] << "].");
#endif

    const Type_idx idx{typeid(Dict)};
    if constexpr(Dict::S_BY_PTR_ELSE_VAL)
    {
      by_ptr_timing[idx] = time;
      by_ptr_timing1[idx] = time1;
      by_ptr_time_sum += time;
    }
    else
    {
      by_val_timing[idx] = time;
      by_val_timing1[idx] = time1;
      by_val_time_sum += time;
    }
  })(), ...);

  struct Dict_impl_rec { Type_idx m_type; Fine_duration m_time, m_time1; float m_time_multiple = 0; };

  const auto analyze_dict_type = [&](bool by_ptr_else_val, const Timing_map& timing_map, const Timing_map& timing_map1,
                                     Fine_duration time_sum) -> auto
  {
    FLOW_LOG_INFO("---- Results for dict type [by-" << (by_ptr_else_val ? "ptr (fast)" : "val (slow)") << "] ---- ");
    FLOW_LOG_INFO("Total time (all techniques): [" << time_sum << "].");

    vector<Dict_impl_rec> timing_vec;
    for (const auto& time_entry : timing_map)
    {
      timing_vec.emplace_back(Dict_impl_rec{ time_entry.first, time_entry.second,
                                             timing_map1.find(time_entry.first)->second });
    }
    std::sort(timing_vec.begin(), timing_vec.end(),
              [](const auto& rec1, const auto& rec2) -> bool { return rec1.m_time < rec2.m_time; });

    for (auto& rec : timing_vec)
    {
      rec.m_time_multiple = float(double(rec.m_time.count()) / double(timing_vec.front().m_time.count()));

      if (by_ptr_else_val)
      {
        if (rec.m_type == Type_idx(typeid(FLOW_LOG_CONFIG_COMPONENT_PAYLOAD_TYPE_DICT_BY_PTR<cfg_t>)))
        {
#define MSG "Dict type [by-ptr (fast)] impl [" << dict_type_printable(rec.m_type, true) << "] is the default " \
            "but benchmark finds it is not the fastest (of by-ptr/fast impls), AND it is over the safety " \
            "allowance.  Perhaps reconsider the default?"
          if (do_not_fail_benchmarks)
          {
            if (rec.m_time_multiple >= SIGNIFICANT_MULTIPLE_THRESHOLD) { FLOW_LOG_WARNING(MSG); }
          }
          else
          {
            EXPECT_LT(rec.m_time_multiple, SIGNIFICANT_MULTIPLE_THRESHOLD) << MSG;
          }
#undef MSG
        }
      }
      else
      {
        if (rec.m_type == Type_idx(typeid(FLOW_LOG_CONFIG_COMPONENT_PAYLOAD_TYPE_DICT_BY_VAL<cfg_t>)))
        {
#define MSG "Dict type [by-val (slow)] impl [" << dict_type_printable(rec.m_type, true) << "] is the default " \
            "but benchmark finds it is not the fastest (of by-val/slow impls), AND it is over the safety " \
            "allowance.  Perhaps reconsider the default?"

          if (do_not_fail_benchmarks)
          {
            if (rec.m_time_multiple >= SIGNIFICANT_MULTIPLE_THRESHOLD) { FLOW_LOG_WARNING(MSG); }
          }
          else
          {
            EXPECT_LT(rec.m_time_multiple, SIGNIFICANT_MULTIPLE_THRESHOLD) << MSG;
          }
#undef MSG

          /* This isn't a benchmark check but a sanity-check of a basic assumption (slow hashing + hash map = let us
           * not default to it, even among the slow-lookup map types). */
          EXPECT_NE(rec.m_type, Type_idx(typeid(Dict_val_s_hash_map)))
            << "The default [by-val (slow)] impl is an (std::) hash-map?  That contradicts the whole "
               "impetus of the feature, wherein hashing of typeid() strings is thought to be slow!";
          EXPECT_NE(rec.m_type, Type_idx(typeid(Dict_val_b_hash_map)))
            << "The default [by-val (slow)] impl is a (boost::) hash-map?  That contradicts the whole "
               "impetus of the feature, wherein hashing of typeid() strings is thought to be slow!";
        }
      } // if (!by_ptr_else_val)
    } // for (rec : timing_vec)

    vector<string> rel_time_str_vec{timing_vec.size()};
    std::transform(timing_vec.begin(), timing_vec.end(), rel_time_str_vec.begin(),
                   [](const auto& rec) -> auto
                     { return util::ostream_op_string('[', dict_type_printable(rec.m_type, true),
                                                      " => x",
                                                      (rec.m_time_multiple == 1.f) ? std::defaultfloat : std::fixed,
                                                      std::setprecision((rec.m_time_multiple == 1.f) ? 1 : 2),
                                                      rec.m_time_multiple, ']'); });

    vector<string> time_str_vec{timing_vec.size()};
    std::transform(timing_vec.begin(), timing_vec.end(), time_str_vec.begin(),
                   [](const auto& rec) -> auto
    {
      using boost::chrono::nanoseconds;

      util::String_ostream os;
      const nanoseconds time = rec.m_time, time1 = rec.m_time1, time2 = rec.m_time - rec.m_time1;

      os.os() << '[' << dict_type_printable(rec.m_type, true) << ": "
              << time.count() << '=' << time1.count() << '+' << time2.count() << "ns]";
      os.os() << std::flush;
      return os.str();
    });

    FLOW_LOG_INFO("Relative: " << boost::algorithm::join(rel_time_str_vec, " < ") << '.');
    FLOW_LOG_INFO("Deets (total lookup time = unfindable + findable): "
                  << boost::algorithm::join(time_str_vec, " < ") << '.');

    return timing_vec;
  }; // analyze_dict_type =

  FLOW_LOG_INFO("-- Results for [n_cfgs=" << n_cfgs << "] --");
  const auto fast_timing_vec = analyze_dict_type(true, by_ptr_timing, by_ptr_timing1, by_ptr_time_sum);
  const auto slow_timing_vec = analyze_dict_type(false, by_val_timing, by_val_timing1, by_val_time_sum);
  FLOW_LOG_INFO("-- END OF: Results for [n_cfgs=" << n_cfgs << "] --");

#define MSG "While individual by-val (slow) dict lookups might occasionally benchmark as faster than " \
            "by-ptr (fast) dict lookups (for lower n_cfgs and just spuriously every now and then), the sum thereof " \
            "should really show them to be overall slow.  What happened?"
  if (do_not_fail_benchmarks)
  {
    if (by_ptr_time_sum >= by_val_time_sum) { FLOW_LOG_WARNING(MSG); }
  }
  else
  {
    EXPECT_LT(by_ptr_time_sum, by_val_time_sum) << MSG;
  }
#undef MSG

  /* My (ygoldfel) heart was in the right place with the following check; but while in some real environments
   * this straightforwardly passes, on others instead results are muddled together and close.  So we'll let it go;
   * while keeping the most important checks, which up-above, which are simply that (to reiterate):
   *   - slow-map is slower than fast-map overall; and
   *   - the default (in actualy log::Config) impl for each type of lookup is either the fastest or close-enough
   *     to where it's "fine." */
#if 0
  // The 2 slowest slow-map look impls should be the hash-maps; and they should be a lot slower than the best impl.
  const auto check_slow_guy = [&](bool worst_else_2nd_worst)
  {
    auto it = --slow_timing_vec.end();
    if (!worst_else_2nd_worst) { --it; }

    const auto type = it->m_type;
    const bool its_a_hash_map = (type == Type_idx(typeid(Dict_val_b_hash_map)))
                                || (type == Type_idx(typeid(Dict_val_s_hash_map)));
    EXPECT_TRUE(its_a_hash_map)
      << "The [" << (worst_else_2nd_worst ? "worst" : "2nd-worst") << "] by-val (slow) lookup is expected to be "
         "hash-map-based, comfortably; but it is somehow [" << dict_type_printable(type, true) << "] instead.  "
         "Hmmm....";
    if (its_a_hash_map)
    {
      const auto best_time = std::min(slow_timing_vec.front().m_time, fast_timing_vec.front().m_time);
      const auto multiple = float(double(it->m_time.count()) / double(best_time.count()));
      EXPECT_GT(multiple, SIGNIFICANT_MULTIPLE_THRESHOLD)
        << "The [" << (worst_else_2nd_worst ? "worst" : "2nd-worst") << "] by-val (slow) lookup (which is indeed "
           "hash-map-based) should be comfortably slower than the overall-best time; but it is closer than "
           "the allowance in this test.";
    }
  };
  check_slow_guy(true);
  check_slow_guy(false);
#endif
} // dict_benchmark()

} // Anonymous namespace

// detail/ Component_payload_type_dict_*: correctness testing.
TEST(Component_cfg_test, Dict_internals_interface)
{
  dict_test<Dict_ptr_tree_map, Dict_ptr_s_hash_map, Dict_ptr_b_hash_map, Dict_ptr_array, Dict_ptr_sorted_array,
            Dict_val_tree_map, Dict_val_s_hash_map, Dict_val_b_hash_map, Dict_val_array, Dict_val_sorted_array>();

  /* @todo What about Component_payload_type_dict, the one that combines a Dict_ptr_* and a Dict_val_* and looks up
   * first in the former and if needed the other one?  Could test that, straightforward though it is.
   * It's probably not a huge deal, as we test that end-to-end in the `Interface` test below, from the log::Config
   * layer which uses the compoung Component_payload_type_dict. */
}

#ifdef NDEBUG // These "deaths" occur only if assert()s enabled; else these are guaranteed failures.
TEST(Component_cfg_DeathTest, DISABLED_Dict_internals)
#else
TEST(Component_cfg_DeathTest, Dict_internals)
#endif
{
  // Only the map guys have dupe-insert checking.
  dict_map_death_test<Dict_ptr_tree_map, Dict_ptr_s_hash_map, Dict_ptr_b_hash_map,
                      Dict_val_tree_map, Dict_val_s_hash_map, Dict_val_b_hash_map>();
}

// detail/ Component_payload_type_dict_*: benchmark them and gently verify they are as expected.
TEST(Component_cfg_test, Dict_internals_benchmark)
{
  constexpr std::array<size_t, 3> N_CFGS_ARRAY = {2, 5, 10};
  for(const auto n_cfgs : N_CFGS_ARRAY)
  {
    dict_benchmark<Dict_ptr_tree_map, Dict_ptr_s_hash_map, Dict_ptr_b_hash_map, Dict_ptr_array, Dict_ptr_sorted_array,
                   Dict_val_tree_map, Dict_val_s_hash_map, Dict_val_b_hash_map, Dict_val_array, Dict_val_sorted_array>
      (n_cfgs);
  }
}

/* While Dict_internals_interface tests the nitty-gritty of the map lookup at the heart of it -- which due to
 * perf matters has much subtlety to it -- this tests it end-to-end through the publicly available log::Config API. */
TEST(Component_cfg_test, Interface)
{
  Config cfg{Sev::S_INFO};

  Component comp0a{n0::n0::Cmps::S_COMP_A};
  Component comp0b{n0::n0::Cmps::S_COMP_B};
  Component comp1a{n1::n1::Cmps::S_COMP_A};
  Component comp1b{n1::n1::Cmps::S_COMP_B};
  Component comp2a{n2::n2::Cmps::S_COMP_A};
  Component comp2b{n2::n2::Cmps::S_COMP_B};
  Component comp3a{n3::n3::Cmps::S_COMP_A};
  Component comp3b{n3::n3::Cmps::S_COMP_B};
  Component compXa{nX::nX::Cmps::S_COMP_A};
  Component compXb{nX::nX::Cmps::S_COMP_B};

  cfg.init_component_to_union_idx_mapping<n0::n0::Cmps>(10,
                                                        S_CMPS_SZ, // @todo Test subtleties of this arg.
                                                        true);
  cfg.init_component_to_union_idx_mapping<n1::n1::Cmps>(10 + S_CMPS_SZ,
                                                        S_CMPS_SZ);
                                                        // 3rd arg default = false.
  cfg.init_component_to_union_idx_mapping<n2::n2::Cmps>(10 + (2 * S_CMPS_SZ),
                                                        S_CMPS_SZ,
                                                        true);
  cfg.init_component_to_union_idx_mapping<n3::n3::Cmps>(10 + (3 * S_CMPS_SZ),
                                                        S_CMPS_SZ,
                                                        false);
  // Logging (and/or per-component verbosity configuring) would begin here and below.

  /* These enums are registered, so internally the base index for each enum mentioned will be found;
   * but there will be no actual per-component verbosity configured for it; hence defaults to the overall
   * verbosity (INFO, seen above in ctor args). */
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp0a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp0b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp1a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp1b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp3a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp3b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp0a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp0b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp1a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp1b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp3a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp3b));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp0a));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp0b));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp1a));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp1b));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp3a));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp3b));
  // These enums are not registered so no base index found (but same result for now: no per-component verbosity).
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, compXa));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, compXb));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, compXa));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, compXb));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, compXa));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, compXb));

  cfg.configure_component_verbosity(Sev::S_TRACE, comp0a);
  cfg.configure_component_verbosity(Sev::S_TRACE, comp3b);
  /* Some (not all certainly) per-component verbosities have indeed been set.
   * So the base index lookup (internally via the Component_payload_type_dict_*::lookup()) will work for
   * all but the nX::nx:: component checks; but then only the actually-configured per-component verbosities
   * will be found in the big flat table. */

  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp0a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp0b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp1a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp1b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp2a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp2b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp3a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, comp3b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp0a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp0b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp1a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp1b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp2a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp2b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp3a));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, comp3b));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_TRACE, comp0a)); // Oh goody!  This should log now!
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp0b)); // Not this, though, still.
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp1a));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp1b));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp2a));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp2b));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, comp3a)); // Not this, though, still.
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_TRACE, comp3b)); // Oh yay!
  // These enums are not registered... so same as before.
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, compXa));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_INFO, compXb));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, compXa));
  EXPECT_TRUE(cfg.output_whether_should_log(Sev::S_WARNING, compXb));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, compXa));
  EXPECT_FALSE(cfg.output_whether_should_log(Sev::S_TRACE, compXb));

  /* @todo Test component names (config via component-name Config::configure_component_verbosity_by_name();
   * output of component-names Config::output_component_to_ostream(); the relevant Config::init_*()).
   * (Originally unit-test was created in the first place due to the Component_payload_type_dict_... work;
   * but as of this writing the other aspects of per-component log::Config remain un-unit-tested, though they
   * have long been verified via heavy use + functional tests.  Obv the point of unit tests = do better than that.) */

  /* @todo Test various `Logger`s and/or Ostream_log_msg_writer in the sense that they leverage component-based
   * log::Config aspects.  I.e., test from a still-higher layer.  Possibly that would go in the unit tests
   * for those classes though.  As of this writing they don't exist. */
} // TEST(Component_cfg_test, Interface)

} // namespace flow::log::test
