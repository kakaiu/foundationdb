/*
 * DataDistribution.actor.cpp
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

#include <set>
#include <string>

#include "fdbclient/DatabaseContext.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbclient/StorageServerInterface.h"
#include "fdbclient/SystemData.h"
#include "fdbclient/Tenant.h"
#include "fdbrpc/Replication.h"
#include "fdbserver/DataDistribution.actor.h"
#include "fdbserver/DDTeamCollection.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/TenantCache.h"
#include "fdbserver/TLogInterface.h"
#include "fdbserver/WaitFailure.h"
#include "flow/ActorCollection.h"
#include "flow/Arena.h"
#include "flow/BooleanParam.h"
#include "flow/genericactors.actor.h"
#include "flow/serialize.h"
#include "flow/Trace.h"
#include "flow/UnitTest.h"

#include "flow/actorcompiler.h" // This must be the last #include.

void DataMove::validateShard(const DDShardInfo& shard, KeyRangeRef range, int priority) {
	if (!valid) {
		if (shard.hasDest && shard.destId != anonymousShardId) {
			TraceEvent(SevError, "DataMoveValidationError")
			    .detail("Range", range)
			    .detail("Reason", "DataMoveMissing")
			    .detail("ShardPrimaryDest", describe(shard.primaryDest))
			    .detail("ShardRemoteDest", describe(shard.remoteDest));
		}
		return;
	}

	ASSERT(this->meta.range.contains(range));

	if (!shard.hasDest) {
		TraceEvent(SevError, "DataMoveValidationError")
		    .detail("Range", range)
		    .detail("Reason", "ShardMissingDest")
		    .detail("DataMoveMetaData", this->meta.toString())
		    .detail("DataMovePrimaryDest", describe(this->primaryDest))
		    .detail("DataMoveRemoteDest", describe(this->remoteDest));
		cancelled = true;
		return;
	}

	if (shard.destId != this->meta.id) {
		TraceEvent(SevError, "DataMoveValidationError")
		    .detail("Range", range)
		    .detail("Reason", "DataMoveIDMissMatch")
		    .detail("DataMoveMetaData", this->meta.toString())
		    .detail("ShardMoveID", shard.destId);
		cancelled = true;
		return;
	}

	if (!std::includes(
	        this->primaryDest.begin(), this->primaryDest.end(), shard.primaryDest.begin(), shard.primaryDest.end()) ||
	    !std::includes(
	        this->remoteDest.begin(), this->remoteDest.end(), shard.remoteDest.begin(), shard.remoteDest.end())) {
		TraceEvent(SevError, "DataMoveValidationError")
		    .detail("Range", range)
		    .detail("Reason", "DataMoveDestMissMatch")
		    .detail("DataMoveMetaData", this->meta.toString())
		    .detail("DataMovePrimaryDest", describe(this->primaryDest))
		    .detail("DataMoveRemoteDest", describe(this->remoteDest))
		    .detail("ShardPrimaryDest", describe(shard.primaryDest))
		    .detail("ShardRemoteDest", describe(shard.remoteDest));
		cancelled = true;
	}
}

// Read keyservers, return unique set of teams
ACTOR Future<Reference<InitialDataDistribution>> getInitialDataDistribution(Database cx,
                                                                            UID distributorId,
                                                                            MoveKeysLock moveKeysLock,
                                                                            std::vector<Optional<Key>> remoteDcIds,
                                                                            const DDEnabledState* ddEnabledState) {
	state Reference<InitialDataDistribution> result = makeReference<InitialDataDistribution>();
	state Key beginKey = allKeys.begin;

	state bool succeeded;

	state Transaction tr(cx);

	state std::map<UID, Optional<Key>> server_dc;
	state std::map<std::vector<UID>, std::pair<std::vector<UID>, std::vector<UID>>> team_cache;
	state std::vector<std::pair<StorageServerInterface, ProcessClass>> tss_servers;

	// Get the server list in its own try/catch block since it modifies result.  We don't want a subsequent failure
	// causing entries to be duplicated
	loop {
		server_dc.clear();
		result->allServers.clear();
		succeeded = false;
		try {
			// Read healthyZone value which is later used to determine on/off of failure triggered DD
			tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			Optional<Value> val = wait(tr.get(healthyZoneKey));
			if (val.present()) {
				auto p = decodeHealthyZoneValue(val.get());
				if (p.second > tr.getReadVersion().get() || p.first == ignoreSSFailuresZoneString) {
					result->initHealthyZoneValue = Optional<Key>(p.first);
				} else {
					result->initHealthyZoneValue = Optional<Key>();
				}
			} else {
				result->initHealthyZoneValue = Optional<Key>();
			}

			result->mode = 1;
			tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				rd >> result->mode;
			}
			if (!result->mode || !ddEnabledState->isDDEnabled()) {
				// DD can be disabled persistently (result->mode = 0) or transiently (isDDEnabled() = 0)
				TraceEvent(SevDebug, "GetInitialDataDistribution_DisabledDD").log();
				return result;
			}

			state Future<std::vector<ProcessData>> workers = getWorkers(&tr);
			state Future<RangeResult> serverList = tr.getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY);
			wait(success(workers) && success(serverList));
			ASSERT(!serverList.get().more && serverList.get().size() < CLIENT_KNOBS->TOO_MANY);

			std::map<Optional<Standalone<StringRef>>, ProcessData> id_data;
			for (int i = 0; i < workers.get().size(); i++)
				id_data[workers.get()[i].locality.processId()] = workers.get()[i];

			for (int i = 0; i < serverList.get().size(); i++) {
				auto ssi = decodeServerListValue(serverList.get()[i].value);
				if (!ssi.isTss()) {
					result->allServers.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
					server_dc[ssi.id()] = ssi.locality.dcId();
				} else {
					tss_servers.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
				}
			}

			RangeResult dms = wait(tr.getRange(dataMoveKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!dms.more && dms.size() < CLIENT_KNOBS->TOO_MANY);
			for (int i = 0; i < dms.size(); ++i) {
				auto dataMove = std::make_shared<DataMove>(decodeDataMoveValue(dms[i].value), true);
				const DataMoveMetaData& meta = dataMove->meta;
				for (const UID& id : meta.src) {
					auto& dc = server_dc[id];
					if (std::find(remoteDcIds.begin(), remoteDcIds.end(), dc) != remoteDcIds.end()) {
						dataMove->remoteSrc.push_back(id);
					} else {
						dataMove->primarySrc.push_back(id);
					}
				}
				for (const UID& id : meta.dest) {
					auto& dc = server_dc[id];
					if (std::find(remoteDcIds.begin(), remoteDcIds.end(), dc) != remoteDcIds.end()) {
						dataMove->remoteDest.push_back(id);
					} else {
						dataMove->primaryDest.push_back(id);
					}
				}
				std::sort(dataMove->primarySrc.begin(), dataMove->primarySrc.end());
				std::sort(dataMove->remoteSrc.begin(), dataMove->remoteSrc.end());
				std::sort(dataMove->primaryDest.begin(), dataMove->primaryDest.end());
				std::sort(dataMove->remoteDest.begin(), dataMove->remoteDest.end());

				auto ranges = result->dataMoveMap.intersectingRanges(meta.range);
				for (auto& r : ranges) {
					ASSERT(!r.value()->valid);
				}
				result->dataMoveMap.insert(meta.range, std::move(dataMove));
			}

			succeeded = true;

			break;
		} catch (Error& e) {
			wait(tr.onError(e));

			ASSERT(!succeeded); // We shouldn't be retrying if we have already started modifying result in this loop
			TraceEvent("GetInitialTeamsRetry", distributorId).log();
		}
	}

	// If keyServers is too large to read in a single transaction, then we will have to break this process up into
	// multiple transactions. In that case, each iteration should begin where the previous left off
	while (beginKey < allKeys.end) {
		TEST(beginKey > allKeys.begin); // Multi-transactional getInitialDataDistribution
		loop {
			succeeded = false;
			try {
				tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
				wait(checkMoveKeysLockReadOnly(&tr, moveKeysLock, ddEnabledState));
				state RangeResult UIDtoTagMap = wait(tr.getRange(serverTagKeys, CLIENT_KNOBS->TOO_MANY));
				ASSERT(!UIDtoTagMap.more && UIDtoTagMap.size() < CLIENT_KNOBS->TOO_MANY);
				RangeResult keyServers = wait(krmGetRanges(&tr,
				                                           keyServersPrefix,
				                                           KeyRangeRef(beginKey, allKeys.end),
				                                           SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT,
				                                           SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT_BYTES));
				succeeded = true;

				std::vector<UID> src, dest, last;
				UID srcId, destId;

				// for each range
				for (int i = 0; i < keyServers.size() - 1; i++) {
					decodeKeyServersValue(UIDtoTagMap, keyServers[i].value, src, dest, srcId, destId);
					DDShardInfo info(keyServers[i].key, srcId, destId);
					if (remoteDcIds.size()) {
						auto srcIter = team_cache.find(src);
						if (srcIter == team_cache.end()) {
							for (auto& id : src) {
								auto& dc = server_dc[id];
								if (std::find(remoteDcIds.begin(), remoteDcIds.end(), dc) != remoteDcIds.end()) {
									info.remoteSrc.push_back(id);
								} else {
									info.primarySrc.push_back(id);
								}
							}
							result->primaryTeams.insert(info.primarySrc);
							result->remoteTeams.insert(info.remoteSrc);
							team_cache[src] = std::make_pair(info.primarySrc, info.remoteSrc);
						} else {
							info.primarySrc = srcIter->second.first;
							info.remoteSrc = srcIter->second.second;
						}
						if (dest.size()) {
							info.hasDest = true;
							auto destIter = team_cache.find(dest);
							if (destIter == team_cache.end()) {
								for (auto& id : dest) {
									auto& dc = server_dc[id];
									if (std::find(remoteDcIds.begin(), remoteDcIds.end(), dc) != remoteDcIds.end()) {
										info.remoteDest.push_back(id);
									} else {
										info.primaryDest.push_back(id);
									}
								}
								result->primaryTeams.insert(info.primaryDest);
								result->remoteTeams.insert(info.remoteDest);
								team_cache[dest] = std::make_pair(info.primaryDest, info.remoteDest);
							} else {
								info.primaryDest = destIter->second.first;
								info.remoteDest = destIter->second.second;
							}
						}
					} else {
						info.primarySrc = src;
						auto srcIter = team_cache.find(src);
						if (srcIter == team_cache.end()) {
							result->primaryTeams.insert(src);
							team_cache[src] = std::pair<std::vector<UID>, std::vector<UID>>();
						}
						if (dest.size()) {
							info.hasDest = true;
							info.primaryDest = dest;
							auto destIter = team_cache.find(dest);
							if (destIter == team_cache.end()) {
								result->primaryTeams.insert(dest);
								team_cache[dest] = std::pair<std::vector<UID>, std::vector<UID>>();
							}
						}
					}
					result->shards.push_back(info);
				}

				ASSERT_GT(keyServers.size(), 0);
				beginKey = keyServers.end()[-1].key;
				break;
			} catch (Error& e) {
				TraceEvent("GetInitialTeamsKeyServersRetry", distributorId).error(e);

				wait(tr.onError(e));
				ASSERT(!succeeded); // We shouldn't be retrying if we have already started modifying result in this loop
			}
		}

		tr.reset();
	}

	// a dummy shard at the end with no keys or servers makes life easier for trackInitialShards()
	result->shards.push_back(DDShardInfo(allKeys.end));

	if (CLIENT_KNOBS->SHARD_ENCODE_LOCATION_METADATA) {
		for (int shard = 0; shard < result->shards.size() - 1; shard++) {
			const DDShardInfo& iShard = result->shards[shard];
			KeyRangeRef keys = KeyRangeRef(iShard.key, result->shards[shard + 1].key);
			result->dataMoveMap[keys.begin]->validateShard(iShard, keys);
		}
	}

	// add tss to server list AFTER teams are built
	for (auto& it : tss_servers) {
		result->allServers.push_back(it);
	}

	return result;
}

// add server to wiggling queue
void StorageWiggler::addServer(const UID& serverId, const StorageMetadataType& metadata) {
	// std::cout << "size: " << pq_handles.size() << " add " << serverId.toString() << " DC: "
	//           << teamCollection->isPrimary() << std::endl;
	ASSERT(!pq_handles.count(serverId));
	pq_handles[serverId] = wiggle_pq.emplace(metadata, serverId);
	nonEmpty.set(true);
}

void StorageWiggler::removeServer(const UID& serverId) {
	// std::cout << "size: " << pq_handles.size() << " remove " << serverId.toString() << " DC: "
	//           << teamCollection->isPrimary() << std::endl;
	if (contains(serverId)) { // server haven't been popped
		auto handle = pq_handles.at(serverId);
		pq_handles.erase(serverId);
		wiggle_pq.erase(handle);
	}
	nonEmpty.set(!wiggle_pq.empty());
}

void StorageWiggler::updateMetadata(const UID& serverId, const StorageMetadataType& metadata) {
	//	std::cout << "size: " << pq_handles.size() << " update " << serverId.toString()
	//	          << " DC: " << teamCollection->isPrimary() << std::endl;
	auto handle = pq_handles.at(serverId);
	if ((*handle).first == metadata) {
		return;
	}
	wiggle_pq.update(handle, std::make_pair(metadata, serverId));
}

Optional<UID> StorageWiggler::getNextServerId() {
	if (!wiggle_pq.empty()) {
		auto [metadata, id] = wiggle_pq.top();
		wiggle_pq.pop();
		pq_handles.erase(id);
		return Optional<UID>(id);
	}
	return Optional<UID>();
}

Future<Void> StorageWiggler::resetStats() {
	auto newMetrics = StorageWiggleMetrics();
	newMetrics.smoothed_round_duration = metrics.smoothed_round_duration;
	newMetrics.smoothed_wiggle_duration = metrics.smoothed_wiggle_duration;
	return StorageWiggleMetrics::runSetTransaction(teamCollection->cx, teamCollection->isPrimary(), newMetrics);
}

Future<Void> StorageWiggler::restoreStats() {
	auto& metricsRef = metrics;
	auto assignFunc = [&metricsRef](Optional<Value> v) {
		if (v.present()) {
			metricsRef = BinaryReader::fromStringRef<StorageWiggleMetrics>(v.get(), IncludeVersion());
		}
		return Void();
	};
	auto readFuture = StorageWiggleMetrics::runGetTransaction(teamCollection->cx, teamCollection->isPrimary());
	return map(readFuture, assignFunc);
}
Future<Void> StorageWiggler::startWiggle() {
	metrics.last_wiggle_start = StorageMetadataType::currentTime();
	if (shouldStartNewRound()) {
		metrics.last_round_start = metrics.last_wiggle_start;
	}
	return StorageWiggleMetrics::runSetTransaction(teamCollection->cx, teamCollection->isPrimary(), metrics);
}

Future<Void> StorageWiggler::finishWiggle() {
	metrics.last_wiggle_finish = StorageMetadataType::currentTime();
	metrics.finished_wiggle += 1;
	auto duration = metrics.last_wiggle_finish - metrics.last_wiggle_start;
	metrics.smoothed_wiggle_duration.setTotal((double)duration);

	if (shouldFinishRound()) {
		metrics.last_round_finish = metrics.last_wiggle_finish;
		metrics.finished_round += 1;
		duration = metrics.last_round_finish - metrics.last_round_start;
		metrics.smoothed_round_duration.setTotal((double)duration);
	}
	return StorageWiggleMetrics::runSetTransaction(teamCollection->cx, teamCollection->isPrimary(), metrics);
}

ACTOR Future<std::vector<std::pair<StorageServerInterface, ProcessClass>>> getServerListAndProcessClasses(
    Transaction* tr) {
	state Future<std::vector<ProcessData>> workers = getWorkers(tr);
	state Future<RangeResult> serverList = tr->getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY);
	wait(success(workers) && success(serverList));
	ASSERT(!serverList.get().more && serverList.get().size() < CLIENT_KNOBS->TOO_MANY);

	std::map<Optional<Standalone<StringRef>>, ProcessData> id_data;
	for (int i = 0; i < workers.get().size(); i++)
		id_data[workers.get()[i].locality.processId()] = workers.get()[i];

	std::vector<std::pair<StorageServerInterface, ProcessClass>> results;
	for (int i = 0; i < serverList.get().size(); i++) {
		auto ssi = decodeServerListValue(serverList.get()[i].value);
		results.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
	}

	return results;
}

ACTOR Future<Void> remoteRecovered(Reference<AsyncVar<ServerDBInfo> const> db) {
	TraceEvent("DDTrackerStarting").log();
	while (db->get().recoveryState < RecoveryState::ALL_LOGS_RECRUITED) {
		TraceEvent("DDTrackerStarting").detail("RecoveryState", (int)db->get().recoveryState);
		wait(db->onChange());
	}
	return Void();
}

ACTOR Future<Void> waitForDataDistributionEnabled(Database cx, const DDEnabledState* ddEnabledState) {
	state Transaction tr(cx);
	loop {
		wait(delay(SERVER_KNOBS->DD_ENABLED_CHECK_DELAY, TaskPriority::DataDistribution));

		try {
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (!mode.present() && ddEnabledState->isDDEnabled()) {
				TraceEvent("WaitForDDEnabledSucceeded").log();
				return Void();
			}
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				int m;
				rd >> m;
				TraceEvent(SevDebug, "WaitForDDEnabled")
				    .detail("Mode", m)
				    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
				if (m && ddEnabledState->isDDEnabled()) {
					TraceEvent("WaitForDDEnabledSucceeded").log();
					return Void();
				}
			}

			tr.reset();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<bool> isDataDistributionEnabled(Database cx, const DDEnabledState* ddEnabledState) {
	state Transaction tr(cx);
	loop {
		try {
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (!mode.present() && ddEnabledState->isDDEnabled())
				return true;
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				int m;
				rd >> m;
				if (m && ddEnabledState->isDDEnabled()) {
					TraceEvent(SevDebug, "IsDDEnabledSucceeded")
					    .detail("Mode", m)
					    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
					return true;
				}
			}
			// SOMEDAY: Write a wrapper in MoveKeys.actor.h
			Optional<Value> readVal = wait(tr.get(moveKeysLockOwnerKey));
			UID currentOwner =
			    readVal.present() ? BinaryReader::fromStringRef<UID>(readVal.get(), Unversioned()) : UID();
			if (ddEnabledState->isDDEnabled() && (currentOwner != dataDistributionModeLock)) {
				TraceEvent(SevDebug, "IsDDEnabledSucceeded")
				    .detail("CurrentOwner", currentOwner)
				    .detail("DDModeLock", dataDistributionModeLock)
				    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
				return true;
			}
			TraceEvent(SevDebug, "IsDDEnabledFailed")
			    .detail("CurrentOwner", currentOwner)
			    .detail("DDModeLock", dataDistributionModeLock)
			    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
			return false;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

// Ensures that the serverKeys key space is properly coalesced
// This method is only used for testing and is not implemented in a manner that is safe for large databases
ACTOR Future<Void> debugCheckCoalescing(Database cx) {
	state Transaction tr(cx);
	loop {
		try {
			state RangeResult serverList = wait(tr.getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY);

			state int i;
			for (i = 0; i < serverList.size(); i++) {
				state UID id = decodeServerListValue(serverList[i].value).id();
				RangeResult ranges = wait(krmGetRanges(&tr, serverKeysPrefixFor(id), allKeys));
				ASSERT(ranges.end()[-1].key == allKeys.end);

				for (int j = 0; j < ranges.size() - 2; j++)
					if (ranges[j].value == ranges[j + 1].value)
						TraceEvent(SevError, "UncoalescedValues", id)
						    .detail("Key1", ranges[j].key)
						    .detail("Key2", ranges[j + 1].key)
						    .detail("Value", ranges[j].value);
			}

			TraceEvent("DoneCheckingCoalescing").log();
			return Void();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

static std::set<int> const& normalDDQueueErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert(error_code_movekeys_conflict);
		s.insert(error_code_broken_promise);
		s.insert(error_code_data_move_cancelled);
		s.insert(error_code_data_move_dest_team_not_found);
	}
	return s;
}

ACTOR Future<Void> pollMoveKeysLock(Database cx, MoveKeysLock lock, const DDEnabledState* ddEnabledState) {
	loop {
		wait(delay(SERVER_KNOBS->MOVEKEYS_LOCK_POLLING_DELAY));
		state Transaction tr(cx);
		loop {
			try {
				wait(checkMoveKeysLockReadOnly(&tr, lock, ddEnabledState));
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}
}

struct DataDistributorData : NonCopyable, ReferenceCounted<DataDistributorData> {
	Reference<AsyncVar<ServerDBInfo> const> dbInfo;
	UID ddId;
	PromiseStream<Future<Void>> addActor;
	DDTeamCollection* teamCollection;
	Reference<EventCacheHolder> initialDDEventHolder;
	Reference<EventCacheHolder> movingDataEventHolder;
	Reference<EventCacheHolder> totalDataInFlightEventHolder;
	Reference<EventCacheHolder> totalDataInFlightRemoteEventHolder;

	DataDistributorData(Reference<AsyncVar<ServerDBInfo> const> const& db, UID id)
	  : dbInfo(db), ddId(id), teamCollection(nullptr),
	    initialDDEventHolder(makeReference<EventCacheHolder>("InitialDD")),
	    movingDataEventHolder(makeReference<EventCacheHolder>("MovingData")),
	    totalDataInFlightEventHolder(makeReference<EventCacheHolder>("TotalDataInFlight")),
	    totalDataInFlightRemoteEventHolder(makeReference<EventCacheHolder>("TotalDataInFlightRemote")) {}
};

ACTOR Future<Void> monitorPhysicalShardStatus(Database cx, Reference<PhysicalShardCollection> self,
                                         Reference<DataDistributionRuntimeMonitor> dataDistributionRuntimeMonitor) {
	ASSERT(CLIENT_KNOBS->DD_PHYSICAL_SHARD_CORE);
	loop {
		wait(delay(SERVER_KNOBS->ROCKSDB_METRICS_DELAY * 2));
		self->keyRangePhysicalShardIDMap.coalesce(allKeys);
		state std::map<uint64_t, std::vector<KeyRange>> physicalShardKeyRanges;
		state std::map<uint64_t, StorageMetrics> metricsReplies;
		state KeyRangeMap<uint64_t>::Ranges keyRangePhysicalShardIDRanges = self->keyRangePhysicalShardIDMap.ranges();
		state KeyRangeMap<uint64_t>::iterator it = keyRangePhysicalShardIDRanges.begin();
		// Get the mapping between PhysicalShardID and KeyRanges based on self->keyRangePhysicalShardIDMap
		for (; it != keyRangePhysicalShardIDRanges.end(); ++it) {
			uint64_t physicalShardID = it->value();
			KeyRange keyRange = Standalone(KeyRangeRef(it->range().begin, it->range().end));
			if (physicalShardKeyRanges.count(physicalShardID) == 0) {
				std::vector<KeyRange> initKeyRangeList;
				initKeyRangeList.push_back(keyRange);
				physicalShardKeyRanges.insert(std::make_pair(physicalShardID, initKeyRangeList));
			} else {
				physicalShardKeyRanges[physicalShardID].push_back(keyRange);
			}
		}
		for (auto [physicalShardID, keyRanges] : physicalShardKeyRanges) {
			TraceEvent e("PhysicalShardStatus");
			e.detail("PhysicalShardID", physicalShardID);
			e.detail("NumKeyRanges", keyRanges.size());
			e.detail("TotalBytes", self->physicalShardCollection[physicalShardID].metrics.bytes);
		}
		// Get metrics for each physicalShard
		/*it = keyRangePhysicalShardIDRanges.begin();
		for (; it != keyRangePhysicalShardIDRanges.end(); ++it) {
		    StorageMetrics metrics = wait(brokenPromiseToNever(
		        cx->getStorageMetrics(Standalone(KeyRangeRef(it->range().begin, it->range().end)),
		CLIENT_KNOBS->TOO_MANY))); uint64_t physicalShardID = it->value(); if (metricsReplies.count(physicalShardID)==0)
		{ metricsReplies.insert(std::make_pair(physicalShardID, metrics)); } else { metricsReplies[physicalShardID] =
		metricsReplies[physicalShardID] + metrics;
		    }
		}*/
		// Set metrics and clear unused physicalShard in self->physicalShardCollection based on
		// self->keyRangePhysicalShardIDMap
		for (auto it = self->physicalShardCollection.begin(); it != self->physicalShardCollection.end();) {
			uint64_t physicalShardID = it->first;
			if (physicalShardKeyRanges.count(physicalShardID) == 0) {
				TraceEvent("PhysicalShardHasNoKey").detail("PhysicalShard", physicalShardID);
				it = self->physicalShardCollection.erase(it);
				// erase from teamPhysicalShardIDs is delayed to "logging ServerPhysicalShardStatus"
			} else {
				// self->physicalShardCollection[physicalShardID].metrics = metricsReplies[physicalShardID];
				it++;
			}
		}

		std::vector<uint64_t> smallPhysicalShards;

		// Logging TeamPhysicalShardStatus
		for (auto [team, physicalShardIDs] : self->teamPhysicalShardIDs) {
			TraceEvent e("TeamPhysicalShardStatus");
			e.detail("Team", team.toString());
			std::string metricsStr = "";
			int64_t counter = 0;
			int64_t totalBytes = 0;
			int64_t maxPhysicalShardBytes = -1;
			int64_t minPhysicalShardBytes = StorageMetrics::infinity;
			uint64_t maxPhysicalShardID = 0;
			uint64_t minPhysicalShardID = 0;
			for (auto physicalShardID : physicalShardIDs) {
				uint64_t id = self->physicalShardCollection[physicalShardID].id;
				int64_t bytes = self->physicalShardCollection[physicalShardID].metrics.bytes;
				if (bytes < SERVER_KNOBS->MIN_PHYSICAL_SHARD_BYTES) {
					smallPhysicalShards.push_back(physicalShardID);
				}
				if (bytes > maxPhysicalShardBytes) {
					maxPhysicalShardBytes = bytes;
					maxPhysicalShardID = id;
				}
				if (bytes < minPhysicalShardBytes) {
					minPhysicalShardBytes = bytes;
					minPhysicalShardID = id;
				}
				totalBytes = totalBytes + bytes;
				metricsStr = metricsStr + std::to_string(id) + ":" + std::to_string(bytes);
				if (counter < physicalShardIDs.size() - 1) {
					metricsStr = metricsStr + ",";
				}
				counter = counter + 1;
			}
			e.detail("Metrics", metricsStr);
			e.detail("TotalBytes", totalBytes);
			e.detail("NumPhysicalShards", counter);
			e.detail("MaxPhysicalShard",
			         std::to_string(maxPhysicalShardID) + ":" + std::to_string(maxPhysicalShardBytes));
			e.detail("MinPhysicalShard",
			         std::to_string(minPhysicalShardID) + ":" + std::to_string(minPhysicalShardBytes));
		}
		// Logging ServerPhysicalShardStatus and update self->teamPhysicalShardIDs
		std::map<UID, std::map<uint64_t, int64_t>> storageServerPhysicalShardStatus;
		for (auto [team, physicalShardIDs] : self->teamPhysicalShardIDs) {
			for (auto ssid : team.servers) {
				for (auto it = self->teamPhysicalShardIDs[team].begin();
				     it != self->teamPhysicalShardIDs[team].end();) {
					uint64_t physicalShardID = *it;
					if (self->physicalShardCollection.count(physicalShardID) == 0) {
						it =
						    self->teamPhysicalShardIDs[team].erase(it); // This physicalShard is empty now, so remove it
					} else {
						if (storageServerPhysicalShardStatus.count(ssid) != 0) {
							if (storageServerPhysicalShardStatus[ssid].count(physicalShardID) == 0) {
								storageServerPhysicalShardStatus[ssid].insert(std::make_pair(
								    physicalShardID, self->physicalShardCollection[physicalShardID].metrics.bytes));
							} else {
								ASSERT(false);
							}
						} else {
							std::map<uint64_t, int64_t> tmp;
							tmp.insert(std::make_pair(physicalShardID,
							                          self->physicalShardCollection[physicalShardID].metrics.bytes));
							storageServerPhysicalShardStatus.insert(std::make_pair(ssid, tmp));
						}
						it++;
					}
				}
			}
		}
		for (auto [serverID, physicalShardMetrics] : storageServerPhysicalShardStatus) {
			TraceEvent e("ServerPhysicalShardStatus");
			e.detail("Server", serverID);
			e.detail("NumPhysicalShards", physicalShardMetrics.size());
			int64_t totalBytes = 0;
			int64_t maxPhysicalShardBytes = -1;
			int64_t minPhysicalShardBytes = StorageMetrics::infinity;
			uint64_t maxPhysicalShardID = 0;
			uint64_t minPhysicalShardID = 0;
			std::string metricsStr = "";
			int64_t counter = 0;
			for (auto [physicalShardID, bytes] : physicalShardMetrics) {
				totalBytes = totalBytes + bytes;
				if (bytes > maxPhysicalShardBytes) {
					maxPhysicalShardBytes = bytes;
					maxPhysicalShardID = physicalShardID;
				}
				if (bytes < minPhysicalShardBytes) {
					minPhysicalShardBytes = bytes;
					minPhysicalShardID = physicalShardID;
				}
				metricsStr = metricsStr + std::to_string(physicalShardID) + ":" + std::to_string(bytes);
				if (counter < physicalShardMetrics.size() - 1) {
					metricsStr = metricsStr + ",";
				}
				counter = counter + 1;
			}
			e.detail("TotalBytes", totalBytes);
			e.detail("MaxPhysicalShard",
			         std::to_string(maxPhysicalShardID) + ":" + std::to_string(maxPhysicalShardBytes));
			e.detail("MinPhysicalShard",
			         std::to_string(minPhysicalShardID) + ":" + std::to_string(minPhysicalShardBytes));
		}

		// Issue relocations for cold physicalShard
		if (CLIENT_KNOBS->PHYSICAL_SHARD_SIZE_CONTROL) {
			for (auto & physicalShardID : smallPhysicalShards) {
				dataDistributionRuntimeMonitor->triggerDDEvent(
					DDEventBuffer::DDEvent(SERVER_KNOBS->PRIORITY_MERGE_PHYSICAL_SHARD, physicalShardID), true);
			}
		}
	}
}

