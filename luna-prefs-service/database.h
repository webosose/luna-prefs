// Copyright (c) 2015-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef DATABASE_H
#define DATABASE_H

#include <glib.h>
#include <sys/types.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <syslog.h>

const gchar* setBackupFile(const gchar* tempDir);
const gchar* getBackupFile();
void         create_backup(gpointer file, gpointer data);
bool         create_prefs_backup();
void         free_list_and_data(GList * list);
void         restore_prefs_data(gchar* key, gchar* value, bool set_flag);
bool         restore_action(const gchar* path);
bool         begin_restore(const gchar* db_file);
bool         try_restore(const gchar* db_file);

#endif
