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

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "database.h"

const char* backup_db_file = "/var/preferences/lunaprefs_backup.db";
const char* prefs_dir = "/var/preferences";
const char* s_temp_backup_DbFilename = "lunaprefs_backup.db";

static gchar* backup_db_path = NULL;

static sqlite3_stmt* backup_statement = NULL;
static sqlite3_stmt* prefs_db_statement = NULL;
static sqlite3_stmt* individual_db_statement = NULL;
static sqlite3_stmt* count_stmt = NULL;

static sqlite3* backUpDb = NULL;
static sqlite3* readDb = NULL;
static sqlite3* restoreDb = NULL;

static bool exec_command(sqlite3* db, const char* command);

/**
 * Create and store backup database full file name
 *
 * @param tempDir Directory of backup database file allocation.
 *                If NULL - use default value.
 *
 * @return Backup database full file name.
 */
const gchar* setBackupFile(const gchar* tempDir)
{
    g_free (backup_db_path);
    backup_db_path = tempDir ? g_build_filename(tempDir, s_temp_backup_DbFilename, NULL)
                             : g_strdup (backup_db_file);
    return backup_db_path;
}

/**
 * Get stored backup database full file name
 *
 * @return Stored by setBackUpFile function backup database full file name
 *         If setBackUpFile not been used, set and return default value.
 */
const gchar* getBackupFile()
{
    if (!backup_db_path)
    {
        setBackupFile(NULL);
    }
    return backup_db_path;
}

static bool open_database(const char* file, sqlite3** db)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    return SQLITE_OK == sqlite3_open(file, db);
}

static bool close_database(sqlite3* db)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    int error_code;
    error_code = sqlite3_close(db);
    syslog(LOG_DEBUG, "error_code %d", error_code );
    return (SQLITE_OK == error_code);
}

static bool prepare_statement(sqlite3* db, sqlite3_stmt** statement, const char* command)
{
    int errCode;
    syslog(LOG_DEBUG, "%s", __func__ );
    errCode = sqlite3_prepare_v2(db, command, -1, statement, 0);
    syslog(LOG_DEBUG, "ErrorCode = %d", errCode );
    if (errCode != SQLITE_OK)
    {
        syslog(LOG_DEBUG, "Failed to prepare [statement] : %s", command);
        *statement = NULL;
        return false;
    }
    return true;
}

static void finalize_statement(sqlite3_stmt** statement)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    if (*statement)
    {
        sqlite3_finalize(*statement);
        *statement = NULL;
    }
}


static int64_t backup_action(const char* path, const char* key, const char* value)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    if( !path || !key || !value )
        return 0;

    sqlite3_reset(backup_statement);
    sqlite3_bind_text( backup_statement, 1, path , -1, SQLITE_TRANSIENT);
    sqlite3_bind_text( backup_statement, 2, key , -1, SQLITE_TRANSIENT);
    sqlite3_bind_text( backup_statement, 3, value , -1, SQLITE_TRANSIENT);

    int ret = sqlite3_step(backup_statement);
    if (ret != SQLITE_DONE)
    {
        syslog(LOG_DEBUG, "Failed sqlite3_step(backup_statement)");
        return 0;
    }
    return sqlite3_last_insert_rowid(backUpDb);
}

static bool setup_database()
{
    syslog(LOG_DEBUG, "%s", __func__ );
    if (!exec_command(backUpDb, "create table if not exists lunaPrefs_backup ("
            "appPath string,"
            "key string, "
            "value string,"
            "PRIMARY KEY (key)"")"))
    {
        syslog(LOG_DEBUG, "Failed to create lunaPrefs_backup table");
        return false;
    }

    if (!prepare_statement(backUpDb, &backup_statement,
        "insert into lunaPrefs_backup (appPath, key, value) values (?,?,?)"))
    {
        syslog(LOG_DEBUG, "Failed to prepare insert statement");
        return false;
    }
    return true;
}

static bool exec_command(sqlite3* db, const char* command)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    return sqlite3_exec(db, command, NULL, 0, 0) == SQLITE_OK;
}

static GList* make_list(const char* path)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    if(!path ) return NULL;

    GList *db_files = NULL;

    GDir*        dir  = NULL;
    GError*      err  = NULL;
    const gchar* file = NULL;
    gchar*       full_path;

    dir = g_dir_open(path, 0, &err);
    if( dir == NULL) {
        syslog(LOG_DEBUG, "g_dir_open(%s) failed - %s", path, err->message );
        g_error_free (err);
    } else {
        while( ( file = g_dir_read_name(dir) ) )
        {
            full_path = g_build_filename(path, file, "prefsDB.sl", (gchar*)NULL);
            if( g_file_test(full_path, G_FILE_TEST_EXISTS ) )
            {
                db_files = g_list_append(db_files, full_path );
                syslog(LOG_DEBUG, "adding to list  %s", full_path);
            }
        }
        g_dir_close( dir );
    }
    return db_files;
}

