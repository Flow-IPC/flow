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

include("${PROJECT_SOURCE_DIR}/tools/cmake/FlowLikeLoadGoogleTest.cmake")
# See instructions in that file re. mechanics and conventions of GoogleTest-using programs;
# we shall declare a few now.  GTEST_LIB and GTEST_INCLUDE_DIR are set for us.

# Test program: unit_test.
set(name libflow_unit_test.exec)
set(SRCS
    ${PROJECT_SOURCE_DIR}/src/flow/test/test_common_util.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/test/test_file_util.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/log/detail/test/component_cfg_test.cpp
    test_main.cpp)

add_executable(${name} ${SRCS})

# Test header files are *not* exported for user to include; rather they exist only to be cross-included by
# each other (namely the support files are).  For that to work add the source dir(s) into the include-path.
target_include_directories(${name} PRIVATE
                           ${GTEST_INCLUDE_DIR}
                           ${PROJECT_SOURCE_DIR}/src
                           # So that generated-above .capnp.h are found by SRCS' .cpps' `#include`s.:
                           ${CMAKE_CURRENT_BINARY_DIR})

# Do stuff we've resolved to do on all our targets.
common_set_target_properties(${name})

# Link good ol' flow; and gtest.
target_link_libraries(${name} PRIVATE flow ${GTEST_LIB})

# Might as well export if they `make install` or equivalent.
install(TARGETS ${name}
        RUNTIME DESTINATION bin)
