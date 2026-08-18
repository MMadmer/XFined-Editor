#include "stdafx.h"
#include "LEPhysics.h"
#include "..\..\..\XrECore\Editor\UI_ViewportNavigation.h"

// file: SceneChunks.h
#define CURRENT_FILE_VERSION    	0x00000005

#define CURRENT_LEVELOP_VERSION 	0x0000000C
//0x00000008

#define CURRENT_LEVELOP_BP_VERSION 	0x00000009
#define CURRENT_ENV_VERSION	 		0x00000007

#define CHUNK_VERSION       	0x9df3
#define CHUNK_OBJECT_CLASS  	0x7703
#define CHUNK_OBJECT_LIST		0x7708
#define CHUNK_CAMERA        	0x7709
#define CHUNK_SNAPOBJECTS   	0x7710
#define CHUNK_LEVELOP       	0x7711
#define CHUNK_OBJECT_COUNT  	0x7712
#define CHUNK_LEVEL_TAG			0x7777

#define CHUNK_TOOLS_GUID		0x7000
#define CHUNK_TOOLS_DATA		0x8000

// level options
#define CHUNK_LO_VERSION		0x7801
#define CHUNK_LO_NAMES 			0x7802
#define CHUNK_LO_BOP		 	0x7803
#define CHUNK_LO_PREFIX 		0x7804
#define CHUNK_LO_BP_VERSION		0x7849
#define CHUNK_BUILD_PARAMS		0x7850
#define CHUNK_LIGHT_QUALITY		0x7851
#define CHUNK_MAP_USAGE			0x7852
#define CHUNK_LO_MAP_VER	 	0x7853

namespace
{
void SetSaveError(xr_string* error, LPCSTR message, LPCSTR path)
{
	if (!error)
		return;
	*error = message;
	*error += path;
}

void SetLoadError(xr_string* error, LPCSTR message, LPCSTR path = "")
{
	if (!error)
		return;
	*error = message;
	*error += path;
}

bool HasIniLines(CInifile& ini, LPCSTR section, const LPCSTR* lines, u32 count)
{
	if (!ini.section_exist(section))
		return false;
	for (u32 index = 0; index < count; ++index)
	{
		if (!ini.line_exist(section, lines[index]))
			return false;
	}
	return true;
}

bool HasTerminatedString(IReader& reader, u32 trailing_bytes = 0)
{
	const u32 size = reader.elapsed();
	if (size <= trailing_bytes)
		return false;
	const u8* begin = static_cast<const u8*>(reader.pointer());
	const u8* terminator = static_cast<const u8*>(memchr(begin, 0, size - trailing_bytes));
	return terminator && u32(terminator - begin + 1) + trailing_bytes == size;
}

void SetWindowsSaveError(xr_string* error, LPCSTR operation, LPCSTR path, DWORD code)
{
	if (!error)
		return;
	error->sprintf("%s '%s' (Windows error %lu)", operation, path, static_cast<unsigned long>(code));
}

bool QuerySaveFile(LPCSTR path, bool& exists, xr_string* error)
{
	const DWORD attributes = ::GetFileAttributesA(path);
	if (attributes != INVALID_FILE_ATTRIBUTES)
	{
		if (attributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			SetSaveError(error, "Save target is a directory: ", path);
			return false;
		}
		exists = true;
		return true;
	}

	const DWORD code = ::GetLastError();
	if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
	{
		exists = false;
		return true;
	}

	SetWindowsSaveError(error, "Cannot inspect save target", path, code);
	return false;
}

bool WriteCheckedSaveFile(LPCSTR path, const void* data, u32 size, xr_string* error)
{
	VerifyPath(path);
	HANDLE file = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		SetWindowsSaveError(error, "Cannot create staged save file", path, ::GetLastError());
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
	if (!::CloseHandle(file) && write_error == ERROR_SUCCESS)
		write_error = ::GetLastError();

	if (write_error == ERROR_SUCCESS && offset == size)
		return true;

	if (write_error == ERROR_SUCCESS)
		write_error = ERROR_WRITE_FAULT;
	::DeleteFileA(path);
	SetWindowsSaveError(error, "Cannot write staged save file", path, write_error);
	return false;
}

xr_string LegacySaveBackupName(LPCSTR target)
{
	LPCSTR extension = strext(target);
	if (!extension)
		return xr_string(target) + "~";

	xr_string backup_extension = extension;
	backup_extension.insert(1, "~");
	return EFS.ChangeFileExt(target, backup_extension.c_str());
}

struct SaveTransactionEntry
{
	xr_string Target;
	xr_string Staged;
	xr_string Rollback;
	xr_string Backup;
	xr_string BackupStaged;
	xr_string BackupRollback;
	bool Remove = false;
	bool KeepLegacyBackup = false;
	bool HadTarget = false;
	bool HadBackup = false;
	bool TargetTouched = false;
	bool BackupTouched = false;
};

// The transaction is failure-atomic inside this process, but it is not a multi-file crash journal.
class SaveTransaction
{
public:
	~SaveTransaction()
	{
		if (!m_PreserveRecovery)
			Cleanup();
	}

	bool Initialize(LPCSTR anchor, xr_string* error)
	{
		xr_string parent = EFS.ExtractFilePath(anchor);
		if (parent.empty())
			parent = ".\\";

		for (u32 attempt = 0; attempt < 100; ++attempt)
		{
			m_Directory.sprintf("%s.xfsave-%lu-%llu-%u", parent.c_str(),
				static_cast<unsigned long>(::GetCurrentProcessId()),
				static_cast<unsigned long long>(::GetTickCount64()), attempt);
			VerifyPath(m_Directory.c_str());
			if (::CreateDirectoryA(m_Directory.c_str(), nullptr))
				return true;
			if (::GetLastError() != ERROR_ALREADY_EXISTS)
			{
				SetWindowsSaveError(error, "Cannot create save transaction directory", m_Directory.c_str(),
					::GetLastError());
				m_Directory.clear();
				return false;
			}
		}

		SetSaveError(error, "Cannot allocate a unique save transaction directory near: ", anchor);
		m_Directory.clear();
		return false;
	}

	bool StageFile(LPCSTR target, const void* data, u32 size, bool keep_legacy_backup, xr_string* error)
	{
		if (!CanAddTarget(target, error))
			return false;

		SaveTransactionEntry entry;
		entry.Target = target;
		entry.Staged = TemporaryName('s', m_Entries.size());
		entry.KeepLegacyBackup = keep_legacy_backup;
		if (!WriteCheckedSaveFile(entry.Staged.c_str(), data, size, error))
			return false;

		m_Entries.push_back(entry);
		return true;
	}

	bool StageRemoval(LPCSTR target, bool keep_legacy_backup, xr_string* error)
	{
		if (!CanAddTarget(target, error))
			return false;

		bool exists = false;
		if (!QuerySaveFile(target, exists, error))
			return false;
		if (!exists)
			return true;

		SaveTransactionEntry entry;
		entry.Target = target;
		entry.Remove = true;
		entry.KeepLegacyBackup = keep_legacy_backup;
		m_Entries.push_back(entry);
		return true;
	}

	bool Commit(xr_string* error)
	{
		if (!PrepareRollback(error))
			return false;

		xr_string failure;
		for (SaveTransactionEntry& entry : m_Entries)
		{
			if (!CommitTarget(entry, &failure))
				return FailAndRollback(failure, error);
			entry.TargetTouched = !entry.Remove || entry.HadTarget;
		}

		for (SaveTransactionEntry& entry : m_Entries)
		{
			if (!entry.HadTarget || !entry.KeepLegacyBackup)
				continue;
			if (!CommitReplacement(entry.BackupStaged.c_str(), entry.Backup.c_str(), entry.HadBackup,
				"Cannot commit save backup", &failure))
				return FailAndRollback(failure, error);
			entry.BackupTouched = true;
			entry.BackupStaged.clear();
		}

		Cleanup();
		return true;
	}

private:
	xr_string TemporaryName(char kind, size_t index) const
	{
		xr_string result;
		result.sprintf("%s\\%c%08u.tmp", m_Directory.c_str(), kind, static_cast<u32>(index));
		return result;
	}

	bool CanAddTarget(LPCSTR target, xr_string* error) const
	{
		for (const SaveTransactionEntry& entry : m_Entries)
		{
			if (!_stricmp(entry.Target.c_str(), target))
			{
				SetSaveError(error, "Duplicate output in save transaction: ", target);
				return false;
			}
		}
		return true;
	}

	bool CopyForRollback(LPCSTR source, LPCSTR destination, LPCSTR operation, xr_string* error)
	{
		if (::CopyFileA(source, destination, TRUE))
			return true;
		SetWindowsSaveError(error, operation, source, ::GetLastError());
		return false;
	}

	bool PrepareRollback(xr_string* error)
	{
		for (size_t index = 0; index < m_Entries.size(); ++index)
		{
			SaveTransactionEntry& entry = m_Entries[index];
			if (!QuerySaveFile(entry.Target.c_str(), entry.HadTarget, error))
				return false;

			if (!entry.HadTarget)
			{
				if (entry.Remove)
					entry.TargetTouched = false;
				continue;
			}

			entry.Rollback = TemporaryName('r', index);
			if (!CopyForRollback(entry.Target.c_str(), entry.Rollback.c_str(),
				"Cannot stage canonical rollback copy", error))
				return false;

			if (!entry.KeepLegacyBackup)
				continue;

			entry.Backup = LegacySaveBackupName(entry.Target.c_str());
			entry.BackupStaged = TemporaryName('b', index);
			if (!CopyForRollback(entry.Target.c_str(), entry.BackupStaged.c_str(),
				"Cannot stage legacy save backup", error))
				return false;

			if (!QuerySaveFile(entry.Backup.c_str(), entry.HadBackup, error))
				return false;
			if (!entry.HadBackup)
				continue;

			entry.BackupRollback = TemporaryName('o', index);
			if (!CopyForRollback(entry.Backup.c_str(), entry.BackupRollback.c_str(),
				"Cannot stage previous save backup", error))
				return false;
		}
		return true;
	}

