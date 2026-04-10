# SysForge — Parallel Windows System Utility
### PDC (Parallel & Distributed Computing) Lab — Mid Exam Project

> A high-performance, real-time Windows system monitor and process manager.
> Built with Win32 API, OpenMP parallel scanning, and a producer-consumer threading architecture.

---

## Quick Start

### Prerequisites
- **MinGW-w64** installed at `C:\msys64\mingw64\` (includes `g++` with OpenMP support)
- Windows 10/11 (64-bit)
- Administrator privileges (required for process suspension/kill and recycle bin operations)

### Build & Run

Open **PowerShell** or **Command Prompt** in the `src/` folder:

```powershell
cd "s:\Semesters\SEMESTER 6\PDC\LAB\Mid_Lab\SysForge\src"
.\build.bat
```

Then **double-click `sysforge.exe`** and accept the UAC (Administrator) prompt.

**Or build manually:**
```powershell
C:\msys64\mingw64\bin\g++.exe -std=c++17 -fopenmp -O2 `
  -o sysforge.exe sysforge.cpp sysforge_res.o `
  -lpdh -lpsapi -lcomctl32 -lgdi32 -luser32 -lkernel32 `
  -ladvapi32 -lshell32 -lole32 -lcomdlg32 -mwindows
```

> **Note:** If `sysforge.exe` is locked (app still running), `build.bat` automatically builds to a timestamped name like `sysforge_1044.exe` — build never fails.

---

## Project Structure

```
SysForge/
├── README.md                    ← This file
├── agent_smartscan_prompt.md    ← Agent instructions for Smart Scan feature
├── agent_bugfix_prompt.md       ← Agent instructions for earlier bug fixes
├── implementation.md            ← Implementation notes
├── project_plan.md              ← Original project plan
└── src/
    ├── sysforge.cpp             ← ENTIRE application (single-file C++17)
    ├── build.bat                ← Smart build script (never fails)
    ├── sysforge.rc              ← Windows resource file (manifest embed)
    ├── sysforge.manifest        ← UAC + Common Controls v6 manifest
    ├── sysforge_res.o           ← Pre-compiled resource object
    └── sysforge.exe             ← Final executable (run this)
```

---

## Architecture Overview

SysForge uses a **Producer-Consumer** threading model. Background threads collect data and post Windows messages to the GUI thread. The GUI thread *never* blocks — it only handles messages.

```
┌──────────────────────────────────────────────────────┐
│                    GUI Thread (WndProc)              │
│  Handles: WM_MONITOR_DATA, WM_PROCESS_DATA,          │
│           WM_CLEANER_DONE, WM_SMART_DONE, ...        │
└──────────┬──────────┬──────────┬────────────┬────────┘
           │          │          │            │
    ┌──────┴──┐ ┌─────┴────┐ ┌──┴──────┐ ┌──┴──────────┐
    │Monitor  │ │ Process  │ │ Cleaner │ │ Smart Scan  │
    │ Thread  │ │  Scan    │ │ Master  │ │  Thread     │
    │(500ms)  │ │ Thread   │ │ Thread  │ │ (7 threads) │
    │PDH API  │ │(PSAPI +  │ │(OpenMP) │ │ detached    │
    │         │ │TlHelp32) │ │         │ │             │
    └─────────┘ └──────────┘ └─────────┘ └─────────────┘
```

### Custom Message IDs (WM_USER + N)
| Message | Purpose |
|---|---|
| `WM_MONITOR_DATA` (+1) | CPU/RAM/GPU readings from MonitorThread |
| `WM_PROCESS_DATA` (+2) | Process list from ProcessScanThread |
| `WM_CLEANER_PROGRESS` (+3) | Directory scan progress updates |
| `WM_CLEANER_DONE` (+4) | Cleaner scan complete with file list |
| `WM_STRESS_DONE` (+5) | Stress test finished signal |
| `WM_SMART_DONE` (+7) | Smart Scan complete with junk file list |

