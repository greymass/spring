#!/usr/bin/env python3

import hashlib
import os
import shutil
import signal
import struct
import time

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from roborovski_writer_store import (checkContiguous, committedView, latestStderr, launchExpectingExit, markDead,
                                     readManifest, waitForExit)

# One scenario per row of the writer's restart and recovery table; the removed-suffix row is in the rotation test.

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})
Utils.Debug = args.v
cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, port=TestHelper.DEFAULT_WALLET_PORT)
testSuccessful = False

prodNodeId = 0
writerId = 1
snapWriterId = 2
FAULT_ENV = "ROBOROVSKI_WRITER_FAULT"
REPLAY = "--replay-blockchain"


def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def outputDigest(store):
    """Digest of each core and ABI file a writer owns, leaving out its lock and the buffer."""
    digests = {}
    for dirpath, dirs, files in os.walk(store):
        dirs[:] = [d for d in dirs if d != "buffer"]
        for name in files:
            if name == "writer.lock":
                continue
            path = os.path.join(dirpath, name)
            with open(path, "rb") as f:
                digests[os.path.relpath(path, store)] = hashlib.sha256(f.read()).hexdigest()
    return digests


def smallFiles(store, names):
    contents = {}
    for rel in names:
        path = os.path.join(store, rel)
        if os.path.getsize(path) <= 65536:
            with open(path, "rb") as f:
                contents[rel] = f.read()
    return contents


def changedFiles(before, after):
    return sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))


def stop(node):
    node.kill(signal.SIGTERM)
    markDead(node)


def relaunch(node, chainArg=None):
    assert node.relaunch(chainArg=chainArg, timeout=120), f"node {node.nodeId} did not restart"
    for flag in (REPLAY, "--snapshot"):
        while flag in node.cmd:
            i = node.cmd.index(flag)
            node.cmd.pop(i)
            if flag == "--snapshot":
                node.cmd.pop(i)