	bool CommitReplacement(LPCSTR staged, LPCSTR target, bool target_exists, LPCSTR operation,
		xr_string* error)
	{
		VerifyPath(target);
		const bool result = target_exists
			? !!::ReplaceFileA(target, staged, nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
			: !!::MoveFileExA(staged, target, MOVEFILE_WRITE_THROUGH);
		if (result)
			return true;
		SetWindowsSaveError(error, operation, target, ::GetLastError());
		return false;
	}

	bool CommitTarget(SaveTransactionEntry& entry, xr_string* error)
	{
		if (entry.Remove)
		{
			if (!entry.HadTarget || ::DeleteFileA(entry.Target.c_str()))
				return true;
			SetWindowsSaveError(error, "Cannot remove stale level part", entry.Target.c_str(), ::GetLastError());
			return false;
		}

		if (!CommitReplacement(entry.Staged.c_str(), entry.Target.c_str(), entry.HadTarget,
			"Cannot commit staged save file", error))
			return false;
		entry.Staged.clear();
		return true;
	}

	bool RestoreFile(LPCSTR source, LPCSTR target, LPCSTR operation, xr_string& error)
	{
		VerifyPath(target);
		if (::MoveFileExA(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		SetWindowsSaveError(&error, operation, target, ::GetLastError());
		return false;
	}

	bool RemoveRestoredNewFile(LPCSTR path, LPCSTR operation, xr_string& error)
	{
		if (::DeleteFileA(path))
			return true;
		const DWORD code = ::GetLastError();
		if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
			return true;
		SetWindowsSaveError(&error, operation, path, code);
		return false;
	}

	bool Rollback(xr_string& rollback_error)
	{
		bool result = true;
		for (auto entry = m_Entries.rbegin(); entry != m_Entries.rend(); ++entry)
		{
			if (!entry->BackupTouched)
				continue;
			xr_string current_error;
			const bool restored = entry->HadBackup
				? RestoreFile(entry->BackupRollback.c_str(), entry->Backup.c_str(),
					"Cannot restore previous save backup", current_error)
				: RemoveRestoredNewFile(entry->Backup.c_str(), "Cannot remove new save backup during rollback",
					current_error);
			if (!restored && result)
				rollback_error = current_error;
			result = restored && result;
		}

		for (auto entry = m_Entries.rbegin(); entry != m_Entries.rend(); ++entry)
		{
			if (!entry->TargetTouched)
				continue;
			xr_string current_error;
			const bool restored = entry->HadTarget
				? RestoreFile(entry->Rollback.c_str(), entry->Target.c_str(),
					"Cannot restore canonical save file", current_error)
				: RemoveRestoredNewFile(entry->Target.c_str(), "Cannot remove new canonical file during rollback",
					current_error);
			if (!restored && result)
				rollback_error = current_error;
			result = restored && result;
		}
		return result;
	}

	bool FailAndRollback(const xr_string& failure, xr_string* error)
	{
		xr_string rollback_error;
		if (Rollback(rollback_error))
		{
			if (error)
				*error = failure;
			Cleanup();
			return false;
		}

		m_PreserveRecovery = true;
		Msg("! Save rollback is incomplete; transaction files remain in '%s'", m_Directory.c_str());
		for (const SaveTransactionEntry& entry : m_Entries)
		{
			Msg("! Save recovery target='%s' staged='%s' rollback='%s' backup='%s' previous_backup='%s'",
				entry.Target.c_str(), entry.Staged.c_str(), entry.Rollback.c_str(), entry.Backup.c_str(),
				entry.BackupRollback.c_str());
		}
		if (error)
		{
			error->sprintf("%s; %s; recovery files kept in '%s'", failure.c_str(), rollback_error.c_str(),
				m_Directory.c_str());
		}
		return false;
	}

	void CleanupFile(const xr_string& path)
	{
		if (path.empty() || ::DeleteFileA(path.c_str()))
			return;
		const DWORD code = ::GetLastError();
		if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
			Msg("! Cannot clean save transaction file '%s' (Windows error %lu)", path.c_str(),
				static_cast<unsigned long>(code));
	}

	void Cleanup()
	{
		for (SaveTransactionEntry& entry : m_Entries)
		{
			CleanupFile(entry.Staged);
			CleanupFile(entry.Rollback);
			CleanupFile(entry.BackupStaged);
			CleanupFile(entry.BackupRollback);
		}
		if (!m_Directory.empty() && !::RemoveDirectoryA(m_Directory.c_str()))
		{
			const DWORD code = ::GetLastError();
			if (code != ERROR_PATH_NOT_FOUND)
				Msg("! Cannot clean save transaction directory '%s' (Windows error %lu)", m_Directory.c_str(),
					static_cast<unsigned long>(code));
		}
		m_Entries.clear();
		m_Directory.clear();
	}

private:
	xr_string m_Directory;
	xr_vector<SaveTransactionEntry> m_Entries;
	bool m_PreserveRecovery = false;
};

xr_string IndexedLevelPartName(LPCSTR base_name, int index)
{
	if (!index)
		return base_name;
	xr_string result;
	result.sprintf("%s%d", base_name, index);
	return result;
}

bool StageToolLTX(SaveTransaction& transaction, ESceneToolBase* tool, xrGUID& guid, LPCSTR file_name,
	bool keep_legacy_backup, xr_string* error)
{
	const int file_count = tool->SaveFileCount();
	if (file_count < 1)
	{
		SetSaveError(error, "Level part reported an invalid save file count: ", file_name);
		return false;
	}

	for (int index = 0; index < file_count; ++index)
	{
		const xr_string output = IndexedLevelPartName(file_name, index);
		CInifile ini(output.c_str(), FALSE, FALSE, FALSE);
		tool->SaveLTX(ini, index);
		guid.SaveLTX(ini, "guid", "guid");

		CMemoryWriter serialized;
		ini.save_as(serialized);
		if (!transaction.StageFile(output.c_str(), serialized.pointer(), serialized.size(), keep_legacy_backup,
			error))
			return false;
	}

	// The loader consumes numbered parts until the first gap, so obsolete suffixes must disappear atomically.
	for (int index = file_count;; ++index)
	{
		const xr_string stale = IndexedLevelPartName(file_name, index);
		bool exists = false;
		if (!QuerySaveFile(stale.c_str(), exists, error))
			return false;
		if (!exists)
			break;
		if (!transaction.StageRemoval(stale.c_str(), keep_legacy_backup, error))
			return false;
	}
	return true;
}
}

// Level Options

void st_LevelOptions::SaveLTX( CInifile& ini )
{
	LPCSTR section 	= "level_options";
	ini.w_u32		(section, "version", CURRENT_LEVELOP_VERSION);

	ini.w_string	(section, "level_path", m_FNLevelPath.c_str());
	ini.w_string	(section, "level_prefix", m_LevelPrefix.c_str());
	xr_string s		= "\"";
    s 				+= m_BOPText.c_str();
    s				+= "\"";
	ini.w_string	(section, "bop", s.c_str());
	ini.w_string	(section, "map_version", m_map_version.c_str());

	ini.w_u32		(section, "version_bp", CURRENT_LEVELOP_BP_VERSION);

	m_BuildParams.SaveLTX	(ini);

	ini.w_u8		(section, "light_hemi_quality", m_LightHemiQuality );
	ini.w_u8		(section, "light_sun_quality", m_LightSunQuality );

    m_mapUsage.SaveLTX(ini,section);
}

void st_LevelOptions::Save( IWriter& F )
{
    F.open_chunk( CHUNK_LO_VERSION );
	F.w_u32		( CURRENT_LEVELOP_VERSION );
    F.close_chunk();

    F.open_chunk( CHUNK_LO_NAMES );
	F.w_stringZ	( m_FNLevelPath.size()?m_FNLevelPath.c_str():"" );
    F.close_chunk();

    F.open_chunk( CHUNK_LO_PREFIX );
	F.w_stringZ	( m_LevelPrefix.size()?m_LevelPrefix.c_str():"");
    F.close_chunk();

    F.open_chunk( CHUNK_LO_BOP );
	F.w_stringZ	( m_BOPText.size()?m_BOPText.c_str():"" );
    F.close_chunk();

    F.open_chunk( CHUNK_LO_MAP_VER );
	F.w_stringZ	( m_map_version.size()?m_map_version.c_str():"1.0" );
    F.close_chunk();

    F.open_chunk( CHUNK_LO_BP_VERSION );
	F.w_u32		( CURRENT_LEVELOP_BP_VERSION );
    F.close_chunk();

    F.open_chunk( CHUNK_BUILD_PARAMS );
	F.w			( &m_BuildParams, sizeof(m_BuildParams) );
    F.close_chunk();

    F.open_chunk( CHUNK_LIGHT_QUALITY );
	F.w_u8		( m_LightHemiQuality );
	F.w_u8		( m_LightSunQuality );
    F.close_chunk();

    F.open_chunk( CHUNK_MAP_USAGE );
	F.w_u16		( m_mapUsage.m_GameType.get() );
    F.close_chunk();
}

bool st_LevelOptions::ReadLTX(CInifile& ini)
{
	LPCSTR section 	= "level_options";
	if (!ini.section_exist(section) || !ini.line_exist(section, "version") ||
		!ini.line_exist(section, "level_path") || !ini.line_exist(section, "level_prefix") ||
		!ini.line_exist(section, "bop") || !ini.line_exist(section, "light_hemi_quality") ||
		!ini.line_exist(section, "light_sun_quality"))
		return false;

    u32 vers_op 		= ini.r_u32(section, "version");
    if( vers_op < 0x00000008 )
    {
		ELog.Msg(mtError, "Skipping bad version of level options.");
        return false;
    }
	static LPCSTR build_lines[] = {
		"smooth_angle", "weld_distance", "light_pixel_per_meter", "light_jitter_samples",
		"light_rms_zero", "light_rms", "light_quality", "light_quality_reserved",
		"reserved_0", "reserved_1", "reserved_2", "reserved_3", "reserved_4", "reserved_5",
	};
	if (!HasIniLines(ini, "build_params", build_lines, _countof(build_lines)) ||
		(vers_op > 0x0000000B && !ini.line_exist(section, "map_version")) ||
		(vers_op > 0x0000000A && !ini.line_exist(section, "game_type")))
		return false;
	if (vers_op <= 0x0000000A)
	{
		static LPCSTR legacy_usage[] = {"usage_deathmatch", "usage_teamdeathmatch", "usage_artefacthunt"};
		if (!HasIniLines(ini, section, legacy_usage, _countof(legacy_usage)))
			return false;
		if (vers_op > 0x00000008 &&
			(!ini.line_exist(section, "usage_captretheartefact") ||
				!ini.line_exist(section, "usage_team_domination_zone") ||
				!ini.line_exist(section, vers_op == 0x00000009 ? "domination_zone" : "usage_domination_zone")))
			return false;
	}

    m_FNLevelPath		= ini.r_string 		(section, "level_path");
    m_LevelPrefix		= ini.r_string 		(section, "level_prefix");
    m_BOPText			= ini.r_string_wb	(section, "bop");

    if(vers_op > 0x0000000B)
    	m_map_version		= ini.r_string		(section, "map_version");

    m_BuildParams.LoadLTX(ini);

    m_LightHemiQuality 				= ini.r_u8(section, "light_hemi_quality" );
    m_LightSunQuality 				= ini.r_u8(section, "light_sun_quality" );

    m_mapUsage.SetDefaults			();
    if(vers_op > 0x0000000A)
	{
     m_mapUsage.LoadLTX				(ini,section,false);
    }else
    {

    m_mapUsage.m_GameType.set		(eGameIDDeathmatch ,	ini.r_s32(section, "usage_deathmatch"));
    m_mapUsage.m_GameType.set		(eGameIDTeamDeathmatch, ini.r_s32(section, "usage_teamdeathmatch"));
    m_mapUsage.m_GameType.set		(eGameIDArtefactHunt,	ini.r_s32(section, "usage_artefacthunt"));


	if(vers_op > 0x00000008)
    {
        m_mapUsage.m_GameType.set	(eGameIDCaptureTheArtefact,	ini.r_s32(section, "usage_captretheartefact"));

        m_mapUsage.m_GameType.set	(eGameIDTeamDominationZone,	ini.r_s32(section, "usage_team_domination_zone"));
        if(vers_op==0x00000009)
        	m_mapUsage.m_GameType.set(eGameIDDominationZone,		ini.r_s32(section, "domination_zone"));
        else
        	m_mapUsage.m_GameType.set(eGameIDDominationZone,		ini.r_s32(section, "usage_domination_zone"));
     }
    }
	return true;
}

bool st_LevelOptions::Read(IReader& F)
{
	if (F.find_chunk(CHUNK_LO_VERSION) != sizeof(u32))
		return false;
	const DWORD level_version = F.r_u32();
	if (level_version < 0x00000008)
	{
		ELog.Msg(mtError, "Skipping bad version of level options.");
		return false;
	}

	auto read_string_chunk = [&F](u32 chunk_id, shared_str& value, bool required)
	{
		IReader* chunk = F.open_chunk(chunk_id);
		if (!chunk)
			return !required;
		const bool valid = chunk->elapsed() && memchr(chunk->pointer(), 0, chunk->elapsed());
		if (valid)
			chunk->r_stringZ(value);
		chunk->close();
		return valid;
	};
	if (!read_string_chunk(CHUNK_LO_NAMES, m_FNLevelPath, true) ||
		!read_string_chunk(CHUNK_LO_PREFIX, m_LevelPrefix, false) ||
		!read_string_chunk(CHUNK_LO_BOP, m_BOPText, true) ||
		!read_string_chunk(CHUNK_LO_MAP_VER, m_map_version, false))
		return false;

	DWORD build_version = 0;
	if (IReader* version_chunk = F.open_chunk(CHUNK_LO_BP_VERSION))
	{
		if (version_chunk->elapsed() != sizeof(build_version))
		{
			version_chunk->close();
			return false;
		}
		build_version = version_chunk->r_u32();
		version_chunk->close();
	}

	if (CURRENT_LEVELOP_BP_VERSION == build_version)
	{
		IReader* build_params = F.open_chunk(CHUNK_BUILD_PARAMS);
		if (!build_params || build_params->elapsed() != sizeof(m_BuildParams))
		{
			if (build_params)
				build_params->close();
			return false;
		}
		build_params->r(&m_BuildParams, sizeof(m_BuildParams));
		build_params->close();
	}
	else
	{
		ELog.Msg(mtError, "Skipping bad version of build params.");
		m_BuildParams.Init();
	}

	if (IReader* light_quality = F.open_chunk(CHUNK_LIGHT_QUALITY))
	{
		if (light_quality->elapsed() != sizeof(u8) * 2)
		{
			light_quality->close();
			return false;
		}
		m_LightHemiQuality = light_quality->r_u8();
		m_LightSunQuality = light_quality->r_u8();
		light_quality->close();
	}
	if (IReader* map_usage = F.open_chunk(CHUNK_MAP_USAGE))
	{
		const u32 expected_size = build_version > 0x00000008 ? sizeof(u16) : sizeof(s32) * 3;
		if (map_usage->elapsed() != expected_size)
		{
			map_usage->close();
			return false;
		}
		if (build_version > 0x00000008)
			m_mapUsage.m_GameType.assign(map_usage->r_u16());
		else
		{
			m_mapUsage.m_GameType.zero();
			m_mapUsage.m_GameType.set(eGameIDDeathmatch, map_usage->r_s32());
			m_mapUsage.m_GameType.set(eGameIDTeamDeathmatch, map_usage->r_s32());
			m_mapUsage.m_GameType.set(eGameIDArtefactHunt, map_usage->r_s32());
		}
		map_usage->close();
	}
	return true;
}


// Scene

BOOL EScene::LoadLevelPartLTX(ESceneToolBase* M, LPCSTR mn, xr_string* error)
{
	if (!M)
	{
		SetLoadError(error, "Level part has no matching editor tool: ", mn);
		return FALSE;
	}
	string_path map_name;
    strcpy(map_name, mn);
    
	if(!M->can_use_inifile())
		return LoadLevelPartStream(M, map_name, error);

    int fnidx=0;
    while(FS.exist(map_name))
    {
        IReader* R		= FS.r_open	(map_name);
		if (!R)
		{
			SetLoadError(error, "Cannot open level part: ", map_name);
			return FALSE;
		}
		if (!R->elapsed())
		{
			FS.r_close(R);
			SetLoadError(error, "Level part is empty: ", map_name);
			return FALSE;
		}
        char 			ch;
        R->r			(&ch,sizeof(ch));
        bool b_is_inifile = (ch=='[');
        FS.r_close		(R);

        if(!b_is_inifile)
			return LoadLevelPartStream(M, map_name, error);

        M->m_EditFlags.set(ESceneToolBase::flReadonly,FALSE);

        CInifile			ini(map_name);

        // check level part GUID
		if (!ini.section_exist("guid") || !ini.line_exist("guid", "guid_g0") ||
			!ini.line_exist("guid", "guid_g1"))
		{
			SetLoadError(error, "Level part has no GUID section: ", map_name);
			return FALSE;
		}
        xrGUID				guid;
        guid.LoadLTX		(ini, "guid", "guid");

        if (guid!=m_GUID)
        {
			ELog.Msg(mtError, "Skipping invalid version of level part: '%s\\%s.part'",
				EFS.ExtractFileName(map_name).c_str(), M->ClassName());
			SetLoadError(error, "Level part GUID does not match the scene: ", map_name);
            return 			FALSE;
        }
        // read data
		if (!M->LoadLTX(ini))
		{
			SetLoadError(error, "Editor tool rejected level part: ", map_name);
			return FALSE;
		}

		++fnidx;
        sprintf(map_name, "%s%d", mn, fnidx);
    }

    return 					TRUE;
}

BOOL EScene::LoadLevelPart(ESceneToolBase* M, LPCSTR map_name, xr_string* error)
{
	if (M && M->can_use_inifile())
	    return LoadLevelPartLTX(M, map_name, error);
	return LoadLevelPartStream(M, map_name, error);
	
}

BOOL EScene::LoadLevelPartStream(ESceneToolBase* M, LPCSTR map_name, xr_string* error)
{
	if (!M)
	{
		SetLoadError(error, "Level part has no matching editor tool: ", map_name);
		return FALSE;
	}
    if (FS.exist(map_name))
    {
        // check locking
        M->m_EditFlags.set(ESceneToolBase::flReadonly, FALSE);

        IReader* R = FS.r_open(map_name);
		if (!R)
		{
			SetLoadError(error, "Cannot open level part: ", map_name);
			return FALSE;
		}
        // check level part GUID
		if (R->find_chunk(CHUNK_TOOLS_GUID) != sizeof(xrGUID))
		{
			FS.r_close(R);
			SetLoadError(error, "Level part has an invalid GUID chunk: ", map_name);
			return FALSE;
		}
        xrGUID			guid;
        R->r(&guid, sizeof(guid));

        if (guid != m_GUID)
        {
			ELog.Msg(mtError, "Skipping invalid version of level part: '%s\\%s.part'",
				EFS.ExtractFileName(map_name).c_str(), M->ClassName());
            FS.r_close(R);
			SetLoadError(error, "Level part GUID does not match the scene: ", map_name);
            return 			FALSE;
        }
        // read data
        IReader* chunk = R->open_chunk(CHUNK_TOOLS_DATA + M->FClassID);
		if (chunk)
        {
			const bool loaded = M->LoadStream(*chunk);
            chunk->close();
			if (!loaded)
			{
				FS.r_close(R);
				SetLoadError(error, "Editor tool rejected level part: ", map_name);
				return FALSE;
			}
        }
        else
        {
			ELog.Msg(mtError, "Skipping corrupted version of level part: '%s\\%s.part'",
				EFS.ExtractFileName(map_name).c_str(), M->ClassName());
            FS.r_close(R);
			SetLoadError(error, "Level part has no tool data chunk: ", map_name);
            return 			FALSE;
        }
        //success
        FS.r_close(R);
        return 				TRUE;
    }
    return 					TRUE;
}

BOOL EScene::LoadLevelPart(LPCSTR map_name, ObjClassID cls, xr_string* error)
{
	xr_string pn	= LevelPartName(map_name,cls);
	if (LoadLevelPart(GetTool(cls), pn.c_str(), error))
    	return 		TRUE;
    else
	    return 			FALSE;
}

BOOL EScene::UnloadLevelPart(ESceneToolBase* M)
{
	M->Clear		();
    return 			TRUE;
}

BOOL EScene::UnloadLevelPart(LPCSTR map_name, ObjClassID cls)
{
	xr_string pn	= LevelPartName(map_name,cls);
    if (UnloadLevelPart(GetTool(cls)))
    	return 		TRUE;
    else
    	return			FALSE;
}

xr_string EScene::LevelPartPath(LPCSTR full_name)
{
    return 			EFS.ExtractFilePath(full_name)+EFS.ExtractFileName(full_name)+"\\";
}

xr_string EScene::LevelPartName(LPCSTR map_name, ObjClassID cls)
{
    return 			LevelPartPath(map_name)+GetTool(cls)->ClassName() + ".part";
}

bool EScene::SaveLTX(LPCSTR map_name, bool bForUndo, bool bForceSaveAll, xr_string* error)
{
	VERIFY			(map_name);
    R_ASSERT		(!bForUndo);
	if (error)
		error->clear();

    CTimer 			T;
    T.Start			();
    xr_string 		full_name;
	full_name		= map_name;
	xr_string part_prefix = LevelPartPath(full_name.c_str());
	SaveTransaction transaction;
	if (!transaction.Initialize(full_name.c_str(), error))
		return false;

    CInifile ini(full_name.c_str(), FALSE, FALSE, FALSE);
	ini.w_u32("version", "value", CURRENT_FILE_VERSION);
	m_LevelOp.SaveLTX(ini);
	m_GUID.SaveLTX(ini, "guid", "guid");
	ini.w_string("level_tag", "owner", m_OwnerName.c_str());
	ini.w_u32("level_tag", "create_time", m_CreateTime);
	ini.w_fvector3("camera", "hpb", EDevice->m_Camera.GetHPB());
	ini.w_fvector3("camera", "pos", EDevice->m_Camera.GetPosition());
	for (ObjectIt object = m_ESO_SnapObjects.begin(); object != m_ESO_SnapObjects.end(); ++object)
		ini.w_string("snap_objects", (*object)->GetName(), nullptr);

    m_SaveCache.clear		();
	for (SceneToolsMapPairIt tool_it = m_SceneTools.begin(); tool_it != m_SceneTools.end(); ++tool_it)
    {
		ESceneToolBase* tool = tool_it->second;
		if (tool_it->first != OBJCLASS_DUMMY && tool && tool->IsEnabled() && tool->IsEditable())
        {
			xr_string part_name = part_prefix + tool->ClassName() + ".part";
			if (tool->can_use_inifile())
            {
				if (!StageToolLTX(transaction, tool, m_GUID, part_name.c_str(), true, error))
				{
					m_SaveCache.clear();
					return false;
				}
			}
			else
			{
				tool->SaveStream(m_SaveCache);
				CMemoryWriter serialized;
				serialized.open_chunk(CHUNK_TOOLS_GUID);
				serialized.w(&m_GUID, sizeof(m_GUID));
				serialized.close_chunk();
				serialized.open_chunk(CHUNK_TOOLS_DATA + tool_it->first);
				if (m_SaveCache.size())
					serialized.w(m_SaveCache.pointer(), m_SaveCache.size());
				serialized.close_chunk();
				if (!transaction.StageFile(part_name.c_str(), serialized.pointer(), serialized.size(), true, error))
				{
					m_SaveCache.clear();
					return false;
				}
            }
			m_SaveCache.clear	();
        }
    }

	CMemoryWriter serialized_main;
	ini.save_as(serialized_main);
	if (!transaction.StageFile(full_name.c_str(), serialized_main.pointer(), serialized_main.size(), true, error))
		return false;
	if (!transaction.Commit(error))
		return false;

	Msg("Saving time: %3.2f sec", T.GetElapsed_sec());
	return true;
}

bool EScene::SaveToolLTX(ObjClassID clsid, LPCSTR fn, xr_string* error)
{
	if (error)
		error->clear();
    ESceneToolBase* tool 	= GetTool(clsid);
	if (!tool)
	{
		SetSaveError(error, "Cannot save unavailable level part: ", fn);
		return false;
	}

	SaveTransaction transaction;
	if (!transaction.Initialize(fn, error))
		return false;
	if (!StageToolLTX(transaction, tool, m_GUID, fn, false, error))
		return false;
	return transaction.Commit(error);
}

bool EScene::LoadToolLTX(ObjClassID clsid, LPCSTR fn)
{
    ESceneToolBase* tool 	= GetTool(clsid);
    tool->Clear				(true);
	bool res 				= LoadLevelPartLTX(tool, fn);
	return 					res;
}

bool EScene::Save(LPCSTR map_name, bool bUndo, bool bForceSaveAll, xr_string* error)
{
	R_ASSERT		(bUndo);
	VERIFY			(map_name);
	if (error)
		error->clear();

	SaveTransaction transaction;
	if (!transaction.Initialize(map_name, error))
		return false;
	CMemoryWriter serialized;
	SaveUndoStream(serialized, bForceSaveAll);
	if (!transaction.StageFile(map_name, serialized.pointer(), serialized.size(), false, error))
		return false;
	return transaction.Commit(error);
}

void EScene::SaveUndoStream(IWriter& writer, bool bForceSaveAll)
{
	writer.open_chunk	(CHUNK_VERSION);
	writer.w_u32		(CURRENT_FILE_VERSION);
	writer.close_chunk	();

	writer.open_chunk	(CHUNK_LEVELOP);
	m_LevelOp.Save		(writer);
	writer.close_chunk	();

	writer.open_chunk	(CHUNK_TOOLS_GUID);
	writer.w			(&m_GUID, sizeof(m_GUID));
	writer.close_chunk	();

	writer.open_chunk	(CHUNK_LEVEL_TAG);
	writer.w_stringZ	(m_OwnerName);
	writer.w			(&m_CreateTime, sizeof(m_CreateTime));
	writer.close_chunk	();

	writer.open_chunk	(CHUNK_CAMERA);
	writer.w_fvector3	(EDevice->m_Camera.GetHPB());
	writer.w_fvector3	(EDevice->m_Camera.GetPosition());
	writer.close_chunk	();

	writer.open_chunk	(CHUNK_SNAPOBJECTS);
	writer.w_u32		(m_ESO_SnapObjects.size());
	for (ObjectIt object = m_ESO_SnapObjects.begin(); object != m_ESO_SnapObjects.end(); ++object)
		writer.w_stringZ	((*object)->GetName());
	writer.close_chunk	();

    m_SaveCache.clear		();

    SceneToolsMapPairIt _I = m_SceneTools.begin();
    SceneToolsMapPairIt _E = m_SceneTools.end();

    for (; _I!=_E; ++_I)
    {
        if (	(_I->first!=OBJCLASS_DUMMY) && 
        		_I->second 					&& 
                _I->second->IsEnabled()		&&
                _I->second->IsEditable() 	&&
                (_I->second->IsChanged()||bForceSaveAll)	)
        {

            if (_I->second->IsEnabled()&&_I->second->IsEditable())
            {
            	if (_I->second->IsNeedSave())
                {
                    _I->second->SaveStream	(m_SaveCache);
                    writer.open_chunk		(CHUNK_TOOLS_DATA+_I->first);
                    writer.w				(m_SaveCache.pointer(),m_SaveCache.size());
                    writer.close_chunk		();
                }
            }
			m_SaveCache.clear	();
		}
	}
}


void EScene::SaveObjectLTX(CCustomObject* O, LPCSTR sect_name, CInifile& ini)
{
	ini.w_u32	(sect_name,"clsid",O->FClassID);
	O->SaveLTX	(ini, sect_name);
}

void EScene::SaveObjectStream( CCustomObject* O, IWriter& F )
{
    F.open_chunk	(CHUNK_OBJECT_CLASS);
    F.w_u32			(O->FClassID);
    F.close_chunk	();
    F.open_chunk	(CHUNK_OBJECT_BODY);
    O->SaveStream	(F);
    F.close_chunk	();
}


void EScene::SaveObjectsLTX(ObjectList& lst, LPCSTR sect_name_parent, LPCSTR sect_name_prefix, CInifile& ini)
{
    u32 i 				= 0;
    string256			buff;
    for(ObjectIt _F = lst.begin(); _F!=lst.end(); ++_F, ++i)
    {
    	sprintf				(buff,"%s_%s_%d",sect_name_parent,sect_name_prefix,i);
        SaveObjectLTX		(*_F,buff,ini);
    }
	sprintf					(buff,"%s_count",sect_name_prefix);
    ini.w_u32				(sect_name_parent, buff, lst.size());
}

void EScene::SaveObjectsStream( ObjectList& lst, u32 chunk_id, IWriter& F )
{
    F.open_chunk			(chunk_id);
    int count 				= 0;
    for(ObjectIt _F = lst.begin();_F!=lst.end();++_F)
    {
        F.open_chunk		(count);
        ++count;
        SaveObjectStream	(*_F,F);
        F.close_chunk		();
    }
	F.close_chunk			();
}

bool EScene::ReadObjectStream(IReader& F, CCustomObject*& O)
{
	O = nullptr;
	if (!F.find_chunk(CHUNK_OBJECT_CLASS))
		return false;
	const ObjClassID clsid = ObjClassID(F.r_u32());
	if (u32(clsid) >= OBJCLASS_COUNT || clsid == OBJCLASS_DUMMY)
		return false;
	ESceneCustomOTool* tool = GetOTool(clsid);
	if (!tool)
		return false;
	O = tool->CreateObject(0, 0);
	if (!O)
		return false;

    IReader* S 				= F.open_chunk(CHUNK_OBJECT_BODY);
	if (!S)
	{
		xr_delete(O);
		return false;
	}
    bool bRes 				= O->LoadStream(*S);
    S->close				();

	if (!bRes)
    	xr_delete			(O);

	return bRes;
}

bool EScene::ReadObjectLTX(CInifile& ini, LPCSTR sect_name, CCustomObject*& O)
{
	O = nullptr;
	if (!ini.section_exist(sect_name) || !ini.line_exist(sect_name, "clsid"))
		return false;
	const ObjClassID clsid = ObjClassID(ini.r_u32(sect_name, "clsid"));
	if (u32(clsid) >= OBJCLASS_COUNT || clsid == OBJCLASS_DUMMY)
		return false;
	ESceneCustomOTool* tool = GetOTool(clsid);
	if (!tool)
		return false;
	O = tool->CreateObject(0, 0);
	if (!O)
		return false;

    bool bRes 				= O->LoadLTX(ini, sect_name);

	if (!bRes)
    	xr_delete			(O);

	return bRes;
}

bool EScene::ReadObjectsLTX(CInifile& ini,  LPCSTR sect_name_parent, LPCSTR sect_name_prefix, TAppendObject on_append, SPBItem* pb)
{
	string128			buff;
	if (!on_append)
		return false;
	sprintf				(buff, "%s_count", sect_name_prefix);
	if (!ini.section_exist(sect_name_parent) || !ini.line_exist(sect_name_parent, buff))
		return false;
    u32 count			= ini.r_u32(sect_name_parent, buff);
	bool bRes 			= true;

	for(u32 i=0; i<count; ++i)
    {
    	sprintf				(buff, "%s_%s_%d", sect_name_parent, sect_name_prefix, i);
        CCustomObject* obj	= NULL;

        if (ReadObjectLTX(ini, buff, obj))
        {
            LPCSTR obj_name = obj->GetName();
            CCustomObject* existing = FindObjectByName(obj_name, obj->FClassID);
            if (existing)
            {

                /*if(g_frmConflictLoadObject->m_result!=2 && g_frmConflictLoadObject->m_result!=4 && g_frmConflictLoadObject->m_result!=6)
                {
                    g_frmConflictLoadObject->m_existing_object 	= existing;
                    g_frmConflictLoadObject->m_new_object 		= obj;
                    g_frmConflictLoadObject->Prepare			();
                    g_frmConflictLoadObject->ShowModal			();
                }*/
                /*     switch(g_frmConflictLoadObject->m_result)
                     {
                         case 1: //Overwrite
                         case 2: //Overwrite All
                         {
                            bool res = RemoveObject		(existing, true, true);
                             if(!res)
                                 Msg("! RemoveObject [%s] failed", existing->GetName());
                              else
                                 xr_delete(existing);
                         }break;
                         case 3: //Insert new
                         case 4: //Insert new All
                         {
                             string256 				buf;
                             GenObjectName			(obj->FClassID, buf, obj->GetName());
                             obj->SetName(buf);
                         }break;
                         case 0: //Cancel
                         case 5: //Skip
                         case 6: //Skip All
                         {
                             xr_delete(obj);
                         }break;
                     } //switch
                 } //if exist*/
                string256 				buf;
                GenObjectName(obj->FClassID, buf, obj->GetName());
                obj->SetName(buf);
            }
			if (obj && !on_append(obj))
			{
				xr_delete(obj);
				bRes = false;
			}

        }
        
        else
        	bRes = false;

        if (pb)
			pb->Inc();
		UI->ProgressCheckpoint();
    }
    return bRes;
}

bool EScene::ReadObjectsStream(IReader& F, u32 chunk_id, TAppendObject on_append, SPBItem* pb,
	u32* decoded_count)
{
	if (decoded_count)
		*decoded_count = 0;
	if (!on_append)
		return false;
	bool bRes 			= true;
    IReader* OBJ 		= F.open_chunk(chunk_id);
    if (OBJ)
    {
        IReader* O   	= OBJ->open_chunk(0);
        for (int count=1; O; ++count)
        {
			if (decoded_count)
				++*decoded_count;
            CCustomObject* obj	=NULL;
            if (ReadObjectStream(*O, obj))
            {
                LPCSTR obj_name = obj->GetName();
                CCustomObject* existing = FindObjectByName(obj_name,obj->FClassID);
                if(existing)
                {
                	/*if(g_frmConflictLoadObject->m_result!=2 && g_frmConflictLoadObject->m_result!=4 && g_frmConflictLoadObject->m_result!=6)
                    {
                        g_frmConflictLoadObject->m_existing_object 	= existing;
                        g_frmConflictLoadObject->m_new_object 		= obj;
                        g_frmConflictLoadObject->Prepare			();
                        g_frmConflictLoadObject->ShowModal			();
                    }
                    switch(g_frmConflictLoadObject->m_result)
                    {
                    	case 1: //Overwrite
                    	case 2: //Overwrite All
                        {
                           bool res = RemoveObject		(existing, true, true);
							if(!res)
                            	Msg("! RemoveObject [%s] failed", existing->Name);
                             else
                             	xr_delete(existing);
                        }break;
                    	case 3: //Insert new
                    	case 4: //Insert new All*/
                        
                            string256 				buf;
    						GenObjectName			(obj->FClassID, buf, obj->GetName());
    						obj->SetName(buf);
                        /*}break;
                    	case 0: //Cancel
                    	case 5: //Skip
                    	case 6: //Skip All
                        {
                        	xr_delete(obj);
                        }break;
                    }*/
                }
				if (obj && !on_append(obj))
				{
					xr_delete(obj);
					bRes = false;
				}
			}
            else
            	bRes = false;

            O->close	();
            if (pb)
				pb->Inc();
			UI->ProgressCheckpoint();
			O = OBJ->open_chunk(count);
        }
        OBJ->close();
    }
    return bRes;
}

bool EScene::OnLoadAppendObject(CCustomObject* O)
{
	AppendObject	(O,false);
    return true;
}

namespace
{
constexpr u32 LOAD_ROLLBACK_VERSION = 1;
constexpr u32 CHUNK_LOAD_ROLLBACK_VERSION = 0x6f000001;
constexpr u32 CHUNK_LOAD_ROLLBACK_LEVEL_OPTIONS = 0x6f000002;
constexpr u32 CHUNK_LOAD_ROLLBACK_METADATA = 0x6f000003;
constexpr u32 CHUNK_LOAD_ROLLBACK_SNAP_OBJECTS = 0x6f000004;
constexpr u32 CHUNK_LOAD_ROLLBACK_TOOL_BASE = 0x6f001000;
constexpr u32 CHUNK_LOAD_ROLLBACK_TOOL_FLAGS = 1;
constexpr u32 CHUNK_LOAD_ROLLBACK_TOOL_DATA = 2;
}

bool EScene::CaptureLoadRollback(CMemoryWriter& snapshot, xr_string& error)
{
	snapshot.clear();
	u32 snapshot_version = LOAD_ROLLBACK_VERSION;
	snapshot.w_chunk(CHUNK_LOAD_ROLLBACK_VERSION, &snapshot_version, sizeof(snapshot_version));

	snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_LEVEL_OPTIONS);
	m_LevelOp.Save(snapshot);
	snapshot.close_chunk();

	snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_METADATA);
	snapshot.w(&m_GUID, sizeof(m_GUID));
	snapshot.w_stringZ(m_OwnerName);
	snapshot.w(&m_CreateTime, sizeof(m_CreateTime));
	snapshot.w_s32(m_LastAvailObject);
	snapshot.close_chunk();

