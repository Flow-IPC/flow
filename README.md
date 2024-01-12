# Flow  -- Modern C++ toolkit for async loops, logs, config, benchmarking, and more

C++ power users, these days, are likely to use the standard library (a/k/a STL), Boost, and/or any number of
third-party libraries.  Nevertheless every large project or organization tends to need more reusable goodies,
whether to add to STL/Boost/etc. or in some cases do something better, perhaps in a specialized way.

Flow is such a library (provided as both headers and an actual library).  It's written in modern C++ (C++ 17
as of this writing) and is meant to be generally usable as opposed to particularly specialized.
(One exception to this is the included, but wholly optional, NetFlow protocol, contained in `flow::net_flow`
namespace.  While still reusable in a general way, interest in this functionality is likely niche.)

We refrain from delving into any particulars as to what's in Flow, aside from the following brief list of
its top-level modules.  The documentation (see Documentation below) covers all of its contents in great detail.
So, that said, Flow includes (alphabetically ordered):
  - `flow::async`: Single-threaded and multi-threaded event loops, augmenting boost.asio so as to actually create
    boost.asio-powered threads and thread pools, schedule timers more easily, and other niceties.
  - `flow::cfg`: Key-value configuration file parsing, augmenting a boost.program_options core with a large number of
    quality-of life additions including support for high-speed dynamically updated configuration `struct`s.
  - `flow::error`: A few niceties around the ubiquitous boost.system (now adopted by STL also) error-reporting
    system.  It also adds a simple convention for reporting errors, used across Flow, wherein each error-reporting
    API handles reporting results via error code return or exception (whichever the caller prefers at the call-site).
  - `flow::log`: A powerful and performant logging system.  While it can actually output to console and files,
    if so configured, its loggers will also easily integrate with your own log system of choice.
  - `flow::perf`: Benchmarking with multiple clock types and checkpoint-based accounting (if desired).
  - `flow::util`: General goodies.  Highlights: `Blob` (efficient and controlled `vector<uint8_t>` replacement with
    optional sharing and mem-pools), `Linked_hash_{map|set}` (hashed-lookup containers that maintain MRU-LRU iterator
    ordering as opposed to being unordered as in `unordered_{map|set}`), `String_ostream` (a sped-up `ostringstream`
    replacement leveraging boost.iostreams internally) plus `ostream_op_string()` function.

## Installation

An exported Flow consists of C++ header files installed under "flow/..." in the include-root; and a
library such as `libflow.a`.  Certain items are also exported for people who use CMake to build their own
projects; we make it particularly easy to use Flow in that case (`find_package(Flow)`).  Lastly documentation
can be optionally generated.

The basic prerequisites for *building* the above:

  - Linux;
  - a C++ compiler with C++ 17 support;
  - Boost headers (plus certain libraries) install;
  - CMake;
  - (optional, only if generating docs) Doxygen and Graphviz.

The basic prerequisites for *using* the above:

  - Linux, C++ compiler, Boost (but CMake is not required); plus:
  - your source code `#include`ing any exported Flow headers must be itself built in C++ 17 mode;
  - any executable using the Flow library must be linked with certain Boost and ubiquitous system libraries.

We intentionally omit version numbers and even specific compiler types in the above description; the CMake run
should help you with that.

To build Flow:

  1. Ensure a Boost install is available.  If you don't have one, please install the latest version at
     [boost.org](https://boost.org).  If you do have one, try using that one (our build will complain if insufficient).
     (From this point on, that's the recommended tactic to use when deciding on the version number for any given
     prerequisite.  E.g., same deal with CMake in step 2.)
  2. Ensure a CMake install is available (available at [CMake web site](https://cmake.org/download/) if needed).
  3. (Optional, only if generating docs) Have Doxygen and Graphviz installs available.
  4. Use CMake `cmake` (command-line tool) or `ccmake` (interactive text-UI tool) to configure and generate
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
      - Link against libflow.a.
      - Link against Boost libraries mentioned in a `CMakeLists.txt` line (search `$SRC` for it):
        `set(BOOST_LIBS ...)`.
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

In contrast, if you have changed the source code (for Flow-IPC proper, Flow, or both): See Contributing below.
It includes instructions on how docs are generated and updated.  Spoiler alert: Most things are automatic;
the only manual part is that you should peruse any changed docs for your visual satisfaction before
submitting your code.

## Contributing

See Flow-IPC meta-project's `README.md` Contributing section.
