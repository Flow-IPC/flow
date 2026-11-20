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

// Deliberately first, and before any other flow::cfg header: this header must compile when included by itself.
#include "flow/cfg/dynamic_cfg_context.hpp"
#include "flow/cfg/cfg_manager.hpp"
#include "flow/cfg/static_cfg_manager.hpp"
#include "flow/cfg/test/test_cfg_util.hpp"
#include "flow/test/test_logger.hpp"
#include "flow/util/shared_ptr_alias_holder.hpp"
#include "flow/util/util.hpp"
#include "flow/log/log.hpp"
#include "flow/common.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <type_traits>

/* Tests of flow::cfg::Config_manager; of Static_config_manager (a simplified facade of it); and of
 * Dynamic_cfg_context (a convenience for accessing dynamic config).  Option_set, on which these are built, has its own
 * tests (option_set_test.cpp); so here the individual-option machinery is exercised only as far as needed to check
 * what Config_manager adds: static versus dynamic value sets; the apply_*() methods and what each tolerates;
 * final-validators (ACCEPT, FAIL, SKIP); multi-source updates (`commit == false`); baseline dynamic values; dynamic
 * change listeners; and the handling of invalid defaults. */

namespace flow::cfg::test
{

namespace
{
using flow::test::Test_logger;
using std::ostringstream;
using std::string;

// Static config.
struct S_opts :
  public util::Shared_ptr_alias_holder<boost::shared_ptr<S_opts>>
{
  int m_s_count = 1;
  int m_s_max = 10;
  string m_s_name{"s"};
};

// Dynamic config.
struct D_opts :
  public util::Shared_ptr_alias_holder<boost::shared_ptr<D_opts>>
{
  int m_d_count = 10;
  string m_d_name{"d"};
  // Used for conditional config (see Config_manager doc header); hence non-accumulating.
  int m_d_section = 0;
};

// Static and dynamic config of a second module; used when testing managers of 2 static/dynamic pairs.
struct S2_opts :
  public util::Shared_ptr_alias_holder<boost::shared_ptr<S2_opts>>
{
  int m_s2_count = 2;
};

struct D2_opts :
  public util::Shared_ptr_alias_holder<boost::shared_ptr<D2_opts>>
{
  int m_d2_count = 40;
  string m_d2_name{"d2"};
};

// Config with an individually-invalid default, which a config source is thus required to override.
struct Port_opts :
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Port_opts>>
{
  int m_port = 0;
  int m_other = 1;
};

void declare_s_opts(const Option_set<S_opts>::Declare_options_func_args& args)
{
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_s_count, "Static count; non-negative.", val >= 0);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_s_max, "Static max; `s-count` must not exceed it.", val >= 0);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_s_name, "Static name.", true);
}

void declare_d_opts(const Option_set<D_opts>::Declare_options_func_args& args)
{
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_d_count, "Dynamic count; non-negative.", val >= 0);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_d_name, "Dynamic name.", true);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC(m_d_section, "Section to which the config source applies.", val >= 0);
}

void declare_s2_opts(const Option_set<S2_opts>::Declare_options_func_args& args)
{
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_s2_count, "Static count of module 2; non-negative.", val >= 0);
}

void declare_d2_opts(const Option_set<D2_opts>::Declare_options_func_args& args)
{
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_d2_count, "Dynamic count of module 2; non-negative.", val >= 0);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_d2_name, "Dynamic name of module 2.", true);
}

void declare_port_opts(const Option_set<Port_opts>::Declare_options_func_args& args)
{
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_port, "Port; positive.", val > 0);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_other, "Something else.", true);
}

using S_validator = Final_validator_func<S_opts>::Type;
using D_validator = Final_validator_func<D_opts>::Type;

// A static-only manager and a static-plus-dynamic one.
using S_mgr = Config_manager<S_opts, Null_value_set>;
using S_d_mgr = Config_manager<S_opts, D_opts>;

// An inter-option consistency check, written the documented way (via FLOW_CFG_OPT_CHECK_ASSERT()).
Final_validator_outcome validate_s_opts(log::Logger* logger_ptr, const S_opts& opts)
{
  FLOW_LOG_SET_CONTEXT(logger_ptr, Flow_log_component::S_UNCAT);

  auto outcome = Final_validator_outcome::S_ACCEPT;
  FLOW_CFG_OPT_CHECK_ASSERT(opts.m_s_count <= opts.m_s_max);
  FLOW_CFG_OPT_CHECK_ASSERT(!opts.m_s_name.empty());
  return outcome;
}

// Validators with a fixed outcome.
template<typename Value_set>
typename Final_validator_func<Value_set>::Type fixed_validator(Final_validator_outcome outcome)
{
  return [outcome](const Value_set&) -> Final_validator_outcome { return outcome; };
}

/* Conditional-config validator: a source applies only if its `d-section` setting equals `my_section`; otherwise it
 * is to be skipped. */
D_validator section_validator(int my_section)
{
  return [my_section](const D_opts& opts) -> Final_validator_outcome
  {
    return (opts.m_d_section == my_section) ? Final_validator_outcome::S_ACCEPT : Final_validator_outcome::S_SKIP;
  };
}

