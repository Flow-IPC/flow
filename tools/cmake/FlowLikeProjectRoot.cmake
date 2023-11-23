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

# Essentially this *is* our root CMake script (CMakeLists.txt); but because at least one dependent project/repo
# (ipc_core: the core part of Flow-IPC, useful in its own right but also dependency of the others as of this
# writing) follows the same build policies/shares similar DNA, we make it available for such dependencies to
# use.
#
# Usage: Assuming you're a dependent project: In your root CMakeLists.txt, at the top:
#
#   # We've only tested with this.  Possibly (probably?) lots of earlier versions would work; but they're not tested,
#   # so let's be conservative.
#   cmake_minimum_required(VERSION 3.26.3)
#
#   set(PROJ "(...see below...)")
#   set(PROJ_CAMEL "(...see below...)")
#   set(PROJ_VERSION "(...see below...)")
#   set(PROJ_HUMAN "(...see below...)")
#   set(OS_SUPPORT_MSG "(...see below...)")
#
#   project(${PROJ_CAMEL} VERSION ${PROJ_VERSION} DESCRIPTION ${PROJ_HUMAN} LANGUAGES CXX)
#
#   find_package(Flow CONFIG REQUIRED)
#   include("${Flow_DIR}/../../../share/flow/cmake/FlowLikeProjectRoot.cmake")
#
# The variables:
#   PROJ: snake_case project name (e.g.: flow, ipc_core, ipc_transport_struct).  Various things will be keyed off
#     it including the main exported library lib${PROJ}.a (or equivalent).
#   PROJ_CAMEL: CamelCase equivalent of ${PROJ} (e.g., IpcTransportStruct).  Generally we stay away from CamelCase
#     in our projects; but this style is conventionally used for certain CMake names, so we make the exception there
#     (nowhere inside the source code itself though, as of this writing).
#   PROJ_VERSION: Version of $PROJ (at least 2 components, a.b).
#   PROJ_HUMAN: Human-friendly brief name; e.g.: "Flow" or "Flow-IPC".  (It can have spaces too.)
#   OS_SUPPORT_MSG: Message to print if the detected OS environment is fatally unsuitable for building this.
#     If OS requirements between us and dependencies using this file diverge, we will need to be more configurable
#     than this.  For now see the OS tests performed below for the common requirement policy.
#     (Same for compiler and other requirements.  In general, though, we intend to share as much policy DNA as
#     possible between Flow and dependents (like Flow-IPC projects) that would use the present .cmake helper.)
#
# Lastly, if you'd like to set up the build environment but not actually any configure any souces to build
# (src/, test/), then set `set(FLOW_LIKE_PROJECT_ROOT_ENV_ONLY TRUE)` just before include()ing us.

# Without further ado... let's go.  Note: it is not allowed for us to do cmake_minimum_required() or project();
# CMake requires that they literally do so themselves.  Hence they must copy/paste the CMake version we listed above;
# and they must make the project() call manually too, code reuse be damned (we do our best).

include(CheckIPOSupported)
include(CheckCXXCompilerFlag)

# Our own config knobs.

option(CFG_SKIP_CODE_GEN
       "If OFF: Build targets (to build actual product + possibly demos/tests) shall be created; thus need Boost, etc."
       OFF)
option(CFG_NO_LTO
       "If OFF: LTO/IPO will be enabled (if possible) for optimized builds; if ON: it will always be disabled."
       OFF)
option(CFG_NO_LTO_FOR_TEST_EXECS
       "Overrides CFG_NO_LTO=OFF (disables LTO) for test/demo executables while internally enabling for libraries."
       OFF)
option(CFG_SKIP_BASIC_TESTS
       "If ON: skips building/installing sanity-checking test program(s) in test/basic/ (lowers build time)."
       OFF)
option(CFG_ENABLE_TEST_SUITE
       "If ON: builds/installs serious test(s) in test/suite/ (increases build time).")
# Note: FlowLikeDocGenerate.cmake will check this -- in the same way we check CFG_SKIP_CODE_GEN below, before going
# on to actually generate targets for building actual code (libs, executables).
# We do not include FlowLikeDocGenerate.cmake; guy that includes us potentially should do so, depending on how
# it wants to structure doc generation (and whether it is even applicable).
option(CFG_ENABLE_DOC_GEN
       "If and only if ON: Doc generation targets shall be created; thus need Doxygen/Graphviz."
       OFF)