def waitCommitted(store, target, what, timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if readManifest(store)["head"] >= target:
                return
        except (OSError, AssertionError, struct.error):
            pass
        time.sleep(0.5)
    raise AssertionError(f"{what}: the writer did not commit block {target} within {timeout} s")


def keptAndContinues(prodNode, store, before, what):
    beforeM, beforeBlocks, _, beforeIndex = before
    waitCommitted(store, prodNode.getIrreversibleBlockNum() + 3, what)
    after, afterBlocks, _, afterIndex = committedView(store)
    checkContiguous(after, afterBlocks, what)
    assert after["epoch"] == beforeM["epoch"], f"{what}: the epoch changed"
    for num, digest in beforeBlocks.items():
        assert afterBlocks.get(num) == digest, f"{what}: committed block {num} changed"
    assert afterIndex.startswith(beforeIndex), f"{what}: committed ABI index entries changed"
    return after, afterBlocks


CONTROLLER_FATAL = "Shutting down due to controller_emit_signal_exception"


def refuses(node, store, flags, chainArgs, message, what, atStartup, timeout=180):
    """Starts a stopped writer that must stop on its own, and checks it named the problem and rewrote nothing."""
    digest = outputDigest(store)
    small = smallFiles(store, digest)
    swap = dict(flags)
    for k, v in chainArgs:
        swap[k] = v
    code = launchExpectingExit(node, swap, timeout)
    for k, _ in chainArgs:
        while k in node.cmd:
            i = node.cmd.index(k)
            node.cmd.pop(i)
            if i < len(node.cmd) and not node.cmd[i].startswith("--"):
                node.cmd.pop(i)
    assert code is not None, f"{what}: the writer kept running"
    log = latestStderr(node)
    if atStartup:
        assert code != 0, f"{what}: nodeos exited 0 after a startup failure"
    else:
        assert CONTROLLER_FATAL in log, f"{what}: nodeos did not stop through the controller-fatal path"
    assert message in log, f"{what}: the log does not contain '{message}'"
    changed = changedFiles(digest, outputDigest(store))
    if changed:
        for rel in changed:
            path = os.path.join(store, rel)
            size = os.path.getsize(path) if os.path.exists(path) else None
            Print(f"{what}: {rel} changed, now {size} bytes; before: {small.get(rel, '(large)')}")
            if size is not None and size <= 65536:
                with open(path, "rb") as f:
                    Print(f"{what}: {rel} after: {f.read()!r}")
    assert not changed, f"{what}: core storage or the ABI cache changed: {changed}"
    Print(f"{what}: stopped with exit {code} and changed nothing")


def takeSnapshot(prodNode):
    ret = prodNode.createSnapshot()
    assert ret is not None, "snapshot creation failed"
    payload = ret["payload"]
    return payload["snapshot_name"], payload["head_block_num"]


def freshFromSnapshot(node, snapshot):
    node.removeDataDir()
    relaunch(node, f"--snapshot {snapshot}")


try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    writerArgs = "--plugin eosio::roborovski_writer_plugin --roborovski-writer-headroom-mb 256"
    specificExtraNodeosArgs = {
        prodNodeId: "--plugin eosio::producer_api_plugin --production-pause-vote-timeout-ms 0",
        writerId: writerArgs,
        snapWriterId: writerArgs,
    }
    if cluster.launch(topo="mesh", pnodes=1, totalNodes=3, activateIF=True, biosFinalizer=False,
                      specificExtraNodeosArgs=specificExtraNodeosArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")
    cluster.waitOnClusterSync(blockAdvancing=5)
    cluster.biosNode.kill(signal.SIGTERM)

    prodNode = cluster.getNode(prodNodeId)
    writer = cluster.getNode(writerId)
    snapWriter = cluster.getNode(snapWriterId)
    store = os.path.join(Utils.getNodeDataDir(writerId), "roborovski")
    snapStore = os.path.join(Utils.getNodeDataDir(snapWriterId), "roborovski")
    stop(snapWriter)
    waitCommitted(store, 2, "genesis start", 60)

    what = "nodeos resumes behind published history"
    Print(f"{what}: stop the writer and replay from the block log")
    stop(writer)
    before = committedView(store)
    relaunch(writer, REPLAY)
    keptAndContinues(prodNode, store, before, what)
    Print(f"{what}: kept blocks {min(before[1])}-{before[0]['head']} and continued")

    what = "nodeos resumes ahead of core storage with the buffer"
    headAtStop = writer.getHeadBlockNum()
    stop(writer)
    before = committedView(store)
    stopped = before[0]["head"]
    assert headAtStop > stopped, f"{what}: nodeos head {headAtStop} is not above committed block {stopped}"
    assert f"stopped at committed block {stopped}" in latestStderr(writer), f"{what}: no clean stop log"
    relaunch(writer)
    keptAndContinues(prodNode, store, before, what)
    Print(f"{what}: blocks {stopped + 1}-{headAtStop}, which nodeos did not execute again, came from the buffer")

    what = "required output is in neither committed storage nor the buffer"
    headAtStop = writer.getHeadBlockNum()
    stop(writer)
    stopped = readManifest(store)["head"]
    assert headAtStop > stopped, f"{what}: nodeos head {headAtStop} is not above committed block {stopped}"
    buffer = os.path.join(store, "buffer")
    for name in os.listdir(buffer):
        if name != "writer.lock":
            os.remove(os.path.join(buffer, name))
    refuses(writer, store, {}, [], "is in neither committed storage nor the buffer", what, False)
    before = committedView(store)
    relaunch(writer, REPLAY)
    keptAndContinues(prodNode, store, before, f"{what}, recovered by replay")

    what = "an append or publication was interrupted"
    stop(writer)
    os.environ[FAULT_ENV] = f"core_synced:kill:{prodNode.getIrreversibleBlockNum() + 8}"
    try:
        relaunch(writer)
        code = waitForExit(writer, 180)
    finally:
        del os.environ[FAULT_ENV]
    assert code == -signal.SIGKILL, f"{what}: writer exit {code}, expected SIGKILL"
    markDead(writer)
    before = committedView(store)
    relaunch(writer, REPLAY)
    keptAndContinues(prodNode, store, before, what)
    Print(f"{what}: committed blocks through {before[0]['head']} kept, none stored twice")

    what = "committed history has a conflicting identity"
    stop(writer)
    manifestPath = os.path.join(store, "control", "manifest")
    with open(manifestPath, "rb") as f:
        original = f.read()
    forged = bytearray(original)
    forged[64] ^= 0xFF
    forged[124:128] = struct.pack("<I", crc32c(bytes(forged[:124])))
    with open(manifestPath, "wb") as f:
        f.write(forged)
    head = readManifest(store)["head"]
    refuses(writer, store, {}, [(REPLAY, None)], "in committed history; stop and repair core storage", what, False)
    with open(manifestPath, "wb") as f:
        f.write(original)
    before = committedView(store)
    relaunch(writer, REPLAY)
    keptAndContinues(prodNode, store, before, f"{what}, after the operator restored block {head}")

    what = "nodeos cannot open its saved state after a crash"
    writer.kill(signal.SIGKILL)
    markDead(writer)
    before = committedView(store)
    code = launchExpectingExit(writer, {}, 60)
    assert code not in (None, 0), f"{what}: nodeos started on a dirty database (exit {code})"
    after = committedView(store)
    assert after[1] == before[1] and after[3] == before[3], f"{what}: committed history changed"
    relaunch(writer, REPLAY)
    keptAndContinues(prodNode, store, before, what)
    Print(f"{what}: nodeos exited {code}; committed history kept and replay continued it")

    what = "fresh core store and ABI cache from a snapshot"
    snapshot, snapHead = takeSnapshot(prodNode)
    Print(f"{what}: snapshot at block {snapHead}")
    shutil.rmtree(snapStore, ignore_errors=True)
    freshFromSnapshot(snapWriter, snapshot)
    waitCommitted(snapStore, prodNode.getIrreversibleBlockNum() + 3, what)
    m, blocks, _, abiIndex = committedView(snapStore)
    checkContiguous(m, blocks, what)
    assert min(blocks) == snapHead + 1, f"{what}: history starts at {min(blocks)}, expected {snapHead + 1}"
    assert readManifest(snapStore)["abiFloor"] == snapHead, f"{what}: ABI floor is not the snapshot block"
    entries = [struct.unpack_from("<IQQ", abiIndex, i * 20) for i in range(len(abiIndex) // 20)]
    seeded = [e for e in entries if e[0] == snapHead]
    assert seeded, f"{what}: no ABI was seeded at block {snapHead}"
    assert f"history starts at block {snapHead + 1}" in latestStderr(snapWriter), f"{what}: no seed log line"
    sliceStart = (snapHead // 10000) * 10000 + 1
    assert any(d.startswith(f"history_{sliceStart:010d}-") for d in os.listdir(snapStore)), \
        f"{what}: no partial first slice starting at {sliceStart}"
    Print(f"{what}: seeded {len(seeded)} ABIs at block {snapHead}, history from block {snapHead + 1}")

    what = "snapshot too new to execute the first missing block"
    waitCommitted(snapStore, snapHead + 20, what)
    stop(snapWriter)
    stopped = readManifest(snapStore)["head"]
    while prodNode.getIrreversibleBlockNum() < stopped + 10:
        time.sleep(0.5)
    newSnapshot, newHead = takeSnapshot(prodNode)
    assert newHead > stopped + 1, f"{what}: the new snapshot at {newHead} is not past block {stopped + 1}"
    snapWriter.removeDataDir()
    refuses(snapWriter, snapStore, {}, [("--snapshot", newSnapshot)],
            f"cannot name block {stopped + 1}; restore from a snapshot at or below block {stopped}", what, True)

    what = "compatible history through N with a snapshot at or below N"
    before = committedView(snapStore)
    assert snapHead <= before[0]["head"]
    freshFromSnapshot(snapWriter, snapshot)
    keptAndContinues(prodNode, snapStore, before, what)
    Print(f"{what}: verified blocks {snapHead + 1}-{before[0]['head']} and appended from {before[0]['head'] + 1}")

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)

exit(0 if testSuccessful else 1)