// apply_static() of a file with the given contents.
template<typename Mgr>
bool apply_s(Mgr* mgr, const string& contents,
             const S_validator& validator = Final_validator_func<S_opts>::null(), bool commit = true)
{
  const Cfg_file file{contents};
  return mgr->template apply_static<S_opts>(file.m_path, validator, commit);
}

// apply_dynamic() of a file with the given contents.
bool apply_d(S_d_mgr* mgr, const string& contents,
             const D_validator& validator = Final_validator_func<D_opts>::null(), bool commit = true)
{
  const Cfg_file file{contents};
  return mgr->apply_dynamic<D_opts>(file.m_path, validator, commit);
}

// apply_static_and_dynamic() of a file with the given contents.
bool apply_s_d(S_d_mgr* mgr, const string& contents,
               const S_validator& s_validator = Final_validator_func<S_opts>::null(),
               const D_validator& d_validator = Final_validator_func<D_opts>::null(), bool commit = true)
{
  const Cfg_file file{contents};
  return mgr->apply_static_and_dynamic(file.m_path, s_validator, d_validator, commit);
}

template<typename Mgr>
string state_str(const Mgr& mgr)
{
  ostringstream os;
  mgr.state_to_ostream(os);
  return os.str();
}

template<typename Mgr>
string help_str(const Mgr& mgr)
{
  ostringstream os;
  mgr.help_to_ostream(os);
  return os.str();
}
} // namespace (anon)

TEST(Config_manager_test, Static_only)
{
  Test_logger logger;
  S_mgr mgr{&logger, "s-mgr", &declare_s_opts, null_declare_opts_func()};
  const auto& s_opts = mgr.static_values<S_opts>(0);
  const S_validator s_validator = [&](const S_opts& opts) { return validate_s_opts(&logger, opts); };

  // Before any apply_static(): defaults.
  EXPECT_EQ(s_opts.m_s_count, 1);
  EXPECT_EQ(s_opts.m_s_name, "s");

  // Help and state mention the static options and nothing dynamic (the dynamic slot being a Null_value_set).
  const string help = help_str(mgr);
  EXPECT_TRUE(contains(help, "[STATIC] [0]")) << "Help: [\n" << help << "].";
  EXPECT_TRUE(contains(help, "--s-count")) << "Help: [\n" << help << "].";
  EXPECT_FALSE(contains(help, "DYNAMIC")) << "Help: [\n" << help << "].";
  mgr.log_help();

  ASSERT_TRUE(apply_s(&mgr, "s-count=5\n", s_validator));
  EXPECT_EQ(s_opts.m_s_count, 5);
  // static_values() always returns the same reference; all_static_values() emits a pointer to the same thing.
  EXPECT_EQ(&s_opts, &mgr.static_values<S_opts>(0));
  const S_opts* s_opts_ptr = nullptr;
  mgr.all_static_values(&s_opts_ptr);
  EXPECT_EQ(s_opts_ptr, &s_opts);

  // Failures: canonical values stay as they were.
  EXPECT_FALSE(apply_s(&mgr, "s-count=6\nno-such-option=1\n", s_validator)); // Unknown option.
  EXPECT_FALSE(apply_s(&mgr, "s-count=-1\n", s_validator)); // Individual-option validator.
  EXPECT_FALSE(apply_s(&mgr, "s-count=11\n", s_validator)); // Final-validator: exceeds default `s-max`.
  EXPECT_FALSE(apply_s(&mgr, "s-count=6\n", fixed_validator<S_opts>(Final_validator_outcome::S_FAIL)));
  EXPECT_EQ(s_opts.m_s_count, 5);
  // The final-validator checks the candidate as a whole; so raising the max in the same source makes it pass.
  ASSERT_TRUE(apply_s(&mgr, "s-count=11\ns-max=20\n", s_validator));
  EXPECT_EQ(s_opts.m_s_count, 11);
  EXPECT_EQ(s_opts.m_s_max, 20);

  // SKIP: success, but the source is not applied.
  ASSERT_TRUE(apply_s(&mgr, "s-count=3\n", fixed_validator<S_opts>(Final_validator_outcome::S_SKIP)));
  EXPECT_EQ(s_opts.m_s_count, 11);

  /* A failed update's values must not carry into the next update.  Here the source parses fine (so its values make
   * it into the candidate), then fails final-validation; the next source does not mention `s-name`. */
  EXPECT_FALSE(apply_s(&mgr, "s-count=7\ns-name=must-not-stick\n",
                       fixed_validator<S_opts>(Final_validator_outcome::S_FAIL)));
  ASSERT_TRUE(apply_s(&mgr, "s-count=8\n", s_validator));
  EXPECT_EQ(s_opts.m_s_count, 8);
  EXPECT_EQ(s_opts.m_s_name, "s");

  // While a final-validator runs, the candidate (not yet canonical) is available via the *_candidate() accessors.
  bool validator_ran = false;
  const S_validator checking_validator = [&](const S_opts& opts)
  {
    validator_ran = true;
    EXPECT_EQ(opts.m_s_count, 9);
    EXPECT_EQ(s_opts.m_s_count, 8);
    EXPECT_EQ(&mgr.static_values_candidate<S_opts>(0), &opts);
    const S_opts* candidate_ptr = nullptr;
    mgr.all_static_values_candidates(&candidate_ptr);
    EXPECT_EQ(candidate_ptr, &opts);
    return Final_validator_outcome::S_ACCEPT;
  };
  ASSERT_TRUE(apply_s(&mgr, "s-count=9\n", checking_validator));
  EXPECT_TRUE(validator_ran);
  EXPECT_EQ(s_opts.m_s_count, 9);
  // Outside of an update they are the same as the canonical values.
  EXPECT_EQ(&mgr.static_values_candidate<S_opts>(0), &s_opts);

  const string state = state_str(mgr);
  EXPECT_TRUE(contains(state, "[STATIC] [0]")) << "State: [\n" << state << "].";
  EXPECT_TRUE(contains(state, "(9) <==current")) << "State: [\n" << state << "].";
  EXPECT_FALSE(contains(state, "DYNAMIC")) << "State: [\n" << state << "].";
  mgr.log_state();

  const auto os_str = util::ostream_op_string(mgr);
  EXPECT_TRUE(contains(os_str, "s-mgr")) << "Output: [" << os_str << "].";
}

