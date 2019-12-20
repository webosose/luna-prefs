// Copyright (c) 2008-2018 LG Electronics, Inc.
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
/* -*-mode: C; fill-column: 78; c-basic-offset: 4; -*- */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <glib.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>

#include <luna-service2/lunaservice.h>
#include <lunaprefs.h>
#include <json.h>

#include "database.h"
#include "accesschecker.h"

/*!
 * \page com_palm_preferences_system_properties Service API com.palm.preferences/systemProperties/
 *
 * Public methods:
 *
 * - \ref com_palm_preferences_system_properties_get_sys_keys
 * - \ref com_palm_preferences_system_properties_get_sys_keys_obj
 * - \ref com_palm_preferences_system_properties_get_all_sys_properties
 * - \ref com_palm_preferences_system_properties_get_all_sys_properties_obj
 * - \ref com_palm_preferences_system_properties_get_some_sys_properties
 * - \ref com_palm_preferences_system_properties_get_some_sys_properties_obj
 * - \ref com_palm_preferences_system_properties_get_sys_property
 *
 */
/*!
 * \page com_palm_preferences_app_properties Service API com.palm.preferences/appProperties/
 *
 * Public methods:
 *
 * - \ref com_palm_preferences_app_properties_get_app_keys
 * - \ref com_palm_preferences_app_properties_get_app_keys_obj
 * - \ref com_palm_preferences_app_properties_get_all_app_properties
 * - \ref com_palm_preferences_app_properties_get_all_app_properties_obj
 * - \ref com_palm_preferences_app_properties_get_app_property
 * - \ref com_palm_preferences_app_properties_set_app_property
 * - \ref com_palm_preferences_app_properties_remove_app_property
 *
 */

static GMainLoop *g_mainloop = NULL;
static int sLogLevel = G_LOG_LEVEL_MESSAGE;
static bool sUseSyslog = false;

#define SERVICE_ROOT_URI "luna://com.palm.preferences"
#define GET_SYS_KEY_OBJ_API "getSysKeysObj"
#define GET_SOME_SYS_PROP_OBJ_API "getSomeSysPropertiesObj"
#define GET_ALL_SYS_PROP_OBJ_API "getAllSysPropertiesObj"

#define EXIT_TIMER_SECONDS 30

#define FREE_IF_SET(lserrp)                     \
    if ( LSErrorIsSet( lserrp ) ) {             \
        LSErrorFree( lserrp );                  \
    }

static void
term_handler( int signal )
{
    g_main_loop_quit( g_mainloop );
}

static gboolean
sourceFunc( gpointer data )
{
    g_debug( "%s()", __func__ );
    g_main_loop_quit( g_mainloop );
    return false;
}

/**
 * Start/Restart no activity quit timer
 *
 * - Should be called as fist command in methods callback functions for restart timer.
 * - Should be called immediately before g_main_loop_run to start timer for luna bus
 *   internal default category /com/palm/luna/private.
 */
static void
reset_timer( void )
{
    g_debug( "%s()", __func__ );
    static GSource* s_source = NULL;

    if ( NULL != s_source ) {
        g_source_destroy( s_source );
    }

    s_source = g_timeout_source_new_seconds( EXIT_TIMER_SECONDS );
    g_source_set_callback( s_source, sourceFunc, NULL, NULL );
    (void)g_source_attach( s_source, NULL );
}


static void
errorReplyStr( LSHandle* lsh, LSMessage* message, const char* errString )
{
    LSError lserror;

    if ( !errString ) {
        errString = "error text goes here";
    }

    char* errJson = g_strdup_printf( "{\"returnValue\": false, \"errorText\": \"%s\"}", errString );
    g_debug( "sending error reply: %s", errJson );

    LSErrorInit( &lserror );
    if ( !LSMessageReply( lsh, message, errJson, &lserror ) )
    {
        g_critical( "error from LSMessageReply: %s", lserror.message );
        LSErrorPrint( &lserror, stderr );
    }
    FREE_IF_SET(&lserror);
    g_free( errJson );
} /* errorReply */

static void
errorReplyStrMissingParam( LSHandle* lsh, LSMessage* message, const char* param )
{
    char* msg = g_strdup_printf( "Missing required parameter '%s'.", param );
    errorReplyStr( lsh, message, msg );
    g_free( msg );
}

static void
errorReplyErr( LSHandle* lsh, LSMessage* message, LPErr err )
{
    if ( LP_ERR_NONE != err ) {
        char* errMsg = NULL;
        (void)LPErrorString( err, &errMsg );
        errorReplyStr( lsh, message, errMsg );
        g_free( errMsg );
    }
}

char * formURI(LSMessage *message)
{
    char* formed_uri = (char *)g_strdup_printf("%s%s", SERVICE_ROOT_URI, LSMessageGetKind(message));
    return formed_uri;
}

static void
successReply( LSHandle* lsh, LSMessage* message )
{
    LSError lserror;
    const char* answer = "{\"returnValue\": true}";

    LSErrorInit( &lserror );
    if ( !LSMessageReply( lsh, message, answer, &lserror ) )
    {
        g_critical( "error from LSMessageReply: %s", lserror.message );
        LSErrorPrint( &lserror, stderr );
    }
    FREE_IF_SET(&lserror);
} /* successReply */

static bool
parseMessage( LSMessage* message, const char* firstKey, ... )
{
    bool success = false;
    const char* str = LSMessageGetPayload( message );
    if ( NULL != str ) {
        struct json_object* doc = json_tokener_parse( str );
        if ( doc ) {
            va_list ap;
            va_start( ap, firstKey );

            const char* key;
            for ( key = firstKey; !!key; key = va_arg(ap, char*) ) {
                enum json_type typ = va_arg(ap, enum json_type);
                g_assert( typ == json_type_string );

                char** out = va_arg(ap, char**);
                g_assert( out != NULL );
                *out = NULL;

                struct json_object* match = json_object_object_get( doc, key );
                if (NULL == match) {
                    goto error;
                }
                if ( json_object_is_type( match, typ ) == 0) {
                    goto error;
                }
                *out = g_strdup( json_object_get_string( match ) );
            }
            success = key == NULL; /* reached the end of arglist correctly */
        error:
            va_end( ap );
            json_object_put( doc );
        }
    }

    return success;
} /* parseMessage */

static void
add_true_result( struct json_object* obj )
{
    json_object_object_add( obj, "returnValue",
                            json_object_new_boolean( true ) );
}

static bool
replyWithValue( LSHandle* sh, LSMessage* message, LSError* lserror,
                const gchar* value )
{
    g_assert( !!value );
    g_debug( "%s(%s)", __func__, value );
    return LSMessageReply( sh, message, value, lserror );
} /* replyWithValue */