// Runs the data distribution algorithm for FDB, including the DD Queue, DD tracker, and DD team collection
ACTOR Future<Void> dataDistribution(Reference<DataDistributorData> self,
                                    PromiseStream<GetMetricsListRequest> getShardMetricsList,
                                    const DDEnabledState* ddEnabledState) {
	state Database cx = openDBOnServer(self->dbInfo, TaskPriority::DataDistributionLaunch, LockAware::True);
	cx->locationCacheSize = SERVER_KNOBS->DD_LOCATION_CACHE_SIZE;

	// cx->setOption( FDBDatabaseOptions::LOCATION_CACHE_SIZE, StringRef((uint8_t*)
	// &SERVER_KNOBS->DD_LOCATION_CACHE_SIZE, 8) ); ASSERT( cx->locationCacheSize ==
	// SERVER_KNOBS->DD_LOCATION_CACHE_SIZE
	// );

	// wait(debugCheckCoalescing(cx));
	state std::vector<Optional<Key>> primaryDcId;
	state std::vector<Optional<Key>> remoteDcIds;
	state DatabaseConfiguration configuration;
	state Reference<InitialDataDistribution> initData;
	state MoveKeysLock lock;
	state Reference<DDTeamCollection> primaryTeamCollection;
	state Reference<DDTeamCollection> remoteTeamCollection;
	state bool trackerCancelled;
	state bool ddIsTenantAware = SERVER_KNOBS->DD_TENANT_AWARENESS_ENABLED;
	loop {
		trackerCancelled = false;

		// Stored outside of data distribution tracker to avoid slow tasks
		// when tracker is cancelled
		state KeyRangeMap<ShardTrackedData> shards;
		state Promise<UID> removeFailedServer;
		try {
			loop {
				TraceEvent("DDInitTakingMoveKeysLock", self->ddId).log();
				MoveKeysLock lock_ = wait(takeMoveKeysLock(cx, self->ddId));
				lock = lock_;
				TraceEvent("DDInitTookMoveKeysLock", self->ddId).log();

				DatabaseConfiguration configuration_ = wait(getDatabaseConfiguration(cx));
				configuration = configuration_;
				primaryDcId.clear();
				remoteDcIds.clear();
				const std::vector<RegionInfo>& regions = configuration.regions;
				if (configuration.regions.size() > 0) {
					primaryDcId.push_back(regions[0].dcId);
				}
				if (configuration.regions.size() > 1) {
					remoteDcIds.push_back(regions[1].dcId);
				}

				TraceEvent("DDInitGotConfiguration", self->ddId).detail("Conf", configuration.toString());

				state Transaction tr(cx);
				loop {
					try {
						tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
						tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

						RangeResult replicaKeys = wait(tr.getRange(datacenterReplicasKeys, CLIENT_KNOBS->TOO_MANY));

						for (auto& kv : replicaKeys) {
							auto dcId = decodeDatacenterReplicasKey(kv.key);
							auto replicas = decodeDatacenterReplicasValue(kv.value);
							if ((primaryDcId.size() && primaryDcId[0] == dcId) ||
							    (remoteDcIds.size() && remoteDcIds[0] == dcId && configuration.usableRegions > 1)) {
								if (replicas > configuration.storageTeamSize) {
									tr.set(kv.key, datacenterReplicasValue(configuration.storageTeamSize));
								}
							} else {
								tr.clear(kv.key);
							}
						}

						wait(tr.commit());
						break;
					} catch (Error& e) {
						wait(tr.onError(e));
					}
				}

				TraceEvent("DDInitUpdatedReplicaKeys", self->ddId).log();
				Reference<InitialDataDistribution> initData_ = wait(getInitialDataDistribution(
				    cx,
				    self->ddId,
				    lock,
				    configuration.usableRegions > 1 ? remoteDcIds : std::vector<Optional<Key>>(),
				    ddEnabledState));
				initData = initData_;
				if (initData->shards.size() > 1) {
					TraceEvent("DDInitGotInitialDD", self->ddId)
					    .detail("B", initData->shards.end()[-2].key)
					    .detail("E", initData->shards.end()[-1].key)
					    .detail("Src", describe(initData->shards.end()[-2].primarySrc))
					    .detail("Dest", describe(initData->shards.end()[-2].primaryDest))
					    .trackLatest(self->initialDDEventHolder->trackingKey);
				} else {
					TraceEvent("DDInitGotInitialDD", self->ddId)
					    .detail("B", "")
					    .detail("E", "")
					    .detail("Src", "[no items]")
					    .detail("Dest", "[no items]")
					    .trackLatest(self->initialDDEventHolder->trackingKey);
				}

				if (initData->mode && ddEnabledState->isDDEnabled()) {
					// mode may be set true by system operator using fdbcli and isDDEnabled() set to true
					break;
				}

				TraceEvent("DataDistributionDisabled", self->ddId).log();

				TraceEvent("MovingData", self->ddId)
				    .detail("InFlight", 0)
				    .detail("InQueue", 0)
				    .detail("AverageShardSize", -1)
				    .detail("UnhealthyRelocations", 0)
				    .detail("HighestPriority", 0)
				    .detail("BytesWritten", 0)
				    .detail("PriorityRecoverMove", 0)
				    .detail("PriorityRebalanceUnderutilizedTeam", 0)
				    .detail("PriorityRebalannceOverutilizedTeam", 0)
				    .detail("PriorityTeamHealthy", 0)
				    .detail("PriorityTeamContainsUndesiredServer", 0)
				    .detail("PriorityTeamRedundant", 0)
				    .detail("PriorityMergeShard", 0)
				    .detail("PriorityTeamUnhealthy", 0)
				    .detail("PriorityTeam2Left", 0)
				    .detail("PriorityTeam1Left", 0)
				    .detail("PriorityTeam0Left", 0)
				    .detail("PrioritySplitShard", 0)
				    .trackLatest(self->movingDataEventHolder->trackingKey);

				TraceEvent("TotalDataInFlight", self->ddId)
				    .detail("Primary", true)
				    .detail("TotalBytes", 0)
				    .detail("UnhealthyServers", 0)
				    .detail("HighestPriority", 0)
				    .trackLatest(self->totalDataInFlightEventHolder->trackingKey);
				TraceEvent("TotalDataInFlight", self->ddId)
				    .detail("Primary", false)
				    .detail("TotalBytes", 0)
				    .detail("UnhealthyServers", 0)
				    .detail("HighestPriority", configuration.usableRegions > 1 ? 0 : -1)
				    .trackLatest(self->totalDataInFlightRemoteEventHolder->trackingKey);

				wait(waitForDataDistributionEnabled(cx, ddEnabledState));
				TraceEvent("DataDistributionEnabled").log();
			}

			state Reference<TenantCache> ddTenantCache;
			if (ddIsTenantAware) {
				ddTenantCache = makeReference<TenantCache>(cx, self->ddId);
				wait(ddTenantCache->build(cx));
			}

			// When/If this assertion fails, Evan owes Ben a pat on the back for his foresight
			ASSERT(configuration.storageTeamSize > 0);

			state PromiseStream<RelocateShard> output;
			state PromiseStream<RelocateShard> input;
			state PromiseStream<Promise<int64_t>> getAverageShardBytes;
			state PromiseStream<Promise<int>> getUnhealthyRelocationCount;
			state PromiseStream<GetMetricsRequest> getShardMetrics;
			state PromiseStream<GetTopKMetricsRequest> getTopKShardMetrics;
			state Reference<AsyncVar<bool>> processingUnhealthy(new AsyncVar<bool>(false));
			state Reference<AsyncVar<bool>> processingWiggle(new AsyncVar<bool>(false));
			state Promise<Void> readyToStart;
			state Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure(new ShardsAffectedByTeamFailure);
			state Reference<PhysicalShardCollection> physicalShardCollection(new PhysicalShardCollection);
			state Reference<DDEventBuffer> ddEventBuffer(new DDEventBuffer);
			state Reference<DataDistributionRuntimeMonitor> dataDistributionRuntimeMonitor(
			    new DataDistributionRuntimeMonitor);
			dataDistributionRuntimeMonitor->setGetShardMetrics(getShardMetrics);
			dataDistributionRuntimeMonitor->setPhysicalShardCollection(physicalShardCollection);
			dataDistributionRuntimeMonitor->setRelocateBuffer(output);
			dataDistributionRuntimeMonitor->setDDEventBuffer(ddEventBuffer);

			state int shard = 0;
			for (; shard < initData->shards.size() - 1; shard++) {
				const DDShardInfo& iShard = initData->shards[shard];
				KeyRangeRef keys = KeyRangeRef(iShard.key, initData->shards[shard + 1].key);

				shardsAffectedByTeamFailure->defineShard(keys);
				std::vector<ShardsAffectedByTeamFailure::Team> teams;
				teams.push_back(ShardsAffectedByTeamFailure::Team(iShard.primarySrc, true));
				if (configuration.usableRegions > 1) {
					teams.push_back(ShardsAffectedByTeamFailure::Team(iShard.remoteSrc, false));
				}
				if (CLIENT_KNOBS->DD_PHYSICAL_SHARD_CORE) {
					std::vector<ShardsAffectedByTeamFailure::Team> destTeams;
					destTeams.push_back(ShardsAffectedByTeamFailure::Team(iShard.primaryDest, true));
					if (configuration.usableRegions > 1) {
						destTeams.push_back(ShardsAffectedByTeamFailure::Team(iShard.remoteDest, false));
					}
					if (iShard.srcId != anonymousShardId) {
						physicalShardCollection->updatePhysicalShardToTeams(
						    iShard.srcId.first(), teams, configuration.storageTeamSize, 0);
						if (physicalShardCollection->physicalShardCollection.count(iShard.srcId.first()) == 0) {
							physicalShardCollection->physicalShardCollection.insert(std::make_pair(
							    iShard.srcId.first(), PhysicalShardCollection::PhysicalShard(iShard.srcId.first())));
						}
					}
					if (iShard.hasDest && iShard.destId != anonymousShardId) {
						physicalShardCollection->updatePhysicalShardToTeams(
						    iShard.destId.first(), destTeams, configuration.storageTeamSize, 1);
						if (physicalShardCollection->physicalShardCollection.count(iShard.destId.first()) == 0) {
							physicalShardCollection->physicalShardCollection.insert(std::make_pair(
							    iShard.destId.first(), PhysicalShardCollection::PhysicalShard(iShard.destId.first())));
						}
					}
					// Assigning keyRange to destId is delayed to restoring key move in relocation
					physicalShardCollection->keyRangePhysicalShardIDMap.insert(Standalone(keys), iShard.srcId.first());
					// Metrics of physicalShardCollection is inited by trackShardMetrics (see: forDDRestore in
					// updatePhysicalShardMetrics())
				}
				if (g_network->isSimulated()) {
					TraceEvent("DDInitShard")
					    .detail("Keys", keys)
					    .detail("PrimarySrc", describe(iShard.primarySrc))
					    .detail("RemoteSrc", describe(iShard.remoteSrc))
					    .detail("PrimaryDest", describe(iShard.primaryDest))
					    .detail("RemoteDest", describe(iShard.remoteDest))
					    .detail("SrcID", iShard.srcId)
					    .detail("DestID", iShard.destId);
				}

				shardsAffectedByTeamFailure->moveShard(keys, teams);
				if (iShard.hasDest && iShard.destId == anonymousShardId) {
					// This shard is already in flight.  Ideally we should use dest in ShardsAffectedByTeamFailure and
					// generate a dataDistributionRelocator directly in DataDistributionQueue to track it, but it's
					// easier to just (with low priority) schedule it for movement.
					bool unhealthy = iShard.primarySrc.size() != configuration.storageTeamSize;
					if (!unhealthy && configuration.usableRegions > 1) {
						unhealthy = iShard.remoteSrc.size() != configuration.storageTeamSize;
					}
					int priority =
					    unhealthy ? SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY : SERVER_KNOBS->PRIORITY_RECOVER_MOVE;
					RelocateShard rs(keys, priority, RelocateReason::OTHER);
					// output.send(rs);
					if (CLIENT_KNOBS->DD_FRAMEWORK) {
						dataDistributionRuntimeMonitor->triggerDDEvent(DDEventBuffer::DDEvent(priority, rs), true);
					} else {
						output.send(rs);
					}
				}

				wait(yield(TaskPriority::DataDistribution));
			}

			state KeyRangeMap<std::shared_ptr<DataMove>>::iterator it = initData->dataMoveMap.ranges().begin();
			for (; it != initData->dataMoveMap.ranges().end(); ++it) {
				const DataMoveMetaData& meta = it.value()->meta;
				if (it.value()->isCancelled() || (it.value()->valid && !CLIENT_KNOBS->SHARD_ENCODE_LOCATION_METADATA)) {
					RelocateShard rs(meta.range, SERVER_KNOBS->PRIORITY_RECOVER_MOVE, RelocateReason::OTHER);
					rs.dataMoveId = meta.id;
					rs.cancelled = true;
					// output.send(rs);
					if (CLIENT_KNOBS->DD_FRAMEWORK) {
						dataDistributionRuntimeMonitor->triggerDDEvent(
							DDEventBuffer::DDEvent(SERVER_KNOBS->PRIORITY_RECOVER_MOVE, rs), true);
					} else {
						output.send(rs);
					}
					TraceEvent("DDInitScheduledCancelDataMove", self->ddId).detail("DataMove", meta.toString());
				} else if (it.value()->valid) {
					TraceEvent(SevDebug, "DDInitFoundDataMove", self->ddId).detail("DataMove", meta.toString());
					ASSERT(meta.range == it.range());
					// TODO: Persist priority in DataMoveMetaData.
					RelocateShard rs(meta.range, SERVER_KNOBS->PRIORITY_RECOVER_MOVE, RelocateReason::OTHER);
					rs.dataMoveId = meta.id;
					rs.dataMove = it.value();
					std::vector<ShardsAffectedByTeamFailure::Team> teams;
					teams.push_back(ShardsAffectedByTeamFailure::Team(rs.dataMove->primaryDest, true));
					if (!rs.dataMove->remoteDest.empty()) {
						teams.push_back(ShardsAffectedByTeamFailure::Team(rs.dataMove->remoteDest, false));
					}

					// Since a DataMove could cover more than one keyrange, e.g., during merge, we need to define
					// the target shard and restart the shard tracker.
					shardsAffectedByTeamFailure->restartShardTracker.send(rs.keys);
					shardsAffectedByTeamFailure->defineShard(rs.keys);

					// When restoring a DataMove, the destination team is determined, and hence
					shardsAffectedByTeamFailure->moveShard(rs.keys, teams);
					// output.send(rs);
					if (CLIENT_KNOBS->DD_FRAMEWORK) {
						dataDistributionRuntimeMonitor->triggerDDEvent(
							DDEventBuffer::DDEvent(SERVER_KNOBS->PRIORITY_RECOVER_MOVE, rs), true);
					} else {
						output.send(rs);
					}
					wait(yield(TaskPriority::DataDistribution));
				}
			}

			std::vector<TeamCollectionInterface> tcis;

			Reference<AsyncVar<bool>> anyZeroHealthyTeams;
			std::vector<Reference<AsyncVar<bool>>> zeroHealthyTeams;
			tcis.push_back(TeamCollectionInterface());
			zeroHealthyTeams.push_back(makeReference<AsyncVar<bool>>(true));
			int storageTeamSize = configuration.storageTeamSize;

			std::vector<Future<Void>> actors;
			if (configuration.usableRegions > 1) {
				tcis.push_back(TeamCollectionInterface());
				storageTeamSize = 2 * configuration.storageTeamSize;

				zeroHealthyTeams.push_back(makeReference<AsyncVar<bool>>(true));
				anyZeroHealthyTeams = makeReference<AsyncVar<bool>>(true);
				actors.push_back(anyTrue(zeroHealthyTeams, anyZeroHealthyTeams));
			} else {
				anyZeroHealthyTeams = zeroHealthyTeams[0];
			}
			if (ddIsTenantAware) {
				actors.push_back(reportErrorsExcept(
				    ddTenantCache->monitorTenantMap(), "DDTenantCacheMonitor", self->ddId, &normalDDQueueErrors()));
			}
			dataDistributionRuntimeMonitor->setTeamCollections(tcis);

			actors.push_back(pollMoveKeysLock(cx, lock, ddEnabledState));
			actors.push_back(reportErrorsExcept(dataDistributionTracker(initData,
			                                                            cx,
			                                                            output,
			                                                            shardsAffectedByTeamFailure,
			                                                            getShardMetrics,
			                                                            getTopKShardMetrics.getFuture(),
			                                                            getShardMetricsList,
			                                                            getAverageShardBytes.getFuture(),
			                                                            readyToStart,
			                                                            anyZeroHealthyTeams,
			                                                            self->ddId,
			                                                            &shards,
			                                                            &trackerCancelled,
			                                                            physicalShardCollection,
			                                                            dataDistributionRuntimeMonitor),
			                                    "DDTracker",
			                                    self->ddId,
			                                    &normalDDQueueErrors()));
			actors.push_back(reportErrorsExcept(dataDistributionQueue(cx,
			                                                          readyToStart.getFuture(),
			                                                          output,
			                                                          input.getFuture(),
			                                                          getShardMetrics,
			                                                          getTopKShardMetrics,
			                                                          processingUnhealthy,
			                                                          processingWiggle,
			                                                          tcis,
			                                                          shardsAffectedByTeamFailure,
			                                                          lock,
			                                                          getAverageShardBytes,
			                                                          getUnhealthyRelocationCount.getFuture(),
			                                                          self->ddId,
			                                                          storageTeamSize,
			                                                          configuration.storageTeamSize,
			                                                          ddEnabledState,
			                                                          physicalShardCollection,
			                                                          dataDistributionRuntimeMonitor),
			                                    "DDQueue",
			                                    self->ddId,
			                                    &normalDDQueueErrors()));

			std::vector<DDTeamCollection*> teamCollectionsPtrs;
			primaryTeamCollection = makeReference<DDTeamCollection>(
			    cx,
			    self->ddId,
			    lock,
			    output,
			    shardsAffectedByTeamFailure,
			    dataDistributionRuntimeMonitor,
			    configuration,
			    primaryDcId,
			    configuration.usableRegions > 1 ? remoteDcIds : std::vector<Optional<Key>>(),
			    readyToStart.getFuture(),
			    zeroHealthyTeams[0],
			    IsPrimary::True,
			    processingUnhealthy,
			    processingWiggle,
			    getShardMetrics,
			    removeFailedServer,
			    getUnhealthyRelocationCount);
			teamCollectionsPtrs.push_back(primaryTeamCollection.getPtr());
			auto recruitStorage = IAsyncListener<RequestStream<RecruitStorageRequest>>::create(
			    self->dbInfo, [](auto const& info) { return info.clusterInterface.recruitStorage; });
			if (configuration.usableRegions > 1) {
				remoteTeamCollection =
				    makeReference<DDTeamCollection>(cx,
				                                    self->ddId,
				                                    lock,
				                                    output,
				                                    shardsAffectedByTeamFailure,
				                                    dataDistributionRuntimeMonitor,
				                                    configuration,
				                                    remoteDcIds,
				                                    Optional<std::vector<Optional<Key>>>(),
				                                    readyToStart.getFuture() && remoteRecovered(self->dbInfo),
				                                    zeroHealthyTeams[1],
				                                    IsPrimary::False,
				                                    processingUnhealthy,
				                                    processingWiggle,
				                                    getShardMetrics,
				                                    removeFailedServer,
				                                    getUnhealthyRelocationCount);
				teamCollectionsPtrs.push_back(remoteTeamCollection.getPtr());
				remoteTeamCollection->teamCollections = teamCollectionsPtrs;
				actors.push_back(reportErrorsExcept(
				    DDTeamCollection::run(remoteTeamCollection, initData, tcis[1], recruitStorage, *ddEnabledState),
				    "DDTeamCollectionSecondary",
				    self->ddId,
				    &normalDDQueueErrors()));
				actors.push_back(DDTeamCollection::printSnapshotTeamsInfo(remoteTeamCollection));
			}
			primaryTeamCollection->teamCollections = teamCollectionsPtrs;
			self->teamCollection = primaryTeamCollection.getPtr();
			actors.push_back(reportErrorsExcept(
			    DDTeamCollection::run(primaryTeamCollection, initData, tcis[0], recruitStorage, *ddEnabledState),
			    "DDTeamCollectionPrimary",
			    self->ddId,
			    &normalDDQueueErrors()));

			actors.push_back(DDTeamCollection::printSnapshotTeamsInfo(primaryTeamCollection));
			actors.push_back(yieldPromiseStream(output.getFuture(), input));
			if (CLIENT_KNOBS->DD_PHYSICAL_SHARD_CORE) {
				actors.push_back(monitorPhysicalShardStatus(cx, physicalShardCollection, dataDistributionRuntimeMonitor));
			}

			wait(waitForAll(actors));
			return Void();
		} catch (Error& e) {
			trackerCancelled = true;
			state Error err = e;
			TraceEvent("DataDistributorDestroyTeamCollections").error(e);
			state std::vector<UID> teamForDroppedRange;
			if (removeFailedServer.getFuture().isReady() && !removeFailedServer.getFuture().isError()) {
				// Choose a random healthy team to host the to-be-dropped range.
				const UID serverID = removeFailedServer.getFuture().get();
				std::vector<UID> pTeam = primaryTeamCollection->getRandomHealthyTeam(serverID);
				teamForDroppedRange.insert(teamForDroppedRange.end(), pTeam.begin(), pTeam.end());
				if (configuration.usableRegions > 1) {
					std::vector<UID> rTeam = remoteTeamCollection->getRandomHealthyTeam(serverID);
					teamForDroppedRange.insert(teamForDroppedRange.end(), rTeam.begin(), rTeam.end());
				}
			}
			self->teamCollection = nullptr;
			primaryTeamCollection = Reference<DDTeamCollection>();
			remoteTeamCollection = Reference<DDTeamCollection>();
			if (err.code() == error_code_actor_cancelled) {
				// When cancelled, we cannot clear asyncronously because
				// this will result in invalid memory access. This should only
				// be an issue in simulation.
				if (!g_network->isSimulated()) {
					TraceEvent(SevWarnAlways, "DataDistributorCancelled");
				}
				shards.clear();
				throw e;
			} else {
				wait(shards.clearAsync());
			}
			TraceEvent("DataDistributorTeamCollectionsDestroyed").error(err);
			if (removeFailedServer.getFuture().isReady() && !removeFailedServer.getFuture().isError()) {
				TraceEvent("RemoveFailedServer", removeFailedServer.getFuture().get()).error(err);
				wait(removeKeysFromFailedServer(
				    cx, removeFailedServer.getFuture().get(), teamForDroppedRange, lock, ddEnabledState));
				Optional<UID> tssPairID;
				wait(removeStorageServer(cx, removeFailedServer.getFuture().get(), tssPairID, lock, ddEnabledState));
			} else {
				if (err.code() != error_code_movekeys_conflict) {
					throw err;
				}

				bool ddEnabled = wait(isDataDistributionEnabled(cx, ddEnabledState));
				TraceEvent("DataDistributionMoveKeysConflict").error(err).detail("DataDistributionEnabled", ddEnabled);
				if (ddEnabled) {
					throw err;
				}
			}
		}
	}
}

