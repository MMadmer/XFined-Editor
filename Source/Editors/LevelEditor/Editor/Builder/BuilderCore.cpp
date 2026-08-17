#include "stdafx.h"


bool SceneBuilder::PreparePath()
{
	if (Scene->m_LevelOp.m_FNLevelPath.size()==0) return false;
    FS.update_path	(m_LevelPath,_game_levels_,Scene->m_LevelOp.m_FNLevelPath.c_str());
    strcat(m_LevelPath,"\\");
    return true;
}


bool SceneBuilder::PrepareFolders()
{
	FS.dir_delete	(m_LevelPath,TRUE);
	return true;
}


bool SceneBuilder::EvictResource()
{
	ExecCommand(COMMAND_EVICT_OBJECTS);
    ExecCommand(COMMAND_EVICT_TEXTURES);

	int objcount = Scene->ObjCount(OBJCLASS_SCENEOBJECT);
	if( objcount <= 0 ) return true;

	SPBItem* pb = UI->ProgressStart(objcount, "Evict objects...");
    // unload cform, point normals
    ObjectIt _F = Scene->FirstObj(OBJCLASS_SCENEOBJECT);
    ObjectIt _E = Scene->LastObj(OBJCLASS_SCENEOBJECT);
    for(;_F!=_E;_F++){
    	CSceneObject* O = (CSceneObject*)(*_F);
		UI->ProgressCheckpoint();
        if (UI->NeedAbort()) break; // break building
        O->EvictObject();
        pb->Inc();
	}
	UI->ProgressEnd(pb);

    return true;
}


bool SceneBuilder::GetBounding()
{
	Fbox b0;
    bool r0 = Scene->GetBox(m_LevelBox,OBJCLASS_SCENEOBJECT);
    bool r1 = Scene->GetBox(b0,OBJCLASS_GROUP);
    if (r1) m_LevelBox.merge(b0);
	return (r0||r1);
}


bool SceneBuilder::RenumerateSectors()
{
	SPBItem* pb = UI->ProgressStart(Scene->ObjCount(OBJCLASS_SECTOR), "Renumerate sectors...");

	xr_vector<CSector*> sectors;
	sectors.reserve(Scene->ObjCount(OBJCLASS_SECTOR));
	int default_sector_num = -1;
	ObjectIt _F = Scene->FirstObj(OBJCLASS_SECTOR);
	ObjectIt _E = Scene->LastObj(OBJCLASS_SECTOR);
	for (; _F != _E; ++_F) {
		CSector* _S=(CSector*)(*_F);
		sectors.push_back(_S);
		if (_S->IsDefault()) default_sector_num = int(sectors.size()) - 1;
		pb->Inc();
		UI->ProgressCheckpoint();
		if (UI->NeedAbort()) {
			UI->ProgressEnd(pb);
			return false;
		}
	}

	UI->ProgressEnd(pb);

	for (u32 sector_num = 0; sector_num < sectors.size(); ++sector_num)
		sectors[sector_num]->m_sector_num = sector_num;
	m_iDefaultSectorNum = default_sector_num >= 0 ? default_sector_num : int(sectors.size());
	return true;
}


