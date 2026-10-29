# lsm: Linux System Monitor

lsm is a native Linux system monitor written in C, with OpenGL 2D/3D graphs,
per-core CPU and GPU telemetry, process controls, RAM inspection and editable
3D process/filesystem topology.

## Build and run (Linux)

Build with `make lsm` (or `make` for both applications), then run:

```sh
./lsm
./lsm --view pressure  # open the new diagnostic dashboard
./lsm --version        # verify this is lsm 4.1
./lsm --view tree      # live 3D process ancestry
./lsm --view files --root /home/x1/neural-topology
./lsm --view ram       # inspect RAM and process mappings
./lsm --view manual    # open the final Manual tab
./lsm --help           # command-line reference
```

Requires a C11 compiler, Make, OpenGL, FreeGLUT, FreeType development files
and `pkg-config`. Debian/Ubuntu: `build-essential freeglut3-dev libgl1-mesa-dev
libfreetype-dev pkg-config`. Arch Linux: `base-devel freeglut mesa freetype2
pkgconf`. A DejaVu Sans Mono font is used when installed;
otherwise text falls back to FreeGLUT's built-in font. `MONITOR_FONT` can point
to a different monospace TTF file. No root privileges are needed.

### Views

- **Overview:** CPU, RAM, GPU, and network summaries, rotatable 3D CPU history,
  memory history, and disk throughput. Small windows use a compact layout.
- **CPU:** per-thread utilization histories, current clock, maximum clock,
  physical core/socket identity, and core temperatures when available. Select a
  core for user/system/I/O-wait percentages and its frequency range. The header
  includes model, physical and logical core counts, sockets, governor, package
  temperature, architecture, reported cache size, and microcode version.
- **GPU:** device/vendor/PCI IDs, driver, per-engine activity and capacity,
  clocks, temperature, power/limit, dedicated VRAM and GTT counters where
  exposed, visible DRM client count and resident-memory sum. Switch between
  a 2D utilization history and a rotatable 3D engine-history graph. Use the
  wheel to scroll through all telemetry in short or narrow windows.
- **Pressure (5):** CPU, memory and storage stall percentages from Linux PSI,
  10/60/300-second averages, full-stall averages for memory and I/O, and
  per-sample histories. Unavailable kernel interfaces are labeled unavailable.
- **RAM (6):** system anonymous memory, page cache, kernel slab, page tables,
  shared/tmpfs, free/available RAM, swap, and the largest processes by RSS.
  Click a process to open its memory maps. The displayed counters overlap and
  must not be summed as separate parts of physical RAM.
- **Processes:** searchable, sortable PID/user/name/CPU/RAM/thread/state table.
  Select a row to see its command, parent PID, nice value, priority, and disk
  I/O rates. Narrow windows prioritize PID, name, CPU and memory columns.
  Click **Inspect / I** or select a process and press **I** for a dedicated
  inspector with CPU, resident RAM, and disk-throughput histories. History is
  collected from visible snapshots while the inspector is open; exited
  processes leave their recorded history available. Keys 1/2/3 select its graph.
  The Maps and Bytes views add per-region accounting and bounded read-only
  memory inspection, described below.

### History and layout controls

The collector now retains **900 samples**, approximately 15 minutes per running
session. Use the **History** button or V to cycle 1-, 5-, and 15-minute windows.
The same window applies to 2D histories, core sparklines, and CPU/GPU 3D history.
There is no fabricated history or recording across restarts; graphs fill from
live measurements and use actual monotonic sample times. Memory-inspector
requests publish fresh metadata without adding extra timeline points; only
regular metric samples advance the history and selected-process graphs.

Hover over a 2D plot or core sparkline to inspect a measured sample. A shared
cursor marks that timestamp on the other visible histories. Click a graph to
pin/unpin it, or press C to clear it. Tooltip values include units and sample
age; blank time ranges have no invented value. Pinning a cursor does not stop
the collector or rewind the process table, process tree, filesystem, or memory
contents. Space separately freezes the displayed snapshot. A pinned time can
age out of the selected window or retained data. Automatic throughput scales
use the selected time window rather than peaks from older hidden samples.