static std::set<int> const& normalDataDistributorErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert(error_code_worker_removed);
		s.insert(error_code_broken_promise);
		s.insert(error_code_actor_cancelled);
		s.insert(error_code_please_reboot);
		s.insert(error_code_movekeys_conflict);
		s.insert(error_code_data_move_cancelled);
		s.insert(error_code_data_move_dest_team_not_found);
	}
	return s;
}

ACTOR template <class Req>
Future<Void> sendSnapReq(RequestStream<Req> stream, Req req, Error e) {
	ErrorOr<REPLY_TYPE(Req)> reply = wait(stream.tryGetReply(req));
	if (reply.isError()) {
		TraceEvent("SnapDataDistributor_ReqError")
		    .errorUnsuppressed(reply.getError())
		    .detail("ConvertedErrorType", e.what())
		    .detail("Peer", stream.getEndpoint().getPrimaryAddress());
		throw e;
	}
	return Void();
}

ACTOR template <class Req>
Future<ErrorOr<Void>> trySendSnapReq(RequestStream<Req> stream, Req req) {
	ErrorOr<REPLY_TYPE(Req)> reply = wait(stream.tryGetReply(req));
	if (reply.isError()) {
		TraceEvent("SnapDataDistributor_ReqError")
		    .errorUnsuppressed(reply.getError())
		    .detail("Peer", stream.getEndpoint().getPrimaryAddress());
		return ErrorOr<Void>(reply.getError());
	}
	return ErrorOr<Void>(Void());
}

