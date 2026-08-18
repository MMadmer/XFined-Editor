#include "stdafx.h"

void ESceneGroupTool::CreateControls()
{
	inherited::CreateDefaultControls(estDefault);
    AddControl		(xr_new<TUI_ControlGroupAdd >(estDefault,etaAdd,		this));
	// frame
    pForm = xr_new< UIGroupTool>();
    ((UIGroupTool*)pForm)->ParentTools = this;
}


void ESceneGroupTool::RemoveControls()
{
	inherited::RemoveControls();
}


void ESceneGroupTool::UngroupObjects(bool bUndo)
{
    ObjectList lst 	= m_Objects;
    int sel_cnt		= 0;
    if (!lst.empty())
    {
    	bool bModif	= false;
        for (ObjectIt it=lst.begin(); it!=lst.end(); ++it)
        {
            if ((*it)->Selected())
            {
            	sel_cnt++;
            	CGroupObject* obj 	= dynamic_cast<CGroupObject*>(*it); 
                VERIFY(obj);
                if (obj->CanUngroup(true))
                {
                    obj->UngroupObjects	();
                    Scene->RemoveObject	(obj,false,true);
                    xr_delete			(obj);
                    bModif				= true;
                }else
                    ELog.DlgMsg			(mtError,"Can't ungroup object: '%s'.",obj->GetName());
            }
        }
        if (bUndo&&bModif) 
            Scene->UndoSave();
    }
    if (0==sel_cnt)
        ELog.Msg		(mtError,"Nothing selected.");
}


BOOL  ESceneGroupTool::_RemoveObject(CCustomObject* object)
{
	inherited::_RemoveObject(object);

    CGroupObject* go 	= dynamic_cast<CGroupObject*>(object); 
    go->Clear1          ();
    return              TRUE;
}

void ESceneGroupTool::GroupObjects(bool bUndo)
{
    string256                   namebuffer;
    Scene->GenObjectName        (OBJCLASS_GROUP, namebuffer);
    CGroupObject* group         = xr_new<CGroupObject>((LPVOID)0, namebuffer);

    // validate objects
    ObjectList lst;
    if (Scene->GetQueryObjects(lst,OBJCLASS_DUMMY,1,1,0)) 
    	group->GroupObjects(lst);
        
    if (group->ObjectInGroupCount())
    {
		ELog.DlgMsg(mtInformation,"Group '%s' successfully created.\nContain %d object(s)",group->GetName(),group->ObjectInGroupCount());
        Scene->AppendObject(group, bUndo);
    }else
	{
		ELog.DlgMsg	(mtError,"Group can't be created.");
        xr_delete	(group);
    }
}

void ESceneGroupTool::CenterToGroup()
{
    ObjectList& lst 	= m_Objects;
    if (!lst.empty())
    {
    	for (ObjectIt it=lst.begin(); it!=lst.end(); ++it)
        	((CGroupObject*)(*it))->UpdatePivot(0, true);

    	Scene->UndoSave();
    }
}


void   FillGroupItems(ChooseItemVec& items, void* param)
{
	CGroupObject* group = (CGroupObject*)param;
    ObjectList 			grp_lst;
    group->GetObjects	(grp_lst);
    
    for (ObjectIt it=grp_lst.begin(); it!=grp_lst.end(); ++it)
	    items.push_back	(SChooseItem((*it)->GetName(),""));
}
void ESceneGroupTool::OnDrawUI()
{
    if ((*m_ChooseIt)->Selected())
    {
        if (UIChooseForm::IsActive())
        {
            bool ok;
            xr_string name;
            if (UIChooseForm::GetResult(ok, name))
            {
                m_ChooseCnt++;
                if (ok)
                {
                    ((CGroupObject*)(*m_ChooseIt))->UpdatePivot(name.c_str(), false);
                }
                m_ChooseIt++;
            }
            UIChooseForm::Update();
        }
        else
        {
            UIChooseForm::SelectItem(smCustom, 1, "", FillGroupItems, *m_ChooseIt);
        }
       
    }
    else
    {
        m_ChooseIt++;
    }
    if (m_ChooseIt == m_Objects.end())
    {
        if (0 == m_ChooseCnt)
            ELog.Msg(mtError, "Nothing selected.");
        else 
            Scene->UndoSave();
        EDevice->seqDrawUI.Remove(this);
    }
}
void ESceneGroupTool::AlignToObject()
{
    m_ChooseIt = m_Objects.begin();
    m_ChooseCnt = 0;
    EDevice->seqDrawUI.Add(this);  
}


