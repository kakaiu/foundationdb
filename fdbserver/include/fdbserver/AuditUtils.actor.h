/*
 * AuditUtils.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_SERVER_AUDITUTILS_ACTOR_G_H)
#define FDBSERVER_SERVER_AUDITUTILS_ACTOR_G_H
#include "fdbserver/AuditUtils.actor.g.h"
#elif !defined(FDBSERVER_SERVER_AUDITUTILS_ACTOR_H)
#define FDBSERVER_SERVER_AUDITUTILS_ACTOR_H
#pragma once

#include "fdbclient/Audit.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbrpc/fdbrpc.h"

#include "flow/actorcompiler.h" // has to be last include

ACTOR Future<Void> persistAuditStorage(Database cx, AuditStorageState auditState);
ACTOR Future<AuditStorageState> getAuditStorage(Database cx, UID id);

ACTOR Future<Void> persistAuditStorageMap(Database cx, AuditStorageState auditState);
ACTOR Future<std::vector<AuditStorageState>> getAuditStorageFroRange(Database cx, UID id, KeyRange range);

StringRef auditTypeToString(const AuditType type);

#include "flow/unactorcompiler.h"
#endif