	for (CCustomObject* object : m_ESO_SnapObjects)
	{
		if (!object)
		{
			error = "Cannot capture a null snap-object reference";
			return false;
		}
	}
	snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_SNAP_OBJECTS);
	snapshot.w_u32(m_ESO_SnapObjects.size());
	for (CCustomObject* object : m_ESO_SnapObjects)
		snapshot.w_stringZ(object->GetName());
	snapshot.close_chunk();

	for (SceneToolsMapPairIt tool_it = m_SceneTools.begin(); tool_it != m_SceneTools.end(); ++tool_it)
	{
		ESceneToolBase* tool = tool_it->second;
		if (!tool || tool_it->first == OBJCLASS_DUMMY)
			continue;

		snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_TOOL_BASE + tool_it->first);
		u32 flags = tool->m_EditFlags.get();
		snapshot.w_chunk(CHUNK_LOAD_ROLLBACK_TOOL_FLAGS, &flags, sizeof(flags));
		snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_TOOL_DATA);
		tool->SaveStream(snapshot);
		snapshot.close_chunk();
		snapshot.close_chunk();
	}

	IReader verification(snapshot.pointer(), snapshot.size());
	u32 verify_version = 0;
	if (!verification.r_chunk(CHUNK_LOAD_ROLLBACK_VERSION, &verify_version) ||
		verify_version != LOAD_ROLLBACK_VERSION ||
		!verification.find_chunk(CHUNK_LOAD_ROLLBACK_LEVEL_OPTIONS) ||
		!verification.find_chunk(CHUNK_LOAD_ROLLBACK_METADATA) ||
		!verification.find_chunk(CHUNK_LOAD_ROLLBACK_SNAP_OBJECTS))
	{
		error = "Cannot verify the scene rollback snapshot metadata";
		return false;
	}

	for (SceneToolsMapPairIt tool_it = m_SceneTools.begin(); tool_it != m_SceneTools.end(); ++tool_it)
	{
		if (!tool_it->second || tool_it->first == OBJCLASS_DUMMY)
			continue;
		IReader* tool_chunk = verification.open_chunk(CHUNK_LOAD_ROLLBACK_TOOL_BASE + tool_it->first);
		if (!tool_chunk)
		{
			error.sprintf("Rollback snapshot is missing editor tool '%s'", tool_it->second->ClassName());
			return false;
		}
		u32 flags = 0;
		const bool valid = !!tool_chunk->r_chunk(CHUNK_LOAD_ROLLBACK_TOOL_FLAGS, &flags) &&
			!!tool_chunk->find_chunk(CHUNK_LOAD_ROLLBACK_TOOL_DATA);
		tool_chunk->close();
		if (!valid)
		{
			error.sprintf("Rollback snapshot is corrupt for editor tool '%s'", tool_it->second->ClassName());
			return false;
		}
	}
	return true;
}