Drag the vertical dividers in Overview, GPU, Process tree and Filesystem to
resize the graph and detail panels. The two topology views keep independent
sidebar widths. Bounds preserve room for both panels; narrow windows still
use compact layouts. Resizing settings are kept for the current application
session and are not written to a configuration file.

### 3D topology views

**Process tree (7)** draws the collected processes as a 3D parent/child tree.
Links use the observed PPID; their arrowheads point toward children. Vertical
levels represent generations. The synthetic **Visible roots** node groups
processes whose parents are outside the view; it is not a real process. Up to
4,095 process nodes plus that grouping node can be shown. The header gives
shown and collected counts. Each node has a minimum projected size for picking;
CPU activity can increase its size. Cyan indicates your user, purple other
users, red high CPU, and orange the selected node.

Click a node to select its stable PID/start-time identity. The side panel shows
PPID, CPU, RAM, command, thread count, and visible direct children. Click a child
to select it. **Focus / Enter** isolates the selected subtree; **Parent /
Backspace** moves to the selected process's parent and focuses it. **All / A**
restores the full forest. **Mine / U** toggles your user's processes.
**Follow / L** locks onto the selected PID and start time, focuses its subtree,
and refits the camera when its membership or bounds change. Explicit Focus,
Parent, All and Mine navigation end Follow mode. An exited or unavailable
target is reported instead of following a recycled PID. The selected node's
ancestry is highlighted, and its family summary includes that process and
its currently observed descendants. Family CPU sums their CPU percentages;
family RSS is a sum that can count shared pages more than once. Click the
search box or press `/`, enter a name/PID, and press Enter to focus the first
match. I opens the existing process inspector; T/K use the existing per-process
End/Kill dialogs. These edges represent process ancestry, not IPC, sockets,
threads, file handles, or historical process lifetimes. Exited processes leave
the live view; an exited focused root is reported rather than following a
reused PID.

**Filesystem (8)** draws a bounded directory snapshot: cyan cubes for folders,
purple spheres for files, orange for symlinks or the selection, and red for
entries with errors. Clicking an entry shows metadata; Enter enters a selected
directory, and Backspace/Up moves to its parent. Home and `/` open those roots.
Click the path box or press `/` to type a directory, then Enter. The depth button
cycles 1–4 levels; H/the Hidden button toggles hidden entries. Refresh rescans.
The default root is the launch directory; `--root PATH` overrides it.

Filesystem collection uses its own background thread and immutable snapshots.
It reads directory entries, stat data and symlink target text; it does not read
file contents, launch files, modify files, or signal processes. Child symlinks
are displayed without following them. Mount points are shown but not descended
into until explicitly entered. The list is capped at 2,048 entries including
its root; the UI reports truncation. Directory inode sizes are not recursive
sizes, and regular-file logical and allocated sizes can differ for sparse
files or shared filesystem storage. Metadata can change during a scan, and slow
filesystems can delay completion. Snapshots refresh only when requested.

In both views, **left-click a node or its label to select it**, and **left-drag
a node to place it**. Small nodes have an expanded pick area and selection
rings; front visible geometry wins over occluded nodes. A selected node is
captured on press, so release cannot silently select a different node after
a refresh. Small pointer jitter is treated as a click.

**Right-drag orbits**. **Middle-drag, Shift+left-drag, or dragging empty space
pans**. The wheel or +/- zooms. **R / Fit** fits the view without removing node
placements. **Ctrl+Z** undoes node placements. **G / Reset** restores the
automatic layout for the current view/subtree. **Escape** cancels an active
gesture; otherwise it deselects in topology views. F toggles fullscreen.

Node dragging adjusts only the visual placement: process PPID links and files
on disk are not changed. Placements are remembered for the running session,
scoped to the process subtree/filter or filesystem root. A folder refresh
preserves selection and placements by full path rather than row index. Process
placements use PID plus start time, so PID reuse does not inherit an old edit.
Manual camera/node gestures turn off Follow to keep automatic camera fitting
from fighting your actions. Layout state and undo history are bounded.

Background collectors continue during gestures, while the displayed scene is
held steady until release. The camera target stays fixed across ordinary
sampling; it refits on explicit navigation, R/Fit, or Follow changes. Wide
windows show a side panel; smaller windows use a compact selection summary.
Use F if the desktop tiles the window too small to show the scene.

