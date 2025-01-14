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
# use.  Update: That all remains true with one exception: This *is* our CMake script as far as *code generation*
# targets go.  It does *not* do doc generation; for that see FlowLikeDocGenerate.cmake.
#
# Usage: Assuming you're a dependent project: In your root CMakeLists.txt, ~at the top:
#
#   # We've only tested with this.  Possibly (probably?) lots of earlier versions would work; but they're not tested,
#   # so let's be conservative.
#   cmake_minimum_required(VERSION 3.26.3)
#
#   set(PROJ "(...see below...)")
#   set(PROJ_CAMEL "(...see below...)")
#   set(PROJ_HUMAN "(...see below...)")
#   set(OS_SUPPORT_MSG "(...see below...)")
#
#   find_package(Flow CONFIG REQUIRED)
#   include("${Flow_DIR}/../../../share/flow/cmake/FlowLikeProject.cmake")
#   # That, at least, determined $PROJ_VERSION, based on nearby VERSION file.  The following passes it to CMake.
#   project(${PROJ_CAMEL} VERSION ${PROJ_VERSION} DESCRIPTION ${PROJ_HUMAN} LANGUAGES CXX)
#   include("${Flow_DIR}/../../../share/flow/cmake/FlowLikeCodeGenerate.cmake")
#
# The variables:
#   PROJ: snake_case project name (e.g.: flow, ipc_core, ipc_transport_struct).  Various things will be keyed off
#     it including the main exported library lib${PROJ}.a (or equivalent).
#   PROJ_CAMEL: CamelCase equivalent of ${PROJ} (e.g., IpcTransportStruct).  Generally we stay away from CamelCase
#     in our projects; but this style is conventionally used for certain CMake names, so we make the exception there
#     (nowhere inside the source code itself though, as of this writing).
#   PROJ_HUMAN: Human-friendly brief name; e.g.: "Flow" or "Flow-IPC".  (It can have spaces too.)
#   OS_SUPPORT_MSG: Message to print if the detected OS environment is fatally unsuitable for building this.
#     If OS requirements between us and dependencies using this file diverge, we will need to be more configurable
#     than this.  For now see the OS tests performed below for the common requirement policy.
#     (Same for compiler and other requirements.  In general, though, we intend to share as much policy DNA as
#     possible between Flow and dependents (like Flow-IPC projects) that would use the present .cmake helper.)
#
# Additionally:
#   If and only if this is a part of a tree of Flow-like projects bundled into a meta-project, then the
#   meta-project (top) CMakeLists.txt shall `set(FLOW_LIKE_META_ROOT ${CMAKE_CURRENT_SOURCE_DIR})` before
#   the include(...FlowLikeProject.cmake) above, along with setting the other variables.  (The other projects
#   aside from the top one should not do so, only the top one.)
#
# Lastly note that if there is no src/CMakeLists.txt, then no lib${PROJ}.a shall be built.  However
# the tests in test/*, if present, shall still be built.  This combination may be useful in the case of a
# meta-project of several sub-projects: No src/ of its own but has, e.g., test/suite/*.

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
# As an example: Suppose project `flow` uses us (FlowLikeCodeGenerate.cmake), and dependent `ipc_core`
# (while being built as part of the same meta-project `ipc`) also uses
# us (FlowLikeCodeGenerate.cmake).  The CFG_* settings (saved in CMakeCache.txt for each relevant project)
# will affect *both* `flow` and `ipc_core`.  So it would be impossible, as of this writing, to (e.g.) have
# CFG_SKIP_BASIC_TESTS be ON for `flow` but OFF for `ipc_core`.  There is only one `cmake` or `ccmake`
# being invoked, and that setting affects each instance of FlowLikeCodeGenerate.cmake similarly.
# We have not added any namespace-type-thing into the name CFG_SKIP_BASIC_TESTS (or any other CFG_*).
# So all sub-builds sharing our DNA are affected in the same way.
#
# However, if `flow` is built/installed as a separate project, and then `ipc_core` is built/installed
# as a separate project (with `flow` consumed from its installed location), then CFG_SKIP_BASIC_TESTS can
# be different for each, as there's a `cmake` or `ccmake` separate invoked for each.
#
# TODO: *Consider* adding the per-project namespace to these settings.  For simplicity it's good the way it is;
# but it is conceivable a more complex setup may be desired at some point.