void EScene::ClearLoadCandidate()
{
	m_LastAvailObject = 0;
	ClearSnapList(false);
	for (SceneToolsMapPairIt tool_it = m_SceneTools.begin(); tool_it != m_SceneTools.end(); ++tool_it)
	{
		if (tool_it->second && tool_it->first != OBJCLASS_DUMMY)
			tool_it->second->Reset();
	}
	m_LevelOp.Reset();
	m_SaveCache.clear();
}

void EScene::InvalidateBuildCaches()
{
	m_cfrom_builder.clear();
	m_level_graph.clear();
	m_game_graph.clear();
	m_RTFlags.set(flIsBuildedAIMap | flIsBuildedGameGraph | flIsBuildedCForm, FALSE);
	Tools->ClearDebugDraw();
	g_scene_physics.UpdateLevelCollision();
}

bool EScene::RestoreLoadRollback(IReader& snapshot, xr_string& error)
{
	u32 version = 0;
	if (!snapshot.r_chunk(CHUNK_LOAD_ROLLBACK_VERSION, &version) || version != LOAD_ROLLBACK_VERSION)
	{
		error = "Rollback snapshot version is missing or unsupported";
		return false;
	}

	ClearLoadCandidate();
	IReader* level_options = snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_LEVEL_OPTIONS);
	if (!level_options || !m_LevelOp.Read(*level_options))
	{
		if (level_options)
			level_options->close();
		error = "Cannot restore level options from the rollback snapshot";
		return false;
	}
	level_options->close();

	IReader* metadata = snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_METADATA);
	if (!metadata)
	{
		error = "Cannot restore scene metadata from the rollback snapshot";
		return false;
	}
	metadata->r(&m_GUID, sizeof(m_GUID));
	metadata->r_stringZ(m_OwnerName);
	metadata->r(&m_CreateTime, sizeof(m_CreateTime));
	m_LastAvailObject = metadata->r_s32();
	metadata->close();

	for (SceneToolsMapPairIt tool_it = m_SceneTools.begin(); tool_it != m_SceneTools.end(); ++tool_it)
	{
		ESceneToolBase* tool = tool_it->second;
		if (!tool || tool_it->first == OBJCLASS_DUMMY)
			continue;
		IReader* tool_chunk = snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_TOOL_BASE + tool_it->first);
		if (!tool_chunk)
		{
			error.sprintf("Cannot restore editor tool '%s': snapshot chunk is missing", tool->ClassName());
			return false;
		}
		u32 flags = 0;
		IReader* tool_data = tool_chunk->open_chunk(CHUNK_LOAD_ROLLBACK_TOOL_DATA);
		const bool loaded = !!tool_chunk->r_chunk(CHUNK_LOAD_ROLLBACK_TOOL_FLAGS, &flags) && tool_data &&
			tool->LoadStream(*tool_data);
		if (tool_data)
			tool_data->close();
		tool_chunk->close();
		if (!loaded)
		{
			error.sprintf("Editor tool '%s' rejected its rollback snapshot", tool->ClassName());
			return false;
		}
		tool->m_EditFlags.assign(flags);
	}

	IReader* snap_objects = snapshot.open_chunk(CHUNK_LOAD_ROLLBACK_SNAP_OBJECTS);
	if (!snap_objects)
	{
		error = "Cannot restore the snap-object list from the rollback snapshot";
		return false;
	}
	const u32 snap_count = snap_objects->r_u32();
	for (u32 index = 0; index < snap_count; ++index)
	{
		shared_str name;
		snap_objects->r_stringZ(name);
		CCustomObject* object = FindObjectByName(name.c_str(), OBJCLASS_SCENEOBJECT);
		if (!object)
		{
			snap_objects->close();
			error.sprintf("Cannot restore snap object '%s'", name.c_str());
			return false;
		}
		m_ESO_SnapObjects.push_back(object);
	}
	snap_objects->close();
	UpdateSnapList();
	SynchronizeObjects();
	g_scene_physics.UpdateLevelCollision();
	UI->UpdateScene(true);
	return true;
}


