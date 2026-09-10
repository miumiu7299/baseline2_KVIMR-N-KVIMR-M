# Baseline2.2 KVIMR-N / KVIMR-M 測試流程

本文件說明 Baseline2.2 Raw Middleware prototype 的建置、環境確認與 YCSB A–F 測試流程。

## 1. 系統定位

Baseline2.2 的資料路徑：

```text
YCSB
  ↓
RocksDB 5.11.3 + KVIMR modifications
  ↓
KVIMR Middleware
  ├─ SST level detection
  ├─ S2TMap
  ├─ Level-aware track placement
  └─ KVIMR-N / KVIMR-M
  ↓
raw /dev/mapper/imrsim
  ↓
Backing Device
```

SST payload 直接使用 raw IMRSim mapper；`/dev/mapper/imrsim` 不格式化為 ext4、也不掛載。WAL、MANIFEST 等 RocksDB metadata 仍使用一般 filesystem DB directory。

兩種模式：

- **KVIMR-N**：`kvimr.rmw_mode=naive`
- **KVIMR-M**：`kvimr.rmw_mode=merged`

## 2. 正式測試環境

目前 matched comparison 建議固定：

```text
VM              192.168.147.139
OS              Ubuntu 14.10
Kernel          3.16.0-23-generic
RocksDB         5.11.3 + KVIMR modifications
YCSB            0.18.0-SNAPSHOT
Java            JDK 8
Zones           79
Record count    300000
Operation count 1000000
Threads         8
Field count     10
Field length    100
Repetitions     3
Workloads       A B C D E F
Mapper          /dev/mapper/imrsim
Backing device  /dev/sdc (20 GiB)
```

> **警告：runner 會重置 backing device。請確認 `/dev/sdc` 是專用測試磁碟。絕對不要改成系統碟 `/dev/sda`。**

79-zone mapper 大小：

```text
79 × 524288 sectors = 41418752 sectors
```

runner 會把 mapper 的 `max_sectors_kb` 設為 `4`。

## 3. 主要目錄

目前 VM 目錄：

```text
~/IMR-LSM-imr-lsm-validation-split
~/rocksdb-5.11.3
~/YCSB
```

核心 runner：

```text
tests/kvimr_ycsb_workloads_comparison_test.sh
```

## 4. Build modified RocksDB JNI

```bash
export JAVA_HOME=/home/miu/jdk8u504-b01
export PATH="$JAVA_HOME/bin:$PATH"

cd ~/rocksdb-5.11.3
make rocksdbjava -j1
```

確認 custom JNI：

```bash
ls -lh ~/rocksdb-5.11.3/java/target/rocksdbjni-5.11.3-linux64.jar

jar tf ~/rocksdb-5.11.3/java/target/rocksdbjni-5.11.3-linux64.jar   | grep KVIMREnv
```

應可看到：

```text
org/rocksdb/KVIMREnv.class
```

建議另外記錄目前 build 的 SHA256：

```bash
sha256sum ~/rocksdb-5.11.3/java/target/rocksdbjni-5.11.3-linux64.jar
```

## 5. 安裝 `5.11.3-kvimr` Maven dependency

YCSB RocksDB binding 使用：

```text
org.rocksdb:rocksdbjni:5.11.3-kvimr
```

目前 formal runner 由 `sudo` 啟動，因此 Maven runtime 可能解析 `/root/.m2`：

```bash
sudo mkdir -p   /root/.m2/repository/org/rocksdb/rocksdbjni/5.11.3-kvimr

sudo cp   /home/miu/rocksdb-5.11.3/java/target/rocksdbjni-5.11.3-linux64.jar   /root/.m2/repository/org/rocksdb/rocksdbjni/5.11.3-kvimr/rocksdbjni-5.11.3-kvimr.jar
```

建立 POM：

```bash
cat >/tmp/rocksdbjni-5.11.3-kvimr.pom <<'EOF'
<project xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://maven.apache.org/xsd/maven-4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd">
  <modelVersion>4.0.0</modelVersion>
  <groupId>org.rocksdb</groupId>
  <artifactId>rocksdbjni</artifactId>
  <version>5.11.3-kvimr</version>
  <packaging>jar</packaging>
</project>
EOF

sudo cp   /tmp/rocksdbjni-5.11.3-kvimr.pom   /root/.m2/repository/org/rocksdb/rocksdbjni/5.11.3-kvimr/rocksdbjni-5.11.3-kvimr.pom
```

