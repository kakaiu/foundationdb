/*
 * AccumulativeChecksumUtil.h
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

#ifndef ACCUMULATIVECHECKSUMUTIL_H
#define ACCUMULATIVECHECKSUMUTIL_H
#pragma once

#include "fdbclient/AccumulativeChecksum.h"
#include "fdbclient/CommitTransaction.h"
#include "fdbclient/SystemData.h"

static const uint16_t invalidAccumulativeChecksumIndex = 0;
static const uint16_t resolverAccumulativeChecksumIndex = 2;

uint16_t getCommitProxyAccumulativeChecksumIndex(uint16_t commitProxyIndex);

uint32_t calculateAccumulativeChecksum(uint32_t currentAccumulativeChecksum, uint32_t newChecksum);

bool tagSupportAccumulativeChecksum(Tag tag);

class AccumulativeChecksumBuilder {
	struct Entry {
		Entry() : acsState(AccumulativeChecksumState()) {}
		Entry(AccumulativeChecksumState acsState) : acsState(acsState) {}

		AccumulativeChecksumState acsState;
	};

public:
	AccumulativeChecksumBuilder(uint16_t acsIndex) : acsIndex(acsIndex), currentVersion(0) {}

	bool isValid() { return acsIndex != invalidAccumulativeChecksumIndex; }

	void newTag(Tag tag, Version commitVersion);

	uint32_t update(Tag tag, uint32_t checksum, Version version, LogEpoch epoch);

	std::unordered_map<Tag, Entry> acsTable;

private:
	uint16_t acsIndex;
	Version currentVersion;
};

class AccumulativeChecksumValidator {
	struct Entry {
		Entry() {}
		Entry(AccumulativeChecksumState acsState) : acsState(acsState) {}

		Optional<AccumulativeChecksumState> acsState;
		Optional<Version> liveLatestVersion;
		std::vector<MutationRef> cachedMutations;
	};

public:
	AccumulativeChecksumValidator() {}

	void addMutation(UID ssid, Tag tag, MutationRef mutation, Version ssVersion);

	Optional<AccumulativeChecksumState> processAccumulativeChecksum(UID ssid,
	                                                                Tag tag,
	                                                                uint16_t acsIndex,
	                                                                AccumulativeChecksumState acsMutationState,
	                                                                Version ssVersion);

	void restore(UID ssid, Tag tag, uint16_t acsIndex, AccumulativeChecksumState acsState, Version ssVersion);

	std::unordered_map<uint16_t, Entry> acsTable;

	LogEpoch epoch = 0;
};

void acsBuilderUpdateAccumulativeChecksum(UID commitProxyId,
                                          std::shared_ptr<AccumulativeChecksumBuilder> acsBuilder,
                                          MutationRef mutation,
                                          std::vector<Tag> tags,
                                          Version commitVersion,
                                          LogEpoch epoch);

#endif
