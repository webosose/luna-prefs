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

#include "accesschecker.h"
#include <cjson/json.h>

#define IS_CALL_ALLOWED_CHECK "luna://com.webos.service.bus/isCallAllowed"

typedef struct {
    LSMessage** message;
    callbackType callback;
} callbackContext;

bool processReply(LSHandle * handle, LSMessage * message, void* ctx)
{
    struct json_object* payload = json_tokener_parse( LSMessageGetPayload( message ) );
    callbackContext* newCC = (callbackContext *) ctx;

    if(!ctx)
    {
           return true;
    }

    if (!is_error(payload)) {
        if (!json_object_get_boolean(json_object_object_get(payload, "returnValue"))) {
            LSMessage *replyMessage = newCC->message;
            LSMessageUnref(replyMessage);
            json_object_put(payload);
            free(newCC);
            return true;
        }

        if(newCC->callback != NULL && newCC->message != NULL)
            newCC->callback(handle, (LSMessage *) (newCC->message),
                            json_object_get_boolean(json_object_object_get(payload, "allowed")));
    }

    json_object_put(payload);
    free(newCC);
    return true;
}

bool checkAccess(LSHandle * handle, LSMessage * message, const char *uri_to_check, callbackType callback)
{
    const char *serviceName = LSMessageGetSenderServiceName(message);
    callbackContext *cbContext = (callbackContext *) malloc (sizeof(callbackContext));
    if(!cbContext)
        return false;

    if (!serviceName) {
        serviceName = LSMessageGetSender(message);
        if (!serviceName) {
            free(cbContext);
            return false;
        }
    }

    const char *requester_service = serviceName;
    LSError lsError;
    LSErrorInit(&lsError);

    LSMessageToken token = LSMESSAGE_TOKEN_INVALID;
    gchar *payload = g_strdup_printf("{\"requester\":\"%s\",\"uri\":\"%s\"}", requester_service, uri_to_check);

    if (!payload)
    {
        free(cbContext);
        return false;
    }

    cbContext->callback = callback;
    cbContext->message = message;

    if(!LSCallOneReply(handle,
        IS_CALL_ALLOWED_CHECK,
        payload, processReply, (void *) cbContext, NULL, &lsError
    ))
    {
        LSErrorPrint( &lsError, stderr);
        LSErrorFree(&lsError);
        g_free(payload);
        free(cbContext);
        return false;
    }

    g_free(payload);

    return true;
}
