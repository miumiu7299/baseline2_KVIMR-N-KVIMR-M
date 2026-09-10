#!/usr/bin/env bash
set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${TEST_DIR}/.." && pwd)"
BACKING_DEVICE="${1:-}"
MAPPER_NAME="${KVIMR_MAPPER_NAME:-imrsim}"
MAPPER_DEVICE="/dev/mapper/${MAPPER_NAME}"
YCSB_HOME="${KVIMR_YCSB_HOME:-/home/miu/YCSB}"
RESULT_ROOT="${KVIMR_YCSB_COMPARE_RESULT_DIR:-/var/tmp/kvimr-ycsb-a-f-$(date -u +%Y%m%dT%H%M%SZ)}"
RMW_MODE="${KVIMR_RMW_MODE:-naive}"
ZONE_COUNT="${KVIMR_ZONES:-79}"
REPETITIONS="${KVIMR_REPETITIONS:-3}"
WORKLOAD_SELECTION="${KVIMR_WORKLOADS:-A B C D E F}"
RECORD_COUNT="${KVIMR_YCSB_RECORD_COUNT:-100000}"
OPERATION_COUNT="${KVIMR_YCSB_OPERATION_COUNT:-100000}"
THREAD_COUNT="${KVIMR_YCSB_THREAD_COUNT:-1}"
FIELD_COUNT="${KVIMR_YCSB_FIELD_COUNT:-10}"
FIELD_LENGTH="${KVIMR_YCSB_FIELD_LENGTH:-100}"
FORMATTER="${KVIMR_IMR_FORMATTER:-${REPO_ROOT}/imrsim_util/imr_format.sh}"
IMRSIM_UTIL="${KVIMR_IMRSIM_UTIL:-${REPO_ROOT}/imrsim_util/imrsim_util}"
DEBUGFS="${KVIMR_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
KERNEL_ISSUE_PATTERN='blocked for more than|hung task|task (jbd2|sync)[^:]*:.*blocked|I/O error|Buffer I/O error|blk_update_request.*error|end_request.*I/O error|EXT4-fs error|JBD2:.*(error|abort)|journal has aborted'
BENCHMARK_MAPPER_SECTORS=$((79 * 524288))
FORMATTED_USABLE_SECTORS=0
BACKING_REAL=""
BACKING_SYS_STAT=""
ACTIVE_MAPPER=0
YCSB_BIN=""
WORKLOADS=()

log() { printf '[kvimr ycsb-compare] %s\n' "$*"; }
fail() { printf '[kvimr ycsb-compare] FAIL: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: sudo env KVIMR_TEST_DESTRUCTIVE=1 KVIMR_RMW_MODE=naive|merged \\
  KVIMR_YCSB_HOME=/home/miu/YCSB $0 BACKING_DEVICE

The backing device is reset before each repetition. KVIMR never formats or
mounts /dev/mapper/imrsim; only the normal filesystem DB directory is used.

Optional development overrides:
  KVIMR_REPETITIONS=1
  KVIMR_WORKLOADS="A F"
EOF
}

require_tools() {
    local tool
    for tool in awk basename bash blockdev cat cp date dmesg dmsetup find grep head \
        java lsblk mkdir readlink rm seq sleep sort stat tee uname; do
        command -v "${tool}" >/dev/null 2>&1 || fail "missing required tool: ${tool}"
    done
    [[ -r "${FORMATTER}" ]] || fail "missing formatter: ${FORMATTER}"
    [[ -b "${BACKING_DEVICE}" ]] || fail "not a block device: ${BACKING_DEVICE}"
    if [[ -x "${YCSB_HOME%/}/bin/ycsb" ]]; then
        YCSB_BIN="${YCSB_HOME%/}/bin/ycsb"
    elif [[ -x "${YCSB_HOME%/}/bin/ycsb.sh" ]]; then
        YCSB_BIN="${YCSB_HOME%/}/bin/ycsb.sh"
    elif command -v ycsb >/dev/null 2>&1; then
        YCSB_BIN="$(command -v ycsb)"
    else
        fail "cannot find YCSB below ${YCSB_HOME} or in PATH"
    fi
    local workload
    for workload in workloada workloadb workloadc workloadd workloade workloadf; do
        [[ -r "${YCSB_HOME%/}/workloads/${workload}" ]] ||
            fail "missing YCSB workload: ${YCSB_HOME%/}/workloads/${workload}"
    done
}

