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

# This is analogous to FlowLikeCodeGenerate.cmake, in that it must be include()d from the root CMake script also;
# except that:
#   - It is optional.
#   - It takes care of documentation generation specifically.
#   - It will no-op if variable or cache setting CFG_ENABLE_DOC_GEN=OFF; will proceed if ON.
#     - Doxygen won't be necessary in the former case which can be helpful for a mainstream use case;
#       after all one might peruse online docs and only want to build the project in order to use it in one's
#       dev project.  Doc generation is more likely needed for devs of the project itself.
#
# It uses Doxygen to generate the documentation.  The style of the documentation is controlled internally to this
# script; you will just need to:
#   - Set a few knobs via CMake variables (see below).
#   - Accordingly intersperse your source code with Doxygen-enabled (meaning featuring Doxygen @commands) C++
#     comments and (if desired) a set of additional Doxygen-comment-bearing only files containing additional
#     documentation such as a guided manual (as opposed to the reference-style documentation interspersed within
#     the actual source code).
#     - The documentation of C++ code must follow the Flow style guide (doc-coding_style.cpp under flow/src).
#
# By include()ing this your CMake-generated build will feature 2 added targets:
#   - ${PROJ}_doc_full (full documentation including internal/private items and source browser; plus guided manual if
#     any);
#   - ${PROJ}_doc_public (subset of the above omitting internal/private items and source browser instead only exposing
#     the public API; plus guided manual if any).
# So `make ..._doc_full` or `make ..._doc_public` (or equivalent) by the user shall generate html_..._doc_.../, such
# that the index.html therein contains the home page of a nice set of web documentation.  This can then be uploaded to
# a web server or viewed locally.
#
# Usage: Assuming you're a dependent project: In your (root)/CMakeLists.txt: Do your non-documentation stuff
#        as per FlowLikeCodeGenerate.cmake documentation.  Then below all that:
#
#   set(DOC_INPUT (...see below...))
#   set(DOC_EXCLUDE_PATTERNS (...see below...))
#   # Optional:
#   set(DOC_IMAGE_PATH (...see below...))
#
# The variables:
#   DOC_INPUT: The source directories (they will be scanned recursively).  In practice there are 2 possible
#     situations.  The simpler one is where this project is *not* a meta-project containing sub-projects with
#     separate source trees.  In that case this shall be `src/<root name like flow>`.  If it *is* a meta-project
#     named `proj`, then this shall be: `proj1/src/proj proj2/src/proj <...>`, where proj1 is the first sub-project,
#     proj2 the second, etc.  Important: the generated documentation shall behave as if there were one common
#     source directory src/proj, containing the merged-together contents of each proj<...>/src/proj.  As long as
#     there's no overlap between individual files in all the sub-projects, it will all work as a monolith.
#     The generated documentation shall have the <???>/src/proj prefix stripped from any file names.
#   DOC_EXCLUDE_PATTERNS: Subtrees or individual files that will be excluded from DOC_INPUT.  For example if
#     DOC_INPUT = src/flow (which contains async/ which contains a/ and b/), and
#     DOC_EXCLUDE_PATTERNS = src/flow/async/a/*, then the async/a/... subtree shall be excluded from doc generation,
#     leaving in this case async/b only.  Note: You must define this, even if it is "" (i.e., no exclusions).
#   DOC_IMAGE_PATH: See Doxygen IMAGE_PATH config setting.  This is useful typically for guided-manual files with
#     figures.
# All those paths shall be specified relative to the project root (same place as the root CMakeLists.txt).

message(CHECK_START "(Project [${PROJ}] (CamelCase [${PROJ_CAMEL}], human-friendly "
                      "[${PROJ_HUMAN}]), version [${PROJ_VERSION}]: creating doc-generation targets.)")
list(APPEND CMAKE_MESSAGE_INDENT "- ")

if(NOT CFG_ENABLE_DOC_GEN)
  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Skipped via CFG_ENABLE_DOC_GEN=OFF.)")
  return()
endif()

message(VERBOSE
          "Doc generation requested by (presumably) project root CMake script; not skipped via CFG_ENABLE_DOC_GEN=ON.")

message(CHECK_START "(Validating Doxygen/etc. environment.)")
list(APPEND CMAKE_MESSAGE_INDENT "- ")

# We are using the delightful FindDoxygen module of CMake; it allows one to (1) ensure Doxygen and related
# tools are available and at the proper version(s); and (2) actually generate the doc-generating `make`
# (or equivalent) targets that will in fact invoke Doxygen properly.  The latter is done using doxygen_add_docs().
# It might help to glance at FindDoxygen's online docs.  Long story short -- it's delightful; it will generate
# the Doxyfile while letting us tweak individual options as desired; it will run Doxygen; great.