void create_backup(gpointer file, gpointer data)
{
    syslog(LOG_DEBUG, "%s db_path = %s ", __func__, (char*)file );
    gchar* db_path = (gchar*) file;

    gchar* key_copy;
    gchar* value_copy;

    int step_result;
    if( ! db_path )
    {
        syslog(LOG_ERR, "Invalid database path" );
        return;
    }

    if( !open_database(db_path, &readDb) )
    {
        syslog(LOG_ERR, "Failed to open database" );
        return;
    }

    syslog(LOG_DEBUG, "%s db_path = %s file", db_path , (char*)file );

    if (!prepare_statement(readDb, &prefs_db_statement,
        "select key, value from data"))
    {
        return;
    }

    step_result = sqlite3_step(prefs_db_statement);
    while(step_result == SQLITE_ROW )
    {
        key_copy  = (gchar*)sqlite3_column_text(prefs_db_statement, 0 );
        value_copy= (gchar*)sqlite3_column_text(prefs_db_statement, 1 );

        syslog(LOG_DEBUG, "path : %s, key : %s, value : %s", db_path, key_copy, value_copy );

        if( ! backup_action( db_path , key_copy, value_copy ) )
        {
            syslog(LOG_ERR, "backup_action() Failed");
        }
        step_result = sqlite3_step(prefs_db_statement);
    }

    finalize_statement(&prefs_db_statement);
    if( !close_database(readDb))
    {
        syslog(LOG_DEBUG, "Failed to close database");
    }
}

static bool read_and_backup_list(GList* db_files, const gchar* abs_temp_path)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    if ( !open_database( abs_temp_path , &backUpDb ) )
    {
        syslog(LOG_ERR, "Failed to open database" );
        return false;
    }

    setup_database();
    if( !exec_command(backUpDb, "begin immediate transaction") )
      return FALSE;
    g_list_foreach(db_files, create_backup, NULL);
    exec_command(backUpDb, "commit");
    finalize_statement(&backup_statement);
    if( !close_database(backUpDb) )
    {
        syslog(LOG_DEBUG, "Failed to close database");
       return false;
    }
    return true;
}

/**
 * Create backup database file
 *
 * Create new backup database file and save in it data from prefs databases
 *
 * @return Return true if success, false otherwise.
 */
bool create_prefs_backup()
{
    const gchar* abs_temp_path = getBackupFile();

    // Delete old backup database file
    unlink(abs_temp_path);

    // make prefs database files list
    GList* db_files = make_list(prefs_dir);

    // make backup
    bool result = read_and_backup_list(db_files, abs_temp_path);
    free_list_and_data(db_files);
    return result;
}

void free_list_and_data(GList * list)
{
    syslog(LOG_DEBUG, "%s", __func__ );

    if (!list )
        return;

    GList* iter = NULL;
    GList* last = g_list_last(list);
    for (iter = g_list_first (list);; iter = g_list_next (iter))
    {
        g_free(iter->data);
        if ( iter == last )
            break;
    }

    g_list_free(list);
    list = NULL;
}

void restore_prefs_data(gchar* key, gchar* value, bool set_flag)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    syslog(LOG_DEBUG, "set_flag : %d, key : %s,value : %s", set_flag,key,value);
    if(set_flag)
    {
        sqlite3_reset(backup_statement);
        sqlite3_bind_text( backup_statement, 1, key , -1, SQLITE_TRANSIENT);
        sqlite3_bind_text( backup_statement, 2, value , -1, SQLITE_TRANSIENT);
        sqlite3_bind_text( backup_statement, 3, key , -1, SQLITE_TRANSIENT);
        int ret = sqlite3_step(backup_statement);
        if (ret != SQLITE_DONE)
        {
            syslog(LOG_ERR, "Update : Failed to restore key : %s, value : %s ret : %d",key, value, ret);
        }
    }
    else
    {
        sqlite3_reset(individual_db_statement);
        sqlite3_bind_text( individual_db_statement, 1, key , -1, SQLITE_TRANSIENT);
        sqlite3_bind_text( individual_db_statement, 2, value , -1, SQLITE_TRANSIENT);
        int ret = sqlite3_step(individual_db_statement);
        if (ret != SQLITE_DONE)
        {
            syslog(LOG_ERR, "insert : Failed to restore key : %s, value : %s ret : %d",key, value, ret);
        }
    }

    return;
}