CCustomObject* ESceneGroupTool::CreateObject(LPVOID data, LPCSTR name)
{
	CCustomObject* O	= xr_new<CGroupObject>(data, name);
    O->FParentTools		= this;
    return O;
}

void ESceneGroupTool::ReloadRefsSelectedObject()
{
    xr_vector<CGroupObject*> selected_groups;
    for (CCustomObject* object : m_Objects)
    {
        if (!object->Selected())
            continue;

        CGroupObject* group = dynamic_cast<CGroupObject*>(object);
        VERIFY(group);
        selected_groups.push_back(group);
    }

    if (selected_groups.empty())
    {
        ELog.Msg(mtError, "Nothing selected.");
        return;
    }

	FS_Path* temp_path = FS.get_path(_temp_);
	if (!temp_path)
	{
		ELog.DlgMsg(mtError, "Can't reload groups: temporary path is unavailable.");
		return;
	}

	string_path temp_file_name_group = {};
	string_path temp_file_name_sector = {};
	string_path temp_file_name_portal = {};
	auto remove_tool_files = [](LPCSTR base, int count)
	{
		for (int i = 0; i < count; ++i)
		{
			string_path file_name;
			if (i)
				xr_sprintf(file_name, "%s%d", base, i);
			else
				xr_strcpy(file_name, base);
			unlink(file_name);
		}
	};
	auto remove_temp_files = [&]()
	{
		if (temp_file_name_group[0])
			remove_tool_files(temp_file_name_group, Scene->GetTool(OBJCLASS_GROUP)->SaveFileCount());
		if (temp_file_name_sector[0])
			remove_tool_files(temp_file_name_sector, Scene->GetTool(OBJCLASS_SECTOR)->SaveFileCount());
		if (temp_file_name_portal[0])
			remove_tool_files(temp_file_name_portal, Scene->GetTool(OBJCLASS_PORTAL)->SaveFileCount());
	};

	xr_string save_error;
	if (!GetTempFileNameA(temp_path->m_Path, "grp_group", 0, temp_file_name_group) ||
		!Scene->SaveToolLTX(OBJCLASS_GROUP, temp_file_name_group, &save_error))
	{
		remove_temp_files();
		ELog.DlgMsg(mtError, "%s", save_error.empty() ?
			"Can't preserve groups before reloading their references." : save_error.c_str());
		return;
	}

	if (!GetTempFileNameA(temp_path->m_Path, "grp_sector", 0, temp_file_name_sector) ||
		!Scene->SaveToolLTX(OBJCLASS_SECTOR, temp_file_name_sector, &save_error))
	{
		remove_temp_files();
		ELog.DlgMsg(mtError, "%s", save_error.empty() ?
			"Can't preserve sectors before reloading groups." : save_error.c_str());
		return;
	}

	if (!GetTempFileNameA(temp_path->m_Path, "grp_portal", 0, temp_file_name_portal) ||
		!Scene->SaveToolLTX(OBJCLASS_PORTAL, temp_file_name_portal, &save_error))
	{
		remove_temp_files();
		ELog.DlgMsg(mtError, "%s", save_error.empty() ?
			"Can't preserve portals before reloading groups." : save_error.c_str());
		return;
	}

	const BOOL was_unsaved = Scene->m_RTFlags.test(EScene::flRT_Unsaved);
	const BOOL was_modified = Scene->m_RTFlags.test(EScene::flRT_Modified);
	auto rollback_tools = [&](LPCSTR reason)
	{
		const bool group_restored = Scene->LoadToolLTX(OBJCLASS_GROUP, temp_file_name_group);
		const bool sector_restored = Scene->LoadToolLTX(OBJCLASS_SECTOR, temp_file_name_sector);
		const bool portal_restored = Scene->LoadToolLTX(OBJCLASS_PORTAL, temp_file_name_portal);
		const bool restored = group_restored && sector_restored && portal_restored;
		if (restored)
		{
			Scene->m_RTFlags.set(EScene::flRT_Unsaved, was_unsaved);
			Scene->m_RTFlags.set(EScene::flRT_Modified, was_modified);
		}
		else
		{
			Scene->Modified();
		}
		LTools->Reset();
		ExecCommand(COMMAND_CHANGE_ACTION, etaSelect);
		ExecCommand(COMMAND_UPDATE_PROPERTIES, 1);
		ExecCommand(COMMAND_UPDATE_CAPTION);
		UI->UpdateScene(true);
		UI->RedrawScene(true);

		if (restored)
		{
			remove_temp_files();
			ELog.DlgMsg(mtError, "%s\nThe affected scene tools were restored.", reason);
			return;
		}

		ELog.DlgMsg(mtError,
			"%s\nAutomatic rollback failed. Recovery copies were kept:\nGroups: %s\nSectors: %s\nPortals: %s",
			reason, temp_file_name_group, temp_file_name_sector, temp_file_name_portal);
	};

    for (CGroupObject* group : selected_groups)
	{
        if (!group->UpdateReference(true))
        {
			xr_string error;
			error.sprintf("Can't reload group: '%s'.", group->GetName());
			rollback_tools(error.c_str());
			return;
        }
    }

	if (!Scene->LoadToolLTX(OBJCLASS_SECTOR, temp_file_name_sector))
	{
		rollback_tools("Can't restore sectors after reloading groups.");
		return;
	}
	if (!Scene->LoadToolLTX(OBJCLASS_PORTAL, temp_file_name_portal))
	{
		rollback_tools("Can't restore portals after reloading groups.");
		return;
	}

	remove_temp_files();
	Scene->UndoSave();
}


