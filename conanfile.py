from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeDeps

class FlowRecipe(ConanFile):
    name = "flow"
    
    settings = "os", "compiler", "build_type", "arch"
    
    generators = (
        "CMakeToolchain"
    )

    tool_requires = (
        "cmake/3.26.3", 
    )

    options = {
        "build": [True, False], 
        "doc": [True, False]
    }
    
    default_options = {
        "build": True, 
        "doc": False
    }
    
    def configure(self):
        if self.options.build:
            # Currently need all headers;
            # plus libs: chrono, filesystem, program_options, thread, timer (and all headers).
            # `filesystem` requires atomic.  `thread` requires container, date_time, exception.
            # (Boost provides the with_* way of specifying it also; the Conan Boost pkg only has without_*.)
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

    def requirements(self):
        if self.options.build:
            self.requires("boost/1.83.0")
            self.requires("capnproto/1.0.1")
    
    def build_requirements(self):
        if self.options.doc:
            self.tool_requires("doxygen/1.9.4")
            
    def layout(self):
        cmake_layout(self)

    def generate(self):
        cmake = CMakeDeps(self)
        if self.options.doc:
            cmake.build_context_activated = ["doxygen/1.9.4"]
        cmake.generate()
