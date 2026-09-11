#include "win_file_association.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shlobj.h> // SHChangeNotify

#include <cwchar>
#include <string>

namespace VtxInspector {
    namespace {

        constexpr const wchar_t* kProgId = L"VtxInspector.Replay";
        constexpr const wchar_t* kProgIdLabel = L"VTX Replay";
        // Our own settings key -- survives --unregister so we can honor an opt-out.
        constexpr const wchar_t* kSettingsKey = L"Software\\ZenosInteractive\\VtxInspector";
        constexpr const wchar_t* kAutoAssociate = L"AutoAssociate";

        // Full path of the running executable (handles paths longer than MAX_PATH).
        std::wstring ExecutablePath() {
            std::wstring buffer(MAX_PATH, L'\0');
            for (;;) {
                const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (copied == 0) {
                    return {};
                }
                if (copied < buffer.size()) {
                    buffer.resize(copied);
                    return buffer;
                }
                buffer.resize(buffer.size() * 2); // truncated -- grow and retry
            }
        }

        // Writes a REG_SZ value under HKCU, creating the key. value_name == nullptr sets
        // the key's default value.
        bool SetString(const std::wstring& subkey, const wchar_t* value_name, const std::wstring& data) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
                ERROR_SUCCESS) {
                return false;
            }
            const DWORD bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
            const LSTATUS status =
                RegSetValueExW(key, value_name, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()), bytes);
            RegCloseKey(key);
            return status == ERROR_SUCCESS;
        }

        // Reads a REG_SZ value under HKCU. value_name == nullptr reads the default value.
        bool GetString(const std::wstring& subkey, const wchar_t* value_name, std::wstring& out) {
            wchar_t buffer[1024] = {};
            DWORD size = sizeof(buffer);
            if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), value_name, RRF_RT_REG_SZ, nullptr, buffer, &size) !=
                ERROR_SUCCESS) {
                return false;
            }
            const size_t chars = size / sizeof(wchar_t);
            out.assign(buffer, chars > 0 ? chars - 1 : 0); // drop the trailing NUL
            return true;
        }

        void SetDword(const std::wstring& subkey, const wchar_t* value_name, DWORD value) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
                ERROR_SUCCESS) {
                RegSetValueExW(key, value_name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
                RegCloseKey(key);
            }
        }

        DWORD GetDword(const std::wstring& subkey, const wchar_t* value_name, DWORD fallback) {
            DWORD value = 0;
            DWORD size = sizeof(value);
            if (RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), value_name, RRF_RT_REG_DWORD, nullptr, &value, &size) ==
                ERROR_SUCCESS) {
                return value;
            }
            return fallback;
        }

        void NotifyShell() {
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        }

        // The open command this exe wants registered ("<exe>" "%1").
        std::wstring DesiredCommand(const std::wstring& exe) {
            return L"\"" + exe + L"\" \"%1\"";
        }

        // Writes the ProgID + "Open with" entry, pointing at this exe. Claims the .vtx
        // double-click default when force is true, or when it is currently unset or
        // already ours (so a foreign association the user chose is left alone).
        bool WriteAssociation(const std::wstring& exe, bool force_default) {
            const std::wstring classes = L"Software\\Classes\\";
            const std::wstring prog = classes + kProgId;

            bool ok = true;
            ok = ok && SetString(prog, nullptr, kProgIdLabel);
            ok = ok && SetString(prog + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
            ok = ok && SetString(prog + L"\\shell\\open\\command", nullptr, DesiredCommand(exe));
            ok = ok && SetString(classes + L".vtx\\OpenWithProgids", kProgId, L"");

            bool claim_default = force_default;
            if (!claim_default) {
                std::wstring current;
                const bool has = GetString(classes + L".vtx", nullptr, current);
                claim_default = !has || current.empty() || current == kProgId;
            }
            if (claim_default) {
                ok = ok && SetString(classes + L".vtx", nullptr, kProgId);
            }

            NotifyShell();
            return ok;
        }

    } // namespace

    EnsureResult EnsureVtxAssociationOnStartup() {
        // Respect an explicit opt-out written by --unregister.
        if (GetDword(kSettingsKey, kAutoAssociate, 1) == 0) {
            return {false, {}};
        }

        const std::wstring exe = ExecutablePath();
        if (exe.empty()) {
            return {false, {}};
        }

        // Already set up for this exe? Then there is nothing to do.
        std::wstring current_command;
        if (GetString(L"Software\\Classes\\VtxInspector.Replay\\shell\\open\\command", nullptr, current_command) &&
            current_command == DesiredCommand(exe)) {
            return {false, {}};
        }

        if (!WriteAssociation(exe, /*force_default=*/false)) {
            return {false, {}}; // best effort -- never block startup on this
        }
        return {true, "Registered this build as a .vtx handler: double-click and Open with now open replays here "
                      "(File association is per-user; run with --unregister to remove it)."};
    }

    AssociationResult RegisterVtxAssociation() {
        const std::wstring exe = ExecutablePath();
        if (exe.empty()) {
            return {false, "Could not determine the executable path."};
        }

        SetDword(kSettingsKey, kAutoAssociate, 1); // re-enable startup auto-setup
        if (!WriteAssociation(exe, /*force_default=*/true)) {
            return {false, "Failed to write the registry keys under HKCU\\Software\\Classes."};
        }
        return {true, "Registered .vtx with VTX Inspector for the current user.\n\n"
                      "Double-click a .vtx file, or use Open with, to open it here."};
    }

    AssociationResult UnregisterVtxAssociation() {
        const std::wstring classes = L"Software\\Classes\\";

        // Disable the startup auto-setup so a later launch does not silently re-add it.
        SetDword(kSettingsKey, kAutoAssociate, 0);

        RegDeleteTreeW(HKEY_CURRENT_USER, (classes + kProgId).c_str());

        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (classes + L".vtx\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &key) ==
            ERROR_SUCCESS) {
            RegDeleteValueW(key, kProgId);
            RegCloseKey(key);
        }

        // Clear the extension default only if it still points at our ProgID, so we do not
        // wipe an association the user set to something else.
        std::wstring current;
        if (GetString(classes + L".vtx", nullptr, current) && current == kProgId) {
            SetString(classes + L".vtx", nullptr, L"");
        }

        NotifyShell();
        return {true, "Removed the VTX Inspector .vtx association for the current user.\n\n"
                      "It will not re-register on the next launch."};
    }

} // namespace VtxInspector

#else

namespace VtxInspector {

    EnsureResult EnsureVtxAssociationOnStartup() {
        return {false, {}};
    }

    AssociationResult RegisterVtxAssociation() {
        return {false, "File association is only supported on Windows."};
    }

    AssociationResult UnregisterVtxAssociation() {
        return {false, "File association is only supported on Windows."};
    }

} // namespace VtxInspector

#endif
