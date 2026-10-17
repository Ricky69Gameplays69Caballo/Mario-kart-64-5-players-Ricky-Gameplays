#include "CrashMonitor.h"
#ifdef _WIN32
#include <windows.h>
#include <DbgHelp.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>
#include <libultraship.h>
#pragma comment(lib, "Dbghelp.lib")

namespace {
HANDLE sMonitorProcess = nullptr;
HANDLE sMonitorStop = nullptr;
bool sMonitorRequested = false;

std::wstring StopEventName(DWORD pid) {
    return L"Local\\SpaghettiKartCrashMonitorStop-" + std::to_wstring(pid);
}

std::wstring Utf8ToWide(const char* value) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (length <= 0) return {};
    std::vector<wchar_t> text(length);
    MultiByteToWideChar(CP_UTF8, 0, value, -1, text.data(), length);
    return text.data();
}

void WriteCrash(HANDLE process, const DEBUG_EVENT& event, const std::filesystem::path& directory,
                std::ofstream& monitorLog) {
    SYSTEMTIME time;
    GetSystemTime(&time);
    wchar_t filename[160];
    swprintf_s(filename, L"crash-%04u%02u%02u-%02u%02u%02u-%03u-p%lu-external", time.wYear,
               time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
               event.dwProcessId);
    const auto prefix = directory / filename;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId);
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    const bool hasContext = thread && GetThreadContext(thread, &context);
    EXCEPTION_RECORD record = event.u.Exception.ExceptionRecord;
    EXCEPTION_POINTERS pointers{&record, &context};
    MINIDUMP_EXCEPTION_INFORMATION info{event.dwThreadId, &pointers, FALSE};
    HANDLE dump = CreateFileW((prefix.wstring() + L".dmp").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    BOOL written = FALSE;
    DWORD error = GetLastError();
    if (dump != INVALID_HANDLE_VALUE) {
        // Capture from the healthy helper, including heap/array data and all thread stacks.
        const auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithPrivateReadWriteMemory |
            MiniDumpWithFullMemoryInfo | MiniDumpWithUnloadedModules);
        written = MiniDumpWriteDump(process, event.dwProcessId, dump, flags, hasContext ? &info : nullptr,
                                    nullptr, nullptr);
        error = written ? ERROR_SUCCESS : GetLastError();
        FlushFileBuffers(dump);
        CloseHandle(dump);
    }
    std::ofstream report(std::filesystem::path(prefix.wstring() + L".txt"));
    report << "Combined external Windows crash capture\nUTC " << time.wYear << '-' << time.wMonth
           << '-' << time.wDay << ' ' << time.wHour << ':' << time.wMinute << ':' << time.wSecond
           << "\nprocess=" << event.dwProcessId << " thread=" << event.dwThreadId
           << "\nexception=0x" << std::hex << record.ExceptionCode << " address=" << record.ExceptionAddress
           << " firstChance=" << std::dec << event.u.Exception.dwFirstChance
           << "\ndump=" << (written ? "written" : "failed") << " error=" << error
           << " context=" << hasContext << '\n';
    for (DWORD i = 0; i < record.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
        report << "exception_parameter[" << i << "]=0x" << std::hex << record.ExceptionInformation[i] << '\n';
    }
    // The PDB next to the game supplies function names and source line numbers.
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    const std::string searchPath = directory.parent_path().string();
    if (hasContext && SymInitialize(process, searchPath.c_str(), TRUE)) {
        STACKFRAME64 frame{};
        CONTEXT walk = context;
#ifdef _M_ARM64
        const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
        frame.AddrPC.Offset = walk.Pc;
        frame.AddrStack.Offset = walk.Sp;
        frame.AddrFrame.Offset = walk.Fp;
#elif defined(_WIN64)
        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = walk.Rip;
        frame.AddrStack.Offset = walk.Rsp;
        frame.AddrFrame.Offset = walk.Rbp;
#else
        const DWORD machine = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset = walk.Eip;
        frame.AddrStack.Offset = walk.Esp;
        frame.AddrFrame.Offset = walk.Ebp;
#endif
        frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
        report << "\nStack trace:\n";
        std::vector<unsigned char> storage(sizeof(SYMBOL_INFO) + MAX_SYM_NAME);
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 previous = 0;
        for (int index = 0; index < 64; ++index) {
            if (!StackWalk64(machine, process, thread, &frame, &walk, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            if (!frame.AddrPC.Offset || frame.AddrPC.Offset == previous) break;
            previous = frame.AddrPC.Offset;
            DWORD64 displacement = 0;
            report << std::dec << index << ": 0x" << std::hex << frame.AddrPC.Offset;
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
                report << ' ' << symbol->Name << "+0x" << displacement;
            }
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line)) {
                report << " [" << line.FileName << ':' << std::dec << line.LineNumber << ']';
            }
            report << '\n';
        }
        SymCleanup(process);
    }
    report.flush();
    if (thread) CloseHandle(thread);
    monitorLog << "Captured crash: " << prefix.string() << "; dump=" << written << "; error=" << error << std::endl;
}
}