---

## Code Breakdown — `sysforge.cpp`

The entire application lives in **one file (~2100 lines)**. Sections are clearly marked with `// ===` banners.

### 1. Data Structures (line ~95)
| Struct | Used For |
|---|---|
| `MonitorData` | CPU per-core %, total CPU, RAM, GPU, PDH query handle |
| `ProcessInfo` | PID, name, CPU%, RAM (KB), suspended flag |
| `ProcessListData` | Vector of `ProcessInfo` + scan metadata |
| `CleanerFileInfo` | File path, category, size in bytes, selected flag |
| `CleanerProgressData` | Scan result container — passed via PostMessage heap pointer |

### 2. Global State (line ~143)
- `g_hwndMonitorPage / ControllerPage / CleanerPage` — three tab page HWNDs
- `g_cpuBars[MAX_CORES]` / `g_cpuLabels[MAX_CORES]` — per-core progress bar controls
- `g_cpuBarY[MAX_CORES]` — stored Y positions for resize-safe layout
- `g_cleanerFiles` — current displayed file list (shared with delete handler)
- Atomic bools: `g_monitorRunning`, `g_cleanerScanning`, `g_smartScanning`, etc.

### 3. Monitor Thread — `MonitorThread()` (line ~220)
- Runs every **500ms** in a detached thread
- Uses **PDH (Performance Data Helper)** API to read:
  - CPU usage per logical core (`\Processor(N)\% Processor Time`)
  - Total CPU (`\Processor(_Total)\...`)
  - RAM via `GlobalMemoryStatusEx`
  - GPU via `\GPU Engine(*engtype_3D)\Utilization Percentage`
- Uses `PdhAddEnglishCounterW` (locale-safe)
- Posts `WM_MONITOR_DATA` with heap-allocated `MonitorData*`

### 4. Process Scan Thread — `ProcessScanThread()` (line ~370)
- Runs every **2 seconds**
- Uses `CreateToolhelp32Snapshot` + `Process32First/Next` for PID enumeration
- Opens each process with `OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ)`
- Reads RAM via `GetProcessMemoryInfo` (PSAPI)
- Posts `WM_PROCESS_DATA` with heap-allocated `ProcessListData*`

### 5. Stress Test — `StressTestThread()` (line ~460)
- Runs for **10 seconds** at 100% CPU
- **Reserves 1 core** for the GUI (uses `omp_get_max_threads() - 1`) so the app stays responsive
- Uses `#pragma omp parallel for` with a busy-compute loop
- Posts `WM_STRESS_DONE` when finished

### 6. Cleaner Scan — `CleanerMasterThread()` (line ~735)
Activated by **[Scan Dirs]** button. Scans 7 fixed directories:
- `C:\Windows\Temp`
- `%TEMP%` (user temp)
- `C:\Windows\Prefetch`
- `C:\Windows\SoftwareDistribution\Download`
- Chrome cache
- Edge cache
- Windows Logs

Uses `#pragma omp parallel for` across directories. Posts progress + final result via `WM_CLEANER_DONE`.

### 7. Smart Scan — `SmartScanThread()` (line ~900)
Activated by **[Smart Scan]** button. Spawns **7 dedicated `std::thread`s** in parallel:

| Thread | Category | Target |
|---|---|---|
| `th0` | Windows Temp | `.tmp .bak .old .gid .chk` in `C:\Windows\Temp` |
| `th1` | User Temp | Same extensions in `%TEMP%` |
| `th2` | Browser Caches | Chrome, Edge, Opera, Firefox cache dirs |
| `th3` | Logs + Prefetch | `.log .etl` files + `.pf` prefetch cache |
| `th4` | Thumbnail Cache | `thumbcache_*.db` in Explorer dir |
| `th5` | Update Cache | All files in `SoftwareDistribution\Download` |
| `th6` | Recycle Bin + Duplicates | `SHQueryRecycleBin` + same-size/content file detection |

