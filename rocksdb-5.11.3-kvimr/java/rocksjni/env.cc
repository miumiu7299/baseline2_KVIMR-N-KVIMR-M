// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// This file implements the "bridge" between Java and C++ and enables
// calling c++ rocksdb::Env methods from Java side.

#include "include/org_rocksdb_Env.h"
#include "include/org_rocksdb_RocksEnv.h"
#include "include/org_rocksdb_RocksMemEnv.h"
#include "include/org_rocksdb_KVIMREnv.h"
#include "db/kvimr.h"
#include "db/kvimr_env.h"
#include "rocksdb/env.h"
#include "rocksjni/portal.h"

#include <string>

/*
 * Class:     org_rocksdb_Env
 * Method:    getDefaultEnvInternal
 * Signature: ()J
 */
jlong Java_org_rocksdb_Env_getDefaultEnvInternal(
    JNIEnv* env, jclass jclazz) {
  return reinterpret_cast<jlong>(rocksdb::Env::Default());
}

/*
 * Class:     org_rocksdb_Env
 * Method:    setBackgroundThreads
 * Signature: (JII)V
 */
void Java_org_rocksdb_Env_setBackgroundThreads(
    JNIEnv* env, jobject jobj, jlong jhandle,
    jint num, jint priority) {
  auto* rocks_env = reinterpret_cast<rocksdb::Env*>(jhandle);
  switch (priority) {
    case org_rocksdb_Env_FLUSH_POOL:
      rocks_env->SetBackgroundThreads(num, rocksdb::Env::Priority::LOW);
      break;
    case org_rocksdb_Env_COMPACTION_POOL:
      rocks_env->SetBackgroundThreads(num, rocksdb::Env::Priority::HIGH);
      break;
  }
}

/*
 * Class:     org_rocksdb_sEnv
 * Method:    getThreadPoolQueueLen
 * Signature: (JI)I
 */
jint Java_org_rocksdb_Env_getThreadPoolQueueLen(
    JNIEnv* env, jobject jobj, jlong jhandle, jint pool_id) {
  auto* rocks_env = reinterpret_cast<rocksdb::Env*>(jhandle);
  switch (pool_id) {
    case org_rocksdb_RocksEnv_FLUSH_POOL:
      return rocks_env->GetThreadPoolQueueLen(rocksdb::Env::Priority::LOW);
    case org_rocksdb_RocksEnv_COMPACTION_POOL:
      return rocks_env->GetThreadPoolQueueLen(rocksdb::Env::Priority::HIGH);
  }
  return 0;
}

/*
 * Class:     org_rocksdb_RocksMemEnv
 * Method:    createMemEnv
 * Signature: ()J
 */
jlong Java_org_rocksdb_RocksMemEnv_createMemEnv(
    JNIEnv* env, jclass jclazz) {
  return reinterpret_cast<jlong>(rocksdb::NewMemEnv(
      rocksdb::Env::Default()));
}

/*
 * Class:     org_rocksdb_RocksMemEnv
 * Method:    disposeInternal
 * Signature: (J)V
 */
void Java_org_rocksdb_RocksMemEnv_disposeInternal(
    JNIEnv* env, jobject jobj, jlong jhandle) {
  auto* e = reinterpret_cast<rocksdb::Env*>(jhandle);
  assert(e != nullptr);
  delete e;
}

/*
 * Class:     org_rocksdb_KVIMREnv
 * Method:    newKVIMREnv
 * Signature: (Ljava/lang/String;Ljava/lang/String;JLjava/lang/String;)J
 */