ACTOR static Future<Void> waitForMost(std::vector<Future<ErrorOr<Void>>> futures,
                                      int faultTolerance,
                                      Error e,
                                      double waitMultiplierForSlowFutures = 1.0) {
	state std::vector<Future<bool>> successFutures;
	state double startTime = now();
	successFutures.reserve(futures.size());
	for (const auto& future : futures) {
		successFutures.push_back(fmap([](auto const& result) { return result.present(); }, future));
	}
	bool success = wait(quorumEqualsTrue(successFutures, successFutures.size() - faultTolerance));
	if (!success) {
		throw e;
	}
	wait(delay((now() - startTime) * waitMultiplierForSlowFutures) || waitForAll(successFutures));
	return Void();
}

ACTOR Future<Void> ddSnapCreateCore(DistributorSnapRequest snapReq, Reference<AsyncVar<ServerDBInfo> const> db) {
	state Database cx = openDBOnServer(db, TaskPriority::DefaultDelay, LockAware::True);

	state ReadYourWritesTransaction tr(cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			TraceEvent("SnapDataDistributor_WriteFlagAttempt")
			    .detail("SnapPayload", snapReq.snapPayload)
			    .detail("SnapUID", snapReq.snapUID);
			tr.set(writeRecoveryKey, writeRecoveryKeyTrue);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			TraceEvent("SnapDataDistributor_WriteFlagError").error(e);
			wait(tr.onError(e));
		}
	}
	TraceEvent("SnapDataDistributor_SnapReqEnter")
	    .detail("SnapPayload", snapReq.snapPayload)
	    .detail("SnapUID", snapReq.snapUID);
	try {
		// disable tlog pop on local tlog nodes
		state std::vector<TLogInterface> tlogs = db->get().logSystemConfig.allLocalLogs(false);
		std::vector<Future<Void>> disablePops;
		disablePops.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			disablePops.push_back(sendSnapReq(
			    tlog.disablePopRequest, TLogDisablePopRequest{ snapReq.snapUID }, snap_disable_tlog_pop_failed()));
		}
		wait(waitForAll(disablePops));

		TraceEvent("SnapDataDistributor_AfterDisableTLogPop")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap local storage nodes
		// TODO: Atomically read  configuration and storage worker list in a single transaction
		state DatabaseConfiguration configuration = wait(getDatabaseConfiguration(cx));
		std::pair<std::vector<WorkerInterface>, int> storageWorkersAndFailures =
		    wait(transformErrors(getStorageWorkers(cx, db, true /* localOnly */), snap_storage_failed()));
		const auto& [storageWorkers, storageFailures] = storageWorkersAndFailures;
		auto const storageFaultTolerance =
		    std::min(static_cast<int>(SERVER_KNOBS->MAX_STORAGE_SNAPSHOT_FAULT_TOLERANCE),
		             configuration.storageTeamSize - 1) -
		    storageFailures;
		if (storageFaultTolerance < 0) {
			TEST(true); // Too many failed storage servers to complete snapshot
			throw snap_storage_failed();
		}
		TraceEvent("SnapDataDistributor_GotStorageWorkers")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		std::vector<Future<ErrorOr<Void>>> storageSnapReqs;
		storageSnapReqs.reserve(storageWorkers.size());
		for (const auto& worker : storageWorkers) {
			storageSnapReqs.push_back(trySendSnapReq(
			    worker.workerSnapReq, WorkerSnapRequest(snapReq.snapPayload, snapReq.snapUID, "storage"_sr)));
		}
		wait(waitForMost(storageSnapReqs, storageFaultTolerance, snap_storage_failed()));

		TraceEvent("SnapDataDistributor_AfterSnapStorage")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap local tlog nodes
		std::vector<Future<Void>> tLogSnapReqs;
		tLogSnapReqs.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			tLogSnapReqs.push_back(sendSnapReq(tlog.snapRequest,
			                                   TLogSnapRequest{ snapReq.snapPayload, snapReq.snapUID, "tlog"_sr },
			                                   snap_tlog_failed()));
		}
		wait(waitForAll(tLogSnapReqs));

		TraceEvent("SnapDataDistributor_AfterTLogStorage")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// enable tlog pop on local tlog nodes
		std::vector<Future<Void>> enablePops;
		enablePops.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			enablePops.push_back(sendSnapReq(
			    tlog.enablePopRequest, TLogEnablePopRequest{ snapReq.snapUID }, snap_enable_tlog_pop_failed()));
		}
		wait(waitForAll(enablePops));

		TraceEvent("SnapDataDistributor_AfterEnableTLogPops")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap the coordinators
		std::vector<WorkerInterface> coordWorkers = wait(getCoordWorkers(cx, db));
		TraceEvent("SnapDataDistributor_GotCoordWorkers")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		std::vector<Future<ErrorOr<Void>>> coordSnapReqs;
		coordSnapReqs.reserve(coordWorkers.size());
		for (const auto& worker : coordWorkers) {
			coordSnapReqs.push_back(trySendSnapReq(
			    worker.workerSnapReq, WorkerSnapRequest(snapReq.snapPayload, snapReq.snapUID, "coord"_sr)));
		}
		auto const coordFaultTolerance = std::min<int>(std::max<int>(0, coordSnapReqs.size() / 2 - 1),
		                                               SERVER_KNOBS->MAX_COORDINATOR_SNAPSHOT_FAULT_TOLERANCE);
		wait(waitForMost(coordSnapReqs, coordFaultTolerance, snap_coord_failed()));
		TraceEvent("SnapDataDistributor_AfterSnapCoords")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		tr.reset();
		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				TraceEvent("SnapDataDistributor_ClearFlagAttempt")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				tr.clear(writeRecoveryKey);
				wait(tr.commit());
				break;
			} catch (Error& e) {
				TraceEvent("SnapDataDistributor_ClearFlagError").error(e);
				wait(tr.onError(e));
			}
		}
	} catch (Error& err) {
		state Error e = err;
		TraceEvent("SnapDataDistributor_SnapReqExit")
		    .errorUnsuppressed(e)
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		if (e.code() == error_code_snap_storage_failed || e.code() == error_code_snap_tlog_failed ||
		    e.code() == error_code_operation_cancelled || e.code() == error_code_snap_disable_tlog_pop_failed) {
			// enable tlog pop on local tlog nodes
			std::vector<TLogInterface> tlogs = db->get().logSystemConfig.allLocalLogs(false);
			try {
				std::vector<Future<Void>> enablePops;
				enablePops.reserve(tlogs.size());
				for (const auto& tlog : tlogs) {
					enablePops.push_back(transformErrors(
					    throwErrorOr(tlog.enablePopRequest.tryGetReply(TLogEnablePopRequest(snapReq.snapUID))),
					    snap_enable_tlog_pop_failed()));
				}
				wait(waitForAll(enablePops));
			} catch (Error& error) {
				TraceEvent(SevDebug, "IgnoreEnableTLogPopFailure").log();
			}
		}
		throw e;
	}
	return Void();
}