message(CHECK_START "(Project [${PROJ}]: creating code-gen/install targets.)")
list(APPEND CMAKE_MESSAGE_INDENT "- ")

message(VERBOSE "Project [${PROJ}] (CamelCase [${PROJ_CAMEL}], human-friendly "
                  "[${PROJ_HUMAN}])].")

message(VERBOSE "Install to CMAKE_INSTALL_PREFIX [${CMAKE_INSTALL_PREFIX}]; deps search path includes "
                  "CMAKE_PREFIX_PATH [${CMAKE_PREFIX_PATH}].")
message(VERBOSE "Installed items such as Boost and Doxygen might be "
                  "found in either place (among others) or a system location.  One typically sets PREFIX_PATH, if "
                  "INSTALL_PATH is not sufficient.  Build-system's install step shall place our results to "
                  "INSTALL_PATH, and locally installed inputs for the build are often found there too.")

# Environment checks.  Side effect: some "local" variables are set for later use.

# OS:
if(NOT LINUX)
  message(FATAL_ERROR ${OS_SUPPORT_MSG})
endif()

if(CFG_SKIP_CODE_GEN)
  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Skipped via CFG_SKIP_CODE_GEN=ON.)")
  return()
endif()

message(VERBOSE "Build generation requested by (presumably) project root CMake script; "
                  "not skipped via CFG_SKIP_CODE_GEN=OFF.")

# From now on Linux assumed (which implies *nix obv).  Though, where it's not too much trouble, lots of things
# will work in other OS (even Windows/MSVC) when/if that restriction is removed.  I.e., we try not to not-support
# other OS, all else being equal.

# Compiler:

message(STATUS "Detected compiler [ID/VERSION]: ${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}.")

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
  set(FLOW_LIKE_LTO_ON FALSE)
  message(STATUS "IPO/LTO disabled due to user-specified CFG_NO_LTO=ON.")