### Inspecting process memory

Open **RAM (6)** and click a process, or select one in Processes and press I,
then click **Maps** (M). The address map shows permissions, RSS, PSS, and mapped
file names. Heap, stack, shared, anonymous and special mappings can be filtered
by cycling the type button; S switches sorting between resident size and
address. Scroll with the wheel. Up to 4,096 mappings are listed; summary
accounting still includes mappings beyond that cap.

RSS counts resident pages, including pages shared with other processes. PSS
accounts for shared pages proportionally. Private/shared figures describe
kernel page accounting. Labels such as `[heap]` and `[stack]` identify only the
regions explicitly named by the kernel; allocator arenas and other stacks can
appear as anonymous mappings. A virtual range can be reserved without all of
its pages being resident. Shared hugetlb pages are not included in the normal
RSS/PSS counters. Live mappings can change while being inspected; this is not
an atomic snapshot of the whole process.

Select a readable region and click **Read bytes**, click **Bytes**, or press B.
This loads at most 256 bytes into a hex/ASCII viewer. Previous/next move within
the selected mapping, Reload explicitly re-reads that page, and the wheel
scrolls the current page. Byte contents do not refresh automatically. Bytes
have no automatic interpretation as C objects, strings, files or images.
Reading a virtual page can cause it to be faulted into RAM.

Both maps and byte access honor Linux's existing permissions. `/proc/PID/mem`
uses process-debugging access checks and can be denied by ptrace/Yama policy
even for another process owned by the same user. The program reports denials
and does not change kernel settings, attach a debugger, elevate privileges,
write memory or export the bytes. The monitor's own process provides a
readable example on systems which restrict other processes. A checked start
time and a bound `/proc/PID` directory prevent PID reuse from retargeting reads.

The collector reads detailed maps only for the active memory inspector. Close
it or switch to CPU/RAM/I/O graphs to stop those detailed scans.

### End and Kill

Select a process, then click **End / SIGTERM** for a graceful exit request or
**Kill / SIGKILL** for immediate termination. A dialog identifies the exact
process and asks for confirmation. SIGTERM can be ignored; SIGKILL can lose
unsaved work. Children are not automatically signaled. Existing Linux user
permissions apply; permission failures and exited processes are reported in
the window. The monitor does not elevate privileges.

Actions use `pidfd_open` and `pidfd_send_signal` with a checked process start
time, keeping the target stable through sorting, refresh, exit and PID reuse.
PID 1 and the monitor itself are protected. On kernels without pidfd support,
termination is unavailable rather than falling back to a numeric-PID race.

### Controls

| Control | Action |
| --- | --- |
| `1`–`9` | Overview, CPU, GPU, Processes, Pressure, RAM, Process tree, Filesystem, Manual |
| `F` | Toggle fullscreen |
| Left-drag node in topology | Reposition its visual layout |
| Right-drag in topology | Orbit the camera |
| Middle-drag / Shift+left-drag / drag empty space | Pan the topology view |
| Ctrl+Z / G in topology | Undo a placement / reset automatic layout |
| Drag a CPU/GPU 3D history graph | Orbit its camera |
| Wheel | Zoom Overview; scroll CPU, GPU, or Processes |
| `+` / `-` | Zoom 3D graphs |
| `R` | Reset camera |
| Space | Freeze/resume the displayed snapshot; background collection continues |
| `V` / History button | Cycle 1-, 5-, and 15-minute history windows |
| Click a graph / `C` | Pin/unpin its timestamp / clear the shared cursor |
| Drag a panel divider | Resize Overview, GPU, or topology panels |
| `L` in Process tree | Toggle following the selected process family |
| `/` or click search | Search process name, command or PID |
| Click process header | Sort; click again to reverse |
| `I` | Open/close selected process inspector |
| `T` / `K` | Open End / Kill dialog for selected process |
| Enter / Escape in dialog | Confirm / cancel |
| Escape in search | Leave search |
| Escape in inspector | Close inspector |
| Escape in topology | Cancel gesture or deselect |
| Escape elsewhere | Close application |

### Metric sources and limits

