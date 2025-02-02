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

#include "flow/common.hpp"
#include <boost/filesystem.hpp>

namespace flow::test
{

/**
 * Returns the status of a file.
 *
 * @param file_path The path to the file to check.
 * @param ec Any error that occurred.
 *
 * @return See above; the error code must be checked before relying on the return value.
 */
boost::filesystem::file_status get_file_status(const boost::filesystem::path& file_path, Error_code& ec);
/**
 * Returns whether a file exists.
 *
 * @param file_path The path to the file to check.
 * @param ec Any error that occurred.
 *
 * @return See above; a positive value is also an indicator that there was no error.
 */
bool does_file_exist(const boost::filesystem::path& file_path, Error_code& ec);
bool does_dir_exist(const boost::filesystem::path& file_path, Error_code& ec);
bool create_directory_if_not_exists(const boost::filesystem::path& dir_path, Error_code& ec);

} // namespace flow::test