TEST(Config_manager_test, Static_multi_source)
{
  Test_logger logger;
  S_mgr mgr{&logger, "s-mgr", &declare_s_opts, null_declare_opts_func()};
  const auto& s_opts = mgr.static_values<S_opts>(0);
  const S_validator s_accept = Final_validator_func<S_opts>::null();

  // No update in progress: no-op.
  mgr.reject_candidates();

  // A 2-source update: nothing is canonical until the `commit == true` call.
  ASSERT_TRUE(apply_s(&mgr, "s-count=5\n", s_accept, false));
  EXPECT_EQ(s_opts.m_s_count, 1);
  EXPECT_EQ(mgr.static_values_candidate<S_opts>(0).m_s_count, 5);
  const string state = state_str(mgr);
  EXPECT_TRUE(contains(state, "incomplete multi-source update")) << "State: [\n" << state << "].";
  ASSERT_TRUE(apply_s(&mgr, "s-name=two\n", s_accept, true));
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(s_opts.m_s_name, "two");

  // A failure in a later source of an update fails the whole update; the next call starts a fresh one.
  ASSERT_TRUE(apply_s(&mgr, "s-count=6\ns-name=must-not-stick\n", s_accept, false));
  EXPECT_FALSE(apply_s(&mgr, "s-count=-1\n", s_accept, false));
  EXPECT_EQ(s_opts.m_s_count, 5);
  ASSERT_TRUE(apply_s(&mgr, "s-max=30\n", s_accept, true));
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(s_opts.m_s_name, "two");
  EXPECT_EQ(s_opts.m_s_max, 30);

  // reject_candidates() abandons an update in progress.
  ASSERT_TRUE(apply_s(&mgr, "s-count=7\ns-name=must-not-stick\n", s_accept, false));
  mgr.reject_candidates();
  EXPECT_EQ(&mgr.static_values_candidate<S_opts>(0), &s_opts);
  ASSERT_TRUE(apply_s(&mgr, "s-max=40\n", s_accept, true));
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(s_opts.m_s_name, "two");
  EXPECT_EQ(s_opts.m_s_max, 40);

  // SKIP in a multi-source update: that source alone has no effect on the candidate.
  ASSERT_TRUE(apply_s(&mgr, "s-count=8\n", s_accept, false));
  ASSERT_TRUE(apply_s(&mgr, "s-count=9\ns-name=skipped\n",
                      fixed_validator<S_opts>(Final_validator_outcome::S_SKIP), false));
  ASSERT_TRUE(apply_s(&mgr, "s-max=50\n", s_accept, true));
  EXPECT_EQ(s_opts.m_s_count, 8);
  EXPECT_EQ(s_opts.m_s_name, "two");
  EXPECT_EQ(s_opts.m_s_max, 50);
}