Samples are collected approximately once per second, retaining 900 measured samples.
A POSIX worker thread owns collection; the UI consumes immutable snapshots from
three buffers and redraws independently. A frozen view retains its snapshot
while the collector continues publishing into the other buffers. Timestamps
drive 2D history positions, and the header reports collection duration on wide
windows. Read-only copies include only live process entries.
History starts empty and fills with real data. Up to 1,024 CPU IDs, 32,768
processes and 16 GPUs are supported; the Overview 3D graph draws the first
128 CPU IDs. CPU percentages use counter deltas without double-counting guest
time; process CPU uses 100% per logical CPU and may exceed 100%. RAM uses
`MemAvailable`. Counter rates use monotonic elapsed time.

Linux `/proc` supplies CPU, memory, processes, network, disk, load and uptime
counters. CPU topology/clocks and hardware-monitor sensors come from sysfs.
Network totals exclude loopback but can double-count bridges and tunnels.
Disk totals include whole block devices exposed in `/sys/block`, excluding
loop, RAM, device-mapper and md devices to reduce stacked-device duplication.

Intel i915/xe and AMD DRM engine counters are read from accessible process
`fdinfo` files. Clients are deduplicated by PCI device and DRM client ID;
engine percentages account for reported engine capacity. The GPU summary is
the busiest visible engine, not a sum of percentages. Processes belonging to
other users may be inaccessible, limiting GPU coverage. Resident memory is a
sum over clients and can count shared buffers more than once. Integrated GPUs
share system RAM; no dedicated VRAM total is invented.

AMD sysfs supplies additional telemetry when exposed. NVIDIA is supported via
optional dynamic loading of `libnvidia-ml.so.1` (NVML), without a build-time CUDA
or NVML requirement. NVIDIA engine-level splits are not supplied through that
backend. Intel i915 is verified on this machine; AMD, xe and NVIDIA require
validation on their respective hardware. GPU discovery occurs at startup.
Unavailable counters/sensors are labeled unavailable. This machine's Intel GPU
does not expose GPU temperature or board-power counters through these APIs.

The renderer uses OpenGL's compatibility API, matching the existing viewer.
It requires a graphical desktop and a compatibility context. Data collection
runs independently of OpenGL. Very large process/FD counts may reduce sampling
frequency; charts use actual timestamps rather than assuming a fixed interval.

### Verification

```sh
make check                 # both applications; no display needed
./lsm --check   # live metrics, parsers, and process-action checks
./lsm --smoke   # exercise UI views, resize, filtering and actions
```

`--check` also exercises PSI parsing, invalid/unavailable inputs and snapshot
ownership across repeated background publications. It validates CPU/process
rates, tricky process names containing
parentheses, stale process identities, DRM client deduplication, engine
capacity, memory units, counter regression and recovery. Memory tests validate map accounting, paths with spaces, list truncation,
process identity, a protected mapping, and exact known bytes from the test
process itself. No unrelated process bytes are read during tests. Signal tests create
and terminate only temporary child processes. `--smoke` additionally exercises
row selection, cancel, End and Kill through the actual UI callbacks, using
its own child processes. Other running processes are never signaled by tests.
It checks OpenGL errors and traverses all nine views, memory maps, a known-byte viewer, process inspector graphs,
3D node picking, subtree focus, folder navigation, hidden entries,
and signal dialogs; it briefly
requests fullscreen. Run graphical tests as your desktop user.

For visual inspection, set `LSM_CAPTURE_DIR` to an existing writable directory
when running `--smoke`; selected frames are saved there as PPM images.

