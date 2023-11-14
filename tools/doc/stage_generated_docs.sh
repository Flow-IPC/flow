#!/bin/sh

# Flow
# Copyright 2023 Akamai Technologies, Inc.
#
# Licensed under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in
# compliance with the License.  You may obtain a copy
# of the License at
#
#   https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in
# writing, software distributed under the License is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing
# permissions and limitations under the License.

# Author note: I (ygoldfel) personally detest shell scripts; would much rather use a real
# programming language; maybe Perl or Python.  However to avoid adding further dependencies this
# very straightforward script is a shell script.

# Exit immediately if any command has a non-zero exit status.
set -e

# Ensure(ish) the current directory is `flow_doc`.
if [ ! -f index.html ] || [ ! -f ../flow_doc/index.html ]; then
  echo "Please run this from the relative directory doc/flow_doc/ off the project root." >&2
  exit 1
fi

# Check for the mandatory argument.
if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <the build dir where you invoked CMake, thus generating the build system there>" >&2
  exit 1
fi

BUILD_DIR="$1"

# Ensure specified directories exist, meaning the docs have been generated using `make`.
if [ ! -d "${BUILD_DIR}/html_flow_doc_public" ] || [ ! -d "${BUILD_DIR}/html_flow_doc_full" ]; then
  echo "Please run 'make flow_doc_public flow_doc_full' from [${BUILD_DIR}] before invoking me." >&2
  exit 1
fi

# Prepare the target directories.
rm -rf ./generated/html_public
rm -rf ./generated/html_full
mkdir -p ./generated

# Move/rename directories.
mv -v "${BUILD_DIR}/html_flow_doc_public" ./generated/html_public
mv -v "${BUILD_DIR}/html_flow_doc_full" ./generated/html_full

cd ..
rm -f flow_doc.tgz
tar czf flow_doc.tgz flow_doc

echo 'Peruse the docs by opening ./index.html with browser.'
echo 'If satisfied you may check the resulting ../flow_doc.tgz into source control.'
