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

# This is to (root)/src/flow/CMakeLists.txt what FlowLikeCodeGenerate.cmake is to (root)/CMakeLists.txt.
# So: Essentially this *is* the Flow library's CMake script (CMakeLists.txt); but because at least one dependent
# project/repo (ipc_core: the core part of Flow-IPC, useful in its own right but also dependency of the others as of
# this writing) follows the same build policies/shares similar DNA, we make it available for such dependencies to
# use.  More specifically -- we build/export the Flow library a certain way and expect that those DNA-sharing projects
# like ipc_core will each build/export their main library in a similar way.
#
# Usage: Assuming you're a dependent project: In your root (root)/src/CMakeLists.txt, at the top:
#
#   # We assume you already followed the instructions in FlowLikeCodeGenerate.cmake, in your root CMakeLists.txt.
#   # So here you just need to add the stuff relevant to the library specifically; other things like ${PROJ}
#   # and ${Flow_DIR} should already be good to go.
#
#   set(DEP_LIBS ...see below...) # Optional (omit unless applicable).
#   set(DEP_LIBS_PKG_ARG_LISTS ...see below...) # Optional (omit unless applicable).
#   set(BOOST_LIBS ...see below...) # Optional (omit unless applicable).
#
#   set(SRCS ...see below...)
#   set(HDRS ...see below...)
#   # Optional (omit unless applicable):
#   function(generate_custom_srcs)
#     # ...Add more to SRCS and/or HDRS....
#
#     # Save into parent scope.
#     set(SRCS ${SRCS} PARENT_SCOPE) # ...and/or...
#     set(HDRS ${HDRS} PARENT_SCOPE)
#   endfunction()
#
#   include("${Flow_DIR}/../../../share/flow/cmake/FlowLikeLib.cmake")
#
# The variables:
#   DEP_LIBS: List of 1 or more library targets, such that any executable linking to lib${PROJ} must also link to
#     these.  For example if *your* library depends on just flow then: set(DEP_LIB Flow::flow).
#     As a matter of convention we ask that you do *not* include dependency targets already listed as such by
#     those you *did* list (e.g., if Flow::flow requires Threads::Threads, then you should omit it, even if you
#     use stuff from it directly yourself).
#     IMPORTANT: Suppose you need a library... in fact let's use Flow itself.  If your lib or needs Flow,
#     you'd indeed add "Flow::flow" to DEP_LIBS list.  However usually you would need to also make that target
#     available; so it is up to you to use find_package() to do so.  In case of Flow that is:
#       find_package(Flow 1.0 CONFIG REQUIRED) # Version number can be omitted depending on your needs.
#                                              # This would be done anytime before include() of FlowLikeLib.cmake.
#     Unfortunately in CMake that's not quite enough.  Your library, upon being `make install`ed, also needs to
#     essentially have a similar find_package() call in a certain file (namely ${PROJ_CAMEL}Config.cmake) in the
#     installed location (namely under .../lib/cmake/${PROJ_CAMEL}/).  Good news: we take care of all that for you.
#     Bad news: you'll need to tell us the args to find_package() that you used; but *omit* REQUIRED and/or QUIET
#     if you used those.  So in this case you would do: `list(APPEND DEP_LIBS_PKG_ARG_LISTS "Flow 1.0 CONFIG")`.
#     Careful: you need those quotes there; do not omit them.  Having done that the transitive dependencies will work.
#     You need not worry about this for BOOST_LIBS items however (we will take care of it).
#     Lastly: Specify system libs, like `rt`, as needed here too (remember to check for the appropriate platform if
#     applicable; e.g., `rt` is a Linux-only thing).  These don't require any find_package() normally and hence
#     shouldn't need to be reflected in DEP_LIBS_PKG_ARG_LISTS.  That said the "nice" way of specifically including
#     threading library (pthread in Linux for example) is:
#       find_package(Threads REQUIRED)
#       list(APPEND DEP_LIBS Threads::Threads) # THREADS_PREFER_PTHREAD_FLAG is already set by us.
#       list(APPEND DEP_LIBS_PKG_ARG_LISTS "Threads")#
#     Lastly!  Instead of a library target (as found by find_package()), or system lib (e.g., "rt"), you can
#     specify an absolute library path (e.g., "/usr/lib/.../libxyz.a"); you should do so for non-system libraries,
#     if find_package() support is not provided by the supplier of that library.  Tip: Use find_library() to obtain
#     this value.  Key tip: Most likely you must also, after include()ing this .cmake, add a PUBLIC include-path
#     entry for this library:
#       target_include_directories(${PROJ} PUBLIC ${X})
#       # Tip: Use find_path(${X} NAMES ${Y}), where Y is a nice example header; e.g.: "jemalloc/jemalloc.h".
#     For CMake newbies: The niceness is: this allows the CMake-invoking user to specify config values for the lib path
#     and/or include-path, if they live somewhere weird where find_*() can't find them, or if they're just paranoid;
#     but if they don't do so, CMake will probably find them anyway.  Best of both worlds.
#   DEP_LIBS_PKG_ARG_LISTS: See above.  To summarize: for each find_package() call you needed for a DEP_LIBS(), you
#     must append a similar snippet (as a string) into the list DEP_LIBS_PKG_ARG_LISTS.  If no such thing is needed,
#     you can leave DEP_LIBS_PKG_ARG_LISTS undefined or empty.
#   BOOST_LIBS: List of 1 or more Boost library names, such that any executable linking to lib${PROJ} must also
#     link to the accordingly-named Boost library (e.g., if you specify "program_options thread", then
#     the appropriate debug/threadedness version of libboost_program_options and libboost_thread shall get linked).
#     As a matter of convention we ask that you do *not* include Boost libs already set-up by 1 or more of DEP_LIBS.
#     E.g., if Flow::flow requires libboost_program_options, and you also use it directly, then you should omit it
#     (indeed omit setting BOOST_LIBS entirely, if you have no additional libs to mention).
#     Special case: If you don't need a *library*, but you do need Boost headers (any of them), then define
#     BOOST_LIBS but leave it empty: `set(BOOST_LIBS)`.  If you do specify a library, then the need for headers
#     is assumed.  If a DEP_LIBS already acquired the headers one way or another, you need not (and by convention
#     ideally should not) define BOOST_LIBS at all.
#   SRCS, HDRS: These are your source and header files respectively, expressed as relative paths.
#     As of this writing the convention is that they're all under src/${PROJ}, so assuming that stays accurate you'd do,
#     like:
#       set(SRCS ipc_core/a/b/x.cpp ipc_core/a/b/y.cpp ipc_core/d/e/a.cpp ...)
#       set(HDRS ipc_core/a/b/x.hpp ipc_core/d/e/a.hpp ...)
#   generate_custom_srcs(): Define this function if and only if, just before this .cmake performs add_library(SRCS)
#     and exports HDRS, you need to add more source files to compile and/or headers to export.  This is necessary
#     at least when those sources and/or headers are to be themselves generated from something; the known use case
#     at the moment being Cap'n Proto .capnp schemas generating .capnp.c++ and .capnp.h files (another example that
#     comes to mind is lex .l processing; similarly for yacc).  The generated files shall be placed into
#     the typical CMake work dir which is ${CMAKE_CURRENT_BINARY_DIR}; off that root keep in mind that the headers
#     will be exported based on what further subdir they reside in; so for example ${C_C_B_D}/a/b/x.capnp.h will
#     ended up exported in .../include/a/b/x.capnp.h, right by (for example) a regular a/b/y.hpp in HDRS.
#
# (Note: Originally we had a nice searcher for .cpp and .hpp, etc., excluding detail/doc/ -- you get the idea.  Then
# we saw the note in CMake docs: "We do not recommend using GLOB to collect a list of source files from your source
# tree.  If no CMakeLists.txt file changes when a source is added or removed then the generated build system cannot know
# when to ask CMake to regenerate.  The CONFIGURE_DEPENDS flag may not work reliably on all generators, or if a
# new generator is added in the future that cannot support it, projects using it will be stuck.
# Even if CONFIGURE_DEPENDS works reliably, there is still a cost to perform the check on every rebuild.""
# Annoying, but perhaps there's wisdom in their words.)