Sources: [DRM client accounting](https://docs.kernel.org/gpu/drm-usage-stats.html),
[pidfd signaling](https://man7.org/linux/man-pages/man2/pidfd_send_signal.2.html),
[hardware sensor units](https://cdn.kernel.org/doc/html/latest/hwmon/sysfs-interface.html),
[NVML device queries](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlDeviceQueries.html).

Pressure source: [Linux PSI](https://docs.kernel.org/accounting/psi.html).

Memory accounting source: [Linux proc documentation](https://www.kernel.org/doc/html/latest/filesystems/proc.html).

Topology tests cover subtree layout, missing parents, cycles, and a 4,096-node
chain. Filesystem tests create temporary fixtures with hidden entries, spaces,
a symlink cycle, an invalid root, and a capped large directory, then remove
them. Background file publication is checked while holding an immutable view.
Graphical tests use a separate temporary directory and verify 3D node picking
and directory navigation without altering existing user files. Additional
tests cover cursor pin/clear, history ranges, divider drag bounds, independent
sidebar sizes, family summaries, and Follow identity handling. History helpers
are tested with wrapped rings, irregular sample times, empty ranges, and peaks
which have aged out of a shorter window.

Editor regression tests click off-center nodes and labels, check occlusion,
and verify pixel-accurate dragging under rotated cameras. UI tests reproduce
publication arriving between rendering, press and release, then exercise click
jitter, direct node movement, camera orbit/pan, snapshot capture during gestures,
cancel, undo, layout reset and filesystem refresh with reordered entries. The
filesystem fixture is checked to ensure visual dragging never moves its files.

## Original neural topology demonstration

An interactive 3D example with exactly **1,000 neurons**, arranged as ten
10-by-10 grids. Each non-input neuron has six distinct inputs from the preceding
layer: **5,400 directed connections** total. Layers progress left to right.

Weights and biases are deterministic random values. Animated sine-wave inputs
flow through actual weighted sums and tanh activations each frame. This is an
untrained sparse feedforward demonstration, not a trained prediction model.

### Build and run (Linux)

Requires a C11 compiler, Make, OpenGL development files and FreeGLUT development
files. On Debian/Ubuntu these are provided by `build-essential freeglut3-dev
libgl1-mesa-dev`; on Arch Linux use `base-devel freeglut mesa`.

```sh
make
./neural-topology
```

A graphical desktop and OpenGL 2.1 compatibility context are required to open
the viewer. This example deliberately uses the fixed-function OpenGL API to
keep the C code small and readable. A modern core-profile renderer would replace
the immediate-mode drawing and matrix stack with vertex buffers and shaders.

### Controls and colors

- Left click: select a neuron; click empty space or press `C` to clear.
- Left mouse drag: orbit the network (dragging does not change selection).
- Mouse wheel over the inspector: scroll incoming and outgoing connections.
- Mouse wheel or `+` / `-`: zoom.
- Space: pause/resume the input animation.
- `E`: toggle connections.
- `R`: reset camera and animation time.
- Escape: exit.

Cyan connections have positive weights; red connections have negative weights.
Connection opacity encodes weight magnitude. Neuron color shows activation
sign and magnitude; sphere radius grows with absolute activation. The layer
labels identify the input (L0) and subsequent layers (L1–L9).

Selected neurons turn gold, and their incoming and outgoing connections stay
highlighted even when `E` hides the other edges. The inspector shows the live
activation, bias, connection counts, peer neuron IDs, weights, and each
connection's contribution (`source activation * weight`). Input neurons use
sine waves, so their generated biases are unused. Pause with Space to inspect
a fixed forward pass.

If launching from a root shell on this machine, use the desktop user:

```sh
runuser -u x1 -- env DISPLAY=:0 /home/x1/neural-topology/neural-topology
```

### Verify

```sh
make check                    # topology and forward passes; no display needed
./neural-topology --smoke      # render five frames, check GL errors, then exit
```

`main.c` separates network construction (`build_network`), inference (`forward`)
and drawing (`display`). Change weights or activations there to connect a real
network. Larger dense networks need filtering or aggregation to stay readable.

Window and input API reference: [FreeGLUT documentation](https://freeglut.sourceforge.net/docs/api.php).

## Manual and installation

The final **Manual (9)** tab covers every view, process actions, memory inspection,
3D editing, metrics and command-line options. Scroll with the wheel, arrows or
Page Up/Down; Home/End jump to the beginning/end. Open it directly with
`./lsm --view manual`.

The Linux manual page is [lsm.1](lsm.1). Read it from this directory:

```sh
man -l ./lsm.1
```

Install the monitor and its manual together (the default prefix is `/usr/local`):

```sh
make install
lsm
man lsm
```

Use the privileges required by your chosen prefix. For a user installation,
use `make install PREFIX="$HOME/.local"` and ensure `$HOME/.local/bin` is on PATH.
Packaging can stage the installation with `DESTDIR` and override `BINDIR` or `MANDIR`.
The original `neural-topology` demonstration remains a separate build target.