Results posted via `WM_SMART_DONE`.

**Duplicate Detection:** Groups files by exact size, then compares first **4KB of content** using `ReadFile` + `memcmp`. Flags the newer copy as a duplicate.

### 8. Delete Handler — `DeleteFilesThread()` (line ~836)
- Checks if any selected item has category `"Recycle Bin"`
- If yes → calls `SHEmptyRecycleBin` (with `CoInitializeEx` for COM on the thread)
- All other files → parallel `DeleteFile` via OpenMP
- Shows result dialog: `"Deleted N items. Failed: M."`

### 9. Process Controller Actions
| Button | API Used |
|---|---|
| **Suspend** | `NtSuspendProcess` via `GetProcAddress` on `ntdll.dll` |
| **Resume** | `NtResumeProcess` via `GetProcAddress` |
| **Kill** | `TerminateProcess` |
| **Launch** | `CreateProcess` |
| **Browse & Launch** | `GetOpenFileName` (file picker) + `CreateProcess` |

Critical system processes (`System`, `svchost`, `lsass`, etc.) are blocked from suspend/kill.

### 10. Responsive Layout — `DoLayout()` (line ~1620)
Called on every `WM_SIZE`. Reflows:
- Tab control to fill 100% window
- Per-page containers (Monitor / Controller / Cleaner) to fill tab area
- ListViews to fill page height
- Progress bars to stretch full width
- Per-core bars move both **label + bar** as a unit into correct columns

### 11. UI Theming
- **Font system:** Segoe UI (body), Segoe UI Semibold (headers), Cascadia Mono (numeric data)
- **Colors (namespace `C::`):** Blue-600 accent, Slate-900 text, alternating ListView rows
- `WM_CTLCOLORSTATIC` → colors static labels based on their font (header = blue, mono = dark, other = muted)
- `WM_ERASEBKGND` → fills background with palette color (no white flashing on resize)
- Progress bars: Green (0–60%), Yellow (60–80%), Red (80–100%)

### 12. Proof Panel — F2 Key
Press **F2** to overlay a diagnostic panel showing:
- Raw PDH query handle value
- Thread IDs of active background threads
- Timestamps of last data received
- Real process count and scan duration

---

## Key Libraries Used

| Library | Purpose |
|---|---|
| `pdh.lib` | Performance Data Helper — CPU/GPU counters |
| `psapi.lib` | Process memory info (`GetProcessMemoryInfo`) |
| `comctl32.lib` | ListView, TabControl, ProgressBar common controls |
| `shell32.lib` | `SHEmptyRecycleBin`, `SHQueryRecycleBin` |
| `ole32.lib` | COM init for Shell APIs (`CoInitializeEx`) |
| `comdlg32.lib` | File picker dialog (`GetOpenFileName`) |
| `advapi32.lib` | Debug privilege (`AdjustTokenPrivileges`) |
| OpenMP (`-fopenmp`) | Parallel directory scanning, parallel file deletion |

---

## Known Limitations

| Item | Detail |
|---|---|
| GPU reading | Returns 0 on systems without standard `\GPU Engine` PDH counters |
| Duplicate scan | Only scans Downloads + Documents (not full disk, by design) |
| Smart Scan speed | First run may take 10–30s depending on browser cache size |
| UAC required | Process suspend/kill/resume and recycle bin empty need Admin rights |

---

## PDC Concepts Demonstrated

| Concept | Where |
|---|---|
| **Parallel Threads** | 7 threads in SmartScanThread, 3 always-running background threads |
| **OpenMP** | `#pragma omp parallel for` in CleanerMasterThread + DeleteFilesThread |
| **Producer-Consumer** | All threads post `PostMessage` → GUI thread consumes |
| **Atomic Operations** | `std::atomic<bool>` guards all thread-start paths |
| **Thread Safety** | No shared mutable state between scan threads (thread-local vectors) |
| **Detached Threads** | `std::thread(...).detach()` for fire-and-forget operations |