# Quick discussion regarding which projects are affected by the above (and any other CFG_*):
# As an example: Suppose project `flow` uses us (FlowLikeProjectRoot.cmake), and dependent `ipc_core`
# (while being built as part of the same meta-project `ipc`) also uses
# us (FlowLikeProjectRoot.cmake).  The CFG_* settings (saved in CMakeCache.txt for each relevant project)
# will affect *both* `flow` and `ipc_core`.  So it would be impossible, as of this writing, to (e.g.) have
# CFG_SKIP_BASIC_TESTS be ON for `flow` but OFF for `ipc_core`.  There is only one `cmake` or `ccmake`
# being invoked, and that setting affects each instance of FlowLikeProjectRoot.cmake similarly.
# We have not added any namespace-type-thing into the name CFG_SKIP_BASIC_TESTS (or any other CFG_*).
# So all sub-builds sharing our DNA are affected in the same way.
#
# However, if `flow` is built/installed as a separate project, and then `ipc_core` is built/installed
# as a separate project (with `flow` consumed from its installed location), then CFG_SKIP_BASIC_TESTS can
# be different for each, as there's a `cmake` or `ccmake` separate invoked for each.
#
# TODO: *Consider* adding the per-project namespace to these settings.  For simplicity it's good the way it is;
# but it is conceivable a more complex setup may be desired at some point.

message("Welcome to the build configuration script for [${PROJ}] (a/k/a [${PROJ_CAMEL}] or "
        "for humans [${PROJ_HUMAN}]), vesion [${PROJ_VERSION}].  We use CMake for this task.")
message("We shall generate the build script(s) (e.g., in *nix: Makefile(s)) one can invoke to build/install us; "
        "and/or generate our documentation, if relevant.  (Build generation and doc generation can each be "
        "independently enabled/disabled using knobs CFG_ENABLE_CODE_GEN and CFG_SKIP_CODE_GEN.)  "
        "You should run `cmake` or `ccmake` on me outside any actual source directory.  "
        "Once you've set all knobs how you want them, we will generate at your command.  Then you can actually "
        "build/install (e.g., in *nix: `make -j32 && make install` assuming a 32-core machine).  You can also "
        "have `cmake` perform the build itself.  Please, in general, see CMake docs.")
message("A couple commonly used CMake settings: CMAKE_INSTALL_PREFIX is [${CMAKE_INSTALL_PREFIX}]; "
        "CMAKE_PREFIX_PATH is [${CMAKE_PREFIX_PATH}].  Installed items such as Boost and Doxygen might be "
        "found in either place (among others) or a system location.  One typically sets PREFIX_PATH, if "
        "INSTALL_PATH is not sufficient.  Build-system's install step shall place our results to INSTALL_PATH, "
        "and locally installed inputs for the build are often found there too.")

# Environment checks.  Side effect: some "local" variables are set for later use.

# OS:
if(NOT LINUX)
  message(FATAL_ERROR ${OS_SUPPORT_MSG})
endif()

if(CFG_SKIP_CODE_GEN)
  message("Build generation requested by (presumably) project root CMake script; skipped via CFG_SKIP_CODE_GEN=ON.")
  return()
endif()

message("Build generation requested by (presumably) project root CMake script; "
        "not skipped via CFG_SKIP_CODE_GEN=OFF.")

# From now on Linux assumed (which implies *nix obv).  Though, where it's not too much trouble, lots of things
# will work in other OS (even Windows/MSVC) when/if that restriction is removed.  I.e., we try not to not-support
# other OS, all else being equal.

# Compiler:

message("Detected compiler [ID/VERSION]: ${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}.")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(GCC_MIN_VER 8)
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS GCC_MIN_VER)
    message(WARNING "gcc version should be at least ${GCC_MIN_VER}.  Letting build continue but beware: untested.")
  endif()

  # This must be set for each allowed compiler.  Our standard of warnings is:
  #   - The common ones;
  #   - and some more on top of that.
  # Obviously what that is is subjective; but for gcc that's roughly the following.
  set(WARN_FLAGS -Wall -Wextra)

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # Same deal as for gcc.
  # TODO: Consider a slight deep-dive into what -Wall -Wextra means for gcc versus clang.  If they do mean
  # roughly the same thing, then remove this to-do; otherwise tweak them to be functionally similar or identical.
  set(WARN_FLAGS -Wall -Wextra)

