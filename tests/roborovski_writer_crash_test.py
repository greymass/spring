#!/usr/bin/env python3

import glob
import hashlib
import os
import signal
import struct
import threading
import time

from TestHarness import Cluster, TestHelper, Utils, WalletMgr

# Kills the writer at each live persistence boundary and checks recovery without operator action.

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})
Utils.Debug = args.v
cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, port=TestHelper.DEFAULT_WALLET_PORT)
testSuccessful = False

prodNodeId = 0
writerNodeId = 1
FAULT_ENV = "ROBOROVSKI_WRITER_FAULT"
LIVE_POINTS = ["buffer_synced", "abi_synced", "core_synced", "manifest_renamed", "manifest_dir_synced"]

MANIFEST = struct.Struct("<8sII16s16sQII32sQIIQII")
INDEX_ENTRY = struct.Struct("<IQIQQ")


def readManifest(store):
    with open(os.path.join(store, "control", "manifest"), "rb") as f:
        raw = f.read()
    (magic, _, _, coreUUID, abiUUID, epoch, head, abiIndexLen, headID,
     sliceDataEnd, sliceIndexLen, abiFloor, abiDataEnd, _, _) = MANIFEST.unpack(raw)
    assert magic == b"RBMANIF\0", f"bad manifest magic {magic}"
    return {"epoch": epoch, "head": head, "abiIndexLen": abiIndexLen, "headID": headID.hex(),
            "sliceDataEnd": sliceDataEnd, "sliceIndexLen": sliceIndexLen, "abiDataEnd": abiDataEnd}


def committedView(store):
    """Digest of every record and ABI byte the manifest marks committed."""
    m = readManifest(store)
    blocks = {}
    for sliceDir in sorted(glob.glob(os.path.join(store, "history_*-*"))):
        first, last = (int(x) for x in os.path.basename(sliceDir)[len("history_"):].split("-"))
        if first > m["head"]:
            continue
        headSlice = first <= m["head"] <= last
        with open(os.path.join(sliceDir, "blocks.index"), "rb") as f:
            index = f.read()
        count = m["sliceIndexLen"] if headSlice else struct.unpack_from("<I", index, 0)[0]
        with open(os.path.join(sliceDir, "data.log"), "rb") as f:
            data = f.read()
        if headSlice:
            data = data[:m["sliceDataEnd"]]
        for i in range(count):
            num, offset, size, _, _ = INDEX_ENTRY.unpack_from(index, 4 + i * INDEX_ENTRY.size)
            assert offset + size <= len(data), f"block {num} lies past committed data in {sliceDir}"
            assert num not in blocks, f"block {num} stored twice"
            blocks[num] = hashlib.sha256(data[offset:offset + size]).hexdigest()
    abiRoot = os.path.join(store, "abis")
    with open(os.path.join(abiRoot, "abis.dat"), "rb") as f:
        abiData = f.read()[:m["abiDataEnd"]]
    with open(os.path.join(abiRoot, "abis.index"), "rb") as f:
        abiIndex = f.read()[:m["abiIndexLen"] * 20]
    return m, blocks, hashlib.sha256(abiData).hexdigest(), abiIndex


def checkContiguous(m, blocks, stage):
    nums = sorted(blocks)
    assert nums, f"{stage}: no committed blocks"
    assert nums[-1] == m["head"], f"{stage}: last stored block {nums[-1]}, manifest head {m['head']}"
    assert nums == list(range(nums[0], nums[-1] + 1)), f"{stage}: committed blocks have a gap"


def latestStderr(node):
    files = sorted(glob.glob(os.path.join(Utils.getNodeDataDir(node.nodeId), "stderr.*.txt")), key=os.path.getmtime)
    with open(files[-1]) as f:
        return f.read()


def markDead(node):
    node.pid = None
    node.killed = True


