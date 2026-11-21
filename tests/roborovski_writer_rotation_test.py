#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time

from TestHarness import Utils
from roborovski_writer_store import HeadWatcher, checkContiguous, committedBlockIDs, committedView, readManifest

# Kills the writer at the persistence points a replay past the first slice reaches, then removes and rebuilds a slice.

Print = Utils.Print

parser = argparse.ArgumentParser()
parser.add_argument("-v", action="store_true")
parser.add_argument("--keep-logs", action="store_true")
args, _ = parser.parse_known_args()
Utils.Debug = args.v

FIXTURE_ENV = "ROBOROVSKI_WRITER_CHAIN_FIXTURE"
COREINDEX_ENV = "ROBOROVSKI_COREINDEX"
FAULT_ENV = "ROBOROVSKI_WRITER_FAULT"
SLICE = 10000

fixture = os.environ.get(FIXTURE_ENV)
coreindex = os.environ.get(COREINDEX_ENV)
if not fixture or not coreindex:
    Print(f"SKIPPED: set {FIXTURE_ENV} to the output of roborovski_writer_chain_fixture.py and {COREINDEX_ENV} to a coreindex binary")
    sys.exit(77)
with open(os.path.join(fixture, "fixture.json")) as f:
    fixtureLib = json.load(f)["lib"]
target = fixtureLib - 10
assert target > SLICE + 100, f"the fixture's LIB {fixtureLib} is not far enough past block {SLICE}"

work = os.path.join(Utils.TestLogRoot, f"roborovski_writer_rotation_test{os.getpid()}")
dataDir = os.path.join(work, "node")
store = os.path.join(dataDir, "roborovski")
runs = 0


def startNode(fault=None):
    global runs
    runs += 1
    env = dict(os.environ)
    env.pop(FAULT_ENV, None)
    if fault:
        env[FAULT_ENV] = fault
    cmd = [Utils.EosServerPath, "--data-dir", dataDir, "--config-dir", dataDir, "--replay-blockchain",
           "--plugin", "eosio::roborovski_writer_plugin", "--roborovski-writer-headroom-mb", "256",
           "--http-server-address", "127.0.0.1:8888", "--chain-state-db-size-mb", "4096"]
    log = open(os.path.join(work, f"stderr.{runs}.txt"), "w")
    return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env), log.name


def logText(path):
    with open(path) as f:
        return f.read()


def waitHead(proc, want, what, timeout=900):
    deadline = time.time() + timeout
    while time.time() < deadline:
        assert proc.poll() is None, f"{what}: nodeos exited with {proc.returncode}"
        try:
            if readManifest(store)["head"] >= want:
                return
        except (OSError, AssertionError):
            pass
        time.sleep(1)
    raise AssertionError(f"{what}: the writer did not commit block {want} within {timeout} s")


def stopNode(proc):
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=120)


def crashAt(point, fromBlock):
    Print(f"Fault {point}: replay with the fault armed from block {fromBlock}")
    proc, log = startNode(f"{point}:kill" if fromBlock is None else f"{point}:kill:{fromBlock}")
    code = proc.wait(timeout=900)
    assert code == -signal.SIGKILL, f"{point}: nodeos exit {code}, expected SIGKILL"
    assert f"ROBOROVSKI_WRITER_FAULT reached {point}" in logText(log), f"{point}: the log does not show the fault point"
    if not os.path.exists(os.path.join(store, "control", "manifest")) or readManifest(store)["head"] == 0:
        Print(f"Fault {point}: killed before the first commit")
        return None
    view = committedView(store)
    checkContiguous(view[0], view[1], f"{point} after the kill")
    Print(f"Fault {point}: killed with block {view[0]['head']} committed")
    return view


def recover(point, before, upTo):
    m = readManifest(store) if before else None
    watcher = HeadWatcher(store, (m["epoch"], m["head"])) if m else None
    if watcher:
        watcher.start()
    proc, _ = startNode()
    try:
        waitHead(proc, upTo, f"{point} recovery")
    finally:
        if watcher:
            watcher.stop.set()
            watcher.join()
    stopNode(proc)
    if watcher:
        assert watcher.regressed is None, f"{point}: a reader saw the head move from {watcher.regressed[0]} to {watcher.regressed[1]}"
    after = committedView(store)
    checkContiguous(after[0], after[1], f"{point} after recovery")
    assert min(after[1]) == 2, f"{point}: history starts at block {min(after[1])}"
    if before:
        assert after[0]["epoch"] == before[0]["epoch"], f"{point}: epoch changed across the crash"
        for num, digest in before[1].items():
            assert after[1].get(num) == digest, f"{point}: committed block {num} changed across the crash"
        assert after[3].startswith(before[3]), f"{point}: committed ABI index entries changed"
    Print(f"Fault {point}: recovered to block {after[0]['head']} with no operator action")
    return after