bool EScene::LoadLTX(LPCSTR map_name, bool bUndo, xr_string* error)
{
    DWORD version = 0;
	if (error)
		error->clear();
	if (!map_name || !map_name[0])
	{
		SetLoadError(error, "Scene name is empty");
		return false;
	}

    xr_string 		full_name;
    full_name 		= map_name;

	ELog.Msg( mtInformation, "EScene: loading '%s'", map_name);
    if (FS.exist(full_name.c_str()))
    {
        CTimer T; T.Start();

		// lock main level
		CInifile	ini(full_name.c_str());
		if (!ini.section_exist("version") || !ini.line_exist("version", "value"))
		{
			SetLoadError(error, "LTX scene has no version value: ", map_name);
			return false;
		}
        version 	= ini.r_u32("version","value");

        if (version!=CURRENT_FILE_VERSION)
        {
			SetLoadError(error, "Unsupported LTX scene version: ", map_name);
            UI->UpdateScene();
            return false;
        }

		if (!m_LevelOp.ReadLTX(ini))
		{
			SetLoadError(error, "Invalid LTX level options: ", map_name);
			return false;
		}
		if (!ini.section_exist("camera") || !ini.line_exist("camera", "pos") ||
			!ini.line_exist("camera", "hpb") || !ini.section_exist("guid") ||
			!ini.line_exist("guid", "guid_g0") || !ini.line_exist("guid", "guid_g1") ||
			!ini.section_exist("level_tag") || !ini.line_exist("level_tag", "owner") ||
			!ini.line_exist("level_tag", "create_time"))
		{
			SetLoadError(error, "LTX scene metadata is incomplete: ", map_name);
			return false;
		}

       	Fvector hpb, pos;
        pos					= ini.r_fvector3("camera","pos");
        hpb					= ini.r_fvector3("camera","hpb");
        EDevice->m_Camera.Set(hpb,pos);
		ViewportNavigation::ResetState();
        EDevice->m_Camera.SetStyle(EDevice->m_Camera.GetStyle());

        m_GUID.LoadLTX			(ini,"guid","guid");

		m_OwnerName				= ini.r_string("level_tag","owner");
        m_CreateTime			= ini.r_u32("level_tag","create_time");


        SceneToolsMapPairIt _I 	= m_SceneTools.begin();
        SceneToolsMapPairIt _E 	= m_SceneTools.end();
        for (; _I!=_E; ++_I)
        {
            if (_I->second)
            {
                {
                    if (!bUndo && _I->second->IsEnabled() && (_I->first!=OBJCLASS_DUMMY))
                    {
                        xr_string fn 		 = LevelPartName(map_name, _I->first).c_str();
						if (!LoadLevelPartLTX(_I->second, fn.c_str(), error))
							return false;
                    }
                }
            }
		}

        if(ini.section_exist("snap_objects"))
        {
			CInifile::Sect& S 		= ini.r_section("snap_objects");
            CInifile::SectCIt Si 	= S.Data.begin();
            CInifile::SectCIt Se 	= S.Data.end();
            for(;Si!=Se; ++Si)
            {
                CCustomObject* 	O = FindObjectByName(Si->first.c_str(),OBJCLASS_SCENEOBJECT);
                if (!O)
                    ELog.Msg(mtError,"EScene: Can't find snap object '%s'.",Si->second.c_str());
                else
                	m_ESO_SnapObjects.push_back(O);
            }
            UpdateSnapList();
        }

        Msg("EScene: %d objects loaded, %3.2f sec", ObjCount(), T.GetElapsed_sec() );

    	UI->UpdateScene(true);

        SynchronizeObjects();

	    if (!bUndo)
        	m_RTFlags.set(flRT_Unsaved|flRT_Modified,FALSE);
        
		return true;
    }else
    {
    	ELog.Msg(mtError,"Can't find file: '%s'",map_name);
		SetLoadError(error, "Scene file not found: ", map_name);
    }
	return false;
}

