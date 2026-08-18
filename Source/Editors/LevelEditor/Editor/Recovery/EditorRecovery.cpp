#include "stdafx.h"
#include "EditorRecovery.h"

namespace
{
constexpr u32 kManifestVersion = 1;
constexpr u32 kMaximumManifestEntries = 64;
constexpr u64 kRetryDelayMs = 30ull * 1000ull;
constexpr u64 kClaimRetryDelayMs = 5ull * 1000ull;

struct SRecoveryEntry
{
    xr_string snapshot;
    xr_string source;
    u64 saved_at = 0;
    u64 snapshot_size = 0;
    u64 snapshot_hash = 0;
    u64 source_mtime = 0;
    u64 source_size = 0;
};

enum class EOperationState
{
    Idle,
    Available,
    SnapshotCreated,
    RestorePending,
    Restored,
    DiscardPending,
    Discarded,
    Error,
};

xr_string s_project_root;
xr_string s_recovery_dir;
xr_string s_marker_path;
xr_string s_claim_path;
xr_string s_manifest_path;
xr_string s_startup_error;
xr_string s_last_operation_error;
SRecoveryEntry s_startup_entry;
u64 s_process_started_at = 0;
u64 s_next_save_at = 0;
u64 s_next_claim_retry_at = 0;
u64 s_operation_id = 0;
u64 s_next_operation_id = 0;
u32 s_snapshot_sequence = 0;
DWORD s_last_seen_input = 0;
DWORD s_last_editor_input = 0;
bool s_bound = false;
bool s_owns_marker = false;
bool s_shared_project = false;
bool s_offer_recovery = false;
bool s_snapshot_available = false;
bool s_restore_requested = false;
bool s_discard_requested = false;
bool s_manual_snapshot_requested = false;
bool s_internal_source_load = false;
bool s_preserve_on_shutdown = false;
EOperationState s_operation_state = EOperationState::Idle;
HANDLE s_claim_handle = INVALID_HANDLE_VALUE;

u64 FileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER result;
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

FILETIME MakeFileTime(u64 value)
{
    ULARGE_INTEGER source;
    source.QuadPart = value;
    FILETIME result;
    result.dwLowDateTime = source.LowPart;
    result.dwHighDateTime = source.HighPart;
    return result;
}

u64 CurrentFileTime()
{
    FILETIME value;
    ::GetSystemTimeAsFileTime(&value);
    return FileTimeValue(value);
}

u64 CurrentProcessStartedAt()
{
    FILETIME created, exited, kernel, user;
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    return FileTimeValue(created);
}

bool FileExists(LPCSTR path)
{
    const DWORD attributes = ::GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectoryTree(LPCSTR path, xr_string& error)
{
    string_path current = {};
    xr_strcpy(current, path);
    for (char* cursor = current; *cursor; ++cursor)
    {
        if (*cursor != '\\' && *cursor != '/')
            continue;

        const char separator = *cursor;
        *cursor = 0;
        const size_t length = xr_strlen(current);
        const bool unc_prefix = length < 2 ||
            (current[0] == '\\' && current[1] == '\\' && !strchr(current + 2, '\\') && !strchr(current + 2, '/'));
        if (!length || unc_prefix)
        {
            *cursor = separator;
            continue;
        }
        if (length > 0 && current[length - 1] != ':' &&
            !::CreateDirectoryA(current, nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
        {
            error.sprintf("cannot create recovery folder '%s' (Windows error %lu)", current, ::GetLastError());
            *cursor = separator;
            return false;
        }
        *cursor = separator;
    }

    if (!::CreateDirectoryA(current, nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        error.sprintf("cannot create recovery folder '%s' (Windows error %lu)", current, ::GetLastError());
        return false;
    }
    return true;
}

xr_string JoinPath(LPCSTR directory, LPCSTR leaf)
{
    xr_string result;
    result.sprintf("%s\\%s", directory, leaf);
    return result;
}

bool IsSafeLeaf(LPCSTR value)
{
    return value && value[0] && !strchr(value, '\\') && !strchr(value, '/') &&
        !strstr(value, "..") && !strchr(value, ':');
}

bool WriteCheckedFile(LPCSTR path, const void* data, u32 size, xr_string& error)
{
    HANDLE file = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error.sprintf("cannot create recovery file '%s' (Windows error %lu)", path, ::GetLastError());
        return false;
    }

    const u8* bytes = static_cast<const u8*>(data);
    u32 offset = 0;
    DWORD write_error = ERROR_SUCCESS;
    while (offset < size)
    {
        DWORD written = 0;
        const DWORD chunk = std::min<DWORD>(size - offset, 1024u * 1024u);
        if (!::WriteFile(file, bytes + offset, chunk, &written, nullptr))
        {
            write_error = ::GetLastError();
            break;
        }
        if (written != chunk)
        {
            write_error = ERROR_WRITE_FAULT;
            break;
        }
        offset += written;
    }
    if (write_error == ERROR_SUCCESS && !::FlushFileBuffers(file))
        write_error = ::GetLastError();
    ::CloseHandle(file);
    if (write_error != ERROR_SUCCESS)
    {
        ::DeleteFileA(path);
        error.sprintf("cannot write recovery file '%s' (Windows error %lu)", path, write_error);
        return false;
    }
    return true;
}

bool HashFile(LPCSTR path, u64& size, u64& hash, xr_string& error)
{
    size = 0;
    hash = 14695981039346656037ull;
    HANDLE file = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error.sprintf("cannot read recovery file '%s' (Windows error %lu)", path, ::GetLastError());
        return false;
    }

    u8 buffer[64 * 1024];
    DWORD read = 0;
    bool result = true;
    DWORD read_error = ERROR_SUCCESS;
    for (;;)
    {
        if (!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr))
        {
            result = false;
            read_error = ::GetLastError();
            break;
        }
        if (!read)
            break;
        size += read;
        for (DWORD index = 0; index < read; ++index)
        {
            hash ^= buffer[index];
            hash *= 1099511628211ull;
        }
    }
    ::CloseHandle(file);
    if (!result)
        error.sprintf("cannot verify recovery file '%s' (Windows error %lu)", path, read_error);
    return result;
}

xr_string TemporaryPath(LPCSTR target)
{
    xr_string result;
    for (u32 attempt = 0; attempt < 100; ++attempt)
    {
        result.sprintf("%s.tmp.%lu.%llu.%u", target, ::GetCurrentProcessId(),
            static_cast<unsigned long long>(::GetTickCount64()), attempt);
        if (!FileExists(result.c_str()))
            return result;
    }
    result.clear();
    return result;
}

bool CommitTemporaryFile(LPCSTR temporary, LPCSTR target, xr_string& error)
{
    bool result = false;
    if (FileExists(target))
        result = !!::ReplaceFileA(target, temporary, nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    else
        result = !!::MoveFileExA(temporary, target, MOVEFILE_WRITE_THROUGH);

    if (!result)
        error.sprintf("cannot commit recovery metadata '%s' (Windows error %lu)", target, ::GetLastError());
    return result;
}

bool WriteAtomicText(LPCSTR target, const xr_string& text, xr_string& error)
{
    const xr_string temporary = TemporaryPath(target);
    if (temporary.empty())
    {
        error = "cannot allocate a unique recovery metadata name";
        return false;
    }

    if (!WriteCheckedFile(temporary.c_str(), text.data(), static_cast<u32>(text.size()), error))
        return false;
    if (!CommitTemporaryFile(temporary.c_str(), target, error))
    {
        ::DeleteFileA(temporary.c_str());
        return false;
    }
    return true;
}

u64 ReadIniU64(LPCSTR file, LPCSTR section, LPCSTR key)
{
    char value[32] = {};
    ::GetPrivateProfileStringA(section, key, "0", value, sizeof(value), file);
    return _strtoui64(value, nullptr, 10);
}

void ReadIniString(LPCSTR file, LPCSTR section, LPCSTR key, xr_string& result)
{
    char value[MAX_PATH] = {};
    ::GetPrivateProfileStringA(section, key, "", value, sizeof(value), file);
    result = value;
}

bool ReadManifest(xr_vector<SRecoveryEntry>& entries)
{
    entries.clear();
    if (!FileExists(s_manifest_path.c_str()) ||
        ::GetPrivateProfileIntA("recovery", "version", 0, s_manifest_path.c_str()) != kManifestVersion)
        return false;

    const u32 count = clampr<u32>(::GetPrivateProfileIntA("recovery", "count", 0,
        s_manifest_path.c_str()), 0, kMaximumManifestEntries);
    for (u32 index = 0; index < count; ++index)
    {
        char section[32];
        sprintf_s(section, "entry_%u", index);
        SRecoveryEntry entry;
        ReadIniString(s_manifest_path.c_str(), section, "snapshot", entry.snapshot);
        if (!IsSafeLeaf(entry.snapshot.c_str()))
            continue;
        ReadIniString(s_manifest_path.c_str(), section, "source", entry.source);
        entry.saved_at = ReadIniU64(s_manifest_path.c_str(), section, "saved_at");
        entry.snapshot_size = ReadIniU64(s_manifest_path.c_str(), section, "snapshot_size");
        entry.snapshot_hash = ReadIniU64(s_manifest_path.c_str(), section, "snapshot_hash");
        entry.source_mtime = ReadIniU64(s_manifest_path.c_str(), section, "source_mtime");
        entry.source_size = ReadIniU64(s_manifest_path.c_str(), section, "source_size");
        entries.push_back(entry);
    }
    return !entries.empty();
}

xr_string BuildManifest(const xr_vector<SRecoveryEntry>& entries)
{
    xr_string result = "[recovery]\r\nversion=1\r\n";
    char line[MAX_PATH * 4];
    sprintf_s(line, "count=%u\r\n", static_cast<u32>(entries.size()));
    result += line;

    for (u32 index = 0; index < entries.size(); ++index)
    {
        const SRecoveryEntry& entry = entries[index];
        sprintf_s(line, "\r\n[entry_%u]\r\nsnapshot=%s\r\nsource=%s\r\nsaved_at=%llu\r\n"
            "snapshot_size=%llu\r\nsnapshot_hash=%llu\r\nsource_mtime=%llu\r\nsource_size=%llu\r\n",
            index, entry.snapshot.c_str(), entry.source.c_str(),
            static_cast<unsigned long long>(entry.saved_at),
            static_cast<unsigned long long>(entry.snapshot_size),
            static_cast<unsigned long long>(entry.snapshot_hash),
            static_cast<unsigned long long>(entry.source_mtime),
            static_cast<unsigned long long>(entry.source_size));
        result += line;
    }
    return result;
}

bool GetFileMetadata(LPCSTR path, u64& modified, u64& size)
{
    modified = 0;
    size = 0;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!path || !path[0] || !::GetFileAttributesExA(path, GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    modified = FileTimeValue(data.ftLastWriteTime);
    ULARGE_INTEGER file_size;
    file_size.LowPart = data.nFileSizeLow;
    file_size.HighPart = data.nFileSizeHigh;
    size = file_size.QuadPart;
    return true;
}

bool WriteSessionMarker(xr_string& error)
{
    char text[160];
    sprintf_s(text, "[session]\r\nversion=1\r\npid=%lu\r\nstarted_at=%llu\r\n",
        ::GetCurrentProcessId(), static_cast<unsigned long long>(s_process_started_at));
    return WriteAtomicText(s_marker_path.c_str(), xr_string(text), error);
}

void RemoveTemporaryFiles()
{
    const xr_string mask = JoinPath(s_recovery_dir.c_str(), "*.tmp.*");
    WIN32_FIND_DATAA data;
    HANDLE search = ::FindFirstFileA(mask.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            ::DeleteFileA(JoinPath(s_recovery_dir.c_str(), data.cFileName).c_str());
    } while (::FindNextFileA(search, &data));
    ::FindClose(search);
}

void DeleteSnapshotsNotIn(const xr_vector<SRecoveryEntry>& entries)
{
    const xr_string mask = JoinPath(s_recovery_dir.c_str(), "*.recovery");
    WIN32_FIND_DATAA data;
    HANDLE search = ::FindFirstFileA(mask.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const bool keep = std::find_if(entries.begin(), entries.end(), [&data](const SRecoveryEntry& entry)
        {
            return 0 == _stricmp(entry.snapshot.c_str(), data.cFileName);
        }) != entries.end();
        if (!keep)
            ::DeleteFileA(JoinPath(s_recovery_dir.c_str(), data.cFileName).c_str());
    } while (::FindNextFileA(search, &data));
    ::FindClose(search);
}

bool DeleteRecoveryData(xr_string& error)
{
    xr_vector<SRecoveryEntry> empty;
    if (!WriteAtomicText(s_manifest_path.c_str(), BuildManifest(empty), error))
        return false;
    DeleteSnapshotsNotIn(empty);
    ::DeleteFileA(s_manifest_path.c_str());
    RemoveTemporaryFiles();
    return true;
}

bool ValidateSnapshot(const SRecoveryEntry& entry, LPCSTR path, xr_string& why)
{
    if (!entry.snapshot_size || !entry.snapshot_hash)
    {
        why = "missing size or checksum metadata";
        return false;
    }
    u64 size = 0;
    u64 hash = 0;
    if (!HashFile(path, size, hash, why))
        return false;
    if (size != entry.snapshot_size || hash != entry.snapshot_hash)
    {
        why = "size or checksum mismatch";
        return false;
    }
    return EScene::IsSceneFile(path, why);
}

bool SelectStartupEntry()
{
    xr_vector<SRecoveryEntry> entries;
    if (!ReadManifest(entries))
        return false;

    for (const SRecoveryEntry& entry : entries)
    {
        const xr_string snapshot = JoinPath(s_recovery_dir.c_str(), entry.snapshot.c_str());
        xr_string why;
        if (ValidateSnapshot(entry, snapshot.c_str(), why))
        {
            s_startup_entry = entry;
            return true;
        }
        Msg("! recovery: skipping invalid snapshot '%s': %s", snapshot.c_str(), why.c_str());
    }
    return false;
}

bool TryAcquireSessionOwner(bool report_failure)
{
    xr_string error;
    const u64 now = ::GetTickCount64();
    if (!EnsureDirectoryTree(s_recovery_dir.c_str(), error))
    {
        if (report_failure)
            Msg("! recovery: %s", error.c_str());
        s_shared_project = true;
        s_next_claim_retry_at = now + kClaimRetryDelayMs;
        return false;
    }

    s_claim_handle = ::CreateFileA(s_claim_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s_claim_handle == INVALID_HANDLE_VALUE)
    {
        if (report_failure)
            Msg("! recovery: project '%s' already has a live recovery owner; autosave is disabled in this process",
                s_project_root.c_str());
        s_shared_project = true;
        s_next_claim_retry_at = now + kClaimRetryDelayMs;
        return false;
    }

    // The held claim is the ownership boundary for inspecting the predecessor marker.
    const bool unclean_session = FileExists(s_marker_path.c_str());
    if (!WriteSessionMarker(error))
    {
        if (report_failure)
            Msg("! recovery: %s", error.c_str());
        ::CloseHandle(s_claim_handle);
        s_claim_handle = INVALID_HANDLE_VALUE;
        ::DeleteFileA(s_claim_path.c_str());
        s_shared_project = true;
        s_next_claim_retry_at = now + kClaimRetryDelayMs;
        return false;
    }

    s_owns_marker = true;
    s_shared_project = false;
    s_next_claim_retry_at = 0;
    RemoveTemporaryFiles();
    if (unclean_session)
    {
        s_offer_recovery = SelectStartupEntry();
        s_snapshot_available = s_offer_recovery;
        if (s_offer_recovery)
            s_operation_state = EOperationState::Available;
        else
        {
            s_operation_state = EOperationState::Error;
            s_last_operation_error = "the previous session ended unexpectedly, but no valid recovery snapshot was found";
            Msg("! recovery: the previous project session ended unexpectedly, but no valid snapshot was found");
        }
    }
    s_next_save_at = now;
    LASTINPUTINFO input = { sizeof(input) };
    if (::GetLastInputInfo(&input))
        s_last_seen_input = input.dwTime;
    s_last_editor_input = ::GetTickCount();
    return true;
}

void EndProjectSession()
{
    const bool clean_session = s_owns_marker && !s_offer_recovery && !s_preserve_on_shutdown &&
        (!Scene || !Scene->IsUnsaved());
    if (clean_session)
    {
        xr_string error;
        if (DeleteRecoveryData(error))
            ::DeleteFileA(s_marker_path.c_str());
        else
            Msg("! recovery: cannot finalize the clean session: %s", error.c_str());
    }
    if (s_claim_handle != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(s_claim_handle);
        s_claim_handle = INVALID_HANDLE_VALUE;
    }
    if (s_owns_marker)
        ::DeleteFileA(s_claim_path.c_str());

    s_project_root.clear();
    s_recovery_dir.clear();
    s_marker_path.clear();
    s_claim_path.clear();
    s_manifest_path.clear();
    s_startup_error.clear();
    s_last_operation_error.clear();
    s_startup_entry = {};
    s_process_started_at = 0;
    s_next_save_at = 0;
    s_next_claim_retry_at = 0;
    s_operation_id = 0;
    s_last_seen_input = 0;
    s_last_editor_input = 0;
    s_bound = false;
    s_owns_marker = false;
    s_shared_project = false;
    s_offer_recovery = false;
    s_snapshot_available = false;
    s_restore_requested = false;
    s_discard_requested = false;
    s_manual_snapshot_requested = false;
    s_internal_source_load = false;
    s_preserve_on_shutdown = false;
    s_operation_state = EOperationState::Idle;
}

bool BindProject()
{
    if (!EditorProject::Active())
    {
        if (s_bound)
            EndProjectSession();
        return false;
    }

    const LPCSTR root = EditorProject::Root();
    if (s_bound && 0 == _stricmp(s_project_root.c_str(), root))
    {
        if (s_shared_project && ::GetTickCount64() >= s_next_claim_retry_at && TryAcquireSessionOwner(false))
            Msg("* recovery: this process acquired the project recovery session");
        return true;
    }
    if (s_bound)
        EndProjectSession();

    s_project_root = root;
    s_recovery_dir = JoinPath(root, "backup\\recovery");
    s_marker_path = JoinPath(s_recovery_dir.c_str(), "session.lock");
    s_claim_path = JoinPath(s_recovery_dir.c_str(), "session.claim");
    s_manifest_path = JoinPath(s_recovery_dir.c_str(), "manifest.ltx");
    s_process_started_at = CurrentProcessStartedAt();
    s_bound = true;
    TryAcquireSessionOwner(true);
    return true;
}

CLevelPreferences* RecoveryPreferences()
{
    return EPrefs ? dynamic_cast<CLevelPreferences*>(EPrefs) : nullptr;
}

bool CanSerializeScene(xr_string& error)
{
    if (!s_bound || s_shared_project || !s_owns_marker)
    {
        error = s_shared_project ? "recovery is disabled because another live session owns this project" :
            "recovery session is not available";
        return false;
    }
    if (s_offer_recovery)
    {
        error = "resolve the startup recovery offer before creating another snapshot";
        return false;
    }
    if (!EditorProject::GameLinked())
    {
        error = "the project has no valid game link";
        return false;
    }
    if (!Scene || !Scene->valid())
    {
        error = "no valid scene";
        return false;
    }
    if (!UI || UI->GetEState() != esEditScene || UI->ProgressOperationActive())
    {
        error = "the editor is busy";
        return false;
    }
    if (ImGui::GetCurrentContext() && ImGui::IsAnyItemActive())
    {
        error = "an editor control is active";
        return false;
    }
    if (Scene->locked() || Scene->IsPlayInEditor())
    {
        error = Scene->locked() ? "the scene is locked" : "play in editor is active";
        return false;
    }
    return true;
}

bool SnapshotTemporarilyBlocked()
{
    return Scene && Scene->valid() && UI &&
        (UI->GetEState() != esEditScene || UI->ProgressOperationActive() || Scene->locked() ||
            Scene->IsPlayInEditor() || (ImGui::GetCurrentContext() && ImGui::IsAnyItemActive()));
}

bool UserInteractionIdle(u32 delay_seconds)
{
    if (UI->IsMouseCaptured() || UI->IsMouseInUse() || UI->IsViewportNavigating())
        return false;
    LASTINPUTINFO input = { sizeof(input) };
    if (!::GetLastInputInfo(&input))
        return false;
    if (input.dwTime != s_last_seen_input)
    {
        s_last_seen_input = input.dwTime;
        DWORD foreground_process = 0;
        if (const HWND foreground = ::GetForegroundWindow())
            ::GetWindowThreadProcessId(foreground, &foreground_process);
        if (foreground_process == ::GetCurrentProcessId())
            s_last_editor_input = input.dwTime;
    }
    return static_cast<DWORD>(::GetTickCount() - s_last_editor_input) >= delay_seconds * 1000u;
}

bool RestoreStartupEntry(xr_string& error)
{
    if (!UI || !Scene || !Scene->valid() || UI->GetEState() != esEditScene || UI->ProgressOperationActive())
    {
        error = "the editor became busy before recovery could run";
        return false;
    }
    if (Scene->locked() || Scene->IsPlayInEditor())
    {
        error = Scene->locked() ? "the scene became locked before recovery could run" :
            "play in editor started before recovery could run";
        return false;
    }
    if (Scene->IsUnsaved())
    {
        error = "the current scene became dirty before recovery could run";
        return false;
    }
    const xr_string snapshot = JoinPath(s_recovery_dir.c_str(), s_startup_entry.snapshot.c_str());
    xr_string why;
    if (!ValidateSnapshot(s_startup_entry, snapshot.c_str(), why))
    {
        error.sprintf("the recovery snapshot is no longer valid: %s", why.c_str());
        return false;
    }

    bool original_loaded = false;
    if (!s_startup_entry.source.empty())
    {
        xr_string source_why;
        if (!EScene::IsSceneFile(s_startup_entry.source.c_str(), source_why))
        {
            error.sprintf("cannot restore without the recorded source scene '%s': %s",
                s_startup_entry.source.c_str(), source_why.c_str());
            return false;
        }
        const bool previous_suppression = s_internal_source_load;
        s_internal_source_load = true;
        const bool loaded = !!ExecCommand(COMMAND_LOAD, xr_string(s_startup_entry.source.c_str()));
        s_internal_source_load = previous_suppression;
        if (!loaded)
        {
            error.sprintf("cannot load the original scene '%s'", s_startup_entry.source.c_str());
            return false;
        }
        original_loaded = true;
    }

    // A normal source load preserves its read-only external parts. The undo
    // snapshot then replaces editable tools only, exactly like Undo/Redo.
    Scene->Unload(TRUE);
    if (!Scene->Load(snapshot.c_str(), true))
    {
        if (original_loaded)
        {
            const bool previous_suppression = s_internal_source_load;
            s_internal_source_load = true;
            const bool rolled_back = !!ExecCommand(COMMAND_LOAD, xr_string(s_startup_entry.source.c_str()));
            s_internal_source_load = previous_suppression;
            if (!rolled_back)
            {
                UI->ResetStatus();
                ExecCommand(COMMAND_UPDATE_PROPERTIES);
                UI->UpdateScene();
                UI->RedrawScene();
                error = "cannot deserialize the recovery snapshot, and the original scene could not be reloaded";
                return false;
            }
        }
        error = "cannot deserialize the recovery snapshot";
        return false;
    }

    LTools->m_LastFileName = s_startup_entry.source.c_str();
    // Consume the deferred startup scene only after recovery succeeded. On a
    // failure, Discard can still continue with the project's normal last scene.
    EditorProject::PopPendingScene();
    Scene->UndoClear();
    Scene->UndoSave();
    ExecCommand(COMMAND_CLEAN_LIBRARY);
    ExecCommand(COMMAND_UPDATE_CAPTION);
    ExecCommand(COMMAND_CHANGE_ACTION, etaSelect);
    ExecCommand(COMMAND_UPDATE_PROPERTIES);
    UI->UpdateScene();
    UI->RedrawScene();
    ELog.Msg(mtInformation, "Recovered scene from '%s'. Save explicitly to update the original file.", snapshot.c_str());
    return true;
}

bool SourceIsNewer(const SRecoveryEntry& entry)
{
    if (entry.source.empty())
        return false;
    u64 modified = 0;
    u64 size = 0;
    if (!GetFileMetadata(entry.source.c_str(), modified, size))
        return true;
    return modified != entry.source_mtime || size != entry.source_size;
}

LPCSTR OperationStateName(EOperationState state)
{
    switch (state)
    {
    case EOperationState::Available:         return "available";
    case EOperationState::SnapshotCreated:  return "snapshot_created";
    case EOperationState::RestorePending:    return "restore_pending";
    case EOperationState::Restored:          return "restored";
    case EOperationState::DiscardPending:    return "discard_pending";
    case EOperationState::Discarded:         return "discarded";
    case EOperationState::Error:             return "error";
    default:                                 return s_snapshot_available ? "snapshot_ready" : "idle";
    }
}

u64 BeginOperation(EOperationState state)
{
    s_operation_id = ++s_next_operation_id;
    s_operation_state = state;
    s_last_operation_error.clear();
    return s_operation_id;
}

bool CanRestoreStartup(xr_string& error)
{
    if (!s_offer_recovery || !s_snapshot_available)
    {
        error = "no startup recovery snapshot is available";
        return false;
    }
    if (s_restore_requested || s_discard_requested)
    {
        error = "a recovery operation is already pending";
        return false;
    }
    if (!s_owns_marker || s_shared_project)
    {
        error = "this process does not own the project recovery session";
        return false;
    }
    if (!EditorProject::GameLinked())
    {
        error = "the project has no valid game link";
        return false;
    }
    if (!UI || !Scene || !Scene->valid() || UI->GetEState() != esEditScene || UI->ProgressOperationActive())
    {
        error = "the editor is busy";
        return false;
    }
    if (Scene->locked() || Scene->IsPlayInEditor())
    {
        error = Scene->locked() ? "the scene is locked" : "play in editor is active";
        return false;
    }
    if (Scene->IsUnsaved())
    {
        error = "the current scene has unsaved changes; save or clear it before restoring startup recovery";
        return false;
    }
    return true;
}

bool CanDiscardStartup(xr_string& error)
{
    if (!s_offer_recovery || !s_snapshot_available)
    {
        error = "no startup recovery snapshot is available";
        return false;
    }
    if (s_restore_requested || s_discard_requested)
    {
        error = "a recovery operation is already pending";
        return false;
    }
    if (!s_owns_marker || s_shared_project)
    {
        error = "this process does not own the project recovery session";
        return false;
    }
    return true;
}

bool ProcessPendingOperation()
{
    if (s_discard_requested)
    {
        s_discard_requested = false;
        xr_string error;
        if (!DeleteRecoveryData(error))
        {
            s_startup_error = error;
            s_last_operation_error = error;
            s_operation_state = EOperationState::Error;
            s_offer_recovery = true;
            return true;
        }
        s_offer_recovery = false;
        s_snapshot_available = false;
        s_startup_entry = {};
        s_startup_error.clear();
        s_last_operation_error.clear();
        s_operation_state = EOperationState::Discarded;
        ELog.Msg(mtInformation, "Recovery data discarded. The original scene was not changed.");
        return true;
    }

    if (s_restore_requested)
    {
        s_restore_requested = false;
        xr_string error;
        if (RestoreStartupEntry(error))
        {
            s_offer_recovery = false;
            s_startup_error.clear();
            s_last_operation_error.clear();
            s_operation_state = EOperationState::Restored;
            return true;
        }
        s_startup_error = error;
        s_last_operation_error = error;
        s_operation_state = EOperationState::Error;
        s_offer_recovery = true;
        return true;
    }
    return false;
}

void FormatSavedAt(u64 value, char* result, size_t result_size)
{
    FILETIME utc = MakeFileTime(value);
    FILETIME local;
    SYSTEMTIME time;
    if (::FileTimeToLocalFileTime(&utc, &local) && ::FileTimeToSystemTime(&local, &time))
        sprintf_s(result, result_size, "%04u-%02u-%02u %02u:%02u:%02u",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    else
        strcpy_s(result, result_size, "unknown time");
}
}

bool EditorRecovery::SaveNow(xr_string& error)
{
    error.clear();
    BindProject();
    if (!CanSerializeScene(error))
        return false;

    const u64 saved_at = CurrentFileTime();
    char snapshot_name[128];
    sprintf_s(snapshot_name, "snapshot-%016llx-%08lx-%04u.recovery",
        static_cast<unsigned long long>(saved_at), ::GetCurrentProcessId(), ++s_snapshot_sequence);
    const xr_string snapshot = JoinPath(s_recovery_dir.c_str(), snapshot_name);
    const xr_string temporary = TemporaryPath(snapshot.c_str());
    if (temporary.empty())
    {
        error = "cannot allocate a unique recovery snapshot name";
        return false;
    }

    CMemoryWriter serialized;
    Scene->SaveUndoStream(serialized, true);
    if (!serialized.size() || !WriteCheckedFile(temporary.c_str(), serialized.pointer(), serialized.size(), error))
    {
        if (error.empty())
            error = "recovery serializer produced an empty snapshot";
        return false;
    }
    xr_string why;
    if (!EScene::IsSceneFile(temporary.c_str(), why))
    {
        ::DeleteFileA(temporary.c_str());
        error.sprintf("recovery snapshot validation failed: %s", why.c_str());
        return false;
    }
    u64 snapshot_size = 0;
    u64 snapshot_hash = 0;
    if (!HashFile(temporary.c_str(), snapshot_size, snapshot_hash, error))
    {
        ::DeleteFileA(temporary.c_str());
        return false;
    }
    if (!::MoveFileExA(temporary.c_str(), snapshot.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        error.sprintf("cannot commit recovery snapshot (Windows error %lu)", ::GetLastError());
        ::DeleteFileA(temporary.c_str());
        return false;
    }

    SRecoveryEntry fresh;
    fresh.snapshot = snapshot_name;
    fresh.source = LTools->m_LastFileName.c_str();
    fresh.saved_at = saved_at;
    fresh.snapshot_size = snapshot_size;
    fresh.snapshot_hash = snapshot_hash;
    GetFileMetadata(fresh.source.c_str(), fresh.source_mtime, fresh.source_size);

    xr_vector<SRecoveryEntry> previous;
    ReadManifest(previous);
    xr_vector<SRecoveryEntry> retained;
    retained.push_back(fresh);
    for (const SRecoveryEntry& entry : previous)
    {
        if (retained.size() >= kMaximumManifestEntries ||
            0 == _stricmp(entry.snapshot.c_str(), fresh.snapshot.c_str()) || !IsSafeLeaf(entry.snapshot.c_str()))
            continue;
        retained.push_back(entry);
    }

    const CLevelPreferences* preferences = RecoveryPreferences();
    const u32 retention = preferences ? clampr(preferences->RecoveryRetention, 2u, kMaximumManifestEntries) : 10u;
    if (retained.size() > retention)
        retained.resize(retention);

    if (!WriteAtomicText(s_manifest_path.c_str(), BuildManifest(retained), error))
    {
        ::DeleteFileA(snapshot.c_str());
        return false;
    }

    DeleteSnapshotsNotIn(retained);
    s_startup_entry = fresh;
    s_snapshot_available = true;
    if (s_operation_state == EOperationState::Error)
        s_operation_state = EOperationState::Idle;
    s_last_operation_error.clear();
    s_next_save_at = ::GetTickCount64() +
        u64(preferences ? clampr(preferences->RecoveryIntervalMinutes, 1u, 120u) : 10u) * 60ull * 1000ull;
    Msg("* recovery: snapshot saved to '%s'", snapshot.c_str());
    return true;
}

bool EditorRecovery::SnapshotNow(u64& operation_id, xr_string& error)
{
    operation_id = 0;
    error.clear();
    BindProject();
    if (!CanSerializeScene(error))
        return false;
    operation_id = BeginOperation(EOperationState::SnapshotCreated);
    if (SaveNow(error))
        return true;
    s_operation_state = EOperationState::Error;
    s_last_operation_error = error;
    return false;
}

bool EditorRecovery::CanCreateSnapshot()
{
    xr_string error;
    return BindProject() && CanSerializeScene(error);
}

bool EditorRecovery::CanReplaceScene(xr_string& error)
{
    error.clear();
    if (s_internal_source_load)
        return true;
    BindProject();
    if (!s_offer_recovery)
        return true;
    error = "restore or discard the pending crash recovery before replacing the scene";
    return false;
}

void EditorRecovery::RequestSnapshot()
{
    s_manual_snapshot_requested = true;
}

bool EditorRecovery::RequestRestore(u64& operation_id, xr_string& error)
{
    operation_id = 0;
    error.clear();
    if (!BindProject() || !CanRestoreStartup(error))
        return false;
    operation_id = BeginOperation(EOperationState::RestorePending);
    s_restore_requested = true;
    if (UI)
        UI->RedrawScene();
    return true;
}

bool EditorRecovery::RequestDiscard(u64& operation_id, xr_string& error)
{
    operation_id = 0;
    error.clear();
    if (!BindProject() || !CanDiscardStartup(error))
        return false;
    operation_id = BeginOperation(EOperationState::DiscardPending);
    s_discard_requested = true;
    if (UI)
        UI->RedrawScene();
    return true;
}

void EditorRecovery::GetStatus(SStatus& status)
{
    status = {};
    const bool active = s_bound && EditorProject::Active() &&
        0 == _stricmp(s_project_root.c_str(), EditorProject::Root());
    const CLevelPreferences* preferences = RecoveryPreferences();
    status.enabled = preferences && !!preferences->RecoveryEnabled;
    status.owns_session = active && s_owns_marker;
    status.shared_session = active && s_shared_project;
    status.dirty = Scene && Scene->IsUnsaved();
    status.pending = active && (s_restore_requested || s_discard_requested);
    status.operation_id = s_operation_id;
    status.snapshot_available = active && s_snapshot_available;
    status.saved_at = status.snapshot_available ? s_startup_entry.saved_at : 0;
    status.source = status.snapshot_available ? s_startup_entry.source : xr_string();
    status.snapshot = status.snapshot_available ? JoinPath(s_recovery_dir.c_str(), s_startup_entry.snapshot.c_str()) : xr_string();
    status.source_newer = status.snapshot_available && SourceIsNewer(s_startup_entry);
    if (active)
    {
        xr_string ignored;
        status.can_snapshot = CanSerializeScene(ignored);
        status.can_restore = CanRestoreStartup(ignored);
        status.can_discard = CanDiscardStartup(ignored);
    }
    status.last_error = !s_last_operation_error.empty() ? s_last_operation_error : s_startup_error;
    if (!active)
        status.state = "unavailable";
    else if (s_shared_project)
        status.state = "shared_session";
    else
        status.state = OperationStateName(s_operation_state);
}

void EditorRecovery::Tick()
{
    if (!EditorProject::Active())
    {
        if (s_bound)
            EndProjectSession();
        return;
    }
    if (!EditorProject::GameLinked() || !BindProject())
        return;
    if (ProcessPendingOperation())
        return;

    if (s_manual_snapshot_requested)
    {
        xr_string error;
        if (!CanSerializeScene(error))
        {
            if (SnapshotTemporarilyBlocked())
                return;
            s_manual_snapshot_requested = false;
            ELog.Msg(mtError, "Can't create recovery snapshot: %s", error.c_str());
            return;
        }
        s_manual_snapshot_requested = false;
        if (SaveNow(error))
            ELog.Msg(mtInformation, "Recovery snapshot created. The scene file and dirty state were not changed.");
        else
            ELog.Msg(mtError, "Can't create recovery snapshot: %s", error.c_str());
    }

    CLevelPreferences* preferences = RecoveryPreferences();
    if (!preferences || !preferences->RecoveryEnabled || s_shared_project)
        return;

    const u64 now = ::GetTickCount64();
    const u64 interval = u64(clampr(preferences->RecoveryIntervalMinutes, 1u, 120u)) * 60ull * 1000ull;
    if (!Scene || !Scene->IsUnsaved())
    {
        s_next_save_at = now + interval;
        return;
    }
    if (now < s_next_save_at || !UserInteractionIdle(clampr(preferences->RecoveryInteractionDelaySeconds, 0u, 120u)))
        return;

    xr_string error;
    if (!CanSerializeScene(error))
        return;
    if (!SaveNow(error))
    {
        s_next_save_at = now + kRetryDelayMs;
        Msg("! recovery: autosave failed: %s", error.c_str());
    }
}

bool EditorRecovery::DrawStartupRecovery()
{
    if (!BindProject())
        return false;
    ProcessPendingOperation();

    if (!s_offer_recovery)
        return false;

    ImGui::OpenPopup("Recover Unsaved Scene");
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680, 300), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Recover Unsaved Scene", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        char saved_at[64];
        FormatSavedAt(s_startup_entry.saved_at, saved_at, sizeof(saved_at));
        ImGui::TextUnformatted("The previous editor session did not close cleanly.");
        ImGui::TextWrapped("A validated recovery snapshot is available. Restoring opens it as an unsaved scene; it never overwrites the original file.");
        ImGui::Separator();
        ImGui::Text("Snapshot: %s", saved_at);
        ImGui::TextWrapped("Original: %s", s_startup_entry.source.empty() ? "(untitled scene)" : s_startup_entry.source.c_str());
        if (SourceIsNewer(s_startup_entry))
            ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f),
                "The original scene changed after this snapshot. Restore only if you need the older unsaved state.");
        if (!s_startup_error.empty())
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%s", s_startup_error.c_str());
        ImGui::Separator();
        if (ImGui::Button("Restore Recovery", ImVec2(180, 0)))
        {
            u64 operation_id = 0;
            xr_string error;
            if (EditorRecovery::RequestRestore(operation_id, error))
                ImGui::CloseCurrentPopup();
            else
                s_startup_error = error;
        }
        ImGui::SameLine(0, 16);
        if (ImGui::Button("Discard Recovery", ImVec2(180, 0)))
        {
            u64 operation_id = 0;
            xr_string error;
            if (EditorRecovery::RequestDiscard(operation_id, error))
                ImGui::CloseCurrentPopup();
            else
                s_startup_error = error;
        }
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("The original scene is untouched");
        ImGui::EndPopup();
    }
    return true;
}

void EditorRecovery::OnSceneSaved()
{
    if (!BindProject() || s_shared_project || !s_owns_marker)
        return;
    if (s_offer_recovery)
    {
        Msg("! recovery: saved scene did not discard the unresolved startup recovery");
        return;
    }
    const bool had_manifest = FileExists(s_manifest_path.c_str());
    xr_string error;
    if (!DeleteRecoveryData(error))
    {
        Msg("! recovery: cannot remove obsolete snapshots after save: %s", error.c_str());
        return;
    }
    s_offer_recovery = false;
    s_preserve_on_shutdown = false;
    s_snapshot_available = false;
    s_startup_entry = {};
    s_startup_error.clear();
    if (!s_operation_id &&
        (s_operation_state == EOperationState::Available || s_operation_state == EOperationState::Error))
    {
        s_operation_state = EOperationState::Idle;
        s_last_operation_error.clear();
    }
    const CLevelPreferences* preferences = RecoveryPreferences();
    s_next_save_at = ::GetTickCount64() +
        u64(preferences ? clampr(preferences->RecoveryIntervalMinutes, 1u, 120u) : 10u) * 60ull * 1000ull;
    if (had_manifest)
        Msg("* recovery: obsolete snapshots removed after the scene was saved");
}

void EditorRecovery::OnSceneReplaced()
{
    if (s_internal_source_load || !BindProject() || s_shared_project || !s_owns_marker)
        return;
    const bool had_manifest = FileExists(s_manifest_path.c_str());
    xr_string error;
    if (!DeleteRecoveryData(error))
    {
        Msg("! recovery: cannot reset snapshots after the scene changed: %s", error.c_str());
        return;
    }
    s_offer_recovery = false;
    s_preserve_on_shutdown = false;
    s_snapshot_available = false;
    s_startup_entry = {};
    s_startup_error.clear();
    if (!s_operation_id &&
        (s_operation_state == EOperationState::Available || s_operation_state == EOperationState::Error))
    {
        s_operation_state = EOperationState::Idle;
        s_last_operation_error.clear();
    }
    const CLevelPreferences* preferences = RecoveryPreferences();
    s_next_save_at = ::GetTickCount64() +
        u64(preferences ? clampr(preferences->RecoveryIntervalMinutes, 1u, 120u) : 10u) * 60ull * 1000ull;
    if (had_manifest)
        Msg("* recovery: snapshots reset for the newly opened scene");
}

void EditorRecovery::OnBackupSaved()
{
    s_preserve_on_shutdown = true;
}

void EditorRecovery::Shutdown()
{
    if (s_bound)
        EndProjectSession();
}