def waitForExit(node, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if node.popenProc.poll() is not None:
            return node.popenProc.returncode
        time.sleep(0.2)
    return None


class HeadWatcher(threading.Thread):
    """Polls the manifest as a reader would and records any move backwards."""

    def __init__(self, store, start):
        super().__init__(daemon=True)
        self.store, self.last, self.stop, self.regressed = store, start, threading.Event(), None

    def run(self):
        while not self.stop.is_set():
            try:
                m = readManifest(self.store)
            except (OSError, AssertionError, struct.error):
                time.sleep(0.01)
                continue
            if (m["epoch"], m["head"]) < self.last:
                self.regressed = (self.last, (m["epoch"], m["head"]))
                return
            self.last = (m["epoch"], m["head"])
            time.sleep(0.02)


def crashAndRecover(prodNode, writer, store, point):
    Print(f"Fault {point}: arm and relaunch the writer")
    writer.kill(signal.SIGTERM)
    fromBlock = prodNode.getIrreversibleBlockNum() + 8
    os.environ[FAULT_ENV] = f"{point}:kill:{fromBlock}"
    try:
        writer.relaunch(timeout=30)
        code = waitForExit(writer, 180)
    finally:
        del os.environ[FAULT_ENV]
    assert code == -signal.SIGKILL, f"{point}: writer exit {code}, expected SIGKILL"
    markDead(writer)
    log = latestStderr(writer)
    assert f"ROBOROVSKI_WRITER_FAULT reached {point}" in log, f"{point}: the log does not show the fault point"

    before, beforeBlocks, beforeABI, beforeIndex = committedView(store)
    checkContiguous(before, beforeBlocks, f"{point} after the kill")
    Print(f"Fault {point}: killed with block {before['head']} committed; restart with the replay a dirty chainbase needs")

    watcher = HeadWatcher(store, (before["epoch"], before["head"]))
    watcher.start()
    replay = None if "--replay-blockchain" in writer.cmd else "--replay-blockchain"
    assert writer.relaunch(chainArg=replay, timeout=120), f"{point}: writer did not restart"
    target = prodNode.getIrreversibleBlockNum() + 5
    deadline = time.time() + 120
    while readManifest(store)["head"] < target and time.time() < deadline:
        time.sleep(0.5)
    watcher.stop.set()
    watcher.join()
    assert watcher.regressed is None, f"{point}: a reader saw the head move from {watcher.regressed[0]} to {watcher.regressed[1]}"

    after, afterBlocks, afterABI, afterIndex = committedView(store)
    assert after["head"] >= target, f"{point}: writer reached {after['head']}, expected {target}"
    assert after["epoch"] == before["epoch"], f"{point}: epoch changed across the crash"
    checkContiguous(after, afterBlocks, f"{point} after recovery")
    for num, digest in beforeBlocks.items():
        assert afterBlocks.get(num) == digest, f"{point}: committed block {num} changed across the crash"
    assert afterIndex.startswith(beforeIndex), f"{point}: committed ABI index entries changed"
    Print(f"Fault {point}: recovered to block {after['head']}, committed blocks {min(beforeBlocks)}-{before['head']} unchanged")


try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    specificExtraNodeosArgs = {
        prodNodeId: "--plugin eosio::producer_api_plugin --production-pause-vote-timeout-ms 0",
        writerNodeId: "--plugin eosio::roborovski_writer_plugin --roborovski-writer-headroom-mb 256",
    }
    if cluster.launch(topo="mesh", pnodes=1, totalNodes=2, activateIF=True, biosFinalizer=False,
                      specificExtraNodeosArgs=specificExtraNodeosArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")
    cluster.waitOnClusterSync(blockAdvancing=5)
    cluster.biosNode.kill(signal.SIGTERM)

    prodNode = cluster.getNode(prodNodeId)
    writer = cluster.getNode(writerNodeId)
    store = os.path.join(Utils.getNodeDataDir(writerNodeId), "roborovski")

    deadline = time.time() + 60
    while not os.path.exists(os.path.join(store, "control", "manifest")) and time.time() < deadline:
        time.sleep(0.5)
    m, blocks, _, _ = committedView(store)
    checkContiguous(m, blocks, "first commit")

    for point in LIVE_POINTS:
        crashAndRecover(prodNode, writer, store, point)

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