bool EScene::IsSceneFile(LPCSTR full_name, xr_string& why)
{
    why.clear();
    if (!full_name || !full_name[0])		{ why = "empty scene name"; return false; }
    if (!FS.exist(full_name))				{ why = "file not found"; return false; }

    IReader* F = FS.r_open(full_name);
    if (!F)									{ why = "cannot open the file"; return false; }

    bool ok = false;
    if (F->elapsed() > 0)
    {
        // the loader itself sniffs the first byte to pick the ltx path
        const char first = *(const char*)F->pointer();
        if ('[' == first)
        {
			// CInifile treats missing required keys as fatal, so reject unrelated INI files first.
            const u32 len = F->elapsed();
            xr_string text;
            text.assign((const char*)F->pointer(), len);
            xr_string low = text;
            xr_strlwr(low);
            u32 version = 0;
            size_t sec = low.find("[version]");
            if (sec == xr_string::npos)			why = "not a level file (no [version] section)";
            else
            {
                size_t val = low.find("value", sec);
                size_t next_sec = low.find('[', sec + 1);
                if (val == xr_string::npos || (next_sec != xr_string::npos && val > next_sec))
                    why = "not a level file (no version value)";
                else
                {
                    size_t eq = low.find('=', val);
                    if (eq != xr_string::npos) version = (u32)atoi(low.c_str() + eq + 1);
                    if (version != CURRENT_FILE_VERSION)	why = "unsupported level version";
                    else									ok = true;
                }
            }
        }
		else
		{
			u32 version = 0;
			IReader* version_chunk = F->open_chunk(CHUNK_VERSION);
			if (!version_chunk)
				why = "not a level file (no version chunk)";
			else
			{
				if (version_chunk->elapsed() != sizeof(version))
					why = "not a level file (invalid version chunk)";
				else
				{
					version = version_chunk->r_u32();
					if (version != CURRENT_FILE_VERSION)
						why = "unsupported level version";
					else
						ok = true;
				}
				version_chunk->close();
			}
		}
    }
    else
        why = "the file is empty";

    FS.r_close(F);
    if (!ok && why.empty()) why = "not a level file";
    return ok;
}

bool EScene::Load(LPCSTR map_name, bool bUndo, xr_string* error)
{
    u32 version = 0;

	if (error)
		error->clear();
	if (!map_name || !map_name[0])
	{
		SetLoadError(error, "Scene name is empty");
		return false;
	}

    xr_string 		full_name;
    full_name 		= map_name;

	ELog.Msg( mtInformation, "EScene: loading '%s'", map_name);
    if (FS.exist(full_name.c_str()))
    {
        CTimer T; T.Start();
            
        // read main level
		IReader* F = FS.r_open(full_name.c_str());
		if (!F)
		{
			SetLoadError(error, "Cannot open scene file: ", map_name);
			return false;
		}
        // Version. A file the user picked is untrusted input, so a missing
        // version chunk is a refusal, not an assert that kills the editor.
		IReader* version_chunk = F->open_chunk(CHUNK_VERSION);
		if (!version_chunk || version_chunk->elapsed() != sizeof(version))
		{
			if (version_chunk)
				version_chunk->close();
			SetLoadError(error, "Binary scene has an invalid version chunk: ", map_name);
			UI->UpdateScene();
			FS.r_close(F);
			return false;
		}
		version = version_chunk->r_u32();
		version_chunk->close();
        if (version!=CURRENT_FILE_VERSION)
        {
			SetLoadError(error, "Unsupported binary scene version: ", map_name);
            UI->UpdateScene();
            FS.r_close(F);
            return false;
        }

        // Lev. ops.
        IReader* LOP = F->open_chunk(CHUNK_LEVELOP);
        if (LOP)
        {
	        const bool loaded = m_LevelOp.Read(*LOP);
        	LOP->close		();
			if (!loaded)
			{
				FS.r_close(F);
				SetLoadError(error, "Invalid binary level options: ", map_name);
				return false;
			}
        }else
        {
			FS.r_close(F);
			SetLoadError(error, "Binary scene has no level-options chunk: ", map_name);
			return false;
	    }

        //
		if (IReader* camera = F->open_chunk(CHUNK_CAMERA))
		{
			if (camera->elapsed() != sizeof(Fvector) * 2)
			{
				camera->close();
				FS.r_close(F);
				SetLoadError(error, "Binary scene has an invalid camera chunk: ", map_name);
				return false;
			}
			Fvector hpb, pos;
			camera->r_fvector3(hpb);
			camera->r_fvector3(pos);
			camera->close();
			EDevice->m_Camera.Set(hpb,pos);
			ViewportNavigation::ResetState();
			EDevice->m_Camera.SetStyle(EDevice->m_Camera.GetStyle());
		}

		IReader* guid_chunk = F->open_chunk(CHUNK_TOOLS_GUID);
		if (!guid_chunk || guid_chunk->elapsed() != sizeof(m_GUID))
		{
			if (guid_chunk)
				guid_chunk->close();
			FS.r_close(F);
			SetLoadError(error, "Binary scene has an invalid tool GUID chunk: ", map_name);
			return false;
		}
		guid_chunk->r(&m_GUID, sizeof(m_GUID));
		guid_chunk->close();

		if (IReader* level_tag = F->open_chunk(CHUNK_LEVEL_TAG))
		{
			if (!HasTerminatedString(*level_tag, sizeof(m_CreateTime)))
			{
				level_tag->close();
				FS.r_close(F);
				SetLoadError(error, "Binary scene has an invalid level-tag chunk: ", map_name);
				return false;
			}
			level_tag->r_stringZ(m_OwnerName);
			level_tag->r(&m_CreateTime, sizeof(m_CreateTime));
			level_tag->close();
		}else
        {
            m_OwnerName		= "";
            m_CreateTime	= 0;
        }

		DWORD obj_cnt = 0;
		bool has_object_count = false;
		if (IReader* object_count = F->open_chunk(CHUNK_OBJECT_COUNT))
		{
			has_object_count = true;
			if (object_count->elapsed() != sizeof(obj_cnt))
			{
				object_count->close();
				FS.r_close(F);
				SetLoadError(error, "Binary scene has an invalid legacy object-count chunk: ", map_name);
				return false;
			}
			obj_cnt = object_count->r_u32();
			object_count->close();
		}
		bool has_object_list = false;
		if (IReader* object_list = F->open_chunk(CHUNK_OBJECT_LIST))
		{
			has_object_list = true;
			object_list->close();
		}
		if (has_object_count != has_object_list)
		{
			FS.r_close(F);
			SetLoadError(error, "Binary scene has incomplete legacy object metadata: ", map_name);
			return false;
		}

        SPBItem* pb 		= UI->ProgressStart(obj_cnt,"Loading objects...");
		u32 decoded_objects = 0;
		const bool objects_loaded = ReadObjectsStream(*F, CHUNK_OBJECT_LIST,
			TAppendObject(this, &EScene::OnLoadAppendObject), pb, &decoded_objects);
        UI->ProgressEnd		(pb);
		if (!objects_loaded)
		{
			FS.r_close(F);
			SetLoadError(error, "Binary scene contains an invalid object: ", map_name);
			return false;
		}
		if (has_object_count && decoded_objects != obj_cnt)
		{
			FS.r_close(F);
			if (error)
				error->sprintf("Binary scene '%s' declares %u legacy objects but contains %u",
					map_name, obj_cnt, decoded_objects);
			return false;
		}

        SceneToolsMapPairIt _I = m_SceneTools.begin();
        SceneToolsMapPairIt _E = m_SceneTools.end();
        for (; _I!=_E; ++_I)
        {
            if (_I->second)
            {
			    IReader* chunk 		= F->open_chunk(CHUNK_TOOLS_DATA+_I->first);
            	if (chunk){
	                const bool loaded = _I->second->LoadStream(*chunk);
    	            chunk->close	();
					if (!loaded)
					{
						FS.r_close(F);
						if (error)
							error->sprintf("Editor tool '%s' rejected embedded scene data in '%s'",
								_I->second->ClassName(), map_name);
						return false;
					}
                }else{
                    if (!bUndo && _I->second->IsEnabled() && (_I->first!=OBJCLASS_DUMMY))
                    {
						xr_string part_name = LevelPartName(map_name, _I->first);
						if (!LoadLevelPart(_I->second, part_name.c_str(), error))
						{
							FS.r_close(F);
							return false;
						}
                    }
                }
            }
		}
        
        // snap list
		if (IReader* snap_objects = F->open_chunk(CHUNK_SNAPOBJECTS))
		{
			if (snap_objects->elapsed() < sizeof(u32))
			{
				snap_objects->close();
				FS.r_close(F);
				SetLoadError(error, "Binary scene has an invalid snap-object chunk: ", map_name);
				return false;
			}
			shared_str buf;
			const u32 count = snap_objects->r_u32();
			for (u32 index = 0; index < count; ++index)
			{
				if (!snap_objects->elapsed() ||
					!memchr(snap_objects->pointer(), 0, snap_objects->elapsed()))
				{
					snap_objects->close();
					FS.r_close(F);
					SetLoadError(error, "Binary scene has a truncated snap-object list: ", map_name);
					return false;
				}
				snap_objects->r_stringZ(buf);
				CCustomObject* object = FindObjectByName(buf.c_str(), OBJCLASS_SCENEOBJECT);
				if (!object)
					ELog.Msg(mtError, "EScene: Can't find snap object '%s'.", buf.c_str());
				else
					m_ESO_SnapObjects.push_back(object);
			}
			if (snap_objects->elapsed())
			{
				snap_objects->close();
				FS.r_close(F);
				SetLoadError(error, "Binary scene has trailing snap-object data: ", map_name);
				return false;
			}
			snap_objects->close();
			UpdateSnapList();
		}

        Msg("EScene: %d objects loaded, %3.2f sec", ObjCount(), T.GetElapsed_sec() );

    	UI->UpdateScene(true); 

		FS.r_close(F);

        SynchronizeObjects();

	    if (!bUndo)
        	m_RTFlags.set(flRT_Unsaved|flRT_Modified,FALSE);
        
		return true;
    }else
    {
    	ELog.Msg(mtError,"Can't find file: '%s'",map_name);
		SetLoadError(error, "Scene file not found: ", map_name);
    }
	return false;
}