static bool
replyWithKeyValue( LSHandle* sh, LSMessage* message, LSError* lserror,
                   const gchar* key, const gchar* value )
{
    g_assert( !!value );
    struct json_object* jsonVal = json_tokener_parse( value );

    /* If it doesn't parse, it's probably just a string.  Turn it into a json string */
    if ( !jsonVal )
    {
        jsonVal = json_object_new_string( value );
    }
    else
    {
        enum json_type typ = json_object_get_type( jsonVal );
        if ( (typ != json_type_object) && (typ != json_type_array) )
        {
            json_object_put( jsonVal );
            jsonVal = json_object_new_string( value );
        }
    }

    struct json_object* result = json_object_new_object();
    g_assert( !!result );
    g_assert( !!key );
    json_object_object_add( result, key, jsonVal );

    add_true_result( result );

    const char* text = json_object_to_json_string( result );
    g_assert( !!text );
    bool success = replyWithValue( sh, message, lserror, text );

    json_object_put( result );

    return success;
} /* replyWithKeyValue */

static struct json_object*
wrapArray( struct json_object* jarray )
{
    g_debug( "%s", __func__ );
    g_assert( json_type_array == json_object_get_type( jarray ) );

    // Find "errorText" element of jarray
    bool returnValue = true;
    int len          = json_object_array_length(jarray);
    int i;
    for (i = 0; i < len; ++i)
    {
        struct json_object* elem = json_object_array_get_idx(jarray, i);
        if (json_object_is_type(elem, json_type_object) &&
            json_object_object_get( elem, "errorText"))
        {
            returnValue = false;
            break;
        }
    }

    // Wrap array
    struct json_object* result = json_object_new_object();
    json_object_object_add(result, "values", jarray);
    json_object_object_add(result, "returnValue", json_object_new_boolean(returnValue));
    return result;
}

typedef LPErr (*SysGetter)( struct json_object** json );

void sysGet_internal( LSHandle* sh, LSMessage* message, SysGetter getter,
                 bool asObj )
{
    struct json_object* json = NULL;

    LPErr err = (*getter)(&json);
    if (err != 0)
    {
        errorReplyErr(sh, message, err);
        LSMessageUnref(message);
        return;
    }

    if (asObj)
    {
        json = wrapArray(json);
    }

    const char* jstr = json_object_to_json_string(json);

    LSError lserror;
    LSErrorInit(&lserror);
    if (!LSMessageReply(sh, message, jstr, &lserror))
    {
        LSErrorPrint(&lserror, stderr);
        LSErrorFree(&lserror);
    }

    json_object_put(json);
    LSMessageUnref(message);

} /* sysGet_internal */

void sysGetKeysObj_callback(LSHandle* sh, LSMessage* message, bool allowed)
{
    sysGet_internal( sh, message, allowed? LPSystemCopyKeysCJ: LPSystemCopyKeysPublicCJ,
                   (0 == strcmp(LSMessageGetMethod(message), GET_SYS_KEY_OBJ_API))? true: false);
}

/*!
\page com_palm_preferences_system_properties
\n
\section com_palm_preferences_system_properties_get_sys_keys getSysKeys

\e Public.

com.palm.preferences/systemProperties/getSysKeys

Get the list of system property keys as a string array.

\subsection com_palm_preferences_system_properties_get_sys_keys_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_preferences_system_properties_get_sys_keys_returns Returns:
\code
[ string array ]
\endcode

\subsection com_palm_preferences_system_properties_get_sys_keys_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/systemProperties/getSysKeys '{}'
\endcode

Example response:
\code
[
    "com.palm.properties.browserOsName",
    "com.palm.properties.DMSETS",
    "com.palm.properties.deviceNameBranded",
    "com.palm.properties.deviceNameShortBranded",
    "com.palm.properties.GMFLAG",
    "com.palm.properties.deviceName",
    "com.palm.properties.productClass",
    "com.palm.properties.machineName",
    "com.palm.properties.deviceNameShort",
    "com.palm.properties.productLineName",
    "com.palm.properties.timing.upstart_finish",
    "com.palm.properties.timing.early_stop",
    "com.palm.properties.timing.early_start",
    "com.palm.properties.pids.upstart_finish",
    "com.palm.properties.timing.upstart_begin",
    "com.palm.properties.pids.early_start",
    "com.palm.properties.version",
    "com.palm.properties.buildName",
    "com.palm.properties.buildNumber",
    "com.palm.properties.nduid",
    "com.palm.properties.boardType",
    "com.palm.properties.storageCapacity",
    "com.palm.properties.storageFreeSpace",
    "com.palm.properties.prevBootPanicked",
    "com.palm.properties.prevShutdownClean"
]
\endcode
*/
/*!
\page com_palm_preferences_system_properties
\n
\section com_palm_preferences_system_properties_get_sys_keys_obj getSysKeysObj

\e Public.

com.palm.preferences/systemProperties/getSysKeysObj

Get the list of system property keys as a JSON object.

\subsection com_palm_preferences_system_properties_get_sys_keys_obj_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_preferences_system_properties_get_sys_keys_obj_returns Returns:
\code
{
    "values": [ string array ],
    "returnValue": boolean
}
\endcode

\param values System keys as a string array
\param returnValue Indicates if the call was succesful or not.

\subsection com_palm_preferences_system_properties_get_sys_keys_obj_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/systemProperties/getSysKeysObj '{}'
\endcode

Example response for a call:
\code
{
    "values": [
        "com.palm.properties.browserOsName",
        "com.palm.properties.DMSETS",
        "com.palm.properties.deviceNameBranded",
        "com.palm.properties.deviceNameShortBranded",
        "com.palm.properties.GMFLAG",
        "com.palm.properties.deviceName",
        "com.palm.properties.productClass",
        "com.palm.properties.machineName",
        "com.palm.properties.deviceNameShort",
        "com.palm.properties.productLineName",
        "com.palm.properties.timing.upstart_finish",
        "com.palm.properties.timing.early_stop",
        "com.palm.properties.timing.early_start",
        "com.palm.properties.pids.upstart_finish",
        "com.palm.properties.timing.upstart_begin",
        "com.palm.properties.pids.early_start",
        "com.palm.properties.version",
        "com.palm.properties.buildName",
        "com.palm.properties.buildNumber",
        "com.palm.properties.nduid",
        "com.palm.properties.boardType",
        "com.palm.properties.storageCapacity",
        "com.palm.properties.storageFreeSpace",
        "com.palm.properties.prevBootPanicked",
        "com.palm.properties.prevShutdownClean"
    ],
    "returnValue": true
}
\endcode
*/
static bool
sysGetKeysObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    /* Takes an array of what are meant to be property keys and returns an
     * array of key-value pairs equivalent to what Get would have returned for
     * each key.  If one of them fails an error is returned in that element of
     * the array but the rest go through.
     *
     * See ./tests/scripts/tests.sh for examples
    */

    char * uri_to_check = formURI(message);

    if(!uri_to_check) {
        errorReplyStr( sh, message, "URI not formed" );
        return true;
    }

    LSMessageRef(message);

    if(!checkAccess(sh, message, uri_to_check, sysGetKeysObj_callback)) {
        errorReplyErr( sh, message, LP_ERR_PERM);
        LSMessageUnref(message);
    }

    free(uri_to_check);
    return true;
}

