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

#include "flow/cfg/option_set.hpp"
#include "flow/cfg/cfg_manager.hpp"
#include "flow/cfg/test/test_cfg_util.hpp"
#include "flow/test/test_logger.hpp"
#include "flow/util/shared_ptr_alias_holder.hpp"
#include "flow/util/util.hpp"
#include "flow/common.hpp"
#include <boost/filesystem.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/unordered_set.hpp>
#include <gtest/gtest.h>
#include <array>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

/* Tests of flow::cfg::Option_set and the FLOW_CFG_OPTION_SET_DECLARE_OPTION*() macros, used directly (no
 * Config_manager).  The contract under test is in the Option_set doc header; the areas are: option naming;
 * CANONICAL/PARSING state transitions (parse, canonicalize, reject); individual-option validation; change detection;
 * accumulating versus non-accumulating options; parse_direct_values(); and output. */

namespace flow::cfg::test
{

namespace
{
using flow::test::Test_logger;
using boost::chrono::nanoseconds;
using boost::chrono::milliseconds;
using boost::chrono::microseconds;
using boost::chrono::seconds;
using boost::unordered_set;
using std::array;
using std::exception;
using std::ostringstream;
using std::string;
using std::vector;

// The config `struct` used throughout.  Its members cover each option-declaring macro and several value types.
struct Opts :
  public util::Shared_ptr_alias_holder<boost::shared_ptr<Opts>>
{
  struct Sub
  {
    int m_depth = 3;
    string m_label{"sub"};
  };

  static constexpr size_t S_N_SLOTS = 2;

  int m_count = 10;
  string m_name{"dflt"};
  bool m_flag = false;
  Fine_duration m_timeout{seconds{5}};
  fs::path m_path{"/default/path"};
  vector<int> m_list;
  // Nested via composition: option names gain a `sub.` prefix.
  Sub m_sub;
  // Declared via the _KEYED() macro: option names are `slots.<idx>.<member>`.
  array<Sub, S_N_SLOTS> m_slots;
  // Declared via the _NO_ACC() macro: reset to default at the start of each parsed config source.
  int m_mode = 0;
  // Declared twice: under its auto-generated name and under a manually specified alias.
  int m_legacy = 1;
};

void declare_opts(const Option_set<Opts>::Declare_options_func_args& args)
{
  // The validator expressions deliberately refer to `val`: that is the documented way to write them.
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_count, "A count in [0, 100].", (val >= 0) && (val <= 100));
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_name, "A non-empty name.", !val.empty());
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_flag, "A flag.", true);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_timeout, "A positive timeout.", val > Fine_duration::zero());
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_path, "A path; may be empty.", true);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_list, "A list of up to 3 numbers.", val.size() <= 3);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_sub.m_depth, "Nested depth; positive.", val > 0);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_sub.m_label, "Nested label.", true);
  /* Subtlety: the _KEYED() macro computes each option name into a temporary string, which is gone by the time
   * anything is parsed; so the tests that parse `slots.*` options also check that Option_set keeps its own copies. */
  for (size_t idx = 0; idx != Opts::S_N_SLOTS; ++idx)
  {
    FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED(m_slots[idx].m_depth, "Slot depth; positive.", val > 0, idx);
    FLOW_CFG_OPTION_SET_DECLARE_OPTION_KEYED(m_slots[idx].m_label, "Slot label.", true, idx);
  }
  FLOW_CFG_OPTION_SET_DECLARE_OPTION_NO_ACC(m_mode, "A mode; not accumulated from source to source.", val >= 0);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION(m_legacy, "A setting with a legacy alias; non-zero.", val != 0);
  FLOW_CFG_OPTION_SET_DECLARE_OPTION_MANUALLY_NAMED(m_legacy, "old-legacy", "Legacy alias of `legacy`.", val != 0,
                                                    false);
}

using Opt_set = Option_set<Opts>;

// Parses `contents` (as a file) into `opt_set`, disallowing unknown options; returns whether it succeeded.
bool parse(Opt_set* opt_set, const string& contents)
{
  const Cfg_file file{contents};
  bool ok = false;
  opt_set->parse_config_file(file.m_path, false, &ok);
  return ok;
}

