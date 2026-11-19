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

#pragma once

#include "flow/cfg/cfg_fwd.hpp"
#include "flow/common.hpp"
#include <boost/filesystem.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <string>

// Helpers shared by the flow::cfg unit tests.

namespace flow::cfg::test
{

// A config file with the given contents, in the temp directory, removed on destruction.
class Cfg_file
{
public:
  explicit Cfg_file(const std::string& contents) :
    m_path(fs::temp_directory_path() / fs::unique_path("flow_cfg_test-%%%%-%%%%-%%%%.cfg"))
  {
    std::ofstream ofs{m_path.string()};
    ofs << contents;
    ofs.close();
    EXPECT_TRUE(ofs.good()) << "Could not write temp config file [" << m_path << "].";
  }

  ~Cfg_file()
  {
    Error_code sink;
    fs::remove(m_path, sink);
  }

  const fs::path m_path;
};

// Whether `haystack` contains `needle`.
inline bool contains(const std::string& haystack, const std::string& needle)
{
  return haystack.find(needle) != std::string::npos;
}

} // namespace flow::cfg::test