確認：

```bash
sudo jar tf   /root/.m2/repository/org/rocksdb/rocksdbjni/5.11.3-kvimr/rocksdbjni-5.11.3-kvimr.jar   | grep KVIMREnv
```

## 6. Build YCSB RocksDB binding

```bash
cd ~/YCSB

export JAVA_HOME=/home/miu/jdk8u504-b01
export MAVEN_HOME=/home/miu/apache-maven-3.9.11
export PATH="$JAVA_HOME/bin:$MAVEN_HOME/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

mvn -pl site.ycsb:rocksdb-binding -am clean package   -Dmaven.test.skip=true
```

執行 log 中應使用：

```text
rocksdbjni-5.11.3-kvimr.jar
```

而不是一般 upstream `rocksdbjni-5.11.3.jar`。

## 7. Pre-flight

```bash
cd ~/IMR-LSM-imr-lsm-validation-split

lsblk -o NAME,SIZE,TYPE,MOUNTPOINT /dev/sdc
findmnt -rn -S /dev/sdc || true

sudo dmsetup ls --tree 2>/dev/null || true

lsmod | grep -i imrsim
sudo dmsetup targets | grep -i imrsim
```

如果 mapper 已存在，先確認沒有任何 YCSB/Java process 正在使用。不要在 active benchmark 中移除 mapper。

## 8. Workload 定義

runner 使用：

```text
A: 50% Read / 50% Update
B: 95% Read / 5% Update
C: 100% Read
D: 95% Read / 5% Insert, latest distribution
E: 95% Scan / 5% Insert
F: 50% Read / 50% Read-Modify-Write
```

## 9. Smoke test：KVIMR-N A ×1

> **以下指令具有破壞性，會重置 `/dev/sdc`。**

```bash
cd ~/IMR-LSM-imr-lsm-validation-split

RESULT=/var/tmp/kvimr-n-smoke-$(date -u +%Y%m%dT%H%M%SZ)

sudo env   KVIMR_TEST_DESTRUCTIVE=1   KVIMR_RMW_MODE=naive   KVIMR_YCSB_HOME=/home/miu/YCSB   KVIMR_YCSB_COMPARE_RESULT_DIR="$RESULT"   KVIMR_REPETITIONS=1   KVIMR_WORKLOADS="A"   KVIMR_YCSB_RECORD_COUNT=300000   KVIMR_YCSB_OPERATION_COUNT=1000000   KVIMR_YCSB_THREAD_COUNT=8   KVIMR_YCSB_FIELD_COUNT=10   KVIMR_YCSB_FIELD_LENGTH=100   KVIMR_ZONES=79   bash tests/kvimr_ycsb_workloads_comparison_test.sh /dev/sdc
```

查看：

```bash
cat "$RESULT/runs.csv"
cat "$RESULT/comparison.md"
```

至少確認：

```text
load_status = PASS
run_status  = PASS
kernel_issue_count = 0
```

## 10. Formal KVIMR-N：A–F ×3

```bash
cd ~/IMR-LSM-imr-lsm-validation-split

KVIMR_N_RESULT=/var/tmp/kvimr-n-formal-$(date -u +%Y%m%dT%H%M%SZ)

sudo env   KVIMR_TEST_DESTRUCTIVE=1   KVIMR_RMW_MODE=naive   KVIMR_YCSB_HOME=/home/miu/YCSB   KVIMR_YCSB_COMPARE_RESULT_DIR="$KVIMR_N_RESULT"   KVIMR_REPETITIONS=3   KVIMR_WORKLOADS="A B C D E F"   KVIMR_YCSB_RECORD_COUNT=300000   KVIMR_YCSB_OPERATION_COUNT=1000000   KVIMR_YCSB_THREAD_COUNT=8   KVIMR_YCSB_FIELD_COUNT=10   KVIMR_YCSB_FIELD_LENGTH=100   KVIMR_ZONES=79   bash tests/kvimr_ycsb_workloads_comparison_test.sh /dev/sdc
```

主要輸出：

```text
$KVIMR_N_RESULT/runs.csv
$KVIMR_N_RESULT/medians.csv
$KVIMR_N_RESULT/comparison.md
$KVIMR_N_RESULT/manifest.txt
```

## 11. Formal KVIMR-M：A–F ×3

