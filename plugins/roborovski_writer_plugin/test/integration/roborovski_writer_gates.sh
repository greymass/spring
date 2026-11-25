#!/usr/bin/env bash
# Runs every Roborovski writer fault and recovery gate once and keeps the logs; run from the Spring build directory.
set -uo pipefail

usage() {
   echo "usage: $0 <output dir> <roborovski source dir> <trace-patched nodeos> <chain fixture dir>" >&2
   exit 2
}
[[ $# -eq 4 ]] || usage
out=$(realpath -m "$1")
robo=$(realpath "$2")
trace_nodeos=$(realpath "$3")
fixture=$(realpath -m "$4")
build=$(pwd)
[[ -x bin/nodeos && -d tests ]] || { echo "run from the Spring build directory" >&2; exit 2; }
[[ $EUID -ne 0 ]] || { echo "run as a user with sudo, not as root" >&2; exit 2; }
mkdir -p "$out/bin"

{
   echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
   echo "spring: ${SPRING_COMMIT:-$(git -C "$build/.." rev-parse HEAD 2>/dev/null || echo unknown)}"
   echo "roborovski: ${ROBOROVSKI_COMMIT:-$(git -C "$robo" rev-parse HEAD 2>/dev/null || echo unknown)}"
   echo "nodeos: $(bin/nodeos --full-version)"
   echo "trace nodeos: $("$trace_nodeos" --full-version)"
   echo "kernel: $(uname -r)"
} > "$out/versions.txt"

(cd "$robo/services/coreindex" && go build -o "$out/bin/coreindex" ./cmd/coreindex) || exit 1
(cd "$robo/services/actionindex" && go build -o "$out/bin/actionindex" ./cmd/actionindex) || exit 1
export ROBOROVSKI_COREINDEX=$out/bin/coreindex
export ROBOROVSKI_ACTIONINDEX=$out/bin/actionindex
export ROBOROVSKI_TRACE_NODEOS=$trace_nodeos
export ROBOROVSKI_WRITER_CHAIN_FIXTURE=$fixture

declare -A results
gate() {
   local name=$1
   shift
   echo "== $name"
   "$@" > "$out/$name.log" 2>&1
   local code=$?
   results[$name]=$code
   if grep -q "^.*SKIPPED" "$out/$name.log"; then results[$name]="skipped"; fi
   echo "   exit ${results[$name]}"
}

if [[ ! -f $fixture/fixture.json ]]; then
   gate chain_fixture python3 tests/roborovski_writer_chain_fixture.py -v --unshared
fi
gate unit ./plugins/roborovski_writer_plugin/test/test_roborovski_writer_plugin
for t in crash two_writers restart fork cutover rotation; do
   gate "$t" python3 "tests/roborovski_writer_${t}_test.py" -v --unshared
done
gate disk sudo -E python3 tests/roborovski_writer_disk_test.py -v --unshared

img=$out/go-tmp.img
truncate -s 4G "$img"
loop=$(sudo losetup -f --show "$img")
sudo mkfs.ext4 -q -m 0 "$loop"
mkdir -p "$out/go-tmp"
sudo mount "$loop" "$out/go-tmp"
sudo chown "$(id -u)" "$out/go-tmp"
gate go_gates env TMPDIR="$out/go-tmp" bash -c "cd '$robo' && go test -count=1 ./libraries/coremanifest/... ./libraries/corereader/... ./libraries/abicache/... ./services/coreindex/..."
sudo umount "$out/go-tmp"
sudo losetup -d "$loop"
rm -f "$img"
rmdir "$out/go-tmp"

for name in "${!results[@]}"; do
   echo "$name ${results[$name]}"
done | sort | tee "$out/results.txt"
! grep -qv " 0$" "$out/results.txt"