static void
addKeyValueToArray( struct json_object* array, const char* key, const char* value )
{
    struct json_object* elemOut = json_object_new_object();
    struct json_object* jsonVal = json_object_new_string( value );
    json_object_object_add( elemOut, key, jsonVal );
    (void)json_object_array_add( array, elemOut );
}

static bool
onWhitelist( const char* key )
{
    bool privilegedAccessAllowed = false;

    LPErr err = LPSystemKeyIsPublic(key, &privilegedAccessAllowed);
    g_assert( LP_ERR_NONE == err );

    return privilegedAccessAllowed;
}

void sysGetSomeObj_callback(LSHandle* sh, LSMessage* message, bool allowed)
{
    struct json_object* arrayOut = NULL;
    const char* str = LSMessageGetPayload( message );

    if ( NULL != str ) {
        struct json_object* doc = json_tokener_parse( str );
        if ( doc && json_object_is_type( doc, json_type_array ) ) {
            int len = json_object_array_length( doc );
            int ii;

            arrayOut = json_object_new_array();

            for ( ii = 0; ii < len; ++ii )
            {
                struct json_object* key;
                struct json_object* elem = json_object_array_get_idx( doc, ii );
                if ( ( json_object_is_type( elem, json_type_object ) )
                    && (NULL != (key = json_object_object_get( elem, "key" ))) )
                {
                    char* errMsg = NULL;
                    const char* keyText = json_object_get_string( key );
                    if ( !allowed && !onWhitelist(keyText) ) {
                        (void)LPErrorString( LP_ERR_PERM, &errMsg );
                        addKeyValueToArray( arrayOut, "errorText", errMsg );
                    } else {
                        gchar* value = NULL;
                        LPErr err = LPSystemCopyStringValue( keyText, &value );
                        if ( LP_ERR_NONE == err ) {
                            addKeyValueToArray( arrayOut, keyText, value );
                        } else {
                            (void)LPErrorString( err, &errMsg );
                            addKeyValueToArray( arrayOut, "errorText", errMsg );
                        }
                        g_free(value);
                    }
                    g_free( errMsg );
                } else {
                    addKeyValueToArray( arrayOut, "errorText", "missing 'key' parameter" );
                }
            } /* for */
            json_object_put( doc );
        }
    }

    if ( !!arrayOut )
    {
        if (0 == strcmp(LSMessageGetMethod(message), GET_SOME_SYS_PROP_OBJ_API)){
            arrayOut = wrapArray( arrayOut );
        }

        const char* text = json_object_to_json_string( arrayOut );
        g_assert( !!text );

        LSError lserror;
        LSErrorInit( &lserror );
        (void)replyWithValue( sh, message, &lserror, text );

        json_object_put( arrayOut );
        FREE_IF_SET( &lserror );
    } else {
        errorReplyErr( sh, message, LP_ERR_PARAM_ERR ); /* Takes an array */
    }

    LSMessageUnref(message);

}

/*!
\page com_palm_preferences_system_properties
\n
\section com_palm_preferences_system_properties_get_some_sys_properties getSomeSysProperties

\e Public.

com.palm.preferences/systemProperties/getSomeSysProperties

Takes an object array of property keys and returns an array of objects
containing key-value pairs equivalent to what getSysProperty would have
returned for each key.

If one of them fails an error is returned in that element of the array but the
rest go through.

\subsection com_palm_preferences_system_properties_get_some_sys_properties_syntax Syntax:
\code
[
    { "key": string },
    { "key": string },
    ...
]
\endcode

\param key Name of the property.

\subsection com_palm_preferences_system_properties_get_some_sys_properties_returns_success Returns with a succesful call:
\code
[
    { "<key>": string },
    { "errorText": string },
    ...
]
\endcode

\param <key> Property for the key given as parameter.
\param errorText Describes the error if the call was not succesful.

\subsection com_palm_preferences_system_properties_get_some_sys_properties_returns_failure Returns with a failed call:
\code
{
    "returnValue": boolean,
    "errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful, i.e. always false in this situation.
\param errorText Describes the error.

\subsection com_palm_preferences_system_properties_get_some_sys_properties_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/systemProperties/getSomeSysProperties '[ {"key": "com.palm.properties.version"}, {"key": "com.palm.properties.productLineName"}, {"key": "should result in an error"} ]'
\endcode

Example response for a succesful call:
\code
[
    {
        "com.palm.properties.version": "Open webOS 3.5.0 SDK"
    },
    {
        "com.palm.properties.productLineName": "Emulator"
    },
    {
        "errorText": "no such key"
    }
]
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "general parameter error"
}
\endcode
*/
/*!
\page com_palm_preferences_system_properties
\n
\section com_palm_preferences_system_properties_get_some_sys_properties_obj getSomeSysPropertiesObj

\e Public.

com.palm.preferences/systemProperties/getSomeSysPropertiesObj

Takes an object array of property keys and returns an object containing an array
of objects with key-value pairs equivalent to what getSysProperty would have
returned for each key.

If one of them fails an error is returned in that element of the array but the
rest go through.

\subsection com_palm_preferences_system_properties_get_some_sys_properties_obj_syntax Syntax:
\code
[
    { "key": string },
    { "key": string },
    ...
]
\endcode

\param key Name of the property.

\subsection com_palm_preferences_system_properties_get_some_sys_properties_obj_returns Returns:
\code
{
    "values": [
        {
            "<key>": string
        },
        {
            "errorText": string
        }
    ],
    "returnValue": boolean,
    "errorText": string
}
\endcode

\param values Object array containing the property key-value pairs.
\param returnValue Indicates if the call was succesful or not.
\param errorText Describes the error if the call was not succesful.


\subsection com_palm_preferences_system_properties_get_some_sys_properties_obj_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/systemProperties/getSomeSysPropertiesObj '[ {"key": "com.palm.properties.version"}, {"key": "com.palm.properties.productLineName"}, {"key": "should result in an error"} ]'
\endcode

Example response for a succesful call:
\code
{
    "values": [
        {
            "com.palm.properties.version": "Open webOS 3.5.0 SDK"
        },
        {
            "com.palm.properties.productLineName": "Emulator"
        },
        {
            "errorText": "no such key"
        }
    ],
    "returnValue": true
}
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "general parameter error"
}
\endcode
*/
static bool
sysGetSomeObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    /* Takes an array of what are meant to be property keys and returns an
     * array of key-value pairs equivalent to what Get would have returned for
     * each key.  If one of them fails an error is returned in that element of
     * the array but the rest go through.
     *
     * See ./tests/scripts/tests.sh for examples
    */

    char * uri_to_check = formURI(message);

    if(!uri_to_check) {
        errorReplyStr( sh, message, "URI not formed" );
        return true;
    }

    LSMessageRef(message);

    if(!checkAccess(sh, message, uri_to_check, sysGetSomeObj_callback)) {
        errorReplyErr( sh, message, LP_ERR_PERM);
        LSMessageUnref(message);
    }

    free(uri_to_check);
    return true;
}

