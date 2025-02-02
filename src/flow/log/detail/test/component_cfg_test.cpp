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
#include <gtest/gtest.h>
#include <vector>
#include <array>
#include <boost/shared_ptr.hpp>
#include <random>

namespace flow::log::test
{

namespace
{
using flow::test::Test_logger;
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

template<typename... Dict> // So you can give it a list of types, and the little lambda will be repeated for each.
void dict_map_death_test()
{
  Test_logger logger;
  FLOW_LOG_SET_CONTEXT(&logger, Flow_log_component::S_UNCAT);

  (([&]()
  {
    FLOW_LOG_INFO("Testing dict-type [" << typeid(Dict).name() << "].");
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

  using Ti = const std::type_info*;

  constexpr size_t N_SAMPLES = 100 * 1000;
  const auto results_unfinding = std::make_unique<std::array<cfg_t, N_SAMPLES>>();
  const auto results_finding = std::make_unique<std::array<cfg_t, N_SAMPLES>>();

  struct Type_rec { Ti m_type; cfg_t m_cfg; };
  std::vector<Type_rec> all_types =
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

  (([&]()
  {
    Timer::Aggregator timer_agg(nullptr, util::ostream_op_string("n_cfgs=", n_cfgs, '|', typeid(Dict).name()),
                                N_SAMPLES);

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

    timer_agg.log_aggregated_results(&logger, false, true, 0);
  })(), ...);
}

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