ACTOR Future<Void> ddSnapCreate(DistributorSnapRequest snapReq,
                                Reference<AsyncVar<ServerDBInfo> const> db,
                                DDEnabledState* ddEnabledState) {
	state Future<Void> dbInfoChange = db->onChange();
	if (!ddEnabledState->setDDEnabled(false, snapReq.snapUID)) {
		// disable DD before doing snapCreate, if previous snap req has already disabled DD then this operation fails
		// here
		TraceEvent("SnapDDSetDDEnabledFailedInMemoryCheck").log();
		snapReq.reply.sendError(operation_failed());
		return Void();
	}
	double delayTime = g_network->isSimulated() ? 70.0 : SERVER_KNOBS->SNAP_CREATE_MAX_TIMEOUT;
	try {
		choose {
			when(wait(dbInfoChange)) {
				TraceEvent("SnapDDCreateDBInfoChanged")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.sendError(snap_with_recovery_unsupported());
			}
			when(wait(ddSnapCreateCore(snapReq, db))) {
				TraceEvent("SnapDDCreateSuccess")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.send(Void());
			}
			when(wait(delay(delayTime))) {
				TraceEvent("SnapDDCreateTimedOut")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.sendError(timed_out());
			}
		}
	} catch (Error& e) {
		TraceEvent("SnapDDCreateError")
		    .errorUnsuppressed(e)
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		if (e.code() != error_code_operation_cancelled) {
			snapReq.reply.sendError(e);
		} else {
			// enable DD should always succeed
			bool success = ddEnabledState->setDDEnabled(true, snapReq.snapUID);
			ASSERT(success);
			throw e;
		}
	}
	// enable DD should always succeed
	bool success = ddEnabledState->setDDEnabled(true, snapReq.snapUID);
	ASSERT(success);
	return Void();
}

