#!/usr/bin/env python3

import os
import signal
import struct
import subprocess
import sys
import time

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from roborovski_writer_store import (HeadWatcher, checkContiguous, committedView, latestStderr, launchExpectingExit,
                                     markDead, readManifest, servedHead)

# Low space, I/O errors and power loss on loop-backed device-mapper volumes; needs root and dm-flakey.

Print = Utils.Print

args = TestHelper.parse_args({"--dump-error-details", "--keep-logs", "-v", "--leave-running", "--unshared"})
Utils.Debug = args.v
if os.geteuid() != 0:
    Print("roborovski_writer_disk_test needs root for loop devices and device-mapper")
    sys.exit(1)

cluster = Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
walletMgr = WalletMgr(True, port=TestHelper.DEFAULT_WALLET_PORT)
testSuccessful = False

prodNodeId = 0
writerId = 1
FAULT_ENV = "ROBOROVSKI_WRITER_FAULT"
REPLAY = "--replay-blockchain"
CONTROLLER_FATAL = "Shutting down due to controller_emit_signal_exception"
HEADROOM_MB = 64
DISK_MB = 512
MIB = 1 << 20


def run(*cmd, ok=(0,)):
    proc = subprocess.run(list(cmd), capture_output=True, text=True)
    assert proc.returncode in ok, f"{' '.join(cmd)} exited {proc.returncode}: {proc.stderr.strip()}"
    return proc.stdout.strip()


class Disk:
    def __init__(self, name, root):
        self.name = name
        self.mount = os.path.join(root, name)
        self.image = os.path.join(root, f"{name}.img")
        self.dm = f"rwq-{name}-{os.getpid()}"
        self.dev = f"/dev/mapper/{self.dm}"
        os.makedirs(self.mount, exist_ok=True)
        with open(self.image, "wb") as f:
            f.truncate(DISK_MB * MIB)
        self.loop = run("losetup", "-f", "--show", self.image)
        self.sectors = run("blockdev", "--getsz", self.loop)
        run("dmsetup", "create", self.dm, "--table", self.table("linear"))
        run("mkfs.ext4", "-q", "-m", "0", self.dev)
        run("mount", self.dev, self.mount)

    def table(self, mode):
        if mode == "linear":
            return f"0 {self.sectors} linear {self.loop} 0"
        return f"0 {self.sectors} flakey {self.loop} 0 0 3600 1 {mode}"

    def suspend(self):
        run("dmsetup", "suspend", "--nolockfs", "--noflush", self.dm)

    def load(self, mode):
        run("dmsetup", "load", self.dm, "--table", self.table(mode))

    def resume(self):
        run("dmsetup", "resume", self.dm)

    def switch(self, mode):
        self.suspend()
        self.load(mode)
        self.resume()

    def unmount(self):
        run("umount", self.mount)

    def remount(self):
        self.switch("linear")
        run("e2fsck", "-fy", self.dev, ok=(0, 1))
        run("mount", self.dev, self.mount)

    def free(self):
        st = os.statvfs(self.mount)
        return st.f_bavail * st.f_frsize

    def fill(self, leave):
        ballast = os.path.join(self.mount, "ballast")
        current = os.path.getsize(ballast) if os.path.exists(ballast) else 0
        size = current + self.free() - leave
        assert size > 0, f"{self.name}: cannot leave {leave} bytes free"
        run("fallocate", "-l", str(size), ballast)
        os.sync()

    def clear(self):
        ballast = os.path.join(self.mount, "ballast")
        if os.path.exists(ballast):
            os.remove(ballast)
        os.sync()

    def destroy(self):
        subprocess.run(["dmsetup", "resume", self.dm], capture_output=True)
        subprocess.run(["umount", self.mount], capture_output=True)
        subprocess.run(["dmsetup", "remove", self.dm], capture_output=True)
        subprocess.run(["losetup", "-d", self.loop], capture_output=True)
        if os.path.exists(self.image):
            os.remove(self.image)


def waitCommitted(target, what, timeout=180):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if readManifest(core.mount)["head"] >= target:
                return
        except (OSError, AssertionError, struct.error):
            pass
        time.sleep(0.5)
    raise AssertionError(f"{what}: the writer did not commit block {target} within {timeout} s")


