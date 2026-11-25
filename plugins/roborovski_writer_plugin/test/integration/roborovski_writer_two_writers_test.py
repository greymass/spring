#!/usr/bin/env python3

import os
import shutil
import signal
import subprocess
import time

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from roborovski_writer_store import checkContiguous, committedView, latestStderr, launchExpectingExit, markDead, readManifest

# A second writer aimed at a running writer's core storage, ABI cache or buffer must refuse before it changes anything.

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})
Utils.Debug = args.v
cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, port=TestHelper.DEFAULT_WALLET_PORT)
testSuccessful = False

COREINDEX_ENV = "ROBOROVSKI_COREINDEX"
LOCK_HELD = "another writer holds the lock"

prodNodeId = 0
firstId = 1
secondId = 2


def fileSet(root):
    found = set()
    for dirpath, _, files in os.walk(root):
        for name in files:
            if ".tmp-" in name:
                continue
            found.add(os.path.relpath(os.path.join(dirpath, name), root))
    return found


def treeState(root):
    state = {}
    for rel in fileSet(root):
        state[rel] = os.path.getsize(os.path.join(root, rel))
    return state


def ownDirsUntouched(dirs, case):
    for d in dirs:
        if not os.path.exists(d):
            continue
        extra = fileSet(d) - {"writer.lock"}
        assert not extra, f"{case}: the refused writer created {sorted(extra)} in {d}"


def firstContinues(prodNode, first, store, before, case):
    assert first.verifyAlive(), f"{case}: the first writer stopped"
    target = prodNode.getIrreversibleBlockNum() + 3
    deadline = time.time() + 60
    while readManifest(store)["head"] < target and time.time() < deadline:
        time.sleep(0.5)
    after, afterBlocks, _, afterIndex = committedView(store)
    beforeM, beforeBlocks, _, beforeIndex = before
    assert after["head"] >= target, f"{case}: the first writer reached {after['head']}, expected {target}"
    assert after["epoch"] == beforeM["epoch"], f"{case}: the epoch changed"
    checkContiguous(after, afterBlocks, case)
    for num, digest in beforeBlocks.items():
        assert afterBlocks.get(num) == digest, f"{case}: committed block {num} changed"
    assert afterIndex.startswith(beforeIndex), f"{case}: committed ABI index entries changed"


def sharedFilesKept(sharedRoots, beforeTrees, case):
    for root in sharedRoots:
        now = treeState(root)
        for rel, size in beforeTrees[root].items():
            assert rel in now, f"{case}: {rel} disappeared from {root}"
            assert now[rel] >= size, f"{case}: {rel} in {root} shrank from {size} to {now[rel]} bytes"


def secondPluginRefuses(prodNode, first, second, store, coreDir, abiDir, bufDir, case, sharedRoots, ownDirs):
    Print(f"{case}: start a second writer node")
    for d in ownDirs:
        shutil.rmtree(d, ignore_errors=True)
    before = committedView(store)
    beforeTrees = {r: treeState(r) for r in sharedRoots}
    flags = {"--roborovski-writer-dir": coreDir, "--roborovski-writer-abi-dir": abiDir,
             "--roborovski-writer-buffer-dir": bufDir}
    code = launchExpectingExit(second, flags, 60)
    assert code not in (None, 0), f"{case}: the second writer exited with {code}"
    log = latestStderr(second)
    assert LOCK_HELD in log, f"{case}: the second writer's log does not name the held lock"
    ownDirsUntouched(ownDirs, case)
    sharedFilesKept(sharedRoots, beforeTrees, case)
    firstContinues(prodNode, first, store, before, case)
    Print(f"{case}: refused with exit {code}; the first writer continued")


def coreindexRefuses(prodNode, first, store, abiDir, chainID, case):
    coreindex = os.environ.get(COREINDEX_ENV)
    if not coreindex:
        Print(f"SKIPPED {case}: set {COREINDEX_ENV} to a coreindex binary")
        return False
    Print(f"{case}: start a trace-mode coreindex on the first writer's storage")
    traceDir = os.path.join(Utils.getNodeDataDir(secondId), "empty-traces")
    os.makedirs(traceDir, exist_ok=True)
    before = committedView(store)
    beforeTrees = {r: treeState(r) for r in (store, abiDir)}
    cmd = [coreindex, "--index-storage", store, "--abi-source", abiDir, "--trace-dir", traceDir,
           "--chain-id", chainID, "--http-listen", "none", "--http-socket", "none",
           "--stream-enabled=false", "--zstd"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    output = proc.stdout + proc.stderr
    assert proc.returncode != 0, f"{case}: coreindex exited 0"
    assert "Cannot take ownership of storage" in output and "lock" in output, \
        f"{case}: coreindex did not refuse on the lock:\n{output[-2000:]}"
    sharedFilesKept((store, abiDir), beforeTrees, case)
    firstContinues(prodNode, first, store, before, case)
    Print(f"{case}: refused with exit {proc.returncode}; the first writer continued")
    return True


try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    writerArgs = "--plugin eosio::roborovski_writer_plugin --roborovski-writer-headroom-mb 256"
    specificExtraNodeosArgs = {
        prodNodeId: "--plugin eosio::producer_api_plugin --production-pause-vote-timeout-ms 0",
        firstId: writerArgs,
        secondId: writerArgs,
    }
    if cluster.launch(topo="mesh", pnodes=1, totalNodes=3, activateIF=True, biosFinalizer=False,
                      specificExtraNodeosArgs=specificExtraNodeosArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")
    cluster.waitOnClusterSync(blockAdvancing=5)
    cluster.biosNode.kill(signal.SIGTERM)

    prodNode = cluster.getNode(prodNodeId)
    first = cluster.getNode(firstId)
    second = cluster.getNode(secondId)
    second.kill(signal.SIGTERM)
    markDead(second)

    store = os.path.join(Utils.getNodeDataDir(firstId), "roborovski")
    abiDir = os.path.join(store, "abis")
    bufDir = os.path.join(store, "buffer")
    deadline = time.time() + 60
    while not os.path.exists(os.path.join(store, "control", "manifest")) and time.time() < deadline:
        time.sleep(0.5)
    m, blocks, _, _ = committedView(store)
    checkContiguous(m, blocks, "first commit")

    own = os.path.join(Utils.getNodeDataDir(secondId), "own")
    ownCore, ownABI, ownBuf = (os.path.join(own, n) for n in ("core", "abis", "buffer"))

    secondPluginRefuses(prodNode, first, second, store, store, ownABI, ownBuf, "shared core storage",
                        (store,), (ownABI, ownBuf))
    secondPluginRefuses(prodNode, first, second, store, ownCore, abiDir, ownBuf, "shared ABI cache",
                        (abiDir,), (ownCore, ownBuf))
    secondPluginRefuses(prodNode, first, second, store, store, abiDir, ownBuf, "shared core storage and ABI cache",
                        (store,), (ownBuf,))
    chainID = prodNode.getInfo()["chain_id"]
    coreindexRefuses(prodNode, first, store, abiDir, chainID, "trace-mode coreindex on shared storage")

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)

exit(0 if testSuccessful else 1)