void ESceneGroupTool::SaveSelectedObject()
{
	u32 scnt = SelectionCount(true);
	if(scnt==0)
    {
        ELog.DlgMsg(mtError,"No object(s) selected.");
        return;
    }else
	if(scnt>1)
    {
        if(mrYes != ELog.DlgMsg(mtConfirmation, mbYes | mbNo, "Process multiple objects?") )
        	return;
    }
    
	CGroupObject* obj	= 0;
	// find single selected object
    for(ObjectIt it=m_Objects.begin(); it!=m_Objects.end(); ++it)
    {
    	if((*it)->Selected())
        {
        	obj 		= dynamic_cast<CGroupObject*>(*it);
            
            xr_string fn;
            if(scnt==1)
            {
            	fn 					= obj->RefName();
                if( !EFS.GetSaveName(_groups_,fn) )
                    return;
            }else
            {
            	string_path 		S;
               FS.update_path		(S, _groups_, obj->RefName());
               fn 					= S;
               fn 					+= ".group";
            }
            
            IWriter* W 				= FS.w_open(fn.c_str());
            if (W)
            {
                obj->SaveStream		(*W);
                FS.w_close			(W);
            }else
                ELog.DlgMsg			(mtError, "Cant write file [%s]", fn.c_str());
      }          
    }
}


void ESceneGroupTool::SetCurrentObject(LPCSTR nm)
{
	m_CurrentObject				= nm;
/*	TfraGroup* frame			=(TfraGroup*)pFrame;
    frame->lbCurrent->Caption 	= m_CurrentObject.c_str();*/
}


void ESceneGroupTool::OnActivate()
{
	inherited::OnActivate		();
	/*TfraGroup* frame			= (TfraGroup*)pFrame;
    frame->lbCurrent->Caption 	= m_CurrentObject.c_str();*/
}


void ESceneGroupTool::MakeThumbnail()
{
	if (SelectionCount(true)==1)
    {
	    CGroupObject* object		= 0;
        for (ObjectIt it=m_Objects.begin(); it!=m_Objects.end(); it++)
        {
        	if ((*it)->Selected())
            {
	            object				= dynamic_cast<CGroupObject*>(*it);
                break;
            }
        }
        VERIFY						(object);
        object->Select				(false);
        // save render params
        Flags32 old_flag			= psDeviceFlags;
        // set render params
        psDeviceFlags.set			(rsStatistic|rsDrawGrid,FALSE);

        U32Vec pixels;
        u32 w=512,h=512;
        if (EDevice->MakeScreenshot	(pixels,w,h))
        {
            xr_string tex_name		= ChangeFileExt(object->GetName(),".thm");
            SStringVec lst;

            ObjectList 				grp_lst;
            object->GetObjects		(grp_lst);
            
            for (ObjectIt it=grp_lst.begin(); it!=grp_lst.end(); ++it)
                lst.push_back		((*it)->GetName());
                
            EGroupThumbnail 		tex	(tex_name.c_str(),false);
            tex.CreateFromData		(pixels.data(),w,h,lst);
            string_path fn;
            FS.update_path			(fn,_groups_,object->RefName());
            strcat					(fn,".group");
            tex.Save				(FS.get_file_age(fn));
        }else
        {
            ELog.DlgMsg				(mtError,"Can't make screenshot.");
        }
        object->Select				(true);
        // restore render params
        psDeviceFlags 				= old_flag;
    }else
    {
    	ELog.DlgMsg		(mtError,"Select 1 GroupObject.");
    }
}