def sliceDirs():
    return sorted(d for d in os.listdir(store) if d.startswith("history_"))


testSuccessful = False
try:
    os.makedirs(os.path.join(dataDir, "blocks"))
    for name in ("blocks.log", "blocks.index"):
        shutil.copy2(os.path.join(fixture, name), os.path.join(dataDir, "blocks", name))

    before = crashAt("abi_initialized", None)
    assert before is None, "abi_initialized: a commit preceded ABI initialization on a fresh store"
    recover("abi_initialized", None, 1000)

    before = crashAt("slice_rotated", SLICE + 1)
    full = recover("slice_rotated", before, target)
    dirs = sliceDirs()
    assert dirs == ["history_0000000001-0000010000", "history_0000010001-0000020000"], f"slice directories {dirs}"
    with open(os.path.join(store, "slices.json")) as f:
        slices = json.load(f)
    assert slices[0]["finalized"] and not slices[1]["finalized"], f"slices.json {slices}"
    fullIDs = committedBlockIDs(store, sorted(full[1]))

    Print("segment_reclaimed: replay into an empty store")
    shutil.rmtree(store)
    before = crashAt("segment_reclaimed", SLICE)
    assert before[0]["head"] >= SLICE, f"segment_reclaimed: killed at committed block {before[0]['head']}"
    nums = sorted(full[1])
    after = recover("segment_reclaimed", before, full[0]["head"])
    afterIDs = committedBlockIDs(store, nums)
    wrong = [n for n in nums if afterIDs.get(n) != fullIDs[n]]
    assert not wrong, f"segment_reclaimed: blocks {wrong[:5]} carry different IDs from the first replay"
    Print(f"segment_reclaimed: the second replay holds the same block IDs through block {full[0]['head']}")
    full, fullIDs = after, afterIDs

    what = "the operator removed a suffix of slices"
    Print(f"{what}: coreindex --reset-after-block {SLICE}")
    reset = subprocess.run([coreindex, "--index-storage", store, "--abi-source", os.path.join(store, "abis"),
                            "--reset-after-block", str(SLICE)], capture_output=True, text=True, timeout=300)
    assert reset.returncode == 0, f"{what}: coreindex exited {reset.returncode}:\n{reset.stdout}{reset.stderr}"
    m = readManifest(store)
    assert m["head"] == SLICE, f"{what}: manifest head {m['head']} after the reset"
    assert m["epoch"] == full[0]["epoch"] + 1, f"{what}: epoch {m['epoch']}, expected {full[0]['epoch'] + 1}"
    assert sliceDirs() == ["history_0000000001-0000010000"], f"{what}: slice directories {sliceDirs()}"
    kept = committedView(store)
    for num, digest in kept[1].items():
        assert full[1].get(num) == digest, f"{what}: retained block {num} changed"

    watcher = HeadWatcher(store, (m["epoch"], m["head"]))
    watcher.start()
    proc, log = startNode()
    try:
        waitHead(proc, full[0]["head"], what)
    finally:
        watcher.stop.set()
        watcher.join()
    stopNode(proc)
    assert watcher.regressed is None, f"{what}: a reader saw the head move from {watcher.regressed[0]} to {watcher.regressed[1]}"
    rebuilt = committedView(store)
    checkContiguous(rebuilt[0], rebuilt[1], what)
    assert rebuilt[0]["epoch"] == m["epoch"], f"{what}: epoch changed during the rebuild"
    for num in range(2, SLICE + 1):
        assert rebuilt[1].get(num) == full[1][num], f"{what}: retained block {num} changed during the rebuild"
    rebuiltIDs = committedBlockIDs(store, list(range(SLICE + 1, full[0]["head"] + 1)))
    wrong = [n for n in rebuiltIDs if rebuiltIDs[n] != fullIDs[n]]
    assert not wrong and len(rebuiltIDs) == full[0]["head"] - SLICE, f"{what}: rebuilt blocks {wrong[:5]} differ"
    assert rebuilt[3].startswith(kept[3]), f"{what}: retained ABI index entries changed"
    Print(f"{what}: kept blocks 2-{SLICE}, rebuilt {SLICE + 1}-{full[0]['head']} with the same IDs under epoch {m['epoch']}")

    testSuccessful = True
finally:
    if testSuccessful and not args.keep_logs:
        shutil.rmtree(work, ignore_errors=True)
    else:
        Print(f"logs and store kept in {work}")

sys.exit(0 if testSuccessful else 1)
