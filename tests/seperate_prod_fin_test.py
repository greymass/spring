#!/usr/bin/env python3

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from TestHarness.TestHelper import AppArgs

###############################################################
# seperate_prod_fin_test
#
# Test producer nodes and finalizer nodes are seperate. Configure 2 producer nodes and
# 3 non-producer nodes; each of them has a finalizer key. Since threshold is 4,
# if LIB advances, it implies at least 2 non-producer finalizer participates in
# the finalization process.
#
###############################################################


Print=Utils.Print
errorExit=Utils.errorExit

appArgs = AppArgs()
args=TestHelper.parse_args({"-d","-s","--keep-logs","--dump-error-details","-v","--leave-running","--unshared"},
                            applicationSpecificArgs=appArgs)
pnodes=2 # producer node
delay=args.d
topo=args.s
debug=args.v
total_nodes=pnodes+3 # 3 non-producer nodes
dumpErrorDetails=args.dump_error_details

Utils.Debug=debug
testSuccessful=False

cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr=WalletMgr(True, keepRunning=args.leave_running, keepLogs=args.keep_logs)

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)

    Print(f'producing nodes: {pnodes}, topology: {topo}, delay between nodes launch: {delay} second{"s" if delay != 1 else ""}')

    numTrxGenerators=2
    Print("Stand up cluster")
    # For now do not load system contract as it does not support setfinalizer
    # seperate_prod_fin_test_shape.json defines 2 producer nodes each has 1
    # producer and 3 non-producer nodes
    if cluster.launch(pnodes=pnodes, totalNodes=total_nodes, totalProducers=pnodes,
                      maximumP2pPerHost=total_nodes+numTrxGenerators, topo="./tests/seperate_prod_fin_test_shape.json", delay=delay,
                      loadSystemContract=False, activateIF=False) is False:
        errorExit("Failed to stand up eos cluster.")

    assert cluster.biosNode.getInfo(exitOnError=True)["head_block_producer"] != "eosio", "launch should have waited for production to change"

    Print("Start transition to Savanna")
    success, transId = cluster.activateInstantFinality(biosFinalizer=False, waitForFinalization=False, finalizerMustBeProducer=False)
    assert success, "Activate instant finality failed"

    cluster.biosNode.waitForHeadToAdvance()

    Print("Wait for LIB of setfinalizers")
    # Wait for (number of producers + 1) rounds to be safe
    if not cluster.biosNode.waitForTransFinalization(transId, timeout=(pnodes+1) * 12 * 3):
        Utils.Print("ERROR: Failed to validate setfinalizer transaction %s got rolled into a LIB block" % (transId))

    assert cluster.biosNode.waitForLibToAdvance(), "Lib should advance after instant finality activated"
    assert cluster.biosNode.waitForProducer("defproducera"), "Did not see defproducera"
    assert cluster.biosNode.waitForHeadToAdvance(blocksToAdvance=13), "Head did not advance 13 blocks to next producer"
    assert cluster.biosNode.waitForLibToAdvance(), "Lib stopped advancing on biosNode"
    assert cluster.getNode(1).waitForLibToAdvance(), "Lib stopped advancing on Node 1"

    info = cluster.biosNode.getInfo(exitOnError=True)
    assert (info["head_block_num"] - info["last_irreversible_block_num"]) < 9, "Instant finality enabled LIB diff should be small"

    # LIB has advanced, which indicate at least 2 of non-producer finalizers have voted.
    # Double check that's indeed the case in qc_extension
    info = cluster.getNode(1).getInfo(exitOnError=True)
    block_num = info["last_irreversible_block_num"]
    block = cluster.getNode(1).getBlock(block_num)
    qc_ext = block["qc_extension"]
    Print(f'{qc_ext}')
    # "11111" is the bits of finalizers have voted (we have total finalizers)
    assert qc_ext["qc"]["data"]["strong_votes"] == "11111", 'Not all finalizers voted' 

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)