void sysGetAllObj_callback(LSHandle* sh, LSMessage* message, bool allowed)
{
    sysGet_internal( sh, message, allowed? LPSystemCopyAllCJ: LPSystemCopyAllPublicCJ,
                   (0 == strcmp(LSMessageGetMethod(message), GET_ALL_SYS_PROP_OBJ_API))? true: false);

}

/*!
\page com_palm_preferences_system_properties
\n
\section com_palm_preferences_system_properties_get_all_sys_properties getAllSysProperties

\e Public.

com.palm.preferences/systemProperties/getAllSysProperties

Get all system properties as an object array.

\subsection com_palm_preferences_system_properties_get_all_sys_properties_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_preferences_system_properties_get_all_sys_properties_returns_succesful Returns for a succesful call:
\code
[ object array ]
\endcode

\subsection com_palm_preferences_system_properties_get_all_sys_properties_returns_failed Returns for a failed call:
\code
{
    "returnValue": boolean,
    "errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful, i.e. always false in this situation.
\param errorText Describes the error.

\subsection com_palm_preferences_system_properties_get_all_sys_properties_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/systemProperties/getAllSysProperties '{}'
\endcode

Example response for a succesful call:
\code
[
    {
        "com.palm.properties.deviceNameShort": "TouchPad"
    },
    {
        "com.palm.properties.deviceNameShortBranded": "(TouchPad)"
    },
    {
        "com.palm.properties.productClass": "hp-tablet"
    },
    {
        "com.palm.properties.browserOsName": "hpwOS"
    },
    {
        "com.palm.properties.GMFLAG": "1"
    },
    {
        "com.palm.properties.deviceNameBranded": "HP (TouchPad)"
    },

    ...

    {
        "com.palm.properties.storageFreeSpace": "29575446528"
    },
    {
        "com.palm.properties.prevBootPanicked": "false"
    },
    {
        "com.palm.properties.prevShutdownClean": "true"
    }
]
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "required system resource is missing"
}
\endcode
*/
/*!
\page com_palm_preferences_system_properties
\n
\section com_palm_preferences_system_properties_get_all_sys_properties_obj getAllSysPropertiesObj

\e Public.

com.palm.preferences/systemProperties/getAllSysPropertiesObj

Get all system properties as an object.

\subsection com_palm_preferences_system_properties_get_all_sys_properties_obj_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_preferences_system_properties_get_all_sys_properties_obj_returns Returns:
\code
{
    "values": [ object array ],
    "returnValue": boolean,
    "errorText": string
}
\endcode

\param values The properties as an object array.
\param returnValue Indicates if the call was succesful or not.
\param errorText Describes the error if the call was not succesful.

\subsection com_palm_preferences_system_properties_get_all_sys_properties_obj_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/systemProperties/getAllSysPropertiesObj '{}'
\endcode

Example response for a succesful call:
\code
{
    "values": [
        {
            "com.palm.properties.deviceNameShort": "TouchPad"
        },
        {
            "com.palm.properties.deviceNameShortBranded": "(TouchPad)"
        },
        {
            "com.palm.properties.productClass": "hp-tablet"
        },
        {
            "com.palm.properties.browserOsName": "hpwOS"
        },

        ...

        {
            "com.palm.properties.prevBootPanicked": "false"
        },
        {
            "com.palm.properties.prevShutdownClean": "true"
        }
    ],
    "returnValue": true
}
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "required system resource is missing"
}
\endcode
*/

static bool
sysGetAllObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    /* Takes an array of what are meant to be property keys and returns an
     * array of key-value pairs equivalent to what Get would have returned for
     * each key.  If one of them fails an error is returned in that element of
     * the array but the rest go through.
     *
     * See ./tests/scripts/tests.sh for examples
    */
    char * uri_to_check = formURI(message);

    if(!uri_to_check) {
        errorReplyStr( sh, message, "URI not formed" );
        return true;
    }

    LSMessageRef(message);

    if(!checkAccess(sh, message, uri_to_check, sysGetAllObj_callback)) {
        errorReplyErr( sh, message, LP_ERR_PERM);
        LSMessageUnref(message);
    }

    free(uri_to_check);

    return true;
}

void sysGetValue_callback(LSHandle* sh, LSMessage* message, bool allowed)
{
    gchar* key = NULL;
    LPErr err = LP_ERR_NONE;

    if ( parseMessage( message, "key", json_type_string, &key, NULL )
         && ( NULL != key ) ) {
        if ( !allowed && !onWhitelist( key ) ) {
            err = LP_ERR_PERM;
        } else {
            gchar* value = NULL;
            err = LPSystemCopyStringValue( key, &value );
            if ( LP_ERR_NONE == err && NULL != value ) {
                LSError lserror;
                LSErrorInit( &lserror );
                if ( !replyWithKeyValue( sh, message, &lserror, key, value ) ) {
                    LSErrorPrint( &lserror, stderr );
                    err = LP_ERR_INTERNAL;
                }
                FREE_IF_SET( &lserror );
            }
            g_free( value );
        }
    } else {
        errorReplyStr( sh, message, "missing parameter key" );
    }

    if (LP_ERR_NONE != err)
        errorReplyErr( sh, message, err );

    g_free( key );
    LSMessageUnref(message);

}/* sysGetValue callback */

/*!
\page com_palm_preferences_system_properties
\n
\section com_palm_preferences_system_properties_get_sys_property getSysProperty

\e Public.

com.palm.preferences/systemProperties/getSysProperty

Get a system property.

\subsection com_palm_preferences_system_properties_get_sys_property_syntax Syntax:
\code
{
    "key": string
}
\endcode

\param key Name of the property.

\subsection com_palm_preferences_system_properties_get_sys_property_returns Returns:
\code
{
    "<key>": string,
    "returnValue": boolean
}
\endcode

\param <key> The system property that was requested.
\param returnValue Indicates if the call was succesful or not.

\subsection com_palm_preferences_system_properties_get_sys_property_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/systemProperties/getSysProperty '{"key": "com.palm.properties.version"}'
\endcode

Example response for a succesful call:
\code
{
    "com.palm.properties.version": "Open webOS 3.5.0",
    "returnValue": true
}
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "no such key"
}
\endcode
*/
static bool
sysGetValue( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    LPErr err = LP_ERR_NONE;

    const char* uri_to_check = formURI(message);

    if(!uri_to_check) {
        errorReplyStr( sh, message, "URI not formed" );
        return true;
    }

    LSMessageRef(message);

    if (!checkAccess(sh, message, uri_to_check, sysGetValue_callback)) {
        errorReplyErr( sh, message, LP_ERR_PERM);
        LSMessageUnref(message);
    }

    free(uri_to_check);

    return true;
} /* sysGetValue */