TEST(Config_manager_test, Static_and_dynamic)
{
  Test_logger logger;
  S_d_mgr mgr{&logger, "s-d-mgr", &declare_s_opts, &declare_d_opts};
  const auto& s_opts = mgr.static_values<S_opts>(0);

  int n_listener_calls = 0;
  const auto listener_handle = mgr.register_dynamic_change_listener<D_opts>(0, [&]() { ++n_listener_calls; });

  // Before dynamic config is initialized, state output has the dynamic options' docs but no values.
  string state = state_str(mgr);
  EXPECT_TRUE(contains(state, "[DYNAMIC] [0]")) << "State: [\n" << state << "].";
  EXPECT_TRUE(contains(state, "no dynamic config init payload")) << "State: [\n" << state << "].";

  /* apply_static_and_dynamic() accepts the options of all the value sets, static and dynamic, but nothing else.
   * (This failure occurs before anything is parsed successfully.) */
  EXPECT_FALSE(apply_s_d(&mgr, "s-count=5\nd-count=20\nno-such-option=1\n"));

  /* As in the static-only test: a failed update's values must not carry into the next one; here for the static and
   * the dynamic value set alike.  Both parse fine; then the dynamic one fails final-validation. */
  EXPECT_FALSE(apply_s_d(&mgr, "s-name=must-not-stick\nd-name=must-not-stick\n",
                         Final_validator_func<S_opts>::null(),
                         fixed_validator<D_opts>(Final_validator_outcome::S_FAIL)));
  EXPECT_EQ(n_listener_calls, 0);

  ASSERT_TRUE(apply_s_d(&mgr, "s-count=5\nd-count=20\n"));
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(s_opts.m_s_name, "s");
  auto d_opts = mgr.dynamic_values<D_opts>(0);
  ASSERT_TRUE(d_opts);
  EXPECT_EQ(d_opts->m_d_count, 20);
  EXPECT_EQ(d_opts->m_d_name, "d");
  // The initial load of dynamic config counts as a change.
  EXPECT_EQ(n_listener_calls, 1);

  D_opts::Const_ptr d_opts_via_all;
  mgr.all_dynamic_values<D_opts>(&d_opts_via_all);
  EXPECT_EQ(d_opts_via_all, d_opts);

  state = state_str(mgr);
  EXPECT_TRUE(contains(state, "[STATIC] [0]")) << "State: [\n" << state << "].";
  EXPECT_TRUE(contains(state, "[DYNAMIC] [0]")) << "State: [\n" << state << "].";
  EXPECT_TRUE(contains(state, "(20) <==current")) << "State: [\n" << state << "].";

  /* A dynamic update.  The values that apply_static_and_dynamic() loaded are the baseline: each update is applied
   * on top of the baseline, not on top of the preceding update.  So `d-count` stays 20 here. */
  ASSERT_TRUE(apply_d(&mgr, "d-name=one\n"));
  const auto d_opts1 = mgr.dynamic_values<D_opts>(0);
  EXPECT_NE(d_opts1, d_opts);
  EXPECT_EQ(d_opts1->m_d_count, 20);
  EXPECT_EQ(d_opts1->m_d_name, "one");
  EXPECT_EQ(n_listener_calls, 2);
  // A value set obtained earlier is immutable.
  EXPECT_EQ(d_opts->m_d_name, "d");

  // No change: no new value set; no listener call.
  ASSERT_TRUE(apply_d(&mgr, "d-name=one\n"));
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts1);
  EXPECT_EQ(n_listener_calls, 2);

  // The next update does not mention `d-name`; so that reverts to its baseline value.
  ASSERT_TRUE(apply_d(&mgr, "d-count=30\n"));
  d_opts = mgr.dynamic_values<D_opts>(0);
  EXPECT_EQ(d_opts->m_d_count, 30);
  EXPECT_EQ(d_opts->m_d_name, "d");
  EXPECT_EQ(n_listener_calls, 3);

  /* Unlike the other two, apply_dynamic() tolerates unknown options.  That includes static options, which are
   * unknown to the dynamic value sets; so they are ignored. */
  ASSERT_TRUE(apply_d(&mgr, "d-count=31\nsome-future-option=1\ns-count=99\n"));
  d_opts = mgr.dynamic_values<D_opts>(0);
  EXPECT_EQ(d_opts->m_d_count, 31);
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(n_listener_calls, 4);

  // Failures: no change; no listener call.
  EXPECT_FALSE(apply_d(&mgr, "d-count=-5\n"));
  EXPECT_FALSE(apply_d(&mgr, "d-count=32\n", fixed_validator<D_opts>(Final_validator_outcome::S_FAIL)));
  EXPECT_FALSE(apply_d(&mgr, "d-count=not-a-number\n"));
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  EXPECT_EQ(n_listener_calls, 4);

  // An unregistered listener is not called.
  mgr.unregister_dynamic_change_listener(listener_handle);
  ASSERT_TRUE(apply_d(&mgr, "d-count=33\n"));
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0)->m_d_count, 33);
  EXPECT_EQ(n_listener_calls, 4);
}