else()
  # TODO: Maybe it's too draconian at this stage.  CMake is built around portability; we are using it now, as opposed
  # the much more rigid pre-open-source environment.  Consider merely warning.
  message(FATAL_ERROR "Only gcc and clang are supported for now.  Will not let build continue, "
                      "though nearby TODO suggests merely warning here instead.")
endif()

if(NOT DEFINED WARN_FLAGS)
  message(FATAL_ERROR "Bug in our CMake script: Need to code the proper warning flags for the active compiler "
                      "(CMAKE_CXX_COMPILER_ID/VERSION [${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}]) and "
                      "load them into local variable WARN_FLAGS.  Reminder: only the warning level shall be, "
                      "not whether to treat warnings as error or other knobs.")
endif()

# Check if the compiler is capable of LTO (link-time optimization) a/k/a IPO (interprocedural optimization).
# This is important for us in particular, as we stylistically eschew explicitly inlining functions;
# so that even one-liner accessor bodies are in .cpp files (unless template or constexpr); max optimization
# (-O3 in gcc) will auto-inline, but this can only occur within a translation unit (.cpp, .c++) normally.
# LTO/IPO makes it occur across translation units too.
if(CFG_NO_LTO)
  set(LTO_ON FALSE)
  message("IPO/LTO disabled due to user-specified CFG_NO_LTO=ON.")
else()
  check_ipo_supported(RESULT LTO_ON OUTPUT output)

  if(LTO_ON)
      message("IPO/LTO is supported by compiler "
              "(CMAKE_CXX_COMPILER_ID/VERSION [${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}]).  "
              "Will enable it (for optimized builds only -- *Rel* build types).")
  else()
      message(WARNING "IPO/LTO is not supported: ${output}.")
  endif()

  unset(output)
endif()

message("Environment checks passed.  Configuring build environment.")

# Project proper.

# Set CMake global stuff.