bool EScene::LoadTransactional(LPCSTR map_name, xr_string& error)
{
	error.clear();
	m_LastLoadRollbackSucceeded = true;
	xr_string preflight_error;
	if (!IsSceneFile(map_name, preflight_error))
	{
		error = preflight_error;
		return false;
	}

	CMemoryWriter rollback;
	if (!CaptureLoadRollback(rollback, error))
		return false;

	const u32 rt_flags = m_RTFlags.get();
	const Fvector camera_hpb = EDevice->m_Camera.GetHPB();
	const Fvector camera_position = EDevice->m_Camera.GetPosition();
	const Fvector camera_target = EDevice->m_Camera.GetTarget();
	const ViewportNavigation::Target navigation_target = ViewportNavigation::CurrentTarget();
	const u64 undo_generation = m_UndoObjectGeneration;
	const u64 pending_generation = m_PendingTransformGeneration;
	const xr_vector<UndoTransformItem> pending_transform = m_PendingTransformUndo;
	const bool transform_active = m_TransformUndoActive;
	const bool transform_fallback = m_TransformUndoFallback;
	const bool undo_rollback_failed = m_UndoRollbackFailed;

	IReader* scene_reader = FS.r_open(map_name);
	if (!scene_reader)
	{
		error = "Cannot reopen scene after preflight: ";
		error += map_name;
		return false;
	}
	const bool is_ltx = scene_reader->elapsed() && *(const char*)scene_reader->pointer() == '[';
	FS.r_close(scene_reader);

	ClearLoadCandidate();
	xr_string load_error;
	const bool loaded = is_ltx ? LoadLTX(map_name, false, &load_error) : Load(map_name, false, &load_error);
	if (loaded)
	{
		InvalidateBuildCaches();
		return true;
	}

	IReader rollback_reader(rollback.pointer(), rollback.size());
	xr_string rollback_error;
	if (RestoreLoadRollback(rollback_reader, rollback_error))
	{
		m_RTFlags.assign(rt_flags);
		m_UndoObjectGeneration = undo_generation;
		m_PendingTransformGeneration = pending_generation;
		m_PendingTransformUndo = pending_transform;
		m_TransformUndoActive = transform_active;
		m_TransformUndoFallback = transform_fallback;
		m_UndoRollbackFailed = undo_rollback_failed;

		ViewportNavigation::ResetState();
		if (navigation_target != ViewportNavigation::Target::Perspective)
			ViewportNavigation::SetTarget(navigation_target);
		EDevice->m_Camera.Set(camera_hpb, camera_position, camera_target);
		EDevice->m_Camera.SetStyle(EDevice->m_Camera.GetStyle());
		error = load_error.empty() ? "Scene load failed" : load_error;
		return false;
	}

	m_LastLoadRollbackSucceeded = false;
	InvalidateBuildCaches();
	m_RTFlags.set(flRT_Unsaved | flRT_Modified, TRUE);
	m_UndoRollbackFailed = true;
	error = load_error.empty() ? "Scene load failed" : load_error;
	error += "; rollback failed: ";
	error += rollback_error.empty() ? "unknown rollback error" : rollback_error;
	return false;
}


//copy/paste utils

void EScene::SaveSelection( ObjClassID classfilter, LPCSTR fname )
{
	VERIFY			( fname );

    xr_string 		full_name;
    full_name 		= fname;

    IWriter* F		= FS.w_open(full_name.c_str());  R_ASSERT(F);

    F->open_chunk	(CHUNK_VERSION);
    F->w_u32	   	(CURRENT_FILE_VERSION);
    F->close_chunk	();

    m_SaveCache.clear();
    if (OBJCLASS_DUMMY==classfilter)
    {
        SceneToolsMapPairIt _I = m_SceneTools.begin();
        SceneToolsMapPairIt _E = m_SceneTools.end();
        for (; _I!=_E; ++_I)
            if (_I->second&&_I->second->IsNeedSave())
            {
                F->open_chunk				(CHUNK_TOOLS_DATA+_I->first);
                _I->second->SaveSelection	(m_SaveCache);
                F->w						(m_SaveCache.pointer(),m_SaveCache.size());
                m_SaveCache.clear			();
                F->close_chunk				();
            }
    }else{
    	ESceneToolBase* mt = GetTool(classfilter); VERIFY(mt);
        F->open_chunk	(CHUNK_TOOLS_DATA+classfilter);
        mt->SaveSelection(m_SaveCache);
        F->w			(m_SaveCache.pointer(),m_SaveCache.size());
        m_SaveCache.clear();
        F->close_chunk	();
    }
        
    FS.w_close		(F);
}


bool EScene::OnLoadSelectionAppendObject(CCustomObject* obj)
{
    string256 				buf;
    GenObjectName			(obj->FClassID,buf,obj->GetName());
    obj->SetName(buf);
    AppendObject			(obj, false);
    // paste/duplicate is the author adding a copy - even a copy of a base
    // object is THEIR addition and belongs to the mod
    obj->SetAuthorPlaced	(TRUE);
    obj->Select				(true);
    return 					true;
}


bool EScene::LoadSelection( LPCSTR fname )
{
    u32 version = 0;

	VERIFY( fname );

    xr_string 		full_name;
    full_name 		= fname;

	ELog.Msg( mtInformation, "EScene: loading part %s...", fname );

    bool res = true;

    if (FS.exist(full_name.c_str())){
		SelectObjects( false );

        IReader* F = FS.r_open(full_name.c_str());

        // Version
        R_ASSERT(F->r_chunk(CHUNK_VERSION, &version));
        if (version!=CURRENT_FILE_VERSION){
            ELog.DlgMsg( mtError, "EScene: unsupported file version. Can't load Level.");
            UI->UpdateScene();
            FS.r_close(F);
            return false;
        }

        // Objects
        if (!ReadObjectsStream(*F,CHUNK_OBJECT_LIST, TAppendObject (this,&EScene::OnLoadSelectionAppendObject),0))
        {
            ELog.DlgMsg(mtError,"EScene. Failed to load selection.");
            res = false;
        }

        SceneToolsMapPairIt _I = m_SceneTools.begin();
        SceneToolsMapPairIt _E = m_SceneTools.end();
        for (; _I!=_E; _I++)
            if (_I->second&&_I->second->IsEnabled()&&_I->second->IsEditable()){
			    IReader* chunk 		= F->open_chunk(CHUNK_TOOLS_DATA+_I->first);
            	if (chunk){
	                _I->second->LoadSelection(*chunk);
    	            chunk->close	();
                }
            }
        // Synchronize
		SynchronizeObjects();
		FS.r_close(F);
    }
	return res;
}


#pragma pack(push,1)
struct SceneClipData {
	int m_ClassFilter;
	char m_FileName[MAX_PATH];
};
#pragma pack(pop)