# Without further ado... let's go.

# Our goal here is to make lib${PROJ}.a (e.g, libflow.a) and export it and its headers
# (so `make install` in particular would put them into .../lib and .../include respectively);
# and to have the ${PROJ} target ready, so that test/demo programs of ours can link against it in subsequent
# `CMakeLists.txt`s.  In addition, so that dependent CMake-based projects can use find_package() to easily so
# that they, like our aforementioned test/demo programs, can link against it also, we need to export a bit
# more CMake voodoo into the install root (for when they do `make install` or equivalent).

include(CMakePackageConfigHelpers)

message(CHECK_START "(Library [${PROJ}] + headers/etc.: creating code-gen/install targets.)")
list(APPEND CMAKE_MESSAGE_INDENT "- ")

# Prepare dependencies.

# As specified above:
#   - undefined BOOST_LIBS => don't worry about it;
#   - defined BOOST_LIBS but blank => do ensure Boost of the proper version is available/configured for headers;
#   - defined BOOST_LIBS and not blank => additionally ensure those Boost libs are available/configured for use.
if(DEFINED BOOST_LIBS)
  set(BOOST_VER 1.86) # Current as of Jan 2025.#XXX 87

  message(CHECK_START "(Finding dep: Boost-${BOOST_VER}: headers plus libs [${BOOST_LIBS}].)")
  list(APPEND CMAKE_MESSAGE_INDENT "- ")

  # Boost is necessary.
  #   - Boost headers are assumed available and commonly `#include`d by us.
  #   - (Optional) Certain Boost libraries must also be linked by any consumer executable's build.
  # Details:
  #   - `CONFIG`: Use the Boost-supplied BoostConfig.cmake (available from 1.70 on) to make it work; rather than falling
  #     back to CMake's own FindBoost.cmake mechanism.  CMake's FindBoost docs suggest this method is the cool way of
  #     doing it; at this stage we want to reduce entropy by relying only on that method.  Also we don't need to
  #     worry about FindBoost's various hairy switches this way.
  #   - $BOOST_VER is current as of (see above).  We have no reason to suspect a newer version would break something.
  #     Slightly older versions would probably work, but we have lost track of how far back is okay; this is the
  #     baseline of when we know it works.  TODO: Consider finding out and changing this accordingly to achieve wider
  #     usability.  PRs are welcome as ever.
  # Caution: There is related find_dependency() code closer to the bottom of this .cmake.
  find_package(Boost ${BOOST_VER} CONFIG REQUIRED COMPONENTS ${BOOST_LIBS}) # OK if ${BOOST_LIBS} blank.

  foreach(boost_lib ${BOOST_LIBS})
    list(APPEND BOOST_TARGETS "Boost::${boost_lib}")
  endforeach()
  # OK if ${BOOST_TARGETS} is undefined (empty).

  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Done; found around: [${Boost_DIR}].)")