# Compile our own source files in C++17 mode; and refuse to proceed, if the compiler does not support it.
# Note that any `#include`r will also need to follow this, as our headers are not shy about using C++17 features;
# but that's enforced by a check in common.hpp (at least); not by us here somehow.
set(CXX_STD 17)
set(CMAKE_CXX_STANDARD ${CXX_STD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
message("C++${CXX_STD} language and STL required.")

# When we do find_package(Threads) to link threading library this will cause it to
# prefer -pthread flag where applicable (as of this writing, just Linux, but perhaps any *nix ultimately).
set(THREADS_PREFER_PTHREAD_FLAG ON)

# Set our own global stuff (constants, functions).
# Note: Some such things (WARN_FLAGS as of this writing) have already been set while scanning environment above.

# Call this on each executable and library.  Doesn't seem to be a way to do it "globally" (plus some things in here
# differ depending on the target's specifics; like library versus executable).
function(common_set_target_properties name)
  # Treat warnings as errors.  Note ${WARN_FLAGS} is separate and must be applied too.
  message("Target [${name}]: Warnings shall be treated as errors.")
  set_target_properties(${name} PROPERTIES COMPILE_WARNING_AS_ERROR TRUE)

  # Show only a few errors per file before bailing out.
  set(MAX_ERRORS 3)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${name} PRIVATE "-fmax-errors=${MAX_ERRORS}")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(${name} PRIVATE "-ferror-limit=${MAX_ERRORS}")
  else()
    message(FATAL_ERROR "Target [${name}]: For this target wanted to limit # of compile errors per file but compiler "
                        "(CMAKE_CXX_COMPILER_ID/VERSION [${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}])"
                        "is unknown; not sure what to do.  Please adjust the script to cover this eventuality.")
  endif()

  message("Target [${name}]: Compiler will show at most [${MAX_ERRORS}] errors per file before bailing out.")

  # If LTO enabled+possible *and* not specifically and relevantly disabled (via option) for test executables...
  # ...then turn it on for this target, for all optimized build types (never for debug/unspecified build types).
  get_target_property(target_type ${name} TYPE)
  if(LTO_ON
       AND (NOT
              ((target_type STREQUAL "EXECUTABLE") AND CFG_NO_LTO_FOR_TEST_EXECS)))

    message("Target [${name}]: LTO enabled (for *Rel* build types only).")

    set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
    # Do not do that for Debug (LTO + debug = no good) or unspecified type.

    check_cxx_compiler_flag(-ffat-lto-objects FAT_LTO_OBJECTS_IS_SUPPORTED)

    # In case the consuming executable has LTO disabled generate object code that will support that as well;
    # in that case the LTO improvements will be limited to within the library -- which is most of the win anyway.
    if(FAT_LTO_OBJECTS_IS_SUPPORTED)
      message("Target [${name}]: LTO: instructing compiler to generate in fat-LTO format.")

      # Again, though, do it for *Rel* build types only.

      # Subtlety: INTERPROCEDURAL_OPTIMIZATION=TRUE causes CMake to add -fno-fat-lto-objects to flags (as seen
      # empirically); this will not make it not do that; but our adding it here will override their thing by appearing
      # later in the command line, so it is fine.
      set(additional_flags -ffat-lto-objects)
      target_compile_options(${name} PRIVATE
                             $<$<CONFIG:Release>:${additional_flags}>
                             $<$<CONFIG:RelWithDebInfo>:${additional_flags}>
                             $<$<CONFIG:MinSizeRel>:${additional_flags}>)
      # (if() would be fine for when we'll be generating Makefiles; but in the future if we target MSVC for example,
      # then it'd be wrong, as it's a multi-build-type system, where a single build script is created for all
      # possible build types (Debug, Release, etc.).  Apparently this <<>> stuff handles that nicely and works
      # for the simpler `make` situation equally well too.)
    else()
      message(WARNING "Target [${name}]:  "
                      "${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}] does not support fat LTO objects.")
    endif()
  endif()
endfunction()

if(FLOW_LIKE_PROJECT_ROOT_ENV_ONLY)
  return()
endif()

message("Environment configured.  Invoking further building in subdirs.")

# This generates and exports lib$PROJ.a and its headers; and the $PROJ library CMake target.
# (By the way I looked into it, and apparently unlike with certain other situations there is no need to prepend
# ${CMAKE_CURRENT_SOURCE_DIR}.)
add_subdirectory(src)

# This generates (and exports? if so why not) test/demo program(s).
if(CFG_SKIP_BASIC_TESTS)
  message("Per cache setting CFG_SKIP_BASIC_TESTS will skip building/installing any demo/test program(s) under "
          "test/basic/.  These are intended to be limited/simple programs that ensure the basic build/linkability "
          "of the library/headers in ${PROJ}; and a very limited sanity-check of a small subset of its capabilities.  "
          "By skipping these the build time will be lowered (possibly quite a bit); but if there are basic build/link "
          "issues you may discover them later in your own consumer build.")
else()
  message("Per cache setting CFG_SKIP_BASIC_TESTS=OFF will build/install any demo/test program(s) under test/basic/.  "
          "Keep in mind these are not a comprehensive unit test suite or integration test; but rather limited/simple "
          "programs that ensure the basic buildability/linkability "
          "of the library/headers in ${PROJ}; and a very limited sanity-check of a small subset of its capabilities.")
  add_subdirectory(test/basic)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/suite")
  if(CFG_ENABLE_TEST_SUITE)
    message("This project has a test/suite/ dir; and CFG_ENABLE_TEST_SUITE=ON enabled building/installing its contents.  "
            "This increases build time but allows serious testing of this project.")
    add_subdirectory(test/suite)
  else()
    message("This project has a test/suite/ dir; but CFG_ENABLE_TEST_SUITE=OFF disabled building/installing its contents.  "
            "This decreases build time; but the orthogonal CFG_SKIP_BASIC_TESTS=ON can still ensure the basics are okay.")
  endif()
else()
  message("In this project there is no test/suite/ dir; hence no serious tests of which to worry "
          "ignoring (CFG_ENABLE_TEST_SUITE).  "
          "See also the message above about basic tests (CFG_SKIP_BASIC_TESTS=ON versus OFF) "
          "which are a different (smaller) thing.")
endif()
