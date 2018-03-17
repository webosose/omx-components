// Copyright (c) 2017-2018 LG Electronics, Inc.
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


#pragma once

#include <string>
#include <exception>

#include <PmLogLib.h>

extern PmLogContext omxComponentsLogContext;
#define LOG_CRITICAL(msgid, kvcount, ...) \
	PmLogCritical(omxComponentsLogContext, msgid, kvcount, ##__VA_ARGS__)

#define LOG_ERROR(msgid, kvcount, ...) \
	PmLogError(omxComponentsLogContext, msgid, kvcount,##__VA_ARGS__)

#define LOG_WARNING(msgid, kvcount, ...) \
	PmLogWarning(omxComponentsLogContext, msgid, kvcount, ##__VA_ARGS__)

#define LOG_INFO(msgid, kvcount, ...) \
	PmLogInfo(omxComponentsLogContext, msgid, kvcount, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
	PmLogDebug(omxComponentsLogContext, "%s:%s() " fmt, __FILE__, __FUNCTION__, ##__VA_ARGS__)
#define LOG_TRACE(fmt, ...) \
	PMLOG_TRACE(fmt, ##__VA_ARGS__)

#define LOG_ESCAPED_ERRMSG(msgid, errmsg) \
    do { \
    gchar *escaped_errtext = g_strescape(errmsg, NULL); \
     LOG_ERROR(msgid, 1, PMLOGKS("Error", escaped_errtext), ""); \
    g_free(escaped_errtext); \
    } while(0)


#define MSGID_DRMOMXCOMPONENT_INFO     "MSGID_DRMOMXCOMPONENT_INFO"
#define MSGID_ALSAOMXCOMPONENT_INFO    "MSGID_OMXCOMPONENT_INFO"
#define MSGID_COMXCOMPONENT_INFO     "MSGID_COMXCOMPONENT_INFO"

