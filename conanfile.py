import shutil
import json

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.microsoft import msvc_runtime_flag, is_msvc
from conan.tools.scm import Version, Git
from conan.tools.files import get, patch, rmdir, update_conandata
from conan.tools.build import check_min_cppstd
from conan.tools.meson import MesonToolchain
from conan.tools.gnu import PkgConfigDeps
from collections import namedtuple
import functools
import os, re, textwrap
import sys

required_conan_version = ">=2.0.0"

class CameraConan(ConanFile):
    name = "camera"
    # TODO: a way to retrieve repo version
    # version = 
    user = "ocean"
    channel = "releases"
    url = "https://github.com/tianyi-lu-ocean/libcamera.git"   # e.g., libFDCommon
    homepage = "https://www.oceaninsight.com/"
    topics = ("internal", "ocean", "applied-systems", "libcamera")
    license = "<Put the license here!>"
    description = (
        "A forked version of libcamera from raspberrypi with conan recipe"
    )
    # TODO: figure out what kinds of generator toolchain we need
    # generators = "CMakeDeps", "CMakeToolchain"
    settings = "os", "arch", "build_type", "compiler"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }

    @property
    def _min_cppstd(self):
        return "23" # e.g., "23" -> C++23

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def requirements(self):
        self.requires("openssl/3.3.2")
        self.requires("gnutls/3.8.7")
        # List all requirements of this library (except system libraries, which are specified in package_info())
        # e.g., self.requires("fdcommon/7.0.0@ocean/releases")
        # Set transitive_headers=True if this library includes a requirement's headers in its own public headers
        # e.g., self.requires("fdcommon/7.0.0@ocean/releases", transitive_headers=True)
        # <TEMPLATE_CP>

    # def build_requirements(self):
    #     self.tool_requires("cmake/3.28.1")
    #     self.test_requires("catch2/3.5.1")

    def generate(self):
        tc = MesonToolchain(self)
        tc.generate()
        # since we have some dependencies, the MesonToolchain only works with PkgConfigDeps 
        pc = PkgConfigDeps(self)
        pc.generate()
        # TODO: might need to add additional project_options or subproject_options


    def layout(self):
        self.folders.generators = "build"
        self.folders.build = "build"

    def source(self):
        # Retrieve SCM URL + commit ID from conandata.yml (produced by export())
        git = Git(self)
        sources = self.conan_data["sources"]
        self.output.info(f"Cloning sources from: {sources}")
        git.clone(url=sources["url"], target=".")
        git.checkout(commit=sources["commit"])

    def export(self):
        git = Git(self, self.recipe_folder)
        # Requires that local Git repository has a remote called 'origin' which contains the latest commit
        scm_url, scm_commit = git.get_url_and_commit(remote="origin")
        self.output.info(f"Obtained URL: {scm_url} and {scm_commit}")
        # print the scm_url and scm_commit as json format, output to the stdout
        print(json.dumps({"scm_url": scm_url, "scm_commit": scm_commit}, indent=4), file=sys.stdout)
        # Write the current URL + commmit ID to local conandata.yml file
        update_conandata(self, {"sources": {"commit": scm_commit, "url": scm_url}})

    def validate(self):
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, self._min_cppstd)

    def build(self):
        pass
    
    def package(self):
        pass
        # cmake = CMake(self)
        # cmake.install()

    def package_info(self):
        pass
        # lib_name = self.name
        # if self.settings.build_type == "Debug":
        #     lib_name += "-d"
        # self.cpp_info.libs = [lib_name]
        
        # # Prepend CMake namespace to imported target name for consumers of this library
        # # The CMake namespace is useful because CMake will see it and know this is an imported target, and it can provide better diagnostic messages
        # # https://cmake.org/cmake/help/latest/manual/cmake-developer.7.html
        # self.cpp_info.set_property("cmake_target_name", "ocean::" + self.name)
        # if self.options.shared:
        #     self.cpp_info.defines = [self.name.upper() + "_SHARED_LIB"]
