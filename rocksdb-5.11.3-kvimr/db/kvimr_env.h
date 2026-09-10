// Copyright (c) 2024, KVIMR contributors.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#pragma once

#include "rocksdb/env.h"

namespace rocksdb {

class KVIMRWritableFile;

// Env wrapper that redirects only files ending in .sst to KVIMR.
Env* NewKVIMREnv(Env* base_env);

}  // namespace rocksdb
