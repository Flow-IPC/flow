from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeDeps

class FlowRecipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = (
        "CMakeToolchain"
    )

    requires = (
        "boost/1.83.0",
        "capnproto/1.0.1",
    )

    tool_requires = (
        "cmake/3.26.3", 
        "doxygen/1.9.4"
    )

    def configure(self):
        self.options["boost"].without_context = True
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
    
    def layout(self):
        cmake_layout(self)

    def generate(self):
        cmake = CMakeDeps(self)
        cmake.build_context_activated = ["doxygen/1.9.4"]
        cmake.generate()