TEST(Config_manager_test, Dynamic_multi_source)
{
  Test_logger logger;
  S_d_mgr mgr{&logger, "s-d-mgr", &declare_s_opts, &declare_d_opts};
  const auto& s_opts = mgr.static_values<S_opts>(0);

  int n_listener_calls = 0;
  mgr.register_dynamic_change_listener<D_opts>(0, [&]() { ++n_listener_calls; });

  // apply_static() does not accept dynamic options.
  EXPECT_FALSE(apply_s(&mgr, "s-count=5\nd-count=7\n"));
  ASSERT_TRUE(apply_s(&mgr, "s-count=5\n"));
  EXPECT_EQ(s_opts.m_s_count, 5);

  // Dynamic config initialized by apply_dynamic() alone; so the baseline is the defaults.
  ASSERT_TRUE(apply_d(&mgr, "# Nothing.\n"));
  auto d_opts = mgr.dynamic_values<D_opts>(0);
  ASSERT_TRUE(d_opts);
  EXPECT_EQ(d_opts->m_d_count, 10);
  EXPECT_EQ(n_listener_calls, 1); // Initial load.

  /* Conditional config: an update consists of 3 sources, each aimed at one section.  The final-validator SKIPs the
   * sources not aimed at "our" section. */
  const string SECTION1 = "d-section=1\nd-count=101\n";
  const string SECTION2 = "d-section=2\nd-count=102\n";
  const string SECTION3 = "d-section=3\nd-count=103\n";

  // We are in none of the sections: all SKIPped; nothing changes.
  ASSERT_TRUE(apply_d(&mgr, SECTION1, section_validator(9), false));
  ASSERT_TRUE(apply_d(&mgr, SECTION2, section_validator(9), false));
  ASSERT_TRUE(apply_d(&mgr, SECTION3, section_validator(9), true));
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  EXPECT_EQ(n_listener_calls, 1);

  // We are in section 2.  Nothing is canonical, and no listener runs, until the `commit == true` call.
  ASSERT_TRUE(apply_d(&mgr, SECTION1, section_validator(2), false));
  ASSERT_TRUE(apply_d(&mgr, SECTION2, section_validator(2), false));
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  EXPECT_EQ(n_listener_calls, 1);
  ASSERT_TRUE(apply_d(&mgr, SECTION3, section_validator(2), true));
  d_opts = mgr.dynamic_values<D_opts>(0);
  EXPECT_EQ(d_opts->m_d_count, 102);
  EXPECT_EQ(d_opts->m_d_section, 2);
  EXPECT_EQ(n_listener_calls, 2);

  // A failure in a later source fails the whole update; the next call starts a fresh one, from the baseline.
  ASSERT_TRUE(apply_d(&mgr, "d-count=50\n", Final_validator_func<D_opts>::null(), false));
  EXPECT_FALSE(apply_d(&mgr, "d-count=-1\n", Final_validator_func<D_opts>::null(), false));
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  EXPECT_EQ(n_listener_calls, 2);
  ASSERT_TRUE(apply_d(&mgr, "d-name=fresh\n"));
  d_opts = mgr.dynamic_values<D_opts>(0);
  EXPECT_EQ(d_opts->m_d_count, 10); // Baseline; neither the abandoned 50 nor the preceding update's 102.
  EXPECT_EQ(d_opts->m_d_name, "fresh");
  EXPECT_EQ(d_opts->m_d_section, 0);
  EXPECT_EQ(n_listener_calls, 3);

  // reject_candidates() abandons an update in progress.
  ASSERT_TRUE(apply_d(&mgr, "d-count=60\n", Final_validator_func<D_opts>::null(), false));
  mgr.reject_candidates();
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  ASSERT_TRUE(apply_d(&mgr, "d-name=after-reject\n"));
  d_opts = mgr.dynamic_values<D_opts>(0);
  EXPECT_EQ(d_opts->m_d_count, 10);
  EXPECT_EQ(d_opts->m_d_name, "after-reject");
  EXPECT_EQ(n_listener_calls, 4);

  /* An apply_*() of another type, while an update is in progress, abandons that update.  Here a static update is in
   * progress; a dynamic one interrupts it. */
  ASSERT_TRUE(apply_s(&mgr, "s-name=must-not-stick\n", Final_validator_func<S_opts>::null(), false));
  ASSERT_TRUE(apply_d(&mgr, "d-count=61\n"));
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0)->m_d_count, 61);
  EXPECT_EQ(s_opts.m_s_name, "s");
  ASSERT_TRUE(apply_s(&mgr, "s-count=6\n"));
  EXPECT_EQ(s_opts.m_s_count, 6);
  EXPECT_EQ(s_opts.m_s_name, "s");
}

