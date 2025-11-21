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
    ${PROJECT_SOURCE_DIR}/src/flow/log/test/log_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/util/test/blob_test.cpp
    ${PROJECT_SOURCE_DIR}/src/flow/util/test/linked_hash_test.cpp
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

# In this particular test, in Release and MinSizeRel configs, some versions of gcc issue buggy front-end-driven
# mem-op-optimization-related warnings.  Similar problems were observed in a few other places, including as of
# this writing the production code basic_blob.hpp; but in this case attempts to defeat them (in this case
# the calls to std::fill_n) with pragmas, or direct std::memcpy, or even manual looping all failed.  Since it is
# mere test code, it's fine to defeat it via ignoring specific warnings.  (Note: This class of gcc bugs appears to
# have been eliminated by gcc-15, based on gcc Bugzilla tickets, but we don't know.  As of this writing CI pipeline
# does, gcc-wise, versions 9, 10, 11, 13.  For now we are conservative, in the sense that we do this for the
# versions where we know there are these specific issues.  They might exist in versions the pipeline does not test
# as of this writing.  TODO: Possibly either just say screw it and do it for all gccs, or find the ~exact versions.
# Or let it fail, if pipeline/people's non-pipeline builds expose more versions with the problem, and add here at
# that point.)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  block()
    set(ver ${CMAKE_CXX_COMPILER_VERSION})
    set(evil_file ${PROJECT_SOURCE_DIR}/src/flow/util/test/blob_test.cpp)
    if((ver VERSION_GREATER_EQUAL 11) AND (ver VERSION_LESS 12))
      # stringop-overflow in form A observed in latest gcc-11, not 13, 9, 10.
      set(evil_opts "-Wno-stringop-overflow")
    elseif((ver VERSION_GREATER_EQUAL 13) AND (ver VERSION_LESS 14))
      # stringop-overflow in form B observed in gcc-13, not 11, 9, 10.
      # array-bounds, same.
      set(evil_opts "-Wno-array-bounds;-Wno-stringop-overflow")
    endif()
    if(DEFINED evil_opts)
      set_source_files_properties(${evil_file} PROPERTIES COMPILE_OPTIONS "${evil_opts}")
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