else()
  # This can result in some verbose output and take some time; so we do it only once per CMake run.
  # (We could use INTERNAL cache state for this, but we do not want it to persist across CMake runs; the compiler
  # specified could change between them; it'd be wrong.  So we use a different technique.)
  # To understand the technique look at how we're to be include()d.  If this is not part of a meta-project,
  # then we would only be include()d once, hence this doesn't matter.  Put a pin in that for now.
  # If we are indeed part of a meta-project, then we'd be include()d 1x per sub-project; and 1x for the meta-project
  # itself; in that order.  Hence:
  #   - meta_project/P1/CMakeLists.txt include()s us.
  #     - Our variable scope = meta_project/P1.  Our parent variable scope = meta_project.
  #   - meta_project/P2/CMakeLists.txt include()s us.
  #     - Our variable scope = meta_project/P2.  Our parent variable scope = meta_project.
  #   - ...
  #   - meta_project/CMakeLists.txt include()s us.
  #     - Our variable scope = meta_project.  Our parent variable scope = N/A.
  # Therefore the algorithm:
  #   - Check whether FLOW_LIKE_LTO_ON is DEFINED.  (If not DEFINED then: either it's not a meta-project, or
  #     we are P1.  In that case compute FLOW_LIKE_LTO_ON and set it both in our scope *and* the parent scope;
  #     unless there is no parent scope (then it's not a meta-project, and there's no need to worry about it).
  #     If it is defined though:
  #   - We are either P2, P3, ...; or we are the meta_project.  In the latter case, the previous bullet-point
  #     (for P1) would have set(FLOW_LIKE_LTO_ON) in our scope (its parent-scope); and DEFINED would have picked it up.
  #     In the former case (e.g., P2), the previous bullet-point (for P1) would have set(FLOW_LIKE_LTO_ON) in the parent
  #     scope -- which is also our parent scope -- and the DEFINED check would have picked that up).
  #     - In any case, FLOW_LIKE_LTO_ON's value can be used and not recomputed.
  # So the code is quite simple, but the reason it works is somewhat subtle.

  message(CHECK_START "(Checking IPO/LTO availability/capabilities.)")
  list(APPEND CMAKE_MESSAGE_INDENT "- ")

  if(DEFINED FLOW_LIKE_LTO_ON)
    list(POP_BACK CMAKE_MESSAGE_INDENT)
    if(FLOW_LIKE_LTO_ON)
      message(CHECK_PASS "(Determined earlier: Result = LTO available.)")
    else()
      message(CHECK_PASS "(Determined earlier: Result = LTO unavailable.)")
    endif()
  else()
    check_ipo_supported(RESULT FLOW_LIKE_LTO_ON OUTPUT output)
    if(NOT PROJECT_IS_TOP_LEVEL)
      set(FLOW_LIKE_LTO_ON ${FLOW_LIKE_LTO_ON} PARENT_SCOPE)
      # (It is possible, though less likely than not, that a non-FlowLikeCodeGenerate-using project does
      # add_subdirectory(X), where X = us, an individual Flow-like project but not a meta-project.  In that case
      # we will write FLOW_LIKE_LTO_ON in their scope.  That's a little odd but okay due to the naming.  (In a real
      # language, not a build script, this would be questionable.)  Actually functionally it's pretty good; as
      # if they use 2+ Flow-like projects in that manner, the LTO computations will still only occur once.
    endif()

    if(FLOW_LIKE_LTO_ON)
      # Also set this thing and similarly have it available in parent-scope for following sub-projects.
      check_cxx_compiler_flag(-ffat-lto-objects FLOW_LIKE_LTO_FAT_OBJECTS_IS_SUPPORTED)
      if(NOT PROJECT_IS_TOP_LEVEL)
        set(FLOW_LIKE_LTO_FAT_OBJECTS_IS_SUPPORTED ${FLOW_LIKE_LTO_FAT_OBJECTS_IS_SUPPORTED} PARENT_SCOPE)
      endif()

      if(NOT FLOW_LIKE_LTO_FAT_OBJECTS_IS_SUPPORTED)
        message(WARNING "[${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}] does not support fat LTO objects.  "
                          "This might cause difficulties in building non-LTO consumer (dependent) binaries.")
      endif()

      list(POP_BACK CMAKE_MESSAGE_INDENT)
      if(FLOW_LIKE_LTO_FAT_OBJECTS_IS_SUPPORTED)
        message(CHECK_PASS "(Done; result = LTO w/ fat-object format available, enabled (*Rel* build-types only).)")
      else()
        message(CHECK_PASS "(Done; result = LTO w/o fat-object format available, enabled (*Rel* build-types only).)")
      endif()
    else()
      message(VERBOSE "IPO/LTO is not supported; CMake check-procedure output: [[[\n${output}]]].")
      list(POP_BACK CMAKE_MESSAGE_INDENT)
      message(CHECK_PASS "(Done; result = LTO unavailable.  See VERBOSE output for details.)")
    endif()

    unset(output)
  endif()
endif()

message(VERBOSE "Environment checks passed.  Configuring build environment.")

# Project proper.

# Set CMake global stuff.

# CMAKE_CXX_STANDARD controls the C++ standard version with which items are compiled.
# We require C++17 at the lowest for compilation (and for any `#include`r -- but that is enforced via compile-time
# check in universally-included common.hpp, not by us here somehow).
# So if CMAKE_CXX_STANDARD is not specified by CMake invoker, we in fact build in C++17 mode.
# If it *is* specified, then we don't override it. In practice, as of this writing, that means it should be 17
# or 20. (If a lower one is attempted, we don't fight it here -- but common.hpp check will defeat it anyway.)

# Won't decay to lower standard if compiler does not support CMAKE_CXX_STANDARD (will fail instead).
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  message(STATUS "C++${CMAKE_CXX_STANDARD} language and STL required: set by this build script.")
else()
  message(STATUS "C++${CMAKE_CXX_STANDARD} language and STL requirement "
                   "inherited from externally set CMAKE_CXX_STANDARD.")
endif()

# When we do find_package(Threads) to link threading library this will cause it to
# prefer -pthread flag where applicable (as of this writing, just Linux, but perhaps any *nix ultimately).
set(THREADS_PREFER_PTHREAD_FLAG ON)
message(STATUS "`pthread` flag, if applicable, shall be used.")

# Set our own global stuff (constants, functions).
# Note: Some such things (WARN_FLAGS as of this writing) have already been set while scanning environment above.