def waitExit(node, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if node.popenProc.poll() is not None:
            markDead(node)
            return node.popenProc.returncode
        time.sleep(0.2)
    raise AssertionError(f"{what}: nodeos kept running")


def relaunch(node, chainArg=None):
    ok = node.relaunch(chainArg=chainArg, timeout=120)
    while REPLAY in node.cmd:
        node.cmd.remove(REPLAY)
    return ok


def launchPaused(node, fault):
    """Starts a stopped node with a pause fault, without the HTTP pulse check a paused nodeos cannot answer."""
    os.environ[FAULT_ENV] = fault
    try:
        node.popenProc = node.launchCmd(node.cmd, node.data_dir, launch_time=time.strftime("%Y_%m_%d_%H_%M_%S"))
    finally:
        del os.environ[FAULT_ENV]
    node.pid = node.popenProc.pid
    node.killed = False


def cleanStop(node):
    node.kill(signal.SIGTERM)
    markDead(node)
    return committedView(core.mount, abiDisk.mount)


def recovered(prodNode, before, lastSeen, what):
    """Restarts the writer after a fault and checks group 2's pass criteria against the view before it."""
    onDisk = readManifest(core.mount)
    problem = None
    if (onDisk["epoch"], onDisk["head"]) < lastSeen:
        problem = f"{what}: a reader saw block {lastSeen[1]} published, but only block {onDisk['head']} survived on disk"
    watcher = HeadWatcher(core.mount, servedHead(core.mount))
    watcher.start()
    assert relaunch(writer, REPLAY), f"{what}: the writer did not restart"
    try:
        waitCommitted(prodNode.getIrreversibleBlockNum() + 3, what)
    finally:
        watcher.stop.set()
        watcher.join()
    assert watcher.regressed is None, f"{what}: a reader saw the head move from {watcher.regressed[0]} to {watcher.regressed[1]}"
    m, blocks, _, abiIndex = committedView(core.mount, abiDisk.mount)
    checkContiguous(m, blocks, what)
    for num, digest in before[1].items():
        if num <= onDisk["head"]:
            assert blocks.get(num) == digest, f"{what}: committed block {num} changed"
    assert abiIndex.startswith(before[3]), f"{what}: committed ABI index entries changed"
    Print(f"{what}: recovered from block {onDisk['head']} to {m['head']} with no operator action")
    assert problem is None, problem


def watchedFault(what, start):
    """Runs start() with a reader polling control/published and returns the highest (epoch, head) it saw."""
    watcher = HeadWatcher(core.mount, servedHead(core.mount))
    watcher.start()
    try:
        start()
    finally:
        watcher.stop.set()
        watcher.join()
    assert watcher.regressed is None, f"{what}: a reader saw the head move back before the fault"
    return watcher.last


def lowSpace(prodNode, disk, phase):
    what = f"low space on {disk.name} {phase}"
    if phase == "before startup":
        before = cleanStop(writer)
        disk.fill(HEADROOM_MB * MIB - MIB)
        lastSeen = (before[0]["epoch"], before[0]["head"])
        code = launchExpectingExit(writer, {}, 60)
        assert code not in (None, 0), f"{what}: nodeos started below the reserve (exit {code})"
    elif phase == "live":
        before = cleanStop(writer)
        assert relaunch(writer), f"{what}: the writer did not restart"

        def go():
            disk.fill(HEADROOM_MB * MIB - MIB)
            waitExit(writer, 120, what)
        lastSeen = watchedFault(what, go)
        assert CONTROLLER_FATAL in latestStderr(writer), f"{what}: nodeos did not stop through the controller-fatal path"
    else:
        cleanStop(writer)
        for d in disks:
            for name in os.listdir(d.mount):
                if name != "lost+found":
                    run("rm", "-rf", os.path.join(d.mount, name))
        disk.fill(HEADROOM_MB * MIB + (4096 if disk is abiDisk else 64 * 1024))
        before = (None, {}, None, b"")
        lastSeen = (0, 0)
        code = launchExpectingExit(writer, {REPLAY: None}, 180)
        while REPLAY in writer.cmd:
            writer.cmd.remove(REPLAY)
        assert code is not None, f"{what}: nodeos kept running"
        assert "Roborovski writer started live" not in latestStderr(writer), f"{what}: the replay finished first"
    assert "MiB reserve" in latestStderr(writer), f"{what}: the failure does not name the reserve"
    Print(f"{what}: nodeos stopped on the reserve")
    disk.clear()
    if os.path.exists(os.path.join(core.mount, "control", "manifest")):
        recovered(prodNode, before, lastSeen, what)
    else:
        assert relaunch(writer, REPLAY), f"{what}: the writer did not restart"
        waitCommitted(prodNode.getIrreversibleBlockNum() + 3, what)
        m, blocks, _, _ = committedView(core.mount, abiDisk.mount)
        checkContiguous(m, blocks, what)
        Print(f"{what}: a fresh store reached block {m['head']} once space returned")


def ioError(prodNode, disk):
    what = f"I/O errors on {disk.name}"
    before = cleanStop(writer)
    assert relaunch(writer), f"{what}: the writer did not restart"
    waitCommitted(before[0]["head"] + 5, what)

    def go():
        disk.switch("error_writes")
        waitExit(writer, 120, what)
    lastSeen = watchedFault(what, go)
    log = latestStderr(writer)
    assert CONTROLLER_FATAL in log or "Roborovski writer failed" in log, f"{what}: the writer did not stop on the error"
    Print(f"{what}: nodeos stopped on the failed write")
    disk.unmount()
    disk.remount()
    recovered(prodNode, before, lastSeen, what)


def powerLoss(prodNode, point):
    what = f"power loss after {point}"
    before = cleanStop(writer)
    fromBlock = prodNode.getIrreversibleBlockNum() + 8

    def go():
        launchPaused(writer, f"{point}:pause:{fromBlock}")
        deadline = time.time() + 180
        while f"ROBOROVSKI_WRITER_FAULT reached {point}" not in latestStderr(writer):
            assert time.time() < deadline, f"{what}: the writer never reached {point}"
            time.sleep(0.2)
        for d in disks:
            d.suspend()
        for d in disks:
            d.load("drop_writes")
        for d in disks:
            d.resume()
        writer.kill(signal.SIGKILL)
        markDead(writer)
    lastSeen = watchedFault(what, go)
    for d in disks:
        d.unmount()
    for d in disks:
        d.remount()
    survived = readManifest(core.mount)["head"]
    Print(f"{what}: block {survived} survived the cut; a reader had seen block {lastSeen[1]}")
    recovered(prodNode, before, lastSeen, what)


disks = []
try:
    TestHelper.printSystemInfo("BEGIN")
    cluster.setWalletMgr(walletMgr)
    root = os.path.join(Utils.TestLogRoot, f"roborovski_writer_disks{os.getpid()}")
    os.makedirs(root, exist_ok=True)
    core, abiDisk, bufDisk = (Disk(n, root) for n in ("core", "abis", "buffer"))
    disks = [core, abiDisk, bufDisk]

    writerArgs = (f"--plugin eosio::roborovski_writer_plugin --roborovski-writer-headroom-mb {HEADROOM_MB} "
                  f"--roborovski-writer-dir {core.mount} --roborovski-writer-abi-dir {abiDisk.mount} "
                  f"--roborovski-writer-buffer-dir {bufDisk.mount}")
    specificExtraNodeosArgs = {
        prodNodeId: "--plugin eosio::producer_api_plugin --production-pause-vote-timeout-ms 0",
        writerId: writerArgs,
    }
    if cluster.launch(topo="mesh", pnodes=1, totalNodes=2, activateIF=True, biosFinalizer=False,
                      specificExtraNodeosArgs=specificExtraNodeosArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")
    cluster.waitOnClusterSync(blockAdvancing=5)
    cluster.biosNode.kill(signal.SIGTERM)
    prodNode = cluster.getNode(prodNodeId)
    writer = cluster.getNode(writerId)
    waitCommitted(20, "first commits")

    failures = []

    def case(fn, *a):
        try:
            fn(*a)
        except AssertionError as e:
            failures.append(str(e))
            Print(f"FAILED {e}")
            for d in disks:
                d.clear()
            if writer.popenProc is not None and writer.popenProc.poll() is None:
                writer.kill(signal.SIGKILL)
            markDead(writer)
            assert relaunch(writer, REPLAY), "the writer did not restart after a failed case"

    for point in ("buffer_synced", "core_synced", "manifest_renamed", "manifest_dir_synced", "published_renamed"):
        case(powerLoss, prodNode, point)
    for disk in disks:
        case(ioError, prodNode, disk)
    for disk in disks:
        for phase in ("live", "before startup", "during replay"):
            case(lowSpace, prodNode, disk, phase)

    for f in failures:
        Print(f"FAILED {f}")
    testSuccessful = not failures
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=args.dump_error_details)
    for d in disks:
        d.destroy()

exit(0 if testSuccessful else 1)
