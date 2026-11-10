#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SessionRestart
{
class Handle
{
public:
    explicit Handle(HANDLE value = nullptr) : m_value(value) {}
    ~Handle() { if (m_value) CloseHandle(m_value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other)
        {
            if (m_value) CloseHandle(m_value);
            m_value = std::exchange(other.m_value, nullptr);
        }
        return *this;
    }
    HANDLE get() const { return m_value; }
    explicit operator bool() const { return m_value != nullptr; }
private:
    HANDLE m_value;
};

inline std::wstring QuoteArgument(std::wstring_view value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : value)
    {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

class Pending
{
public:
    ~Pending() { if (!m_committed) Cancel(); }
    void Cancel() { if (cancel) SetEvent(cancel.get()); }
    void Commit() { m_committed = true; }
    Handle process;
    Handle cancel;
private:
    bool m_committed = false;
};

inline bool WaitForParent(uintptr_t parentValue, uintptr_t readyValue, uintptr_t cancelValue)
{
    if (!parentValue || !readyValue || !cancelValue || parentValue == readyValue ||
        parentValue == cancelValue || readyValue == cancelValue)
        return false;
    Handle parent(reinterpret_cast<HANDLE>(parentValue));
    Handle ready(reinterpret_cast<HANDLE>(readyValue));
    Handle cancel(reinterpret_cast<HANDLE>(cancelValue));
    const DWORD parentId = GetProcessId(parent.get());
    if (!parentId || parentId == GetCurrentProcessId())
        return false;
    for (HANDLE handle : {parent.get(), ready.get(), cancel.get()})
        if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0))
            return false;
    if (WaitForSingleObject(cancel.get(), 0) != WAIT_TIMEOUT || !SetEvent(ready.get()))
        return false;
    // A process handle, not a PID, prevents reuse races. No settings, MLC or GPU
    // initialization may run in the replacement until the old process has exited.
    HANDLE waits[] = {cancel.get(), parent.get()};
    return WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
}

inline bool Start(const std::wstring& executable, const std::vector<std::wstring>& arguments,
                  Pending& pending, DWORD& error, DWORD readyTimeout = 10000)
{
    error = ERROR_SUCCESS;
    if (pending.process || pending.cancel) { error = ERROR_ALREADY_EXISTS; return false; }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, GetCurrentProcessId()));
    if (!parent) { error = GetLastError(); return false; }
    Handle ready(CreateEventW(&security, TRUE, FALSE, nullptr));
    if (!ready) { error = GetLastError(); return false; }
    Handle cancel(CreateEventW(&security, TRUE, FALSE, nullptr));
    if (!cancel) { error = GetLastError(); return false; }

    struct Attributes
    {
        std::vector<unsigned char> storage;
        LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
        ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
    } attributes;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (!size) { error = GetLastError(); return false; }
    attributes.storage.resize(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) { error = GetLastError(); return false; }
    attributes.list = list;
    HANDLE inherited[] = {parent.get(), ready.get(), cancel.get()};
    // Do not inherit the emulator's open save files or unrelated handles.
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), nullptr, nullptr))
    { error = GetLastError(); return false; }

    std::wstring command = QuoteArgument(executable);
    for (const auto& argument : arguments)
        command += L" " + QuoteArgument(argument);
    command += L" --wait-for-parent-handle " + std::to_wstring(reinterpret_cast<uintptr_t>(parent.get()));
    command += L" --stop-ready-handle " + std::to_wstring(reinterpret_cast<uintptr_t>(ready.get()));
    command += L" --stop-cancel-handle " + std::to_wstring(reinterpret_cast<uintptr_t>(cancel.get()));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup.StartupInfo, &info))
    { error = GetLastError(); return false; }
    Handle process(info.hProcess);
    Handle thread(info.hThread);
    HANDLE waits[] = {process.get(), ready.get()};
    const DWORD result = WaitForMultipleObjects(2, waits, FALSE, readyTimeout);
    if (result != WAIT_OBJECT_0 + 1)
    {
        error = result == WAIT_FAILED ? GetLastError() : (result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_PROCESS_ABORTED);
        SetEvent(cancel.get());
        return false;
    }
    pending.process = std::move(process);
    pending.cancel = std::move(cancel);
    return true;
}
}
#endif