template<typename Value>
string value_str(const Value& val)
{
  ostringstream os;
  value_to_ostream(os, val);
  return os.str();
}

// EXPECTs that every option-backed member of `opts` is at its default.
void expect_defaults(const Opts& opts)
{
  const Opts dflt;
  EXPECT_EQ(opts.m_count, dflt.m_count);
  EXPECT_EQ(opts.m_name, dflt.m_name);
  EXPECT_EQ(opts.m_flag, dflt.m_flag);
  EXPECT_EQ(opts.m_timeout, dflt.m_timeout);
  EXPECT_EQ(opts.m_path, dflt.m_path);
  EXPECT_EQ(opts.m_list, dflt.m_list);
  EXPECT_EQ(opts.m_sub.m_depth, dflt.m_sub.m_depth);
  EXPECT_EQ(opts.m_sub.m_label, dflt.m_sub.m_label);
  for (size_t idx = 0; idx != Opts::S_N_SLOTS; ++idx)
  {
    EXPECT_EQ(opts.m_slots[idx].m_depth, dflt.m_slots[idx].m_depth);
    EXPECT_EQ(opts.m_slots[idx].m_label, dflt.m_slots[idx].m_label);
  }
  EXPECT_EQ(opts.m_mode, dflt.m_mode);
  EXPECT_EQ(opts.m_legacy, dflt.m_legacy);
}
} // namespace (anon)

TEST(Option_set_test, Opt_names)
{
  EXPECT_EQ(value_set_member_id_to_opt_name("m_cool_object.m_cool_sub_object->m_badass_sub_guy.m_cool_option_name"),
            "cool-object.cool-sub-object.badass-sub-guy.cool-option-name");
  EXPECT_EQ(value_set_member_id_to_opt_name("m_cool_object.m_cool_sub_object->m_badass_sub_guy.m_cool_option_name",
                                            '_'),
            "cool_object.cool_sub_object.badass_sub_guy.cool_option_name");
  EXPECT_EQ(value_set_member_id_to_opt_name("m_simple"), "simple");
  // The `m_` prefix is optional; white-space (as the preprocessor may leave in a stringified argument) is ignored.
  EXPECT_EQ(value_set_member_id_to_opt_name("simple_thing"), "simple-thing");
  EXPECT_EQ(value_set_member_id_to_opt_name(" m_a . m_b_c -> m_d "), "a.b-c.d");

  EXPECT_EQ(value_set_member_id_to_opt_name_keyed
              ("m_cool_object.m_cool_sub_object->m_badass_sub_guy[cool_key].m_cool_option_name", 3),
            "cool-object.cool-sub-object.badass-sub-guy.3.cool-option-name");
  EXPECT_EQ(value_set_member_id_to_opt_name_keyed("m_grand_parents[idx]->m_name", string{"alice"}),
            "grand-parents.alice.name");
  /* Subtlety: the key is spliced in as-is; the `m_` and `_` replacements that apply to the member identifiers
   * around it must not be applied to the key itself. */
  EXPECT_EQ(value_set_member_id_to_opt_name_keyed("m_some_map[key].m_some_val", string{"m_odd_key"}),
            "some-map.m_odd_key.some-val");
  // Subtlety: with 2+ `[...]` fragments it is the last one that is replaced.
  EXPECT_EQ(value_set_member_id_to_opt_name_keyed("m_outer[idx1].m_inner[idx2].m_val", 7),
            "outer[idx1].inner.7.val");

  Test_logger logger;
  const Opt_set opt_set{&logger, "names", &declare_opts};
  const unordered_set<string> exp_names
    = { "count", "name", "flag", "timeout", "path", "list", "sub.depth", "sub.label",
        "slots.0.depth", "slots.0.label", "slots.1.depth", "slots.1.label", "mode", "legacy", "old-legacy" };
  EXPECT_EQ(opt_set.option_names(), exp_names);
  EXPECT_FALSE(opt_set.null());

  const Option_set<Null_value_set> null_opt_set{&logger, "null", null_declare_opts_func()};
  EXPECT_TRUE(null_opt_set.null());
  EXPECT_TRUE(null_opt_set.option_names().empty());
}

