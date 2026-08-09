#!/usr/bin/env bash
# G7 verification driver for asm2c-03. Copied to the VM and run there.
# Every stage appends to /opt/quodlibet/verify.log and the summary lands in
# /opt/quodlibet/verify-summary.txt. No stage is fatal: a broken stage must
# not hide the stages after it (the machine bills by the hour, not by success).
mkdir -p /opt/quodlibet
cd /opt/quodlibet || exit 1
LOG=/opt/quodlibet/verify.log
SUM=/opt/quodlibet/verify-summary.txt
: > "$LOG"; : > "$SUM"
note() { echo "== $*" | tee -a "$LOG" >> "$SUM"; }

note "stage 0: environment $(date -u +%H:%M:%S)"
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>>"$LOG" | sort | uniq -c >> "$SUM"
{ cmake --version | head -1; ninja --version; clang --version | head -1; python3 --version; } >> "$SUM" 2>>"$LOG"

note "stage 1: extract source"
tar xzf /tmp/quodlibet-src.tar.gz -C /opt/quodlibet || note "  EXTRACT FAILED"
cd /opt/quodlibet/quodlibet || exit 1

note "stage 2: linux-clang build + ctest"
cmake --preset linux-clang >>"$LOG" 2>&1 \
  && cmake --build --preset linux-clang --parallel >>"$LOG" 2>&1 \
  && ctest --preset linux-clang >>"$LOG" 2>&1
grep -E "tests passed|tests failed" "$LOG" | tail -1 >> "$SUM"

note "stage 3: python bindings (venv, pip install, pytest)"
python3 -m venv /opt/quodlibet/venv >>"$LOG" 2>&1
/opt/quodlibet/venv/bin/pip install -q ./bindings/python pytest >>"$LOG" 2>&1 \
  || note "  PIP INSTALL FAILED"
/opt/quodlibet/venv/bin/python -c "import quodlibet; print('import OK')" >> "$SUM" 2>>"$LOG"
QL_BITWUZLA=/opt/quodlibet/quodlibet/third_party/bitwuzla-linux-x86_64/bin/bitwuzla \
  /opt/quodlibet/venv/bin/python -m pytest -q bindings/python/tests >>"$LOG" 2>&1
tail -1 "$LOG" >> "$SUM"

note "stage 4: scorer path, 100 pairs, per-pair latency"
/opt/quodlibet/venv/bin/python /opt/quodlibet/quodlibet/docs/vm/scorer_bench.py >> "$SUM" 2>>"$LOG" \
  || note "  SCORER BENCH FAILED"

note "stage 5: RL-path smoke, GPU load + concurrent batch"
/opt/quodlibet/venv/bin/pip install -q torch --index-url https://download.pytorch.org/whl/cu128 >>"$LOG" 2>&1 \
  || note "  TORCH INSTALL FAILED (stage skipped)"
/opt/quodlibet/venv/bin/python /opt/quodlibet/quodlibet/docs/vm/rl_smoke.py >> "$SUM" 2>>"$LOG" \
  || note "  RL SMOKE FAILED"

note "stage 6: vtune threading (if installable inside the budget)"
# Intentionally last and optional: the scorer verdict does not depend on it.
sudo bash -c 'wget -qO- https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB | gpg --dearmor --yes -o /usr/share/keyrings/oneapi-archive-keyring.gpg && echo "deb [signed-by=/usr/share/keyrings/oneapi-archive-keyring.gpg] https://apt.repos.intel.com/oneapi all main" > /etc/apt/sources.list.d/oneAPI.list && apt-get update -qq && apt-get install -y -qq intel-oneapi-vtune' >>"$LOG" 2>&1 \
  || note "  VTUNE INSTALL FAILED (report stays on the WSL/hotspots baseline)"
VT=/opt/intel/oneapi/vtune/latest/bin64/vtune
if [ -x "$VT" ]; then
    sudo sysctl -w kernel.yama.ptrace_scope=0 >>"$LOG" 2>&1
    rm -rf /tmp/vt-thr
    timeout 120 "$VT" -collect threading -result-dir /tmp/vt-thr -- \
        /opt/quodlibet/venv/bin/python /opt/quodlibet/quodlibet/docs/vm/scorer_bench.py --quick >>"$LOG" 2>&1
    "$VT" -report summary -r /tmp/vt-thr 2>>"$LOG" | grep -E "Elapsed|CPU Time|Wait Time|Effective" | head -8 >> "$SUM"
fi

note "done $(date -u +%H:%M:%S)"
cat "$SUM"