# elseif(NOT DEFINED BOOST_LIBS): No Boost (not even headers) required on our behalf.
endif()

# Make the library.

message(VERBOSE "Source (to compile) list: [${SRCS}].")
message(VERBOSE "Exported header list: [${HDRS}].")
if(COMMAND generate_custom_srcs)
  message(CHECK_START "(Generating custom sources/headers.)")
  list(APPEND CMAKE_MESSAGE_INDENT "- ")

  generate_custom_srcs()

  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Done.)")
endif() # Otherwise function does not exist; no hook to execute.

# Create the library.  For now we only operate in static-library-land.  TODO: Look into shared-library-land.
add_library(${PROJ} STATIC ${SRCS})

# Do stuff we've resolved to do on all our targets.
common_set_target_properties(${PROJ})

# Include directories for the target.  This is somewhat subtle.
#   - At a minimum we are specifying the -I (in gcc parlance) include-path for the building of this library itself.
#     Since in the case of CMake the exporting of headers into the install-path would only occur upon `make install`
#     (not mere `make`), we want to be simply "./${PROJ}", so that when our code says `#include <${PROJ}/x/blah.hpp>`,
#     it will work, as we've added "." into the include-path of our own compile commands.
#   - We are also specifying the include-path for consumer executables (and/or libraries) built with CMake.
#     At a minimum that's our own test/demo programs.  Since these would typically be built during the `make`
#     (not after `make install`) stage, we'd want them to have our "." in their include-path too, for when
#     they too do `#include <${PROJ}/whatever/whatever.hpp>`.
#     - However, what about stuff built off the `make install`ed (or equivalent) tree -- the exported library?
#       If it's not built with CMake, then it's not our problem; we can't affect that anyway.  But apparently
#       CMake could be used to build against our installed (exported) tree.  So in that case we'd want
#       the installed (exported) header location to be in the include-path instead.  (The headers' contents
#       would be equal anyway.)
# ChatGPT-4 taught me about this and suggested this allegedly encouraged way of doing it: the `make` stage will
# operate within ".", while building a dependent upon a `make install`ed lib${PROJ} will operate within
# the exported location.
target_include_directories(${PROJ} PUBLIC
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                           $<INSTALL_INTERFACE:include>)  # Relative to CMAKE_INSTALL_PREFIX.