TEST(Option_set_test, Parse_and_canonicalize)
{
  Test_logger logger;
  Opt_set opt_set{&logger, "parse", &declare_opts};

  // Freshly constructed: CANONICAL state, at defaults.
  EXPECT_EQ(opt_set.values_candidate(), nullptr);
  expect_defaults(opt_set.values());

  const string CONTENTS
    = "count=42\n"
      "name=some name with spaces\n"
      "flag=true\n"
      "timeout=250 ms\n"
      "path=/some/where\n"
      "list=1\n"
      "list=2\n"
      "list=3\n"
      "mode=7\n"
      "old-legacy=5\n"
      "[sub]\n"
      "depth=30\n"
      "label=nested\n"
      "[slots.1]\n"
      "depth=11\n"
      "label=slot one\n";

  ASSERT_TRUE(parse(&opt_set, CONTENTS));

  // PARSING state: the candidate has the parsed values; the canonical values are untouched.
  const auto candidate = opt_set.values_candidate();
  ASSERT_NE(candidate, nullptr);
  expect_defaults(opt_set.values());

  const auto check_parsed = [](const Opts& opts)
  {
    EXPECT_EQ(opts.m_count, 42);
    EXPECT_EQ(opts.m_name, "some name with spaces");
    EXPECT_TRUE(opts.m_flag);
    EXPECT_EQ(opts.m_timeout, milliseconds{250});
    EXPECT_EQ(opts.m_path, fs::path{"/some/where"});
    EXPECT_EQ(opts.m_list, (vector<int>{ 1, 2, 3 }));
    EXPECT_EQ(opts.m_sub.m_depth, 30);
    EXPECT_EQ(opts.m_sub.m_label, "nested");
    // Slot 0 was not in the file.
    EXPECT_EQ(opts.m_slots[0].m_depth, 3);
    EXPECT_EQ(opts.m_slots[0].m_label, "sub");
    EXPECT_EQ(opts.m_slots[1].m_depth, 11);
    EXPECT_EQ(opts.m_slots[1].m_label, "slot one");
    EXPECT_EQ(opts.m_mode, 7);
    // Set via the manually named alias.
    EXPECT_EQ(opts.m_legacy, 5);
  };
  check_parsed(*candidate);

  bool changed = false;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_TRUE(changed);
  EXPECT_EQ(opt_set.values_candidate(), nullptr);
  check_parsed(opt_set.values());

  // Parsing the same thing again changes nothing.  (`mode` is non-accumulating but is in the source again.)
  ASSERT_TRUE(parse(&opt_set, CONTENTS));
  changed = true;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_FALSE(changed);
  check_parsed(opt_set.values());

  // A change in just one option is detected.  (`mode=7` is repeated so that it does not reset.)
  ASSERT_TRUE(parse(&opt_set, "legacy=6\nmode=7\n"));
  opt_set.canonicalize_candidate(&changed);
  EXPECT_TRUE(changed);
  EXPECT_EQ(opt_set.values().m_legacy, 6);
  EXPECT_EQ(opt_set.values().m_count, 42);

  // mutable_values_copy() yields a separate copy.
  const auto values_copy = opt_set.mutable_values_copy();
  ASSERT_TRUE(values_copy);
  EXPECT_NE(values_copy.get(), &opt_set.values());
  EXPECT_EQ(values_copy->m_count, 42);
  EXPECT_EQ(values_copy->m_legacy, 6);
}