ACTOR Future<Void> ddExclusionSafetyCheck(DistributorExclusionSafetyCheckRequest req,
                                          Reference<DataDistributorData> self,
                                          Database cx) {
	TraceEvent("DDExclusionSafetyCheckBegin", self->ddId).log();
	std::vector<StorageServerInterface> ssis = wait(getStorageServers(cx));
	DistributorExclusionSafetyCheckReply reply(true);
	if (!self->teamCollection) {
		TraceEvent("DDExclusionSafetyCheckTeamCollectionInvalid", self->ddId).log();
		reply.safe = false;
		req.reply.send(reply);
		return Void();
	}
	// If there is only 1 team, unsafe to mark failed: team building can get stuck due to lack of servers left
	if (self->teamCollection->teams.size() <= 1) {
		TraceEvent("DDExclusionSafetyCheckNotEnoughTeams", self->ddId).log();
		reply.safe = false;
		req.reply.send(reply);
		return Void();
	}
	std::vector<UID> excludeServerIDs;
	// Go through storage server interfaces and translate Address -> server ID (UID)
	for (const AddressExclusion& excl : req.exclusions) {
		for (const auto& ssi : ssis) {
			if (excl.excludes(ssi.address()) ||
			    (ssi.secondaryAddress().present() && excl.excludes(ssi.secondaryAddress().get()))) {
				excludeServerIDs.push_back(ssi.id());
			}
		}
	}
	reply.safe = self->teamCollection->exclusionSafetyCheck(excludeServerIDs);
	TraceEvent("DDExclusionSafetyCheckFinish", self->ddId).log();
	req.reply.send(reply);
	return Void();
}

