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
    ${PROJECT_SOURCE_DIR}/src/flow/cfg/test/cfg_manager_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/cfg/test/option_set_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/log/detail/test/component_cfg_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/log/test/log_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/net_flow/test/backlog_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/net_flow/test/data_in_syn_rcvd_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/net_flow/test/rng_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/util/test/blob_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/util/test/default_init_allocator_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/util/test/linked_hash_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/util/stat/test/stat_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/util/test/thread_lcl_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/util/test/util_test.cpp
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

# In this particular test, in optimized configs (which ones depends on the gcc version), some versions of gcc issue
# buggy (or at any rate false) mem-op-optimization-related warnings.  Similar problems were observed in a few other
# places, including as of this writing the production code basic_blob.hpp; but in this case attempts to defeat
# them (in this case the calls to std::fill_n()) with pragmas, or direct std::memcpy(), or even manual looping all
# failed.  Since it is mere test code, it's fine to defeat it via ignoring specific warnings.
#
# So far we've seen (1) versions where -Wstringop-overflow manifests (e.g.: `memset` "writing 10 bytes into a
# region of size 0") and (2) ones where that one manifests plus also -Warray-bounds manifests (e.g.: `memset`
# "offset [0, 9] is out of the bounds [0, 0]").  Which one happens where depends on the gcc version, and over
# time our testing envs (used to empirically classify this) change (meaning, we might add a newer gcc and drop
# an older one).  So maintaining (and describing) the exact classification of what warning manifests where for
# what gcc version = too much effort for too little gain.  Thus, as (again) this is mere test code, we keep it
# simple by doing this:
#   - disable *all* known warnings
#   - for the range [ver_min_with_prob, ver_that_may_fix_it), which is the range where we've observed
#     *any* known warnings.
#
# TODO: When, in the future, we ~inevitably try a higher-version (>=ver_that_may_fix_it) gcc, it'll either manifest
# 1+ of these warnings and fail the build (because warnings=errors for us), or it won't.  In the former case, bump
# up ver_that_may_fix_it accordingly.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  block()
    set(ver_min_with_prob 11) # Min gcc version such that we know it has this class of problem in $evil_file.
    set(ver_that_may_fix_it 16) # 1 + max gcc version such that we know it has this class of problem in $evil_file.
    set(ver ${CMAKE_CXX_COMPILER_VERSION})
    set(evil_file ${PROJECT_SOURCE_DIR}/src/flow/util/test/blob_test.cpp)
    if((ver VERSION_GREATER_EQUAL ${ver_min_with_prob}) AND (ver VERSION_LESS ${ver_that_may_fix_it}))
      set_source_files_properties(${evil_file} PROPERTIES COMPILE_OPTIONS
                                  "-Wno-array-bounds;-Wno-stringop-overflow")
    endif()
  endblock()
endif()

# Link good ol' flow; and gtest.
target_link_libraries(${name} PRIVATE flow ${GTEST_LIB})
# At least blob_test.cpp requires boost.interprocess SHM ops which require this lib.  (Flow itself doesn't as of
# this writing.)
if(LINUX)
  target_link_libraries(${name} PRIVATE rt)
endif()

# Might as well export if they `make install` or equivalent.
install(TARGETS ${name}
        RUNTIME DESTINATION bin)
