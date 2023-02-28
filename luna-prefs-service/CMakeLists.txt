# Copyright (c) 2012-2023 LG Electronics, Inc.
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

project(luna-prefs-service C)

# -- add local include paths
include_directories(../include/)

# -- check for glib 2.0
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
include_directories(${GLIB2_CFLAGS})
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})

# -- check for json-c
pkg_check_modules(JSON REQUIRED json-c)
include_directories(${JSON_CFLAGS})
webos_add_compiler_flags(ALL ${JSON_CFLAGS})

# -- check for LS2 (internally depends on pmloglib)
pkg_check_modules(LS2 REQUIRED luna-service2)
include_directories(LS2_INCLUDE_DIRS)
webos_add_compiler_flags(ALL ${LS2_CFLAGS})

# -- no way to disable warn_unused_result right now.
webos_add_compiler_flags(ALL -g -O3 -Wall -Wno-unused-but-set-variable -Wno-unused-variable -fno-exceptions)
webos_add_linker_options(ALL --no-undefined)

add_executable(luna-prefs-service main.c database.c accesschecker.c)
target_link_libraries(luna-prefs-service
                      ${GLIB2_LDFLAGS}
                      ${JSON_LDFLAGS}
                      ${LS2_LDFLAGS}
                      luna-prefs
                      )

install(TARGETS luna-prefs-service
        DESTINATION bin
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE)