```bash
cd ~/IMR-LSM-imr-lsm-validation-split

KVIMR_M_RESULT=/var/tmp/kvimr-m-formal-$(date -u +%Y%m%dT%H%M%SZ)

sudo env   KVIMR_TEST_DESTRUCTIVE=1   KVIMR_RMW_MODE=merged   KVIMR_YCSB_HOME=/home/miu/YCSB   KVIMR_YCSB_COMPARE_RESULT_DIR="$KVIMR_M_RESULT"   KVIMR_REPETITIONS=3   KVIMR_WORKLOADS="A B C D E F"   KVIMR_YCSB_RECORD_COUNT=300000   KVIMR_YCSB_OPERATION_COUNT=1000000   KVIMR_YCSB_THREAD_COUNT=8   KVIMR_YCSB_FIELD_COUNT=10   KVIMR_YCSB_FIELD_LENGTH=100   KVIMR_ZONES=79   bash tests/kvimr_ycsb_workloads_comparison_test.sh /dev/sdc
```

## 12. 確認 18 / 18 PASS

KVIMR runner 的 `runs.csv`：

```text
column 5 = load_status
column 6 = run_status
```

KVIMR-N：

```bash
awk -F',' '
NR > 1 {
  total++
  if ($5 == "PASS" && $6 == "PASS") pass++
  else print "NON-PASS:", $3, $4, $5, $6
}
END {
  print "total=" total
  print "PASS=" pass
}' "$KVIMR_N_RESULT/runs.csv"
```

KVIMR-M：

```bash
awk -F',' '
NR > 1 {
  total++
  if ($5 == "PASS" && $6 == "PASS") pass++
  else print "NON-PASS:", $3, $4, $5, $6
}
END {
  print "total=" total
  print "PASS=" pass
}' "$KVIMR_M_RESULT/runs.csv"
```

正式 A–F ×3 應為：

```text
total=18
PASS=18
```

## 13. KVIMR-M correctness

Merged mode runner 會檢查：

```text
merged_logical_update_count
=
merged_rmw_count + merged_updates_saved
```

不成立時 runner 直接 FAIL。

也建議檢查 kernel issues：

```bash
grep -RniE 'blocked for more than|hung task|I/O error|Buffer I/O error|EXT4-fs error|journal has aborted' "$KVIMR_M_RESULT" 2>/dev/null
```

## 14. 結果與 WAF

正式結果至少保留：

- YCSB throughput
- Durable throughput
- Read / Update / Insert / Scan / RMW latency
- KVIMR-N naive RMW count
- KVIMR-M merged RMW count
- `merged_logical_update_count`
- `merged_updates_saved`
- merge reduction ratio
- backing-device write volume
- WAF

WAF 請明確區分：

```text
Device WAF
= backing-device sectors written
  / host mapper write-request sectors
```

與：

```text
Application-to-Device WAF
= backing-device write bytes
  / application logical payload write bytes
```

兩者不是同一指標，不要混用。

## 15. Historical sanity reference

先前 Baseline2.2 A–F ×3 的 median throughput 可作量級 sanity check，但不應作為新 VM 的 pass/fail threshold：

```text
Workload   KVIMR-N ops/s   KVIMR-M ops/s
A          35113.6         39593.0
B          55129.8         56357.1
C          55349.5         59772.9
D          65967.4         61709.3
E          14535.9         14320.5
F          33325.6         33067.7
```

先前 Merged RMW 在會觸發 RMW 的 workload 中約降低 63% physical track-level RMW，例如 Workload A 為：

```text
459 → 166
```

不同 VM / backing-device runtime 會影響 throughput；正式 Baseline1 / Baseline2 comparison 應固定在同一 VM、同一 backing device 與同一 YCSB parameters。

## 16. Windows ↔ VM 同步

Windows project root：

```text
C:\Users\user\Desktop\baseline2
```

VM：

```text
miu@192.168.147.139
```

Windows → VM：

```powershell
scp -r C:\Users\user\Desktop\baseline2 miu@192.168.147.139:~/
```

只同步本測試文件：

```powershell
scp C:\Users\user\Desktop\baseline2\TESTING.md `
  miu@192.168.147.139:~/IMR-LSM-imr-lsm-validation-split/TESTING.md
```

VM → Windows 備份：

```powershell
scp -r miu@192.168.147.139:~/IMR-LSM-imr-lsm-validation-split `
  C:\Users\user\Desktop\baseline2_vm_backup
```