TEST(Option_set_test, Empty_path)
{
  Test_logger logger;
  Opt_set opt_set{&logger, "path", &declare_opts};
  ASSERT_FALSE(opt_set.values().m_path.empty());

  // An empty value is allowed for a `path` option (see boost::filesystem validate() overload in cfg_fwd.hpp).
  ASSERT_TRUE(parse(&opt_set, "path=\n"));
  opt_set.canonicalize_candidate();
  EXPECT_TRUE(opt_set.values().m_path.empty());

  // Same for an all-spaces value.
  ASSERT_TRUE(parse(&opt_set, "path=/not/empty\n"));
  opt_set.canonicalize_candidate();
  ASSERT_FALSE(opt_set.values().m_path.empty());
  ASSERT_TRUE(parse(&opt_set, "path=   \n"));
  opt_set.canonicalize_candidate();
  EXPECT_TRUE(opt_set.values().m_path.empty());
}

TEST(Option_set_test, Parse_failures)
{
  Test_logger logger;
  Opt_set opt_set{&logger, "fail", &declare_opts};

  ASSERT_TRUE(parse(&opt_set, "count=5\nname=first\n"));

  // EXPECTs that the candidate is as it was just after the successful parse above.
  const auto expect_candidate_untouched = [&]()
  {
    const auto candidate = opt_set.values_candidate();
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->m_count, 5);
    EXPECT_EQ(candidate->m_name, "first");
    EXPECT_EQ(candidate->m_sub.m_depth, 3);
    EXPECT_FALSE(candidate->m_flag);
  };

  /* Subtlety: Option_set promises the candidate is untouched by a failed parse, though internally a source is
   * processed in stages (overall syntax; then each value's conversion to its type; then validators, at which point
   * earlier options of the same source may already have been written into the candidate), each of which can fail.
   * The cases below hit each stage. */

  // A validator fails.  The other, valid settings in the same source must not be applied either.
  EXPECT_FALSE(parse(&opt_set, "name=second\nflag=true\ncount=101\n"));
  expect_candidate_untouched();
  EXPECT_FALSE(parse(&opt_set, "name=second\n[sub]\ndepth=0\n"));
  expect_candidate_untouched();
  EXPECT_FALSE(parse(&opt_set, "name=second\nlist=1\nlist=2\nlist=3\nlist=4\n"));
  expect_candidate_untouched();
  EXPECT_FALSE(parse(&opt_set, "name=second\n[slots.0]\ndepth=-1\n"));
  expect_candidate_untouched();
  EXPECT_FALSE(parse(&opt_set, "name=second\nold-legacy=0\n"));
  expect_candidate_untouched();

  // Value of the wrong type.
  EXPECT_FALSE(parse(&opt_set, "name=second\ncount=abc\n"));
  expect_candidate_untouched();
  EXPECT_FALSE(parse(&opt_set, "name=second\ntimeout=soon\n"));
  expect_candidate_untouched();

  // Unknown option, with unknown options disallowed.
  EXPECT_FALSE(parse(&opt_set, "name=second\nno-such-option=1\n"));
  expect_candidate_untouched();

  // Unknown option, with unknown options allowed, but this one is not in the approved-list.
  {
    const Cfg_file file{"name=second\nno-such-option=1\n"};
    bool unapproved_ok = true;
    opt_set.parse_config_file(file.m_path, true, &unapproved_ok, { "some-other-option" });
    EXPECT_FALSE(unapproved_ok);
    expect_candidate_untouched();
  }

  // Malformed source.
  EXPECT_FALSE(parse(&opt_set, "name=second\nthis line is not a setting\n"));
  expect_candidate_untouched();

  // No such file.
  bool ok = true;
  opt_set.parse_config_file(fs::temp_directory_path() / "flow_cfg_option_set_test-no-such-file.cfg", false, &ok);
  EXPECT_FALSE(ok);
  expect_candidate_untouched();

  // In throwing mode the exception message names the option, the failed condition, and the offending value.
  {
    const Cfg_file file{"count=101\n"};
    string msg;
    try
    {
      opt_set.parse_config_file(file.m_path, false);
    }
    catch (const exception& exc)
    {
      msg = exc.what();
    }
    EXPECT_TRUE(contains(msg, "[count]")) << "Message: [" << msg << "].";
    EXPECT_TRUE(contains(msg, "[(val >= 0) && (val <= 100)]")) << "Message: [" << msg << "].";
    EXPECT_TRUE(contains(msg, "[101]")) << "Message: [" << msg << "].";
    expect_candidate_untouched();
  }

  // After all that a valid source still applies on top of the earlier one.
  ASSERT_TRUE(parse(&opt_set, "name=second\n"));
  opt_set.canonicalize_candidate();
  EXPECT_EQ(opt_set.values().m_count, 5);
  EXPECT_EQ(opt_set.values().m_name, "second");
}