# Also we promised in documentation that generated headers (to be placed under ${CMAKE_CURRENT_BINARY_DIR} work dir)
# are allowed; so in that case add that to the include-path (again only for the BUILD_INTERFACE; after
# `make install` or equivalent they will have been placed into ...install-prefix/include.. by the preceding statement).
if(COMMAND generate_custom_srcs)
  target_include_directories(${PROJ} PUBLIC
                             $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
endif()

message(STATUS "Dependents shall need to link libs: Boost [${BOOST_TARGETS}]; other: [${DEP_LIBS}].")

# Consumer of the library will need to link these.
target_link_libraries(${PROJ} PUBLIC
                      ${BOOST_TARGETS} # OK if undefined (empty).  If not empty we find_package()d it ourselves.
                      ${DEP_LIBS}) # Our include()r by contract was to find_package() for each of these as needed.

# The above is sufficient to build (`make` or equivalent) the library.
# Now to ensure `make install` (or equivalent) does the right thing.

message(VERBOSE
          "Install (`make install` or equivalent) will export static lib, headers, and some CMake script(s) for use "
            "with a dependent's CMake script (via find_package()).")

# The headers must be exported.  It can be done with a simple install(FILES), but apparently associating it with
# target ${PROJ} using target_sources(FILE_SET) is the nice/modern way of doing it; then install(TARGET) will
# install the headers along with the library.  Also then by using BASE_DIRS we can specify that some of the headers
# will have been generated (via generate_custom_srcs()) and therefore live in CMAKE_CURRENT_BINARY_DIR; with the
# install() method we'd have to delete the prefix manually which is somewhat painful.
target_sources(${PROJ} PUBLIC FILE_SET HEADERS
               # Know that the generated headers are off this CMAKE_CURRENT_BINARY_DIR (strip prefix).
               # The normal headers are in CMAKE_CURRENT_SOURCE_DIR and are specified as relative paths.
               # We do have to specify CMAKE_CURRENT_SOURCE_DIR explicitly too though.
               BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
               FILES ${HDRS})

install(TARGETS ${PROJ} # Regarding the library:
        # Let's put our CMake info into ...Targets.cmake; supposedly then CMake-using consumers will be
        # able to find_package() us to seamlessly use our library/headers with proper include-path and dependency
        # libs and so on.
        EXPORT ${PROJ_CAMEL}Targets # E.g., FlowTargets.cmake.
        # Put it there (off the install path).
        ARCHIVE DESTINATION lib
        # Put the headers here.
        FILE_SET HEADERS # Without `DESTINATION <...>` it'll go to `include`, which is fine; and accordingly:
        # Express the target_include_directories() stuff configured above.
        # If I (ygoldfel) understand correctly, without an EXPORT clause and a separate install() to export
        # a <...>Targets.cmake, this only has effect in builds within our CMake project itself.  I'm not 100%
        # sure about the details.  However it seems at worst harmless to keep this even without EXPORTing (but
        # we now *do* EXPORTing just below).
        INCLUDES DESTINATION include)

# And lastly apparently ...Targets.cmake must be explicitly exported too (if someone wanted to use it).
# By the way, for context, the Flow-IPC libs (if indeed one is working with that formally separate-from-Flow
# repo/CMake tree/project) which depend on Flow shall make use of Flow doing this (they'll be doing
# find_package() to find it and such).  So in some sense we're eating our own dog-food in terms of one
# of our projects exercising the CMake-info-EXPORTing code above and below.

install(EXPORT ${PROJ_CAMEL}Targets
        FILE ${PROJ_CAMEL}Targets.cmake
        NAMESPACE ${PROJ_CAMEL}::
        DESTINATION lib/cmake/${PROJ_CAMEL})

# For find_package() (of us) to work we must also generate/export ${PROJ_CAMEL}Config.cmake whose main
# (possibly only) job would be to include() ...Targets.cmake sitting right next to it.
# And add versions file, so version can be properly specified (e.g., find_package(Flow 1.0 REQUIRED)).

# Config version file.

set(config_version_file ${CMAKE_CURRENT_BINARY_DIR}/${PROJ_CAMEL}ConfigVersion.cmake)
write_basic_package_version_file(${config_version_file} # In work dir.
                                 VERSION ${PROJECT_VERSION} COMPATIBILITY AnyNewerVersion)

# As for ...Config.cmake we shall generate it dynamically here manually.  First read docs (at top of this file) for
# DEP_LIBS, DEP_LIBS_PKG_ARG_LISTS, and BOOST_LIBS; then return here.

message(STATUS "Install target: Exports ${PROJ_CAMEL}Config.cmake: for dependents' CMake find_package().")

set(config_file "${CMAKE_CURRENT_BINARY_DIR}/${PROJ_CAMEL}Config.cmake") # Prepare it in work dir.
message(VERBOSE "Its location pre-install: [${CMAKE_CURRENT_BINARY_DIR}/${PROJ_CAMEL}Config.cmake].")

file(WRITE ${config_file} [=[
# File generated by FlowLikeLib.cmake.

]=])

block()
  # Now to load any dependencies needed by things in BOOST_LIBS and/or DEP_LIBS.
  if(DEFINED BOOST_LIBS)
    # Per find_dependency() macro docs we are to mirror find_dependency() args but omit REQUIRED and QUIET.
    # Again it is fine if ${BOOST_LIBS} is defined but blank (then this will load the headers target).
    # Caution: May need to keep in sync with find_dependency() code closer to the top of this .cmake.
    list(JOIN BOOST_LIBS " " boost_libs_args)
    set(deps_snippet "find_dependency(Boost ${BOOST_VER} CONFIG COMPONENTS ${boost_libs_args})\n")
  endif()
  if(DEP_LIBS_PKG_ARG_LISTS)
    foreach(dep_libs_pkg_arg_list ${DEP_LIBS_PKG_ARG_LISTS})
      set(deps_snippet "${deps_snippet}"
                       "find_dependency(${dep_libs_pkg_arg_list})\n")
    endforeach()
  endif()

  if(deps_snippet)
    file(APPEND ${config_file} [=[
# Load dependencies that are probably mentioned in ...Targets.cmake include()d below.
include(CMakeFindDependencyMacro) # Needed for find_dependency() macro.
  ]=] ${deps_snippet} "\n")
  endif()

  # Now that any find_dependency() (internally find_package()) calls have been written out, the resulting
  # targets listed in ...Targets.cmake should resolve.  Note the first $ is escaped (${CMAKE_CURRENT_LIST_DIR} is
  # written verbatim, not replaced); the second is not (${PROJ_CAMEL} is replaced).
  file(APPEND ${config_file} "include(\"\${CMAKE_CURRENT_LIST_DIR}/${PROJ_CAMEL}Targets.cmake\")\n")

  # Files are generated: export them via `make install` (or equivalent).
  install(FILES ${config_file} ${config_version_file}
          DESTINATION lib/cmake/${PROJ_CAMEL})
endblock()

list(POP_BACK CMAKE_MESSAGE_INDENT)
message(CHECK_PASS "(Done.)")