TEST(Config_manager_test, Two_pairs)
{
  using Mgr = Config_manager<S_opts, D_opts, S2_opts, D2_opts>;
  using S2_validator = Final_validator_func<S2_opts>::Type;
  using D2_validator = Final_validator_func<D2_opts>::Type;
  const auto ACCEPT = Final_validator_outcome::S_ACCEPT;
  const auto SKIP = Final_validator_outcome::S_SKIP;

  Test_logger logger;
  Mgr mgr{&logger, "2-pair-mgr", &declare_s_opts, &declare_d_opts, &declare_s2_opts, &declare_d2_opts};
  const auto& s_opts = mgr.static_values<S_opts>(0);
  const auto& s2_opts = mgr.static_values<S2_opts>(1);

  int n_listener_calls = 0;
  int n_listener2_calls = 0;
  mgr.register_dynamic_change_listener<D_opts>(0, [&]() { ++n_listener_calls; });
  mgr.register_dynamic_change_listener<D2_opts>(1, [&]() { ++n_listener2_calls; });

  const string help = help_str(mgr);
  for (const auto& header : { "[STATIC] [0]", "[DYNAMIC] [0]", "[STATIC] [1]", "[DYNAMIC] [1]" })
  {
    EXPECT_TRUE(contains(help, header)) << "Header [" << header << "] missing from help: [\n" << help << "].";
  }

  // apply_dynamic() of a file with the given contents.
  const auto apply_dyn = [&](const string& contents, const D_validator& d_validator, const D2_validator& d2_validator)
                           -> bool
  {
    const Cfg_file file{contents};
    return mgr.apply_dynamic<D_opts, D2_opts>(file.m_path, d_validator, d2_validator);
  };

  /* Initial load of everything from one file.  The value sets are handled in order (static 0, dynamic 0, static 1,
   * dynamic 1); so the final-validator of a later one can consult the candidate of an earlier static one. */
  bool s2_validator_ran = false;
  const S2_validator s2_checking_validator = [&](const S2_opts& opts)
  {
    s2_validator_ran = true;
    EXPECT_EQ(opts.m_s2_count, 6);
    EXPECT_EQ(mgr.static_values_candidate<S_opts>(0).m_s_count, 5);
    EXPECT_EQ(s_opts.m_s_count, 1); // Not canonical yet.
    return Final_validator_outcome::S_ACCEPT;
  };
  {
    const Cfg_file file{"s-count=5\nd-count=20\ns2-count=6\nd2-name=two\n"};
    ASSERT_TRUE(mgr.apply_static_and_dynamic(file.m_path,
                                             Final_validator_func<S_opts>::null(), Final_validator_func<D_opts>::null(),
                                             s2_checking_validator, Final_validator_func<D2_opts>::null()));
  }
  EXPECT_TRUE(s2_validator_ran);
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(s2_opts.m_s2_count, 6);
  auto d_opts = mgr.dynamic_values<D_opts>(0);
  auto d2_opts = mgr.dynamic_values<D2_opts>(1);
  ASSERT_TRUE(d_opts);
  ASSERT_TRUE(d2_opts);
  EXPECT_EQ(d_opts->m_d_count, 20);
  EXPECT_EQ(d2_opts->m_d2_count, 40);
  EXPECT_EQ(d2_opts->m_d2_name, "two");
  EXPECT_EQ(n_listener_calls, 1);
  EXPECT_EQ(n_listener2_calls, 1);

  // The all_*() accessors; a null argument means that slot is of no interest.
  const S_opts* s_opts_ptr = nullptr;
  const S2_opts* s2_opts_ptr = nullptr;
  mgr.all_static_values(&s_opts_ptr, &s2_opts_ptr);
  EXPECT_EQ(s_opts_ptr, &s_opts);
  EXPECT_EQ(s2_opts_ptr, &s2_opts);
  s2_opts_ptr = nullptr;
  mgr.all_static_values(static_cast<const S_opts**>(nullptr), &s2_opts_ptr);
  EXPECT_EQ(s2_opts_ptr, &s2_opts);
  D_opts::Const_ptr d_opts_via_all;
  D2_opts::Const_ptr d2_opts_via_all;
  mgr.all_dynamic_values<D_opts, D2_opts>(&d_opts_via_all, &d2_opts_via_all);
  EXPECT_EQ(d_opts_via_all, d_opts);
  EXPECT_EQ(d2_opts_via_all, d2_opts);

  bool d2_validator_ran = false;
  const D2_validator d2_recording_validator = [&](const D2_opts&)
  {
    d2_validator_ran = true;
    return Final_validator_outcome::S_ACCEPT;
  };

  /* Subtlety: if a value set's final-validator yields SKIP, then the value sets after it are skipped as well: not
   * parsed, not validated, their final-validators not run. */
  ASSERT_TRUE(apply_dyn("d-count=21\nd2-count=42\n", fixed_validator<D_opts>(SKIP), d2_recording_validator));
  EXPECT_FALSE(d2_validator_ran);
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  EXPECT_EQ(mgr.dynamic_values<D2_opts>(1), d2_opts);
  EXPECT_EQ(n_listener_calls, 1);
  EXPECT_EQ(n_listener2_calls, 1);

  // A change in one value set only: only that one gets a new value set, and only its listeners are called.
  ASSERT_TRUE(apply_dyn("d2-count=41\n", fixed_validator<D_opts>(ACCEPT), d2_recording_validator));
  EXPECT_TRUE(d2_validator_ran);
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  d2_opts = mgr.dynamic_values<D2_opts>(1);
  EXPECT_EQ(d2_opts->m_d2_count, 41);
  EXPECT_EQ(d2_opts->m_d2_name, "two"); // From the baseline.
  EXPECT_EQ(n_listener_calls, 1);
  EXPECT_EQ(n_listener2_calls, 2);

  /* SKIP in the later value set only: the earlier one is applied.  Subtlety: for the skipped one it is as-if the
   * source said nothing about it; so, as in any dynamic update, it reverts to the baseline (40), which is a change
   * from its current value (41). */
  ASSERT_TRUE(apply_dyn("d-count=21\nd2-count=42\n", fixed_validator<D_opts>(ACCEPT), fixed_validator<D2_opts>(SKIP)));
  d_opts = mgr.dynamic_values<D_opts>(0);
  d2_opts = mgr.dynamic_values<D2_opts>(1);
  EXPECT_EQ(d_opts->m_d_count, 21);
  EXPECT_EQ(d2_opts->m_d2_count, 40);
  EXPECT_EQ(n_listener_calls, 2);
  EXPECT_EQ(n_listener2_calls, 3);

  // A failure in a later value set fails the update for all of them.
  EXPECT_FALSE(apply_dyn("d-count=22\nd2-count=-1\n", fixed_validator<D_opts>(ACCEPT), d2_recording_validator));
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  EXPECT_EQ(mgr.dynamic_values<D2_opts>(1), d2_opts);

  // After a failure in an earlier value set the later ones are not even looked at.
  d2_validator_ran = false;
  EXPECT_FALSE(apply_dyn("d-count=-1\nd2-count=43\n", fixed_validator<D_opts>(ACCEPT), d2_recording_validator));
  EXPECT_FALSE(d2_validator_ran);
  EXPECT_EQ(mgr.dynamic_values<D_opts>(0), d_opts);
  EXPECT_EQ(mgr.dynamic_values<D2_opts>(1), d2_opts);
  EXPECT_EQ(n_listener_calls, 2);
  EXPECT_EQ(n_listener2_calls, 3);
}

