#!/usr/bin/env python3

import os
import re
import signal
import time
import urllib.request

from TestHarness import Cluster, TestHelper, Utils, WalletMgr, CORE_SYMBOL, createAccountKeys
from roborovski_writer_store import (checkContiguous, committedBlockIDs, committedView, latestStderr,
                                     launchExpectingExit, markDead, readManifest)

# Splits a 21-producer Savanna network at its bridge node, then checks what each writer commits once one branch wins.

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})
Utils.Debug = args.v
cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, port=TestHelper.DEFAULT_WALLET_PORT)
testSuccessful = False

PARTITION_SECONDS = 60
CONTROLLER_FATAL = "Shutting down due to controller_emit_signal_exception"


def store(node):
    return os.path.join(Utils.getNodeDataDir(node.nodeId), "roborovski")


def metric(node, name):
    with urllib.request.urlopen(f"http://{node.host}:{node.port}/v1/roborovski_writer/metrics", timeout=5) as r:
        text = r.read().decode()
    found = re.search(rf"^roborovski_writer_{name} (\S+)$", text, re.M)
    assert found, f"node {node.nodeId} has no {name} metric"
    return float(found.group(1))


def waitFor(cond, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if cond():
            return
        time.sleep(1)
    raise AssertionError(f"timed out waiting for {what}")


try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)

    writerArgs = "--plugin eosio::roborovski_writer_plugin --roborovski-writer-headroom-mb 256"
    if cluster.launch(topo="bridge", pnodes=2, totalNodes=3, totalProducers=21, activateIF=True, biosFinalizer=False,
                      specificExtraNodeosArgs={0: writerArgs, 1: writerArgs, 2: writerArgs},
                      extraNodeosArgs=" --production-pause-vote-timeout-ms 0 ") is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")
    cluster.validateAccounts(None)

    accounts = createAccountKeys(5)
    for i, account in enumerate(accounts):
        account.name = "tester" + str(i + 1) * 6
    wallet = walletMgr.create("test", [cluster.eosioAccount] + accounts)
    for _, account in cluster.defProducerAccounts.items():
        walletMgr.importKey(account, wallet, ignoreDupKeyWarning=True)

    prodNodes, producers, bridge = [], [], None
    for i in range(3):
        node = cluster.getNode(i)
        node.producers = Cluster.parseProducers(i)
        if not node.producers:
            bridge = node
            continue
        for prod in node.producers:
            node.regproducer(cluster.defProducerAccounts[prod], "http::/mysite.com", 0, waitForTransBlock=False,
                             exitOnError=True)
        prodNodes.append(node)
        producers.extend(node.producers)
    assert bridge is not None and len(prodNodes) == 2

    for account in accounts:
        bridge.createInitializeAccount(account, cluster.eosioAccount, stakedDeposit=0, waitForTransBlock=True,
                                       stakeNet=1000, stakeCPU=1000, buyRAM=1000, exitOnError=True)
        bridge.transferFunds(cluster.eosioAccount, account, f"100000000.0000 {CORE_SYMBOL}", "test transfer",
                             waitForTransBlock=True)
        bridge.delegatebw(account, 20000000.0000, 20000000.0000, waitForTransBlock=False, exitOnError=True)
    cluster.waitOnClusterSync(blockAdvancing=5)
    for i, account in enumerate(accounts):
        trans = prodNodes[i % 2].vote(account, producers, waitForTransBlock=False)
    cluster.waitOnClusterSync(blockAdvancing=5)
    bridge.getNextCleanProductionCycle(trans)
    cluster.biosNode.kill(signal.SIGTERM)

    writers = prodNodes + [bridge]
    for w in writers:
        waitFor(lambda: os.path.exists(os.path.join(store(w), "control", "manifest")), 60, f"node {w.nodeId} first commit")

    libBefore = min(n.getIrreversibleBlockNum() for n in prodNodes)
    Print(f"Partition the producers at LIB {libBefore} by stopping the bridge for {PARTITION_SECONDS} s")
    bridge.kill(signal.SIGTERM)
    markDead(bridge)
    time.sleep(PARTITION_SECONDS)

    heads = [n.getHeadBlockNum() for n in prodNodes]
    libs = [n.getIrreversibleBlockNum() for n in prodNodes]
    pending = [metric(n, "pending_blocks") for n in prodNodes]
    Print(f"During the partition: heads {heads}, LIBs {libs}, writer pending blocks {pending}")
    assert all(lib < libBefore + 10 for lib in libs), f"finality did not stall: LIBs {libs} from {libBefore}"
    above = [h - lib for h, lib in zip(heads, libs)]
    assert all(p > 0 and abs(p - a) <= 2 for p, a in zip(pending, above)), \
        f"the writers' buffers {pending} do not hold the {above} blocks above LIB during the stall"
    assert max(pending) >= 20, f"the writers' buffers did not grow during the stall: {pending}"
    branches = [{b: n.getBlock(b)["id"] for b in range(libBefore + 1, h + 1)} for n, h in zip(prodNodes, heads)]
    committedDuring = [readManifest(store(n))["head"] for n in prodNodes]
    assert all(c <= lib for c, lib in zip(committedDuring, libs)), f"a writer committed past its LIB: {committedDuring}"

    Print("Reconnect the producers through the bridge")
    assert bridge.relaunch(timeout=120), "the bridge did not restart"
    partitionTop = max(heads)
    waitFor(lambda: min(n.getIrreversibleBlockNum() for n in prodNodes) > partitionTop + 5, 300,
            f"finality to pass block {partitionTop}")
    waitFor(lambda: all(metric(n, "pending_blocks") < 10 for n in prodNodes), 120, "the buffers to drain")

    final = prodNodes[0]
    top = min(final.getIrreversibleBlockNum(), *(readManifest(store(w))["head"] for w in writers))
    nums = list(range(libBefore - 5, top + 1))
    chain = {n: final.getBlock(n)["id"] for n in nums}
    losing = [(n.nodeId, sorted(b for b, i in br.items() if chain.get(b) not in (None, i)))
              for n, br in zip(prodNodes, branches)]
    losing = [(nodeId, lost) for nodeId, lost in losing if lost]
    assert losing, "neither side produced a block the winning branch replaced, so no writer saw competing branches"
    for nodeId, lost in losing:
        Print(f"Node {nodeId} buffered its own blocks {lost[0]}-{lost[-1]} before the other branch replaced them")
    for w in writers:
        m, blocks, _, _ = committedView(store(w))
        checkContiguous(m, blocks, f"node {w.nodeId}")
        ids = committedBlockIDs(store(w), nums)
        wrong = [n for n in nums if ids.get(n) != chain[n]]
        assert not wrong, f"node {w.nodeId} committed blocks {wrong[:5]} that are not on the winning branch"
    Print(f"Every writer committed only winning-branch blocks {nums[0]}-{nums[-1]}")

    what = "the buffer bound"
    bridge.kill(signal.SIGTERM)
    markDead(bridge)
    before = committedView(store(bridge))
    bufDir = os.path.join(store(bridge), "buffer")
    bufBefore = {f: os.path.getsize(os.path.join(bufDir, f)) for f in os.listdir(bufDir)}
    code = launchExpectingExit(bridge, {"--roborovski-writer-buffer-limit-mb": "0"}, 120)
    i = bridge.cmd.index("--roborovski-writer-buffer-limit-mb")
    del bridge.cmd[i:i + 2]
    log = latestStderr(bridge)
    assert code is not None, f"{what}: the writer kept running past its buffer bound"
    assert CONTROLLER_FATAL in log and "MiB bound" in log, f"{what}: the writer did not stop on the bound"
    after = committedView(store(bridge))
    assert after[1] == before[1] and after[3] == before[3], f"{what}: committed history changed"
    for f, size in bufBefore.items():
        path = os.path.join(bufDir, f)
        assert os.path.exists(path) and os.path.getsize(path) >= size, f"{what}: buffer file {f} lost data"
    assert bridge.relaunch(timeout=120), "the bridge did not restart after the bound"
    waitFor(lambda: readManifest(store(bridge))["head"] > before[0]["head"] + 5, 120, f"{what} recovery")
    m, blocks, _, _ = committedView(store(bridge))
    checkContiguous(m, blocks, f"{what} recovery")
    Print(f"{what}: the writer stopped with its buffer intact and continued after a restart")

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)

exit(0 if testSuccessful else 1)
