#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace axe::panel::settings {

struct LoadResult {
    bool ok{false};
    bool found{false};
    bool enabled{false};
    std::wstring error;
};

inline constexpr wchar_t kSettingsFileName[] = L"settings.ini";
inline constexpr char kEnabledSettings[] =
    "AxeFxUsb78PanelSettings\nversion=1\nenabled=true\n";
inline constexpr char kDisabledSettings[] =
    "AxeFxUsb78PanelSettings\nversion=1\nenabled=false\n";

inline std::wstring error_message(const wchar_t* operation, DWORD error) {
    return std::wstring(operation) + L" (Windows error " + std::to_wstring(error) + L")";
}

inline std::wstring settings_path(const std::wstring& directory) {
    if (directory.empty()) return {};
    if (directory.back() == L'\\' || directory.back() == L'/')
        return directory + kSettingsFileName;
    return directory + L"\\" + kSettingsFileName;
}

inline LoadResult load_from_directory(const std::wstring& directory) {
    LoadResult result{};
    const std::wstring path = settings_path(directory);
    if (path.empty()) {
        result.error = L"The settings directory is empty";
        return result;
    }

    const DWORD directoryAttributes = GetFileAttributesW(directory.c_str());
    if (directoryAttributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            result.ok = true;
            result.enabled = true;
            return result;
        }
        result.error = error_message(L"Could not inspect the settings directory", error);
        return result;
    }
    if ((directoryAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (directoryAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.error = L"The settings directory is not a plain directory";
        return result;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            result.ok = true;
            result.enabled = true;
            return result;
        }
        result.error = error_message(L"Could not open the settings file", error);
        return result;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    bool validFile = GetFileInformationByHandle(file, &info) != FALSE &&
                     GetFileSizeEx(file, &size) != FALSE &&
                     (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
                     size.QuadPart > 0 && size.QuadPart <= 1024;
    DWORD inspectError = validFile ? ERROR_SUCCESS : GetLastError();
    if (!validFile) {
        CloseHandle(file);
        result.error = L"The settings file is empty, too large, or not a regular file";
        if (inspectError != ERROR_SUCCESS)
            result.error += L" (Windows error " + std::to_wstring(inspectError) + L")";
        return result;
    }

    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytesRead{};
    const BOOL readSucceeded = ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                                        &bytesRead, nullptr);
    const DWORD readError = readSucceeded ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!readSucceeded || bytesRead != contents.size()) {
        result.error = readSucceeded
            ? L"The settings file changed or was truncated while reading"
            : error_message(L"Could not read the settings file", readError);
        return result;
    }

    if (contents == kEnabledSettings) {
        result.ok = true;
        result.found = true;
        result.enabled = true;
        return result;
    }
    if (contents == kDisabledSettings) {
        result.ok = true;
        result.found = true;
        result.enabled = false;
        return result;
    }
    result.error = L"The settings file is corrupt or uses an unsupported version; routing was kept off";
    return result;
}

inline bool save_to_directory(const std::wstring& directory,
                              bool enabled,
                              std::wstring& error) {
    error.clear();
    if (directory.empty()) {
        error = L"The settings directory is empty";
        return false;
    }

    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        const DWORD createError = GetLastError();
        if (createError != ERROR_ALREADY_EXISTS) {
            error = error_message(L"Could not create the settings directory", createError);
            return false;
        }
    }
    const DWORD directoryAttributes = GetFileAttributesW(directory.c_str());
    if (directoryAttributes == INVALID_FILE_ATTRIBUTES ||
        (directoryAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (directoryAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (directoryAttributes == INVALID_FILE_ATTRIBUTES)
            error = error_message(L"Could not inspect the settings directory", GetLastError());
        else
            error = L"The settings directory is not a plain directory";
        return false;
    }

    const std::wstring targetPath = settings_path(directory);
    const DWORD targetAttributes = GetFileAttributesW(targetPath.c_str());
    if (targetAttributes != INVALID_FILE_ATTRIBUTES &&
        (targetAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        error = L"The settings path is not a plain file";
        return false;
    }
    if (targetAttributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD targetError = GetLastError();
        if (targetError != ERROR_FILE_NOT_FOUND) {
            error = error_message(L"Could not inspect the settings path", targetError);
            return false;
        }
    }

    static std::atomic<std::uint64_t> sequence{0};
    const std::wstring temporaryPath = targetPath + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L"." +
        std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = error_message(L"Could not create the temporary settings file", GetLastError());
        return false;
    }

    const char* contents = enabled ? kEnabledSettings : kDisabledSettings;
    const DWORD contentSize = static_cast<DWORD>(std::strlen(contents));
    DWORD bytesWritten{};
    const BOOL writeSucceeded = WriteFile(file, contents, contentSize, &bytesWritten, nullptr);
    const DWORD writeError = writeSucceeded ? ERROR_SUCCESS : GetLastError();
    const BOOL flushSucceeded = writeSucceeded && bytesWritten == contentSize && FlushFileBuffers(file);
    const DWORD flushError = flushSucceeded ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!writeSucceeded || bytesWritten != contentSize || !flushSucceeded) {
        DeleteFileW(temporaryPath.c_str());
        error = !writeSucceeded
            ? error_message(L"Could not write the temporary settings file", writeError)
            : (!flushSucceeded
                ? error_message(L"Could not flush the temporary settings file", flushError)
                : L"The settings file was not written completely");
        return false;
    }

    if (!MoveFileExW(temporaryPath.c_str(), targetPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        error = error_message(L"Could not atomically replace the settings file", moveError);
        return false;
    }
    return true;
}

inline bool user_settings_directory(std::wstring& directory, std::wstring& error) {
    DWORD required = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
    if (required == 0) {
        error = error_message(L"Could not locate the current user's profile directory", GetLastError());
        return false;
    }
    std::wstring userProfile(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"USERPROFILE", userProfile.data(), required);
    if (written == 0 || written >= required) {
        error = error_message(L"Could not read the current user's profile directory", GetLastError());
        return false;
    }
    userProfile.resize(written);
    while (userProfile.size() > 3 &&
           (userProfile.back() == L'\\' || userProfile.back() == L'/')) {
        userProfile.pop_back();
    }
    directory = userProfile + L"\\.axe-fx-usb78";
    return true;
}

inline LoadResult load() {
    std::wstring directory, error;
    if (!user_settings_directory(directory, error)) return {false, false, false, std::move(error)};
    return load_from_directory(directory);
}

inline bool save(bool enabled, std::wstring& error) {
    std::wstring directory;
    if (!user_settings_directory(directory, error)) return false;
    return save_to_directory(directory, enabled, error);
}

} // namespace axe::panel::settings