static LSMethod sysPropGetMethods[] = {
   { "Get", sysGetValue, LUNA_METHOD_FLAG_DEPRECATED },
   { "getSysKeys", sysGetKeysObj, LUNA_METHOD_FLAG_DEPRECATED },
   { "getSysKeysObj", sysGetKeysObj, LUNA_METHOD_FLAG_DEPRECATED },
   { "getAllSysProperties", sysGetAllObj, LUNA_METHOD_FLAG_DEPRECATED },
   { "getAllSysPropertiesObj", sysGetAllObj, LUNA_METHOD_FLAG_DEPRECATED },
   { "getSomeSysProperties", sysGetSomeObj, LUNA_METHOD_FLAG_DEPRECATED },
   { "getSomeSysPropertiesObj", sysGetSomeObj, LUNA_METHOD_FLAG_DEPRECATED },
   { "getSysProperty", sysGetValue, LUNA_METHOD_FLAG_DEPRECATED },
   { },
};

typedef LPErr (*AppGetter)( LPAppHandle handle, struct json_object** json );

static bool
appGet_internal( LSHandle* sh, LSMessage* message, AppGetter getter, bool asObj )
{
    LPErr err = LP_ERR_NONE;
    gchar* appId = NULL;
    struct json_object* json = NULL;
    LPAppHandle handle = NULL;

    if ( parseMessage( message,
                       "appId", json_type_string, &appId,
                       NULL ) ) {
        err = LPAppGetHandle( appId, &handle );
        if ( 0 != err ) goto error;

        err = (*getter)( handle, &json );
        if ( 0 != err ) goto error;

        if ( asObj ) {
            json = wrapArray( json );
        }

        LSError lserror;
        LSErrorInit( &lserror );

        if ( !LSMessageReply( sh, message,
                              json_object_to_json_string(json),
                              &lserror ) ) {
            LSErrorPrint( &lserror, stderr );
        }
        FREE_IF_SET (&lserror);
    } else {
        errorReplyStr( sh, message, "no appId parameter found" );
    }

 error:
    errorReplyErr( sh, message, err );
    if ( !!handle ) {
        (void)LPAppFreeHandle( handle, FALSE );
    }
    g_free( appId );
    json_object_put( json );

    return true;
} /* appGetKeys */

/*!
\page com_palm_preferences_app_properties
\n
\section com_palm_preferences_app_properties_get_app_keys getAppKeys

\e Public.

com.palm.preferences/appProperties/getAppKeys

Get all property keys for an application as a string array.

\subsection com_palm_preferences_app_properties_get_app_keys_syntax Syntax:
\code
{
    "appId": string
}
\endcode

\param appId Id for the application.

\subsection com_palm_preferences_app_properties_get_app_keys_returns_succesful Returns with a succesful call:
\code
[ string array ]
\endcode

\subsection com_palm_preferences_app_properties_get_app_keys_returns_failure Returns with a failed call:
\code
{
    "returnValue": boolean,
    "errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful, i.e. always false in this situation.
\param errorText Describes the error.

\subsection com_palm_preferences_app_properties_get_app_keys_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/appProperties/getAppKeys '{"appId": "com.palm.app.calendar"}'
\endcode

Example response for a succesful call:
\code
[
    "aKey",
    "anotherKey"
]
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "no appId parameter found"
}
\endcode
*/
static bool
appGetKeys( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    return appGet_internal( sh, message, LPAppCopyKeysCJ, false );
}

/*!
\page com_palm_preferences_app_properties
\n
\section com_palm_preferences_app_properties_get_app_keys_obj getAppKeysObj

\e Public.

com.palm.preferences/appProperties/getAppKeysObj

Get all property keys for an application as a JSON object.

\subsection com_palm_preferences_app_properties_get_app_keys_obj_syntax Syntax:
\code
{
    "appId": string
}
\endcode

\param appId Id for the application.

\subsection com_palm_preferences_app_properties_get_app_keys_obj_returns Returns:
\code
{
    "values": [ string array ],
    "returnValue": boolean,
    "errorText": string
}
\endcode

\param values Property keys as a string array.
\param returnValue Indicates if the call was succesful.
\param errorText Describes the error if call was not succesful.

\subsection com_palm_preferences_app_properties_get_app_keys_obj_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/appProperties/getAppKeysObj '{"appId": "com.palm.app.calendar"}'
\endcode

Example response for a succesful call:
\code
{
    "values": [
        "aKey",
        "anotherKey"
    ],
    "returnValue": true
}
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "no appId parameter found"
}
\endcode
*/
static bool
appGetKeysObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    return appGet_internal( sh, message, LPAppCopyKeysCJ, true );
}

/*!
\page com_palm_preferences_app_properties
\n
\section com_palm_preferences_app_properties_get_all_app_properties getAllAppProperties

\e Public.

com.palm.preferences/appProperties/getAllAppProperties

Get all properties set to an application as an object array.

\subsection com_palm_preferences_app_properties_get_all_app_properties_syntax Syntax:
\code
{
    "appId": string
}
\endcode

\param appId Id for the application.

\subsection com_palm_preferences_app_properties_get_all_app_properties_returns_succesful Returns with a succesful call:
\code
[
    {
        "<key>": object
    },
    {
        "<key>": object
    },
    ...
]
\endcode

\param <key> Object containing the property for this key.

\subsection com_palm_preferences_app_properties_get_all_app_properties_returns_failure Returns with a failed call:
\code
{
    "returnValue": false,
    "errorText": "no appId parameter found"
}

\endcode

\param returnValue Indicates if the call was succesful, i.e. always false in this situation.
\param errorText Describes the error.

\subsection com_palm_preferences_app_properties_get_all_app_properties_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/appProperties/getAllAppProperties '{"appId": "com.palm.app.calendar"}'
\endcode

Example response for a succesful call:
\code
[
    {
        "aKey": {
            "aValue": "lots"
        }
    },
    {
        "anotherKey": {
            "anotherValue": "many"
        }
    },
    {
        "oneMoreKey": {
            "anInt": 1,
            "anotherInt": 2
        }
    }
]
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "no appId parameter found"
}

\endcode
*/
static bool
appGetAll( LSHandle* sh, LSMessage* message, void* user_data )
{
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    reset_timer();
    return appGet_internal( sh, message, LPAppCopyAllCJ, false );
} /* appGetAll */

/*!
\page com_palm_preferences_app_properties
\n
\section com_palm_preferences_app_properties_get_all_app_properties_obj getAllAppPropertiesObj

\e Public.

com.palm.preferences/appProperties/getAllAppPropertiesObj

Get all properties set to an application as an object.

\subsection com_palm_preferences_app_properties_get_all_app_properties_obj_syntax Syntax:
\code
{
    "appId": string
}
\endcode

\param appId Id for the application.

\subsection com_palm_preferences_app_properties_get_all_app_properties_obj_returns Returns:
\code
{
    "values": [
        {
            "<key>": object
        },
        {
            "<key>": object
        },
        ...
    ],
    "returnValue": boolean,
    "errorText": string
]
\endcode

\param values Object array containing the keys and their property objects.
\param <key> Object containing the property for this key.
\param returnValue Indicates if the call was succesful.
\param errorText Describes the error.

\subsection com_palm_preferences_app_properties_get_all_app_properties_obj_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/appProperties/getAllAppPropertiesObj '{"appId": "com.palm.app.calendar"}'
\endcode

Example response for a succesful call:
\code
{
    "values": [
        {
            "aKey": {
                "aValue": "lots"
            }
        },
        {
            "anotherKey": {
                "anotherValue": "many"
            }
        },
        {
            "oneMoreKey": {
                "anInt": 1,
                "anotherInt": 2
            }
        }
    ],
    "returnValue": true
}
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "no appId parameter found"
}

\endcode
*/
static bool
appGetAllObj( LSHandle* sh, LSMessage* message, void* user_data )
{
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    reset_timer();
    return appGet_internal( sh, message, LPAppCopyAllCJ, true );
} /* appGetAllObj */

