// Copyright (c) 2026, KVIMR contributors.

package org.rocksdb;

/**
 * RocksDB Env wrapper backed by the KVIMR userspace middleware.
 * The native wrapper owns only its EnvWrapper object; Env::Default() remains
 * owned by RocksDB and is never deleted here.
 */
public final class KVIMREnv extends Env {
  public KVIMREnv(final String dbPath, final String device,
      final long zones, final String rmwMode) throws RocksDBException {
    super(newKVIMREnv(dbPath, device, zones, rmwMode));
  }

  private static native long newKVIMREnv(String dbPath, String device,
      long zones, String rmwMode) throws RocksDBException;

  public final long getNaiveRmwCount() {
    return getNaiveRmwCount(nativeHandle_);
  }

  public final long getNaiveReadBytes() {
    return getNaiveReadBytes(nativeHandle_);
  }

  public final long getNaiveWriteBytes() {
    return getNaiveWriteBytes(nativeHandle_);
  }

  public final long getMergedRmwCount() {
    return getMergedRmwCount(nativeHandle_);
  }

  public final long getMergedReadBytes() {
    return getMergedReadBytes(nativeHandle_);
  }

  public final long getMergedWriteBytes() {
    return getMergedWriteBytes(nativeHandle_);
  }

  public final long getMergedLogicalUpdateCount() {
    return getMergedLogicalUpdateCount(nativeHandle_);
  }

  public final long getMergedUpdatesSaved() {
    return getMergedUpdatesSaved(nativeHandle_);
  }

  public final long getDirectWriteBytes() {
    return getDirectWriteBytes(nativeHandle_);
  }

  public final long getMergedBufferCreateCount() {
    return getMergedBufferCreateCount(nativeHandle_);
  }

  public final long getMergedBufferHitCount() {
    return getMergedBufferHitCount(nativeHandle_);
  }

  public final long getMergedBufferFlushCount() {
    return getMergedBufferFlushCount(nativeHandle_);
  }

  private static native long getNaiveRmwCount(long handle);
  private static native long getNaiveReadBytes(long handle);
  private static native long getNaiveWriteBytes(long handle);
  private static native long getMergedRmwCount(long handle);
  private static native long getMergedReadBytes(long handle);
  private static native long getMergedWriteBytes(long handle);
  private static native long getMergedLogicalUpdateCount(long handle);
  private static native long getMergedUpdatesSaved(long handle);
  private static native long getDirectWriteBytes(long handle);
  private static native long getMergedBufferCreateCount(long handle);
  private static native long getMergedBufferHitCount(long handle);
  private static native long getMergedBufferFlushCount(long handle);

  @Override
  protected final native void disposeInternal(final long handle);
}