TEST(Option_set_test, Unregistered_opts)
{
  Test_logger logger;
  Opt_set opt_set{&logger, "unreg", &declare_opts};
  const Cfg_file file{"count=5\nother-thing=1\n[other-section]\nstuff=abc\n"};
  bool ok;

  ok = true;
  opt_set.parse_config_file(file.m_path, false, &ok);
  EXPECT_FALSE(ok);

  // Any unknown option is fine, if allowed and given no approved-list.
  ok = false;
  opt_set.parse_config_file(file.m_path, true, &ok);
  EXPECT_TRUE(ok);
  ASSERT_NE(opt_set.values_candidate(), nullptr);
  EXPECT_EQ(opt_set.values_candidate()->m_count, 5);
  opt_set.reject_candidate();

  // With an approved-list every unknown option must be in it.
  ok = true;
  opt_set.parse_config_file(file.m_path, true, &ok, { "other-thing" });
  EXPECT_FALSE(ok);
  opt_set.reject_candidate();

  ok = false;
  opt_set.parse_config_file(file.m_path, true, &ok, { "other-thing", "other-section.stuff", "irrelevant" });
  EXPECT_TRUE(ok);
  ASSERT_NE(opt_set.values_candidate(), nullptr);
  EXPECT_EQ(opt_set.values_candidate()->m_count, 5);
}

TEST(Option_set_test, Reject_candidate)
{
  Test_logger logger;
  Opt_set opt_set{&logger, "reject", &declare_opts};

  // In CANONICAL state it is a no-op.
  opt_set.reject_candidate();
  EXPECT_EQ(opt_set.values_candidate(), nullptr);
  expect_defaults(opt_set.values());

  ASSERT_TRUE(parse(&opt_set, "count=5\nname=rejected\n[sub]\ndepth=9\n"));
  ASSERT_NE(opt_set.values_candidate(), nullptr);
  opt_set.reject_candidate();
  EXPECT_EQ(opt_set.values_candidate(), nullptr);
  expect_defaults(opt_set.values());

  /* The key check: the next parse must start from the canonical values, not from the rejected candidate.  So the
   * options set in the rejected source, but absent from this one, must be at their canonical values (defaults). */
  ASSERT_TRUE(parse(&opt_set, "flag=true\n"));
  const auto candidate = opt_set.values_candidate();
  ASSERT_NE(candidate, nullptr);
  EXPECT_TRUE(candidate->m_flag);
  EXPECT_EQ(candidate->m_count, 10);
  EXPECT_EQ(candidate->m_name, "dflt");
  EXPECT_EQ(candidate->m_sub.m_depth, 3);

  bool changed = false;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_TRUE(changed);
  EXPECT_TRUE(opt_set.values().m_flag);
  EXPECT_EQ(opt_set.values().m_count, 10);
  EXPECT_EQ(opt_set.values().m_name, "dflt");
  EXPECT_EQ(opt_set.values().m_sub.m_depth, 3);

  // Same but with nothing at all in the next source: no change must be detected.
  ASSERT_TRUE(parse(&opt_set, "count=77\n"));
  opt_set.reject_candidate();
  ASSERT_TRUE(parse(&opt_set, "# Nothing.\n"));
  changed = true;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_FALSE(changed);
  EXPECT_EQ(opt_set.values().m_count, 10);
}

