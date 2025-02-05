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

#include "flow/test/test_common_util.hpp"
#include "flow/util/string_ostream.hpp"
#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <string>
#include <regex>
#include <fstream>

using std::string;
using std::stringstream;
using std::ostream;
using std::vector;

namespace flow::test
{

string get_test_suite_name()
{
  return ::testing::UnitTest::GetInstance()->current_test_info()->test_suite_name();
}

/**
 * Captures (log) output directed to a stream during function execution.
 *
 * @param ss The location to store the directed output.
 * @param func The function to execute.
 * @param os The stream to capture output from.
 */
static void collect_output(ostream& os_dest, const std::function<void()>& func, ostream& os_source)
{
  // Save original buffer
  std::streambuf* original_buffer = os_source.rdbuf();
  // Redirect to our buffer
  os_source.rdbuf(os_dest.rdbuf());
  // Execute function
  func();
  // Flush buffer
  os_source.flush();
  // Restore buffer
  os_source.rdbuf(original_buffer);
}

static void collect_output(util::String_ostream& ss,
                           const std::function<void()>& func,
                           ostream& os,
                           bool output_buffer)
{
  collect_output(ss.os(), func, os);
  if (output_buffer)
  {
    // Output results
    os << ss.str();
    os.flush();
  }
}

bool check_output(const string& output, const vector<string>& regex_matches)
{
  bool result = true;
  for (auto const& iter : regex_matches)
  {
    std::regex cur_regex(iter);
    if (!std::regex_search(output, cur_regex))
    {
      result = false;
    }
  }

  return result;
}

bool check_output(const std::function<void()>& func,
                  ostream& os,
                  const string& regex_match,
                  bool output_buffer)
{
  vector<string> v = {regex_match};
  return check_output(func, os, v, output_buffer);
}

bool check_output(const std::function<void()>& func,
                  ostream& os,
                  const vector<string>& regex_matches,
                  bool output_buffer)
{
  util::String_ostream ss;

  collect_output(ss, func, os, output_buffer);
  return check_output(ss.str(), regex_matches);
}

string collect_output(const std::function<void()>& func, ostream& os, bool output_buffer)
{
  util::String_ostream ss;
  collect_output(ss, func, os, output_buffer);
  return ss.str();
}

size_t get_rss()
{
  size_t rss_pages = 0ULL;

  string ignore;
  // See "man 5 proc"
  std::ifstream fstream("/proc/self/stat", std::ios_base::in);
  fstream >>
    ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >>
    ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >>
    ignore >> ignore >> ignore >> rss_pages;
  fstream.close();

  return (rss_pages * sysconf(_SC_PAGE_SIZE));
}

} // namespace flow::test
