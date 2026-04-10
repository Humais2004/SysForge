/*=============================================================================
 *  SysForge — Parallel Windows System Utility
 *  PDC Lab Exam | Single-file C++ Implementation
 *
 *  Compile (MSVC):
 *    cl /EHsc /openmp /std:c++17 sysforge.cpp /link pdh.lib psapi.lib comctl32.lib user32.lib gdi32.lib kernel32.lib advapi32.lib shell32.lib ole32.lib
 *
 *  Compile (MinGW g++):
 *    g++ -std=c++17 -fopenmp sysforge.cpp -o sysforge.exe -lpdh -lpsapi -lcomctl32 -lgdi32 -luser32 -lkernel32 -ladvapi32 -lshell32 -lole32 -static
 *
 *  Architecture: Producer-Consumer via PostMessage
 *  Golden Rule: GUI thread NEVER blocked > 0.5s
 *=============================================================================*/

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>
#include <shellapi.h>   // Shell APIs
#include <shlobj.h>     // SHEmptyRecycleBin, SHQueryRecycleBin, SHQUERYRBINFO
#include <objbase.h>    // CoInitializeEx, CoUninitialize

#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>
#include <map>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cmath>
#include <future>
#include <filesystem>

#ifdef _OPENMP
#include <omp.h>
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
    name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
    processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

// Global UI fonts
static HFONT g_hUIFont      = NULL;   // Segoe UI 15px — body
static HFONT g_hTitleFont   = NULL;   // Segoe UI 11px Bold — section headers
static HFONT g_hMonoFont    = NULL;   // Consolas 13px — numeric data

// Luxury color palette
namespace C {
    static const COLORREF BgPage      = RGB(240, 242, 248); // Page background
    static const COLORREF BgCard      = RGB(255, 255, 255); // Card surface
    static const COLORREF Accent      = RGB( 37, 99, 235);  // Blue-600
    static const COLORREF AccentLight = RGB(219, 234, 254); // Blue-100
    static const COLORREF TextPrimary = RGB( 15,  23,  42); // Slate-900
    static const COLORREF TextMuted   = RGB(100, 116, 139); // Slate-400
    static const COLORREF RowEven     = RGB(248, 250, 252); // Slate-50
    static const COLORREF RowOdd      = RGB(255, 255, 255);
    static const COLORREF BarBg       = RGB(226, 232, 240); // Slate-200
    static const COLORREF BarGreen    = RGB( 34, 197,  94); // Green-500
    static const COLORREF BarYellow   = RGB(234, 179,   8); // Yellow-500
    static const COLORREF BarRed      = RGB(239,  68,  68); // Red-500
}
// Brush cache (created once)
static HBRUSH g_brBgPage  = NULL;
static HBRUSH g_brBgCard  = NULL;
static HBRUSH g_brAccentL = NULL;

// ============================================================================
//  CONSTANTS & CUSTOM MESSAGES
// ============================================================================
constexpr int MAX_CORES = 64;
constexpr int MAX_PROCS = 2048;
constexpr UINT WM_MONITOR_DATA     = WM_USER + 1;
constexpr UINT WM_PROCESS_DATA     = WM_USER + 2;
constexpr UINT WM_CLEANER_PROGRESS = WM_USER + 3;
constexpr UINT WM_CLEANER_DONE     = WM_USER + 4;
constexpr UINT WM_STRESS_DONE      = WM_USER + 5;
constexpr UINT WM_SMART_PROGRESS   = WM_USER + 6;
constexpr UINT WM_SMART_DONE       = WM_USER + 7;

// ============================================================================
//  DATA STRUCTURES (heap-allocated, passed via PostMessage)
// ============================================================================
struct MonitorData {
    int    coreCount;
    double cpuPerCore[MAX_CORES];
    double cpuTotal;
    double ramUsedMB;
    double ramTotalMB;
    double gpuUsage;          // GPU bonus
    DWORD  threadId;
    LONGLONG  timestamp;      // high-res clock
    PDH_HQUERY queryHandle;   // proof: real handle
};

struct ProcessInfo {
    DWORD  pid;
    wchar_t name[260];
    double cpuPercent;        // placeholder for future
    SIZE_T ramKB;
    bool   isSuspended;
};

struct ProcessListData {
    std::vector<ProcessInfo> procs;
    DWORD  threadId;
    LONGLONG timestamp;
    double scanDurationMs;
    int    pidCount;
};

struct CleanerFileInfo {
    std::wstring path;
    std::wstring category;
    ULONGLONG    sizeBytes;
    bool         selected;
};

struct CleanerProgressData {
    int filesFound;
    int directoriesScanned;
    bool scanComplete;
    double scanDurationMs;
    DWORD threadId;
    LONGLONG timestamp;
    std::vector<CleanerFileInfo>* fileList; // owned pointer when scanComplete
};

// ============================================================================
//  GLOBAL STATE (thread-safe where needed)
// ============================================================================
static HWND g_hwndMain   = NULL;
static HWND g_hwndTab    = NULL;
static HWND g_hwndStatus = NULL;
static HINSTANCE g_hInst = NULL;

// Tab 1: Monitor controls
static HWND g_hwndMonitorPage    = NULL;
static HWND g_cpuBars[MAX_CORES] = {};
static HWND g_cpuLabels[MAX_CORES] = {};
static HWND g_hwndCpuTotalBar    = NULL;
static HWND g_hwndCpuTotalLabel  = NULL;
static HWND g_hwndRamBar         = NULL;
static HWND g_hwndRamLabel       = NULL;
static HWND g_hwndGpuBar         = NULL;
static HWND g_hwndGpuLabel       = NULL;
static HWND g_hwndStressBtn      = NULL;
static int  g_coreCount          = 0;
static int  g_cpuBarX[MAX_CORES]   = {};  // bar x at creation
static int  g_cpuBarY[MAX_CORES]   = {};  // bar y at creation (fixed, never changes)
static int  g_cpuLabelX[MAX_CORES] = {};  // label x at creation

// Tab 2: Process Controller controls
static HWND g_hwndControllerPage = NULL;
static HWND g_hwndProcessList    = NULL;
static HWND g_hwndFilterEdit     = NULL;
static HWND g_hwndLaunchBtn      = NULL;
static HWND g_hwndLaunchPathBtn  = NULL;
static HWND g_hwndSuspendBtn     = NULL;
static HWND g_hwndResumeBtn      = NULL;
static HWND g_hwndKillBtn        = NULL;
static HWND g_hwndRefreshBtn     = NULL;
static HWND g_hwndProcCountLabel = NULL;
static std::vector<ProcessInfo> g_currentProcs;

// Tab 3: Cleaner controls
static HWND g_hwndCleanerPage     = NULL;
static HWND g_hwndCleanerList     = NULL;
static HWND g_hwndScanBtn         = NULL;
static HWND g_hwndSmartScanBtn    = NULL;   // NEW: Smart Scan
static HWND g_hwndDeleteBtn       = NULL;
static HWND g_hwndCleanerStatus   = NULL;
static HWND g_hwndCleanerProgress = NULL;
static std::vector<CleanerFileInfo> g_cleanerFiles;

// Proof Panel state
static bool g_proofPanelVisible  = false;
static MonitorData g_lastMonitor = {};
static ProcessListData g_lastProcData = {};

// Per-process CPU tracking: store previous kernel+user times
struct ProcCpuSnapshot {
    ULONGLONG kernelTime;
    ULONGLONG userTime;
    ULONGLONG wallTime; // system tick count
};
static std::map<DWORD, ProcCpuSnapshot> g_prevCpuTimes;

// Thread control
static std::atomic<bool> g_monitorRunning{true};
static std::atomic<bool> g_processRunning{true};
static std::atomic<bool> g_stressRunning{false};
static std::atomic<bool> g_cleanerScanning{false};
static std::atomic<bool> g_smartScanning{false};   // NEW

// IDs for controls
enum ControlIDs {
    IDC_TAB = 100,
    IDC_STRESS_BTN = 200,
    IDC_FILTER_EDIT = 300,
    IDC_LAUNCH_BTN, IDC_LAUNCH_PATH_BTN, IDC_SUSPEND_BTN, IDC_RESUME_BTN, IDC_KILL_BTN, IDC_REFRESH_BTN,
    IDC_SCAN_BTN = 400, IDC_DELETE_BTN, IDC_SELECT_ALL_BTN, IDC_DESELECT_ALL_BTN,
    IDC_SMART_SCAN_BTN = 450,   // NEW: Smart Junk Scanner
    IDC_PROC_LIST = 500,
    IDC_CLEANER_LIST = 600,
};

// ============================================================================
//  UTILITY: Acquire SeDebugPrivilege
// ============================================================================
static bool AcquireDebugPrivilege() {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp = {};
    if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(hToken);
    return ok && GetLastError() == ERROR_SUCCESS;
}

// ============================================================================
//  UTILITY: Safety check — never kill critical processes
// ============================================================================
static const wchar_t* g_noKillList[] = {
    L"System", L"smss.exe", L"csrss.exe", L"wininit.exe",
    L"services.exe", L"lsass.exe", L"svchost.exe", L"dwm.exe",
    L"winlogon.exe", L"explorer.exe", nullptr
};

static bool IsCriticalProcess(DWORD pid, const wchar_t* name) {
    if (pid < 10) return true;
    for (int i = 0; g_noKillList[i]; i++) {
        if (_wcsicmp(name, g_noKillList[i]) == 0) return true;
    }
    return false;
}