jlong Java_org_rocksdb_KVIMREnv_newKVIMREnv(
    JNIEnv* env, jclass jclazz, jstring jdb_path, jstring jdevice,
    jlong jzones, jstring jrmw_mode) {
  if (jdb_path == nullptr || jdevice == nullptr || jrmw_mode == nullptr ||
      jzones <= 0 || static_cast<uint64_t>(jzones) > 0xffffffffULL) {
    rocksdb::IllegalArgumentExceptionJni::ThrowNew(
        env, rocksdb::Status::InvalidArgument(
                 "KVIMR requires nonempty paths and a valid zone count"));
    return 0;
  }
  const char* db_path = env->GetStringUTFChars(jdb_path, nullptr);
  const char* device = env->GetStringUTFChars(jdevice, nullptr);
  const char* rmw_mode = env->GetStringUTFChars(jrmw_mode, nullptr);
  if (db_path == nullptr || device == nullptr || rmw_mode == nullptr) {
    if (db_path != nullptr) env->ReleaseStringUTFChars(jdb_path, db_path);
    if (device != nullptr) env->ReleaseStringUTFChars(jdevice, device);
    if (rmw_mode != nullptr) env->ReleaseStringUTFChars(jrmw_mode, rmw_mode);
    return 0;
  }
  const std::string mode(rmw_mode);
  rocksdb::Status status;
  if (db_path[0] == '\0' || device[0] == '\0') {
    status = rocksdb::Status::InvalidArgument(
        "KVIMR database and device paths must not be empty");
  } else if (mode == "naive") {
    rocksdb::KVIMR::Get()->SetRMWMode(rocksdb::KVIMR::RMWMode::NAIVE);
  } else if (mode == "merged") {
    rocksdb::KVIMR::Get()->SetRMWMode(rocksdb::KVIMR::RMWMode::MERGED);
  } else {
    status = rocksdb::Status::InvalidArgument(
        "KVIMR rmw_mode must be naive or merged");
  }
  if (status.ok()) {
    status = rocksdb::KVIMR::Get()->InitializePersistence(
        db_path, static_cast<uint32_t>(jzones), device);
  }
  env->ReleaseStringUTFChars(jdb_path, db_path);
  env->ReleaseStringUTFChars(jdevice, device);
  env->ReleaseStringUTFChars(jrmw_mode, rmw_mode);
  if (!status.ok()) {
    if (status.IsInvalidArgument()) {
      rocksdb::IllegalArgumentExceptionJni::ThrowNew(env, status);
    } else {
      rocksdb::RocksDBExceptionJni::ThrowNew(env, status);
    }
    return 0;
  }
  return reinterpret_cast<jlong>(rocksdb::NewKVIMREnv(rocksdb::Env::Default()));
}

jlong Java_org_rocksdb_KVIMREnv_getNaiveRmwCount(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().naive_rmw_count);
}

jlong Java_org_rocksdb_KVIMREnv_getNaiveReadBytes(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().naive_rmw_read_bytes);
}

jlong Java_org_rocksdb_KVIMREnv_getNaiveWriteBytes(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().naive_rmw_write_bytes);
}

jlong Java_org_rocksdb_KVIMREnv_getMergedRmwCount(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().merged_rmw_count);
}

jlong Java_org_rocksdb_KVIMREnv_getMergedReadBytes(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().merged_rmw_read_bytes);
}

jlong Java_org_rocksdb_KVIMREnv_getMergedWriteBytes(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().merged_rmw_write_bytes);
}

jlong Java_org_rocksdb_KVIMREnv_getMergedLogicalUpdateCount(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().merged_logical_update_count);
}

jlong Java_org_rocksdb_KVIMREnv_getMergedUpdatesSaved(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().merged_updates_saved);
}

jlong Java_org_rocksdb_KVIMREnv_getDirectWriteBytes(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().direct_write_bytes);
}

jlong Java_org_rocksdb_KVIMREnv_getMergedBufferCreateCount(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().merged_buffer_create_count);
}

jlong Java_org_rocksdb_KVIMREnv_getMergedBufferHitCount(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().merged_buffer_hit_count);
}

jlong Java_org_rocksdb_KVIMREnv_getMergedBufferFlushCount(
    JNIEnv*, jclass, jlong) {
  return static_cast<jlong>(rocksdb::KVIMR::Get()->GetRMWCounters().merged_buffer_flush_count);
}

/*
 * Class:     org_rocksdb_KVIMREnv
 * Method:    disposeInternal
 * Signature: (J)V
 */
void Java_org_rocksdb_KVIMREnv_disposeInternal(
    JNIEnv* env, jobject jobj, jlong jhandle) {
  auto* e = reinterpret_cast<rocksdb::Env*>(jhandle);
  assert(e != nullptr);
  delete e;
}