TEST(Option_set_test, Multi_source_and_no_acc)
{
  Test_logger logger;
  Opt_set opt_set{&logger, "multi", &declare_opts};

  // Source 1.
  ASSERT_TRUE(parse(&opt_set, "count=5\nmode=7\nname=one\n"));
  ASSERT_NE(opt_set.values_candidate(), nullptr);
  EXPECT_EQ(opt_set.values_candidate()->m_mode, 7);

  /* Source 2, in the same PARSING state: regular options accumulate (`count` stays; `name` is overwritten); the
   * non-accumulating one (`mode`), being absent from this source, resets to its default. */
  ASSERT_TRUE(parse(&opt_set, "name=two\n"));
  const auto candidate = opt_set.values_candidate();
  ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(candidate->m_count, 5);
  EXPECT_EQ(candidate->m_name, "two");
  EXPECT_EQ(candidate->m_mode, 0);

  bool changed = false;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_TRUE(changed);
  EXPECT_EQ(opt_set.values().m_count, 5);
  EXPECT_EQ(opt_set.values().m_name, "two");
  EXPECT_EQ(opt_set.values().m_mode, 0);

  /* Subtlety: accumulation is per option, not per list element: a list option present in a later source replaces
   * the list from an earlier source (it is not appended to it). */
  ASSERT_TRUE(parse(&opt_set, "list=1\nlist=2\n"));
  ASSERT_TRUE(parse(&opt_set, "list=9\n"));
  opt_set.canonicalize_candidate();
  EXPECT_EQ(opt_set.values().m_list, (vector<int>{ 9 }));

  /* Change detection compares the final candidate to the canonical values: a value changed by one source and
   * changed back by the next one is not a change. */
  ASSERT_TRUE(parse(&opt_set, "count=6\nmode=3\n"));
  ASSERT_TRUE(parse(&opt_set, "count=5\n"));
  changed = true;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_FALSE(changed);
  EXPECT_EQ(opt_set.values().m_count, 5);
  EXPECT_EQ(opt_set.values().m_mode, 0);
}

TEST(Option_set_test, Parse_direct_values)
{
  Test_logger logger;
  Opt_set opt_set{&logger, "direct", &declare_opts};

  ASSERT_TRUE(parse(&opt_set, "count=5\nname=baseline\n"));
  opt_set.canonicalize_candidate();
  const Opts baseline = opt_set.values();

  // From CANONICAL state: enters PARSING state with the given values as the candidate.
  Opts direct = baseline;
  direct.m_count = 77;
  direct.m_slots[1].m_label = "direct";
  opt_set.parse_direct_values(direct);
  auto candidate = opt_set.values_candidate();
  ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(candidate->m_count, 77);
  EXPECT_EQ(candidate->m_name, "baseline");
  EXPECT_EQ(candidate->m_slots[1].m_label, "direct");
  EXPECT_EQ(opt_set.values().m_count, 5);

  bool changed = false;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_TRUE(changed);
  EXPECT_EQ(opt_set.values().m_count, 77);
  EXPECT_EQ(opt_set.values().m_slots[1].m_label, "direct");

  // The baseline pattern: load the baseline; parse a source on top of it.
  opt_set.parse_direct_values(baseline);
  ASSERT_TRUE(parse(&opt_set, "name=update\n"));
  changed = false;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_TRUE(changed);
  EXPECT_EQ(opt_set.values().m_count, 5); // From baseline; not the 77 from the preceding update.
  EXPECT_EQ(opt_set.values().m_name, "update");
  EXPECT_EQ(opt_set.values().m_slots[1].m_label, "sub");

  // In PARSING state it overwrites whatever the candidate has accumulated.
  ASSERT_TRUE(parse(&opt_set, "count=33\nflag=true\n"));
  opt_set.parse_direct_values(opt_set.values());
  candidate = opt_set.values_candidate();
  ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(candidate->m_count, 5);
  EXPECT_FALSE(candidate->m_flag);
  changed = true;
  opt_set.canonicalize_candidate(&changed);
  EXPECT_FALSE(changed);

  // It does not validate; validate_values_candidate() does.
  Opts invalid = opt_set.values();
  invalid.m_count = 1000;
  opt_set.parse_direct_values(invalid);
  ASSERT_NE(opt_set.values_candidate(), nullptr);
  EXPECT_EQ(opt_set.values_candidate()->m_count, 1000);
  bool ok = true;
  opt_set.validate_values_candidate(&ok);
  EXPECT_FALSE(ok);
  EXPECT_THROW(opt_set.validate_values_candidate(), exception);
  // Nor does a subsequent parse of a source that does not mention the invalid option.
  ASSERT_TRUE(parse(&opt_set, "name=whatever\n"));
  opt_set.validate_values_candidate(&ok);
  EXPECT_FALSE(ok);
  opt_set.reject_candidate();
  EXPECT_EQ(opt_set.values().m_count, 5);
}

