#pragma once

namespace EditorRecovery
{
    struct SStatus
    {
        xr_string state;
        xr_string source;
        xr_string snapshot;
        xr_string last_error;
        u64 operation_id = 0;
        u64 saved_at = 0;
        bool pending = false;
        bool enabled = false;
        bool owns_session = false;
        bool shared_session = false;
        bool dirty = false;
        bool can_snapshot = false;
        bool can_restore = false;
        bool can_discard = false;
        bool snapshot_available = false;
        bool source_newer = false;
    };

    void Tick();
    bool DrawStartupRecovery();
    bool SaveNow(xr_string& error);
    bool SnapshotNow(u64& operation_id, xr_string& error);
    bool CanCreateSnapshot();
    bool CanReplaceScene(xr_string& error);
    void RequestSnapshot();
    bool RequestRestore(u64& operation_id, xr_string& error);
    bool RequestDiscard(u64& operation_id, xr_string& error);
    void GetStatus(SStatus& status);
    void OnBackupSaved();
    void OnSceneSaved();
    void OnSceneReplaced();
    void Shutdown();
}