# Call this on each executable and library.  Doesn't seem to be a way to do it "globally" (plus some things in here
# differ depending on the target's specifics; like library versus executable).
function(common_set_target_properties name)
  message(CHECK_START "(Configuring target [${name}]: uniform settings.)")
  list(APPEND CMAKE_MESSAGE_INDENT "- ")

  # Treat warnings as errors.  Note ${WARN_FLAGS} is separate and must be applied too.
  message(STATUS "Warnings: treated as errors.")
  set_target_properties(${name} PROPERTIES COMPILE_WARNING_AS_ERROR TRUE)

  # For each source file compiled into library add the warning flags we've determined.
  target_compile_options(${name} PRIVATE ${WARN_FLAGS})
  message(STATUS "Warnings: level set to [${WARN_FLAGS}].")

  # Show only a few errors per file before bailing out.
  # TODO: This is convenient when developing locally but can (IME rarely) be limiting in automated builds/CI/etc.;
  # consider making it configurable via knob.
  set(MAX_ERRORS 10) # XXX 3 # TEMP - DO NOT CHECK INTO BASE BRANCH.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${name} PRIVATE "-fmax-errors=${MAX_ERRORS}")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(${name} PRIVATE "-ferror-limit=${MAX_ERRORS}")
  else()
    message(FATAL_ERROR "Target [${name}]: For this target wanted to limit # of compile errors per file but compiler "
                          "(CMAKE_CXX_COMPILER_ID/VERSION [${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}])"
                          "is unknown; not sure what to do.  Please adjust the script to cover this eventuality.")
  endif()

  message(STATUS "Compiler will show at most [${MAX_ERRORS}] errors per file before bailing out.")

  # If LTO enabled+possible *and* not specifically and relevantly disabled (via option) for test executables...
  # ...then turn it on for this target, for all optimized build types (never for debug/unspecified build types).
  get_target_property(target_type ${name} TYPE)
  if(FLOW_LIKE_LTO_ON
       AND (NOT
              ((target_type STREQUAL "EXECUTABLE") AND CFG_NO_LTO_FOR_TEST_EXECS)))

    # Redundant to the message about generally enabling it; so VERBOSE log-level.
    message(VERBOSE "LTO: enabled (for *Rel* build types only).")

    set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
    # Do not do that for Debug (LTO + debug = no good) or unspecified type.


    # In case the consuming executable has LTO disabled generate object code that will support that as well;
    # in that case the LTO improvements will be limited to within the library -- which is most of the win anyway.
    if(FLOW_LIKE_LTO_FAT_OBJECTS_IS_SUPPORTED)
      message(STATUS "LTO: instructing compiler to generate in fat-object-LTO format.")

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
    endif()
  endif()

  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Done.)")
endfunction()

message(VERBOSE "Environment configured.  Invoking further building in subdirs.")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
  # This generates and exports lib$PROJ.a and its headers; and the $PROJ library CMake target.
  # (By the way I looked into it, and apparently unlike with certain other situations there is no need to prepend
  # ${CMAKE_CURRENT_SOURCE_DIR}.)
  add_subdirectory(src)
# else: Probably we are meta-project which typically has test/ but no C++ in src/.
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/basic")
  message(CHECK_START "(Creating code-generation targets: basic tests - simple bins for basic linkability/use "
                        "of lib/headers.)")
  list(APPEND CMAKE_MESSAGE_INDENT "- ")

  if(CFG_SKIP_BASIC_TESTS)
    list(POP_BACK CMAKE_MESSAGE_INDENT)
    message(CHECK_PASS "(Skipped via CFG_SKIP_BASIC_TESTS=ON.)")
  else()
    add_subdirectory(test/basic)
    list(POP_BACK CMAKE_MESSAGE_INDENT)
    message(CHECK_PASS "(Done.)")
  endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/suite")
  message(CHECK_START "(Creating code-generation targets: test suite(s) - serious testing.)")
  list(APPEND CMAKE_MESSAGE_INDENT "- ")

  if(CFG_ENABLE_TEST_SUITE)
    add_subdirectory(test/suite)
    list(POP_BACK CMAKE_MESSAGE_INDENT)
    message(CHECK_PASS "(Done.)")
  else()
    list(POP_BACK CMAKE_MESSAGE_INDENT)
    message(CHECK_PASS "(Skipped via CFG_ENABLE_TEST_SUITE=OFF.)")
  endif()
endif()

list(POP_BACK CMAKE_MESSAGE_INDENT)
message(CHECK_PASS "(Done.)")