TEST(Config_manager_test, Two_pairs_with_null)
{
  // Module 1 has static config only; module 2 has both.
  using Mgr = Config_manager<S_opts, Null_value_set, S2_opts, D2_opts>;
  using S2_validator = Final_validator_func<S2_opts>::Type;
  const auto SKIP = Final_validator_outcome::S_SKIP;

  Test_logger logger;
  Mgr mgr{&logger, "null-mgr", &declare_s_opts, null_declare_opts_func(), &declare_s2_opts, &declare_d2_opts};
  const auto& s_opts = mgr.static_values<S_opts>(0);
  const auto& s2_opts = mgr.static_values<S2_opts>(1);

  int n_listener2_calls = 0;
  mgr.register_dynamic_change_listener<D2_opts>(1, [&]() { ++n_listener2_calls; });

  // apply_static() of a file with the given contents.
  const auto apply_st = [&](const string& contents, const S_validator& s_validator, const S2_validator& s2_validator)
                          -> bool
  {
    const Cfg_file file{contents};
    return mgr.apply_static<S_opts, S2_opts>(file.m_path, s_validator, s2_validator);
  };

  bool s2_validator_ran = false;
  const S2_validator s2_recording_validator = [&](const S2_opts&)
  {
    s2_validator_ran = true;
    return Final_validator_outcome::S_ACCEPT;
  };

  ASSERT_TRUE(apply_st("s-count=5\ns2-count=6\n", Final_validator_func<S_opts>::null(), s2_recording_validator));
  EXPECT_TRUE(s2_validator_ran);
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(s2_opts.m_s2_count, 6);

  // SKIP in an earlier static value set skips the later ones too.
  s2_validator_ran = false;
  ASSERT_TRUE(apply_st("s-count=7\ns2-count=8\n", fixed_validator<S_opts>(SKIP), s2_recording_validator));
  EXPECT_FALSE(s2_validator_ran);
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(s2_opts.m_s2_count, 6);

  // Dynamic config: the Null_value_set slot takes null_final_validator_func() and is otherwise ignored.
  {
    const Cfg_file file{"d2-count=41\n"};
    ASSERT_TRUE((mgr.apply_dynamic<Null_value_set, D2_opts>(file.m_path, null_final_validator_func(),
                                                            Final_validator_func<D2_opts>::null())));
  }
  const auto d2_opts = mgr.dynamic_values<D2_opts>(1);
  ASSERT_TRUE(d2_opts);
  EXPECT_EQ(d2_opts->m_d2_count, 41);
  EXPECT_EQ(n_listener2_calls, 1);

  D2_opts::Const_ptr d2_opts_via_all;
  mgr.all_dynamic_values<Null_value_set, D2_opts>(nullptr, &d2_opts_via_all);
  EXPECT_EQ(d2_opts_via_all, d2_opts);

  // Output omits the Null_value_set slot.
  const string state = state_str(mgr);
  for (const auto& header : { "[STATIC] [0]", "[STATIC] [1]", "[DYNAMIC] [1]" })
  {
    EXPECT_TRUE(contains(state, header)) << "Header [" << header << "] missing from state: [\n" << state << "].";
  }
  EXPECT_FALSE(contains(state, "[DYNAMIC] [0]")) << "State: [\n" << state << "].";

  // Dynamic_cfg_context for a dynamic slot other than 0.
  const Dynamic_cfg_context<D2_opts, int> count_ctx{mgr,
                                                    [](const D2_opts& root) -> const int& { return root.m_d2_count; },
                                                    1};
  EXPECT_EQ(count_ctx.root_dynamic_cfg(), d2_opts);
  EXPECT_EQ(*count_ctx.dynamic_cfg(), 41);
}

TEST(Config_manager_test, Static_config_manager)
{
  Test_logger logger;

  // Usable as a `const` object just to print help.
  {
    const Static_config_manager<S_opts> help_mgr{&logger, "help-mgr", &declare_s_opts};
    const string help = help_str(help_mgr);
    EXPECT_TRUE(contains(help, "--s-count")) << "Help: [\n" << help << "].";
    EXPECT_TRUE(contains(help, "Static count; non-negative.")) << "Help: [\n" << help << "].";
    help_mgr.log_help();
  }

  Static_config_manager<S_opts> mgr{&logger, "static-mgr", &declare_s_opts};
  const auto& s_opts = mgr.values();
  EXPECT_EQ(s_opts.m_s_count, 1);

  {
    const Cfg_file file{"s-count=5\n"};
    ASSERT_TRUE(mgr.apply(file.m_path));
  }
  EXPECT_EQ(s_opts.m_s_count, 5);
  EXPECT_EQ(&mgr.values(), &s_opts);

  const auto apply_with_validator = [&](const string& contents, const S_validator& validator) -> bool
  {
    const Cfg_file file{contents};
    return mgr.apply(file.m_path, validator);
  };
  const S_validator s_validator = [&](const S_opts& opts) { return validate_s_opts(&logger, opts); };

  ASSERT_TRUE(apply_with_validator("s-count=6\n", s_validator));
  EXPECT_EQ(s_opts.m_s_count, 6);
  // Failures.
  EXPECT_FALSE(apply_with_validator("s-count=11\n", s_validator)); // Final-validator.
  EXPECT_FALSE(apply_with_validator("s-count=-1\n", s_validator)); // Individual-option validator.
  EXPECT_FALSE(apply_with_validator("s-count=7\nno-such-option=1\n", s_validator)); // Unknown option.
  EXPECT_EQ(s_opts.m_s_count, 6);
  // SKIP: success, but not applied.
  ASSERT_TRUE(apply_with_validator("s-count=8\n", fixed_validator<S_opts>(Final_validator_outcome::S_SKIP)));
  EXPECT_EQ(s_opts.m_s_count, 6);

  const string state = state_str(mgr);
  EXPECT_TRUE(contains(state, "(6) <==current")) << "State: [\n" << state << "].";
  mgr.log_state();

  const auto os_str = util::ostream_op_string(mgr);
  EXPECT_TRUE(contains(os_str, "static-mgr")) << "Output: [" << os_str << "].";
}