int CrashMonitor_Run(unsigned long processId, const char* logDirectory) {
    const std::filesystem::path directory(Utf8ToWide(logDirectory));
    std::filesystem::create_directories(directory);
    std::ofstream log(directory / ("crash-monitor-p" + std::to_string(processId) + ".log"));
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, StopEventName(processId).c_str());
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, processId);
    if (!process || !stop || !DebugActiveProcess(processId)) {
        log << "Unable to attach crash monitor; Windows error=" << GetLastError() << std::endl;
        if (process) CloseHandle(process);
        if (stop) CloseHandle(stop);
        return 1;
    }
    DebugSetProcessKillOnExit(FALSE);
    log << "Crash monitor attached to process " << processId << ". Ready to capture failures including 0xc0000409." << std::endl;
    bool captured = false;
    bool exited = false;
    for (;;) {
        DEBUG_EVENT event{};
        if (WaitForDebugEvent(&event, 250)) {
            DWORD action = DBG_CONTINUE;
            if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
                const DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
                const bool fatal = !event.u.Exception.dwFirstChance || code == 0xC0000409;
                if (fatal && !captured) {
                    captured = true;
                    WriteCrash(process, event, directory, log);
                }
                // Consume only the initial debugger breakpoint; pass game exceptions on.
                if (code != EXCEPTION_BREAKPOINT || fatal) action = DBG_EXCEPTION_NOT_HANDLED;
            } else if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
                if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                if (event.u.CreateProcessInfo.hProcess) CloseHandle(event.u.CreateProcessInfo.hProcess);
                if (event.u.CreateProcessInfo.hThread) CloseHandle(event.u.CreateProcessInfo.hThread);
            } else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
                if (event.u.CreateThread.hThread) CloseHandle(event.u.CreateThread.hThread);
            } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
                log << "Game exited; code=" << event.u.ExitProcess.dwExitCode << std::endl;
                exited = true;
            }
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, action);
            if (exited) break;
        } else if (GetLastError() != ERROR_SEM_TIMEOUT) {
            log << "Debugger wait failed; Windows error=" << GetLastError() << std::endl;
            break;
        }
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 || WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;
    }
    if (!exited) DebugActiveProcessStop(processId);
    CloseHandle(process);
    CloseHandle(stop);
    log << "Crash monitor stopped." << std::endl;
    return 0;
}

void CrashMonitor_Stop() {
    if (sMonitorStop) { SetEvent(sMonitorStop); CloseHandle(sMonitorStop); sMonitorStop = nullptr; }
    // The helper detaches on its next event-loop pass; never block the game thread.
    if (sMonitorProcess) { CloseHandle(sMonitorProcess); sMonitorProcess = nullptr; }
    sMonitorRequested = false;
}

void CrashMonitor_SetEnabled(bool enabled) {
    if (enabled == sMonitorRequested) return;
    if (!enabled) { CrashMonitor_Stop(); return; }
    sMonitorRequested = true;
    if (IsDebuggerPresent()) { SPDLOG_INFO("Detailed crash capture: an existing debugger is already attached"); return; }
    sMonitorStop = CreateEventW(nullptr, TRUE, FALSE, StopEventName(GetCurrentProcessId()).c_str());
    if (!sMonitorStop) { SPDLOG_ERROR("Crash monitor event creation failed: {}", GetLastError()); return; }
    ResetEvent(sMonitorStop);
    std::vector<wchar_t> executable(32768);
    if (!GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()))) return;
    const auto directory = std::filesystem::path(Ship::Context::GetPathRelativeToAppDirectory("logs")).wstring();
    std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --crash-monitor " +
                           std::to_wstring(GetCurrentProcessId()) + L" \"" + directory + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    if (CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &info)) {
        sMonitorProcess = info.hProcess;
        CloseHandle(info.hThread);
        SPDLOG_INFO("Detailed crash monitor started; helper pid={}", info.dwProcessId);
    } else {
        SPDLOG_ERROR("Detailed crash monitor launch failed: {}", GetLastError());
    }
}

const char* CrashMonitor_Status() {
    if (!sMonitorRequested) return "Crash monitor: off";
    if (IsDebuggerPresent()) return sMonitorProcess ? "Crash monitor: attached and ready" : "Crash monitor: existing debugger attached";
    DWORD code = 0;
    if (sMonitorProcess && GetExitCodeProcess(sMonitorProcess, &code) && code == STILL_ACTIVE) {
        return "Crash monitor: starting...";
    }
    return "Crash monitor: could not attach; see crash-monitor log in logs folder";
}
#endif
