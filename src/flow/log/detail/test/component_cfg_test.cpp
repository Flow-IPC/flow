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

#include "flow/log/detail/component_cfg.hpp"
#include "flow/log/log.hpp"
#include "flow/test/test_logger.hpp"
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

namespace n1::n1 { enum class Enum { S_A }; }
namespace n2::n2 { enum class Enum { S_A }; }
namespace n3::n3 { enum class Enum { S_A }; }
namespace n4::n4 { enum class Enum { S_A }; }
namespace n5::n5 { enum class Enum { S_A }; }
namespace n6::n6 { enum class Enum { S_A }; }
namespace n7::n7 { enum class Enum { S_A }; }
namespace n8::n8 { enum class Enum { S_A }; }
namespace n9::n9 { enum class Enum { S_A }; }
namespace n0::n0 { enum class Enum { S_A }; }
namespace nX::nX { enum class Enum { S_A }; }

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
    ret.remove_prefix(String_view("idx=...|impl=").size());
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
    dict.insert(typeid(n1::n1::Enum), 1);
    dict.insert(typeid(n0::n0::Enum), 0);
    EXPECT_DEATH(dict.insert(typeid(n0::n0::Enum), 0), "duplicate insertion is disallowed");
    EXPECT_DEATH(dict.insert(typeid(n0::n0::Enum), -1), "duplicate insertion is disallowed");
    dict.insert(typeid(n2::n2::Enum), 2);
    cfg_t cfg;
    EXPECT_TRUE(dict.lookup(typeid(n0::n0::Enum), &cfg)); // Sanity checks.
    EXPECT_TRUE(dict.lookup(typeid(n1::n1::Enum), &cfg));
    EXPECT_TRUE(dict.lookup(typeid(n2::n2::Enum), &cfg));
    EXPECT_FALSE(dict.lookup(typeid(n3::n3::Enum), &cfg));
  })(), ...);
}

template<typename... Dict>
void dict_benchmark(size_t n_cfgs)
{
  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Flow_log_component::S_UNCAT);
  log::beautify_chrono_logger_this_thread(&logger); // Nice short-form duration printouts.

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
    Type_rec{ &typeid(n1::n1::Enum), 1 }, Type_rec{ &typeid(n2::n2::Enum), 2 },
    Type_rec{ &typeid(n3::n3::Enum), 3 }, Type_rec{ &typeid(n4::n4::Enum), 4 },
    Type_rec{ &typeid(n5::n5::Enum), 5 }, Type_rec{ &typeid(n6::n6::Enum), 6 },
    Type_rec{ &typeid(n7::n7::Enum), 7 }, Type_rec{ &typeid(n8::n8::Enum), 8 },
    Type_rec{ &typeid(n9::n9::Enum), 9 }, Type_rec{ &typeid(n0::n0::Enum), 0 }
  };
  EXPECT_TRUE(n_cfgs <= all_types.size());
  all_types.resize(n_cfgs);

  std::random_device rd; // Seed source.
  std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dist_to_n_cfgs(0, n_cfgs - 1);

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

      const Ti unfindable_type = &typeid(nX::nX::Enum);
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

      const auto timer = boost::make_shared<Timer>(nullptr, "benchiez", Timer::real_clock_types(), 2);
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
    constexpr auto CLK_TYPE = size_t(perf::Clock_type::S_REAL_HI_RES);
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
        EXPECT_LT(rec.m_time_multiple, SIGNIFICANT_MULTIPLE_THRESHOLD)
          << "Dict type [by-ptr (fast)] impl [" << dict_type_printable(rec.m_type, true) << "] "
             "benchmarked as significantly (beyond allowance) slower than the fastest impl, but by-ptr (fast) impls "
             "should all be similarly quick.  Look into it.";
      }
      else
      {
        if (rec.m_type == Type_idx(typeid(FLOW_LOG_CONFIG_COMPONENT_PAYLOAD_TYPE_DICT_BY_VAL<cfg_t>)))
        {
          EXPECT_LT(rec.m_time_multiple, SIGNIFICANT_MULTIPLE_THRESHOLD)
            << "Dict type [by-val (slow)] impl [" << dict_type_printable(rec.m_type, true) << "] is the default "
               "but benchmark finds it is not the fastest (of by-val/slow impls), AND it is over the safety "
               "allowance.  Perhaps reconsider the default?";

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

    vector<string> rel_time_str_vec(timing_vec.size());
    std::transform(timing_vec.begin(), timing_vec.end(), rel_time_str_vec.begin(),
                   [](const auto& rec) -> auto
                     { return util::ostream_op_string('[', dict_type_printable(rec.m_type, true),
                                                      " => x",
                                                      (rec.m_time_multiple == 1.f) ? std::defaultfloat : std::fixed,
                                                      std::setprecision((rec.m_time_multiple == 1.f) ? 1 : 2),
                                                      rec.m_time_multiple, ']'); });

    vector<string> time_str_vec(timing_vec.size());
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

  EXPECT_LT(by_ptr_time_sum, by_val_time_sum)
    << "While individual by-val (slow) dict lookups might occasionally benchmark as faster than "
       "by-ptr (fast) dict lookups (for lower n_cfgs and just spuriously every now and then), the sum thereof "
       "should really show them to be overall slow.  What happened?";


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

#ifdef NDEBUG // These "deaths" occur only if assert()s enabled; else these are guaranteed failures.
TEST(Component_cfg_DeathTest, DISABLED_Interface)
#else
TEST(Component_cfg_DeathTest, Interface)
#endif
{
  dict_map_death_test<Dict_ptr_tree_map, Dict_ptr_s_hash_map, Dict_ptr_b_hash_map,
                      Dict_val_tree_map, Dict_val_s_hash_map, Dict_val_b_hash_map>();
}

TEST(Component_cfg_test, Interface)
{
  constexpr std::array<size_t, 3> N_CFGS_ARRAY = {2, 5, 10};
  for(const auto n_cfgs : N_CFGS_ARRAY)
  {
    dict_benchmark<Dict_ptr_tree_map, Dict_ptr_s_hash_map, Dict_ptr_b_hash_map, Dict_ptr_array, Dict_ptr_sorted_array,
                   Dict_val_tree_map, Dict_val_s_hash_map, Dict_val_b_hash_map, Dict_val_array, Dict_val_sorted_array>
      (n_cfgs);
  }
}

} // namespace flow::log::test