/*!
\page com_palm_preferences_app_properties
\n
\section com_palm_preferences_app_properties_get_app_property getAppProperty

\e Public.

com.palm.preferences/appProperties/getAppProperty

Get an application property for a specific key.

\subsection com_palm_preferences_app_properties_get_app_property_syntax Syntax:
\code
{
    "appId": string
}
\endcode

\param appId Id for the application.

\subsection com_palm_preferences_app_properties_get_app_property_returns Returns:
\code
{
    "<key>": object,
    "returnValue": boolean,
    "errorText": string
}
\endcode

\param <key> Object containing the property for this key.
\param returnValue Indicates if the call was succesful.
\param errorText Describes the error.

\subsection com_palm_preferences_app_properties_get_app_property_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/appProperties/getAppProperty '{"appId": "com.palm.app.calendar", "key": "oneMoreKey"}'
\endcode

Example response for a succesful call:
\code
{
    "oneMoreKey": {
        "anInt": 1,
        "anotherInt": 2
    },
    "returnValue": true
}
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "no such key"
}
\endcode
*/
static bool
appGetValue( LSHandle* sh, LSMessage* message, void* user_data )
{
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    reset_timer();

    LPErr err = -1;           /* not 0 */

    gchar* appId = NULL;
    gchar* key = NULL;
    if ( parseMessage( message,
                       "appId", json_type_string, &appId,
                       "key", json_type_string, &key,
                       NULL ) ) {
        LPAppHandle handle;
        gchar* value = NULL;

        LSError lserror;
        LSErrorInit(&lserror);

        err = LPAppGetHandle( appId, &handle );
        if ( 0 != err ) goto error;
        err = LPAppCopyValue( handle, key, &value );
        if ( 0 != err ) goto err_with_handle;
        if ( !replyWithKeyValue( sh, message, &lserror, key, value ) ) goto err_with_handle;
        err = 0;
        LSErrorPrint(&lserror, stderr);
        FREE_IF_SET (&lserror);
    err_with_handle:
        errorReplyErr( sh, message, err );
        err = LPAppFreeHandle( handle, true );
        if ( 0 != err ) goto error;
    error:
        g_free( appId );
        g_free( key );
        g_free( value );
    } else {
        errorReplyStr( sh, message, "no appId or key parameter found" );
    }

    return true;
} /* appGetValue */

static bool
getStringParam( struct json_object* param, char** str )
{
    bool ok = !!param
        && json_object_is_type( param, json_type_string );
    if ( ok ) {
        *str = g_strdup( json_object_get_string( param ) );
    }
    return ok;
}

/*!
\page com_palm_preferences_app_properties
\n
\section com_palm_preferences_app_properties_set_app_property setAppProperty

\e Public.

com.palm.preferences/appProperties/setAppProperty

Add or change an application property.

\subsection com_palm_preferences_app_properties_set_app_property_syntax Syntax:
\code
{
    "appId": string,
    "key": string,
    "value": object
}
\endcode

\param appId Id for the application.
\param key Key for the property.
\param value Value for the property.

\subsection com_palm_preferences_app_properties_set_app_property_returns Returns:
\code
{
    "returnValue": boolean,
    "errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param errorText Describes the error.

\subsection com_palm_preferences_app_properties_set_app_property_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/appProperties/setAppProperty '{"appId": "com.palm.app.calendar", "key": "oneMoreKey", "value": {"anInt": 1, "anotherInt": 3} }'
\endcode

Example response for a succesful call:
\code
{
    "returnValue": true
}
\endcode

Example response for a failed call:
\code
{
    "returnValue": false,
    "errorText": "illegal value (not a json document)"
}
\endcode
*/
static bool
appSetValue( LSHandle* sh, LSMessage* message, void* user_data )
{
    reset_timer();

    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    bool success = false;
    LPErr err;

    struct json_object* payload = json_tokener_parse( LSMessageGetPayload( message ) );
    if ( payload ) {
        struct json_object* appId = json_object_object_get( payload, "appId" );
        struct json_object* key = json_object_object_get( payload, "key");
        struct json_object* value = json_object_object_get( payload, "value");
        gchar* appIdString = NULL;
        gchar* keyString = NULL;

        if ( !getStringParam( appId, &appIdString ) ) {
            errorReplyStrMissingParam( sh, message, "appId" );
        } else if ( g_strcmp0(g_strstrip(appIdString),"") == 0) {
            errorReplyStrMissingParam( sh, message, "appId" );
        } else if ( !getStringParam( key, &keyString ) ) {
            errorReplyStrMissingParam( sh, message, "key" );
        } else if ( !value ) {
            errorReplyStrMissingParam( sh, message, "value" );
        } else {
            LPAppHandle handle;
            if ((err = LPAppGetHandle( appIdString, &handle )) == LP_ERR_NONE)
            {
                gchar* valString = json_object_get_string( value );
                if ( valString ) {
                    err = LPAppSetValue( handle, keyString, valString );
                } else {
                    err = LP_ERR_VALUENOTJSON;
                }

                (void)LPAppFreeHandle( handle, true );
                success = LP_ERR_NONE == err;
            }

            errorReplyErr( sh, message, err );
        }

        g_free( keyString );
        g_free( appIdString );
        json_object_put( payload );
    }
    if ( success ) {
        successReply( sh, message );
    }

    return true;
} /* appSetValue */

/*!
\page com_palm_preferences_app_properties
\n
\section com_palm_preferences_app_properties_remove_app_property removeAppProperty

\e Public.

com.palm.preferences/appProperties/removeAppProperty

Remove an application property.

\subsection com_palm_preferences_app_properties_remove_app_property_syntax Syntax:
\code
{
    "appId": string,
    "key": string
}
\endcode

\param appId Id for the application.
\param key Key for the property that should be removed.

\subsection com_palm_preferences_app_properties_remove_app_property_returns Returns:
\code
{
    "returnValue": boolean,
}
\endcode

\param returnValue Indicates if the call was succesful.

\subsection com_palm_preferences_app_properties_remove_app_property_examples Examples:
\code
luna-send -n 1 -f luna://com.palm.preferences/appProperties/removeAppProperty '{"appId": "com.palm.app.calendar", "key": "oneMoreKey" }'
\endcode

Example response for a succesful call:
\code
{
    "returnValue": true
}
\endcode
*/
static bool
appRemoveValue( LSHandle* sh, LSMessage* message, void* user_data )
{
    g_debug( "%s(%s)", __func__, LSMessageGetPayload(message) );
    reset_timer();

    gchar* appId = NULL;
    gchar* key = NULL;

    if ( parseMessage( message,
                       "appId", json_type_string, &appId,
                       "key", json_type_string, &key,
                       NULL ) )
    {
        LPAppHandle handle;
        LPErr err = LPAppGetHandle( appId, &handle );
        if ( LP_ERR_NONE == err )
        {
            err = LPAppRemoveValue( handle, key );
            if (LP_ERR_NONE == err)
            {
                successReply( sh, message );
            }
            else
            {
                errorReplyErr( sh, message, err );
            }
            (void)LPAppFreeHandle( handle, true );
        }
        else
        {
            errorReplyErr( sh, message, err);
        }
    }
    else
    {
        errorReplyStr( sh, message, "'appId'(string)/'key'(string) parameter is missing");
    }

    g_free( appId );
    g_free( key );
    return true;
} /* appRemoveValue */

