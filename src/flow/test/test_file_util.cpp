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

#include "flow/test/test_file_util.hpp"

namespace fs = boost::filesystem;
using Fs_path = fs::path;

namespace flow::test
{

fs::file_status get_file_status(const Fs_path& file_path, Error_code& ec)
{
  fs::file_status file_status = fs::status(file_path, ec);
  if (file_status.type() == fs::file_not_found)
  {
    // Clear out erroneous error code
    ec.clear();
    return file_status;
  }

  return file_status;
}

bool does_file_exist(const Fs_path& file_path, Error_code& ec)
{
  fs::file_status file_status = get_file_status(file_path, ec);
  if (ec)
  {
    return false;
  }

  return (file_status.type() != fs::file_not_found);
}

bool does_dir_exist(const Fs_path& file_path, Error_code& ec)
{
  fs::file_status file_status = get_file_status(file_path, ec);
  if (ec)
  {
    return false;
  }

  return (file_status.type() == fs::directory_file);
}

bool create_directory_if_not_exists(const Fs_path& dir_path, Error_code& ec)
{
  if (does_dir_exist(dir_path, ec))
  {
    // Directory already exists
    return true;
  }

  if (does_file_exist(dir_path, ec))
  {
    // File exists, but it is not a directory
    return false;
  }

  if (ec)
  {
    // Unknown error
    return false;
  }

  return fs::create_directory(dir_path, ec);
}

} // namespace flow::test