// ============================================================================
//  UTILITY: Locale-safe PDH counter registration
//  PdhAddEnglishCounterW works on Vista+ regardless of Windows display language
// ============================================================================
typedef PDH_STATUS (WINAPI *PdhAddEnglishCounterW_t)(
    PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
static PdhAddEnglishCounterW_t g_pPdhAddEnglishCounter = nullptr;

static PDH_STATUS SafeAddCounter(PDH_HQUERY hQuery, LPCWSTR path,
                                  DWORD_PTR userData, PDH_HCOUNTER* pCounter) {
    // Try locale-independent version first
    if (g_pPdhAddEnglishCounter) {
        PDH_STATUS s = g_pPdhAddEnglishCounter(hQuery, path, userData, pCounter);
        if (s == ERROR_SUCCESS) return s;
    }
    // Fallback to standard PdhAddCounter (works if OS language is English)
    return PdhAddCounter(hQuery, path, userData, pCounter);
}

// ============================================================================
//  AGENT 1: TAB 1 — HARDWARE MONITOR THREAD (Producer)
// ============================================================================
static void MonitorThread(HWND hwnd) {
    // Resolve PdhAddEnglishCounterW at runtime (not in MinGW headers)
    HMODULE hPdh = GetModuleHandle(L"pdh.dll");
    if (!hPdh) hPdh = LoadLibrary(L"pdh.dll");
    if (hPdh) {
        g_pPdhAddEnglishCounter = (PdhAddEnglishCounterW_t)
            GetProcAddress(hPdh, "PdhAddEnglishCounterW");
    }

    // Get core count
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    int numCores = (int)si.dwNumberOfProcessors;
    if (numCores > MAX_CORES) numCores = MAX_CORES;

    // --- PDH Setup ---
    PDH_HQUERY hQuery = NULL;
    PDH_HCOUNTER hCpuTotal = NULL;
    PDH_HCOUNTER hCpuCores[MAX_CORES] = {};
    PDH_HCOUNTER hGpu = NULL;
    int pdhErrors = 0;

    PDH_STATUS qs = PdhOpenQuery(NULL, 0, &hQuery);
    if (qs != ERROR_SUCCESS) {
        // Fatal: PDH not available — post empty data forever
        while (g_monitorRunning.load()) { Sleep(1000); }
        return;
    }

    // Total CPU
    if (SafeAddCounter(hQuery, L"\\Processor(_Total)\\% Processor Time", 0, &hCpuTotal) != ERROR_SUCCESS)
        pdhErrors++;

    // Per-core CPU
    for (int i = 0; i < numCores; i++) {
        wchar_t counterPath[256];
        swprintf_s(counterPath, L"\\Processor(%d)\\%% Processor Time", i);
        if (SafeAddCounter(hQuery, counterPath, 0, &hCpuCores[i]) != ERROR_SUCCESS)
            pdhErrors++;
    }

    // GPU bonus — may fail on machines without GPU counters
    bool hasGpu = (SafeAddCounter(hQuery,
        L"\\GPU Engine(*)\\Utilization Percentage", 0, &hGpu) == ERROR_SUCCESS);

    // First collection (baseline — PDH requires two samples to compute delta)
    PdhCollectQueryData(hQuery);
    Sleep(1000); // 1s baseline for reliable first reading

    while (g_monitorRunning.load()) {
        PdhCollectQueryData(hQuery);

        auto* data = new MonitorData();
        data->coreCount = numCores;
        data->queryHandle = hQuery;
        data->threadId = GetCurrentThreadId();

        // Timestamp
        auto now = std::chrono::high_resolution_clock::now();
        data->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        // CPU Total
        PDH_FMT_COUNTERVALUE val = {};
        DWORD counterType = 0;
        if (hCpuTotal && PdhGetFormattedCounterValue(hCpuTotal, PDH_FMT_DOUBLE, &counterType, &val) == ERROR_SUCCESS)
            data->cpuTotal = val.doubleValue;

        // Per-core CPU
        for (int i = 0; i < numCores; i++) {
            if (hCpuCores[i] && PdhGetFormattedCounterValue(hCpuCores[i], PDH_FMT_DOUBLE, &counterType, &val) == ERROR_SUCCESS)
                data->cpuPerCore[i] = val.doubleValue;
        }

        // RAM via GlobalMemoryStatusEx
        MEMORYSTATUSEX memInfo = {};
        memInfo.dwLength = sizeof(memInfo);
        GlobalMemoryStatusEx(&memInfo);
        data->ramTotalMB = memInfo.ullTotalPhys / (1024.0 * 1024.0);
        data->ramUsedMB = (memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024.0 * 1024.0);

        // GPU bonus
        data->gpuUsage = 0.0;
        if (hasGpu && hGpu) {
            if (PdhGetFormattedCounterValue(hGpu, PDH_FMT_DOUBLE, &counterType, &val) == ERROR_SUCCESS)
                data->gpuUsage = val.doubleValue;
        }

        // Post to GUI — GUI thread will delete this
        PostMessage(hwnd, WM_MONITOR_DATA, 0, (LPARAM)data);

        Sleep(500); // 500ms polling rate
    }

    PdhCloseQuery(hQuery);
}

// ============================================================================
//  AGENT 1: STRESS TEST (one thread per core, pinned via affinity)
// ============================================================================
static void StressTestThread(HWND hwnd) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    int numCores = (int)si.dwNumberOfProcessors;
    // Reserve 1 core for GUI + monitor thread responsiveness
    int stressCores = (numCores > 2) ? (numCores - 1) : numCores;

    g_stressRunning.store(true);
    std::vector<std::thread> workers;

    for (int i = 0; i < stressCores; i++) {
        workers.emplace_back([i]() {
            // Pin this thread to core i for per-core bar visibility
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << i);
            // NOTE: Do NOT use THREAD_PRIORITY_HIGHEST — it starves the
            // monitor thread and GUI, causing bars to lag instead of updating live.

            volatile unsigned long long counter = 0;
            auto endTime = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < endTime && g_stressRunning.load()) {
                for (int j = 0; j < 10000; j++) {
                    counter++;
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    g_stressRunning.store(false);
    PostMessage(hwnd, WM_STRESS_DONE, 0, 0);
}

// ============================================================================
//  AGENT 1: TAB 2 — PROCESS CONTROLLER THREAD (Producer)
//  Uses CreateToolhelp32Snapshot for names (works for ALL processes)
//  then OpenProcess + OpenMP only for memory info
// ============================================================================
static ULONGLONG FileTimeToULL(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static void ProcessScanThread(HWND hwnd) {
    // Get number of logical processors for CPU% normalization
    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);
    int numProcessors = (int)sysInfo.dwNumberOfProcessors;

    while (g_processRunning.load()) {
        auto startTime = std::chrono::high_resolution_clock::now();
        ULONGLONG currentWallTime = GetTickCount64();

        // Step 1: Snapshot ALL processes to get names + PIDs
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) {
            Sleep(2000);
            continue;
        }

        std::vector<ProcessInfo> allProcs;
        PROCESSENTRY32 pe = {};
        pe.dwSize = sizeof(pe);

        if (Process32First(hSnap, &pe)) {
            do {
                ProcessInfo pi = {};
                pi.pid = pe.th32ProcessID;
                pi.cpuPercent = 0.0;
                pi.ramKB = 0;
                pi.isSuspended = false;
                wcscpy_s(pi.name, pe.szExeFile);
                allProcs.push_back(pi);
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);

        int pidCount = (int)allProcs.size();

        // Step 2: Parallel memory + CPU time gathering with OpenMP
        // Each thread stores its own CPU snapshot for delta calculation
        std::map<DWORD, ProcCpuSnapshot> currentSnapshots;
        std::mutex snapMutex;

        #pragma omp parallel for schedule(dynamic, 16)
        for (int i = 0; i < pidCount; i++) {
            if (allProcs[i].pid == 0) continue;

            HANDLE hProc = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                FALSE, allProcs[i].pid);
            if (!hProc) {
                hProc = OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION,
                    FALSE, allProcs[i].pid);
            }

            if (hProc) {
                // RAM
                PROCESS_MEMORY_COUNTERS pmc = {};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    allProcs[i].ramKB = pmc.WorkingSetSize / 1024;
                }

                // CPU times for delta calculation
                FILETIME ftCreate, ftExit, ftKernel, ftUser;
                if (GetProcessTimes(hProc, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                    ULONGLONG kernelNow = FileTimeToULL(ftKernel);
                    ULONGLONG userNow   = FileTimeToULL(ftUser);

                    ProcCpuSnapshot snap = { kernelNow, userNow, currentWallTime };

                    // Compute CPU% from previous snapshot
                    {
                        std::lock_guard<std::mutex> lock(snapMutex);
                        auto it = g_prevCpuTimes.find(allProcs[i].pid);
                        if (it != g_prevCpuTimes.end()) {
                            ULONGLONG kernelDelta = kernelNow - it->second.kernelTime;
                            ULONGLONG userDelta   = userNow   - it->second.userTime;
                            ULONGLONG wallDelta   = (currentWallTime - it->second.wallTime) * 10000ULL; // ms -> 100ns

                            if (wallDelta > 0) {
                                double cpuPct = (double)(kernelDelta + userDelta) / (double)(wallDelta * numProcessors) * 100.0;
                                if (cpuPct > 100.0) cpuPct = 100.0;
                                if (cpuPct < 0.0) cpuPct = 0.0;
                                allProcs[i].cpuPercent = cpuPct;
                            }
                        }
                        currentSnapshots[allProcs[i].pid] = snap;
                    }
                }

                CloseHandle(hProc);
            }
        }

        // Update global snapshot for next cycle
        g_prevCpuTimes = std::move(currentSnapshots);

        // Step 3: Sort by CPU% descending (then by RAM) — like Task Manager
        std::sort(allProcs.begin(), allProcs.end(),
            [](const ProcessInfo& a, const ProcessInfo& b) {
                if (std::abs(a.cpuPercent - b.cpuPercent) > 0.01)
                    return a.cpuPercent > b.cpuPercent; // Higher CPU first
                return a.ramKB > b.ramKB;               // Then higher RAM
            });

        auto endTime = std::chrono::high_resolution_clock::now();
        double scanMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        auto* data = new ProcessListData();
        data->procs = std::move(allProcs);
        data->threadId = GetCurrentThreadId();
        data->scanDurationMs = scanMs;
        data->pidCount = pidCount;
        auto now = std::chrono::high_resolution_clock::now();
        data->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        PostMessage(hwnd, WM_PROCESS_DATA, 0, (LPARAM)data);
        Sleep(2000); // 2s refresh cycle
    }
}

// ============================================================================
//  AGENT 1: TAB 2 — SUSPEND / RESUME / KILL (detached operations)
// ============================================================================
static void SuspendProcess(DWORD targetPid) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);

    if (Thread32First(hSnapshot, &te)) {
        do {
            if (te.th32OwnerProcessID == targetPid) {
                HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (hThread) {
                    SuspendThread(hThread);
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(hSnapshot, &te));
    }
    CloseHandle(hSnapshot);
}

static void ResumeProcess(DWORD targetPid) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);

    if (Thread32First(hSnapshot, &te)) {
        do {
            if (te.th32OwnerProcessID == targetPid) {
                HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (hThread) {
                    ResumeThread(hThread);
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(hSnapshot, &te));
    }
    CloseHandle(hSnapshot);
}

static void KillProcess(DWORD targetPid) {
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, targetPid);
    if (hProc) {
        TerminateProcess(hProc, 1);
        CloseHandle(hProc);
    }
}

