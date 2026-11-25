# roborovski_writer_plugin

Operators who run Roborovski add `eosio::roborovski_writer_plugin` to a history node so nodeos writes Roborovski core storage and the ABI cache itself. This is direct ingestion. The other method, trace ingestion, has nodeos write trace files and coreindex convert them into the same storage. With the plugin, coreindex runs in follower mode (`--read-only`) and serves what nodeos commits, and actionindex, txindex and other consumers read the storage as before.

The plugin buffers each accepted block on disk, writes it to core storage once nodeos makes it irreversible, and publishes it to readers only after it is durable. A restart, a crash or a power loss never changes history a reader has seen.

The Roborovski operator documentation (`docs/direct-ingestion` in `greymass/roborovski`) covers coreindex follower mode, moving an existing store between the two ingestion methods, repair, and the production rollout.

## Supported platform

The release is qualified with Spring v1.2.2 on Ubuntu 22.04 x86_64, on dedicated history nodes that produce no blocks. Core storage, the ABI cache and the buffer sit on local filesystems of the server that also runs coreindex. Network filesystems are outside the release.

## Build nodeos with the plugin

Build from the `greymass/spring` branch or tag that contains this directory, following Spring's unpinned build in the top-level `README.md`, with two more packages for the plugin's zstd compression:

```bash
sudo apt-get update
sudo apt-get install -y \
        build-essential \
        cmake \
        git \
        libcurl4-openssl-dev \
        libgmp-dev \
        llvm-11-dev \
        python3-numpy \
        file \
        zlib1g-dev \
        libzstd-dev \
        pkg-config

git clone --recursive https://github.com/greymass/spring.git
cd spring
git checkout <release tag>
git submodule update --init --recursive
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 ..
make -j 8 nodeos spring-util
```

Some Spring compilation units need about 4 GB of memory each, so set `-j` to the server's memory in GB divided by four. On a 32 GB build host, 8 jobs complete and 16 run out of memory.

Check that the plugin is in the binary:

```bash
./programs/nodeos/nodeos --help | grep roborovski-writer
```

Keep the output of `./programs/spring-util/spring-util chain-state build-info` with the commit you built. The release-candidate record asks for both.

The plugin's tests build with the rest of Spring and run as `ctest -R roborovski_writer`: the unit tests in `test/`, and the fault and recovery tests in `test/integration/`, which the build copies into its `tests` directory beside Spring's own. `tests/roborovski_writer_gates.sh`, run from the build directory, runs every fault and recovery gate once and keeps the logs.

## Configure nodeos

Add the plugin to the history node's `config.ini`. A complete example is [`examples/config.ini`](examples/config.ini).

```ini
plugin = eosio::chain_plugin
plugin = eosio::http_plugin
plugin = eosio::roborovski_writer_plugin

roborovski-writer-dir = /data/roborovski/core
roborovski-writer-abi-dir = /data/roborovski/core/abis
```

| Option | Default | Meaning |
|--------|---------|---------|
| `roborovski-writer-dir` | `roborovski` | Core storage. A relative path is relative to the nodeos data directory. coreindex's `--index-storage` names the same directory. |
| `roborovski-writer-abi-dir` | `abis` inside core storage | The ABI cache. coreindex's `--abi-source` names the same directory. |
| `roborovski-writer-buffer-dir` | `buffer` inside core storage | Accepted blocks that wait for irreversibility. Readers never open it, and the plugin discards every buffered block at or below the committed block when it starts. |
| `roborovski-writer-headroom-mb` | `4096` | Free space in MiB that must remain on each filesystem the writer uses. nodeos stops below it. |
| `roborovski-writer-buffer-limit-mb` | `16384` | The most buffered output in MiB awaiting irreversibility. nodeos stops above it. |
| `roborovski-writer-log-interval` | `0` (off) | Seconds between ingestion rate lines on the `roborovski_writer_perf` logger, in coreindex's trace sync line format, for example `Block: 280204974 / 280219472 (100.0%) 2999 BPS \| 1.7 MB (608 B/blk) \| 3.0 acts/blk, 2.5 txns/blk \| glob: 367044615 \| writer: 69 of 1000 ms`. During replay the plugin commits every 1,000 blocks, and an interval of 3 seconds or more smooths those batches. |

Leave `read-mode` at its default, `head`. The plugin captures each block as nodeos applies it, holds it through forks in the buffer, and writes it once it is irreversible. Core storage holds irreversible history only.

The plugin turns off nodeos's replay optimizations so that every replayed block produces the traces the plugin records. Replay with the plugin runs slower than a plain nodeos replay for that reason.

Core storage uses 10,000-block slices. On an existing store with any other slice size the plugin refuses to start before it writes anything.

### Storage placement

The headroom check runs at startup, during replay and on every block, separately for each of the three directories, so placing them on different filesystems is supported. When the free space on any of them falls below `roborovski-writer-headroom-mb`, nodeos stops with a message that names the filesystem and its free space. Free space on that filesystem, then start nodeos again; no history is lost.

