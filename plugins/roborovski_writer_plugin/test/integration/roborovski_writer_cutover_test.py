#!/usr/bin/env python3

import glob
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.request

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from roborovski_writer_store import checkContiguous, committedView, latestStderr, launchExpectingExit, markDead, readManifest

# Hands one store from trace ingestion to the writer plugin and back, with failures after a passing preflight.

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})
Utils.Debug = args.v

TRACE_NODEOS_ENV = "ROBOROVSKI_TRACE_NODEOS"
COREINDEX_ENV = "ROBOROVSKI_COREINDEX"
ACTIONINDEX_ENV = "ROBOROVSKI_ACTIONINDEX"
FAULT_ENV = "ROBOROVSKI_WRITER_FAULT"
STRIDE = "10000"
SKIPPED = 77

traceNodeos = os.environ.get(TRACE_NODEOS_ENV)
coreindex = os.environ.get(COREINDEX_ENV)
actionindex = os.environ.get(ACTIONINDEX_ENV)
if not (traceNodeos and coreindex and actionindex):
    Print(f"SKIPPED: set {TRACE_NODEOS_ENV} to a nodeos built with the Roborovski trace patch, {COREINDEX_ENV} and {ACTIONINDEX_ENV}")
    sys.exit(SKIPPED)

cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, port=TestHelper.DEFAULT_WALLET_PORT)
testSuccessful = False
services = []

prodNodeId = 0
historyId = 1


def start(name, cmd, env=None):
    log = open(os.path.join(Utils.getNodeDataDir(historyId), f"{name}.{len(services)}.log"), "w")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
    services.append(proc)
    return proc, log.name


def readLog(path):
    with open(path) as f:
        return f.read()


def gauge(port, name):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=5) as r:
        found = re.search(rf"^{name} (\S+)$", r.read().decode(), re.M)
    return float(found.group(1)) if found else None


