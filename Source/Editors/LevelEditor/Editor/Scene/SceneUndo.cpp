#include "stdafx.h"

namespace
{
bool SameVector(const Fvector& left, const Fvector& right)
{
	return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool SameTransform(const CCustomObject& object, const UndoTransformValue& value)
{
	return SameVector(object.GetPosition(), value.Position) &&
		SameVector(object.GetRotation(), value.Rotation) &&
		SameVector(object.GetScale(), value.Scale);
}

bool ValidTransform(const UndoTransformValue& value)
{
	return _valid(value.Position) && _valid(value.Rotation) && _valid(value.Scale);
}

void CaptureTransform(const CCustomObject& object, UndoTransformValue& value)
{
	value.Position = object.GetPosition();
	value.Rotation = object.GetRotation();
	value.Scale = object.GetScale();
}

void SetTransform(CCustomObject& object, const UndoTransformValue& value)
{
	if (!SameVector(object.GetPosition(), value.Position))
		object.SetPosition(value.Position);
	if (!SameVector(object.GetRotation(), value.Rotation))
		object.SetRotation(value.Rotation);
	if (!SameVector(object.GetScale(), value.Scale))
		object.SetScale(value.Scale);
	object.UpdateTransform(true);
}

bool SameIdentity(const UndoTransformItem& left, const UndoTransformItem& right)
{
	return left.ClassID == right.ClassID && 0 == xr_strcmp(left.Name.c_str(), right.Name.c_str());
}

bool TransformFastPathSafe(CCustomObject& object)
{
	// Custom setters and owned objects can mutate state beyond the three base vectors.
	return object.FClassID != OBJCLASS_GROUP && object.FClassID != OBJCLASS_WAY &&
		object.FClassID != OBJCLASS_SPAWNPOINT && object.FClassID != OBJCLASS_SHAPE &&
		object.FClassID != OBJCLASS_GLOW && object.FClassID != OBJCLASS_SOUND_SRC &&
		!object.Motionable() && !object.GetOwner();
}
}

void EScene::ClearUndoStack(xr_deque<UndoItem>& stack)
{
	for (const UndoItem& item : stack)
	{
		if (item.Type == EUndoItemType::Snapshot && !item.FileName.empty())
			unlink(item.FileName.c_str());
	}
	stack.clear();
}

void EScene::UndoClear()
{
	UndoTransformCancel();
	ClearUndoStack(m_RedoStack);
	ClearUndoStack(m_UndoStack);
}

bool EScene::SaveUndoSnapshot(UndoItem& item)
{
	FS_Path* tempPath = FS.get_path(_temp_);
	if (!tempPath)
		return false;

	string_path fileName;
	if (!GetTempFileNameA(tempPath->m_Path, "undo", 0, fileName))
		return false;

	item = {};
	item.Type = EUndoItemType::Snapshot;
	item.FileName = fileName;
	xr_string error;
	if (!Save(item.FileName.c_str(), true, true, &error))
	{
		unlink(item.FileName.c_str());
		item = {};
		Msg("! Failed to save undo snapshot: %s", error.empty() ? "unknown error" : error.c_str());
		return false;
	}
	return true;
}

void EScene::MergeUndoBaseline(const UndoItem& transformState)
{
	VERIFY(!m_UndoStack.empty());
	UndoItem& baseline = m_UndoStack.front();
	VERIFY(baseline.Type == EUndoItemType::Snapshot);
	VERIFY(transformState.Type == EUndoItemType::Transform);

	for (const UndoTransformItem& source : transformState.Transforms)
	{
		auto existing = std::find_if(baseline.Transforms.begin(), baseline.Transforms.end(),
			[&source](const UndoTransformItem& candidate) { return SameIdentity(candidate, source); });
		if (existing != baseline.Transforms.end())
			existing->After = source.After;
		else
			baseline.Transforms.push_back(source);
	}
}

void EScene::TrimUndoStack()
{
	const size_t limit = EPrefs->scene_undo_level;
	while (m_UndoStack.size() > limit)
	{
		if (m_UndoStack.size() == 1)
		{
			ClearUndoStack(m_UndoStack);
			return;
		}

		UndoItem& nextState = m_UndoStack[1];
		if (nextState.Type == EUndoItemType::Transform)
		{
			// Keep the front snapshot as the exact reconstruction base when typed states age out.
			MergeUndoBaseline(nextState);
			m_UndoStack.erase(m_UndoStack.begin() + 1);
		}
		else
		{
			UndoItem& oldBaseline = m_UndoStack.front();
			if (!oldBaseline.FileName.empty())
				unlink(oldBaseline.FileName.c_str());
			m_UndoStack.pop_front();
		}
	}
}

void EScene::TrimRedoStack()
{
	const size_t limit = EPrefs->scene_undo_level;
	while (m_RedoStack.size() > limit)
	{
		UndoItem& item = m_RedoStack.front();
		if (item.Type == EUndoItemType::Snapshot && !item.FileName.empty())
			unlink(item.FileName.c_str());
		m_RedoStack.pop_front();
	}
}

void EScene::UndoSave()
{
	UndoTransformCancel();
	if (UI->GetEState() != esEditScene)
		return;

	Modified();
	UI->RedrawScene();

	if (0 == EPrefs->scene_undo_level)
		return;

	UndoItem item;
	if (!SaveUndoSnapshot(item))
		return;

	m_UndoStack.push_back(std::move(item));
	ClearUndoStack(m_RedoStack);
	TrimUndoStack();
}

bool EScene::UndoTransformBegin(const ObjectList& objects)
{
	UndoTransformCancel();
	m_TransformUndoFallback = true;

	if (UI->GetEState() != esEditScene || 0 == EPrefs->scene_undo_level ||
		GetCurrentThreadId() != m_UndoThreadID || objects.empty())
		return false;

	xr_vector<UndoTransformItem> captured;
	captured.reserve(objects.size());
	for (CCustomObject* object : objects)
	{
		if (!object || !ContainsObject(object, object->FClassID) || !TransformFastPathSafe(*object))
			return false;

		UndoTransformItem item;
		item.ClassID = object->FClassID;
		item.Name = object->GetName();
		CaptureTransform(*object, item.Before);
		if (!ValidTransform(item.Before))
			return false;

		const auto duplicate = std::find_if(captured.begin(), captured.end(),
			[&item](const UndoTransformItem& candidate) { return SameIdentity(candidate, item); });
		if (duplicate == captured.end())
			captured.push_back(std::move(item));
	}

	if (captured.empty())
		return false;

	if (m_UndoStack.empty())
	{
		UndoItem baseline;
		if (!SaveUndoSnapshot(baseline))
			return false;
		m_UndoStack.push_back(std::move(baseline));
	}

	m_PendingTransformUndo.swap(captured);
	m_PendingTransformGeneration = m_UndoObjectGeneration;
	m_TransformUndoActive = true;
	m_TransformUndoFallback = false;
	return true;
}

void EScene::UndoTransformCancel()
{
	m_PendingTransformUndo.clear();
	m_TransformUndoActive = false;
	m_TransformUndoFallback = false;
}

void EScene::UndoTransformEnd()
{
	if (m_TransformUndoFallback)
	{
		UndoTransformCancel();
		UndoSave();
		return;
	}
	if (!m_TransformUndoActive)
		return;

	if (GetCurrentThreadId() != m_UndoThreadID || m_PendingTransformGeneration != m_UndoObjectGeneration)
	{
		UndoTransformCancel();
		UndoSave();
		return;
	}

	UndoItem state;
	state.Type = EUndoItemType::Transform;
	state.Transforms.reserve(m_PendingTransformUndo.size());
	for (UndoTransformItem& pending : m_PendingTransformUndo)
	{
		CCustomObject* object = FindObjectByName(pending.Name.c_str(), pending.ClassID);
		if (!object || !TransformFastPathSafe(*object))
		{
			UndoTransformCancel();
			UndoSave();
			return;
		}

		CaptureTransform(*object, pending.After);
		if (!ValidTransform(pending.After))
		{
			UndoTransformCancel();
			UndoSave();
			return;
		}
		if (!SameTransform(*object, pending.Before))
			state.Transforms.push_back(pending);
	}

	UndoTransformCancel();
	if (state.Transforms.empty())
		return;

	m_UndoStack.push_back(std::move(state));
	ClearUndoStack(m_RedoStack);
	TrimUndoStack();
	Modified();
	UI->RedrawScene();
}

EUndoApplyResult EScene::ApplyUndoTransforms(const xr_vector<UndoTransformItem>& transforms, bool before,
	bool validateEdge)
{
	if (GetCurrentThreadId() != m_UndoThreadID)
		return EUndoApplyResult::EdgeMismatch;

	struct ResolvedTransform
	{
		CCustomObject* Object;
		const UndoTransformItem* Change;
	};

	xr_vector<ResolvedTransform> resolved;
	resolved.reserve(transforms.size());
	for (const UndoTransformItem& change : transforms)
	{
		CCustomObject* object = FindObjectByName(change.Name.c_str(), change.ClassID);
		const UndoTransformValue& target = before ? change.Before : change.After;
		const UndoTransformValue& edge = before ? change.After : change.Before;
		if (!object || !TransformFastPathSafe(*object) || !ValidTransform(target) ||
			(validateEdge && !SameTransform(*object, edge)))
			return EUndoApplyResult::EdgeMismatch;
		resolved.push_back({object, &change});
	}

	for (const ResolvedTransform& item : resolved)
	{
		const UndoTransformValue& target = before ? item.Change->Before : item.Change->After;
		SetTransform(*item.Object, target);
	}
	for (const ResolvedTransform& item : resolved)
	{
		const UndoTransformValue& target = before ? item.Change->Before : item.Change->After;
		if (!SameTransform(*item.Object, target))
		{
			for (const ResolvedTransform& rollback : resolved)
			{
				const UndoTransformValue& edge = before ? rollback.Change->After : rollback.Change->Before;
				SetTransform(*rollback.Object, edge);
			}
			for (const ResolvedTransform& rollback : resolved)
			{
				const UndoTransformValue& edge = before ? rollback.Change->After : rollback.Change->Before;
				if (!SameTransform(*rollback.Object, edge))
					return EUndoApplyResult::RollbackFailed;
			}
			return EUndoApplyResult::RolledBack;
		}
	}
	return EUndoApplyResult::Success;
}

void EScene::HandleUndoRollbackFailure(LPCSTR operation)
{
	m_UndoRollbackFailed = true;
	ELog.Msg(mtError, "%s transform rollback was incomplete; the scene was marked modified and snapshot fallback was not started from damaged state.", operation);
	LTools->Reset();
	ExecCommand(COMMAND_CHANGE_ACTION, etaSelect);
	ExecCommand(COMMAND_UPDATE_PROPERTIES, 1);
	m_RTFlags.set(flRT_Unsaved | flRT_Modified, TRUE);
	UI->UpdateScene(true);
	UI->RedrawScene(true);
	ExecCommand(COMMAND_UPDATE_CAPTION);
}

bool EScene::RestoreUndoSnapshot(const UndoItem& snapshot, const xr_vector<const UndoItem*>& replay)
{
	if (snapshot.Type != EUndoItemType::Snapshot || snapshot.FileName.empty())
		return false;

	xr_string why;
	if (!IsSceneFile(snapshot.FileName.c_str(), why))
	{
		ELog.Msg(mtError, "Undo snapshot preflight failed for '%s': %s", snapshot.FileName.c_str(), why.c_str());
		return false;
	}
	for (const UndoTransformItem& transform : snapshot.Transforms)
	{
		if (!ValidTransform(transform.Before) || !ValidTransform(transform.After))
			return false;
	}
	for (const UndoItem* item : replay)
	{
		if (!item || item->Type != EUndoItemType::Transform)
			return false;
		for (const UndoTransformItem& transform : item->Transforms)
		{
			if (!ValidTransform(transform.Before) || !ValidTransform(transform.After))
				return false;
		}
	}

	// Snapshot loading replaces every editable object, so preserve a verified rollback first.
	UndoItem rollback;
	if (!SaveUndoSnapshot(rollback))
		return false;
	xr_string rollbackWhy;
	if (!IsSceneFile(rollback.FileName.c_str(), rollbackWhy))
	{
		ELog.Msg(mtError, "Undo rollback snapshot is invalid: %s", rollbackWhy.c_str());
		unlink(rollback.FileName.c_str());
		return false;
	}

	const BOOL wasUnsaved = m_RTFlags.test(flRT_Unsaved);
	const BOOL wasModified = m_RTFlags.test(flRT_Modified);
	Unload(TRUE);
	bool restored = Load(snapshot.FileName.c_str(), true);
	if (restored)
		restored = ApplyUndoTransforms(snapshot.Transforms, false, false) == EUndoApplyResult::Success;
	if (restored)
	{
		for (const UndoItem* item : replay)
		{
			if (ApplyUndoTransforms(item->Transforms, false, true) != EUndoApplyResult::Success)
			{
				restored = false;
				break;
			}
		}
	}

	if (!restored)
	{
		Unload(TRUE);
		if (Load(rollback.FileName.c_str(), true))
		{
			LTools->Reset();
			ExecCommand(COMMAND_CHANGE_ACTION, etaSelect);
			ExecCommand(COMMAND_UPDATE_PROPERTIES, 1);
			UI->UpdateScene(true);
			UI->RedrawScene(true);
			m_RTFlags.set(flRT_Unsaved, wasUnsaved);
			m_RTFlags.set(flRT_Modified, wasModified);
			ExecCommand(COMMAND_UPDATE_CAPTION);
			unlink(rollback.FileName.c_str());
		}
		else
		{
			ELog.Msg(mtError, "Undo rollback failed. Recoverable emergency snapshot retained at '%s'.",
				rollback.FileName.c_str());
			LTools->Reset();
			ExecCommand(COMMAND_CHANGE_ACTION, etaSelect);
			ExecCommand(COMMAND_UPDATE_PROPERTIES, 1);
			m_RTFlags.set(flRT_Unsaved | flRT_Modified, TRUE);
			UI->UpdateScene(true);
			UI->RedrawScene(true);
			ExecCommand(COMMAND_UPDATE_CAPTION);
		}
		return false;
	}
	unlink(rollback.FileName.c_str());
	return true;
}

bool EScene::RestoreUndoState(size_t index, const UndoItem* finalTransform)
{
	if (index >= m_UndoStack.size())
		return false;

	size_t checkpoint = index + 1;
	while (checkpoint > 0)
	{
		--checkpoint;
		if (m_UndoStack[checkpoint].Type == EUndoItemType::Snapshot)
			break;
	}
	const UndoItem& snapshot = m_UndoStack[checkpoint];
	if (snapshot.Type != EUndoItemType::Snapshot || snapshot.FileName.empty())
		return false;

	xr_vector<const UndoItem*> replay;
	replay.reserve(index - checkpoint);
	for (size_t itemIndex = checkpoint + 1; itemIndex <= index; ++itemIndex)
		replay.push_back(&m_UndoStack[itemIndex]);
	if (finalTransform)
		replay.push_back(finalTransform);
	return RestoreUndoSnapshot(snapshot, replay);
}

bool EScene::Undo()
{
	m_UndoRollbackFailed = false;
	if (m_TransformUndoActive || m_TransformUndoFallback)
		UndoTransformEnd();
	if (m_UndoStack.size() <= 1)
		return false;

	const size_t targetIndex = m_UndoStack.size() - 2;
	const UndoItem& current = m_UndoStack.back();
	bool restored = false;
	if (current.Type == EUndoItemType::Transform)
	{
		const EUndoApplyResult result = ApplyUndoTransforms(current.Transforms, true, true);
		restored = result == EUndoApplyResult::Success;
		if (!restored && result == EUndoApplyResult::RollbackFailed)
		{
			HandleUndoRollbackFailure("Undo");
			return false;
		}
	}
	if (!restored)
		restored = RestoreUndoState(targetIndex);
	if (!restored)
		return false;

	m_RedoStack.push_back(current);
	m_UndoStack.pop_back();
	TrimRedoStack();
	UI->UpdateScene();
	Modified();
	return true;
}

bool EScene::Redo()
{
	m_UndoRollbackFailed = false;
	if (m_TransformUndoActive || m_TransformUndoFallback)
		UndoTransformEnd();
	if (m_RedoStack.empty())
		return false;

	const UndoItem& target = m_RedoStack.back();
	bool restored = false;
	if (target.Type == EUndoItemType::Transform)
	{
		const EUndoApplyResult result = ApplyUndoTransforms(target.Transforms, false, true);
		restored = result == EUndoApplyResult::Success;
		if (!restored && result == EUndoApplyResult::RollbackFailed)
		{
			HandleUndoRollbackFailure("Redo");
			return false;
		}
		if (!restored && !m_UndoStack.empty())
			restored = RestoreUndoState(m_UndoStack.size() - 1, &target);
	}
	else if (!target.FileName.empty())
	{
		xr_vector<const UndoItem*> replay;
		restored = RestoreUndoSnapshot(target, replay);
	}
	if (!restored)
		return false;

	m_UndoStack.push_back(target);
	m_RedoStack.pop_back();
	TrimUndoStack();
	UI->UpdateScene();
	Modified();
	return true;
}
