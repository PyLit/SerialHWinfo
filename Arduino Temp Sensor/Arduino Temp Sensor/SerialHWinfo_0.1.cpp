// Simple Windows console app to read lines from a serial port and write them to Registry
//Created by Donald Carswell using OpenAI ChatGPT-4
//V0.1 2025-09-13

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <iostream>
#include <string>
#include <atomic>
#include <vector>
#include <algorithm>

// Change these to suit your system
static constexpr const wchar_t* COM_PORT = L"COM4"; // use \\.\COM10 style for COM10+
static constexpr const wchar_t* REG_BASE_PATH = L"Software\\HWiNFO64\\Sensors\\Custom\\PC Water Sensor\\Temp0";

static std::atomic<bool> g_running{ true };

// Ctrl-C handler for graceful shutdown
BOOL WINAPI ConsoleHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT ||
        dwCtrlType == CTRL_LOGOFF_EVENT || dwCtrlType == CTRL_SHUTDOWN_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}

// Trim leading/trailing spaces (ASCII)
static inline std::string trim_ascii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && isspace(static_cast<unsigned char>(s[start]))) ++start;
    if (start == s.size()) return "";
    size_t end = s.size() - 1;
    while (end > start && isspace(static_cast<unsigned char>(s[end]))) --end;
    return s.substr(start, end - start + 1);
}

// Convert UTF-8/ASCII std::string to UTF-16 std::wstring
static std::wstring utf8_to_wstring(const std::string& in) {
    if (in.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), NULL, 0);
    if (needed == 0) {
        needed = MultiByteToWideChar(CP_ACP, 0, in.c_str(), (int)in.size(), NULL, 0);
        if (needed == 0) return std::wstring();
    }
    std::wstring out; out.resize(needed);
    MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), &out[0], needed);
    return out;
}

int wmain() {
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::wcout << L"Opening serial port " << COM_PORT << L" ...\n";

    HANDLE hSerial = CreateFileW(
        COM_PORT,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        std::wcerr << L"ERROR: Cannot open serial port. GetLastError=" << GetLastError() << L"\n";
        return 1;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hSerial, &dcb)) {
        std::wcerr << L"ERROR: GetCommState failed: " << GetLastError() << L"\n";
        CloseHandle(hSerial);
        return 1;
    }
    dcb.BaudRate = CBR_9600;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    if (!SetCommState(hSerial, &dcb)) {
        std::wcerr << L"ERROR: SetCommState failed: " << GetLastError() << L"\n";
        CloseHandle(hSerial);
        return 1;
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.ReadTotalTimeoutConstant = 1000;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(hSerial, &timeouts);

    // Open/create the registry key for VSB sensor
    HKEY hKey = NULL;
    LONG res = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        REG_BASE_PATH,
        0, NULL, 0,
        KEY_WRITE | KEY_READ,
        NULL,
        &hKey,
        NULL
    );

    if (res != ERROR_SUCCESS) {
        std::wcerr << L"ERROR: Could not create/open registry key. Error: " << res << L"\n";
        CloseHandle(hSerial);
        return 1;
    }

    // Write minimal metadata: Name (sensor label)
    std::wstring sensorName = L"Temperature";
    res = RegSetValueExW(hKey, L"Name", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(sensorName.c_str()),
        (DWORD)((sensorName.size() + 1) * sizeof(wchar_t)));
    if (res != ERROR_SUCCESS) std::wcerr << L"WARNING: Failed to write Name: " << res << L"\n";

    std::wcout << L"Registry sensor key ready. Listening for serial lines...\n";

    // Line buffering
    std::string readBuffer;
    readBuffer.reserve(512);
    std::vector<char> tempBuf(256);
    DWORD bytesRead = 0;
    std::string lastValueWritten;

    while (g_running.load()) {
        BOOL ok = ReadFile(hSerial, tempBuf.data(), (DWORD)tempBuf.size(), &bytesRead, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_OPERATION_ABORTED) {
                std::wcerr << L"ERROR: ReadFile failed: " << err << L"\n";
            }
            Sleep(50);
            continue;
        }

        if (bytesRead == 0) {
            Sleep(10);
            continue;
        }

        readBuffer.append(tempBuf.data(), tempBuf.data() + bytesRead);

        size_t pos;
        while ((pos = readBuffer.find('\n')) != std::string::npos) {
            std::string line = readBuffer.substr(0, pos + 1);
            readBuffer.erase(0, pos + 1);

            std::string trimmed = trim_ascii(line);
            if (trimmed.empty()) continue;

            // Validate numeric value
            bool valid = true;
            try {
                size_t idx = 0;
                std::stod(trimmed, &idx);
                while (idx < trimmed.size() && isspace(trimmed[idx])) ++idx;
                if (idx != trimmed.size()) valid = false;
            }
            catch (...) { valid = false; }

            if (!valid) {
                std::cout << "Ignored non-numeric line: " << trimmed << "\n";
                continue;
            }

            if (trimmed != lastValueWritten) {
                lastValueWritten = trimmed;
                std::wstring wvalue = utf8_to_wstring(trimmed);

                LONG wres = RegSetValueExW(hKey, L"Value", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(wvalue.c_str()),
                    (DWORD)((wvalue.size() + 1) * sizeof(wchar_t)));
                if (wres != ERROR_SUCCESS) {
                    std::wcerr << L"ERROR: Failed to write Value: " << wres << "\n";
                }
                else {
                    std::wcout << L"Wrote Value: " << wvalue.c_str() << L" to registry\n";
                }
            }
        }
    }

    std::wcout << L"Shutting down...\n";
    if (hKey) RegCloseKey(hKey);
    if (hSerial && hSerial != INVALID_HANDLE_VALUE) CloseHandle(hSerial);
    std::wcout << L"Exit complete.\n";
    return 0;
}