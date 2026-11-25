#!/usr/bin/env python3

import json
import os
import shutil
import signal
import time

from TestHarness import Cluster, TestHelper, Utils, WalletMgr

# Produces a Savanna chain past the first 10,000-block slice and saves its block log for replay scenarios.

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})

FIXTURE_ENV = "ROBOROVSKI_WRITER_CHAIN_FIXTURE"
TARGET_LIB = 10600

fixtureDir = os.environ.get(FIXTURE_ENV)
assert fixtureDir, f"set {FIXTURE_ENV} to the directory that receives the fixture"
fixtureDir = os.path.abspath(fixtureDir)

Utils.Debug = args.v
cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, port=TestHelper.DEFAULT_WALLET_PORT)
testSuccessful = False

try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)
    if cluster.launch(topo="mesh", pnodes=1, totalNodes=1, activateIF=True, biosFinalizer=False,
                      specificExtraNodeosArgs={0: "--production-pause-vote-timeout-ms 0"}) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")
    cluster.waitOnClusterSync(blockAdvancing=5)
    cluster.biosNode.kill(signal.SIGTERM)

    prod = cluster.getNode(0)
    while True:
        lib = prod.getIrreversibleBlockNum()
        if lib >= TARGET_LIB:
            break
        Print(f"LIB {lib} of {TARGET_LIB}")
        time.sleep(60)

    prod.kill(signal.SIGTERM)
    blocks = os.path.join(Utils.getNodeDataDir(0), "blocks")
    os.makedirs(fixtureDir, exist_ok=True)
    for name in ("blocks.log", "blocks.index"):
        shutil.copy2(os.path.join(blocks, name), os.path.join(fixtureDir, name))
    with open(os.path.join(fixtureDir, "fixture.json"), "w") as f:
        json.dump({"lib": lib}, f)
    Print(f"Saved a block log with LIB {lib} to {fixtureDir}")
    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)

exit(0 if testSuccessful else 1)
