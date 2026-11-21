import glob
import hashlib
import os
import struct
import subprocess
import threading
import time
from datetime import datetime

from TestHarness import Utils

# Reads a Roborovski store's committed view the way a reader would, for the writer gate tests.

MANIFEST = struct.Struct("<8sII16s16sQII32sQIIQII")
INDEX_ENTRY = struct.Struct("<IQIQQ")
RECORD_OVERHEAD = 8


def readManifest(store):
    with open(os.path.join(store, "control", "manifest"), "rb") as f:
        raw = f.read()
    (magic, _, _, coreUUID, abiUUID, epoch, head, abiIndexLen, headID,
     sliceDataEnd, sliceIndexLen, abiFloor, abiDataEnd, _, _) = MANIFEST.unpack(raw)
    assert magic == b"RBMANIF\0", f"bad manifest magic {magic}"
    return {"epoch": epoch, "head": head, "abiIndexLen": abiIndexLen, "headID": headID.hex(), "abiFloor": abiFloor,
            "sliceDataEnd": sliceDataEnd, "sliceIndexLen": sliceIndexLen, "abiDataEnd": abiDataEnd}


def committedView(store, abiRoot=None):
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
            end = offset + RECORD_OVERHEAD + size
            assert end <= len(data), f"block {num} lies past committed data in {sliceDir}"
            assert num not in blocks, f"block {num} stored twice"
            blocks[num] = hashlib.sha256(data[offset:end]).hexdigest()
    abiRoot = abiRoot or os.path.join(store, "abis")
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


def launchExpectingExit(node, swapFlags, timeout):
    """Starts a stopped node with swapped flags and returns its exit code, or None if it is still running."""
    cmd = node.cmd[:]
    for flag in ("--genesis-json", "--genesis-timestamp"):
        if flag in cmd:
            i = cmd.index(flag)
            del cmd[i:i + 2]
    for k, v in swapFlags.items():
        if k in cmd:
            cmd[cmd.index(k) + 1] = v
        else:
            cmd.extend([k] if v is None else [k, v])
    node.popenProc = node.launchCmd(cmd, node.data_dir, launch_time=datetime.now().strftime("%Y_%m_%d_%H_%M_%S"))
    node.pid = node.popenProc.pid
    node.cmd = cmd
    code = waitForExit(node, timeout)
    if code is None:
        node.popenProc.kill()
        node.popenProc.wait()
    markDead(node)
    return code


def committedBlockIDs(store, nums):
    """Block IDs stored in the committed records of the given blocks, read with the zstd command."""
    m = readManifest(store)
    ids = {}
    for sliceDir in sorted(glob.glob(os.path.join(store, "history_*-*"))):
        first, last = (int(x) for x in os.path.basename(sliceDir)[len("history_"):].split("-"))
        wanted = [n for n in nums if first <= n <= min(last, m["head"])]
        if not wanted:
            continue
        with open(os.path.join(sliceDir, "blocks.index"), "rb") as f:
            index = f.read()
        with open(os.path.join(sliceDir, "data.log"), "rb") as f:
            data = f.read()
        entries = {}
        for i in range((len(index) - 4) // INDEX_ENTRY.size):
            num, offset, size, _, _ = INDEX_ENTRY.unpack_from(index, 4 + i * INDEX_ENTRY.size)
            entries[num] = (offset, size)
        for n in wanted:
            offset, size = entries[n]
            payload = data[offset + 4:offset + 4 + size]
            if payload[:4] == b"\x28\xb5\x2f\xfd":
                payload = subprocess.run(["zstd", "-dcq"], input=payload, capture_output=True, check=True).stdout
            ids[n] = payload[8:40].hex()
    return ids
