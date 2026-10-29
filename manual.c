#include "manual.h"

const ManualSection lsm_manual[] = {
    {"GETTING STARTED",
     "lsm is a Linux system monitor written in C with OpenGL graphs. Click a tab or press 1-9 "
     "to choose a view. F toggles fullscreen. Space freezes the displayed data while background "
     "collection continues. Escape closes core details, a dialog or search first; in topology it "
     "cancels a "
     "gesture or deselects; elsewhere it exits. This manual scrolls with the wheel, arrow keys "
     "or Page Up/Down. Home/End jump to the start/end."},
    {"THE NINE VIEWS",
     "1 Overview: resource summaries and CPU/RAM/disk histories. 2 CPU: logical cores, clocks, "
     "topology and available sensors; click a core for details. 3 GPU: device information, "
     "engine histories and available telemetry. 4 Processes: sortable, searchable process "
     "table. 5 Pressure: CPU, memory and I/O stall percentages. 6 RAM: memory accounting and "
     "largest resident processes. 7 Process tree: live parent/child topology. 8 Filesystem: "
     "directory metadata in 3D. 9 Manual: this reference."},
    {"COLORS AND DANGER ZONES",
     "P or the palette button cycles Spectrum, Aurora and Ember for this session. Each resource "
     "has its own accent; amber and red keep the same meaning in every palette. CPU/core/GPU "
     "utilization, RAM and dedicated VRAM turn amber at 85% and red at 95%. Percentage histories "
     "shade the warning/danger zones and color measured samples; the 3D history marks the 95% "
     "boundary. The header names readings that triggered WARNING or DANGER ZONE. These are "
     "display thresholds for resource use, not hardware temperature limits."},
    {"SYSTEM OVERLOAD ALERTS",
     "SYSTEM OVERLOAD appears in red when 1-minute load divided by online logical CPUs reaches "
     "1.5, or PSI some avg10 reaches 25%, or memory/I/O PSI full avg10 reaches 15%. Amber starts "
     "at load/CPU 1.0, PSI some 10%, and PSI full 5%. PSI describes stalled time. High utilization "
     "alone uses DANGER ZONE. Missing counters stay unavailable. Swap occupancy alone does not "
     "trigger overload. Process CPU values still use 100% per logical CPU; their warning color "
     "uses the percentage of total machine CPU capacity. HELD marks a frozen or edited snapshot."},
    {"HISTORY AND LAYOUT",
     "V or the History button cycles 1, 5 and 15 minutes. Up to 900 measured samples are kept "
     "during this session. Hover a 2D graph to inspect a sample; click to pin/unpin its time, "
     "and C clears the shared cursor. This does not rewind tables, topology or memory bytes. "
     "Drag the dividers in Overview, GPU or topology views to resize their panels. CPU/GPU "
     "3D histories use left-drag to orbit, wheel or +/- to zoom, and R to reset the camera."},
    {"CPU CORE DETAILS",
     "On Overview, hover a 3D CPU trace to read its measured usage and sample age. Click to "
     "select the logical CPU, pin that sample and open its details. I toggles the panel; "
     "Escape or Close closes it. Use < and >, [ and ], or left/right arrows to change cores. "
     "Wheel over the panel or up/down/Page Up/Down scrolls its rows. In CPU view, select a "
     "core and press I or Core details to inspect it. Small windows show details in place "
     "of the graph; close the panel to return. CPU view lists all collected CPU IDs."},
    {"READING CORE INFORMATION",
     "Each entry is a logical CPU. Details show physical core/socket/die IDs, sibling CPU "
     "IDs, raw kernel core type when exposed, busy/user/kernel/idle/I/O-wait percentages, "
     "reported frequency, hardware frequency range, current policy min/max, base frequency, "
     "governor, driver and available core temperature. L1 data/instruction and L2/L3 cache "
     "sizes may describe shared cache instances; core sensors can also be shared by SMT "
     "siblings. Missing fields are unavailable. Hardware fields are from the latest displayed "
     "snapshot. Only CPU usage is historical here; a pinned sample does not rewind clocks "
     "or temperatures. Window average/peak/count use real samples in the selected window."},
    {"PROCESS SEARCH AND INSPECTION",
     "In Processes, click a column heading to sort; click again to reverse. Press / or click "
     "the search box to filter by name, command or PID. Select a row and press I to open its "
     "inspector. Inspector keys 1/2/3 show CPU/RAM/I/O histories, 4 or M opens Maps, and 5 or B "
     "requests Bytes for the selected readable mapping. I or Escape closes the inspector. "
     "Process CPU uses 100% per logical CPU, so a multithreaded process can exceed 100%."},
    {"END AND KILL",
     "Select a process in Processes or Process tree. T opens End (SIGTERM), which asks it to "
     "exit; K opens Kill (SIGKILL), which terminates it immediately and can lose unsaved work. "
     "Check the named process in the dialog, then Enter confirms or Escape cancels. Only that "
     "process is signaled, not its children. lsm checks PID/start-time identity and uses a "
     "pidfd; it protects PID 1 and itself. Normal Linux permissions apply."},
    {"RAM, MAPS AND BYTES",
     "RAM categories overlap; do not add them as separate physical memory totals. Click a "
     "process in RAM to open its maps. RSS includes shared resident pages; PSS apportions "
     "shared pages. Maps lists at most 4096 regions, with full summary accounting. Cycle the "
     "type filter or press S to switch map sorting. Bytes reads at most 256 bytes per explicit "
     "request from /proc/PID/mem. Prev/Next/Reload request another page of bytes. Reads obey "
     "Linux debugging permissions and are never automatic; denied access is reported."},
    {"SELECTING AND MOVING 3D NODES",
     "Click a node or its label to select it. Left-drag a node to move its visual placement "
     "in the camera-facing plane. Right-drag orbits. Middle-drag, Shift+left-drag, or dragging "
     "empty space pans. Wheel or +/- zooms. R/Fit fits the camera and keeps placements. "
     "Ctrl+Z undoes a placement; G/Reset restores the automatic layout for this scope. Escape "
     "cancels an active gesture. Edits change only the drawing: files and process ancestry "
     "remain unchanged. Placements and undo are bounded and kept for this session."},
    {"PROCESS TREE NAVIGATION",
     "Links point from observed parents to children. Visible roots is a synthetic grouping "
     "node. Select a process, then Enter/Focus isolates its family; Backspace/Parent focuses "
     "its parent. A/All restores all processes; U/Mine toggles your user's processes. / searches "
     "names or PIDs; Enter focuses the first match. L/Follow tracks the selected PID and start "
     "time and refits when its family changes. Manual camera/node gestures end Follow. "
     "I inspects and T/K open the process action dialogs. Family RSS can double-count shared "
     "pages. This tree describes ancestry, not IPC or socket connections."},
    {"FILESYSTEM NAVIGATION",
     "Enter opens the selected directory; Backspace/Up goes to its parent. Home and / buttons "
     "open those roots. Press / or click the path box to type a path, then Enter. Depth cycles "
     "1-4; H toggles hidden entries; Refresh rescans. The default root is the launch directory. "
     "Scans read metadata only and include at most 2048 entries. Child symlinks are not followed; "
     "mount points are shown but not traversed until entered. Directory sizes are inode sizes, "
     "not recursive totals. Refresh preserves node placements and selection by full path."},
    {"METRICS AND HARDWARE COVERAGE",
     "Samples arrive approximately once per second from /proc, sysfs and available GPU "
     "interfaces. Up to 1024 CPU IDs, 32768 processes and 16 GPUs are collected; the Overview "
     "3D graph draws the first 128 CPU IDs. Process tree displays at most 4095 processes. "
     "Unavailable sensors are labeled unavailable. Intel/AMD engine coverage depends on "
     "accessible DRM client counters; NVIDIA uses optional libnvidia-ml.so.1. Intel i915 was "
     "tested on this machine; AMD, xe and NVIDIA still need hardware validation. Pressure "
     "values are stall percentages, not CPU utilization."},
    {"COMMAND LINE",
     "./lsm starts Overview. ./lsm --view tree opens process topology. ./lsm --view files "
     "--root /home opens a filesystem root. Views: overview, cpu, gpu, processes, pressure, "
     "ram, tree, files, manual. --help lists options; --version prints the version. --check "
     "runs checks without a display. --smoke exercises the graphical UI, including temporary "
     "test children and files; it does not signal unrelated processes."},
    {"TERMINAL MANUAL AND ENVIRONMENT",
     "Read the full terminal reference with man lsm after make install, or man -l ./lsm.1 "
     "from the build directory. MONITOR_FONT selects a monospace TTF font. LSM_CAPTURE_DIR "
     "selects an existing writable directory for PPM screenshots during --smoke. The normal "
     "UI needs a graphical desktop with compatibility OpenGL. No root privileges are needed "
     "to start lsm. Histories, camera settings and visual layouts are not saved across restarts."}};

const size_t lsm_manual_count = sizeof lsm_manual / sizeof lsm_manual[0];