TEST(Option_set_test, Validate_values)
{
  Test_logger logger;
  const Opt_set opt_set{&logger, "validate", &declare_opts};

  // Defaults are valid.
  bool ok = false;
  opt_set.validate_values(&ok);
  EXPECT_TRUE(ok);
  EXPECT_NO_THROW(opt_set.validate_values());

  // Each of these makes exactly one option invalid.
  vector<Opts> invalids(6);
  invalids[0].m_count = -1;
  invalids[1].m_name.clear();
  invalids[2].m_timeout = Fine_duration::zero();
  invalids[3].m_list = { 1, 2, 3, 4 };
  invalids[4].m_slots[1].m_depth = 0;
  invalids[5].m_legacy = 0;

  for (size_t idx = 0; idx != invalids.size(); ++idx)
  {
    SCOPED_TRACE(idx);
    const auto& invalid = invalids[idx];

    // Non-static overload: uses the validators given to the Option_set.
    ok = true;
    opt_set.validate_values(invalid, &ok);
    EXPECT_FALSE(ok);
    EXPECT_THROW(opt_set.validate_values(invalid), exception);

    // Static overload: no Option_set needed.
    ok = true;
    Opt_set::validate_values(&logger, invalid, &declare_opts, &ok);
    EXPECT_FALSE(ok);
    EXPECT_THROW(Opt_set::validate_values(&logger, invalid, &declare_opts), exception);
  }

  ok = false;
  Opt_set::validate_values(&logger, Opts{}, &declare_opts, &ok);
  EXPECT_TRUE(ok);

#ifndef NDEBUG
  // There is no candidate to validate in CANONICAL state.
  EXPECT_DEATH(opt_set.validate_values_candidate(), "CANONICAL state");
#endif
}

TEST(Option_set_test, Output)
{
  Test_logger logger;
  Opt_set opt_set{&logger, "the-nickname", &declare_opts};

  {
    const auto os_str = util::ostream_op_string(opt_set);
    EXPECT_TRUE(contains(os_str, "the-nickname")) << "Output: [" << os_str << "].";
  }

  // Help: every option name, its description, and its default.
  ostringstream help_os;
  opt_set.help_to_ostream(help_os);
  const string help = help_os.str();
  for (const auto& name : opt_set.option_names())
  {
    EXPECT_TRUE(contains(help, "--" + name)) << "Option [" << name << "] missing from help: [\n" << help << "].";
  }
  EXPECT_TRUE(contains(help, "A count in [0, 100].")) << "Help: [\n" << help << "].";
  EXPECT_TRUE(contains(help, "(=10)")) << "Help: [\n" << help << "].";
  EXPECT_TRUE(contains(help, "(=dflt)")) << "Help: [\n" << help << "].";
  // The duration default is printed in convenient units, not as a nanosecond count.
  EXPECT_FALSE(contains(help, "5000000000")) << "Help: [\n" << help << "].";

  // Current values: by default the canonical ones; each shown along with its default.
  ASSERT_TRUE(parse(&opt_set, "count=42\nname=current\n"));
  opt_set.canonicalize_candidate();
  ostringstream values_os;
  opt_set.values_to_ostream(values_os);
  const string values = values_os.str();
  for (const auto& name : opt_set.option_names())
  {
    EXPECT_TRUE(contains(values, "--" + name)) << "Option [" << name << "] missing from: [\n" << values << "].";
  }
  EXPECT_TRUE(contains(values, "(42) <==current")) << "Values: [\n" << values << "].";
  EXPECT_TRUE(contains(values, "(=10)")) << "Values: [\n" << values << "].";
  EXPECT_TRUE(contains(values, "(current) <==current")) << "Values: [\n" << values << "].";
  EXPECT_TRUE(contains(values, "A count in [0, 100].")) << "Values: [\n" << values << "].";

  // An arbitrary Value_set can be printed instead.
  Opts other;
  other.m_count = 99;
  ostringstream other_os;
  opt_set.values_to_ostream(other_os, &other);
  EXPECT_TRUE(contains(other_os.str(), "(99) <==current")) << "Values: [\n" << other_os.str() << "].";
  EXPECT_FALSE(contains(other_os.str(), "(42) <==current")) << "Values: [\n" << other_os.str() << "].";

  // These just must not blow up.
  opt_set.log_help("test help");
  opt_set.log_values("test values");
  opt_set.log_values("test other values", &other);
}