/**
 * CallBack for preBackup method of category backup
 *
 * @code
 *
 * luna-send -n 1 -f luna://com.palm.preferences/backup/preBackup '{"tempDir": "/tmp"}'
 * echo 'SELECT * FROM lunaPrefs_backup;' | sqlite3 /tmp/lunaprefs_backup.db
 *
 * luna-send -n 1 -f luna://com.palm.preferences/backup/preBackup '{}'
 * echo 'SELECT * FROM lunaPrefs_backup;' | sqlite3 /var/preferences/lunaprefs_backup.db
 *
 * @endcode
 */
static bool
preBackup( LSHandle* sh, LSMessage* message, void* user_data )
{
    struct json_object* payload_json    = NULL;
    struct json_object* reply_json      = json_object_new_object();
    const gchar*        backup_db_path  = NULL;
    bool                success         = false;
    const gchar*        payload         = NULL;
    bool                addErrorPayload = false;
    const gchar*        errorText       = NULL;
    LSError             lserror;
    LSErrorInit(&lserror);

    // Restart no activity quit timer
    reset_timer();

    do
    {
        // Get payload string
        payload = LSMessageGetPayload(message);
        if (!payload)
        {
            errorText = "Cannot get payload";
            break;
        }

        // Parse payload string
        payload_json = json_tokener_parse(payload);
        if (!payload_json)
        {
            errorText = "Cannot parse payload";
            break;
        }

        // Check payload
        if (!json_object_is_type(payload_json, json_type_object))
        {
            errorText = "Peyload must have type object";
            break;
        }

        // Get, check and process "tempDir" parameter
        const char*         tempDir      = NULL;
        struct json_object* tempDir_json = NULL;
        bool exists = json_object_object_get_ex(payload_json, "tempDir", &tempDir_json);
        if (exists)
        {
            if (!tempDir_json || !json_object_is_type(tempDir_json, json_type_string))
            {
                errorText       = "Parameter \"tempDir\" must have value type string";
                addErrorPayload = true;
                break;
            }
            tempDir = json_object_get_string(tempDir_json);
        }
        backup_db_path = setBackupFile(tempDir);

        // Create and fill backup file
        if (!create_prefs_backup())
        {
            errorText = "Unable to create backup file";
            break;
        }

        // Add "files" array to reply for back compatibility
        struct json_object* files_json  = json_object_new_array();
        json_object_array_add(files_json, json_object_new_string(backup_db_path));
        json_object_object_add(reply_json, "files", files_json);

        // Ok!
        success = true;
    }
    while (false);

    // Send reply and release resources
    if (backup_db_path)
    {
        json_object_object_add(reply_json, "backupFile",    json_object_new_string(backup_db_path));
    }
    if (errorText)
    {
        json_object_object_add(reply_json, "errorText",    json_object_new_string(errorText));
    }
    if (addErrorPayload)
    {
        json_object_object_add(reply_json, "errorPayload", json_object_new_string(payload));
    }
    json_object_object_add(reply_json,     "returnValue",  json_object_new_boolean(success));
    if (!LSMessageReply(sh, message, json_object_to_json_string(reply_json), &lserror))
    {
        fprintf(stderr, "%s:%d ", __FILE__, __LINE__);
        LSErrorPrint(&lserror, stderr);
        LSErrorLogDefault("CAN_NOT_SEND_REPLY", &lserror);
        LSErrorFree(&lserror);
    }
    if (payload_json && payload_json)
    {
        json_object_put(payload_json);
    }
    json_object_put(reply_json);

    return true;
}

static bool
postRestore( LSHandle* sh, LSMessage* message, void* user_data )
{
    gchar* temp_dir_str = NULL;
    LSError lserror;
    LSErrorInit(&lserror);
    char* cpath = NULL;
    char* final_path = NULL;

    reset_timer();

    const char* str = LSMessageGetPayload(message);
    struct json_object* response = json_object_new_object();
    if (!str)
    {
        json_object_object_add (response, "returnValue", json_object_new_boolean(false));
        json_object_object_add (response, "errorText",
                                          json_object_new_string("Cannot get payload"));
        if (!LSMessageReply (sh, message, json_object_to_json_string(response), &lserror ))
        {
                LSErrorPrint(&lserror, stderr);
                LSErrorFree (&lserror);
        }

        json_object_put (response);
        return true;
    }

    struct json_object* payload = json_tokener_parse( LSMessageGetPayload( message ) );

    if (!payload || !payload)
    {
        json_object_object_add (response, "returnValue", json_object_new_boolean(false));
        json_object_object_add (response, "errorText",
                                          json_object_new_string("Cannot parse payload"));
        json_object_object_add (response, "errorPayload", json_object_new_string(str));

        if (!LSMessageReply (sh, message, json_object_to_json_string(response), &lserror ))
        {
            LSErrorPrint(&lserror, stderr);
            LSErrorFree (&lserror);
        }

        json_object_put (response);
        return true;
    }

    struct json_object* tempDirLabel = json_object_object_get (payload, "tempDir");
    if ((!tempDirLabel) || !tempDirLabel)
    {
        json_object_object_add (response, "returnValue", json_object_new_boolean(false));
        json_object_object_add (response, "errorText",
                    json_object_new_string("Required parameter \"tempDir\" is missing"));
        json_object_object_add (response, "errorPayload", json_object_new_string(str));

        if (!LSMessageReply (sh, message, json_object_to_json_string(response), &lserror ))
        {
                LSErrorPrint(&lserror, stderr);
                LSErrorFree (&lserror);
        }

        json_object_put (response);
        return true;
    }

    if (!json_object_is_type(tempDirLabel, json_type_string))
    {
        json_object_object_add (response, "returnValue", json_object_new_boolean(false));
        json_object_object_add (response, "errorText",
            json_object_new_string("Parameter \"tempDir\" must have value type string"));
        json_object_object_add (response, "errorPayload", json_object_new_string(str));

        if (!LSMessageReply (sh, message, json_object_to_json_string(response), &lserror ))
        {
                LSErrorPrint(&lserror, stderr);
                LSErrorFree (&lserror);
        }

        json_object_put (response);
        return true;
    }

    temp_dir_str = json_object_get_string(tempDirLabel);

    struct json_object* files = json_object_object_get (payload, "files");
    if (!files || !files)
    {
        json_object_object_add (response, "returnValue", json_object_new_boolean(false));
        json_object_object_add (response, "errorText",
                              json_object_new_string("Required parameter \"files\" is missing"));
        json_object_object_add (response, "errorPayload", json_object_new_string(str));

        if (!LSMessageReply (sh, message, json_object_to_json_string(response), &lserror )) {
            LSErrorPrint(&lserror, stderr);
            LSErrorFree (&lserror);
        }

        json_object_put (response);
        return true;
    }

    struct array_list* fileArray = json_object_get_array(files);

    if (!fileArray || !fileArray)
    {
        json_object_object_add (response, "returnValue", json_object_new_boolean(false));
        json_object_object_add (response, "errorText",
            json_object_new_string("Parameter \"files\" must have value type array"));
        json_object_object_add (response, "errorPayload", json_object_new_string(str));

        if (!LSMessageReply (sh, message, json_object_to_json_string(response), &lserror ))
        {
            LSErrorPrint(&lserror, stderr);
            LSErrorFree (&lserror);
        }

        json_object_put (response);
        return true;
    }

    int fileArrayLength = array_list_length (fileArray);
    int index = 0;

    syslog(LOG_DEBUG, "fileArrayLength = %d", fileArrayLength);

    for (index = 0; index < fileArrayLength; ++index)
    {
        struct json_object* obj = (struct json_object*) array_list_get_idx (fileArray, index);
        if ((!obj) || !obj)
        {
            syslog(LOG_WARNING,"array object isn't valid (skipping)");
            continue;
        }

        cpath = json_object_get_string(obj);
        syslog(LOG_DEBUG, "array[%d] file: %s", index, cpath);

        if (NULL == cpath)
        {
            syslog(LOG_WARNING, "array object [index : %d] is a file path that is empty (skipping)", index);
            continue;
        }

        if (NULL == strpbrk(cpath, "/"))
        {
            syslog(LOG_DEBUG, "strpbrk entered");
            final_path = g_build_filename(temp_dir_str, cpath, (gchar*)NULL);
        }
        else
        {
            syslog(LOG_DEBUG, "strpbrk not entered");
            final_path = cpath;
        }

        if(NULL != strstr(final_path,"lunaprefs_backup.db"))
        {
            syslog(LOG_DEBUG, "final_path : %s",final_path);
            if(!try_restore(final_path))
                errorReplyStr( sh, message, "unable to restore preference db");
            else
                successReply( sh, message );
        }

    }

    json_object_put (response);
    successReply( sh, message );
    return true;
}