def waitFor(cond, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if cond():
                return
        except (OSError, AssertionError, ValueError, TypeError):
            pass
        time.sleep(1)
    raise AssertionError(f"timed out waiting for {what}")


def stopProc(proc, sig=signal.SIGTERM):
    if proc.poll() is None:
        proc.send_signal(sig)
    return proc.wait(timeout=120)


def coreindexArgs(traceDir):
    return [coreindex, "--index-storage", core, "--abi-source", abis, "--trace-dir", traceDir, "--stride", STRIDE,
            "--chain-id", chainID, "--zstd", "--http-listen", "127.0.0.1:9400", "--http-socket", "none",
            "--stream-enabled=false", "--metrics-listen", "127.0.0.1:9402"]


def preflight(target, extra):
    cmd = [coreindex, "--preflight", "--preflight-target", target, "--index-storage", core, "--abi-source", abis,
           "--chain-id", chainID, "--stride", STRIDE] + extra
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return proc.returncode, proc.stdout + proc.stderr


def snapshotInfo(snapshot):
    out = subprocess.run([Utils.SpringClientPath, "snapshot", "info", snapshot], capture_output=True, text=True,
                         check=True).stdout
    path = snapshot + ".info.json"
    with open(path, "w") as f:
        f.write(out)
    return path


def takeSnapshot(node):
    ret = node.createSnapshot()
    assert ret is not None, "snapshot creation failed"
    return ret["payload"]["snapshot_name"], ret["payload"]["head_block_num"]


def history():
    m, blocks, _, abiIndex = committedView(core)
    checkContiguous(m, blocks, "store")
    return m, blocks, abiIndex


def keptSince(before, what):
    m, blocks, abiIndex = history()
    for num, digest in before[1].items():
        assert blocks.get(num) == digest, f"{what}: committed block {num} changed"
    assert abiIndex.startswith(before[2]), f"{what}: committed ABI index entries changed"
    return m


def flagValue(cmd, flag, value):
    out = [a for a in cmd]
    if flag in out:
        out[out.index(flag) + 1] = value
    else:
        out += [flag, value]
    return out


def dropPlugin(cmd, name):
    out = [a for a in cmd]
    while name in out:
        i = out.index(name)
        del out[i - 1:i + 1]
    return out


def dropFlag(cmd, flag, hasValue=True):
    out = [a for a in cmd]
    while flag in out:
        i = out.index(flag)
        del out[i:i + (2 if hasValue else 1)]
    return out


try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)
    specificExtraNodeosArgs = {
        prodNodeId: "--plugin eosio::producer_api_plugin --production-pause-vote-timeout-ms 0",
        historyId: "--plugin eosio::producer_api_plugin",
    }
    if cluster.launch(topo="mesh", pnodes=1, totalNodes=2, activateIF=True, biosFinalizer=False,
                      specificExtraNodeosArgs=specificExtraNodeosArgs,
                      specificNodeosInstances={historyId: traceNodeos}) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")
    cluster.waitOnClusterSync(blockAdvancing=5)
    cluster.biosNode.kill(signal.SIGTERM)

    prodNode = cluster.getNode(prodNodeId)
    node = cluster.getNode(historyId)
    dataDir = Utils.getNodeDataDir(historyId)
    core = os.path.join(dataDir, "core")
    abis = os.path.join(core, "abis")
    chainID = prodNode.getInfo()["chain_id"]
    releaseNodeos = Utils.EosServerPath

    Print("Trace ingestion: coreindex writes the store from the trace files")
    os.makedirs(core)
    writer, writerLog = start("coreindex-trace", coreindexArgs(os.path.join(dataDir, "traces")))
    waitFor(lambda: readManifest(core)["head"] >= 40, 180, "trace ingestion to commit block 40")
    consumer, _ = start("actionindex", [actionindex, "--history-source", core, "--abi-source", abis,
                                        "--index-storage", os.path.join(dataDir, "actionindex.db"),
                                        "--http-listen", "none", "--http-socket", "none",
                                        "--metrics-listen", "127.0.0.1:9412", "--sync-memory-gb", "1",
                                        "--pebble-cache-size-mb", "64", "--block-cache-size-mb", "64"])
    waitFor(lambda: gauge(9412, "actionindex_indexed_head_block") >= 30, 180, "actionindex to index block 30")

    what = "handoff to direct ingestion"
    snapshot, snapHead = takeSnapshot(node)
    waitFor(lambda: readManifest(core)["head"] >= snapHead + 20, 180, f"trace ingestion to pass block {snapHead + 20}")
    info = snapshotInfo(snapshot)
    code, report = preflight("direct", ["--snapshot-info", info])
    assert code == 0, f"{what}: preflight exit {code}:\n{report}"
    Print(f"{what}: preflight passed for the snapshot at block {snapHead}")

    Print(f"{what}: the trace writer dies at its core sync while stopping (step 3)")
    stopProc(writer)
    os.environ[FAULT_ENV] = "core_synced:kill"
    writer, writerLog = start("coreindex-trace", coreindexArgs(os.path.join(dataDir, "traces")), dict(os.environ))
    del os.environ[FAULT_ENV]
    assert writer.wait(timeout=180) == -signal.SIGKILL, f"{what}: coreindex did not stop at its fault point"
    before = history()
    handoffHead = before[0]["head"]

    Print(f"{what}: a newer snapshot replaces the checked one after the preflight")
    while node.getIrreversibleBlockNum() < handoffHead + 20:
        time.sleep(0.5)
    newSnapshot, newHead = takeSnapshot(node)
    node.kill(signal.SIGTERM)
    markDead(node)
    node.removeDataDir()
    direct = {"--roborovski-writer-dir": core, "--roborovski-writer-headroom-mb": "256"}
    node.cmd = node.cmd + ["--plugin", "eosio::roborovski_writer_plugin"]
    node.cmd[0] = releaseNodeos
    node.cmd = dropPlugin(dropFlag(node.cmd, "--trace-no-abis", False), "eosio::trace_api_plugin")
    code = launchExpectingExit(node, dict(direct, **{"--snapshot": newSnapshot}), 180)
    node.cmd = dropFlag(node.cmd, "--snapshot")
    assert code not in (None, 0), f"{what}: the writer started from a snapshot past block {handoffHead + 1}"
    assert f"cannot name block {handoffHead + 1}" in latestStderr(node), f"{what}: the refusal does not name the range"
    keptSince(before, f"{what} refusal")
    Print(f"{what}: refused the snapshot at block {newHead} naming block {handoffHead + 1}; history intact")

    Print(f"{what}: the writer dies at its first manifest rename (step 4), then starts again")
    node.removeDataDir()
    os.environ[FAULT_ENV] = f"manifest_renamed:kill:{handoffHead + 1}"
    try:
        code = launchExpectingExit(node, {"--snapshot": snapshot}, 300)
    finally:
        del os.environ[FAULT_ENV]
    assert code == -signal.SIGKILL, f"{what}: the writer exit {code}, expected SIGKILL"
    node.cmd = dropFlag(node.cmd, "--snapshot")
    keptSince(before, f"{what} step 4 kill")
    node.removeDataDir()
    assert node.relaunch(chainArg=f"--snapshot {snapshot}", timeout=180), f"{what}: the writer did not start"
    node.cmd = dropFlag(node.cmd, "--snapshot")
    follower, _ = start("coreindex-follower", [coreindex, "--read-only", "--index-storage", core, "--abi-source", abis,
                                               "--chain-id", chainID, "--trace-dir", os.path.join(dataDir, "traces"),
                                               "--http-listen", "127.0.0.1:9400",
                                               "--http-socket", "none", "--stream-enabled=false",
                                               "--metrics-listen", "127.0.0.1:9402"])
    target = handoffHead + 30
    waitFor(lambda: readManifest(core)["head"] >= target, 300, f"direct ingestion to commit block {target}")
    waitFor(lambda: gauge(9402, "coreindex_served_head_block") >= target, 60, "the follower to serve the new head")
    waitFor(lambda: gauge(9412, "actionindex_indexed_head_block") >= target, 120, "actionindex to continue")
    m = keptSince(before, what)
    assert m["epoch"] == before[0]["epoch"], f"{what}: the epoch changed"
    Print(f"{what}: blocks through {handoffHead} kept; the plugin appended from {handoffHead + 1}, "
          f"the follower serves {target} and actionindex continued without a reindex")

    what = "handoff back to trace ingestion"
    backSnapshot, backHead = takeSnapshot(node)
    waitFor(lambda: readManifest(core)["head"] >= backHead + 20, 180, f"direct ingestion to pass block {backHead + 20}")
    node.kill(signal.SIGTERM)
    markDead(node)
    before = history()
    handoffHead = before[0]["head"]
    stopProc(follower)

    Print(f"{what}: the trace node replays from block {backHead} to write the trace files the store resumes from")
    traceDir = os.path.join(dataDir, "traces-back")
    node.removeDataDir()
    node.cmd[0] = traceNodeos
    node.cmd = dropPlugin(node.cmd, "eosio::roborovski_writer_plugin")
    node.cmd = dropFlag(dropFlag(node.cmd, "--roborovski-writer-dir"), "--roborovski-writer-headroom-mb")
    node.cmd += ["--plugin", "eosio::trace_api_plugin", "--trace-no-abis", "--trace-dir", traceDir]
    assert node.relaunch(chainArg=f"--snapshot {backSnapshot}", timeout=180), f"{what}: the trace node did not start"
    node.cmd = dropFlag(node.cmd, "--snapshot")
    waitFor(lambda: node.getIrreversibleBlockNum() > handoffHead + 10, 180, "the trace node to pass the store head")

    code, report = preflight("trace", ["--trace-dir", traceDir])
    assert code == 0, f"{what}: preflight exit {code}:\n{report}"
    Print(f"{what}: preflight passed with traces for block {handoffHead + 1}")

    Print(f"{what}: the trace files for block {handoffHead + 1} disappear after the preflight")
    aside = traceDir + ".aside"
    shutil.copytree(traceDir, aside)
    for f in glob.glob(os.path.join(traceDir, "*")):
        if os.path.isfile(f):
            os.remove(f)
        else:
            shutil.rmtree(f)
    refused, refusedLog = start("coreindex-trace", coreindexArgs(traceDir))
    try:
        code = refused.wait(timeout=60)
    except subprocess.TimeoutExpired:
        stopProc(refused)
        code = None
    log = readLog(refusedLog)
    assert code not in (None, 0), f"{what}: coreindex did not refuse without the trace files (exit {code}):\n{log[-2000:]}"
    assert re.search(r"^.*error.*trace files", log, re.M | re.I), f"{what}: the refusal does not name the trace files:\n{log[-2000:]}"
    keptSince(before, f"{what} refusal")
    Print(f"{what}: coreindex refused without the trace files (exit {code}); history intact")
    shutil.rmtree(traceDir)
    shutil.copytree(aside, traceDir)

    writer, writerLog = start("coreindex-trace", coreindexArgs(traceDir))
    target = handoffHead + 30
    waitFor(lambda: readManifest(core)["head"] >= target, 300, f"trace ingestion to commit block {target}")
    waitFor(lambda: gauge(9412, "actionindex_indexed_head_block") >= target, 120, "actionindex to continue")
    m = keptSince(before, what)
    assert m["epoch"] == before[0]["epoch"], f"{what}: the epoch changed"
    Print(f"{what}: blocks through {handoffHead} kept; trace ingestion appended from {handoffHead + 1} "
          f"and actionindex continued without a reindex")

    testSuccessful = True
finally:
    for proc in services:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)

exit(0 if testSuccessful else 1)