TEST(Option_set_test, Value_to_ostream)
{
  // Generic: just `ostream<<`.
  EXPECT_EQ(value_str(42), "42");
  EXPECT_EQ(value_str(string{"abc def"}), "abc def");

  /* Subtlety: durations are printed in the coarsest units that lose no precision (not, e.g., always in nanoseconds,
   * which is how a Fine_duration is stored); zero is printed in seconds.  The unit may be printed in long or short
   * form, so check the count and the start of the unit only. */
  const auto dur_str = [](Fine_duration dur) -> string { return value_str(dur); };
  EXPECT_EQ(dur_str(Fine_duration::zero()).rfind("0 s", 0), 0u) << "Got [" << dur_str(Fine_duration::zero()) << "].";
  EXPECT_EQ(dur_str(nanoseconds{1500}).rfind("1500 n", 0), 0u);
  EXPECT_EQ(dur_str(microseconds{2}).rfind("2 ", 0), 0u);
  EXPECT_FALSE(contains(dur_str(microseconds{2}), "2000"));
  EXPECT_EQ(dur_str(milliseconds{250}).rfind("250 m", 0), 0u);
  EXPECT_EQ(dur_str(seconds{9}).rfind("9 s", 0), 0u);
  EXPECT_EQ(dur_str(seconds{90}).rfind("90 s", 0), 0u);
  EXPECT_EQ(dur_str(seconds{60}).rfind("1 min", 0), 0u);
  EXPECT_EQ(dur_str(seconds{7200}).rfind("2 h", 0), 0u);
  EXPECT_EQ(dur_str(seconds{5400}).rfind("90 min", 0), 0u);
  // Negative durations get the same treatment.
  EXPECT_EQ(dur_str(-seconds{90}).rfind("-90 s", 0), 0u) << "Got [" << dur_str(-seconds{90}) << "].";
  EXPECT_EQ(dur_str(-seconds{60}).rfind("-1 min", 0), 0u) << "Got [" << dur_str(-seconds{60}) << "].";
  EXPECT_EQ(dur_str(-milliseconds{1}).rfind("-1 m", 0), 0u) << "Got [" << dur_str(-milliseconds{1}) << "].";
  // A coarser `duration` type works too.
  EXPECT_EQ(value_str(milliseconds{3000}).rfind("3 s", 0), 0u);

  // Lists.
  EXPECT_EQ(value_str(vector<int>{}), "");
  EXPECT_EQ(value_str(vector<int>{ 7 }), "[7]");
  EXPECT_EQ(value_str(vector<int>{ 1, 2, 3 }), "[1] [2] [3]");
  // Subtlety: each element is itself printed via value_to_ostream(); so a list of durations gets convenient units.
  {
    const string list_str = value_str(vector<Fine_duration>{ seconds{5}, milliseconds{250} });
    EXPECT_EQ(list_str.rfind("[5 s", 0), 0u) << "Got [" << list_str << "].";
    EXPECT_TRUE(contains(list_str, "] [250 m")) << "Got [" << list_str << "].";
    EXPECT_FALSE(contains(list_str, "000")) << "Got [" << list_str << "].";
  }
}

} // namespace flow::cfg::test