ACTOR Future<Void> waitFailCacheServer(Database* db, StorageServerInterface ssi) {
	state Transaction tr(*db);
	state Key key = storageCacheServerKey(ssi.id());
	wait(waitFailureClient(ssi.waitFailure));
	loop {
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		try {
			tr.addReadConflictRange(storageCacheServerKeys);
			tr.clear(key);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
	return Void();
}

ACTOR Future<Void> cacheServerWatcher(Database* db) {
	state Transaction tr(*db);
	state ActorCollection actors(false);
	state std::set<UID> knownCaches;
	loop {
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		try {
			RangeResult range = wait(tr.getRange(storageCacheServerKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!range.more);
			std::set<UID> caches;
			for (auto& kv : range) {
				UID id;
				BinaryReader reader{ kv.key.removePrefix(storageCacheServersPrefix), Unversioned() };
				reader >> id;
				caches.insert(id);
				if (knownCaches.find(id) == knownCaches.end()) {
					StorageServerInterface ssi;
					BinaryReader reader{ kv.value, IncludeVersion() };
					reader >> ssi;
					actors.add(waitFailCacheServer(db, ssi));
				}
			}
			knownCaches = std::move(caches);
			tr.reset();
			wait(delay(5.0) || actors.getResult());
			ASSERT(!actors.getResult().isReady());
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

static int64_t getMedianShardSize(VectorRef<DDMetricsRef> metricVec) {
	std::nth_element(metricVec.begin(),
	                 metricVec.begin() + metricVec.size() / 2,
	                 metricVec.end(),
	                 [](const DDMetricsRef& d1, const DDMetricsRef& d2) { return d1.shardBytes < d2.shardBytes; });
	return metricVec[metricVec.size() / 2].shardBytes;
}

GetStorageWigglerStateReply getStorageWigglerStates(Reference<DataDistributorData> self) {
	GetStorageWigglerStateReply reply;
	if (self->teamCollection) {
		std::tie(reply.primary, reply.lastStateChangePrimary) = self->teamCollection->getStorageWigglerState();
		if (self->teamCollection->teamCollections.size() > 1) {
			std::tie(reply.remote, reply.lastStateChangeRemote) =
			    self->teamCollection->teamCollections[1]->getStorageWigglerState();
		}
	}
	return reply;
}

ACTOR Future<Void> ddGetMetrics(GetDataDistributorMetricsRequest req,
                                PromiseStream<GetMetricsListRequest> getShardMetricsList) {
	ErrorOr<Standalone<VectorRef<DDMetricsRef>>> result = wait(
	    errorOr(brokenPromiseToNever(getShardMetricsList.getReply(GetMetricsListRequest(req.keys, req.shardLimit)))));

	if (result.isError()) {
		req.reply.sendError(result.getError());
	} else {
		GetDataDistributorMetricsReply rep;
		if (!req.midOnly) {
			rep.storageMetricsList = result.get();
		} else {
			auto& metricVec = result.get();
			if (metricVec.empty())
				rep.midShardSize = 0;
			else {
				rep.midShardSize = getMedianShardSize(metricVec.contents());
			}
		}
		req.reply.send(rep);
	}

	return Void();
}

ACTOR Future<Void> dataDistributor(DataDistributorInterface di, Reference<AsyncVar<ServerDBInfo> const> db) {
	state Reference<DataDistributorData> self(new DataDistributorData(db, di.id()));
	state Future<Void> collection = actorCollection(self->addActor.getFuture());
	state PromiseStream<GetMetricsListRequest> getShardMetricsList;
	state Database cx = openDBOnServer(db, TaskPriority::DefaultDelay, LockAware::True);
	state ActorCollection actors(false);
	state DDEnabledState ddEnabledState;
	self->addActor.send(actors.getResult());
	self->addActor.send(traceRole(Role::DATA_DISTRIBUTOR, di.id()));

	try {
		TraceEvent("DataDistributorRunning", di.id());
		self->addActor.send(waitFailureServer(di.waitFailure.getFuture()));
		self->addActor.send(cacheServerWatcher(&cx));
		state Future<Void> distributor =
		    reportErrorsExcept(dataDistribution(self, getShardMetricsList, &ddEnabledState),
		                       "DataDistribution",
		                       di.id(),
		                       &normalDataDistributorErrors());

		loop choose {
			when(wait(distributor || collection)) {
				ASSERT(false);
				throw internal_error();
			}
			when(HaltDataDistributorRequest req = waitNext(di.haltDataDistributor.getFuture())) {
				req.reply.send(Void());
				TraceEvent("DataDistributorHalted", di.id()).detail("ReqID", req.requesterID);
				break;
			}
			when(GetDataDistributorMetricsRequest req = waitNext(di.dataDistributorMetrics.getFuture())) {
				actors.add(ddGetMetrics(req, getShardMetricsList));
			}
			when(DistributorSnapRequest snapReq = waitNext(di.distributorSnapReq.getFuture())) {
				actors.add(ddSnapCreate(snapReq, db, &ddEnabledState));
			}
			when(DistributorExclusionSafetyCheckRequest exclCheckReq =
			         waitNext(di.distributorExclCheckReq.getFuture())) {
				actors.add(ddExclusionSafetyCheck(exclCheckReq, self, cx));
			}
			when(GetStorageWigglerStateRequest req = waitNext(di.storageWigglerState.getFuture())) {
				req.reply.send(getStorageWigglerStates(self));
			}
		}
	} catch (Error& err) {
		if (normalDataDistributorErrors().count(err.code()) == 0) {
			TraceEvent("DataDistributorError", di.id()).errorUnsuppressed(err);
			throw err;
		}
		TraceEvent("DataDistributorDied", di.id()).errorUnsuppressed(err);
	}

	return Void();
}

static Future<ErrorOr<Void>> goodTestFuture(double duration) {
	return tag(delay(duration), ErrorOr<Void>(Void()));
}

static Future<ErrorOr<Void>> badTestFuture(double duration, Error e) {
	return tag(delay(duration), ErrorOr<Void>(e));
}

TEST_CASE("/DataDistribution/WaitForMost") {
	state std::vector<Future<ErrorOr<Void>>> futures;
	{
		futures = { goodTestFuture(1), goodTestFuture(2), goodTestFuture(3) };
		wait(waitForMost(futures, 1, operation_failed(), 0.0)); // Don't wait for slowest future
		ASSERT(!futures[2].isReady());
	}
	{
		futures = { goodTestFuture(1), goodTestFuture(2), goodTestFuture(3) };
		wait(waitForMost(futures, 0, operation_failed(), 0.0)); // Wait for all futures
		ASSERT(futures[2].isReady());
	}
	{
		futures = { goodTestFuture(1), goodTestFuture(2), goodTestFuture(3) };
		wait(waitForMost(futures, 1, operation_failed(), 1.0)); // Wait for slowest future
		ASSERT(futures[2].isReady());
	}
	{
		futures = { goodTestFuture(1), goodTestFuture(2), badTestFuture(1, success()) };
		wait(waitForMost(futures, 1, operation_failed(), 1.0)); // Error ignored
	}
	{
		futures = { goodTestFuture(1), goodTestFuture(2), badTestFuture(1, success()) };
		try {
			wait(waitForMost(futures, 0, operation_failed(), 1.0));
			ASSERT(false);
		} catch (Error& e) {
			ASSERT_EQ(e.code(), error_code_operation_failed);
		}
	}
	return Void();
}

TEST_CASE("/DataDistributor/StorageWiggler/Order") {
	StorageWiggler wiggler(nullptr);
	wiggler.addServer(UID(1, 0), StorageMetadataType(1, KeyValueStoreType::SSD_BTREE_V2));
	wiggler.addServer(UID(2, 0), StorageMetadataType(2, KeyValueStoreType::MEMORY, true));
	wiggler.addServer(UID(3, 0), StorageMetadataType(3, KeyValueStoreType::SSD_ROCKSDB_V1, true));
	wiggler.addServer(UID(4, 0), StorageMetadataType(4, KeyValueStoreType::SSD_BTREE_V2));

	std::vector<UID> correctOrder{ UID(2, 0), UID(3, 0), UID(1, 0), UID(4, 0) };
	for (int i = 0; i < correctOrder.size(); ++i) {
		auto id = wiggler.getNextServerId();
		std::cout << "Get " << id.get().shortString() << "\n";
		ASSERT(id == correctOrder[i]);
	}
	ASSERT(!wiggler.getNextServerId().present());
	return Void();
}