static LSMethod appPropMethods[] = {
   { "getAppKeys", appGetKeys },
   { "getAppKeysObj", appGetKeysObj },
   { "getAllAppProperties", appGetAll },
   { "getAllAppPropertiesObj", appGetAllObj },
   { "getAppProperty", appGetValue },
   { "setAppProperty", appSetValue },
   { "removeAppProperty", appRemoveValue },
   { },
};

static LSMethod backupMethods[] = {
    { "preBackup", preBackup },
    { "postRestore", postRestore },
    {},
};

static void
logFilter(const gchar *log_domain, GLogLevelFlags log_level,
          const gchar *message, gpointer unused_data )
{
    if (log_level > sLogLevel) return;

    if (sUseSyslog)
    {
        int priority;
        switch (log_level & G_LOG_LEVEL_MASK) {
            case G_LOG_LEVEL_ERROR:
                priority = LOG_CRIT;
                break;
            case G_LOG_LEVEL_CRITICAL:
                priority = LOG_ERR;
                break;
            case G_LOG_LEVEL_WARNING:
                priority = LOG_WARNING;
                break;
            case G_LOG_LEVEL_MESSAGE:
                priority = LOG_NOTICE;
                break;
            case G_LOG_LEVEL_DEBUG:
                priority = LOG_DEBUG;
                break;
            case G_LOG_LEVEL_INFO:
            default:
                priority = LOG_INFO;
                break;
        }
        syslog(priority, "%s", message);
    }
    else
    {
        g_log_default_handler(log_domain, log_level, message, unused_data);
    }
} /* logFilter */

static void
usage( char** argv )
{
    fprintf( stderr,
             "usage: %s \\\n"
             "    [-d]        # enable debug logging \\\n"
             "    [-l]        # log to syslog instead of stderr \\\n"
             , argv[0] );
}

int
main( int argc, char** argv )
{
    bool retVal;
    LSError lserror;
    bool optdone = false;

    while ( !optdone )
    {
        switch( getopt( argc, argv, "dl" ) ) {
        case 'd':
            sLogLevel = G_LOG_LEVEL_DEBUG;
            break;
        case 'l':
            sUseSyslog = true;
            break;
        case -1:
            optdone = true;
            break;
        default:
            usage( argv );
            exit( 0 );
        }
    }

    g_log_set_default_handler(logFilter, NULL);

    LSErrorInit( &lserror );

    g_debug( "%s() in %s starting", __func__, __FILE__ );

    g_mainloop = g_main_loop_new( NULL, FALSE );

    /* Man pages say prefer sigaction() to signal() */
    struct sigaction sact;
    memset( &sact, 0, sizeof(sact) );
    sact.sa_handler = term_handler;
    (void)sigaction( SIGTERM, &sact, NULL );

    LSHandle* sh;

    do {
    retVal = LSRegister( "com.palm.preferences", &sh, &lserror);
    if (!retVal) break;

    /** Methods for backup service*/
    retVal = LSRegisterCategory( sh, "/backup",
                                            backupMethods,
                                            NULL, /* signals */
                                            NULL, /* user data */
                                            &lserror );
    if (!retVal) break;

    retVal = LSCategorySetData(sh,  "/backup", sh, &lserror);

    if (!retVal)
    {

        fprintf( stderr,
            "Failed to set user data for the systemProperties category: %s\n",
            lserror.message );
    }

    retVal = LSRegisterCategory( sh, "/systemProperties",
                                            sysPropGetMethods,
                                            NULL, /* signals */
                                            NULL, /* user data */
                                            &lserror );
    if (!retVal) break;

    retVal = LSCategorySetData(sh,  "/systemProperties", sh, &lserror);

    if (!retVal)
    {

        fprintf( stderr,
            "Failed to set user data for the systemProperties category: %s\n",
            lserror.message );
    }

    retVal = LSRegisterCategory( sh, "/appProperties", appPropMethods,
                                 NULL, /* signals */
                                 NULL, /* properties */
                                 &lserror );
    if (!retVal) break;

    retVal = LSGmainAttach( sh, g_mainloop, &lserror );

    reset_timer();

        g_main_loop_run( g_mainloop );
        g_main_loop_unref( g_mainloop );
    } while (false);

    if (!retVal)
    {
        fprintf( stderr, "error from LS call: %s\n", lserror.message );
    }

    (void)LSUnregister( sh, &lserror );

    FREE_IF_SET(&lserror);

    g_debug( "%s() exiting", __func__ );
    return 0;
}