# Find the required tools including Doxygen; thus making doxygen_add_docs() available to use.

# (Avoiding DOXYGEN_ prefix, as it has special meaning in find_package(Doxygen) (see FindDoxygen docs).)
set(DOC_DOXYGEN_VERSION 1.9.3)
# CAUTION: If you decide to increase DOC_DOXYGEN_VERSION: Carefully ensure you've properly modified
# doc_generate() below.  See inside it.
find_package(Doxygen ${DOC_DOXYGEN_VERSION} REQUIRED dot)
message(STATUS "Doxygen version [${DOXYGEN_VERSION}] (+ deps like `dot`) found.")
if(DOXYGEN_VERSION STREQUAL ${DOC_DOXYGEN_VERSION})
  message(STATUS "That *exact* version = what we wanted.  Good.")
else()
  message(WARNING "Doxygen version [${DOXYGEN_VERSION}] found; we require at least [${DOC_DOXYGEN_VERSION}]; but "
                    "to guarantee the proper output we ideally want *exactly* that version.  Unfortunately Doxygen "
                    "tends to be finicky with unpleasant surprises as versions increase (in our experience).  "
                    "Build will continue; however care *must* be taken before publishing any documentation generated "
                    "by this build.  Yes: it is unfortunate Doxygen tends to have regressions.")
endif()

block()
  get_target_property(doxygen_exec Doxygen::doxygen IMPORTED_LOCATION)
  get_target_property(dot_exec Doxygen::dot IMPORTED_LOCATION)
  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Done; Doxygen bin at [${doxygen_exec}]; `dot` bin at [${dot_exec}].)")
endblock()

# Sanity-check the DOC_* inputs of our include()r.

if(NOT PROJ_HUMAN) # This is a FlowLikeCodeGenerate.cmake requirement.
  message(FATAL_ERROR "Please set PROJ_HUMAN in your root CMake script (see FlowLikeCodeGenerate.cmake for spec).")
endif()

if(NOT DOC_INPUT) # Must not be empty.
  message(FATAL_ERROR "Please set DOC_INPUT in your root CMake script to the input directories to (recursively) "
                        "use for doc generation, specified relative to the project root.  "
                        "This should include at least your src/ or certain subdirs of it; and if there are "
                        "guided manual *.dox.txt files then that dir as weel.  Use DOC_EXCLUDE_PATTERNS to remove "
                        "subtrees and/or files from this.")
endif()
if(NOT DEFINED DOC_EXCLUDE_PATTERNS) # Can be empty but must be consciously so.
  message(FATAL_ERROR "Please set DOC_EXCLUDE_PATTERNS (to empty, if so desired) in your root CMake script to "
                        "any subtrees and/or files to remove from consideration after finding files based on "
                        "DOC_INPUT.")
endif()

