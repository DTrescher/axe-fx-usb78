#include "tray_settings.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using axe::panel::settings::LoadResult;
using axe::panel::settings::kDisabledSettings;
using axe::panel::settings::kEnabledSettings;
using axe::panel::settings::kSettingsFileName;
using axe::panel::settings::load_from_directory;
using axe::panel::settings::save_to_directory;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::array<wchar_t, MAX_PATH + 1> root{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(root.size()), root.data());
        if (length == 0 || length >= root.size())
            throw std::runtime_error("Could not locate the Windows temporary directory");

        std::array<wchar_t, MAX_PATH + 1> candidate{};
        if (GetTempFileNameW(root.data(), L"ax7", 0, candidate.data()) == 0)
            throw std::runtime_error("Could not reserve a temporary test path");
        if (!DeleteFileW(candidate.data())) {
            const DWORD error = GetLastError();
            throw std::runtime_error("Could not replace the reserved temporary file (Windows error " +
                                     std::to_string(error) + ")");
        }
        if (!CreateDirectoryW(candidate.data(), nullptr)) {
            const DWORD error = GetLastError();
            throw std::runtime_error("Could not create the settings test directory (Windows error " +
                                     std::to_string(error) + ")");
        }
        path_ = candidate.data();
    }

    ~TemporaryDirectory() {
        if (path_.empty()) return;
        const std::wstring pattern = path_ + L"\\*";
        WIN32_FIND_DATAW item{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &item);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(item.cFileName, L".") == 0 || wcscmp(item.cFileName, L"..") == 0)
                    continue;
                const std::wstring child = path_ + L"\\" + item.cFileName;
                if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    RemoveDirectoryW(child.c_str());
                } else {
                    SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(child.c_str());
                }
            } while (FindNextFileW(search, &item));
            FindClose(search);
        }
        RemoveDirectoryW(path_.c_str());
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::wstring& path() const noexcept { return path_; }
    std::wstring settings_path() const { return path_ + L"\\" + kSettingsFileName; }

private:
    std::wstring path_;
};

void write_file(const std::wstring& path, const std::string& contents) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Could not create test settings file");
    DWORD written{};
    const BOOL succeeded = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                                     &written, nullptr);
    const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    require(succeeded != FALSE && written == contents.size(),
            error == ERROR_SUCCESS ? "Could not write complete test settings file"
                                   : "Could not write test settings file");
}

std::string read_file(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Could not open test settings file for verification");
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 && size.QuadPart <= 4096,
            "Could not read test settings file size");
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    const BOOL succeeded = contents.empty() ||
        ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr);
    CloseHandle(file);
    require(succeeded != FALSE && read == contents.size(), "Could not read complete test settings file");
    return contents;
}

bool has_temporary_files(const std::wstring& directory) {
    WIN32_FIND_DATAW item{};
    const std::wstring pattern = directory + L"\\settings.ini.tmp.*";
    HANDLE search = FindFirstFileW(pattern.c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) return false;
    FindClose(search);
    return true;
}

void expect_bad_contents(const std::string& contents, const char* label) {
    TemporaryDirectory directory;
    write_file(directory.settings_path(), contents);
    const LoadResult result = load_from_directory(directory.path());
    require(!result.ok && result.found == false && result.enabled == false && !result.error.empty(), label);
}

void test_missing_settings_defaults_enabled() {
    TemporaryDirectory directory;
    const LoadResult missing_directory = load_from_directory(directory.path() + L"\\not-created");
    require(missing_directory.ok && !missing_directory.found && missing_directory.enabled,
            "A missing settings directory should retain the enabled-by-default behavior");

    const LoadResult missing_file = load_from_directory(directory.path());
    require(missing_file.ok && !missing_file.found && missing_file.enabled,
            "A missing settings file should retain the enabled-by-default behavior");
}

void test_save_and_load_states() {
    TemporaryDirectory directory;
    std::wstring error = L"stale error";
    require(save_to_directory(directory.path(), false, error), "Saving disabled state failed");
    require(error.empty(), "Successful save should clear the caller's prior error");
    require(read_file(directory.settings_path()) == kDisabledSettings,
            "Disabled save did not write the exact versioned format");
    LoadResult result = load_from_directory(directory.path());
    require(result.ok && result.found && !result.enabled && result.error.empty(),
            "Disabled state did not round trip");

    require(save_to_directory(directory.path(), true, error), "Replacing settings with enabled state failed");
    require(read_file(directory.settings_path()) == kEnabledSettings,
            "Enabled save did not atomically replace the disabled state");
    result = load_from_directory(directory.path());
    require(result.ok && result.found && result.enabled,
            "Enabled state did not round trip");
    require(!has_temporary_files(directory.path()), "Successful settings save left a temporary file behind");
}

void test_corrupt_truncated_and_unsupported_settings_fail_closed() {
    expect_bad_contents("", "An empty settings file must be rejected");
    expect_bad_contents("AxeFxUsb78PanelSettings\nversion=1\n",
                        "A truncated settings file must be rejected");
    expect_bad_contents("AxeFxUsb78PanelSettings\nversion=2\nenabled=true\n",
                        "An unsupported settings version must be rejected");
    expect_bad_contents("AxeFxUsb78PanelSettings\nversion=1\nenabled=true\nextra=yes\n",
                        "Unknown settings fields must be rejected");

    TemporaryDirectory directory;
    write_file(directory.settings_path(), std::string(1025, 'x'));
    const LoadResult oversized = load_from_directory(directory.path());
    require(!oversized.ok && !oversized.enabled && !oversized.error.empty(),
            "An oversized settings file must be rejected without enabling routing");
}

void test_non_directory_settings_path_fails() {
    TemporaryDirectory directory;
    const std::wstring file_path = directory.path() + L"\\not-a-directory";
    write_file(file_path, "ordinary file");
    const LoadResult load = load_from_directory(file_path);
    require(!load.ok && !load.enabled && !load.error.empty(),
            "Loading from a path that is a file must fail closed");

    std::wstring error;
    require(!save_to_directory(file_path, true, error) && !error.empty(),
            "Saving beneath a path that is a file must fail");
}

void test_atomic_replace_failure_preserves_old_value() {
    TemporaryDirectory directory;
    std::wstring error;
    require(save_to_directory(directory.path(), true, error), "Could not seed the atomic replacement test");

    HANDLE blocker = CreateFileW(directory.settings_path().c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    require(blocker != INVALID_HANDLE_VALUE, "Could not open a handle to block atomic replacement");
    const bool saved = save_to_directory(directory.path(), false, error);
    CloseHandle(blocker);

    require(!saved && !error.empty(), "Atomic replacement unexpectedly succeeded while delete sharing was denied");
    require(read_file(directory.settings_path()) == kEnabledSettings,
            "Failed atomic replacement changed the previously committed setting");
    const LoadResult result = load_from_directory(directory.path());
    require(result.ok && result.found && result.enabled,
            "Failed atomic replacement did not preserve a loadable previous setting");
    require(!has_temporary_files(directory.path()), "Failed atomic replacement left its temporary file behind");
}

} // namespace

int wmain() {
    try {
        test_missing_settings_defaults_enabled();
        test_save_and_load_states();
        test_corrupt_truncated_and_unsupported_settings_fail_closed();
        test_non_directory_settings_path_fails();
        test_atomic_replace_failure_preserves_old_value();
        std::cout << "PASS: tray settings default, round-trip, corruption, truncation, path failures, and atomic replacement.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