bool restore_action(const gchar* path)
{
    syslog(LOG_DEBUG, "%s path %s", __func__, path );
    gchar* key;
    gchar* value;
    int count = 0;
    gchar* count_str = NULL;
    bool set_flag = false;

    if( ! path )
    {
        syslog(LOG_ERR, "Invalid database path" );
        return false;
    }


    if (!exec_command(restoreDb, "create table if not exists data ("
            "key string, "
            "value string"
            ")"))
    {
        syslog(LOG_DEBUG, "could not create data table");
        return false;
    }

    if (!prepare_statement(restoreDb, &backup_statement,
        "update data set  key=?, value=? where key=?"))
    {
        syslog(LOG_DEBUG, "prepare statement fail restore_prefs_data() : update");
        return false;
    }

    if (!prepare_statement(restoreDb, &individual_db_statement,
        "insert into data (key,value) values (?,?)"))
    {
        syslog(LOG_DEBUG, "prepare statement fail restore_prefs_data() : insert");
        finalize_statement(&backup_statement);
        return false;
    }

    sqlite3_reset(prefs_db_statement);
    sqlite3_bind_text( prefs_db_statement, 1, path , -1, SQLITE_TRANSIENT);

    int ret = sqlite3_step(prefs_db_statement);
    while(ret == SQLITE_ROW )
    {
        key  = g_strdup ( (gchar*)sqlite3_column_text(prefs_db_statement, 0 ) );
        value = g_strdup ( (gchar*)sqlite3_column_text(prefs_db_statement, 1 ) );
        syslog(LOG_DEBUG, "restore: key %s, value %s",key,value);

        count_str = g_strdup_printf("select count(*) from data where key='%s'", key);

        if( !prepare_statement(restoreDb,&count_stmt,count_str))
        {
             syslog(LOG_DEBUG, "prepare statement failed(count_stmt)");
        }
        ret = sqlite3_step(count_stmt);
        if( ret == SQLITE_ROW )
        {
            count = sqlite3_column_int(count_stmt, 0);
        }

        syslog(LOG_DEBUG, "count = %d",count);

        if(count)
        {
            set_flag = true;
            restore_prefs_data(key, value, set_flag);
            set_flag = false;
        }
        else
        {
            restore_prefs_data(key, value, set_flag);
            set_flag = false;
        }
        ret = sqlite3_step(prefs_db_statement);
        finalize_statement(&count_stmt);
        g_free(count_str);

    }

    if (ret != SQLITE_DONE)
    {
        syslog(LOG_DEBUG, "Failed to restore action (%d)", ret);
        finalize_statement(&individual_db_statement);
        finalize_statement(&backup_statement);
        return false;
    }

    finalize_statement(&individual_db_statement);
    finalize_statement(&backup_statement);
    return true;
}

bool begin_restore(const gchar* db_file)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    int step_result;
    gchar* app_db_path = NULL;
    GList* restore_db_list = NULL;
    GList* list_iter = NULL;

    if( ! db_file )
    {
        syslog(LOG_ERR, "Invalid database path" );
        return false;
    }

    if( !open_database(db_file, &backUpDb) )
    {
        syslog(LOG_ERR, "Failed to open database" );
        return false;
    }

    if (!prepare_statement(backUpDb, &prefs_db_statement,
        "select distinct appPath from lunaPrefs_backup"))
    {
        return false;
    }

    step_result = sqlite3_step(prefs_db_statement);
    while( step_result == SQLITE_ROW )
    {
        app_db_path  = g_strdup ( (gchar*)sqlite3_column_text(prefs_db_statement, 0 ) );
        syslog(LOG_DEBUG, "path %s", app_db_path );
        restore_db_list = g_list_append(restore_db_list, (gpointer) app_db_path );
        step_result =  sqlite3_step(prefs_db_statement);
    }

    finalize_statement(&prefs_db_statement);
    list_iter = g_list_first(restore_db_list);

    if (!prepare_statement(backUpDb, &prefs_db_statement,
    "select key, value from lunaPrefs_backup where appPath=?"))
    {
        free_list_and_data(restore_db_list);
        return false;
    }

    while( list_iter )
    {
        gchar* parent_dir = g_path_get_dirname( (gchar* ) list_iter->data );
        syslog(LOG_DEBUG, "individual DB directory path %s", parent_dir );

        if (mkdir(parent_dir, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) < 0)
        {
            syslog(LOG_ERR, "Failed to create directory, with error : %s ", strerror(errno));
            free_list_and_data(restore_db_list);
            g_free(parent_dir);
            return false;
        }

        g_free(parent_dir);

        if( !open_database(list_iter->data , &restoreDb) )
        {
            syslog(LOG_ERR, "Failed to open database" );
            free_list_and_data(restore_db_list);
            return false;
        }

        if( !restore_action( (gchar* ) list_iter->data) )
        {
            syslog(LOG_DEBUG, "restore_action bind statement error");

            if( !close_database(restoreDb) )
            {
                syslog(LOG_DEBUG, "Failed to close database");
            }

            free_list_and_data(restore_db_list);
            return false;
        }

        list_iter = list_iter->next;

        if( !close_database(restoreDb) )
        {
            syslog(LOG_DEBUG, "Failed to close database");
        }
    }

    finalize_statement(&prefs_db_statement);
    free_list_and_data(restore_db_list);
    if( !close_database(backUpDb) )
    {
        syslog(LOG_DEBUG, "Failed to close database");
        return false;
    }
    return true;
}

bool try_restore(const gchar* db_file)
{
    syslog(LOG_DEBUG, "%s", __func__ );
    syslog(LOG_DEBUG, "db_file : %s", db_file );

    if( db_file && g_file_test(db_file , G_FILE_TEST_EXISTS ))
    {
        return begin_restore(db_file);
    }
    else
    {
        syslog(LOG_DEBUG, "%s does not exist",db_file);
    }
    return true;
}