# OK; so as promised we'll make 2 targets; one for full docs; one for public-facing docs.  The following function
# takes care of each one depending on the Boolean arg.
function(doc_generate full_else_public)
  # We will doxygen_add_docs() at the bottom.  The inputs into it are the file list passed directly to it as args;
  # a handful of other optional arg options; and variables named DOXYGEN_$x, where each $x is a Doxygen config
  # setting (see Doxygen docs) as would appear in Doxyfile.  (doxygen_add_docs()'s generated target will take care
  # of the Doxyfile creation/management itself however.)  Note:
  #   - List options have different format in CMake versus Doxyfiles, but doxygen_add_docs() knows this and
  #     will do the translation for us -- so we can specify lists in standard CMake style.
  #   - A handful of explicitly named options will be handled somewhat specially by CMake; like for example
  #     DOXYGEN_EXCLUDE_PATTERNS given by us will be merged with a few other common-sense patterns added by
  #     CMake.  Anyway -- just check the FindDoxygen docs; ultimately it's all common-sense stuff.
  #   - That aside most knobs are straightforward; set(DOXYGEN_$x $y) => `$x = $y` in Doxyfile.
  #
  # Doxygen config setting considerations:
  #   - The way one does it directly is, for Doxygen version X, they'll do `doxygen -u` to generate
  #     a clean Doxyfile (which will include X in the top comment) containing every option, set to its default
  #     value and preceded by its official description/documentation including a textual note as to the default
  #     value as well.
  #   - Hence we shall rely on our chosen version DOC_DOXYGEN_VERSION's similar clean file as input to the below
  #     logic.  For each option where we do *not* override the default, we will omit it here for brevity.
  #   - For each option where (for the present value of `full_else_public`) we *do* override it, we will
  #     - Copy/paste the comment from the clean Doxyfile.
  #     - Explain, under "# OVERRIDING DEFAULT: ", why we're choosing the value we are.
  #     - Then `set(... ...)` accordingly.
  # It is important, to retain maintability -- and sanity -- to keep following these conventions.
  #
  # Lastly, if you've just decided to increase DOC_DOXYGEN_VERSION, you will need to carefully go through this
  # function and update it.  If I (ygoldfel) recall correctly Doxygen might provide a tool to upgrade a Doxyfile;
  # so one could take the 2 configurations' Doxyfiles generated by `make` (or equivalent) and run them through
  # that.  Then carefully go through the diff between the before-versus-after Doxyfiles and change anything below
  # (including any per-option comments).  Last but not least it is important to carefully run through the
  # generated HTML (x2) in a browser, so that any (new) Doxygen mess-ups are caught and worked-around -- either
  # via option tweak or source code tweak.  As noted earlier unfortunately, for all its power, Doxygen tends to
  # add regressions big and small as versions increase (though, bugs get fixed too, and at a higher rate).

  set(DOXYGEN_PROJECT_NAME ${PROJ_HUMAN})
  if(full_else_public)
    set(DOXYGEN_PROJECT_BRIEF "${PROJ_HUMAN} project: Full implementation reference.")
    set(target ${PROJ}_doc_full)
    set(comment "Generate full docs to [html_${target}]: includes private items and a source browser.")

    message(CHECK_START "(Target [${PROJ}_doc_full] [${comment}]: generating.)")
    list(APPEND CMAKE_MESSAGE_INDENT "- ")

    # Attn: Doxygen options.  Comment directly from Doxygen-generate default config file is always included.

    # If the EXTRACT_PRIVATE tag is set to YES, all private members of a class will
    # be included in the documentation.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_EXTRACT_PRIVATE YES)
    # If the EXTRACT_PRIV_VIRTUAL tag is set to YES, documented private virtual
    # methods of a class will be included in the documentation.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_EXTRACT_PRIV_VIRTUAL YES)
    # If the SOURCE_BROWSER tag is set to YES then a list of source files will be
    # generated. Documented entities will be cross-referenced with these sources.
    #
    # Note: To get rid of all source code in the generated output, make sure that
    # also VERBATIM_HEADERS is set to NO.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_SOURCE_BROWSER YES)
    # If the EXTRACT_PACKAGE tag is set to YES, all members with package or internal
    # scope will be included in the documentation.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_EXTRACT_PACKAGE YES)
    # If the EXTRACT_STATIC tag is set to YES, all static members of a file will be
    # included in the documentation.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_EXTRACT_STATIC YES)
    # The INTERNAL_DOCS tag determines if documentation that is typed after a
    # \internal command is included. If the tag is set to NO then the documentation
    # will be excluded. Set it to YES to include the internal documentation.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_INTERNAL_DOCS YES)
    # Setting the STRIP_CODE_COMMENTS tag to YES will instruct doxygen to hide any
    # special comment blocks from generated source code fragments. Normal C, C++ and
    # Fortran comments will always remain visible.
    # The default value is: YES.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_STRIP_CODE_COMMENTS NO)
    # If the REFERENCED_BY_RELATION tag is set to YES then for each documented
    # entity all documented functions referencing it will be listed.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_REFERENCED_BY_RELATION YES)
    # If the REFERENCES_RELATION tag is set to YES then for each documented function
    # all documented entities called/used by that function will be listed.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_REFERENCES_RELATION YES)
    # If the CALL_GRAPH tag is set to YES then doxygen will generate a call
    # dependency graph for every global function or class method.
    #
    # Note that enabling this option will significantly increase the time of a run.
    # So in most cases it will be better to enable call graphs for selected
    # functions only using the \callgraph command. Disabling a call graph can be
    # accomplished by means of the command \hidecallgraph.
    # The default value is: NO.
    # This tag requires that the tag HAVE_DOT is set to YES.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_CALL_GRAPH YES)
    # If the CALLER_GRAPH tag is set to YES then doxygen will generate a caller
    # dependency graph for every global function or class method.
    #
    # Note that enabling this option will significantly increase the time of a run.
    # So in most cases it will be better to enable caller graphs for selected
    # functions only using the \callergraph command. Disabling a caller graph can be
    # accomplished by means of the command \hidecallergraph.
    # The default value is: NO.
    # This tag requires that the tag HAVE_DOT is set to YES.
    # OVERRIDING DEFAULT: This is a full source browser/annotated doc.
    set(DOXYGEN_CALLER_GRAPH YES)
  else()
    set(DOXYGEN_PROJECT_BRIEF "${PROJ_HUMAN} project: Public API.")
    set(target ${PROJ}_doc_public)
    set(comment "Generate public-API docs to [html_${target}]: suitable for public-facing docs like a user manual.")

    message(CHECK_START "(Target [${PROJ}_doc_full] [${comment}]: generating.)")
    list(APPEND CMAKE_MESSAGE_INDENT "- ")

    # Attn: Doxygen options.  Comment directly from Doxygen-generate default config file is always included.

    # If the EXTRACT_LOCAL_CLASSES tag is set to YES, classes (and structs) defined
    # locally in source files will be included in the documentation. If set to NO,
    # only classes defined in header files are included. Does not have any effect
    # for Java sources.
    # The default value is: YES.
    # OVERRIDING DEFAULT: This is a public API doc.
    set(DOXYGEN_EXTRACT_LOCAL_CLASSES NO)
    # If the HIDE_UNDOC_MEMBERS tag is set to YES, doxygen will hide all
    # undocumented members inside documented classes or files. If set to NO these
    # members will be included in the various overviews, but no documentation
    # section is generated. This option has no effect if EXTRACT_ALL is enabled.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a public API doc.
    set(DOXYGEN_HIDE_UNDOC_MEMBERS YES)
    # If the HIDE_UNDOC_CLASSES tag is set to YES, doxygen will hide all
    # undocumented classes that are normally visible in the class hierarchy. If set
    # to NO, these classes will be included in the various overviews. This option
    # has no effect if EXTRACT_ALL is enabled.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a public API doc.
    set(DOXYGEN_HIDE_UNDOC_CLASSES YES)
    # If the HIDE_FRIEND_COMPOUNDS tag is set to YES, doxygen will hide all friend
    # declarations. If set to NO, these declarations will be included in the
    # documentation.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a public API doc.
    set(DOXYGEN_HIDE_FRIEND_COMPOUNDS YES)
    # If the HIDE_IN_BODY_DOCS tag is set to YES, doxygen will hide any
    # documentation blocks found inside the body of a function. If set to NO, these
    # blocks will be appended to the function's detailed documentation block.
    # The default value is: NO.
    # OVERRIDING DEFAULT: This is a public API doc.
    set(DOXYGEN_HIDE_IN_BODY_DOCS YES)
    # If the SHOW_INCLUDE_FILES tag is set to YES then doxygen will put a list of
    # the files that are included by a file in the documentation of that file.
    # The default value is: YES.
    # OVERRIDING DEFAULT: This is a public API doc.
    set(DOXYGEN_SHOW_INCLUDE_FILES NO)
    # If the VERBATIM_HEADERS tag is set the YES then doxygen will generate a
    # verbatim copy of the header file for each class for which an include is
    # specified. Set to NO to disable this.
    # See also: Section \class.
    # The default value is: YES.
    # OVERRIDING DEFAULT: This is a public API doc.
    set(DOXYGEN_VERBATIM_HEADERS NO)
  endif()
  # Version should come from project() automatically.

  # Attn: Doxygen options.  Comment directly from Doxygen-generate default config file is always included.

  # If the value of the INPUT tag contains directories, you can use the
  # FILE_PATTERNS tag to specify one or more wildcard patterns (like *.cpp and
  # *.h) to filter out the source-files in the directories.
  #
  # Note that for custom extensions or not directly supported extensions you also
  # need to set EXTENSION_MAPPING for the extension otherwise the files are not
  # read by doxygen.
  #
  # Note the list of default checked file patterns might differ from the list of
  # default file extension mappings.
  #
  # If left blank the following patterns are tested:*.c, *.cc, *.cxx, *.cpp,
  # *.c++, *.java, *.ii, *.ixx, *.ipp, *.i++, *.inl, *.idl, *.ddl, *.odl, *.h,
  # *.hh, *.hxx, *.hpp, *.h++, *.l, *.cs, *.d, *.php, *.php4, *.php5, *.phtml,
  # *.inc, *.m, *.markdown, *.md, *.mm, *.dox (to be provided as doxygen C
  # comment), *.py, *.pyw, *.f90, *.f95, *.f03, *.f08, *.f18, *.f, *.for, *.vhd,
  # *.vhdl, *.ucf, *.qsf and *.ice.
  # OVERRIDING DEFAULT: Let's just specify what we do want, not make exceptions for things we don't.
  # The original list is commented out below.
  # UPDATE: Added *.dox.txt, which are pseudo-C++ files with /** */ comments
  # containing guided Manual documentation (and so on -- stuff outside source
  # proper).  As of this writing Flow actually lacks these; but sister component
  # Flow-IPC does have a budding Manual, and we might as well inherit the same
  # patterns.  Another way to do this was .md (markdown) files; or *.dox (see
  # above); and maybe the latter would have been better a tiny bit; but it's a
  # text file, and I (ygoldfel) prefer not to use weird extensions unrecognized
  # by text editors.  Anyway, it's not a big deal and can be changed later.
  # *.dox.txt does work though; and it assumes C++ in ~~~code~~~ blocks, so that
  # seems good for us.
  # NOTE: This used to have .h, but we emphatically don't have any such files,
  # as that implies C, not C++, and also then it starts picking up capnp-generated
  # files which is annoying to filter out.  If we have C-exported headers this
  # might change, but until then... keep it clean.
  set(DOXYGEN_FILE_PATTERNS "*.cpp" "*.hpp" "*.dox.txt")
  # *.c *.cc *.cxx *.cpp *.c++ *.d *.java *.ii *.ixx *.ipp *.i++ *.inl *.h *.hh *.hxx *.hpp
  #   *.h++ *.idl *.odl *.cs *.php *.php3 *.inc *.m *.markdown *.md *.mm *.dox *.py *.f90 *.f *.for *.vhd
  #   *.vhdl

  # The PREDEFINED tag can be used to specify one or more macro names that are
  # defined before the preprocessor is started (similar to the -D option of e.g.
  # gcc). The argument of the tag is a list of macros of the form: name or
  # name=definition (no spaces). If the definition and the "=" are omitted, "=1"
  # is assumed. To prevent a macro definition from being undefined via #undef or
  # recursively expanded use the := operator instead of the = operator.
  # This tag requires that the tag ENABLE_PREPROCESSING is set to YES.
  # OVERRIDING DEFAULT: Set (FLOW|IPC|etc...)_DOXYGEN_ONLY to make it possible to have code
  # only Doxygen scans, while compiler ignores it. Helpful for corner case
  # situations usually involving preprocessor-conditioned branching.
  # To use it, just bracket code with an #ifdef referring to this.
  string(TOUPPER ${PROJ} proj_upper)
  set(DOXYGEN_PREDEFINED "${proj_upper}_DOXYGEN_ONLY:=1")

  # If the JAVADOC_AUTOBRIEF tag is set to YES then doxygen will interpret the
  # first line (until the first dot) of a Javadoc-style comment as the brief
  # description. If set to NO, the Javadoc-style will behave just like regular Qt-
  # style comments (thus requiring an explicit @brief command for a brief
  # description.)
  # The default value is: NO.
  # OVERRIDING DEFAULT: We rely on this instead of explicit @brief -- which is an annoying boiler-plate thing.
  set(DOXYGEN_JAVADOC_AUTOBRIEF YES)
  # If you use STL classes (i.e. std::string, std::vector, etc.) but do not want
  # to include (a tag file for) the STL sources as input, then you should set this
  # tag to YES in order to let doxygen match functions declarations and
  # definitions whose arguments contain STL classes (e.g. func(std::string);
  # versus func(std::string) {}). This also make the inheritance and collaboration
  # diagrams that involve STL classes more complete and accurate.
  # The default value is: NO.
  # OVERRIDING DEFAULT: Looks pretty good!
  set(DOXYGEN_BUILTIN_STL_SUPPORT YES)
  # This WARN_NO_PARAMDOC option can be enabled to get warnings for functions that
  # are documented, but have no documentation for their parameters or return
  # value. If set to NO, doxygen will only warn about wrong parameter
  # documentation, but not about the absence of documentation. If EXTRACT_ALL is
  # set to YES then this flag will automatically be disabled. See also
  # WARN_IF_INCOMPLETE_DOC
  # The default value is: NO.
  # OVERRIDING DEFAULT: We are stringent like that, baby.  -Werror -Wall -Wextra for the win, etc.
  set(DOXYGEN_WARN_NO_PARAMDOC YES)
  # If the HTML_TIMESTAMP tag is set to YES then the footer of each generated HTML
  # page will contain the date and time when the page was generated. Setting this
  # to YES can help to show when doxygen was last run and thus if the
  # documentation is up to date.
  # The default value is: NO.
  # This tag requires that the tag GENERATE_HTML is set to YES.
  # OVERRIDING DEFAULT: Looks pretty good!
  set(DOXYGEN_HTML_TIMESTAMP YES)
  # If set to YES the inheritance and collaboration graphs will hide inheritance
  # and usage relations if the target is undocumented or is not a class.
  # The default value is: YES.
  # OVERRIDING DEFAULT: It's nice to show things like vector<>.
  set(DOXYGEN_HIDE_UNDOC_RELATIONS NO)
  # The DOT_FONTSIZE tag can be used to set the size (in points) of the font of
  # dot graphs.
  # Minimum value: 4, maximum value: 24, default value: 10.
  # This tag requires that the tag HAVE_DOT is set to YES.
  # OVERRIDING DEFAULT: Got used to this look for whatever reason; don't want to mess it up now.
  set(DOXYGEN_DOT_FONTSIZE 8)
  # If the INCLUDE_GRAPH, ENABLE_PREPROCESSING and SEARCH_INCLUDES tags are set to
  # YES then doxygen will generate a graph for each documented file showing the
  # direct and indirect include dependencies of the file with other documented
  # files.
  # The default value is: YES.
  # This tag requires that the tag HAVE_DOT is set to YES.
  # OVERRIDING DEFAULT: Sort of useful in theory, but it's just too insane and
  # WIDE-looking IMO.
  set(DOXYGEN_INCLUDE_GRAPH NO)
  # If the INCLUDED_BY_GRAPH, ENABLE_PREPROCESSING and SEARCH_INCLUDES tags are
  # set to YES then doxygen will generate a graph for each documented file showing
  # the direct and indirect include dependencies of the file with other documented
  # files.
  # The default value is: YES.
  # This tag requires that the tag HAVE_DOT is set to YES.
  # OVERRIDING DEFAULT: See preceding.
  set(DOXYGEN_INCLUDED_BY_GRAPH NO)
  # The DOT_IMAGE_FORMAT tag can be used to set the image format of the images
  # generated by dot. For an explanation of the image formats see the section
  # output formats in the documentation of the dot tool (Graphviz (see:
  # http://www.graphviz.org/)).
  # Note: If you choose svg you need to set HTML_FILE_EXTENSION to xhtml in order
  # to make the SVG files visible in IE 9+ (other browsers do not have this
  # requirement).
  # Possible values are: png, jpg, gif, svg, png:gd, png:gd:gd, png:cairo,
  # png:cairo:gd, png:cairo:cairo, png:cairo:gdiplus, png:gdiplus and
  # png:gdiplus:gdiplus.
  # The default value is: png.
  # This tag requires that the tag HAVE_DOT is set to YES.
  # OVERRIDING DEFAULT: As of this writing the available `dot` in certain settings in certain orgs
  # (observed by ygoldfel within a certain mainstream Akamai standard build env around 2020) in the `graphviz`
  # component is incapable of generating PNGs (probably lacking Cairo or some
  # such; probably fixable but not easily).  SVG works, and the IE problem
  # mentioned above shouldn't come up in practice.
  set(DOXYGEN_DOT_IMAGE_FORMAT svg)
  # The DOT_GRAPH_MAX_NODES tag can be used to set the maximum number of nodes
  # that will be shown in the graph. If the number of nodes in a graph becomes
  # larger than this value, doxygen will truncate the graph, which is visualized
  # by representing a node as a red box. Note that doxygen if the number of direct
  # children of the root node in a graph is already larger than
  # DOT_GRAPH_MAX_NODES then the graph will not be shown at all. Also note that
  # the size of a graph can be further restricted by MAX_DOT_GRAPH_DEPTH.
  # Minimum value: 0, maximum value: 10000, default value: 50.
  # This tag requires that the tag HAVE_DOT is set to YES.
  # OVERRIDING DEFAULT: I changed from 50 to 100 because of a warning that started
  # coming up for a couple of things eventually.
  set(DOXYGEN_DOT_GRAPH_MAX_NODES 100)
  # The MAX_DOT_GRAPH_DEPTH tag can be used to set the maximum depth of the graphs
  # generated by dot. A depth value of 3 means that only nodes reachable from the
  # root by following a path via at most 3 edges will be shown. Nodes that lay
  # further from the root node will be omitted. Note that setting this option to 1
  # or 2 may greatly reduce the computation time needed for large code bases. Also
  # note that the size of a graph can be further restricted by
  # DOT_GRAPH_MAX_NODES. Using a depth of 0 means no depth restriction.
  # Minimum value: 0, maximum value: 1000, default value: 0.
  # This tag requires that the tag HAVE_DOT is set to YES.
  # OVERRIDING DEFAULT: Honestly I (ygoldfel) don't remember why and seem to have failed to comment it
  # at the time.  A lesson to you, kids....  Now, though, let's just let things look how we like.
  set(DOXYGEN_MAX_DOT_GRAPH_DEPTH 4)
  # When the TOC_INCLUDE_HEADINGS tag is set to a non-zero value, all headings up
  # to that level are automatically included in the table of contents, even if
  # they do not have an id attribute.
  # Note: This feature currently applies only to Markdown headings.
  # Minimum value: 0, maximum value: 99, default value: 5.
  # This tag requires that the tag MARKDOWN_SUPPORT is set to YES.
  # OVERRIDING DEFAULT: Oh my GOD! This was such a pain in the ass!  Bear with me.
  # The value 0 is fine.  The value 5 (the default) messes up SO much however.
  # In 1.8.14 it makes warnings appear when heading markup (-------... and ###)
  # is used in any form except that following the strict hierarchy (# containing
  # ## containing ###); and then headings don't show up in the HTML output.
  # In 1.8.15+ (or was it 1.8.16+?) it's even worse; @internal stops working
  # properly at least in any doc header with heading markup. Why? It has
  # something to do with Table-of-Contents generation and auto-including any
  # section hierarchy implied by such heading markup.  If >=1 then it seems to
  # parse the heading markup as more than mere visual markup but as an indication
  # of paragraph hierarchy for the ToC; hence it demands that the proper levels
  # are used without skipping... so like 1.2 doesn't contain 1.2.?.4 directly
  # without a 1.2.<something> in between.  We just use it for visual markup;
  # we don't even generate a ToC at all (I think it's not a thing in HTML output
  # but definitely is for PDF and others)!  Then there's the slight mystery of
  # why `doxygen -u` saw it fit to set this to 5 and not 0; I could probably
  # retrace my steps to figure that out but not worth it.  ANYWAY!!!
  # Now we know, 0 is good.  If you set it to >=1 (if we want to generate PDF
  # at some point or whatever) then will need to change the source to use
  # strict section hierarchy markup to avoid this problem.
  set(DOXYGEN_TOC_INCLUDE_HEADINGS 0)
  # If the MACRO_EXPANSION tag is set to YES, doxygen will expand all macro names
  # in the source code. If set to NO, only conditional compilation will be
  # performed. Macro expansion can be done in a controlled way by setting
  # EXPAND_ONLY_PREDEF to YES.
  # The default value is: NO.
  # This tag requires that the tag ENABLE_PREPROCESSING is set to YES.
  # OVERRIDING DEFAULT:  Needed for TEMPLATE_* and CLASS_* trick used by various ygoldfel code in at least
  # Flow-IPC (but may well spread to Flow or elsewhere); otherwise it warns about non-existent
  # classes TEMPLATE_... and CLASS_... when encountering method bodies CLASS_...::...().
  # In Flow-IPC, at least, see transport/struc/channel.hpp, where a comment goes into this annoying little situation.
  set(DOXYGEN_MACRO_EXPANSION YES)

  # Time to really add the target (invoked via `make ${target}` or equivalent).
  #   - The input dirs are (recursively) to be given via DOC_INPUT; pass that through.  Reminder:
  #     these are to be relative off project root, also the working directory from which Doxygen by CMake.
  #   - It is important to set STRIP_FROM_PATH to those same dirs.  (TODO: Actually arguably it could/should be
  #     the parent.  E.g., for Flow DOC_INPUT = src/flow, so STRIP_FROM_PATH = src/flow but perhaps should be
  #     src; as even though indeed everything is under src/flow, it is
  #     nevertheless exported e.g. to (...include path...)/flow/... and is written as such in `#include <>`s;
  #     so maybe it should be referred to with the flow/ prefix in the generated docs.  On the other hand it's
  #     redundant and lengthier and self-explanatory.  That said if DOC_INPUT features 2+ things *and* those two
  #     things ever don't *all* end in the same sub-dir, then we'll need to change this.
  #     E.g., `src/x/a src/y/a` = cool; but if it becomes `src/x/a src/y/b` ever, then this must be changed.
  #     End of TODO....)
  #
  #     Example with 2+ source dirs and manual: ipc_core/src/ipc ipc_session/src/ipc doc/manual
  #       - Source files in ipc_{core|session}/src/ipc shall be scanned for doc generation.
  #         These trees shall be seen as merged together at the trailing /ipc; in particular
  #         when headers are exported they'll in reality be exported right by each other's side despite
  #         coming from different sub-projects ipc_core and ipc_session.  And everything up to (and including)
  #         the trailing /ipc shall be stripped from any mentions of files; as which projects they come from
  #         is to be ignored by this doc-generation model: we are treating the whole thing as a monolith.
  #       - Manual shall also be scanned (not to contain any actual source, only Doxygen-enabled "comments").
  #
  #   - DOC_EXCLUDE_PATTERNS (can be empty) can supply subtrees and/or files to skip after doing the above
  #     globbage (by Doxygen).  That said we do something extra; if generating public API doc then skip
  #     any and all */detail/* dirs from scanning.  And lastly, either way, we always skip */test/*.
  #   - Lastly if (especially manual) Doxygen comments mention embedded images they can supply that
  #     via DOC_IMAGE_PATH.

  if(DOC_IMAGE_PATH)
    # The IMAGE_PATH tag can be used to specify one or more files or directories
    # that contain images that are to be included in the documentation (see the
    # \image command).
    set(DOXYGEN_IMAGE_PATH ${DOC_IMAGE_PATH})
  endif()

  set(DOXYGEN_STRIP_FROM_PATH ${DOC_INPUT})
  set(DOXYGEN_EXCLUDE_PATTERNS ${DOC_EXCLUDE_PATTERNS} "*/test/*")
  if(NOT ${full_else_public})
    list(APPEND DOXYGEN_EXCLUDE_PATTERNS "*/detail/*")
  endif()

  # Check for a certain annoyance that'd halt is in our tracks.  For example say our include()r does:
  #   set(DOC_INPUT ${IPC_META_ROOT_ipc_core}/src/ipc)
  # but ${IPC_META_ROOT_ipc_core}, which is probably based on where the user's project source lives in their
  # particular file-system, happens to contain "/test/" somewhere.  We happen to always exclude */test/* (see above);
  # so now we've excluded all input files.  If ${DOC_INPUT} has only relative paths, then there's no issue;
  # but sometimes that is not sufficient, and then this niggling issue could occur.  So we just blow up.
  # TODO: Do something more elegant, though it's not immediately apparent what that might be, given Doxygen's
  # semantics on this.
  foreach(doc_exclude_pattern ${DOXYGEN_EXCLUDE_PATTERNS})
    string(REPLACE "*" ".*" doc_exclude_pattern_regex ${doc_exclude_pattern})
    foreach(doc_input ${DOC_INPUT})
      if(${doc_input} MATCHES ${doc_exclude_pattern_regex})
        message(FATAL_ERROR "A Doxygen peculiarity combined with our desire to skip doc-generating from "
                              "[${doc_exclude_pattern}] files sadly means your source locations like "
                              "[${doc_input}] may not contain the former stuff "
                              "anywhere in the path prefix... please rename those.  Sorry.")
      endif()
    endforeach()
  endforeach()

  # Almost-lastly: output subtleties:
  # Doxygen logs to stdout voluminously -- unless QUIET is enabled; then it keeps it cool.
  # If there are warnings it logs them to stderr but does not consider them fatal by default;
  # one can tell to stop at the first warning; or to keep going but fail with bad exit code
  # (which `make` or equivalent will show quite clearly, when it occurs).
  # One can also specify a warn-log file instead (but no way to put progress to stdout via config option).
  #
  # We do want warnings to be very visible.  Moreover we want 1+ warnings to be fatal (consistent with our
  # stringest standards for our code when building).  However the fail-on-first-warning mode is too tedious
  # to debug with in our experience.  Meanwhile we could take or leave the voluminous progress messages;
  # but if warnings appear they are lost horribly in that case.  Long story short... with all these available
  # knobs, and the fact we can't (easily) control what `make doc_*` will exactly invoke as the Doxygen command
  # (meaning must use the config knobs), the best combo appears to be the following:
  #   - Keep the progress output to a minimum (essentially nothing on stdout).
  #   - Show all warnings (don't stop on 1st warning).
  #   - Return failure, if there's 1+ warnings (`make` or equivalent will make that quite clear).
  set(DOXYGEN_QUIET YES)
  set(DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)

  # And lastly:
  set(DOXYGEN_HTML_OUTPUT "html_${target}")

  doxygen_add_docs(${target}
                   ${DOC_INPUT}

                   # Re-run if and only if last-generated docs older than input-source.
                   #   USE_STAMP_FILE
                   # Update: Decided not to (commented out); reason being: based on the CMake docs in this mode
                   # one must provide in the input-file list above all the individual files -- no dirs allowed.
                   # Generating-if-needed-only would be nice (a pre-open-source/CMake version of all this by ygoldfel
                   # in the past did do this, via manual `find`+time-stamp craziness); but really in practice
                   # the dev should know pretty well when they really need to regenerate; and if they forgot then
                   # they can always just do it (better safe than sorry mentality).
                   # TODO: Revisit this.  The actual build targets specify the exact files that are built; or
                   # perhaps we could do a glob-search (but be careful; in other contexts at least CMake docs
                   # discourage this for incremental-build reasons).  No approach is particularly easy, but with
                   # some though it could be done (USE_STAMP_FILE could eventually be enabled).  Whether it's
                   # worth it is a question however.

                   COMMENT ${comment})

  list(POP_BACK CMAKE_MESSAGE_INDENT)
  message(CHECK_PASS "(Done.)")
endfunction()

doc_generate(TRUE)
doc_generate(FALSE)

list(POP_BACK CMAKE_MESSAGE_INDENT)
message(CHECK_PASS "(Done.)")
