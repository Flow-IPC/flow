# Installing the software

## Installation

An exported Flow consists of C++ header files installed under "flow/..." in the include-root; and a
library such as `libflow.a`.  Certain items are also exported for people who use CMake to build their own
projects; we make it particularly easy to use Flow in that case (`find_package(Flow)`).  Lastly documentation
is included in the source tree for local perusal; and can be optionally re-generated from local source (more
on that below).

The basic prerequisites for *building* the above:

  - Linux;
  - a C++ compiler with C++ 17 support;
  - Boost headers (plus certain libraries) install;
  - {fmt} install;
  - CMake;
  - (optional, only if generating docs) Doxygen and Graphviz.

The basic prerequisites for *using* the above:

  - Linux, C++ compiler, Boost, {fmt} (but CMake is not required); plus:
  - your source code `#include`ing any exported Flow headers must be itself built in C++ 17 mode;
  - any executable using the Flow library must be linked with certain Boost and ubiquitous system libraries.

We intentionally omit version numbers and even specific compiler types in the above description; the CMake run
should help you with that.

To build Flow:

  1. Ensure a Boost install is available.  If you don't have one, please install the latest version at
     [boost.org](https://boost.org).  If you do have one, try using that one (our build will complain if insufficient).
     (From this point on, that's the recommended tactic to use when deciding on the version number for any given
     prerequisite.  E.g., same deal with CMake in step 2.)
  2. Ensure a {fmt} install is available (available at [{fmt} web site](https://fmt.dev/) if needed).
  3. Ensure a CMake install is available (available at [CMake web site](https://cmake.org/download/) if needed).
  4. (Optional, only if generating docs) Have Doxygen and Graphviz installs available.
  5. Use CMake `cmake` (command-line tool) or `ccmake` (interactive text-UI tool) to configure and generate
     a build system (namely a GNU-make `Makefile` and friends).  Details on using CMake are outside our scope here;
     but the basics are as follows.  CMake is very flexible and powerful; we've tried not to mess with that principle
     in our build script(s).
     1. Choose a tool.  `ccmake` will allow you to interactively configure aspects of the build system, including
        showing docs for various knobs our CMakeLists.txt (and friends) have made availale.  `cmake` will do so without
        asking questions; you'll need to provide all required inputs on the command line.  Let's assume `cmake` below,
        but you can use whichever makes sense for you.
     2. Choose a working *build directory*, somewhere outside the present `flow` distribution.  Let's call this
        `$BUILD`: please `mkdir -p $BUILD`.  Below we'll refer to the directory containing the present `README.md` file
        as `$SRC`.
     3. Configure/generate the build system.  The basic command line:
        `cd $BUILD && cmake -DCMAKE_INSTALL_PREFIX=... -DCMAKE_BUILD_TYPE=... -DCFG_ENABLE_DOC_GEN=ON $SRC`,
        where `$CMAKE_INSTALL_PREFIX/{include|lib|...}` will be the export location for headers/library/goodies;
        `CMAKE_BUILD_TYPE={Release|RelWithDebInfo|RelMinSize|Debug|}` specifies build config;
        `CFG_ENABLE_DOC_GEN` makes it possible to locally generate documentation (if so desired).
        More options are available -- `CMAKE_*` for CMake ones; `CFG_*` for Flow ones -- and can be viewed with
        `ccmake` or by glancing at `$SRC/CMakeCache.txt` after running `cmake` or `ccmake`.
        - Regarding `CMAKE_BUILD_TYPE`, you can use the empty "" type to supply
          your own compile/link flags, such as if your organization or product has a standard set suitable for your
          situation.  With the non-blank types we'll take CMake's sensible defaults -- which you can override
          as well.  (See CMake docs; and/or a shortcut is checking out `$SRC/CMakeCache.txt`.)
        - This is the likeliest stage at which CMake would detect lacking dependencies.  See CMake docs for
          how to tweak its robust dependency-searching behavior; but generally if it's not in a global system
          location, or not in the `CMAKE_INSTALL_PREFIX` (export) location itself, then you can provide more
          search locations by adding a semicolon-separated list thereof via `-DCMAKE_PREFIX_PATH=...`.
        - Alternatively most things' locations can be individually specified via `..._DIR` settings.
     4. Build using the build system generated in the preceding step:  In `$BUILD` run `make`.  
        - (To generate documentation run `make flow_doc_public flow_doc_full`.)
     5. Install (export):  In `$BUILD` run `make install`.  

To use Flow:

  - `#include` the relevant exported header(s).
  - Link the exported flow library (such as `libflow.a`) and the required other libraries to your executable(s).
    - If using CMake to build such executable(s):
      1. Simply use `find_package(Flow)` to find it.
      2. Then use `target_link_libraries(... Flow::flow)` on your target to ensure all necessary libraries are linked.
         (This will include the Flow library itself and the dependency libraries it needs to avoid undefined-reference
         errors when linking.)
    - Otherwise specify it manually based on your build system of choice (if any).  To wit, in order:
      - Link against `libflow.a`.
      - Link against Boost libraries mentioned in a `CMakeLists.txt` line (search `$SRC` for it):
        `set(BOOST_LIBS ...)`.
      - Link against the {fmt} library, `libfmt`.
      - Link against the system pthreads library.
  - Read the documentation to learn how to use Flow's various features.  (See Documentation below.)

## Documentation

The documentation consists of:
  - (minor) this README;
  - (minor) comments, about the build, in `CMakeLists.txt`, `*.cmake`, `conanfile.py` (in various directories
    including this one where the top-level `CMakeLists.txt` lives);
  - (major/main) documentation directly in the comments throughout the source code; these have been,
    and can be again, conviently generated using certain tools (namely Doxygen and friends), via the
    above-shown `make flow_doc_public flow_doc_full` command.
    - Seeing the doc comments in the various .hpp files works.
    - Browsing the clickable generated documentation is probably quite a bit nicer.

To read the latter -- the Reference -- consider the following.
  - The latest generated docs from the source code in `*/src/*` has been included nearby:
    Use a browser to open `doc/flow_doc/index.html`.
  - Or see the online documentation at the [project web site](https://flow-ipc.github.io).  This will just mirror what
    the above from the corresponding source code: the tip of the master branch; and for each released version of
    the project.
  - If you're perusing docs only, that's all.  You're done!

In contrast, if you have changed the source code (for Flow-IPC proper, Flow, or both):  See
[CONTRIBUTING](./CONTRIBUTING.md) file.
It includes instructions on how docs are generated and updated.  Spoiler alert: Most things are automatic;
the only manual part is that you should peruse any changed docs for your visual satisfaction before
submitting your code.
