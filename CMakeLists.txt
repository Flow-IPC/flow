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

cmake_minimum_required(VERSION 3.26.3) # See FlowLikeCodeGenerate.cmake for details.

# See this guy; it'll explain inside.  It mandates the following procedure and documents details.
set(PROJECT_ROOT_CMAKE_SCRIPT "tools/cmake/FlowLikeCodeGenerate.cmake")

set(PROJ "flow")

message(CHECK_START "(Project [${PROJ}] root CMake script executing.)")
list(APPEND CMAKE_MESSAGE_INDENT "- ")

set(PROJ_CAMEL "Flow")
# TODO: Get it from nearest `VERSION` file perhaps.
set(PROJ_VERSION "1.0.0")
set(PROJ_HUMAN "Flow")

set(OS_SUPPORT_MSG "Only Linux is supported for now.  For Flow: macOS/BSD support might not "
                      "require any coding, just extra testing; Windows might require a little bit of coding too.")
# Subtext: The main reason Flow is being open-sourced (delightful though it is...!) is that the Flow-IPC
# project requires it; but getting that to work in macOS/BSD and Windows would require more actual coding due to
# certain nonportable techniques used in a few low-level spots.  This information is technically irrelevant
# to Flow (the present project), but we include it here for your convenience, as Flow and Flow-IPC are often
# considered together as of this writing.  (Nevertheless Flow is absolutely usable and useful by itself.)

include(${PROJECT_ROOT_CMAKE_SCRIPT})

# Last but not least... export that same guy, so dependents can use it!  Natually dependents need *not* do the same;
# they should instead:
#   find_package(Flow CONFIG REQUIRED)
#   include("${Flow_DIR}/../../../share/flow/cmake/FlowLikeCodeGenerate.cmake")
message(STATUS "Install target: Exports FlowLikeCodeGenerate.cmake: utility for dependents' CMake scripts.")
install(FILES ${PROJECT_ROOT_CMAKE_SCRIPT} DESTINATION share/${PROJ}/cmake)

# Tip: Most likely you are next interested in ./src/CMakeLists.txt and then perhaps ./test/{basic|suite}/CMakeLists.txt.

# Documentation generation.

# See this guy; it'll explain inside.  It mandates the following procedure and documents details.
set(DOC_GEN_CMAKE_SCRIPT "tools/cmake/FlowLikeDocGenerate.cmake")

# As of this writing Flow has only reference documentation within the code (it is quite extensive though!).
# TODO: A Guided Manual (as can be seen in, e.g., Flow-IPC) would be nice.  In that case DOC_INPUT would
# probably contain an added subdir with the Doxygen-only *.dox.txt files containing the manual;
# and if it contains figures we'd add a set(DOC_IMAGE_PATH ...) here for the location of the image assets.
set(DOC_INPUT src/flow)
set(DOC_EXCLUDE_PATTERNS "")

include(${DOC_GEN_CMAKE_SCRIPT})

# Last but not least... export that same guy, so dependents can use it!  Natually dependents need *not* do the same;
# they should instead:
#   include("${Flow_DIR}/../../../share/flow/cmake/FlowLikeDocGenerate.cmake")
message(STATUS "Install target: Exports FlowLikeDocGenerate.cmake: utility for dependents' CMake scripts.")
install(FILES ${DOC_GEN_CMAKE_SCRIPT} DESTINATION share/${PROJ}/cmake)

list(POP_BACK CMAKE_MESSAGE_INDENT)
message(CHECK_PASS "(Done, success.)")
