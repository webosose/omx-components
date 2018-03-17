# Copyright (c) 2017-2018 LG Electronics, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 2.8.7)
project(omx-components)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC7)
webos_component(1 0 0)
webos_add_compiler_flags(ALL -std=c++11 -Wall -Wextra -fno-permissive)
#disable specific warnings
webos_add_compiler_flags(ALL -Wno-unused-parameter -Wno-type-limits -Wno-comment)
#promote specific warnings to errors
webos_add_compiler_flags(ALL -Werror=return-type  -Werror=reorder -Werror=uninitialized -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -DTARGET_LINUX -fPIC -DPIC -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D__VIDEOCORE4__ -DHAVE_OMXLIB -DFORTIFY_SOURCE -DOMX -DOMX_SKIP64BIT -ftree-vectorize -DUSE_EXTERNAL_OMX -DTARGET_RASPBERRY_PI -DUSE_EXTERNAL_LIBBCM_HOST )
# Require that all undefined symbols are satisfied by the libraries from target_link_libraries()
webos_add_linker_options(ALL --no-undefined)
webos_add_linker_options(ALL --hash-style=gnu --as-needed --as-needed)


include(FindPkgConfig)

#pkg_check_modules(OMXCOMPONENTS REQUIRED omx-components)
#include_directories(${OMXCOMPONENTS_INCLUDE_DIRS})
#webos_add_compiler_flags(ALL ${OMXCOMPONENTS})
pkg_check_modules(OPENMAXIL REQUIRED openmaxil)
include_directories(${OPENMAXIL_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${OPENMAXIL})

pkg_check_modules(PMLOG REQUIRED PmLogLib)
include_directories(${PMLOG_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${PMLOG_CFLAGS_OTHER})

pkg_check_modules(PBNJSON_CPP REQUIRED pbnjson_cpp)
include_directories(${PBNJSON_CPP_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${PBNJSON_CPP_CFLAGS_OTHER})


pkg_check_modules(LIBDRM REQUIRED libdrm)
include_directories(${LIBDRM_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${LIBDRM})

pkg_check_modules(LIBSWRESAMPLE REQUIRED libswresample)
include_directories(${LIBSWRESAMPLE_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${LIBSWRESAMPLE})

pkg_check_modules(LIBAVUTIL REQUIRED libavutil)
include_directories(${LIBAVUTIL_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${LIBAVUTIL})

include_directories(${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/common ${PROJECT_SOURCE_DIR}/src/omxalsa ${PROJECT_SOURCE_DIR}/src/omxdrm ${PROJECT_SOURCE_DIR}/include/public)
file(GLOB SOURCES src/*.cpp src/omxdrm/*.cpp src/common/*.cpp src/omxalsa/*.cpp)

#add local include directories, accessible only by double quotes include.
webos_add_compiler_flags(ALL -iquote ${CMAKE_CURRENT_BINARY_DIR}/Configured/files/conf)
webos_add_compiler_flags(ALL -iquote ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--export-dynamic")
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--no-undefined")

set(CMAKE_SHARED_MODULE_PREFIX "")
set(NO_SONAME)

add_library(omxcomponents SHARED ${SOURCES})
target_link_libraries(omxcomponents
        ${PMLOG_LDFLAGS}
        ${PBNJSON_CPP_LDFLAGS}
        pthread
        drm
        asound
        swresample
        ${LIBAVUTIL_LDFLAGS}
        )

webos_build_library(TARGET omxcomponents)


install(TARGETS omxcomponents DESTINATION ${WEBOS_INSTALL_LIBDIR})

webos_configure_source_files(SOURCES files/conf/config.h)

webos_build_pkgconfig(files/pkgconfig/omxcomponents)
