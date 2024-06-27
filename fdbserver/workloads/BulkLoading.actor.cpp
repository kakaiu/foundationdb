/*
 * BulkLoading.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2024 Apple Inc. and the FoundationDB project authors
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

#include "fdbclient/BulkLoading.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/BulkLoadUtil.actor.h"
#include "fdbserver/RocksDBCheckpointUtils.actor.h"
#include "fdbserver/StorageMetrics.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

const std::string simulationBulkLoadFolder = "bulkLoad";

struct BulkLoadTaskTestUnit {
	BulkLoadState bulkLoadTask;
	std::vector<KeyValue> data;
	BulkLoadTaskTestUnit() = default;
};

struct BulkLoading : TestWorkload {
	static constexpr auto NAME = "BulkLoadingWorkload";
	const bool enabled;
	bool pass;

	// This workload is not compatible with following workload because they will race in changing the DD mode
	void disableFailureInjectionWorkloads(std::set<std::string>& out) const override {
		out.insert({ "RandomMoveKeys",
		             "DataLossRecovery",
		             "IDDTxnProcessorApiCorrectness",
		             "PerpetualWiggleStatsWorkload",
		             "PhysicalShardMove",
		             "StorageCorruption",
		             "StorageServerCheckpointRestoreTest",
		             "ValidateStorage" });
	}

	BulkLoading(WorkloadContext const& wcx) : TestWorkload(wcx), enabled(true), pass(true) {}

	Future<Void> setup(Database const& cx) override { return Void(); }

	Future<Void> start(Database const& cx) override { return _start(this, cx); }

	Future<bool> check(Database const& cx) override { return true; }

	void getMetrics(std::vector<PerfMetric>& m) override {}

	bool allComplete(RangeResult input) {
		TraceEvent e("BulkLoadingCheckStatusAllComplete");
		bool res = true;
		for (int i = 0; i < input.size() - 1; i++) {
			TraceEvent e("BulkLoadingCheckStatus");
			e.detail("Range", Standalone(KeyRangeRef(input[i].key, input[i + 1].key)));
			if (!input[i].value.empty()) {
				BulkLoadState bulkLoadState = decodeBulkLoadState(input[i].value);
				ASSERT(bulkLoadState.isValid());
				e.detail("BulkLoadState", bulkLoadState.toString());
				if (bulkLoadState.phase != BulkLoadPhase::Complete) {
					res = false;
					e.detail("Status", "Running");
				} else {
					e.detail("Status", "Complete");
				}
			} else {
				e.detail("Status", "N/A");
			}
		}
		return res;
	}

	ACTOR Future<Void> issueBulkLoadTasksFdbcli(BulkLoading* self, Database cx, std::vector<BulkLoadState> tasks) {
		state int i = 0;
		for (; i < tasks.size(); i++) {
			loop {
				try {
					wait(submitBulkLoadTask(cx->getConnectionRecord(), tasks[i], /*timeoutSecond=*/300));
					TraceEvent("BulkLoadingIssueBulkLoadTask").detail("BulkLoadStates", describe(tasks[i]));
					break;
				} catch (Error& e) {
					TraceEvent("BulkLoadingIssueBulkLoadTaskError")
					    .errorUnsuppressed(e)
					    .detail("BulkLoadStates", describe(tasks));
					wait(delay(5.0));
				}
			}
		}
		return Void();
	}

	ACTOR Future<Void> issueBulkLoadTasksTr(BulkLoading* self, Database cx, std::vector<BulkLoadState> tasks) {
		state Transaction tr(cx);
		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				for (auto task : tasks) {
					wait(krmSetRange(&tr, bulkLoadPrefix, task.getRange(), bulkLoadStateValue(task)));
				}
				wait(tr.commit());
				TraceEvent("BulkLoadingIssueBulkLoadTask").detail("BulkLoadStates", describe(tasks));
				break;
			} catch (Error& e) {
				TraceEvent("BulkLoadingIssueBulkLoadTaskError")
				    .errorUnsuppressed(e)
				    .detail("BulkLoadStates", describe(tasks));
				wait(tr.onError(e));
			}
		}
		return Void();
	}

	ACTOR Future<Void> issueBulkLoadTasks(BulkLoading* self, Database cx, std::vector<BulkLoadState> tasks) {
		if (deterministicRandom()->coinflip()) {
			wait(self->issueBulkLoadTasksTr(self, cx, tasks));
		} else {
			wait(self->issueBulkLoadTasksFdbcli(self, cx, tasks));
		}
		return Void();
	}

	Key getRandomKey(const std::vector<Key>& keyCharList, size_t keySizeMin, size_t keySizeMax) {
		Key key = ""_sr;
		int keyLength = deterministicRandom()->randomInt(keySizeMin, keySizeMax);
		for (int j = 0; j < keyLength; j++) {
			Key appendedItem = deterministicRandom()->randomChoice(keyCharList);
			key = key.withSuffix(appendedItem);
		}
		return key;
	}

	std::vector<KeyValue> generateRandomData(KeyRange range, size_t count, const std::vector<Key>& keyCharList) {
		std::set<Key> keys;
		while (keys.size() < count) {
			Key key = getRandomKey(keyCharList, 1, 1000);
			if (!range.contains(key)) {
				continue;
			}
			keys.insert(key);
		}
		std::vector<KeyValue> res;
		for (const auto& key : keys) {
			UID randomId = deterministicRandom()->randomUniqueID();
			Value val = Standalone(StringRef(randomId.toString()));
			res.push_back(Standalone(KeyValueRef(key, val)));
		}
		ASSERT(res.size() == count);
		return res;
	}

	void produceFilesToLoad(BulkLoadTaskTestUnit task) {
		std::string folder = task.bulkLoadTask.getFolder();
		platform::eraseDirectoryRecursive(folder);
		ASSERT(platform::createDirectory(folder));
		std::string bytesSampleFile = task.bulkLoadTask.getBytesSampleFile().get();
		std::string dataFile = *(task.bulkLoadTask.getDataFiles().begin());

		std::unique_ptr<IRocksDBSstFileWriter> sstWriter = newRocksDBSstFileWriter();
		sstWriter->open(abspath(dataFile));
		std::vector<KeyValue> bytesSample;
		for (const auto& kv : task.data) {
			ByteSampleInfo sampleInfo = isKeyValueInSample(kv);
			if (sampleInfo.inSample) {
				Key sampleKey = kv.key;
				Value sampleValue = BinaryWriter::toValue(sampleInfo.sampledSize, Unversioned());
				bytesSample.push_back(Standalone(KeyValueRef(sampleKey, sampleValue)));
			}
			sstWriter->write(kv.key, kv.value);
		}
		TraceEvent("BulkLoadingDataProduced")
		    .detail("LoadKeyCount", task.data.size())
		    .detail("BytesSampleSize", bytesSample.size())
		    .detail("Folder", folder)
		    .detail("DataFile", dataFile)
		    .detail("BytesSampleFile", bytesSampleFile);
		ASSERT(sstWriter->finish());

		if (bytesSample.size() > 0) {
			sstWriter->open(abspath(bytesSampleFile));
			for (const auto& kv : bytesSample) {
				sstWriter->write(kv.key, kv.value);
			}
			TraceEvent("BulkLoadingByteSampleProduced")
			    .detail("LoadKeyCount", task.data.size())
			    .detail("BytesSampleSize", bytesSample.size())
			    .detail("Folder", folder)
			    .detail("DataFile", dataFile)
			    .detail("BytesSampleFile", bytesSampleFile);
			ASSERT(sstWriter->finish());
		}
		TraceEvent("BulkLoadingProduceDataToLoad").detail("Folder", folder).detail("LoadKeyCount", task.data.size());
		return;
	}

	ACTOR Future<bool> checkDDEnabled(Database cx) {
		loop {
			state Transaction tr(cx);
			tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			try {
				state int ddMode = 1;
				Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
				if (mode.present()) {
					BinaryReader rd(mode.get(), Unversioned());
					rd >> ddMode;
				}
				return ddMode == 1;

			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	ACTOR Future<bool> allComplete(Database cx) {
		state Transaction tr(cx);
		state Key beginKey = allKeys.begin;
		state Key endKey = allKeys.end;
		while (beginKey < endKey) {
			try {
				tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				RangeResult res = wait(krmGetRanges(&tr,
				                                    bulkLoadPrefix,
				                                    Standalone(KeyRangeRef(beginKey, endKey)),
				                                    CLIENT_KNOBS->KRM_GET_RANGE_LIMIT,
				                                    CLIENT_KNOBS->KRM_GET_RANGE_LIMIT_BYTES));
				for (int i = 0; i < res.size() - 1; i++) {
					if (!res[i].value.empty()) {
						BulkLoadState bulkLoadState = decodeBulkLoadState(res[i].value);
						ASSERT(bulkLoadState.isValid());
						if (bulkLoadState.phase != BulkLoadPhase::Complete) {
							return false;
						}
					}
				}
				beginKey = res[res.size() - 1].key;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
		return true;
	}

	ACTOR Future<Void> waitUntilAllComplete(BulkLoading* self, Database cx) {
		loop {
			bool complete = wait(self->allComplete(cx));
			if (complete) {
				break;
			}
			bool ddEnabled = wait(self->checkDDEnabled(cx));
			if (!ddEnabled) {
				throw timed_out();
			}
			wait(delay(10.0));
		}
		return Void();
	}

	ACTOR Future<Void> checkData(Database cx, std::vector<KeyValue> kvs) {
		state Key keyRead;
		state Transaction tr(cx);
		state int i = 0;
		loop {
			try {
				Optional<Value> value = wait(tr.get(kvs[i].key));
				if (!value.present() || value.get() != kvs[i].value) {
					TraceEvent(SevError, "BulkLoadingWorkLoadValueError")
					    .detail("Version", tr.getReadVersion().get())
					    .detail("ToCheckCount", kvs.size())
					    .detail("Key", kvs[i].key.toString())
					    .detail("ExpectedValue", kvs[i].value.toString())
					    .detail("Value", value.present() ? value.get().toString() : "None");
				}
				i = i + 1;
				if (i >= kvs.size()) {
					break;
				}
			} catch (Error& e) {
				TraceEvent(SevInfo, "BulkLoadingWorkLoadValueError").errorUnsuppressed(e);
				wait(tr.onError(e));
			}
		}
		return Void();
	}

	BulkLoadTaskTestUnit produceBulkLoadTaskUnit(BulkLoading* self,
	                                             const std::vector<Key>& keyCharList,
	                                             KeyRange range,
	                                             std::string folderName) {
		std::string dataFileName = generateRandomBulkLoadDataFileName();
		std::string bytesSampleFileName = generateRandomBulkLoadBytesSampleFileName();
		std::string folder = joinPath(simulationBulkLoadFolder, folderName);
		BulkLoadTaskTestUnit taskUnit;
		taskUnit.bulkLoadTask = newBulkLoadTaskLocalSST(
		    range, folder, joinPath(folder, dataFileName), joinPath(folder, bytesSampleFileName));
		size_t dataSize = deterministicRandom()->randomInt(10, 100);
		taskUnit.data = self->generateRandomData(range, dataSize, keyCharList);
		self->produceFilesToLoad(taskUnit);
		return taskUnit;
	}

	std::vector<KeyValue> generateSortedKVS(StringRef prefix, size_t count) {
		std::vector<KeyValue> res;
		for (int i = 0; i < count; i++) {
			UID keyId = deterministicRandom()->randomUniqueID();
			Value key = Standalone(StringRef(keyId.toString())).withPrefix(prefix);
			UID valueId = deterministicRandom()->randomUniqueID();
			Value val = Standalone(StringRef(valueId.toString()));
			res.push_back(Standalone(KeyValueRef(key, val)));
		}
		std::sort(res.begin(), res.end(), [](KeyValue a, KeyValue b) { return a.key < b.key; });
		return res;
	}

	void produceLargeDataToLoad(BulkLoadTaskTestUnit task, int count) {
		std::string folder = task.bulkLoadTask.getFolder();
		platform::eraseDirectoryRecursive(folder);
		ASSERT(platform::createDirectory(folder));
		std::string bytesSampleFile = task.bulkLoadTask.getBytesSampleFile().get();
		std::string dataFile = *(task.bulkLoadTask.getDataFiles().begin());

		std::unique_ptr<IRocksDBSstFileWriter> sstWriter = newRocksDBSstFileWriter();
		sstWriter->open(abspath(dataFile));
		std::vector<KeyValue> bytesSample;
		int insertedKeyCount = 0;
		for (int i = 0; i < 10; i++) {
			std::string idxStr = std::to_string(i);
			Key prefix = Standalone(StringRef(idxStr)).withPrefix(task.bulkLoadTask.getRange().begin);
			std::vector<KeyValue> kvs = generateSortedKVS(prefix, std::max(count / 10, 1));
			for (const auto& kv : kvs) {
				ByteSampleInfo sampleInfo = isKeyValueInSample(kv);
				if (sampleInfo.inSample) {
					Key sampleKey = kv.key;
					Value sampleValue = BinaryWriter::toValue(sampleInfo.sampledSize, Unversioned());
					bytesSample.push_back(Standalone(KeyValueRef(sampleKey, sampleValue)));
				}
				sstWriter->write(kv.key, kv.value);
				insertedKeyCount++;
			}
		}
		TraceEvent("BulkLoadingDataProduced")
		    .detail("LoadKeyCount", insertedKeyCount)
		    .detail("BytesSampleSize", bytesSample.size())
		    .detail("Folder", folder)
		    .detail("DataFile", dataFile)
		    .detail("BytesSampleFile", bytesSampleFile);
		ASSERT(sstWriter->finish());

		if (bytesSample.size() > 0) {
			sstWriter->open(abspath(bytesSampleFile));
			for (const auto& kv : bytesSample) {
				sstWriter->write(kv.key, kv.value);
			}
			TraceEvent("BulkLoadingByteSampleProduced")
			    .detail("LoadKeyCount", task.data.size())
			    .detail("BytesSampleSize", bytesSample.size())
			    .detail("Folder", folder)
			    .detail("DataFile", dataFile)
			    .detail("BytesSampleFile", bytesSampleFile);
			ASSERT(sstWriter->finish());
		}
	}

	void produceDataSet(BulkLoading* self, KeyRange range, std::string folderName) {
		std::string dataFileName = generateRandomBulkLoadDataFileName();
		std::string bytesSampleFileName = generateRandomBulkLoadBytesSampleFileName();
		std::string folder = joinPath(simulationBulkLoadFolder, folderName);
		BulkLoadTaskTestUnit taskUnit;
		taskUnit.bulkLoadTask = newBulkLoadTaskLocalSST(range, folder, dataFileName, bytesSampleFileName);
		self->produceLargeDataToLoad(taskUnit, 5000000);
		return;
	}

	// Issue three non-overlapping tasks and check data consistency and correctness
	// Repeat twice
	ACTOR Future<Void> simpleTest(BulkLoading* self, Database cx) {
		TraceEvent("BulkLoadingWorkLoadSimpleTestBegin");
		state std::vector<Key> keyCharList = { "0"_sr, "1"_sr, "2"_sr, "3"_sr, "4"_sr, "5"_sr };
		// First round of issuing tasks
		state std::vector<BulkLoadState> bulkLoadStates;
		state std::vector<std::vector<KeyValue>> bulkLoadDataList;
		for (int i = 0; i < 3; i++) {
			std::string strIdx = std::to_string(i);
			std::string strIdxPlusOne = std::to_string(i + 1);
			std::string folderName = strIdx;
			Key beginKey = Standalone(StringRef(strIdx));
			Key endKey = Standalone(StringRef(strIdxPlusOne));
			KeyRange range = Standalone(KeyRangeRef(beginKey, endKey));
			BulkLoadTaskTestUnit taskUnit = self->produceBulkLoadTaskUnit(self, keyCharList, range, folderName);
			bulkLoadStates.push_back(taskUnit.bulkLoadTask);
			bulkLoadDataList.push_back(taskUnit.data);
		}
		wait(self->issueBulkLoadTasks(self, cx, bulkLoadStates));
		TraceEvent("BulkLoadingWorkLoadSimpleTestIssuedTasks");
		int oldDDMode = wait(setDDMode(cx, 1));
		TraceEvent("BulkLoadingWorkLoadSimpleTestSetDDMode").detail("OldMode", oldDDMode).detail("NewMode", 1);
		int old1 = wait(setBulkLoadMode(cx, 1));
		TraceEvent("BulkLoadingWorkLoadSimpleTestSetMode").detail("OldMode", old1).detail("NewMode", 1);
		try {
			wait(self->waitUntilAllComplete(self, cx));
			TraceEvent("BulkLoadingWorkLoadSimpleTestAllComplete");
		} catch (Error& e) {
			if (e.code() == error_code_timed_out) {
				return Void();
			}
		}
		// Second round of issuing tasks
		bulkLoadStates.clear();
		bulkLoadDataList.clear();
		for (int i = 0; i < 3; i++) {
			std::string strIdx = std::to_string(i);
			std::string strIdxPlusOne = std::to_string(i + 1);
			std::string folderName = strIdx;
			Key beginKey = Standalone(StringRef(strIdx));
			Key endKey = Standalone(StringRef(strIdxPlusOne));
			KeyRange range = Standalone(KeyRangeRef(beginKey, endKey));
			BulkLoadTaskTestUnit taskUnit = self->produceBulkLoadTaskUnit(self, keyCharList, range, folderName);
			bulkLoadStates.push_back(taskUnit.bulkLoadTask);
			bulkLoadDataList.push_back(taskUnit.data);
		}
		wait(self->issueBulkLoadTasks(self, cx, bulkLoadStates));
		TraceEvent("BulkLoadingWorkLoadSimpleTestIssuedTasks");
		try {
			wait(self->waitUntilAllComplete(self, cx));
			TraceEvent("BulkLoadingWorkLoadSimpleTestAllComplete");
		} catch (Error& e) {
			if (e.code() == error_code_timed_out) {
				return Void();
			}
		}
		int old2 = wait(setBulkLoadMode(cx, 0));
		TraceEvent("BulkLoadingWorkLoadSimpleTestSetMode").detail("OldMode", old2).detail("NewMode", 0);
		state int j = 0;
		for (; j < bulkLoadDataList.size(); j++) {
			wait(self->checkData(cx, bulkLoadDataList[j]));
		}
		TraceEvent("BulkLoadingWorkLoadSimpleTestComplete");
		return Void();
	}

	void produceLargeData(BulkLoading* self, Database cx) {
		std::string folderName1 = "1";
		KeyRange range1 = Standalone(KeyRangeRef("1"_sr, "2"_sr));
		self->produceDataSet(self, range1, folderName1);
		std::string folderName2 = "2";
		KeyRange range2 = Standalone(KeyRangeRef("2"_sr, "3"_sr));
		self->produceDataSet(self, range2, folderName2);
		std::string folderName3 = "4";
		KeyRange range3 = Standalone(KeyRangeRef("4"_sr, "5"_sr));
		self->produceDataSet(self, range3, folderName3);
		return;
	}

	ACTOR Future<Void> _start(BulkLoading* self, Database cx) {
		if (self->clientId != 0) {
			return Void();
		}

		if (g_network->isSimulated()) {
			// Network partition between CC and DD can cause DD no longer existing,
			// which results in the bulk loading task cannot complete
			disableConnectionFailures("BulkLoading");
		}

		wait(self->simpleTest(self, cx));
		// self->produceLargeData(self, cx); // Produce data set that is used in loop back cluster test

		return Void();
	}
};

WorkloadFactory<BulkLoading> BulkLoadingFactory;