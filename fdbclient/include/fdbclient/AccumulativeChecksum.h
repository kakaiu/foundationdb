/*
 * AccumulativeChecksum.h
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

#ifndef FDBCLIENT_ACCUMULATIVECHECKSUM_H
#define FDBCLIENT_ACCUMULATIVECHECKSUM_H
#pragma once

#include "fdbclient/FDBTypes.h"
#include "fdbrpc/fdbrpc.h"

struct AccumulativeChecksumState {
	constexpr static FileIdentifier file_identifier = 13804380;

	AccumulativeChecksumState()
	  : acs(0), cachedAcs(Optional<uint32_t>()), version(-1), outdated(false), liveLatestVersion(Optional<Version>()) {}
	AccumulativeChecksumState(uint32_t acs, Version version)
	  : acs(acs), cachedAcs(Optional<uint32_t>()), version(version), outdated(false),
	    liveLatestVersion(Optional<Version>()) {}

	bool isValid() { return version != -1; }

	std::string toString() const {
		return "AccumulativeChecksumState: [ACS]: " + std::to_string(acs) + ", [Version]: " + std::to_string(version);
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, acs, version, outdated);
	}

	uint32_t acs;
	Optional<uint32_t> cachedAcs;
	Version version;
	bool outdated;
	Optional<Version> liveLatestVersion;
};

#endif