The buffer grows while nodeos waits for finality. If finality stalls long enough for the buffer to pass `roborovski-writer-buffer-limit-mb`, nodeos stops with a message naming the last committed block. Start it again once the network finalizes blocks.

The resource monitor plugin, when loaded, also watches all three directories.

## Start a new store

A new store needs no preparation beyond an empty or missing core storage directory.

- **From a snapshot.** Start nodeos with `--snapshot`. On the first block, the plugin records every contract ABI in the snapshot into the ABI cache, valid from the snapshot's block, and core storage starts at the next block. Keep the snapshot: it is the only source for repairing history back to that height.
- **From genesis.** Start nodeos with the chain's `genesis.json` and a blocks log or peers. Core storage starts at block 2.

On an existing store the plugin needs no special option: it reads its committed position from the store, discards what was written but not committed, and continues. Moving an existing store from trace ingestion to the plugin is a cutover, described in the Roborovski documentation.

## Run and watch

Startup logs one line with the three directories and the committed block:

```
Roborovski writer core storage: /data/roborovski/core, ABI cache: /data/roborovski/core/abis, buffer: /data/roborovski/core/buffer, committed block: 24850000, ...
Roborovski writer started live at committed block 24850000
```

The plugin adds two HTTP endpoints to nodeos:

- `/v1/roborovski_writer/metrics` returns Prometheus text. It is in the `prometheus` API category, so `http-category-address` can expose it on its own address. nodeos refuses to start with a `prometheus` category address unless `plugin = eosio::prometheus_plugin` is also loaded, as in `examples/config.ini`.
- `/v1/roborovski_writer/recent`, in the `chain_ro` category, returns the latest blocks with the time nodeos made each irreversible and the time the plugin committed it. `coreverify live-probe` reads it to measure availability.

| Metric | Meaning |
|--------|---------|
| `roborovski_writer_notified_block` | Last block nodeos reported irreversible to the plugin. |
| `roborovski_writer_prepared_block` | Last block written to the buffer. |
| `roborovski_writer_committed_block` | Last block readers may see. |
| `roborovski_writer_notify_to_commit_seconds` | Time from the irreversible notification to the commit, for the last commit. |
| `roborovski_writer_fsync_seconds` | Time spent syncing files in the last commit. |
| `roborovski_writer_pending_blocks`, `roborovski_writer_pending_bytes` | Blocks and bytes in the buffer above the committed block. |
| `roborovski_writer_failures_total` | Writer failures since nodeos started. |
| `roborovski_writer_disk_free_bytes{filesystem="core"\|"abi"\|"buffer"}` | Free space on each filesystem. |

Alert when `roborovski_writer_committed_block` stops advancing while nodeos's irreversible block advances, and when `roborovski_writer_pending_blocks` passes a few thousand. The plugin also logs a warning when blocks pile up above the committed block.

## Failures and restarts

When the writer cannot write safely, it stops nodeos instead of skipping a block. The `roborovski_writer` logger records the cause with the words `Roborovski writer failed`, and the history it already committed stays intact.

A failure while nodeos runs ends nodeos through Spring's controller-fatal path, which exits with status 0. Supervise the service on its log or on `roborovski_writer_committed_block`, because a supervisor that restarts only on a non-zero exit leaves it stopped. A failure during startup exits with a non-zero status.

After a crash or a power loss, start nodeos again. The plugin removes anything past the last commit, restores blocks from its buffer, and continues. A nodeos that was killed needs `--replay-blockchain` or a snapshot to start, as without the plugin; the plugin checks each block nodeos replays below its committed head against core storage and skips it.

When the writer finds a gap or a mismatch it cannot resolve, it names the blocks and the next step. Two messages operators may meet:

- `nodeos is irreversible through block R but cannot name block N; restore from a snapshot at or below block H and replay`: nodeos's state is ahead of core storage. Start nodeos from a snapshot at or below the committed block `H`.
- `block N is <id> in nodeos but <id> in committed history; stop and repair core storage`: the chain and the store disagree at a committed height. Follow the repair procedure in the Roborovski documentation.

When nodeos cannot start at all, `coreindex --recover` completes the same recovery from the command line.

## Files the plugin writes

Core storage and the ABI cache have the layout coreindex writes in trace ingestion, so readers need no change of format. The plugin adds two control files that every writer shares:

- `control/manifest`, replaced atomically on each commit, records the committed head and what a writer resumes from.
- `control/published`, a copy of the manifest written after the commit is durable, is the only file readers use to decide what they may serve.

The format is specified in `libraries/coremanifest/FORMAT.md` in `greymass/roborovski`. Each writer holds a lock on the core storage and ABI directories, so a second nodeos, or a coreindex writing trace ingestion, refuses to start on the same store.

The plugin reads the environment variable `ROBOROVSKI_WRITER_FAULT` for the release's fault tests. Leave it unset in operation.
