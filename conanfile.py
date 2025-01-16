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

from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout, CMakeDeps, CMakeToolchain

def load_version_from_file():
    version_path = './VERSION'
    with open(version_path, 'r') as version_file:
        # Read the entire file content and strip whitespace (matches what FlowLikeProject.cmake does).
        version = version_file.read().strip()
    return version

class FlowRecipe(ConanFile):
    name = "flow"
    version = load_version_from_file()
    settings = "os", "compiler", "build_type", "arch"

    DOXYGEN_VERSION = "1.9.4"

    options = {
        "build": [True, False],
        "build_no_lto": [True, False],

        # 0 => default (let build script decide, as of this writing 17 meaning C++17) or a #, probably `20` as of
        # this writing.
        "build_cxx_std": ["ANY"],

        "doc": [True, False]
    }

    default_options = {
        "build": True,
        "build_no_lto": False,
        "build_cxx_std": 0,
        "doc": False
    }

    def configure(self):
        if self.options.build:
            # Currently need all headers;
            # plus libs: chrono, filesystem, program_options, thread, timer (and all headers).
            # `filesystem` requires atomic.  `thread` requires container, date_time, exception.
            # (Boost provides the with_* way of specifying it also; the Conan Boost pkg only has without_*.)
            self.options["boost"].without_charconv = True
            self.options["boost"].without_cobalt = True
            # TODO: The next line is commented out with the upgrade from 1.84 to 1.87; otherwise this happened
            # when building Boost: ConanException:
            #                      These libraries were built, but were not used in any boost module: {'boost_context'}
            # Unclear what's up exactly; maybe some module we *do* need now requires `context` to itself build.
            # Whatever... this gets rid of the problem.  Look into it sometime though.
            # self.options["boost"].without_context = True
            self.options["boost"].without_contract = True
            self.options["boost"].without_coroutine = True
            self.options["boost"].without_fiber = True
            self.options["boost"].without_graph = True
            self.options["boost"].without_graph_parallel = True
            self.options["boost"].without_iostreams = True
            self.options["boost"].without_json = True
            self.options["boost"].without_locale = True
            self.options["boost"].without_log = True
            self.options["boost"].without_math = True
            self.options["boost"].without_mpi = True
            self.options["boost"].without_nowide = True
            self.options["boost"].without_python = True
            self.options["boost"].without_random = True
            self.options["boost"].without_regex = True
            self.options["boost"].without_serialization = True
            self.options["boost"].without_stacktrace = True
            self.options["boost"].without_test = True
            self.options["boost"].without_type_erasure = True
            self.options["boost"].without_url = True
            self.options["boost"].without_wave = True

    def generate(self):
        cmake = CMakeDeps(self)
        if self.options.doc:
            cmake.build_context_activated = [f"doxygen/{self.DOXYGEN_VERSION}"]
        cmake.generate()

        toolchain = CMakeToolchain(self)
        if self.options.build:
            if self.options.build_no_lto:
                toolchain.variables["CFG_NO_LTO"] = "ON"
            if self.options.build_cxx_std != 0:
                toolchain.variables["CMAKE_CXX_STANDARD"] = self.options.build_cxx_std
        else:
            toolchain.variables["CFG_SKIP_CODE_GEN"] = "ON"
        if self.options.doc:
            toolchain.variables["CFG_ENABLE_DOC_GEN"] = "ON"
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()

        # Cannot use cmake.build(...) because not possible to pass make arguments like --keep-going.
        if self.options.build:
            self.run("cmake --build . -- --keep-going VERBOSE=1")
        if self.options.doc:
            self.run("cmake --build . -- flow_doc_public flow_doc_full --keep-going VERBOSE=1")

    def requirements(self):
        if self.options.build:
            # Attention: This version of Boost is not unfortunately as of this writing (1/14/2025) not
            # in conan-center; 1.86 is the latest version in conan-center, with 1.87 PR
            # (https://github.com/conan-io/conan-center-index/pull/26079) still open
            # since 1.87's release ~1 month earlier.  We try to get it manually for now, which is why the "@"
            # is at the end there.  To be clear, "it" is just the recipe (which wrangles Boost), not Boost
            # itself.  To make this work:
            #   - We grabbed the contents of the PR and placed the Boost-relevant parts (a handful of files
            #     under recipes/boost) directly in ./conan dir.
            #   - The pipeline .yml in .github, when wrangling Conan, installs that recipe manually.
            #   - Then the "@" here gets the desired Boost based on that locally-obtained recipe.
            # TODO: Surely soon enough the version will be in conan-center, at which point:
            #   - Remove the "@" here.
            #   - Remove the `conan expert` + related command(s) from the relevant `.yml`s.
            self.requires("boost/1.87.0@")
            #self.requires("boost/1.84.0")

            self.requires("fmt/10.0.0")

    def build_requirements(self):
        self.tool_requires("cmake/3.26.3")
        if self.options.doc:
            self.tool_requires(f"doxygen/{self.DOXYGEN_VERSION}")

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def layout(self):
        cmake_layout(self)


    def layout(self):
        cmake_layout(self)