TEST(Config_manager_test, Invalid_defaults)
{
  using Port_mgr = Static_config_manager<Port_opts>;

  Test_logger logger;
  Port_mgr mgr{&logger, "port-mgr", &declare_port_opts};
  const auto& port_opts = mgr.values();

  const auto apply = [&](const string& contents) -> bool
  {
    const Cfg_file file{contents};
    return mgr.apply(file.m_path);
  };
  const auto apply_allow_invalid_defaults = [&](const string& contents) -> bool
  {
    const Cfg_file file{contents};
    return mgr.apply(Port_mgr::S_ALLOW_INVALID_DEFAULTS, file.m_path);
  };

  // By default an invalid default is an error, even if the source overrides it with a valid value.
  EXPECT_FALSE(apply("port=5\n"));
  EXPECT_EQ(port_opts.m_port, 0);

  /* If invalid defaults are allowed, then the source must override each one.  (The first source here parses fine,
   * and only then is the non-overridden invalid default caught; so `other=2` must not carry into the next update.) */
  EXPECT_FALSE(apply_allow_invalid_defaults("other=2\n"));
  EXPECT_FALSE(apply_allow_invalid_defaults("port=0\n"));
  ASSERT_TRUE(apply_allow_invalid_defaults("port=5\n"));
  EXPECT_EQ(port_opts.m_port, 5);
  EXPECT_EQ(port_opts.m_other, 1);

  // The overload with a final-validator.
  {
    const Cfg_file file{"port=6\n"};
    EXPECT_FALSE(mgr.apply(Port_mgr::S_ALLOW_INVALID_DEFAULTS, file.m_path,
                           fixed_validator<Port_opts>(Final_validator_outcome::S_FAIL)));
    EXPECT_EQ(port_opts.m_port, 5);
    ASSERT_TRUE(mgr.apply(Port_mgr::S_ALLOW_INVALID_DEFAULTS, file.m_path, Final_validator_func<Port_opts>::null()));
    EXPECT_EQ(port_opts.m_port, 6);
  }
}

TEST(Config_manager_test, Dynamic_cfg_context)
{
  using Name_context = Dynamic_cfg_context<D_opts, string>;
  static_assert(std::is_same_v<Name_context::Target_ptr_type, boost::shared_ptr<const string>>,
                "By default the target pointer type should be the root pointer type rebound to const-Target.");

  Test_logger logger;
  S_d_mgr mgr{&logger, "s-d-mgr", &declare_s_opts, &declare_d_opts};
  ASSERT_TRUE(apply_s_d(&mgr, "d-name=one\n"));

  // Context that obtains its root (the whole D_opts) from the Config_manager; its target is one member.
  const Name_context name_ctx{mgr, [](const D_opts& root) -> const string& { return root.m_d_name; }};

  EXPECT_EQ(name_ctx.root_dynamic_cfg(), mgr.dynamic_values<D_opts>(0));
  auto name = name_ctx.dynamic_cfg();
  ASSERT_TRUE(name);
  EXPECT_EQ(*name, "one");
  EXPECT_EQ(name.get(), &mgr.dynamic_values<D_opts>(0)->m_d_name);

  /* The key feature: the target pointer keeps the whole root alive.  So after a dynamic update the earlier target
   * still has its earlier value; and its root goes away only once the target pointer does. */
  const boost::weak_ptr<const D_opts> root_observer{name_ctx.root_dynamic_cfg()};
  ASSERT_TRUE(apply_d(&mgr, "d-name=two\n"));
  EXPECT_EQ(*name, "one");
  EXPECT_FALSE(root_observer.expired());
  EXPECT_EQ(*name_ctx.dynamic_cfg(), "two");
  name.reset();
  EXPECT_TRUE(root_observer.expired());

  // Context with a custom root source; and with target equal to root.
  const D_opts::Const_ptr custom_root{new D_opts};
  const Dynamic_cfg_context<D_opts, D_opts> root_ctx{[&]() { return custom_root; },
                                                     [](const D_opts& root) -> const D_opts& { return root; }};
  EXPECT_EQ(root_ctx.root_dynamic_cfg(), custom_root);
  EXPECT_EQ(root_ctx.dynamic_cfg(), custom_root);
  EXPECT_EQ(root_ctx.dynamic_cfg()->m_d_count, 10);
}

} // namespace flow::cfg::test
