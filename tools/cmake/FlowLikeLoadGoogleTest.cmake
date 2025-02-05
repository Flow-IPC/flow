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

# include() this in a given test program to set-up GoogleTest, so it's available for linking and
# `#include`ing.  See notes at the end of this file regarding how to proceed in more detail.

# We'll need Google's unit-test framework, so let's find that.
block()
  set(lib_name "gtest")
  # Search for static lib specifically.  TODO: Is this needed really?  Being paranoid here?
  if(UNIX)
    set(lib_name "lib${lib_name}.a")
  else()
    message(FATAL_ERROR "Unsupported OS; please modify script to search for static lib Windows-style.")
  endif()
  find_library(GTEST_LIB NAMES ${lib_name})
  if(NOT GTEST_LIB)
    message(FATAL_ERROR
              "Could not find Google unit-test gtest library ([${lib_name}]) location via CMake uber-searcher "
              "find_library().  Do not fret: Please build gtest if not yet done and try again; "
              "if it is still not found then please supply the full path to the library file (including "
              "the file itself) as the CMake knob (cache setting) GTEST_LIB.")
  endif()
  message(VERBOSE "CMake uber-searcher find_library() located Google unit-test gtest lib (see below); "
                    "though you can override this via cache setting.")

  # Search for its include-root.
  find_path(GTEST_INCLUDE_DIR NAMES gtest/gtest.h)
  if(NOT GTEST_INCLUDE_DIR)
    message(FATAL_ERROR
              "Could not find Google unit-test gtest/*.h include-path via CMake uber-searcher "
              "find_path(gtest/gtest.h).  Do not fret: Please build gtest if not yet done and try again; "
              "if it is still not found then please supply the full path to gtest/gtest.h (excluding the "
              "directory and file themselves) as the CMake knob (cache setting) GTEST_INCLUDE_DIR.")
  endif()

  # Note: These days Google's unit-test framework, at least when built with CMake, exports
  # the stuff needed to just do find_package(GTest).  We could use that above, as we do for Cap'n Proto
  # and others.  However I (ygoldfel) have it on good authority that people build the notoriously easy-to-build
  # GTest in a myriad ways, and this find_package() support may not be produced.  So we go with the
  # naughty more-manual way above.
endblock()

message(STATUS "Google unit-test gtest lib location: [${GTEST_LIB}].")

# At this point one can declare a GoogleTest-using program.  Some general info on how we actually do that
# in Flow-like scenarios:

# Unit tests are built on Google's unit-test framework.  The conventions we use are roughly as follows.
#   - The unit test files shall be interspersed together with the code of the various libraries lib*,
#     albeit always in directories named test/.
#     - In addition the needed program driver(s) (i.e., guys with main()), in the root namespace, shall *not* be
#       so interspersed but rather will live separately under <your test program dir> (here, where we are include()d).
#   - Let `class X` in namespace N (so N::X) be located in D/X.hpp (at least), where D is a directory usually
#     corresponding to namespace N (more or less).
#   - Its unit test might be then called X_test and shall reside in namespace N::test::X_test.
#     Its source code shall be in D/test/X_test.cpp.
#   - Any supporting code outside of that file can be anywhere under D/test, in files named `test_*.hpp` and
#     `test_*.cpp`; a convention might be to start with `test_X.?pp`, though even more support files may also
#     be present.
#     - For support files like this potentially applicable to ~all unit tests, they shall live in... I'll explain
#       by example.  flow/ is a dependency of everything, so things useful for Flow and/or non-Flow (Flow-dependent)
#       projects (e.g., Flow-IPC) shall live in flow::test and therefore its flow/test/.  Things useful for
#       a particular project only (e.g., Flow-IPC) shall live in <that guy>::test and therefore <that guy>/test;
#       for example ipc::test + ipc/test.  It's just a question of, is this utility for everyone (Flow + possibly
#       others) or just that project.
#
# The relevant translation units (.cpp, .capnp -> .c++) shall be listed directly below the include() that include()d
# us, in normal CMake fashion, under the relevant unit-test executables.
# We do not make an intermediate libtest* or anything like that;
# the relevant object files are compiled directly from their relevant source files.  (This is pretty common
# when using the Google unit-test framework.)
#
# A given GoogleTest-using executable (e.g., libipc_unit_test in Flow-IPC; libflow_unit_test in Flow),
# is a compendium of unit tests to execute.  We leave the details of declaring an executable to you
# (@todo try to set up code-reuse; for now it is reasonably short enough to be done each time manually but...).
# GTEST_INCLUDE_DIR for target_include_directories() and
# GTEST_LIB for target_link_libraries() are available (computed above for you).