validate_config() {
    [[ "${EUID}" -eq 0 ]] || fail "run as root"
    [[ "${KVIMR_TEST_DESTRUCTIVE:-0}" == "1" ]] ||
        fail "set KVIMR_TEST_DESTRUCTIVE=1 to authorize backing-device reset"
    [[ "${RMW_MODE}" == "naive" || "${RMW_MODE}" == "merged" ]] ||
        fail "KVIMR_RMW_MODE must be naive or merged"
    [[ "${ZONE_COUNT}" =~ ^[1-9][0-9]*$ && "${ZONE_COUNT}" -eq 79 ]] ||
        fail "KVIMR_ZONES must be exactly 79 for this comparison"
    [[ "${REPETITIONS}" =~ ^[1-9][0-9]*$ ]] || fail "invalid repetitions"
    local selected_workload
    local selected_workloads=()
    read -r -a selected_workloads <<< "${WORKLOAD_SELECTION}"
    [[ "${#selected_workloads[@]}" -gt 0 ]] || fail "KVIMR_WORKLOADS must not be empty"
    for selected_workload in "${selected_workloads[@]}"; do
        case "${selected_workload}" in
            A) WORKLOADS+=(workloada) ;;
            B) WORKLOADS+=(workloadb) ;;
            C) WORKLOADS+=(workloadc) ;;
            D) WORKLOADS+=(workloadd) ;;
            E) WORKLOADS+=(workloade) ;;
            F) WORKLOADS+=(workloadf) ;;
            *) fail "KVIMR_WORKLOADS contains invalid workload: ${selected_workload}" ;;
        esac
    done
    [[ "${RECORD_COUNT}" =~ ^[1-9][0-9]*$ ]] || fail "invalid record count"
    [[ "${OPERATION_COUNT}" =~ ^[1-9][0-9]*$ ]] || fail "invalid operation count"
    [[ "${THREAD_COUNT}" =~ ^[1-9][0-9]*$ ]] || fail "invalid thread count"
    [[ "${MAPPER_NAME}" =~ ^[A-Za-z0-9_.+-]+$ ]] || fail "invalid mapper name"
    [[ "${RESULT_ROOT}" == /* ]] || fail "result directory must be absolute"
    [[ ! -e "${RESULT_ROOT}" ]] || fail "result directory already exists: ${RESULT_ROOT}"
    BACKING_REAL="$(readlink -f "${BACKING_DEVICE}")" || fail "cannot resolve backing device"
    BACKING_SYS_STAT="/sys/class/block/$(basename "$BACKING_REAL")/stat"
    [[ -r "$BACKING_SYS_STAT" ]] || BACKING_SYS_STAT=""
    [[ "$(blockdev --getsize64 "${BACKING_REAL}")" =~ ^[0-9]+$ ]] ||
        fail "cannot read backing-device size"
    FORMATTED_USABLE_SECTORS="$(bash "${FORMATTER}" -d "${BACKING_REAL}")" ||
        fail "cannot inspect formatter usable capacity"
    [[ "${FORMATTED_USABLE_SECTORS}" =~ ^[1-9][0-9]*$ ]] ||
        fail "formatter returned invalid usable sector count: ${FORMATTED_USABLE_SECTORS}"
    [[ "${FORMATTED_USABLE_SECTORS}" -ge "${BENCHMARK_MAPPER_SECTORS}" ]] ||
        fail "formatted capacity ${FORMATTED_USABLE_SECTORS} is smaller than benchmark mapper ${BENCHMARK_MAPPER_SECTORS}"
}

mapper_exists() { dmsetup info "${MAPPER_NAME}" >/dev/null 2>&1; }

mapper_stat_path() {
    local majmin sysdev

    majmin="$(dmsetup info -c --noheadings -o major,minor "${MAPPER_NAME}" 2>/dev/null \
        | tr -d '[:space:]')" || return 1

    [[ "${majmin}" =~ ^[0-9]+:[0-9]+$ ]] || return 1

    sysdev="$(readlink -f "/sys/dev/block/${majmin}" 2>/dev/null)" || return 1

    [[ -n "${sysdev}" && -r "${sysdev}/stat" ]] || return 1

    printf '%s\n' "${sysdev}/stat"
}

ensure_unmounted() {
    local mounts
    mounts="$(lsblk -nrpo MOUNTPOINT "${MAPPER_DEVICE}" 2>/dev/null || true)"
    [[ ! "${mounts}" =~ [^[:space:]] ]] ||
        fail "${MAPPER_DEVICE} is mounted; KVIMR must use an unmounted mapper"
}

remove_mapper() {
    if mapper_exists; then
        ensure_unmounted
        dmsetup remove "${MAPPER_NAME}" ||
            fail "cannot remove mapper ${MAPPER_NAME}"
    fi
    ACTIVE_MAPPER=0
}

validate_existing_mappers() {
    local mapper existing_backing
    while read -r mapper _; do
        [[ -n "${mapper}" && "${mapper}" != "No" ]] || continue
        [[ "${mapper}" == "${MAPPER_NAME}" ]] ||
            fail "another imrsim mapper is active: ${mapper}"
    done < <(dmsetup ls --target imrsim 2>/dev/null || true)
    if mapper_exists; then
        ensure_unmounted
        existing_backing="$(dmsetup table "${MAPPER_NAME}" | awk 'NF {print $4; exit}')"
        case "${existing_backing}" in
            /*) ;;
            [0-9]*:[0-9]*) existing_backing="/dev/block/${existing_backing}" ;;
            *) existing_backing="/dev/${existing_backing}" ;;
        esac
        existing_backing="$(readlink -f "${existing_backing}")" ||
            fail "cannot resolve existing mapper backing device"
        [[ "${existing_backing}" == "${BACKING_REAL}" ]] ||
            fail "existing ${MAPPER_NAME} uses ${existing_backing}, not ${BACKING_REAL}"
    fi
}

cleanup() {
    set +e
    if [[ "${ACTIVE_MAPPER}" -eq 1 ]] && mapper_exists; then
        ensure_unmounted >/dev/null 2>&1 || true
        dmsetup remove "${MAPPER_NAME}" >/dev/null 2>&1 || true
    fi
}

capture_stats() {
    local destination="$1"
    local mapper_stat=""

    # Linux block-layer statistics for /dev/mapper/imrsim.
    # This is the denominator used for Device WAF.
    mapper_stat="$(mapper_stat_path 2>/dev/null || true)"

    if [[ -n "${mapper_stat}" && -r "${mapper_stat}" ]]; then
        cat "${mapper_stat}" > "${destination}.imrsim"
    else
        printf 'unavailable\n' > "${destination}.imrsim"
    fi

    # Preserve IMRSim internal utility output separately for diagnostics.
    if [[ -x "${IMRSIM_UTIL}" && -b "${MAPPER_DEVICE}" ]]; then
        "${IMRSIM_UTIL}" "${MAPPER_DEVICE}" s 1 \
            > "${destination}.imrsim-util" 2>&1 ||
            printf 'unavailable\n' > "${destination}.imrsim-util"
    else
        printf 'unavailable\n' > "${destination}.imrsim-util"
    fi

    if [[ -r "${DEBUGFS}/stats" ]]; then
        cp -- "${DEBUGFS}/stats" "${destination}.debugfs"
    else
        printf 'unavailable\n' > "${destination}.debugfs"
    fi

    # Physical backing-device statistics (/dev/sdb).
    # This is the numerator used for Device WAF.
    if [[ -n "${BACKING_SYS_STAT}" && -r "${BACKING_SYS_STAT}" ]]; then
        cat "${BACKING_SYS_STAT}" > "${destination}.device"
    else
        printf 'unavailable\n' > "${destination}.device"
    fi
}

prepare_mapper() {
    local sectors
    validate_existing_mappers
    remove_mapper
    sectors="$(KVIMR_TEST_DESTRUCTIVE=1 bash "${FORMATTER}" -i -d "${BACKING_REAL}")" ||
        fail "IMRSim persistence reset failed"
    [[ "${sectors}" =~ ^[1-9][0-9]*$ && "${sectors}" -ge "${BENCHMARK_MAPPER_SECTORS}" ]] ||
        fail "formatter returned ${sectors} sectors; need at least ${BENCHMARK_MAPPER_SECTORS}"
    FORMATTED_USABLE_SECTORS="${sectors}"
    dmsetup create "${MAPPER_NAME}" --table \
        "0 ${BENCHMARK_MAPPER_SECTORS} imrsim ${BACKING_REAL} 0" ||
        fail "cannot create ${MAPPER_NAME}"
    ACTIVE_MAPPER=1
    for _ in $(seq 1 50); do
        [[ -b "${MAPPER_DEVICE}" ]] && break
        sleep 0.1
    done
    [[ -b "${MAPPER_DEVICE}" ]] || fail "mapper device did not appear"
        local dm_node queue_file actual_max_sectors_kb
    dm_node="$(basename "$(readlink -f "${MAPPER_DEVICE}")")"
    queue_file="/sys/block/${dm_node}/queue/max_sectors_kb"

    [[ -e "${queue_file}" ]] ||
        fail "max_sectors_kb sysfs entry not found: ${queue_file}"

    echo 4 > "${queue_file}" ||
        fail "cannot set ${MAPPER_NAME} max_sectors_kb=4"

    actual_max_sectors_kb="$(cat "${queue_file}")"
    [[ "${actual_max_sectors_kb}" == "4" ]] ||
        fail "max_sectors_kb verification failed: expected 4, got ${actual_max_sectors_kb}"

    echo "[kvimr ycsb-compare] mapper_max_sectors_kb=${actual_max_sectors_kb}"
    ensure_unmounted
}

write_manifest() {
    local repo_commit="unavailable"
    local ycsb_commit="unavailable"
    command -v git >/dev/null 2>&1 &&
        repo_commit="$(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || printf unavailable)"
    command -v git >/dev/null 2>&1 &&
        ycsb_commit="$(git -C "${YCSB_HOME}" rev-parse HEAD 2>/dev/null || printf unavailable)"
    {
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'repo_commit=%s\n' "${repo_commit}"
        printf 'ycsb_commit=%s\n' "${ycsb_commit}"
        printf 'backing_device=%s\n' "${BACKING_REAL}"
        printf 'formatted_usable_sectors=%s\n' "${FORMATTED_USABLE_SECTORS}"
        printf 'mapper_name=%s\n' "${MAPPER_NAME}"
        printf 'benchmark_zones=%s\n' "${ZONE_COUNT}"
        printf 'benchmark_mapper_sectors=%s\n' "${BENCHMARK_MAPPER_SECTORS}"
        printf 'mapper_capacity_note=larger backing capacity is intentionally truncated at the device-mapper layer for comparability with the existing IMR-LSM benchmark\n'
        printf 'recordcount=%s\noperationcount=%s\nrepetitions=%s\nthreads=%s\n' \
            "${RECORD_COUNT}" "${OPERATION_COUNT}" "${REPETITIONS}" "${THREAD_COUNT}"
        printf 'workloads=%s\n' "${WORKLOAD_SELECTION}"
        printf 'kvimr_mode=%s\n' "${RMW_MODE}"
        printf 'ycsb_home=%s\n' "${YCSB_HOME}"
        printf 'rocksdb_version=5.11.3\njava_version=%s\n' "$(java -version 2>&1 | head -n 1 || printf unavailable)"
        printf 'kernel=%s\n' "$(uname -a)"
        printf 'kvimr_payload_storage=raw_imrsim_no_ext4_mount\n'
        printf 'kvimr_counters=emitted in YCSB logs and parsed into runs.csv\n'
        printf 'device_write_counter_source=%s field7_sectors_written\n' "$BACKING_SYS_STAT"
        printf 'device_write_counter_bytes=sectors_written*512\n'
        printf 'application_logical_write_bytes=load_INSERT*FIELD_LENGTH;A/B_run_UPDATE*FIELD_LENGTH;C_run=NA;D/E_run_INSERT*FIELD_LENGTH;F_run_READ-MODIFY-WRITE*FIELD_LENGTH\n'
    } > "${RESULT_ROOT}/manifest.txt"
}

workload_props() {
    case "$1" in
        workloada) printf '%s\n' 'readproportion=0.5' 'updateproportion=0.5' ;;
        workloadb) printf '%s\n' 'readproportion=0.95' 'updateproportion=0.05' ;;
        workloadc) printf '%s\n' 'readproportion=1.0' 'updateproportion=0.0' ;;
        workloadd) printf '%s\n' 'readproportion=0.95' 'insertproportion=0.05' 'requestdistribution=latest' ;;
        workloade) printf '%s\n' 'scanproportion=0.95' 'insertproportion=0.05' ;;
        workloadf) printf '%s\n' 'readproportion=0.5' 'readmodifywriteproportion=0.5' ;;
        *) fail "unknown workload $1" ;;
    esac
}

metric() {
    local file="$1" operation="$2" name="$3"
    awk -F', ' -v op="[${operation}]" -v metric="${name}" \
        '$1 == op && $2 == metric { print $3; exit }' "${file}"
}

overall_metric() {
    local file="$1" name="$2"
    awk -F', ' -v metric="${name}" \
        '$1 == "[OVERALL]" && $2 == metric { print $3; exit }' "${file}"
}

kvimr_metric() {
    local file="$1" name="$2"
    awk -F', ' -v metric="${name}" \
        '$1 == "[KVIMR]" && $2 == metric { print $3; exit }' "${file}"
}

kvimr_counter_value() {
    local value
    value="$(kvimr_metric "$1" "$2")"
    if [[ "${value}" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "${value}"
    else
        printf 'NA\n'
    fi
}

counter_ratio() {
    awk -v numerator="$1" -v denominator="$2" \
        'BEGIN { if (numerator ~ /^[0-9]+([.][0-9]+)?$/ &&
                        denominator ~ /^[0-9]+([.][0-9]+)?$/ &&
                        denominator > 0) {
                   printf "%.6f\n", numerator / denominator
               } else { print "NA" } }'
}

counter_sum() {
    if [[ "$1" =~ ^[0-9]+$ && "$2" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "$(( $1 + $2 ))"
    else
        printf 'NA\n'
    fi
}

validate_kvimr_invariants() {
    local phase="$1" file="$2"
    [[ "${RMW_MODE}" == merged ]] || return 0
    local logical updates saved
    logical="$(kvimr_metric "${file}" merged_logical_update_count)"
    updates="$(kvimr_metric "${file}" merged_rmw_count)"
    saved="$(kvimr_metric "${file}" merged_updates_saved)"
    if [[ "${logical}" =~ ^[0-9]+$ && "${updates}" =~ ^[0-9]+$ &&
          "${saved}" =~ ^[0-9]+$ ]]; then
        [[ "${logical}" -eq $((updates + saved)) ]] ||
            fail "${phase} KVIMR merged invariant mismatch: logical=${logical}, rmw=${updates}, saved=${saved}"
    fi
}

validate_output() {
    local phase="$1" file="$2" expected="$3"
    local operation count return_ok total=0 operation_list
    if [[ "${phase}" == load ]]; then
        [[ "$(metric "${file}" INSERT Operations)" == "${expected}" ]] ||
            fail "load INSERT Operations did not equal ${expected}"
        [[ "$(metric "${file}" INSERT Return=OK)" == "${expected}" ]] ||
            fail "load INSERT Return=OK did not equal ${expected}"
        return
    fi

    case "${YCSB_VALIDATION_WORKLOAD}" in
        workloada|workloadb) operation_list="READ UPDATE" ;;
        workloadc) operation_list="READ" ;;
        workloadd) operation_list="READ INSERT" ;;
        workloade) operation_list="SCAN INSERT" ;;
        workloadf) operation_list="READ READ-MODIFY-WRITE" ;;
        *) fail "cannot validate unknown workload: ${YCSB_VALIDATION_WORKLOAD}" ;;
    esac

    for operation in READ UPDATE INSERT SCAN READ-MODIFY-WRITE; do
        count="$(metric "${file}" "${operation}" Operations)"
        return_ok="$(metric "${file}" "${operation}" Return=OK)"
        if [[ -n "${return_ok}" ]]; then
            [[ "${count}" =~ ^[0-9]+$ ]] ||
                fail "${phase} ${operation} reports Return=OK without Operations"
            [[ "${return_ok}" == "${count}" ]] ||
                fail "${phase} ${operation} Return=OK=${return_ok} differs from Operations=${count}"
        fi
    done

    if [[ "${YCSB_VALIDATION_WORKLOAD}" == workloadf ]]; then
        local read_operations rmw_operations update_operations read_return_ok update_return_ok
        read_operations="$(metric "${file}" READ Operations)"
        rmw_operations="$(metric "${file}" READ-MODIFY-WRITE Operations)"
        update_operations="$(metric "${file}" UPDATE Operations)"
        read_return_ok="$(metric "${file}" READ Return=OK)"
        update_return_ok="$(metric "${file}" UPDATE Return=OK)"
        [[ "${read_operations}" =~ ^[0-9]+$ &&
           "${rmw_operations}" =~ ^[0-9]+$ &&
           "${update_operations}" =~ ^[0-9]+$ ]] ||
            fail "run workloadf is missing a required READ, UPDATE, or READ-MODIFY-WRITE counter"
        [[ "${read_operations}" -ge "${rmw_operations}" ]] ||
            fail "run workloadf READ operations are less than READ-MODIFY-WRITE operations"
        [[ "${update_operations}" -eq "${rmw_operations}" ]] ||
            fail "run workloadf UPDATE operations=${update_operations}, expected ${rmw_operations}"
        [[ "${read_return_ok}" =~ ^[0-9]+$ &&
           "${read_return_ok}" -eq "${read_operations}" ]] ||
            fail "run workloadf READ Return=OK does not equal READ Operations"
        if [[ -n "${update_return_ok}" ]]; then
            [[ "${update_return_ok}" == "${update_operations}" ]] ||
                fail "run workloadf UPDATE Return=OK=${update_return_ok} differs from UPDATE Operations=${update_operations}"
        fi
        total=$((read_operations - rmw_operations + rmw_operations))
        [[ "${total}" -eq "${expected}" ]] ||
            fail "run workloadf logical operation total=${total}, expected=${expected}"
        return
    fi

    for operation in ${operation_list}; do
        count="$(metric "${file}" "${operation}" Operations)"
        [[ "${count}" =~ ^[0-9]+$ ]] ||
            fail "${phase} ${operation} Operations is missing or invalid"
        total=$((total + count))
    done
    [[ "${total}" -eq "${expected}" ]] ||
        fail "${phase} ${YCSB_VALIDATION_WORKLOAD} operation total=${total}, expected=${expected}"
}

operation_count() {
    local value
    value="$(metric "$1" "$2" Operations)"
    if [[ "${value}" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "${value}"
    else
        printf '0\n'
    fi
}
device_write_sectors() {
    local file="$1"
    awk 'NF >= 7 && $7 ~ /^[0-9]+$/ { print $7; found=1; exit } END { if (!found) print "NA" }' "$file"
}

device_write_bytes_delta() {
    local before="$1" after="$2" a b
    a="$(device_write_sectors "$before")"
    b="$(device_write_sectors "$after")"
    if [[ "$a" =~ ^[0-9]+$ && "$b" =~ ^[0-9]+$ && "$b" -ge "$a" ]]; then
        printf '%s\n' "$(( (b - a) * 512 ))"
    else
        printf 'NA\n'
    fi
}
device_waf_delta() {
    local backing_before="$1"
    local backing_after="$2"
    local mapper_before="$3"
    local mapper_after="$4"

    local backing_a backing_b mapper_a mapper_b
    local backing_delta mapper_delta

    backing_a="$(device_write_sectors "${backing_before}")"
    backing_b="$(device_write_sectors "${backing_after}")"
    mapper_a="$(device_write_sectors "${mapper_before}")"
    mapper_b="$(device_write_sectors "${mapper_after}")"

    if [[ "${backing_a}" =~ ^[0-9]+$ &&
          "${backing_b}" =~ ^[0-9]+$ &&
          "${mapper_a}" =~ ^[0-9]+$ &&
          "${mapper_b}" =~ ^[0-9]+$ &&
          "${backing_b}" -ge "${backing_a}" &&
          "${mapper_b}" -ge "${mapper_a}" ]]; then

        backing_delta=$((backing_b - backing_a))
        mapper_delta=$((mapper_b - mapper_a))

        awk -v backing="${backing_delta}" -v mapper="${mapper_delta}" \
            'BEGIN {
                if (mapper > 0)
                    printf "%.6f\n", backing / mapper;
                else
                    print "NA";
            }'
    else
        printf 'NA\n'
    fi
}
application_logical_write_bytes() {
    local phase="$1" workload="$2" file="$3" operation count
    if [[ "$phase" == load ]]; then operation=INSERT
    else
        case "$workload" in
            workloada|workloadb) operation=UPDATE ;;
            workloadc) printf '0\n'; return ;;
            workloadd|workloade) operation=INSERT ;;
            workloadf) operation=READ-MODIFY-WRITE ;;
            *) printf 'NA\n'; return ;;
        esac
    fi
    count="$(operation_count "$file" "$operation")"
    if [[ "$count" =~ ^[0-9]+$ && "$FIELD_LENGTH" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "$((count * FIELD_LENGTH))"
    else printf 'NA\n'; fi
}

waf_or_na() {
    awk -v device="$1" -v logical="$2" 'BEGIN { if (device ~ /^[0-9]+$/ && logical ~ /^[0-9]+$/ && logical > 0) printf "%.6f\n", device / logical; else print "NA" }'
}

operation_latency() {
    local value
    value="$(metric "$1" "$2" "$3")"
    if [[ -n "${value}" ]]; then
        printf '%s\n' "${value}"
    else
        printf 'NA\n'
    fi
}

durable_throughput() {
    awk -v operations="$1" -v runtime_ms="$2" \
        'BEGIN { if (runtime_ms ~ /^[0-9]+([.][0-9]+)?$/ && runtime_ms > 0) {
            printf "%.6f\n", operations / (runtime_ms / 1000)
        } else { print "NA" } }'
}

run_phase() {
    local phase="$1" workload="$2" db_path="$3" output="$4"
    local start end
    local args=("${phase}" rocksdb -s -P "${YCSB_HOME%/}/workloads/${workload}" \
        -p "rocksdb.dir=${db_path}" -p "recordcount=${RECORD_COUNT}" \
        -p "operationcount=${OPERATION_COUNT}" -p "threadcount=${THREAD_COUNT}" \
        -p "fieldcount=${FIELD_COUNT}" -p "fieldlength=${FIELD_LENGTH}" \
        -p "kvimr.enabled=true" -p "kvimr.device=${MAPPER_DEVICE}" \
        -p "kvimr.zones=${ZONE_COUNT}" -p "kvimr.rmw_mode=${RMW_MODE}")
    while IFS= read -r property; do args+=("-p" "${property}"); done < <(workload_props "${workload}")
    start="$(date +%s%N)"
    set +e
    (
        cd "${YCSB_HOME}" || exit 1
        "${YCSB_BIN}" "${args[@]}"
    ) 2>&1 | tee "${output}"
    local status="${PIPESTATUS[0]}"
    set -e
    end="$(date +%s%N)"
    [[ "${status}" -eq 0 ]] || return "${status}"
    printf '%s\n' "$(((end - start) / 1000000))" > "${output}.runtime_ms"
    YCSB_VALIDATION_WORKLOAD="${workload}" \
        validate_output "${phase}" "${output}" "$([[ "${phase}" == load ]] && printf '%s' "${RECORD_COUNT}" || printf '%s' "${OPERATION_COUNT}")"
    validate_kvimr_invariants "${phase}" "${output}"
}

numeric_median() {
    awk -F',' -v workload="$1" -v field="$2" \
        'NR > 1 && $3 == workload && $6 == "PASS" && $field ~ /^[0-9]+([.][0-9]+)?$/ { print $field }' \
        "${RESULT_ROOT}/runs.csv" | sort -n | awk '{ v[NR]=$1 } END { if (!NR) print "NA"; else if (NR%2) print v[(NR+1)/2]; else printf "%.6f\n", (v[NR/2]+v[NR/2+1])/2 }'
}

write_medians() {
    printf '%s\n' 'workload,successful_runs,median_load_ycsb_runtime_ms,median_load_ycsb_throughput_ops_sec,median_load_durable_runtime_ms,median_load_durable_throughput_ops_sec,median_run_ycsb_runtime_ms,median_run_ycsb_throughput_ops_sec,median_run_durable_runtime_ms,median_run_durable_throughput_ops_sec,median_read_avg_us,median_read_p99_us,median_update_avg_us,median_update_p99_us,median_insert_avg_us,median_insert_p99_us,median_scan_avg_us,median_scan_p99_us,median_readmodifywrite_avg_us,median_readmodifywrite_p99_us,median_run_naive_rmw_count,median_run_naive_read_bytes,median_run_naive_write_bytes,median_run_merged_rmw_count,median_run_merged_logical_update_count,median_run_merged_updates_saved,median_run_merge_reduction_ratio,median_run_merged_buffer_create_count,median_run_merged_buffer_hit_count,median_run_merged_buffer_flush_count,median_run_merged_buffer_hit_ratio,median_load_logical_write_bytes,median_load_device_write_bytes,median_load_application_to_device_waf,median_load_device_waf,median_run_logical_write_bytes,median_run_device_write_bytes,median_run_application_to_device_waf,median_run_device_waf' > "${RESULT_ROOT}/medians.csv"

    local workload
    for workload in "${WORKLOADS[@]}"; do
        local count
        count="$(awk -F',' -v w="${workload}" \
            'NR>1 && $3==w && $6=="PASS" {n++} END{print n+0}' \
            "${RESULT_ROOT}/runs.csv")"

        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${workload}" "${count}" \
            "$(numeric_median "${workload}" 7)" \
            "$(numeric_median "${workload}" 8)" \
            "$(numeric_median "${workload}" 9)" \
            "$(numeric_median "${workload}" 10)" \
            "$(numeric_median "${workload}" 11)" \
            "$(numeric_median "${workload}" 12)" \
            "$(numeric_median "${workload}" 13)" \
            "$(numeric_median "${workload}" 14)" \
            "$(numeric_median "${workload}" 16)" \
            "$(numeric_median "${workload}" 17)" \
            "$(numeric_median "${workload}" 19)" \
            "$(numeric_median "${workload}" 20)" \
            "$(numeric_median "${workload}" 22)" \
            "$(numeric_median "${workload}" 23)" \
            "$(numeric_median "${workload}" 25)" \
            "$(numeric_median "${workload}" 26)" \
            "$(numeric_median "${workload}" 28)" \
            "$(numeric_median "${workload}" 29)" \
            "$(numeric_median "${workload}" 47)" \
            "$(numeric_median "${workload}" 48)" \
            "$(numeric_median "${workload}" 49)" \
            "$(numeric_median "${workload}" 50)" \
            "$(numeric_median "${workload}" 53)" \
            "$(numeric_median "${workload}" 54)" \
            "$(numeric_median "${workload}" 59)" \
            "$(numeric_median "${workload}" 56)" \
            "$(numeric_median "${workload}" 57)" \
            "$(numeric_median "${workload}" 58)" \
            "$(numeric_median "${workload}" 60)" \
            "$(numeric_median "${workload}" 61)" \
            "$(numeric_median "${workload}" 62)" \
            "$(numeric_median "${workload}" 63)" \
            "$(numeric_median "${workload}" 64)" \
            "$(numeric_median "${workload}" 65)" \
            "$(numeric_median "${workload}" 66)" \
            "$(numeric_median "${workload}" 67)" \
            "$(numeric_median "${workload}" 68)" \
            >> "${RESULT_ROOT}/medians.csv"
    done
}

write_comparison() {
    {
        printf '# KVIMR YCSB Workload A-F comparison\n\n'
        printf 'Mode: `%s`; values are medians of successful repetitions.\n\n' "${RMW_MODE}"
        printf '| Workload | Run YCSB ops/s | Run durable ops/s | Run logical write bytes | Run device write bytes | App-to-device WAF | Device WAF | Read avg us | Read P99 us | Write/RMW avg us | Write/RMW P99 us |\n'
        printf '|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n'
        tail -n +2 "$RESULT_ROOT/medians.csv" | awk -F',' '{
            w=$1;
            if (w=="workloada" || w=="workloadb") {
                a=$13; b=$14
            } else if (w=="workloadd" || w=="workloade") {
                a=$15; b=$16
            } else if (w=="workloadf") {
                a=$19; b=$20
            } else {
                a="NA"; b="NA"
            }

            printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", \
                $1,$8,$10,$36,$37,$38,$39,$11,$12,a,b
        }'
        printf '\n## RMW / merge counters\n\n'
        if [[ "${RMW_MODE}" == naive ]]; then
            printf '| Workload | Naive physical TOP RMW count | Naive read bytes | Naive write bytes |\n'
            printf '|---|---:|---:|---:|\n'
            tail -n +2 "${RESULT_ROOT}/medians.csv" | awk -F',' '{printf "| %s | %s | %s | %s |\n",$1,$21,$22,$23}'
        else
            printf '| Workload | Logical TOP updates | Physical TOP RMW count | Updates saved | Merge reduction ratio | Buffer creates | Buffer hits | Buffer flushes | Buffer hit ratio |\n'
            printf '|---|---:|---:|---:|---:|---:|---:|---:|---:|\n'
            tail -n +2 "${RESULT_ROOT}/medians.csv" | awk -F',' '{printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",$1,$25,$24,$26,$27,$28,$29,$30,$31}'
        fi
        printf '\nKVIMR SST payloads use the raw IMRSim mapper; the mapper is never formatted as ext4 or mounted. KVIMR counters are emitted by the binding and parsed into runs.csv.\n'
    } > "${RESULT_ROOT}/comparison.md"
}

run_one() {
    local workload="$1" repetition="$2" run_dir="${RESULT_ROOT}/${workload}/run-${repetition}"
    local db_path="${run_dir}/db" load_log="${run_dir}/ycsb-load.log" run_log="${run_dir}/ycsb-run.log"
    local load_status=PASS run_status=PASS start_dmesg end_dmesg kernel_count review
    mkdir -p -- "${run_dir}"
    prepare_mapper
    start_dmesg="${run_dir}/dmesg-before.log"; dmesg > "${start_dmesg}"
    dmsetup table "${MAPPER_NAME}" > "${run_dir}/mapper-table.txt"
    capture_stats "${run_dir}/before-load"
    mkdir -p -- "${db_path}"
    set +e; run_phase load "${workload}" "${db_path}" "${load_log}"; local rc=$?; set -e
    if [[ "${rc}" -ne 0 ]]; then load_status=FAIL; fi
    cp -a -- "${db_path}" "${run_dir}/db-after-load" 2>/dev/null || true
    capture_stats "${run_dir}/after-load"
    if [[ "${load_status}" == PASS ]]; then
        capture_stats "${run_dir}/before-run"
        set +e; run_phase run "${workload}" "${db_path}" "${run_log}"; rc=$?; set -e
        if [[ "${rc}" -ne 0 ]]; then run_status=FAIL; fi
    else
        run_status=SKIPPED
    fi
    capture_stats "${run_dir}/after-run"
    find "${db_path}" -maxdepth 1 -type f \( -name 'LOG*' -o -name 'KVIMR-META*' -o -name 'MANIFEST*' -o -name 'OPTIONS*' -o -name 'CURRENT' \) -exec cp -a {} "${run_dir}" \; 2>/dev/null || true
    end_dmesg="${run_dir}/dmesg-after.log"; dmesg > "${end_dmesg}"
    grep -Ei "${KERNEL_ISSUE_PATTERN}" "${end_dmesg}" > "${run_dir}/kernel-issues.log" || true
    kernel_count="$(awk 'END{print NR+0}' "${run_dir}/kernel-issues.log")"
    review="${load_status};${run_status}"
    [[ "${kernel_count}" -eq 0 ]] || review="${review};kernel_issue"
    local load_durable_runtime run_durable_runtime load_ycsb_runtime load_ycsb_throughput
    local run_ycsb_runtime run_ycsb_throughput load_durable_throughput run_durable_throughput meta_size
    local load_logical_write_bytes load_device_write_bytes load_application_to_device_waf
    local run_logical_write_bytes run_device_write_bytes run_application_to_device_waf
    local load_device_waf run_device_waf
    local counter_name load_merge_reduction_ratio run_merge_reduction_ratio
    local load_buffer_hit_ratio run_buffer_hit_ratio counter_value
    local kvimr_load_counters=() kvimr_run_counters=()
    load_durable_runtime="$(cat "${load_log}.runtime_ms" 2>/dev/null || printf NA)"
    run_durable_runtime="$(cat "${run_log}.runtime_ms" 2>/dev/null || printf NA)"
    load_ycsb_runtime="$(overall_metric "${load_log}" 'RunTime(ms)' || true)"
    load_ycsb_throughput="$(overall_metric "${load_log}" 'Throughput(ops/sec)' || true)"
    run_ycsb_runtime="$(overall_metric "${run_log}" 'RunTime(ms)' || true)"
    run_ycsb_throughput="$(overall_metric "${run_log}" 'Throughput(ops/sec)' || true)"
    load_durable_throughput="$(durable_throughput "${RECORD_COUNT}" "${load_durable_runtime}")"
    run_durable_throughput="$(durable_throughput "${OPERATION_COUNT}" "${run_durable_runtime}")"
    load_logical_write_bytes="$(application_logical_write_bytes load "$workload" "$load_log")"
    load_device_write_bytes="$(device_write_bytes_delta "$run_dir/before-load.device" "$run_dir/after-load.device")"
    load_application_to_device_waf="$(waf_or_na "$load_device_write_bytes" "$load_logical_write_bytes")"
    load_device_waf="$(device_waf_delta \
    "$run_dir/before-load.device" \
    "$run_dir/after-load.device" \
    "$run_dir/before-load.imrsim" \
    "$run_dir/after-load.imrsim")"
    run_logical_write_bytes="$(application_logical_write_bytes run "$workload" "$run_log")"
    run_device_write_bytes="$(device_write_bytes_delta "$run_dir/before-run.device" "$run_dir/after-run.device")"
    run_application_to_device_waf="$(waf_or_na "$run_device_write_bytes" "$run_logical_write_bytes")"
    run_device_waf="$(device_waf_delta \
    "$run_dir/before-run.device" \
    "$run_dir/after-run.device" \
    "$run_dir/before-run.imrsim" \
    "$run_dir/after-run.imrsim")"
    [[ "$load_logical_write_bytes" == NA || "$load_logical_write_bytes" -ge 0 ]] || fail "negative load logical write bytes"
    [[ "$run_logical_write_bytes" == NA || "$run_logical_write_bytes" -ge 0 ]] || fail "negative run logical write bytes"
    for counter_name in naive_rmw_count naive_read_bytes naive_write_bytes \
        merged_rmw_count merged_read_bytes merged_write_bytes \
        merged_logical_update_count merged_updates_saved direct_write_bytes \
        merged_buffer_create_count merged_buffer_hit_count merged_buffer_flush_count; do
        counter_value="$(kvimr_counter_value "${load_log}" "${counter_name}")"
        kvimr_load_counters+=("${counter_value}")
        counter_value="$(kvimr_counter_value "${run_log}" "${counter_name}")"
        kvimr_run_counters+=("${counter_value}")
    done
    load_merge_reduction_ratio="$(counter_ratio "${kvimr_load_counters[7]}" "$(counter_sum "${kvimr_load_counters[3]}" "${kvimr_load_counters[7]}")")"
    run_merge_reduction_ratio="$(counter_ratio "${kvimr_run_counters[7]}" "$(counter_sum "${kvimr_run_counters[3]}" "${kvimr_run_counters[7]}")")"
    load_buffer_hit_ratio="$(counter_ratio "${kvimr_load_counters[10]}" "$(counter_sum "${kvimr_load_counters[9]}" "${kvimr_load_counters[10]}")")"
    run_buffer_hit_ratio="$(counter_ratio "${kvimr_run_counters[10]}" "$(counter_sum "${kvimr_run_counters[9]}" "${kvimr_run_counters[10]}")")"
    meta_size="$(find "${db_path}" -type f -name 'KVIMR-META*' -exec stat -c '%s' {} \; 2>/dev/null | awk '{sum += $1} END {print sum + 0}')"
    local row=()
    row+=("KVIMR" "${RMW_MODE}" "${workload}" "${repetition}" "${load_status}" "${run_status}" "${load_ycsb_runtime}" "${load_ycsb_throughput}" "${load_durable_runtime}" "${load_durable_throughput}" "${run_ycsb_runtime}" "${run_ycsb_throughput}" "${run_durable_runtime}" "${run_durable_throughput}")
    for operation in READ UPDATE INSERT SCAN READ-MODIFY-WRITE; do
        row+=("$(operation_count "${run_log}" "${operation}")" "$(operation_latency "${run_log}" "${operation}" 'AverageLatency(us)')" "$(operation_latency "${run_log}" "${operation}" '99thPercentileLatency(us)')")
    done
    row+=("${meta_size}" "${kernel_count}" "${review}")
    row+=("${kvimr_load_counters[@]}" "${load_merge_reduction_ratio}" "${load_buffer_hit_ratio}")
    row+=("${kvimr_run_counters[@]}" "${run_merge_reduction_ratio}" "${run_buffer_hit_ratio}")
    row+=(
        "$load_logical_write_bytes"
        "$load_device_write_bytes"
        "$load_application_to_device_waf"
        "$load_device_waf"
        "$run_logical_write_bytes"
        "$run_device_write_bytes"
        "$run_application_to_device_waf"
        "$run_device_waf"
    )
    (IFS=,; printf '%s\n' "${row[*]}") >> "${RESULT_ROOT}/runs.csv"
    remove_mapper
    [[ "${load_status}" == PASS && "${run_status}" == PASS ]] || return 1
}

main() {
    local workload repetition rc
    [[ "$#" -eq 1 ]] || { usage >&2; fail "one backing device argument is required"; }
    require_tools; validate_config
    mkdir -p -- "${RESULT_ROOT}"
    trap cleanup EXIT
    write_manifest

    printf '%s\n' 'system,rmw_mode,workload,repetition,load_status,run_status,load_ycsb_runtime_ms,load_ycsb_throughput_ops_sec,load_durable_runtime_ms,load_durable_throughput_ops_sec,run_ycsb_runtime_ms,run_ycsb_throughput_ops_sec,run_durable_runtime_ms,run_durable_throughput_ops_sec,read_ops,read_avg_us,read_p99_us,update_ops,update_avg_us,update_p99_us,insert_ops,insert_avg_us,insert_p99_us,scan_ops,scan_avg_us,scan_p99_us,readmodifywrite_ops,readmodifywrite_avg_us,readmodifywrite_p99_us,kvimr_meta_size,kernel_issue_count,review,load_naive_rmw_count,load_naive_read_bytes,load_naive_write_bytes,load_merged_rmw_count,load_merged_read_bytes,load_merged_write_bytes,load_merged_logical_update_count,load_merged_updates_saved,load_direct_write_bytes,load_merged_buffer_create_count,load_merged_buffer_hit_count,load_merged_buffer_flush_count,load_merge_reduction_ratio,load_merged_buffer_hit_ratio,run_naive_rmw_count,run_naive_read_bytes,run_naive_write_bytes,run_merged_rmw_count,run_merged_read_bytes,run_merged_write_bytes,run_merged_logical_update_count,run_merged_updates_saved,run_direct_write_bytes,run_merged_buffer_create_count,run_merged_buffer_hit_count,run_merged_buffer_flush_count,run_merge_reduction_ratio,run_merged_buffer_hit_ratio,load_logical_write_bytes,load_device_write_bytes,load_application_to_device_waf,load_device_waf,run_logical_write_bytes,run_device_write_bytes,run_application_to_device_waf,run_device_waf' > "${RESULT_ROOT}/runs.csv"

    for repetition in $(seq 1 "${REPETITIONS}"); do
        for workload in "${WORKLOADS[@]}"; do
            set +e
            run_one "${workload}" "${repetition}"
            rc=$?
            set -e
            [[ "${rc}" -eq 0 ]] || fail "${workload} repetition ${repetition} failed; see ${RESULT_ROOT}"
        done
    done

    write_medians
    write_comparison
    log "PASS: results in ${RESULT_ROOT}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