static void LaunchProcess(const wchar_t* cmdLine) {
    STARTUPINFO si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    wchar_t cmd[MAX_PATH * 2];
    wcscpy_s(cmd, cmdLine);

    CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);
}

static void LaunchProcessFromDialog(HWND hwndOwner) {
    wchar_t filePath[MAX_PATH * 2] = {};

    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwndOwner;
    ofn.lpstrFilter = L"Executable Files (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"Select Application to Launch";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileName(&ofn)) {
        // Launch the selected executable in a background thread
        std::wstring path(filePath);
        std::thread([path]() {
            LaunchProcess(path.c_str());
        }).detach();
    }
}

// ============================================================================
//  AGENT 1: TAB 3 — PARALLEL CLEANER (Producer)
// ============================================================================
struct CleanTarget {
    std::wstring path;
    std::wstring category;
};

static bool DirectoryExists(const std::wstring& path) {
    DWORD attr = GetFileAttributes(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

static std::vector<CleanTarget> GetCleanTargets() {
    wchar_t winDir[MAX_PATH], tempDir[MAX_PATH], userProfile[MAX_PATH], localAppData[MAX_PATH];
    GetWindowsDirectory(winDir, MAX_PATH);
    ExpandEnvironmentStrings(L"%TEMP%", tempDir, MAX_PATH);
    ExpandEnvironmentStrings(L"%USERPROFILE%", userProfile, MAX_PATH);
    ExpandEnvironmentStrings(L"%LOCALAPPDATA%", localAppData, MAX_PATH);

    std::vector<CleanTarget> candidates;

    // 1. Windows Temp
    candidates.push_back({std::wstring(winDir) + L"\\Temp", L"Windows Temp"});

    // 2. User Temp
    candidates.push_back({std::wstring(tempDir), L"User Temp (%TEMP%)"});

    // 3. Prefetch
    candidates.push_back({std::wstring(winDir) + L"\\Prefetch", L"Prefetch"});

    // 4. SoftwareDistribution Download
    candidates.push_back({std::wstring(winDir) + L"\\SoftwareDistribution\\Download", L"SoftwareDistribution"});

    // 5. Chrome Cache — try multiple known paths
    std::wstring chromeCache1 = std::wstring(localAppData) +
        L"\\Google\\Chrome\\User Data\\Default\\Cache\\Cache_Data";
    std::wstring chromeCache2 = std::wstring(localAppData) +
        L"\\Google\\Chrome\\User Data\\Default\\Cache";
    if (DirectoryExists(chromeCache1))
        candidates.push_back({chromeCache1, L"Chrome Cache"});
    else
        candidates.push_back({chromeCache2, L"Chrome Cache"});

    // 6. Edge Cache — try multiple known paths
    std::wstring edgeCache1 = std::wstring(localAppData) +
        L"\\Microsoft\\Edge\\User Data\\Default\\Cache\\Cache_Data";
    std::wstring edgeCache2 = std::wstring(localAppData) +
        L"\\Microsoft\\Edge\\User Data\\Default\\Cache";
    if (DirectoryExists(edgeCache1))
        candidates.push_back({edgeCache1, L"Edge Cache"});
    else
        candidates.push_back({edgeCache2, L"Edge Cache"});

    // 7. Windows Logs (replaces C:\$Recycle.Bin which is a reparse point)
    candidates.push_back({std::wstring(winDir) + L"\\Logs", L"Windows Logs"});

    // Filter: only keep targets where the directory actually exists
    std::vector<CleanTarget> validTargets;
    for (auto& t : candidates) {
        if (DirectoryExists(t.path)) {
            validTargets.push_back(std::move(t));
        }
    }

    return validTargets;
}

static void ScanDirectory(const std::wstring& dirPath, const std::wstring& category,
                          std::vector<CleanerFileInfo>& localFiles,
                          std::atomic<int>& fileCount) {
    // Safety: never scan System32 or Program Files
    std::wstring upper = dirPath;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
    if (upper.find(L"SYSTEM32") != std::wstring::npos) return;
    if (upper.find(L"PROGRAM FILES") != std::wstring::npos) return;

    WIN32_FIND_DATA ffd = {};
    std::wstring searchPath = dirPath + L"\\*";
    HANDLE hFind = FindFirstFile(searchPath.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
            continue;

        // Skip reparse points (junctions, symlinks) to prevent infinite loops
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;

        std::wstring fullPath = dirPath + L"\\" + ffd.cFileName;

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (dirPath.length() > 4) {
                ScanDirectory(fullPath, category, localFiles, fileCount);
            }
        } else {
            ULONGLONG fileSize = ((ULONGLONG)ffd.nFileSizeHigh << 32) | ffd.nFileSizeLow;
            localFiles.push_back({fullPath, category, fileSize, true});
            fileCount.fetch_add(1, std::memory_order_relaxed);
        }
    } while (FindNextFile(hFind, &ffd));

    FindClose(hFind);
}

static void CleanerMasterThread(HWND hwnd) {
    g_cleanerScanning.store(true);

    // Wrap everything in try/catch — if the thread crashes, we MUST still
    // send WM_CLEANER_DONE so the Scan button gets re-enabled.
    try {
        auto startTime = std::chrono::high_resolution_clock::now();

        auto targets = GetCleanTargets();
        int numTargets = (int)targets.size();

        // Update progress bar range to match actual number of valid targets
        PostMessage(g_hwndCleanerProgress, PBM_SETRANGE, 0, MAKELPARAM(0, numTargets > 0 ? numTargets : 1));

        if (numTargets == 0) {
            // No valid targets found — send done immediately
            auto* done = new CleanerProgressData();
            done->filesFound = 0;
            done->directoriesScanned = 0;
            done->scanComplete = true;
            done->scanDurationMs = 0.0;
            done->threadId = GetCurrentThreadId();
            done->timestamp = 0;
            done->fileList = new std::vector<CleanerFileInfo>();
            PostMessage(hwnd, WM_CLEANER_DONE, 0, (LPARAM)done);
            g_cleanerScanning.store(false);
            return;
        }

        std::atomic<int> totalFiles{0};

        // Thread-local vectors — one per target directory
        std::vector<std::vector<CleanerFileInfo>> localVectors(numTargets);

        // Parallel scan using OpenMP
        int ompThreads = (numTargets < 7) ? numTargets : 7;
        #pragma omp parallel for schedule(dynamic, 1) num_threads(ompThreads)
        for (int i = 0; i < numTargets; i++) {
            ScanDirectory(targets[i].path, targets[i].category,
                          localVectors[i], totalFiles);

            // Post progress update
            auto* prog = new CleanerProgressData();
            prog->filesFound = totalFiles.load();
            prog->directoriesScanned = i + 1;
            prog->scanComplete = false;
            prog->threadId = GetCurrentThreadId();
            auto now = std::chrono::high_resolution_clock::now();
            prog->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            prog->fileList = nullptr;
            PostMessage(hwnd, WM_CLEANER_PROGRESS, 0, (LPARAM)prog);
        }

        // Merge all thread-local vectors
        auto* finalList = new std::vector<CleanerFileInfo>();
        for (auto& lv : localVectors) {
            finalList->insert(finalList->end(),
                std::make_move_iterator(lv.begin()),
                std::make_move_iterator(lv.end()));
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        double scanMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        // Post final result
        auto* done = new CleanerProgressData();
        done->filesFound = (int)finalList->size();
        done->directoriesScanned = numTargets;
        done->scanComplete = true;
        done->scanDurationMs = scanMs;
        done->threadId = GetCurrentThreadId();
        auto now = std::chrono::high_resolution_clock::now();
        done->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        done->fileList = finalList;

        PostMessage(hwnd, WM_CLEANER_DONE, 0, (LPARAM)done);
    }
    catch (...) {
        // Crash safety: always re-enable the Scan button
        auto* done = new CleanerProgressData();
        done->filesFound = 0;
        done->directoriesScanned = 0;
        done->scanComplete = true;
        done->scanDurationMs = 0.0;
        done->threadId = GetCurrentThreadId();
        done->timestamp = 0;
        done->fileList = new std::vector<CleanerFileInfo>();
        PostMessage(hwnd, WM_CLEANER_DONE, 0, (LPARAM)done);
    }

    g_cleanerScanning.store(false);
}

static void DeleteFilesThread(HWND hwnd, std::vector<std::wstring> filesToDelete,
                              bool emptyRecycleBin) {
    int deleted = 0, failed = 0;

    // SHEmptyRecycleBin requires COM on the calling thread
    if (emptyRecycleBin) {
        HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        HRESULT hr = SHEmptyRecycleBin(hwnd, NULL,
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        if (SUCCEEDED(hr) || hr == S_FALSE)   // S_FALSE = already empty, still ok
            deleted++;
        else
            failed++;    // will show in final message
        if (SUCCEEDED(hrCom) || hrCom == S_FALSE) CoUninitialize();
    }

    #pragma omp parallel for reduction(+:deleted,failed)
    for (int i = 0; i < (int)filesToDelete.size(); i++) {
        if (DeleteFile(filesToDelete[i].c_str())) deleted++;
        else failed++;
    }

    wchar_t msg[256];
    swprintf_s(msg, L"Deleted %d items. Failed: %d.", deleted, failed);
    MessageBox(hwnd, msg, L"SysForge Cleaner", MB_OK | MB_ICONINFORMATION);
}

// ============================================================================
//  SMART SCAN — Junk File Scanner (7 parallel categories)
// ============================================================================
static ULONGLONG GetRecycleBinSize() {
    SHQUERYRBINFO rbi = {};
    rbi.cbSize = sizeof(rbi);
    if (SHQueryRecycleBin(NULL, &rbi) == S_OK)
        return (ULONGLONG)rbi.i64Size;
    return 0;
}

// Scan a directory for files matching optional extension list (nullptr = all files)
static void SmartScanDir(const std::wstring& dir, const std::wstring& category,
                          const std::vector<std::wstring>& exts,   // empty = all
                          std::vector<CleanerFileInfo>& out,
                          int depth = 0) {
    if (depth > 6) return;
    // Safety guard
    std::wstring up = dir;
    std::transform(up.begin(), up.end(), up.begin(), ::towupper);
    if (up.find(L"SYSTEM32")    != std::wstring::npos) return;
    if (up.find(L"PROGRAM FILES") != std::wstring::npos) return;

    WIN32_FIND_DATA fd = {};
    HANDLE h = FindFirstFile((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            SmartScanDir(full, category, exts, out, depth + 1);
        } else {
            // Extension filter
            if (!exts.empty()) {
                std::wstring name(fd.cFileName);
                std::wstring nameLo = name;
                std::transform(nameLo.begin(), nameLo.end(), nameLo.begin(), ::towlower);
                bool match = false;
                for (auto& e : exts) {
                    if (nameLo.size() >= e.size() &&
                        nameLo.compare(nameLo.size()-e.size(), e.size(), e) == 0) {
                        match = true; break;
                    }
                }
                if (!match) continue;
            }
            ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            out.push_back({full, category, sz, true});
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

// Basic duplicate detection by size then byte comparison
static void FindDuplicates(const std::wstring& dir,
                            std::vector<CleanerFileInfo>& out) {
    // Collect all files with size
    std::vector<std::pair<ULONGLONG, std::wstring>> files;
    WIN32_FIND_DATA fd = {};
    HANDLE h = FindFirstFile((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (sz < 1024) continue; // skip tiny files
        files.push_back({sz, dir + L"\\" + fd.cFileName});
    } while (FindNextFile(h, &fd));
    FindClose(h);

    // Sort by size
    std::sort(files.begin(), files.end());

    // Compare same-size pairs by first 4KB
    for (size_t i = 0; i + 1 < files.size(); i++) {
        if (files[i].first != files[i+1].first) continue;
        // Read first 4096 bytes of each
        auto readHead = [](const std::wstring& p, char* buf) -> bool {
            HANDLE hf = CreateFile(p.c_str(), GENERIC_READ, FILE_SHARE_READ,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf == INVALID_HANDLE_VALUE) return false;
            DWORD rd = 0;
            ReadFile(hf, buf, 4096, &rd, NULL);
            CloseHandle(hf);
            return rd > 0;
        };
        char buf1[4096] = {}, buf2[4096] = {};
        if (readHead(files[i].second, buf1) && readHead(files[i+1].second, buf2)) {
            if (memcmp(buf1, buf2, 4096) == 0) {
                // Keep older (i), flag newer (i+1) as duplicate
                out.push_back({files[i+1].second, L"Duplicate", files[i+1].first, true});
            }
        }
    }
}

static void SmartScanThread(HWND hwnd) {
    try {
        auto t0 = std::chrono::high_resolution_clock::now();

        wchar_t winDir[MAX_PATH], tempDir[MAX_PATH],
                localApp[MAX_PATH], appData[MAX_PATH],
                userProfile[MAX_PATH];
        GetWindowsDirectory(winDir, MAX_PATH);
        ExpandEnvironmentStrings(L"%TEMP%",        tempDir,     MAX_PATH);
        ExpandEnvironmentStrings(L"%LOCALAPPDATA%",localApp,    MAX_PATH);
        ExpandEnvironmentStrings(L"%APPDATA%",     appData,     MAX_PATH);
        ExpandEnvironmentStrings(L"%USERPROFILE%", userProfile, MAX_PATH);

        // 7 scan categories run in parallel threads
        std::vector<CleanerFileInfo> cat[7];
        std::vector<std::wstring> tmpExts  = {L".tmp",L".temp",L".bak",L".old",L".gid",L".chk"};
        std::vector<std::wstring> logExts  = {L".log",L".etl"};
        std::vector<std::wstring> pfExts   = {L".pf"};
        std::vector<std::wstring> dbExts   = {L".db"};
        std::vector<std::wstring> allExts;

        std::thread th0([&]{ SmartScanDir(std::wstring(winDir)+L"\\Temp",
            L"Windows Temp", tmpExts, cat[0]); });
        std::thread th1([&]{ SmartScanDir(tempDir,
            L"User Temp", tmpExts, cat[1]); });
        std::thread th2([&]{
            // Browser caches
            std::vector<std::pair<std::wstring,std::wstring>> browsers = {
                {std::wstring(localApp)+L"\\Google\\Chrome\\User Data\\Default\\Cache\\Cache_Data", L"Chrome Cache"},
                {std::wstring(localApp)+L"\\Microsoft\\Edge\\User Data\\Default\\Cache\\Cache_Data",  L"Edge Cache"},
                {std::wstring(appData) +L"\\Opera Software\\Opera Stable\\Cache\\Cache_Data",         L"Opera Cache"},
            };
            for (auto& b : browsers)
                if (DirectoryExists(b.first))
                    SmartScanDir(b.first, b.second, allExts, cat[2]);
            // Firefox — scan profiles/*/cache2/entries
            std::wstring ffProf = std::wstring(appData)+L"\\Mozilla\\Firefox\\Profiles";
            if (DirectoryExists(ffProf)) {
                WIN32_FIND_DATA fd2={};
                HANDLE hf = FindFirstFile((ffProf+L"\\*").c_str(), &fd2);
                if (hf!=INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        if (wcscmp(fd2.cFileName,L".")==0||wcscmp(fd2.cFileName,L"..")==0) continue;
                        std::wstring cdir = ffProf+L"\\"+fd2.cFileName+L"\\cache2\\entries";
                        if (DirectoryExists(cdir))
                            SmartScanDir(cdir, L"Firefox Cache", allExts, cat[2]);
                    } while(FindNextFile(hf,&fd2));
                    FindClose(hf);
                }
            }
        });
        std::thread th3([&]{
            SmartScanDir(std::wstring(winDir)+L"\\Logs",     L"Windows Logs",   logExts, cat[3]);
            SmartScanDir(std::wstring(winDir)+L"\\Prefetch",  L"Prefetch Cache", pfExts,  cat[3]);
        });
        std::thread th4([&]{
            // Thumbnail cache
            std::wstring thumbDir = std::wstring(localApp)+
                L"\\Microsoft\\Windows\\Explorer";
            if (DirectoryExists(thumbDir)) {
                WIN32_FIND_DATA fd2={};
                HANDLE hf=FindFirstFile((thumbDir+L"\\thumbcache_*.db").c_str(),&fd2);
                if (hf!=INVALID_HANDLE_VALUE) {
                    do {
                        if (fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        ULONGLONG sz=((ULONGLONG)fd2.nFileSizeHigh<<32)|fd2.nFileSizeLow;
                        cat[4].push_back({thumbDir+L"\\"+fd2.cFileName,
                            L"Thumbnail Cache", sz, true});
                    } while(FindNextFile(hf,&fd2));
                    FindClose(hf);
                }
            }
        });
        std::thread th5([&]{
            // Windows Update download cache
            std::wstring wuDir = std::wstring(winDir)+L"\\SoftwareDistribution\\Download";
            if (DirectoryExists(wuDir))
                SmartScanDir(wuDir, L"Update Cache", allExts, cat[5]);
        });
        std::thread th6([&]{
            // Recycle Bin — single summary entry
            ULONGLONG rbSize = GetRecycleBinSize();
            if (rbSize > 0)
                cat[6].push_back({L"::RecycleBin::", L"Recycle Bin", rbSize, true});
            // Duplicates in Downloads + Documents
            FindDuplicates(std::wstring(userProfile)+L"\\Downloads", cat[6]);
            FindDuplicates(std::wstring(userProfile)+L"\\Documents", cat[6]);
        });

        // Post progress messages as threads finish
        th0.join(); th1.join(); th2.join(); th3.join();
        th4.join(); th5.join(); th6.join();

        // Merge all categories
        auto* result = new std::vector<CleanerFileInfo>();
        ULONGLONG totalBytes = 0;
        for (auto& c : cat) {
            for (auto& f : c) {
                totalBytes += f.sizeBytes;
                result->push_back(std::move(f));
            }
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        auto* done = new CleanerProgressData();
        done->filesFound        = (int)result->size();
        done->directoriesScanned = 7;
        done->scanComplete      = true;
        done->scanDurationMs    = ms;
        done->threadId          = GetCurrentThreadId();
        done->timestamp         = 0;
        done->fileList          = result;
        PostMessage(hwnd, WM_SMART_DONE, totalBytes, (LPARAM)done);
    }
    catch (...) {
        auto* done = new CleanerProgressData();
        done->filesFound = 0; done->directoriesScanned = 0;
        done->scanComplete = true; done->scanDurationMs = 0;
        done->threadId = GetCurrentThreadId(); done->timestamp = 0;
        done->fileList = new std::vector<CleanerFileInfo>();
        PostMessage(hwnd, WM_SMART_DONE, 0, (LPARAM)done);
    }
    g_smartScanning.store(false);
}
//  STATIC controls swallow WM_COMMAND from child buttons.
//  Subclass them so all WM_COMMAND and WM_NOTIFY are forwarded to g_hwndMain.
// ============================================================================
static LRESULT CALLBACK PageSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR uIdSubclass,
                                          DWORD_PTR dwRefData) {
    switch (msg) {
    case WM_COMMAND:
        // Forward button clicks etc. to main window
        return SendMessage(g_hwndMain, WM_COMMAND, wParam, lParam);
    case WM_NOTIFY:
        return SendMessage(g_hwndMain, WM_NOTIFY, wParam, lParam);
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, PageSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ============================================================================
//  AGENT 2: GUI CREATION — Tab Pages
// ============================================================================
static HWND CreateLabel(HWND parent, int x, int y, int w, int h, const wchar_t* text) {
    return CreateWindow(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, NULL, g_hInst, NULL);
}

static HWND CreateProgressBar(HWND parent, int x, int y, int w, int h) {
    HWND pb = CreateWindowEx(0, PROGRESS_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        x, y, w, h, parent, NULL, g_hInst, NULL);
    SendMessage(pb, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(pb, PBM_SETPOS, 0, 0);
    SendMessage(pb, PBM_SETBARCOLOR, 0, (LPARAM)C::BarGreen);
    SendMessage(pb, PBM_SETBKCOLOR,  0, (LPARAM)C::BarBg);
    return pb;
}

static void SetFontRecursive(HWND hwnd) {
    auto applyFont = [](HWND h) {
        if (g_hUIFont) SendMessage(h, WM_SETFONT, (WPARAM)g_hUIFont, TRUE);
    };
    applyFont(hwnd);
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        applyFont(child);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

static void ColorizeProgressBar(HWND pb, int value) {
    COLORREF col;
    if      (value >= 85) col = C::BarRed;
    else if (value >= 60) col = C::BarYellow;
    else                  col = C::BarGreen;
    SendMessage(pb, PBM_SETBARCOLOR, 0, (LPARAM)col);
    SendMessage(pb, PBM_SETBKCOLOR,  0, (LPARAM)C::BarBg);
}

// Create a bold section-header label (accent-colored)
static HWND CreateSectionHeader(HWND parent, int x, int y, int w, const wchar_t* text) {
    HWND h = CreateWindow(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, 22, parent, NULL, g_hInst, NULL);
    if (g_hTitleFont) SendMessage(h, WM_SETFONT, (WPARAM)g_hTitleFont, FALSE);
    return h;
}

static void CreateMonitorPage(HWND parent) {
    g_hwndMonitorPage = CreateWindow(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 30, 940, 600, parent, NULL, g_hInst, NULL);
    SetWindowSubclass(g_hwndMonitorPage, PageSubclassProc, 1, 0);

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    g_coreCount = (int)si.dwNumberOfProcessors;
    if (g_coreCount > MAX_CORES) g_coreCount = MAX_CORES;

    // ---- CPU Per-Core section ----
    int yOff = 12;
    CreateSectionHeader(g_hwndMonitorPage, 12, yOff, 300, L"CPU Per-Core Usage");
    yOff += 28;

    int cols = (g_coreCount > 8) ? 2 : 1;
    int rowsPerCol = (g_coreCount + cols - 1) / cols;
    int colW = (g_coreCount > 8) ? 460 : 920;

    for (int i = 0; i < g_coreCount; i++) {
        int col   = i / rowsPerCol;
        int row   = i % rowsPerCol;
        int xBase = 12 + col * colW / cols;
        int yBase = yOff + row * 24;

        wchar_t label[32];
        swprintf_s(label, L"Core %2d", i);
        // 90px wide label so "Core  0: 100%" fits without clipping
        g_cpuLabelX[i] = xBase;
        g_cpuLabels[i] = CreateLabel(g_hwndMonitorPage, xBase, yBase + 2, 90, 18, label);
        if (g_hMonoFont) SendMessage(g_cpuLabels[i], WM_SETFONT, (WPARAM)g_hMonoFont, FALSE);
        g_cpuBarX[i] = xBase + 94;  // bar starts after 90px label + 4px gap
        g_cpuBarY[i] = yBase;
        g_cpuBars[i] = CreateProgressBar(g_hwndMonitorPage, xBase + 94, yBase, colW/cols - 108, 20);
    }
    yOff += rowsPerCol * 24 + 14;

    // ---- CPU Total ----
    CreateSectionHeader(g_hwndMonitorPage, 12, yOff, 300, L"CPU Total");
    yOff += 26;
    g_hwndCpuTotalLabel = CreateLabel(g_hwndMonitorPage, 12, yOff + 2, 90, 18, L"Total: 0%");
    if (g_hMonoFont) SendMessage(g_hwndCpuTotalLabel, WM_SETFONT, (WPARAM)g_hMonoFont, FALSE);
    g_hwndCpuTotalBar = CreateProgressBar(g_hwndMonitorPage, 106, yOff, 820, 22);
    yOff += 36;

    // ---- RAM ----
    CreateSectionHeader(g_hwndMonitorPage, 12, yOff, 300, L"Memory (RAM)");
    yOff += 26;
    g_hwndRamLabel = CreateLabel(g_hwndMonitorPage, 12, yOff + 2, 230, 18, L"RAM: 0 / 0 MB (0%)");
    if (g_hMonoFont) SendMessage(g_hwndRamLabel, WM_SETFONT, (WPARAM)g_hMonoFont, FALSE);
    g_hwndRamBar = CreateProgressBar(g_hwndMonitorPage, 248, yOff, 678, 22);
    yOff += 36;

    // ---- GPU ----
    CreateSectionHeader(g_hwndMonitorPage, 12, yOff, 300, L"GPU (Bonus)");
    yOff += 26;
    g_hwndGpuLabel = CreateLabel(g_hwndMonitorPage, 12, yOff + 2, 140, 18, L"GPU: N/A");
    if (g_hMonoFont) SendMessage(g_hwndGpuLabel, WM_SETFONT, (WPARAM)g_hMonoFont, FALSE);
    g_hwndGpuBar = CreateProgressBar(g_hwndMonitorPage, 156, yOff, 770, 22);
    yOff += 40;

    // ---- Stress Test button ----
    g_hwndStressBtn = CreateWindow(L"BUTTON", L"Run Stress Test  (10s)",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, yOff, 220, 34,
        g_hwndMonitorPage, (HMENU)IDC_STRESS_BTN, g_hInst, NULL);
    if (g_hTitleFont) SendMessage(g_hwndStressBtn, WM_SETFONT, (WPARAM)g_hTitleFont, FALSE);
}

static void CreateControllerPage(HWND parent) {
    g_hwndControllerPage = CreateWindow(L"STATIC", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 30, 900, 560, parent, NULL, g_hInst, NULL);
    SetWindowSubclass(g_hwndControllerPage, PageSubclassProc, 2, 0);

    // Filter
    CreateLabel(g_hwndControllerPage, 10, 10, 50, 22, L"Filter:");
    g_hwndFilterEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        65, 10, 200, 22, g_hwndControllerPage, (HMENU)IDC_FILTER_EDIT, g_hInst, NULL);

    // Buttons
    int bx = 280;
    g_hwndLaunchPathBtn = CreateWindow(L"BUTTON", L"Launch App...",
        WS_CHILD | WS_VISIBLE, bx, 8, 110, 26,
        g_hwndControllerPage, (HMENU)IDC_LAUNCH_PATH_BTN, g_hInst, NULL);
    bx += 115;
    g_hwndSuspendBtn = CreateWindow(L"BUTTON", L"Suspend",
        WS_CHILD | WS_VISIBLE, bx, 8, 80, 26,
        g_hwndControllerPage, (HMENU)IDC_SUSPEND_BTN, g_hInst, NULL);
    bx += 85;
    g_hwndResumeBtn = CreateWindow(L"BUTTON", L"Resume",
        WS_CHILD | WS_VISIBLE, bx, 8, 80, 26,
        g_hwndControllerPage, (HMENU)IDC_RESUME_BTN, g_hInst, NULL);
    bx += 85;
    g_hwndKillBtn = CreateWindow(L"BUTTON", L"Kill",
        WS_CHILD | WS_VISIBLE, bx, 8, 60, 26,
        g_hwndControllerPage, (HMENU)IDC_KILL_BTN, g_hInst, NULL);
    bx += 65;
    g_hwndRefreshBtn = CreateWindow(L"BUTTON", L"Refresh",
        WS_CHILD | WS_VISIBLE, bx, 8, 80, 26,
        g_hwndControllerPage, (HMENU)IDC_REFRESH_BTN, g_hInst, NULL);

    // Process count label
    g_hwndProcCountLabel = CreateLabel(g_hwndControllerPage, 10, 38, 400, 18,
        L"Processes: 0 | Scan: 0ms");

    // ListView for process list
    g_hwndProcessList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        10, 60, 870, 470, g_hwndControllerPage, (HMENU)IDC_PROC_LIST, g_hInst, NULL);

    ListView_SetExtendedListViewStyle(g_hwndProcessList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // Columns: Name, PID, CPU%, RAM (MB), Status
    LVCOLUMN col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.cx = 220; col.pszText = (LPWSTR)L"Process Name"; col.iSubItem = 0;
    ListView_InsertColumn(g_hwndProcessList, 0, &col);
    col.cx = 80; col.pszText = (LPWSTR)L"PID"; col.iSubItem = 1;
    ListView_InsertColumn(g_hwndProcessList, 1, &col);
    col.cx = 80; col.pszText = (LPWSTR)L"CPU %"; col.iSubItem = 2;
    ListView_InsertColumn(g_hwndProcessList, 2, &col);
    col.cx = 120; col.pszText = (LPWSTR)L"RAM (MB)"; col.iSubItem = 3;
    ListView_InsertColumn(g_hwndProcessList, 3, &col);
    col.cx = 100; col.pszText = (LPWSTR)L"Status"; col.iSubItem = 4;
    ListView_InsertColumn(g_hwndProcessList, 4, &col);
}

static void CreateCleanerPage(HWND parent) {
    g_hwndCleanerPage = CreateWindow(L"STATIC", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 30, 900, 560, parent, NULL, g_hInst, NULL);
    SetWindowSubclass(g_hwndCleanerPage, PageSubclassProc, 3, 0);

    g_hwndScanBtn = CreateWindow(L"BUTTON", L"Scan Dirs",
        WS_CHILD | WS_VISIBLE, 10, 10, 100, 30,
        g_hwndCleanerPage, (HMENU)IDC_SCAN_BTN, g_hInst, NULL);

    g_hwndSmartScanBtn = CreateWindow(L"BUTTON", L"Smart Scan",
        WS_CHILD | WS_VISIBLE, 120, 10, 110, 30,
        g_hwndCleanerPage, (HMENU)IDC_SMART_SCAN_BTN, g_hInst, NULL);
    if (g_hTitleFont) SendMessage(g_hwndSmartScanBtn, WM_SETFONT, (WPARAM)g_hTitleFont, FALSE);

    g_hwndDeleteBtn = CreateWindow(L"BUTTON", L"Delete Selected",
        WS_CHILD | WS_VISIBLE, 240, 10, 140, 30,
        g_hwndCleanerPage, (HMENU)IDC_DELETE_BTN, g_hInst, NULL);

    CreateWindow(L"BUTTON", L"Select All",
        WS_CHILD | WS_VISIBLE, 390, 10, 100, 30,
        g_hwndCleanerPage, (HMENU)IDC_SELECT_ALL_BTN, g_hInst, NULL);

    CreateWindow(L"BUTTON", L"Deselect All",
        WS_CHILD | WS_VISIBLE, 500, 10, 110, 30,
        g_hwndCleanerPage, (HMENU)IDC_DESELECT_ALL_BTN, g_hInst, NULL);

    g_hwndCleanerStatus = CreateLabel(g_hwndCleanerPage, 625, 15, 355, 20,
        L"Ready — click Scan Dirs or Smart Scan");

    g_hwndCleanerProgress = CreateProgressBar(g_hwndCleanerPage, 10, 48, 870, 16);
    SendMessage(g_hwndCleanerProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 7)); // 7 dirs

    // ListView for files
    g_hwndCleanerList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        10, 70, 870, 460, g_hwndCleanerPage, (HMENU)IDC_CLEANER_LIST, g_hInst, NULL);

    ListView_SetExtendedListViewStyle(g_hwndCleanerList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMN col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.cx = 400; col.pszText = (LPWSTR)L"File Path"; col.iSubItem = 0;
    ListView_InsertColumn(g_hwndCleanerList, 0, &col);
    col.cx = 150; col.pszText = (LPWSTR)L"Category"; col.iSubItem = 1;
    ListView_InsertColumn(g_hwndCleanerList, 1, &col);
    col.cx = 100; col.pszText = (LPWSTR)L"Size (KB)"; col.iSubItem = 2;
    ListView_InsertColumn(g_hwndCleanerList, 2, &col);
}

// ============================================================================
//  AGENT 2: PROOF PANEL — GDI overlay drawn on WM_PAINT
// ============================================================================
static void DrawProofPanel(HDC hdc, RECT clientRect) {
    if (!g_proofPanelVisible) return;

    // Semi-transparent dark overlay
    RECT panelRect = {clientRect.right - 380, 30, clientRect.right, clientRect.bottom};
    HBRUSH bgBrush = CreateSolidBrush(RGB(20, 20, 30));
    FillRect(hdc, &panelRect, bgBrush);
    DeleteObject(bgBrush);

    // Border
    HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(0, 200, 100));
    SelectObject(hdc, borderPen);
    MoveToEx(hdc, panelRect.left, panelRect.top, NULL);
    LineTo(hdc, panelRect.left, panelRect.bottom);
    DeleteObject(borderPen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 255, 120));

    HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    int x = panelRect.left + 10;
    int y = panelRect.top + 10;
    int lineH = 18;

    auto drawLine = [&](const wchar_t* text) {
        TextOut(hdc, x, y, text, (int)wcslen(text));
        y += lineH;
    };

    drawLine(L"=== SYSFORGE PROOF PANEL (F2) ===");
    y += 5;

    SetTextColor(hdc, RGB(255, 255, 100));
    drawLine(L"--- Monitor Thread ---");
    SetTextColor(hdc, RGB(200, 230, 255));

    wchar_t buf[256];

    swprintf_s(buf, L"Thread ID: %lu", g_lastMonitor.threadId);
    drawLine(buf);

    swprintf_s(buf, L"Last Wake: %lld ms", g_lastMonitor.timestamp);
    drawLine(buf);

    swprintf_s(buf, L"PDH Handle: 0x%p", (void*)g_lastMonitor.queryHandle);
    drawLine(buf);

    swprintf_s(buf, L"CPU Total (raw): %.4f%%", g_lastMonitor.cpuTotal);
    drawLine(buf);

    for (int i = 0; i < g_lastMonitor.coreCount && i < 8; i++) {
        swprintf_s(buf, L"  Core %d (raw): %.4f%%", i, g_lastMonitor.cpuPerCore[i]);
        drawLine(buf);
    }
    if (g_lastMonitor.coreCount > 8) {
        swprintf_s(buf, L"  ... +%d more cores", g_lastMonitor.coreCount - 8);
        drawLine(buf);
    }

    swprintf_s(buf, L"RAM: %.0f / %.0f MB", g_lastMonitor.ramUsedMB, g_lastMonitor.ramTotalMB);
    drawLine(buf);

    swprintf_s(buf, L"GPU: %.2f%%", g_lastMonitor.gpuUsage);
    drawLine(buf);

    y += 8;
    SetTextColor(hdc, RGB(255, 255, 100));
    drawLine(L"--- Process Scan Thread ---");
    SetTextColor(hdc, RGB(200, 230, 255));

    swprintf_s(buf, L"Thread ID: %lu", g_lastProcData.threadId);
    drawLine(buf);

    swprintf_s(buf, L"Last Wake: %lld ms", g_lastProcData.timestamp);
    drawLine(buf);

    swprintf_s(buf, L"PID Count: %d", g_lastProcData.pidCount);
    drawLine(buf);

    swprintf_s(buf, L"Scan Duration: %.2f ms", g_lastProcData.scanDurationMs);
    drawLine(buf);

    y += 8;
    SetTextColor(hdc, RGB(255, 255, 100));
    drawLine(L"--- Stress Test ---");
    SetTextColor(hdc, RGB(200, 230, 255));
    swprintf_s(buf, L"Active: %s", g_stressRunning.load() ? L"YES (running)" : L"No");
    drawLine(buf);

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}

// ============================================================================
//  AGENT 2: GUI UPDATE HANDLERS (Consumers)
// ============================================================================
static void HandleMonitorData(MonitorData* data) {
    // Save for proof panel
    g_lastMonitor = *data;

    // Update per-core bars with color coding
    for (int i = 0; i < data->coreCount && i < g_coreCount; i++) {
        int val = (int)(data->cpuPerCore[i] + 0.5);
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        ColorizeProgressBar(g_cpuBars[i], val);
        SendMessage(g_cpuBars[i], PBM_SETPOS, val, 0);

        wchar_t label[64];
        swprintf_s(label, L"Core %d: %d%%", i, val);
        SetWindowText(g_cpuLabels[i], label);
    }

    // CPU Total with color
    int totalVal = (int)(data->cpuTotal + 0.5);
    if (totalVal < 0) totalVal = 0;
    if (totalVal > 100) totalVal = 100;
    ColorizeProgressBar(g_hwndCpuTotalBar, totalVal);
    SendMessage(g_hwndCpuTotalBar, PBM_SETPOS, totalVal, 0);
    wchar_t totalLabel[64];
    swprintf_s(totalLabel, L"Total: %d%%", totalVal);
    SetWindowText(g_hwndCpuTotalLabel, totalLabel);

    // RAM with color
    int ramPercent = (int)((data->ramUsedMB / data->ramTotalMB) * 100.0);
    if (ramPercent > 100) ramPercent = 100;
    ColorizeProgressBar(g_hwndRamBar, ramPercent);
    SendMessage(g_hwndRamBar, PBM_SETPOS, ramPercent, 0);
    wchar_t ramLabel[128];
    swprintf_s(ramLabel, L"RAM: %.0f / %.0f MB (%d%%)",
        data->ramUsedMB, data->ramTotalMB, ramPercent);
    SetWindowText(g_hwndRamLabel, ramLabel);

    // GPU
    int gpuVal = (int)(data->gpuUsage + 0.5);
    if (gpuVal > 100) gpuVal = 100;
    ColorizeProgressBar(g_hwndGpuBar, gpuVal);
    SendMessage(g_hwndGpuBar, PBM_SETPOS, gpuVal, 0);
    wchar_t gpuLabel[64];
    swprintf_s(gpuLabel, L"GPU: %d%%", gpuVal);
    SetWindowText(g_hwndGpuLabel, gpuLabel);

    // Repaint proof panel if visible
    if (g_proofPanelVisible) {
        InvalidateRect(g_hwndMain, NULL, FALSE);
    }

    delete data;
}

static void HandleProcessData(ProcessListData* data) {
    g_lastProcData = *data;

    // Build filtered list
    wchar_t filterBuf[256] = {};
    GetWindowText(g_hwndFilterEdit, filterBuf, 256);
    std::wstring filter(filterBuf);
    std::wstring filterLow = filter;
    std::transform(filterLow.begin(), filterLow.end(), filterLow.begin(), ::towlower);

    std::vector<ProcessInfo> filtered;
    for (auto& p : data->procs) {
        if (!filter.empty()) {
            std::wstring name(p.name);
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name.find(filterLow) == std::wstring::npos) continue;
        }
        filtered.push_back(p);
    }

    // ----- IN-PLACE UPDATE — no full delete/reinsert (critical for responsiveness) -----
    SendMessage(g_hwndProcessList, WM_SETREDRAW, FALSE, 0);

    int existing = ListView_GetItemCount(g_hwndProcessList);
    int needed   = (int)filtered.size();

    // Remove excess rows
    for (int i = existing - 1; i >= needed; i--)
        ListView_DeleteItem(g_hwndProcessList, i);

    // Add missing rows
    for (int i = existing; i < needed; i++) {
        LVITEM lvi = {};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = i;
        lvi.pszText = (LPWSTR)L"";
        ListView_InsertItem(g_hwndProcessList, &lvi);
    }

    g_currentProcs = filtered;
    int idx = 0;
    wchar_t buf[128];

    for (auto& p : filtered) {
        ListView_SetItemText(g_hwndProcessList, idx, 0, p.name);
        swprintf_s(buf, L"%lu", p.pid);
        ListView_SetItemText(g_hwndProcessList, idx, 1, buf);
        swprintf_s(buf, L"%.1f", p.cpuPercent);
        ListView_SetItemText(g_hwndProcessList, idx, 2, buf);
        swprintf_s(buf, L"%.1f", p.ramKB / 1024.0);
        ListView_SetItemText(g_hwndProcessList, idx, 3, buf);
        ListView_SetItemText(g_hwndProcessList, idx, 4, (LPWSTR)L"Running");
        idx++;
    }

    SendMessage(g_hwndProcessList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hwndProcessList, NULL, TRUE);

    // Update label
    wchar_t labelBuf[256];
    swprintf_s(labelBuf, L"Processes: %d | Total PIDs: %d | Scan: %.1fms",
        idx, data->pidCount, data->scanDurationMs);
    SetWindowText(g_hwndProcCountLabel, labelBuf);

    if (g_proofPanelVisible) {
        InvalidateRect(g_hwndMain, NULL, FALSE);
    }

    delete data;
}

static void HandleCleanerProgress(CleanerProgressData* data) {
    wchar_t buf[256];
    swprintf_s(buf, L"Scanning... %d files found (%d/7 directories)",
        data->filesFound, data->directoriesScanned);
    SetWindowText(g_hwndCleanerStatus, buf);
    SendMessage(g_hwndCleanerProgress, PBM_SETPOS, data->directoriesScanned, 0);
    delete data;
}

static void HandleCleanerDone(CleanerProgressData* data) {
    wchar_t buf[256];
    swprintf_s(buf, L"Scan complete: %d files found in %.1fms (Thread: %lu)",
        data->filesFound, data->scanDurationMs, data->threadId);
    SetWindowText(g_hwndCleanerStatus, buf);
    SendMessage(g_hwndCleanerProgress, PBM_SETPOS, 7, 0);

    // Populate the list view
    if (data->fileList) {
        g_cleanerFiles = std::move(*(data->fileList));
        delete data->fileList;

        SendMessage(g_hwndCleanerList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g_hwndCleanerList);

        for (int i = 0; i < (int)g_cleanerFiles.size() && i < 5000; i++) {
            LVITEM lvi = {};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = i;
            lvi.pszText = (LPWSTR)g_cleanerFiles[i].path.c_str();
            ListView_InsertItem(g_hwndCleanerList, &lvi);

            ListView_SetItemText(g_hwndCleanerList, i, 1,
                (LPWSTR)g_cleanerFiles[i].category.c_str());

            wchar_t sizeBuf[32];
            swprintf_s(sizeBuf, L"%.1f", g_cleanerFiles[i].sizeBytes / 1024.0);
            ListView_SetItemText(g_hwndCleanerList, i, 2, sizeBuf);

            ListView_SetCheckState(g_hwndCleanerList, i, TRUE);
        }

        SendMessage(g_hwndCleanerList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hwndCleanerList, NULL, TRUE);
    }

    delete data;
}

// ============================================================================
//  AGENT 2: TAB SWITCHING
// ============================================================================
static void SwitchTab(int tabIndex) {
    ShowWindow(g_hwndMonitorPage,    (tabIndex == 0) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hwndControllerPage, (tabIndex == 1) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hwndCleanerPage,    (tabIndex == 2) ? SW_SHOW : SW_HIDE);
}

// ============================================================================
//  AGENT 2: MAIN WINDOW PROCEDURE
// ============================================================================
//  RESPONSIVE LAYOUT  — called on WM_SIZE and WM_CREATE
//  Reflows every control to fill the available client area.
// ============================================================================
static void DoLayout(HWND hwnd) {
    if (!g_hwndTab) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right;
    int H = rc.bottom;

    // Status bar sits at the bottom, auto-sizes itself
    if (g_hwndStatus) SendMessage(g_hwndStatus, WM_SIZE, 0, 0);
    int statusH = 22;

    // Tab fills everything above status bar
    int tabH = H - statusH;
    SetWindowPos(g_hwndTab, NULL, 0, 0, W, tabH, SWP_NOZORDER | SWP_NOACTIVATE);

    // Usable content area inside the tab control
    // Tab header is ~26px tall, tab border is 2px each side
    int iX = 4;
    int iY = 28;       // below tab header row
    int iW = W - 8;   // left+right border
    int iH = tabH - iY - 4;

    // All three page containers fill the same slot
    auto resizePage = [&](HWND pg) {
        if (pg) SetWindowPos(pg, NULL, iX, iY, iW, iH, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    resizePage(g_hwndMonitorPage);
    resizePage(g_hwndControllerPage);
    resizePage(g_hwndCleanerPage);

    // ---- Controller: stretch ListView ----
    if (g_hwndProcessList) {
        int listH = iH - 62;
        if (listH < 50) listH = 50;
        SetWindowPos(g_hwndProcessList, NULL, 10, 60, iW - 20, listH,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // ---- Cleaner: stretch list + progress bar ----
    if (g_hwndCleanerProgress)
        SetWindowPos(g_hwndCleanerProgress, NULL, 10, 48, iW - 20, 16, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hwndCleanerList) {
        int listH = iH - 72;
        if (listH < 50) listH = 50;
        SetWindowPos(g_hwndCleanerList, NULL, 10, 70, iW - 20, listH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // ---- Monitor: stretch bars by WIDTH ONLY (keep stored Y) ----
    // CPU Total bar: label is 90px wide at x=12, bar starts at x=106
    if (g_hwndCpuTotalBar) {
        int barW = iW - 106 - 12;
        if (barW < 80) barW = 80;
        RECT br; GetWindowRect(g_hwndCpuTotalBar, &br);
        POINT pt = { br.left, br.top };
        ScreenToClient(g_hwndMonitorPage, &pt);
        SetWindowPos(g_hwndCpuTotalBar, NULL, pt.x, pt.y, barW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_hwndRamBar) {
        int barW = iW - 248 - 12;
        if (barW < 80) barW = 80;
        RECT br; GetWindowRect(g_hwndRamBar, &br);
        POINT pt = { br.left, br.top };
        ScreenToClient(g_hwndMonitorPage, &pt);
        SetWindowPos(g_hwndRamBar, NULL, pt.x, pt.y, barW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_hwndGpuBar) {
        int barW = iW - 156 - 12;
        if (barW < 80) barW = 80;
        RECT br; GetWindowRect(g_hwndGpuBar, &br);
        POINT pt = { br.left, br.top };
        ScreenToClient(g_hwndMonitorPage, &pt);
        SetWindowPos(g_hwndGpuBar, NULL, pt.x, pt.y, barW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Per-core bars AND labels: recalculate column X, keep stored Y
    if (g_coreCount > 0) {
        int cols       = (g_coreCount > 8) ? 2 : 1;
        int rowsPerCol = (g_coreCount + cols - 1) / cols;
        int colW       = (iW - 12) / cols;  // new column width
        for (int i = 0; i < g_coreCount; i++) {
            if (!g_cpuBars[i]) continue;
            int col   = i / rowsPerCol;
            int xBase = 12 + col * colW;    // new column x origin
            int barW  = colW - 108;         // bar width = colW minus label(90) + gap(4) + margin(14)
            if (barW < 40) barW = 40;
            int y = g_cpuBarY[i];
            // Move label to new column x
            SetWindowPos(g_cpuLabels[i], NULL, xBase,      y + 2, 90,   18, SWP_NOZORDER | SWP_NOACTIVATE);
            // Move bar to follow label
            SetWindowPos(g_cpuBars[i],  NULL, xBase + 94, y,     barW, 20, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

// ============================================================================
//  AGENT 2: MAIN WINDOW PROCEDURE
// ============================================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        // Init common controls
        INITCOMMONCONTROLSEX icex = {};
        icex.dwSize = sizeof(icex);
        icex.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&icex);

        // Tab control (initial size — DoLayout will resize on WM_SIZE)
        g_hwndTab = CreateWindow(WC_TABCONTROL, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_HOTTRACK,
            0, 0, 980, 700, hwnd, (HMENU)IDC_TAB, g_hInst, NULL);
        if (g_hTitleFont) SendMessage(g_hwndTab, WM_SETFONT, (WPARAM)g_hTitleFont, FALSE);

        TCITEM tie = {};
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPWSTR)L"  Monitor  ";
        TabCtrl_InsertItem(g_hwndTab, 0, &tie);
        tie.pszText = (LPWSTR)L"  Controller  ";
        TabCtrl_InsertItem(g_hwndTab, 1, &tie);
        tie.pszText = (LPWSTR)L"  Cleaner  ";
        TabCtrl_InsertItem(g_hwndTab, 2, &tie);

        // Create pages
        CreateMonitorPage(g_hwndTab);
        CreateControllerPage(g_hwndTab);
        CreateCleanerPage(g_hwndTab);
        SwitchTab(0);

        // Status bar
        g_hwndStatus = CreateWindow(STATUSCLASSNAME,
            L"SysForge Ready  |  F2 → Proof Panel  |  Running as Administrator",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, NULL, g_hInst, NULL);

        // Acquire debug privilege
        AcquireDebugPrivilege();

        // Start background threads
        std::thread(MonitorThread, hwnd).detach();
        std::thread(ProcessScanThread, hwnd).detach();

        return 0;
    }

    case WM_NOTIFY: {
        NMHDR* nmhdr = (NMHDR*)lParam;
        if (nmhdr->hwndFrom == g_hwndTab && nmhdr->code == TCN_SELCHANGE) {
            SwitchTab(TabCtrl_GetCurSel(g_hwndTab));
        }

        // Alternating row colors for ListViews (professional look)
        if (nmhdr->code == NM_CUSTOMDRAW) {
            LPNMLVCUSTOMDRAW lpcd = (LPNMLVCUSTOMDRAW)lParam;
            if (nmhdr->hwndFrom == g_hwndProcessList || nmhdr->hwndFrom == g_hwndCleanerList) {
                switch (lpcd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    if (lpcd->nmcd.dwItemSpec % 2 == 0) {
                        lpcd->clrTextBk = RGB(240, 245, 255); // Light blue
                        lpcd->clrText   = RGB(20, 20, 40);
                    } else {
                        lpcd->clrTextBk = RGB(255, 255, 255);
                        lpcd->clrText   = RGB(20, 20, 40);
                    }
                    return CDRF_NEWFONT;
                }
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        switch (id) {

        case IDC_STRESS_BTN:
            if (!g_stressRunning.load()) {
                EnableWindow(g_hwndStressBtn, FALSE);
                SetWindowText(g_hwndStressBtn, L"Stress Running...");
                std::thread(StressTestThread, hwnd).detach();
            }
            break;

        case IDC_LAUNCH_BTN:
            std::thread([](){ LaunchProcess(L"C:\\Windows\\System32\\notepad.exe"); }).detach();
            break;

        case IDC_LAUNCH_PATH_BTN:
            LaunchProcessFromDialog(hwnd);
            break;

        case IDC_SUSPEND_BTN: {
            int sel = ListView_GetNextItem(g_hwndProcessList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < (int)g_currentProcs.size()) {
                DWORD pid = g_currentProcs[sel].pid;
                if (IsCriticalProcess(pid, g_currentProcs[sel].name)) {
                    MessageBox(hwnd, L"Cannot suspend a critical system process!",
                        L"SysForge", MB_OK | MB_ICONWARNING);
                } else {
                    std::thread([pid]() { SuspendProcess(pid); }).detach();
                }
            }
            break;
        }

        case IDC_RESUME_BTN: {
            int sel = ListView_GetNextItem(g_hwndProcessList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < (int)g_currentProcs.size()) {
                DWORD pid = g_currentProcs[sel].pid;
                std::thread([pid]() { ResumeProcess(pid); }).detach();
            }
            break;
        }

        case IDC_KILL_BTN: {
            int sel = ListView_GetNextItem(g_hwndProcessList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < (int)g_currentProcs.size()) {
                DWORD pid = g_currentProcs[sel].pid;
                if (IsCriticalProcess(pid, g_currentProcs[sel].name)) {
                    MessageBox(hwnd, L"Cannot kill a critical system process!",
                        L"SysForge", MB_OK | MB_ICONWARNING);
                } else {
                    wchar_t confirmMsg[256];
                    swprintf_s(confirmMsg, L"Kill %s (PID %lu)?",
                        g_currentProcs[sel].name, pid);
                    if (MessageBox(hwnd, confirmMsg, L"Confirm Kill",
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        std::thread([pid]() { KillProcess(pid); }).detach();
                    }
                }
            }
            break;
        }

        case IDC_SCAN_BTN:
            if (!g_cleanerScanning.load()) {
                EnableWindow(g_hwndScanBtn, FALSE);
                SetWindowText(g_hwndCleanerStatus, L"Scanning directories...");
                SendMessage(g_hwndCleanerProgress, PBM_SETPOS, 0, 0);
                ListView_DeleteAllItems(g_hwndCleanerList);
                g_cleanerFiles.clear();
                std::thread(CleanerMasterThread, hwnd).detach();
            }
            break;

        case IDC_SMART_SCAN_BTN:
            if (!g_smartScanning.load()) {
                g_smartScanning.store(true);
                EnableWindow(g_hwndSmartScanBtn, FALSE);
                SetWindowText(g_hwndCleanerStatus,
                    L"Smart Scan running (7 threads)...");
                SendMessage(g_hwndCleanerProgress, PBM_SETPOS, 0, 0);
                ListView_DeleteAllItems(g_hwndCleanerList);
                g_cleanerFiles.clear();
                std::thread(SmartScanThread, hwnd).detach();
            }
            break;

        case IDC_DELETE_BTN: {
            if (g_cleanerFiles.empty()) break;

            std::vector<std::wstring> toDelete;
            bool hasRecycleBin = false;
            int count = ListView_GetItemCount(g_hwndCleanerList);
            for (int i = 0; i < count; i++) {
                if (ListView_GetCheckState(g_hwndCleanerList, i)) {
                    if (i < (int)g_cleanerFiles.size()) {
                        if (g_cleanerFiles[i].category == L"Recycle Bin")
                            hasRecycleBin = true;
                        else
                            toDelete.push_back(g_cleanerFiles[i].path);
                    }
                }
            }

            if (toDelete.empty() && !hasRecycleBin) break;

            wchar_t confirmMsg[160];
            swprintf_s(confirmMsg, L"Delete %d file(s)%s?",
                (int)toDelete.size(),
                hasRecycleBin ? L" + empty Recycle Bin" : L"");
            if (MessageBox(hwnd, confirmMsg, L"Confirm Delete",
                MB_YESNO | MB_ICONWARNING) == IDYES) {
                std::thread(DeleteFilesThread, hwnd,
                    std::move(toDelete), hasRecycleBin).detach();
            }
            break;
        }

        case IDC_SELECT_ALL_BTN: {
            int count = ListView_GetItemCount(g_hwndCleanerList);
            for (int i = 0; i < count; i++) {
                ListView_SetCheckState(g_hwndCleanerList, i, TRUE);
            }
            break;
        }

        case IDC_DESELECT_ALL_BTN: {
            int count = ListView_GetItemCount(g_hwndCleanerList);
            for (int i = 0; i < count; i++) {
                ListView_SetCheckState(g_hwndCleanerList, i, FALSE);
            }
            break;
        }

        } // end switch id
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_F2) {
            g_proofPanelVisible = !g_proofPanelVisible;
            InvalidateRect(hwnd, NULL, TRUE);

            wchar_t statusText[128];
            swprintf_s(statusText, L"Proof Panel: %s | Press F2 to toggle",
                g_proofPanelVisible ? L"VISIBLE" : L"HIDDEN");
            SetWindowText(g_hwndStatus, statusText);
        }
        return 0;

    // --- Producer-Consumer message handlers ---
    case WM_MONITOR_DATA:
        HandleMonitorData((MonitorData*)lParam);
        return 0;

    case WM_PROCESS_DATA:
        HandleProcessData((ProcessListData*)lParam);
        return 0;

    case WM_CLEANER_PROGRESS:
        HandleCleanerProgress((CleanerProgressData*)lParam);
        return 0;

    case WM_CLEANER_DONE:
        HandleCleanerDone((CleanerProgressData*)lParam);
        EnableWindow(g_hwndScanBtn, TRUE);
        return 0;

    case WM_SMART_DONE: {
        // Re-use HandleCleanerDone to populate the ListView
        CleanerProgressData* sd = (CleanerProgressData*)lParam;
        ULONGLONG totalBytes = (ULONGLONG)wParam;  // packed by SmartScanThread
        if (sd->fileList) {
            g_cleanerFiles = *sd->fileList;
            delete sd->fileList;
            sd->fileList = nullptr;
        }
        SendMessage(g_hwndCleanerList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g_hwndCleanerList);
        for (int i = 0; i < (int)g_cleanerFiles.size(); i++) {
            LVITEM lvi = {};
            lvi.mask    = LVIF_TEXT;
            lvi.iItem   = i;
            lvi.pszText = (LPWSTR)g_cleanerFiles[i].path.c_str();
            ListView_InsertItem(g_hwndCleanerList, &lvi);
            ListView_SetItemText(g_hwndCleanerList, i, 1,
                (LPWSTR)g_cleanerFiles[i].category.c_str());
            wchar_t szBuf[32];
            swprintf_s(szBuf, L"%.1f", g_cleanerFiles[i].sizeBytes / 1024.0);
            ListView_SetItemText(g_hwndCleanerList, i, 2, szBuf);
            ListView_SetCheckState(g_hwndCleanerList, i, TRUE);
        }
        SendMessage(g_hwndCleanerList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hwndCleanerList, NULL, TRUE);
        wchar_t statusBuf[256];
        swprintf_s(statusBuf,
            L"Smart Scan: %d junk items found  |  %.1f MB total  |  %.0f ms",
            (int)g_cleanerFiles.size(),
            totalBytes / (1024.0 * 1024.0),
            sd->scanDurationMs);
        SetWindowText(g_hwndCleanerStatus, statusBuf);
        EnableWindow(g_hwndSmartScanBtn, TRUE);
        delete sd;
        return 0;
    }

    case WM_STRESS_DONE:
        EnableWindow(g_hwndStressBtn, TRUE);
        SetWindowText(g_hwndStressBtn, L"Stress Test (10s)");
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_proofPanelVisible) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            DrawProofPanel(hdc, rc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: {
        // Paint the background in the luxury palette color
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_brBgPage) FillRect(hdc, &rc, g_brBgPage);
        else FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW+1));
        return 1; // tell Windows we handled it
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        HWND hCtrl    = (HWND)lParam;
        // Section headers (TitleFont controls) get accent blue text
        HFONT ctrlFont = (HFONT)SendMessage(hCtrl, WM_GETFONT, 0, 0);
        if (ctrlFont == g_hTitleFont) {
            SetTextColor(hdcStatic, C::Accent);
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)g_brBgPage;
        }
        // Mono-font labels (data readouts) get primary text color
        if (ctrlFont == g_hMonoFont) {
            SetTextColor(hdcStatic, C::TextPrimary);
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)g_brBgPage;
        }
        // All other statics
        SetTextColor(hdcStatic, C::TextMuted);
        SetBkMode(hdcStatic, TRANSPARENT);
        return (LRESULT)g_brBgPage;
    }

    case WM_SIZE:
        DoLayout(hwnd);
        return 0;

    case WM_CLOSE:
        // Graceful shutdown: stop all threads before destroying window
        g_monitorRunning.store(false);
        g_processRunning.store(false);
        g_stressRunning.store(false);
        g_cleanerScanning.store(false);
        Sleep(600); // Let threads wind down (> polling interval)
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// ============================================================================
//  ENTRY POINT
// ============================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInst = hInstance;

    // --- Create luxury font set ---
    g_hUIFont    = CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_hTitleFont = CreateFont(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_hMonoFont  = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH,   L"Cascadia Mono");
    // Fallback mono font if Cascadia not installed
    if (!g_hMonoFont)
        g_hMonoFont = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    // --- Brush cache ---
    g_brBgPage  = CreateSolidBrush(C::BgPage);
    g_brBgCard  = CreateSolidBrush(C::BgCard);
    g_brAccentL = CreateSolidBrush(C::AccentLight);

    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_brBgPage;
    wc.lpszClassName = L"SysForgeClass";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassEx(&wc);

    // Create main window
    g_hwndMain = CreateWindowEx(
        0, L"SysForgeClass", L"SysForge  \x2014  Parallel Windows System Utility  |  PDC Lab",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 700,
        NULL, NULL, hInstance, NULL);

    ShowWindow(g_hwndMain, nCmdShow);
    SetFontRecursive(g_hwndMain);
    UpdateWindow(g_hwndMain);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        // Ensure F2 is captured even when child controls have focus
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F2) {
            SendMessage(g_hwndMain, WM_KEYDOWN, VK_F2, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