void EScene::CopySelection( ObjClassID classfilter )
{
	HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE, sizeof(SceneClipData) );
	SceneClipData *sceneclipdata = (SceneClipData *)GlobalLock(hmem);

	sceneclipdata->m_ClassFilter = classfilter;
	GetTempFileName( FS.get_path(_temp_)->m_Path, "clip", 0, sceneclipdata->m_FileName );
	SaveSelection( classfilter, sceneclipdata->m_FileName );

	GlobalUnlock( hmem );

	int clipformat = RegisterClipboardFormat( "CF_XRAY_CLASS_LIST" );
	if( OpenClipboard( 0 ) ){
		SetClipboardData( clipformat, hmem );
		CloseClipboard();
	} else {
		ELog.DlgMsg( mtError, "Failed to open clipboard" );
		GlobalFree( hmem );
	}
}

void EScene::PasteSelection() 
{
	int clipformat = RegisterClipboardFormat( "CF_XRAY_CLASS_LIST" );
	if( OpenClipboard( 0 ) ){

		HGLOBAL hmem = GetClipboardData(clipformat);
		if( hmem ){
			SceneClipData *sceneclipdata = (SceneClipData *)GlobalLock(hmem);
			LoadSelection( sceneclipdata->m_FileName );
			GlobalUnlock( hmem );
		} else {
			ELog.DlgMsg( mtError, "No data in clipboard" );
		}

		CloseClipboard();

	} else {
		ELog.DlgMsg( mtError, "Failed to open clipboard" );
	}
}

void EScene::CutSelection( ObjClassID classfilter )
{
	CopySelection( classfilter );
	RemoveSelection( classfilter );
}


void EScene::LoadCompilerError(LPCSTR fn)
{
    Tools->ClearDebugDraw();
/*
	CInifile		ini(fn);
   	string256		buff;
    LPCSTR			sect;
	u32				sz, i;

    sect			= "t-junction";
	sz 				= ini.r_u32(sect,"count");
    Tools->m_DebugDraw.m_Points.resize(sz);
	for(i=0; i<sz; ++i)
    {
    	CLevelTool::SDebugDraw::Point& pt = Tools->m_DebugDraw.m_Points[i];
        sprintf		(buff,"%d_p",i);
		pt.p[0]		= ini.r_fvector3(sect,buff);

        sprintf		(buff,"%d_c",i);
		pt.c		= ini.r_u32	(sect,buff);

        sprintf		(buff,"%d_i",i);
		pt.i		= ini.r_bool(sect,buff);

        sprintf		(buff,"%d_m",i);
		pt.m		= ini.r_bool(sect,buff);
    }

    sect			= "m-edje";
	sz 			= ini.r_u32(sect,"count");
    Tools->m_DebugDraw.m_Lines.resize(sz);
	for(i=0; i<sz; ++i)
    {
    	CLevelTool::SDebugDraw::Line& pt = Tools->m_DebugDraw.m_Lines[i];
        sprintf		(buff,"%d_p0",i);
		pt.p[0]		= ini.r_fvector3(sect,buff);

        sprintf		(buff,"%d_p1",i);
		pt.p[1]		= ini.r_fvector3(sect,buff);

        sprintf		(buff,"%d_c",i);
		pt.c		= ini.r_u32	(sect,buff);

        sprintf		(buff,"%d_i",i);
		pt.i		= ini.r_bool(sect,buff);

        sprintf		(buff,"%d_m",i);
		pt.m		= ini.r_bool(sect,buff);
    }
    sect			= "invalid_face";
	sz 			= ini.r_u32(sect,"count");
    Tools->m_DebugDraw.m_WireFaces.resize(sz);
	for(i=0; i<sz; ++i)
    {
    	CLevelTool::SDebugDraw::Face& pt = Tools->m_DebugDraw.m_WireFaces[i];
        sprintf		(buff,"%d_p0",i);
		pt.p[0]		= ini.r_fvector3(sect,buff);

        sprintf		(buff,"%d_p1",i);
		pt.p[1]		= ini.r_fvector3(sect,buff);

        sprintf		(buff,"%d_p2",i);
		pt.p[2]		= ini.r_fvector3(sect,buff);

        sprintf		(buff,"%d_c",i);
		pt.c		= ini.r_u32	(sect,buff);

        sprintf		(buff,"%d_i",i);
		pt.i		= ini.r_bool(sect,buff);

        sprintf		(buff,"%d_m",i);
		pt.m		= ini.r_bool(sect,buff);
    }
*/

	IReader* F	= FS.r_open(fn);
    Tools->ClearDebugDraw();
    Fvector 		pt[3];
    if (F->find_chunk(10)){ // lc error (TJ)
        Tools->m_DebugDraw.m_Points.resize(F->r_u32());
        F->r(Tools->m_DebugDraw.m_Points.data(),sizeof(CLevelTool::SDebugDraw::Point)*Tools->m_DebugDraw.m_Points.size());
    }else if (F->find_chunk(0)){ // lc error (TJ)
    	u32 cnt			= F->r_u32();
        for (u32 k=0;k<cnt; k++){ F->r(pt,sizeof(Fvector)); Tools->m_DebugDraw.AppendPoint(pt[0],0xff00ff00,true,true,"TJ"); }
    }
/*    
    if (F->find_chunk(11)){ // lc error (multiple edges)
        Tools->m_DebugDraw.m_Lines.resize(F->r_u32());
        F->r(Tools->m_DebugDraw.m_Lines.begin(),sizeof(CLevelTool::SDebugDraw::Line)*Tools->m_DebugDraw.m_Lines.size());
    }else if (F->find_chunk(1)){ // lc error (multiple edges)
    	u32 cnt			= F->r_u32();
        for (u32 k=0;k<cnt; k++){ F->r(pt,sizeof(Fvector)*2); Tools->m_DebugDraw.AppendLine(pt[0],pt[1],0xff0000ff,false,false); }
    }
*/
    if (F->find_chunk(12)){ // lc error (invalid faces)
        Tools->m_DebugDraw.m_WireFaces.resize(F->r_u32());
        F->r(Tools->m_DebugDraw.m_WireFaces.data(),sizeof(CLevelTool::SDebugDraw::Face)*Tools->m_DebugDraw.m_WireFaces.size());
    }else if (F->find_chunk(2)){ // lc error (invalid faces)
    	u32 cnt			= F->r_u32();
        for (u32 k=0;k<cnt; k++){ F->r(pt,sizeof(Fvector)*3); Tools->m_DebugDraw.AppendWireFace(pt[0],pt[1],pt[2]); }
    }
    FS.r_close(F);

}

void EScene::SaveCompilerError(LPCSTR fn)
{
/*
	CInifile		ini(fn,FALSE,FALSE,TRUE);
   	string256		buff;
    LPCSTR			sect;
    u32				sz, i;

    sz 				= Tools->m_DebugDraw.m_Points.size();
    sect			= "t-junction";
    ini.w_u32		(sect,"count",sz);
    for(i=0; i<sz; ++i)
    {
        sprintf		(buff,"%d_p",i);
        ini.w_fvector3(sect,buff,Tools->m_DebugDraw.m_Points[i].p[0]);

        sprintf		(buff,"%d_c",i);
        ini.w_u32	(sect,buff,Tools->m_DebugDraw.m_Points[i].c);

        sprintf		(buff,"%d_i",i);
        ini.w_bool	(sect,buff,Tools->m_DebugDraw.m_Points[i].i);

        sprintf		(buff,"%d_m",i);
        ini.w_bool	(sect,buff,Tools->m_DebugDraw.m_Points[i].m);
    }

    sz 				= Tools->m_DebugDraw.m_Lines.size();
    sect			= "m-edje";
    ini.w_u32		("sect","count",sz);
    for(i=0; i<sz; ++i)
    {
        sprintf		(buff,"%d_p0",i);
        ini.w_fvector3(sect,buff,Tools->m_DebugDraw.m_Lines[i].p[0]);

        sprintf		(buff,"%d_p1",i);
        ini.w_fvector3(sect,buff,Tools->m_DebugDraw.m_Lines[i].p[1]);

        sprintf		(buff,"%d_c",i);
        ini.w_u32	(sect,buff,Tools->m_DebugDraw.m_Lines[i].c);

        sprintf		(buff,"%d_i",i);
        ini.w_bool	(sect,buff,Tools->m_DebugDraw.m_Lines[i].i);

        sprintf		(buff,"%d_m",i);
        ini.w_bool	(sect,buff,Tools->m_DebugDraw.m_Lines[i].m);
    }

    sz 				= Tools->m_DebugDraw.m_WireFaces.size();
    sect			= "invalid_face";
    ini.w_u32		(sect,"count",sz);
    for(i=0; i<sz; ++i)
    {
        sprintf		(buff,"%d_p0",i);
        ini.w_fvector3(sect,buff,Tools->m_DebugDraw.m_WireFaces[i].p[0]);

        sprintf		(buff,"%d_p1",i);
        ini.w_fvector3(sect,buff,Tools->m_DebugDraw.m_WireFaces[i].p[1]);

        sprintf		(buff,"%d_p2",i);
        ini.w_fvector3(sect,buff,Tools->m_DebugDraw.m_WireFaces[i].p[2]);

        sprintf		(buff,"%d_c",i);
        ini.w_u32	(sect,buff,Tools->m_DebugDraw.m_WireFaces[i].c);

        sprintf		(buff,"%d_i",i);
        ini.w_bool	(sect,buff,Tools->m_DebugDraw.m_WireFaces[i].i);

        sprintf		(buff,"%d_m",i);
        ini.w_bool	(sect,buff,Tools->m_DebugDraw.m_WireFaces[i].m);
    }
*/
	IWriter*		fs	= FS.w_open(fn);  R_ASSERT(fs);
	IWriter&		err = *fs;

	// t-junction
	err.open_chunk	(10);
	err.w_u32		(Tools->m_DebugDraw.m_Points.size());
	err.w			(Tools->m_DebugDraw.m_Points.data(), Tools->m_DebugDraw.m_Points.size()*sizeof(CLevelTool::SDebugDraw::Point));
	err.close_chunk	();

	// m-edje
	err.open_chunk	(11);
	err.w_u32		(Tools->m_DebugDraw.m_Lines.size());
	err.w			(Tools->m_DebugDraw.m_Lines.data(), Tools->m_DebugDraw.m_Lines.size()*sizeof(CLevelTool::SDebugDraw::Line));
	err.close_chunk	();

	// invalid
	err.open_chunk	(12);
	err.w_u32		(Tools->m_DebugDraw.m_WireFaces.size());
	err.w			(Tools->m_DebugDraw.m_WireFaces.data(), Tools->m_DebugDraw.m_WireFaces.size()*sizeof(CLevelTool::SDebugDraw::Face));
	err.close_chunk	();

    FS.w_close		(fs);

}


void EScene::ExportObj(bool b_selected_only)
{
	Builder.m_save_as_object 	= true;
	Builder.Compile				(b_selected_only);
	Builder.m_save_as_object 	= false;

}

