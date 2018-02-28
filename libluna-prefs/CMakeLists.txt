# Copyright (c) 2012-2018 LG Electronics, Inc.
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

project(luna-prefs C)

include_directories(../include/)

# -- check for glib 2.0
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
include_directories(${GLIB2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})

# -- check for cjson
pkg_check_modules(CJSON REQUIRED cjson)
include_directories(${CJSON_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${CJSON_CFLAGS})

# -- check for sqlite 3.0
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
include_directories(${SQLITE3_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${SQLITE3_CFLAGS})

#-- check for NYX
pkg_check_modules(NYXLIB REQUIRED nyx)
include_directories(${NYXLIB_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${NYXLIB_CFLAGS_OTHER})

webos_add_compiler_flags(ALL -g -O3 -Wall -pthread)
webos_add_linker_options(ALL --no-undefined)

add_library(luna-prefs SHARED lunaprefs.c)
target_link_libraries(luna-prefs
                      ${GLIB2_LDFLAGS}
                      ${CJSON_LDFLAGS}
                      ${SQLITE3_LDFLAGS}
                      ${NYXLIB_LDFLAGS}
                      )

webos_build_pkgconfig()
webos_build_library(NOHEADERS)
