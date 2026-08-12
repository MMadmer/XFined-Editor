#include "stdafx.h"
#include "..\..\XrAPI\xrGameManager.h"
#include "..\..\XrECore\Editor\EditorModManifest.h"
#include "..\..\XrECore\Editor\EditorGameContent.h"
#include "..\..\XrECore\Editor\EditorFileOps.h"
#include "..\..\XrECore\Editor\EThumbnailVisual.h"
#include "EditorModScene.h"
#include "Utils\Cursor3D.h"
#include "..\xrengine\GameFont.h"

#ifdef _LEVEL_EDITOR
//.    if (m_Cursor->GetVisible()) RedrawScene();
#endif

CLevelMain*	LUI=(CLevelMain*)UI;

CLevelMain::CLevelMain()
{
    m_Cursor        = xr_new<C3DCursor>();
    EPrefs			= xr_new<CLevelPreferences>();
}

CLevelMain::~CLevelMain()
{
    xr_delete		(EPrefs);
    xr_delete		(m_Cursor);
   // TClipMaker::DestroyForm(g_clip_maker);
}



// Tools commands
CCommandVar CLevelTool::CommandChangeTarget(CCommandVar p1, CCommandVar p2)
{
	if (Scene->GetTool(p1)->IsEnabled())
    {
	    SetTarget	(p1,p2);
	    ExecCommand	(COMMAND_UPDATE_PROPERTIES);
        return 		TRUE;
    }else{
    	return 		FALSE;
    }
}
CCommandVar CLevelTool::CommandShowObjectList(CCommandVar p1, CCommandVar p2)
{
    if (LUI->GetEState()==esEditScene) ShowObjectList();
    return TRUE;
}

// Main commands
CCommandVar CommandLibraryEditor(CCommandVar p1, CCommandVar p2)
{
   /* if (Scene->ObjCount()||(LUI->GetEState()!=esEditScene)){
        if (LUI->GetEState()==esEditLibrary)	TfrmEditLibrary::ShowEditor();
        else									ELog.DlgMsg(mtError, "Scene must be empty before editing library!");
    }else{
        TfrmEditLibrary::ShowEditor();
    }*/
    return TRUE;
}
CCommandVar CommandLAnimEditor(CCommandVar p1, CCommandVar p2)
{
    //TfrmEditLightAnim::ShowEditor();
    return TRUE;
}
CCommandVar CommandFileMenu(CCommandVar p1, CCommandVar p2)
{
    //FHelper.ShowPPMenu(fraLeftBar->pmSceneFile,0);
    return TRUE;
}
CCommandVar CLevelTool::CommandEnableTarget(CCommandVar p1, CCommandVar p2)
{
	ESceneToolBase* M 	= Scene->GetTool(p1);
    VERIFY					(M);
    BOOL res				= FALSE;
	if (p2)
    {
    	res 				= ExecCommand(COMMAND_LOAD_LEVEL_PART,M->FClassID,TRUE);
        if(res)
		    M->m_EditFlags.set(ESceneToolBase::flEnable,TRUE);
    }else
    {
        if (!Scene->IfModified())
        {
		    M->m_EditFlags.set(ESceneToolBase::flEnable,TRUE);
            res				= FALSE;
        }else
        {
	    	res				= ExecCommand(COMMAND_UNLOAD_LEVEL_PART,M->FClassID,TRUE);
            if(res)
		    	M->m_EditFlags.set(ESceneToolBase::flEnable,FALSE);
        }
		if (res)        	
        	ExecCommand(COMMAND_CHANGE_TARGET,OBJCLASS_SCENEOBJECT);
    }
    ExecCommand				(COMMAND_REFRESH_UI_BAR);
    return res;
}

CCommandVar CLevelTool::CommandShowTarget(CCommandVar p1, CCommandVar p2)
{
	ESceneToolBase* M 	= Scene->GetTool(p1);
    if(p2)
    	M->m_EditFlags.set(ESceneToolBase::flVisible,TRUE);
    else
    	M->m_EditFlags.set(ESceneToolBase::flVisible,FALSE);
        
    return TRUE;
}

CCommandVar CLevelTool::CommandReadonlyTarget(CCommandVar p1, CCommandVar p2)
{
	ESceneToolBase* M 		= Scene->GetTool(p1); VERIFY(M);
    BOOL res				= TRUE;
	if (p2){
        if (!Scene->IfModified())
        {
		    M->m_EditFlags.set(ESceneToolBase::flForceReadonly,FALSE);
            res				= FALSE;
        }else
        {
//.            xr_string pn	= Scene->LevelPartName(LTools->m_LastFileName.c_str(),M->ClassID);
        }
    }else
    {
//.        xr_string pn		= Scene->LevelPartName(LTools->m_LastFileName.c_str(), M->ClassID);
    }
    if (res)
    {
    	Reset				();
	    ExecCommand			(COMMAND_REFRESH_UI_BAR);
    }
    return res;
}

CCommandVar CLevelTool::CommandMultiRenameObjects(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() )
    {
        if (mrYes==ELog.DlgMsg(mtConfirmation, mbYes |mbNo, "Are you sure to rename selected objects?"))
        {
			int cnt			= Scene->MultiRenameObjects();
            if (cnt)
            {
			    ExecCommand	(COMMAND_UPDATE_PROPERTIES);
            	Scene->UndoSave();
            }
	        ELog.DlgMsg		( mtInformation, "%d - objects are renamed.", cnt );
        }
    }else
    {
        ELog.DlgMsg			( mtError, "Scene sharing violation" );
    }
    return 					FALSE;
}
CCommandVar CommandLoadLevelPart(CCommandVar p1, CCommandVar p2)
{
    xr_string temp_fn	= LTools->m_LastFileName.c_str();
    if (!temp_fn.empty())
        return			Scene->LoadLevelPart(temp_fn.c_str(),p1);
    return				TRUE;
}
CCommandVar CommandUnloadLevelPart(CCommandVar p1, CCommandVar p2)
{
    xr_string temp_fn	= LTools->m_LastFileName.c_str();
    if (!temp_fn.empty())
        return			Scene->UnloadLevelPart(temp_fn.c_str(),p1);
    return				TRUE;
}
CCommandVar CommandLoad(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() )
    {
    	if (!p1.IsString())
        {
        	xr_string temp_fn	= LTools->m_LastFileName.c_str();
        	if (EFS.GetOpenName	(EDevice->m_hWnd, _maps_, temp_fn ))
            	return 			ExecCommand(COMMAND_LOAD,temp_fn);
        }else
        {
	        xr_string temp_fn		= p1;
            xr_strlwr				(temp_fn);

            if (!Scene->IfModified())
            	return FALSE;
            
            UI->SetStatus			("Level loading...");
            ExecCommand				(COMMAND_CLEAR);

			IReader* R = FS.r_open	(temp_fn.c_str());
            if (!R)return false;
            char ch;
            R->r(&ch, sizeof(ch));
            bool is_ltx = (ch=='[');
            FS.r_close(R);
            bool res;
            LTools->m_LastFileName	= temp_fn.c_str();

            if(is_ltx)
            	res = Scene->LoadLTX(temp_fn.c_str(), false);
            else
            	res = Scene->Load(temp_fn.c_str(), false);

            if (res)
            {
                UI->ResetStatus		();
                Scene->UndoClear	();
                
                BOOL bk1 			= Scene->m_RTFlags.test(EScene::flRT_Unsaved);
                BOOL bk2 			= Scene->m_RTFlags.test(EScene::flRT_Modified);

                Scene->UndoSave		();

                 Scene->m_RTFlags.set(EScene::flRT_Unsaved,bk1);
                 Scene->m_RTFlags.set(EScene::flRT_Modified,bk2);

				ExecCommand			(COMMAND_CLEAN_LIBRARY);
                ExecCommand			(COMMAND_UPDATE_CAPTION);
                ExecCommand			(COMMAND_CHANGE_ACTION,etaSelect);
                EPrefs->AppendRecentFile(temp_fn.c_str());
            }else
            {
                ELog.DlgMsg	( mtError, "Can't load map '%s'", temp_fn.c_str() );
                LTools->m_LastFileName = "";
            }
            // update props
            ExecCommand			(COMMAND_UPDATE_PROPERTIES);
            UI->RedrawScene		();             
        }
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
        return FALSE;
    }
    return TRUE;
}
CCommandVar CommandSaveBackup(CCommandVar p1, CCommandVar p2)
{
    string_path 	fn;
    strconcat(sizeof(fn),fn,Core.CompName,"_",Core.UserName,"_backup.level");
    FS.update_path	(fn,_maps_,fn);
    return 			ExecCommand(COMMAND_SAVE,xr_string(fn));
}
CCommandVar CommandSave(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() )
    {
        if (p2==1)
        {
            xr_string temp_fn	= LTools->m_LastFileName.c_str();
            if (EFS.GetSaveName	( _maps_, temp_fn ))
                return 			ExecCommand(COMMAND_SAVE,temp_fn, 66);
            else
                return          FALSE;
        }else{
            if (p1.IsInteger())
            	return 			ExecCommand(COMMAND_SAVE,xr_string(LTools->m_LastFileName.c_str()),0);
                
            xr_string temp_fn	= xr_string(p1);
            if (temp_fn.empty())
            {
                return 			ExecCommand(COMMAND_SAVE,temp_fn,1);
            }
            else
            {
                xr_strlwr(temp_fn);

                UI->SetStatus("Level saving...");
                if (xrGameManager::GetGame() == EGame::SHOC)
                {
                    Scene->Save(temp_fn.c_str(), true, (p2 == 66));
                }
                else
                {
                    Scene->SaveLTX(temp_fn.c_str(), false, (p2 == 66));
                }
                UI->ResetStatus	();
                // set new name
                if (0!=xr_strcmp(Tools->m_LastFileName.c_str(),temp_fn.c_str()))
                {
    	            Tools->m_LastFileName 	= temp_fn.c_str();
                }
                ExecCommand		(COMMAND_UPDATE_CAPTION);
                EPrefs->AppendRecentFile(temp_fn.c_str());
                return 			TRUE;
            }
        }
    } else {
        ELog.DlgMsg			( mtError, "Scene sharing violation" );
        return				FALSE;
    }
}

CCommandVar CommandClear(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if (!Scene->IfModified()) return TRUE;
        EDevice->m_Camera.Reset	();
        Scene->Reset			();
        Scene->m_LevelOp.Reset	();
        Tools->m_LastFileName 		= "";
        LTools->m_LastSelectionName = "";
        Scene->UndoClear		();
        ExecCommand				(COMMAND_UPDATE_CAPTION);
        ExecCommand				(COMMAND_CHANGE_TARGET,OBJCLASS_SCENEOBJECT);
        ExecCommand				(COMMAND_CHANGE_ACTION,etaSelect,estDefault);
	    ExecCommand				(COMMAND_UPDATE_PROPERTIES,1);
        Scene->UndoSave			();
        return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
        return					FALSE;
    }
}
CCommandVar CommandLoadFirstRecent(CCommandVar p1, CCommandVar p2)
{
    if (EPrefs->FirstRecentFile())
        return 					ExecCommand(COMMAND_LOAD,xr_string(EPrefs->FirstRecentFile()));
    return 						FALSE;
}

CCommandVar CommandClearDebugDraw(CCommandVar p1, CCommandVar p2)
{
    Tools->ClearDebugDraw		();
    UI->RedrawScene				();
    return 						TRUE;
}
CCommandVar CommandShowClipEditor(CCommandVar p1, CCommandVar p2)
{
/*	if(g_clip_maker==NULL)
       g_clip_maker = TClipMaker::CreateForm();

    if(!g_clip_maker->Visible)	
    {
		ESceneCustomOTool* st = Scene->GetOTool(OBJCLASS_SPAWNPOINT);

    	ObjectList&	ol = st->GetObjects();
        ObjectList::iterator it = ol.begin();    
        ObjectList::iterator it_e = ol.end();    

		CCustomObject* CO = NULL;
        for(;it!=it_e;++it)
        {
        	if((*it)->Selected()==true)
            {
            	CO=*it;
                break;
            }
        }
        if(!CO)
        	return TRUE;
            
    	CSpawnPoint* sp = dynamic_cast<CSpawnPoint*>(CO);

        
    	CKinematicsAnimated* KA 	= PKinematicsAnimated(sp->m_SpawnData.m_Visual->visual);
        R_ASSERT					(KA);
   		g_clip_maker->ShowEditor	(KA);
    }*/
    return 							TRUE;
}

CCommandVar CommandImportCompilerError(CCommandVar p1, CCommandVar p2)
{
    xr_string fn;
    if(EFS.GetOpenName(EDevice->m_hWnd,"$logs$", fn, false, NULL, 0)){
        Scene->LoadCompilerError(fn.c_str());
    }
    UI->RedrawScene		();
    return TRUE;
}
CCommandVar CommandExportCompilerError(CCommandVar p1, CCommandVar p2)
{
    xr_string fn;
    if(EFS.GetSaveName("$logs$", fn, NULL, 0)){
        Scene->SaveCompilerError(fn.c_str());
    }
    return TRUE;
}
CCommandVar CommandValidateScene(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->Validate	(true,true,true,true,true,true);
	    return 			TRUE;
    } else {
        ELog.DlgMsg		( mtError, "Scene sharing violation" );
	    return 			FALSE;
    }
}
CCommandVar CommandCleanLibrary(CCommandVar p1, CCommandVar p2)
{
    if ( !Scene->locked() ){
        Lib.CleanLibrary();
        return 			TRUE;
    }else{
        ELog.DlgMsg		(mtError, "Scene must be empty before refreshing library!");
        return 			FALSE;
    }
}

CCommandVar CommandReloadObjects(CCommandVar p1, CCommandVar p2)
{
    Lib.ReloadObjects	();
    return 				TRUE;
}

CCommandVar CommandCut(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->CutSelection(LTools->CurrentClassID());
       /* fraLeftBar->miPaste->Enabled = true;
        fraLeftBar->miPaste2->Enabled = true;*/
        Scene->UndoSave	();
        return 			TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
        return 			FALSE;
    }
    return FALSE;
}
CCommandVar CommandCopy(CCommandVar p1, CCommandVar p2)
{
      if( !Scene->locked() ){
        Scene->CopySelection(LTools->CurrentClassID());
        return 			TRUE;
    } else {
        ELog.DlgMsg		( mtError, "Scene sharing violation" );
        return 			FALSE;
    }
    return FALSE;
}

CCommandVar CommandPaste(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->PasteSelection();
        Scene->UndoSave	();
        return 			TRUE;
    } else {
        ELog.DlgMsg		( mtError, "Scene sharing violation" );
        return  		FALSE;
    }
    return FALSE;
}

CCommandVar CommandLoadSelection(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() )
    {
        xr_string fn			= LTools->m_LastSelectionName;
        if( EFS.GetOpenName(EDevice->m_hWnd, _maps_, fn ) )
        {
        	LPCSTR maps_path	= FS.get_path(_maps_)->m_Path;
        	if (fn.c_str()==strstr(fn.c_str(),maps_path))
		        LTools->m_LastSelectionName = fn.c_str()+xr_strlen(maps_path);
            UI->SetStatus		("Fragment loading...");

            Scene->LoadSelection(fn.c_str());

            UI->ResetStatus		();
            Scene->UndoSave		();
            ExecCommand			(COMMAND_CHANGE_ACTION,etaSelect);
            ExecCommand			(COMMAND_UPDATE_PROPERTIES);
            UI->RedrawScene		();
	        return 				TRUE;
        }               	
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return FALSE;
}        
CCommandVar CommandSaveSelection(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        xr_string fn			= LTools->m_LastSelectionName;
        if( EFS.GetSaveName		( _maps_, fn ) ){
        	LPCSTR maps_path	= FS.get_path(_maps_)->m_Path;
        	if (fn.c_str()==strstr(fn.c_str(),maps_path))
		        LTools->m_LastSelectionName = fn.c_str()+xr_strlen(maps_path);
            UI->SetStatus		("Fragment saving...");
            Scene->SaveSelection(LTools->CurrentClassID(),fn.c_str());
            UI->ResetStatus		();
	        return 				TRUE;
        }
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}

CCommandVar CommandUndo(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if( !Scene->Undo() ) 	ELog.DlgMsg( mtInformation, "Undo buffer empty" );
        else{
            LTools->Reset		();
            ExecCommand			(COMMAND_CHANGE_ACTION, etaSelect);
	        return 				TRUE;
        }
    } else {
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}

CCommandVar CommandRedo(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if( !Scene->Redo() ) 	ELog.DlgMsg( mtInformation, "Redo buffer empty" );
        else{
            LTools->Reset		();
            ExecCommand			(COMMAND_CHANGE_ACTION, etaSelect);
		    return 				TRUE;
        }
    } else {
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}

CCommandVar CommandClearSceneSummary(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->ClearSummaryInfo	();
	    return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandCollectSceneSummary(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->CollectSummaryInfo();
	    return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandShowSceneSummary(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->ShowSummaryInfo();
	    return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandExportSceneSummary(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->ExportSummaryInfo(xr_string(p1).c_str());
	    return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}

CCommandVar CommandSceneHighlightTexture(CCommandVar p1, CCommandVar p2)
{
    /*if( !Scene->locked() ){
        LPCSTR new_val 		 	= 0;
		if (TfrmChoseItem::SelectItem(smTexture,new_val,1)){
	       	Scene->HighlightTexture(new_val,false,0,0,false);
		    return 				TRUE;
        }
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;*/
    return FALSE;
}

CCommandVar CommandOptions(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        ExecCommand				(COMMAND_SHOW_PROPERTIES, p1, p2);
	    return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}

CCommandVar CommandBuild(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if (mrYes==ELog.DlgMsg(mtConfirmation, mbYes |mbNo, "Are you sure to build level?"))
            return				Builder.Compile(false);
    }else{
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}
CCommandVar CommandMakeAIMap(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if (mrYes==ELog.DlgMsg(mtConfirmation, mbYes |mbNo, "Are you sure to export ai-map?"))
            return 				Builder.MakeAIMap( );
    }else{
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}
CCommandVar CommandMakeGame(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if (mrYes==ELog.DlgMsg(mtConfirmation, mbYes |mbNo, "Are you sure to export game?"))
            return				Builder.MakeGame( );
    }else{
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}
CCommandVar CommandMakeDetails(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if (mrYes==ELog.DlgMsg(mtConfirmation, mbYes |mbNo, "Are you sure to export details?"))
            return 				Builder.MakeDetails();
    }else{
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}
CCommandVar CommandMakeHOM(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if (mrYes==ELog.DlgMsg(mtConfirmation, mbYes |mbNo, "Are you sure to export HOM?"))
            return				Builder.MakeHOM();
    }else{
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}
CCommandVar CommandMakeSOM(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        if (mrYes==ELog.DlgMsg(mtConfirmation, mbYes |mbNo, "Are you sure to export Sound Occlusion Model?"))
            return				Builder.MakeSOM();
    }else{
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}
CCommandVar CommandInvertSelectionAll(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->InvertSelection	(LTools->CurrentClassID());
	    return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}

CCommandVar CommandSelectAll(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->SelectObjects	(true,LTools->CurrentClassID());
	    return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
    }
    return 						FALSE;
}

CCommandVar CommandDeselectAll(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->SelectObjects	(false,LTools->CurrentClassID());
	    return 					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}

CCommandVar CommandDeleteSelection(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->RemoveSelection	( LTools->CurrentClassID() );
        Scene->UndoSave			();
        return					TRUE;
    } else {
        ELog.DlgMsg( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}

CCommandVar CommandHideUnsel(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->ShowObjects		( false, LTools->CurrentClassID(), true, false );
        Scene->UndoSave			();
        ExecCommand				(COMMAND_UPDATE_PROPERTIES);
	    return 					TRUE;
    } else {
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandHideSel(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->ShowObjects		( bool(p1), LTools->CurrentClassID(), true, true );
        Scene->UndoSave			();
        ExecCommand				(COMMAND_UPDATE_PROPERTIES);
	    return 					TRUE;
    } else {
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandHideAll(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->ShowObjects		( bool(p1), LTools->CurrentClassID(), false );
        Scene->UndoSave			();
        ExecCommand				(COMMAND_UPDATE_PROPERTIES);
	    return 					TRUE;
    }else{
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandSetSnapObjects(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->SetSnapList		();
	    return 					TRUE;
    }else{
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandAddSelSnapObjects(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->AddSelToSnapList	();
	    return 					TRUE;
    }else{
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandDelSelSnapObjects(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->DelSelFromSnapList();
	    return 					TRUE;
    }else{
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandClearSnapObjects(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->ClearSnapList	(true);
	    return 					TRUE;
    }else{
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandSelectSnapObjects(CCommandVar p1, CCommandVar p2)
{
    if( !Scene->locked() ){
        Scene->SelectSnapList	();
	    return 					TRUE;
    }else{
        ELog.DlgMsg				( mtError, "Scene sharing violation" );
	    return 					FALSE;
    }
}
CCommandVar CommandRefreshSnapObjects(CCommandVar p1, CCommandVar p2)
{
 //   fraLeftBar->UpdateSnapList();
    return 						TRUE;
}
/*
CCommandVar CommandRefreshSoundEnvs(CCommandVar p1, CCommandVar p2)
{
    ::Sound->refresh_env_library();
    return 						TRUE;
//		::Sound->_restart();
}
*/

CCommandVar CommandRefreshSoundEnvGeometry(CCommandVar p1, CCommandVar p2)
{
    LSndLib->RefreshEnvGeometry();
    return 						TRUE;
}
CCommandVar CommandShowContextMenu(CCommandVar p1, CCommandVar p2)
{
    LUI->ShowContextMenu		(p1);
    return 						TRUE;
}
//------        
CCommandVar CommandRefreshUIBar(CCommandVar p1, CCommandVar p2)
{
    if(MainForm)
        if(MainForm->GetTopBarForm())
            MainForm->GetTopBarForm()->RefreshBar();
    /*fraTopBar->RefreshBar		();
    fraLeftBar->RefreshBar		();
    fraBottomBar->RefreshBar	();*/
    return 						TRUE;
}
CCommandVar CommandRestoreUIBar(CCommandVar p1, CCommandVar p2)
{
    /*fraTopBar->fsStorage->RestoreFormPlacement();
    fraLeftBar->fsStorage->RestoreFormPlacement();
    fraBottomBar->fsStorage->RestoreFormPlacement();*/
    return 						TRUE;
}
CCommandVar CommandSaveUIBar(CCommandVar p1, CCommandVar p2)
{
   /* fraTopBar->fsStorage->SaveFormPlacement();
    fraLeftBar->fsStorage->SaveFormPlacement();
    fraBottomBar->fsStorage->SaveFormPlacement();*/
    return 						TRUE;
}
CCommandVar CommandUpdateToolBar(CCommandVar p1, CCommandVar p2)
{
 /*   fraLeftBar->UpdateBar		();*/
    return 						TRUE;
}
CCommandVar CommandUpdateCaption(CCommandVar p1, CCommandVar p2)
{
  /*  frmMain->UpdateCaption		();*/
    return 						TRUE;
}
//------
CCommandVar CommandCreateSoundLib(CCommandVar p1, CCommandVar p2)
{
    SndLib						= xr_new<CLevelSoundManager>();
    LSndLib = (CLevelSoundManager*)SndLib;
    return 						TRUE;
}

extern BOOL ai_map_shown;
CCommandVar CommandToggleAiMapVisibility(CCommandVar p1, CCommandVar p2)
{
	ai_map_shown 				= !ai_map_shown;
    return 						TRUE;
}

// UE-style gizmo hotkeys: Q/W/E/R pick the mode, Space (EType::Count sentinel)
// cycles Move -> Rotate -> Scale. Unsupported modes are auto-reset by the toolbar.
CCommandVar CommandGizmoMode(CCommandVar p1, CCommandVar p2)
{
    LTools->SetAction			(etaSelect);
    Gizmo* G					= LTools->GetGimzo();
    const Gizmo::EType mode		= (Gizmo::EType)u32(p1);
    if (mode == Gizmo::EType::Count){
        switch (G->GetType()){
        case Gizmo::EType::Move:	G->SetType(Gizmo::EType::Rotate);	break;
        case Gizmo::EType::Rotate:	G->SetType(Gizmo::EType::Scale);	break;
        default:					G->SetType(Gizmo::EType::Move);		break;
        }
    }
    else							G->SetType(mode);
    UI->RedrawScene				();
    return 						TRUE;
}

// Places a library object into the scene — mirrors TUI_ControlObjectAdd::Start,
// but takes the asset name from the content browser instead of the tool panel.
// p2==0 -> raycast down the view centre, p2==1 -> raycast under the mouse.
CCommandVar CommandCBPlaceAsset(CCommandVar p1, CCommandVar p2)
{
    // hold the string: xr_string(p1) is a temporary, its buffer dies at the
    // end of this statement and the name would dangle for the rest of the call
    const xr_string asset_name = p1;
    LPCSTR name = asset_name.c_str();
    if (!name[0]) return FALSE;

    Fvector start, dir;
    if (u32(p2))
    {
        start.set(UI->m_CurrentRStart);
        dir.set  (UI->m_CurrentRDir);
    }
    else
    {
        // centre of the viewport
        Ivector2 c; c.set(UI->GetRenderWidth()/2, UI->GetRenderHeight()/2);
        EDevice->m_Camera.MouseRayFromPoint(start, dir, c);
    }

    Fvector p, n;
    if (!LUI->PickGround(p, start, dir, 1, &n))
    {
        // nothing under the ray — drop it a few metres ahead of the camera
        p.mad(EDevice->m_Camera.GetPosition(), dir, 10.f);
        n.set(0.f, 1.f, 0.f);
    }

    string256 namebuffer;
    Scene->GenObjectName(OBJCLASS_SCENEOBJECT, namebuffer, name);
    CSceneObject* obj = xr_new<CSceneObject>((LPVOID)0, namebuffer);
    if (!obj->SetReference(name))
    {
        ELog.DlgMsg(mtError, "Content Browser: can't load reference object '%s'.", name);
        xr_delete(obj);
        return FALSE;
    }
    obj->MoveTo(p, n);

    if (LTools->CurrentClassID() != OBJCLASS_SCENEOBJECT)
        LTools->SetTarget(OBJCLASS_SCENEOBJECT, 0);

    Scene->SelectObjects(false, OBJCLASS_SCENEOBJECT);
    Scene->AppendObject (obj);
    UI->RedrawScene     ();
    return TRUE;
}

CCommandVar CommandShowContentBrowser(CCommandVar p1, CCommandVar p2)
{
    if (UIContentBrowser::IsOpen())	UIContentBrowser::Close();
    else							UIContentBrowser::Show();
    return TRUE;
}

CCommandVar CommandShowWorldOutliner(CCommandVar p1, CCommandVar p2)
{
    if (UIWorldOutliner::IsOpen())	UIWorldOutliner::Close();
    else							UIWorldOutliner::Show();
    return TRUE;
}

//------------------------------------------------------------------------------
// drop to ground (the End key, like Unreal)
//------------------------------------------------------------------------------
void RetrieveSceneObjPointAndNormal(Fvector& hitpoint, Fvector* hitnormal, const SRayPickInfo& pinf, int bSnap);

// How far down a sweep looks for support.
static const float kDropReach = 1000.f;

// Clips a triangle to an XZ rectangle (Sutherland-Hodgman) keeping the height
// of every produced point. Y is linear across a triangle, so the tallest point
// of the clipped polygon is the exact highest support inside the footprint -
// this is what makes a wide crate rest on the rim of a narrow hole instead of
// slipping through it.
static u32 ClipTriangleToRect(const Fvector* tri, float x0, float x1, float z0, float z1, Fvector* out)
{
    Fvector buf_a[16], buf_b[16];
    Fvector* src = buf_a;
    Fvector* dst = buf_b;
    u32 count = 3;
    src[0] = tri[0]; src[1] = tri[1]; src[2] = tri[2];

    // plane order: x>=x0, x<=x1, z>=z0, z<=z1
    for (int plane = 0; plane < 4 && count; ++plane)
    {
        u32 produced = 0;
        for (u32 i = 0; i < count; ++i)
        {
            const Fvector& cur = src[i];
            const Fvector& nxt = src[(i + 1) % count];
            float cur_d, nxt_d;
            switch (plane)
            {
            case 0: cur_d = cur.x - x0; nxt_d = nxt.x - x0; break;
            case 1: cur_d = x1 - cur.x; nxt_d = x1 - nxt.x; break;
            case 2: cur_d = cur.z - z0; nxt_d = nxt.z - z0; break;
            default: cur_d = z1 - cur.z; nxt_d = z1 - nxt.z; break;
            }
            const bool cur_in = cur_d >= 0.f;
            const bool nxt_in = nxt_d >= 0.f;
            if (cur_in && produced < 15)
                dst[produced++] = cur;
            if (cur_in != nxt_in && produced < 15)
            {
                const float t = cur_d / (cur_d - nxt_d);
                Fvector cross;
                cross.lerp(cur, nxt, t);
                dst[produced++] = cross;
            }
        }
        count = produced;
        std::swap(src, dst);
    }

    for (u32 i = 0; i < count; ++i)
        out[i] = src[i];
    return count;
}

// Settles objects onto the surface below them exactly the way Unreal's End key
// does: UE calls FindSuitableTransformAlongPath with FCollisionShape::MakeBox
// of the element bounds, i.e. it sweeps the whole bounding box straight down
// (EditorServer.cpp, UEditorEngine::SnapElementTo, bUseLineTrace=false).
// Here the same sweep is solved analytically - every triangle in the swept
// column is clipped to the footprint and the tallest clipped point wins - so
// the result is exact rather than sampled.
// line_trace mirrors UE's Alt+End variant: a single ray from the pivot.
u32 DropObjectsToGround(ObjectList& objects, bool align_normal, bool line_trace)
{
    if (objects.empty() || !Scene->GetOTool(OBJCLASS_SCENEOBJECT))
        return 0;

    ObjectList candidates;
    ObjectList& all = Scene->ListObj(OBJCLASS_SCENEOBJECT);
    for (ObjectIt it = all.begin(); it != all.end(); ++it)
    {
        if (!(*it)->Visible()) continue;
        if (std::find(objects.begin(), objects.end(), *it) != objects.end()) continue;
        candidates.push_back(*it);
    }
    if (candidates.empty())
        return 0;

    u32 moved = 0;
    for (ObjectIt it = objects.begin(); it != objects.end(); ++it)
    {
        CCustomObject* obj = *it;
        Fbox box;
        if (!obj->GetBox(box)) continue;

        // a vertical sweep can only ever touch objects overlapping in XZ
        ObjectList floor;
        for (ObjectIt f = candidates.begin(); f != candidates.end(); ++f)
        {
            Fbox fb;
            if (!(*f)->GetBox(fb)) continue;
            if (fb.max.x < box.min.x - EPS_L || fb.min.x > box.max.x + EPS_L) continue;
            if (fb.max.z < box.min.z - EPS_L || fb.min.z > box.max.z + EPS_L) continue;
            if (fb.min.y > box.max.y) continue;	// entirely above: cannot support
            floor.push_back(*f);
        }
        if (floor.empty()) continue;

        float drop = flt_max;
        Fvector contact_normal;
        contact_normal.set(0.f, 1.f, 0.f);

        if (line_trace)
        {
            Fvector start = obj->GetPosition();
            start.y += EPS_L;
            Fvector down; down.set(0.f, -1.f, 0.f);
            SRayPickInfo pinf;
            pinf.inf.range = kDropReach;
            if (Scene->RayPickObject(pinf.inf.range, start, down, OBJCLASS_SCENEOBJECT, &pinf, &floor))
            {
                Fvector hit, normal;
                normal.set(0.f, 1.f, 0.f);
                RetrieveSceneObjPointAndNormal(hit, &normal, pinf, 0);
                if (_valid(hit))
                {
                    drop = obj->GetPosition().y - hit.y;
                    if (_valid(normal)) contact_normal = normal;
                }
            }
        }
        else
        {
            // the volume the box bottom passes through on its way down
            Fbox sweep;
            sweep.set(box.min.x, box.min.y - kDropReach, box.min.z,
                      box.max.x, box.min.y + EPS_L,     box.max.z);

            SPickQuery PQ;
            if (Scene->BoxQuery(PQ, sweep, CDB::OPT_FULL_TEST, &floor))
            {
                float best_y = -flt_max;
                SPickQuery::SResult* results = PQ.r_begin();
                for (int r = 0; r < PQ.r_count(); ++r)
                {
                    Fvector clipped[16];
                    const u32 n = ClipTriangleToRect(results[r].verts, box.min.x, box.max.x,
                        box.min.z, box.max.z, clipped);
                    for (u32 c = 0; c < n; ++c)
                    {
                        // only surfaces at or below the object can support it
                        if (clipped[c].y > box.min.y + EPS_L) continue;
                        if (clipped[c].y > best_y)
                        {
                            best_y = clipped[c].y;
                            Fvector e1, e2, n_tri;
                            e1.sub(results[r].verts[1], results[r].verts[0]);
                            e2.sub(results[r].verts[2], results[r].verts[0]);
                            n_tri.crossproduct(e1, e2);
                            if (n_tri.magnitude() > EPS_S)
                            {
                                n_tri.normalize();
                                if (n_tri.y < 0.f) n_tri.invert();
                                contact_normal = n_tri;
                            }
                        }
                    }
                }
                if (best_y > -flt_max)
                    drop = box.min.y - best_y;
            }
        }

        if (drop == flt_max || drop <= EPS_L)
            continue;	// nothing underneath, or already resting on it

        Fvector pos = obj->GetPosition();
        pos.y -= drop;
        if (align_normal)
            obj->MoveTo(pos, contact_normal);
        else
            obj->SetPosition(pos);
        ++moved;
    }
    return moved;
}

// collects the current selection across every object tool
static void CollectSelectedObjects(ObjectList& out)
{
    for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
    {
        if (it->first == OBJCLASS_DUMMY || !Scene->GetOTool(it->first)) continue;
        ObjectList& lst = Scene->ListObj(it->first);
        for (ObjectIt o = lst.begin(); o != lst.end(); ++o)
            if ((*o)->Selected())
                out.push_back(*o);
    }
}

CCommandVar CommandDropToGround(CCommandVar p1, CCommandVar p2)
{
    if (Scene->locked()) return FALSE;

    ObjectList selection;
    CollectSelectedObjects(selection);
    if (selection.empty())
    {
        ELog.Msg(mtInformation, "Drop to ground: nothing selected.");
        return FALSE;
    }

    // p2 != 0 selects the pivot line trace, mirroring UE's Alt+End
    const u32 moved = DropObjectsToGround(selection, false, 0 != u32(p2));
    if (moved)
    {
        Scene->UndoSave();
        ExecCommand(COMMAND_UPDATE_PROPERTIES);
        UI->RedrawScene(true);
    }
    ELog.Msg(mtInformation, "Drop to ground: %d of %d object(s) landed.", moved, selection.size());
    return moved ? TRUE : FALSE;
}

//------------------------------------------------------------------------------
// MCP inspector: scene / selection / per-object info for AI agents
//------------------------------------------------------------------------------
static void AppendObjectJson(xr_string& out, CCustomObject* obj, LPCSTR cls)
{
    char tmp[1024];
    const Fvector& p = obj->GetPosition();
    const Fvector& r = obj->GetRotation();
    const Fvector& s = obj->GetScale();
    sprintf_s(tmp,
        "{\"name\":\"%s\",\"class\":\"%s\",\"selected\":%s,\"visible\":%s,"
        "\"position\":[%.3f,%.3f,%.3f],\"rotation\":[%.4f,%.4f,%.4f],\"scale\":[%.3f,%.3f,%.3f]}",
        obj->GetName(), cls,
        obj->Selected() ? "true" : "false",
        obj->Visible() ? "true" : "false",
        p.x, p.y, p.z, r.x, r.y, r.z, s.x, s.y, s.z);
    out += tmp;
}

// bare (unquoted) JSON values: {"select":true} / {"limit":50}
static bool GetArgBool(LPCSTR raw, LPCSTR field, bool def)
{
    char pat[64];
    sprintf_s(pat, "\"%s\"", field);
    const char* k = raw ? strstr(raw, pat) : 0;
    if (!k) return def;
    const char* c = strchr(k + xr_strlen(pat), ':');
    if (!c) return def;
    // quotes are stepped over so {"open":"1"} reads the same as {"open":1}
    while (*++c == ' ' || *c == '"') {}
    return 0 == strncmp(c, "true", 4) || *c == '1';
}

static int GetArgInt(LPCSTR raw, LPCSTR field, int def)
{
    char pat[64];
    sprintf_s(pat, "\"%s\"", field);
    const char* k = raw ? strstr(raw, pat) : 0;
    if (!k) return def;
    const char* c = strchr(k + xr_strlen(pat), ':');
    if (!c) return def;
    const int v = atoi(c + 1);
    return v > 0 ? v : def;
}

// bare float: {"x":-12.5}; leaves `out` untouched when the field is absent
static bool GetArgFloat(LPCSTR raw, LPCSTR field, float& out)
{
    char pat[64];
    sprintf_s(pat, "\"%s\"", field);
    const char* k = raw ? strstr(raw, pat) : 0;
    if (!k) return false;
    const char* c = strchr(k + xr_strlen(pat), ':');
    if (!c) return false;
    out = (float)atof(c + 1);
    return true;
}

// asset and file names carry backslashes, which JSON needs doubled
static xr_string JsonEscapePath(LPCSTR text)
{
    xr_string out;
    for (LPCSTR c = text ? text : ""; *c; ++c)
    {
        if (*c == '\\' || *c == '"') out += '\\';
        if (u8(*c) < 0x20) continue;
        out += *c;
    }
    return out;
}

// Content-browser source argument: "project" / "editor" / "darf", or 0 / 1 / 2.
// The value is read straight off the request line because XFinedMCP::GetArg only
// ever returns QUOTED values - a bare {"source":2} would make it latch onto the
// next field's quote instead. Returns false only when a value is present but
// unrecognised; an absent argument leaves the browser on its current tab.
static bool ParseBrowserSource(LPCSTR raw, int& src)
{
    src = -1;
    static const char pat[] = "\"source\"";
    const char* k = raw ? strstr(raw, pat) : 0;
    if (!k) return true;
    const char* c = strchr(k + (sizeof(pat) - 1), ':');
    if (!c) return true;
    while (*++c == ' ') {}

    char val[64] = {};
    if (*c == '"')
    {
        const char* e = strchr(c + 1, '"');
        if (!e) return false;
        u32 n = u32(e - c - 1);
        if (n >= sizeof(val)) n = sizeof(val) - 1;
        CopyMemory(val, c + 1, n);
    }
    else
    {
        u32 n = 0;
        while (n + 1 < sizeof(val) && c[n] && c[n] != ',' && c[n] != '}' && c[n] != ' ')
        {
            val[n] = c[n];
            ++n;
        }
    }
    if (!val[0])						return true;
    if (0 == _stricmp(val, "project"))	{ src = 0; return true; }
    if (0 == _stricmp(val, "editor"))	{ src = 1; return true; }
    if (0 == _stricmp(val, "darf"))		{ src = 2; return true; }
    if (val[0] == '-' || (val[0] >= '0' && val[0] <= '9')) { src = atoi(val); return true; }
    return false;
}

static LPCSTR BrowserSourceName(int src)
{
    switch (src)
    {
    case 0:		return "project";
    case 1:		return "editor";
    case 2:		return "darf";
    default:	return "";
    }
}

// object names are lowercased on SetName - make lookups case-insensitive
static CCustomObject* FindObjectRelaxed(LPCSTR name)
{
    char low[256];
    strncpy_s(low, sizeof(low), name ? name : "", _TRUNCATE);
    strlwr(low);
    return Scene->FindObjectByName(low, (CCustomObject*)0);
}

static LPCSTR ResolveToolClassName(ObjClassID cls)
{
    for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
        if (it->second && it->first == cls) return it->second->ClassName();
    return "unknown";
}

static void AppendCameraJson(xr_string& out)
{
    char tmp[256];
    const Fvector& p = EDevice->m_Camera.GetPosition();
    const Fvector& r = EDevice->m_Camera.GetHPB();
    const Fvector& d = EDevice->m_Camera.GetDirection();
    sprintf_s(tmp,
        "\"camera\":{\"position\":[%.3f,%.3f,%.3f],\"hpb\":[%.4f,%.4f,%.4f],\"direction\":[%.3f,%.3f,%.3f]}",
        p.x, p.y, p.z, r.x, r.y, r.z, d.x, d.y, d.z);
    out += tmp;
}

// full single-object response shared by the object mutation commands
static void RespondObjectJson(xr_string& out, CCustomObject* obj)
{
    out = "{\"ok\":true,\"object\":";
    AppendObjectJson(out, obj, ResolveToolClassName(obj->FClassID));
    out += "}";
}

bool XFinedInspector(LPCSTR cmd, LPCSTR raw, xr_string& out)
{
    // XMS mod commands are project-scoped and do not need an open scene
    if (0 == xr_strcmp(cmd, "mod_manifest"))		{ EditorMod::McpManifest(out);			return true; }
    if (0 == xr_strcmp(cmd, "mod_set_manifest"))	{ EditorMod::McpSetManifest(raw, out);	return true; }
    if (0 == xr_strcmp(cmd, "mod_export"))			{ EditorMod::McpExport(raw, out);		return true; }
    // level overlays check the scene/project themselves and report json errors
    // opens the model preview window - the same thing a double click in the
    // content browser does, reachable without driving the mouse
    if (0 == xr_strcmp(cmd, "preview_model"))
    {
        char name[512] = {}, source[64] = {};
        XFinedMCP::GetArg(raw, "name", name, sizeof(name));
        XFinedMCP::GetArg(raw, "source", source, sizeof(source));
        for (char* p = name; *p; ++p) if (*p == '/') *p = '\\';
        {
            char* w = name;
            for (const char* r = name; *r; ++r)
                if (!(*r == '\\' && w > name && w[-1] == '\\')) *w++ = *r;
            *w = 0;
        }

        if (0 == _stricmp(source, "darf"))
        {
            xr_string mount_err;
            if (!EditorGameContent::EnsureMounted(mount_err))
            {
                out = "{\"ok\":false,\"error\":\"";
                out += mount_err;
                out += "\"}";
                return true;
            }
            const int idx = EditorGameContent::Find(name);
            u32 sz = 0;
            u8* bytes = (idx >= 0) ? EditorGameContent::ReadBytes(idx, sz) : 0;
            if (!bytes || !sz)
            {
                EditorGameContent::FreeBytes(bytes);
                out = "{\"ok\":false,\"error\":\"not found in the linked game install\"}";
                return true;
            }
            UIVisualPreview::ShowFromMemory(name, bytes, sz);
            EditorGameContent::FreeBytes(bytes);
        }
        else
            UIVisualPreview::Show(name[0] ? name : 0);

        out = "{\"ok\":true,\"open\":";
        out += UIVisualPreview::IsOpen() ? "true" : "false";
        out += "}";
        return true;
    }

    // layout maintenance: the arrangement lives in level_imgui.ini, which is not
    // always recoverable by hand once it is lost
    if (0 == xr_strcmp(cmd, "reset_layout"))
    {
        UI->RequestDockLayoutReset();
        out = "{\"ok\":true,\"note\":\"default arrangement rebuilt on the next frame\"}";
        return true;
    }
    if (0 == xr_strcmp(cmd, "mod_export_spawn"))	{ EditorModScene::McpExportSpawn(raw, out);	return true; }
    if (0 == xr_strcmp(cmd, "mod_export_cut"))		{ EditorModScene::McpExportCut(raw, out);	return true; }
    if (0 == xr_strcmp(cmd, "mod_export_xcform"))	{ EditorModScene::McpExportXCForm(raw, out);	return true; }
    if (0 == xr_strcmp(cmd, "mod_export_ogf_probe")){ EditorModScene::McpExportOGF(raw, out);		return true; }

    // project <-> game link and the read-only DARF content view: project
    // scoped, no scene required
    if (0 == xr_strcmp(cmd, "game_link"))			{ EditorProject::McpGameLink(raw, out);			return true; }
    if (0 == xr_strcmp(cmd, "darf_list"))			{ EditorGameContent::McpList(raw, out);			return true; }
    if (0 == xr_strcmp(cmd, "darf_copy"))			{ EditorGameContent::McpCopy(raw, out);			return true; }

    // file management: writes and deletes are clamped to the project folder
    // inside EditorFileOps, which is where the path normalisation lives
    if (0 == xr_strcmp(cmd, "content_copy"))		{ EditorFileOps::McpCopy(raw, out);				return true; }
    if (0 == xr_strcmp(cmd, "content_move"))		{ EditorFileOps::McpMove(raw, out);				return true; }
    if (0 == xr_strcmp(cmd, "content_delete"))		{ EditorFileOps::McpDelete(raw, out);			return true; }
    if (0 == xr_strcmp(cmd, "content_mkdir"))		{ EditorFileOps::McpMakeDir(raw, out);			return true; }

    // reveal an asset in the content browser: the panel-driving twin of
    // asset_preview, which only ever renders offscreen
    if (0 == xr_strcmp(cmd, "content_browser_open"))
    {
        char name[512] = {};
        if (!XFinedMCP::GetArg(raw, "name", name, sizeof(name)) || !name[0])
        {
            out = "{\"ok\":false,\"error\":\"missing 'name' argument (see list_assets / darf_list)\"}";
            return true;
        }
        for (char* p = name; *p; ++p) if (*p == '/') *p = '\\';
        // GetArg hands over the raw JSON value, so an escaped "a\\b" arrives with
        // both slashes - the browser compares names exactly
        {
            char* w = name;
            for (const char* r = name; *r; ++r)
                if (!(*r == '\\' && w > name && w[-1] == '\\')) *w++ = *r;
            *w = 0;
        }

        int src = -1;
        if (!ParseBrowserSource(raw, src))
        {
            out = "{\"ok\":false,\"error\":\"source must be project, editor, darf or 0/1/2\"}";
            return true;
        }

        xr_string err;
        if (!UIContentBrowser::RevealAsset(name, src, GetArgBool(raw, "open", false), err))
        {
            out = "{\"ok\":false,\"error\":\"";
            out += JsonEscapePath(err.c_str());
            out += "\"}";
            return true;
        }

        // report where it actually landed - RevealAsset may have switched source
        int cur = -1;
        xr_string folder;
        xr_vector<xr_string> sel;
        UIContentBrowser::GetSelection(cur, folder, sel);
        char head[128];
        sprintf_s(head, sizeof(head), "{\"ok\":true,\"source\":%d,\"source_name\":\"%s\",\"folder\":\"",
            cur, BrowserSourceName(cur));
        out = head;
        out += JsonEscapePath(folder.c_str());
        out += "\",\"name\":\"";
        out += JsonEscapePath(name);
        out += "\"}";
        return true;
    }

    // pull assets out of a read-only source into the project: the scriptable
    // twin of "Copy to project" / "Copy folder to project" in the browser menu
    if (0 == xr_strcmp(cmd, "content_browser_copy"))
    {
        char names[4096] = {}, folder[512] = {}, dst[512] = {}, cat[64] = {};
        XFinedMCP::GetArg(raw, "names",  names,  sizeof(names));
        XFinedMCP::GetArg(raw, "folder", folder, sizeof(folder));
        XFinedMCP::GetArg(raw, "dst",    dst,    sizeof(dst));
        XFinedMCP::GetArg(raw, "category", cat,  sizeof(cat));
        // GetArg hands over the raw JSON value, so an escaped "a\\b" arrives
        // with both slashes - the browser compares names exactly
        for (char* buf : { names, folder, dst })
        {
            for (char* p = buf; *p; ++p) if (*p == '/') *p = '\\';
            char* w = buf;
            for (const char* r = buf; *r; ++r)
                if (!(*r == '\\' && w > buf && w[-1] == '\\')) *w++ = *r;
            *w = 0;
        }

        int src = -1;
        if (!ParseBrowserSource(raw, src))
        {
            out = "{\"ok\":false,\"error\":\"source must be project, editor, darf or 0/1/2\"}";
            return true;
        }

        int category = -1;
        if (cat[0])
        {
            category = UIContentBrowser::FindCategory(cat);
            if (category < 0)
            {
                out = "{\"ok\":false,\"error\":\"unknown category (see list_assets)\"}";
                return true;
            }
        }

        int files = 0;
        xr_string err;
        const bool ok = UIContentBrowser::McpCopyToProject(
            names, folder, dst, src, category, GetArgBool(raw, "overwrite", true), files, err);

        char head[96];
        sprintf_s(head, sizeof(head), "{\"ok\":%s,\"files\":%d", ok ? "true" : "false", files);
        out = head;
        if (!err.empty())
        {
            out += ",\"error\":\"";
            out += JsonEscapePath(err.c_str());
            out += "\"";
        }
        out += "}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "content_browser_selection"))
    {
        int src = -1;
        xr_string folder;
        xr_vector<xr_string> sel;
        UIContentBrowser::GetSelection(src, folder, sel);

        char head[192];
        sprintf_s(head, sizeof(head), "{\"ok\":true,\"open\":%s,\"source\":%d,\"source_name\":\"%s\",\"folder\":\"",
            UIContentBrowser::IsOpen() ? "true" : "false", src, BrowserSourceName(src));
        out = head;
        out += JsonEscapePath(folder.c_str());
        out += "\",\"count\":";
        char tail[32];
        sprintf_s(tail, sizeof(tail), "%u,\"selection\":[", u32(sel.size()));
        out += tail;
        for (u32 i = 0; i < sel.size(); ++i)
        {
            if (i) out += ",";
            out += "\"";
            out += JsonEscapePath(sel[i].c_str());
            out += "\"";
        }
        out += "]}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "camera_get"))
    {
        out = "{\"ok\":true,";
        AppendCameraJson(out);
        out += "}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "camera_set"))
    {
        // absent fields keep their current value, so partial updates work
        Fvector pos = EDevice->m_Camera.GetPosition();
        Fvector hpb = EDevice->m_Camera.GetHPB();
        bool any = false;
        any |= GetArgFloat(raw, "x", pos.x);
        any |= GetArgFloat(raw, "y", pos.y);
        any |= GetArgFloat(raw, "z", pos.z);
        any |= GetArgFloat(raw, "h", hpb.x);
        any |= GetArgFloat(raw, "p", hpb.y);
        any |= GetArgFloat(raw, "b", hpb.z);
        if (!any)
        {
            out = "{\"ok\":false,\"error\":\"pass at least one of x,y,z,h,p,b\"}";
            return true;
        }
        EDevice->m_Camera.Set(hpb, pos);
        UI->RedrawScene(true);
        out = "{\"ok\":true,";
        AppendCameraJson(out);
        out += "}";
        return true;
    }

    // the outliner panel lives outside the scene, so it toggles without one
    if (0 == xr_strcmp(cmd, "outliner_show"))
    {
        if (GetArgBool(raw, "open", true))	UIWorldOutliner::Show();
        else								UIWorldOutliner::Close();
        out = UIWorldOutliner::IsOpen() ? "{\"ok\":true,\"open\":true}" : "{\"ok\":true,\"open\":false}";
        return true;
    }

    if (!Scene) return false;

    // AI-facing twin of the World Outliner panel: one node per object tool
    if (0 == xr_strcmp(cmd, "scene_tree"))
    {
        char cls_filter[128] = {}, contains[128] = {};
        XFinedMCP::GetArg(raw, "class", cls_filter, sizeof(cls_filter));
        XFinedMCP::GetArg(raw, "filter", contains, sizeof(contains));
        const int limit = GetArgInt(raw, "limit", 200);
        strlwr(contains);

        // same gate and ordering as the panel: object tools only, by class name
        xr_vector<ESceneToolBase*> tools;
        for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
        {
            if (!it->second || it->first == OBJCLASS_DUMMY || !Scene->GetOTool(it->first)) continue;
            if (cls_filter[0] && 0 != _stricmp(it->second->ClassName(), cls_filter)) continue;
            if (Scene->ListObj(it->first).empty()) continue;
            tools.push_back(it->second);
        }
        std::sort(tools.begin(), tools.end(), [](ESceneToolBase* a, ESceneToolBase* b)
        {
            return xr_strcmp(a->ClassName(), b->ClassName()) < 0;
        });

        out = "{\"ok\":true,\"tree\":[";
        for (u32 g = 0; g < tools.size(); ++g)
        {
            ObjectList& lst = Scene->ListObj(tools[g]->FClassID);

            xr_string body;
            int matched = 0, emitted = 0;
            for (ObjectIt o = lst.begin(); o != lst.end(); ++o)
            {
                char low[256];
                strncpy_s(low, sizeof(low), (*o)->GetName(), _TRUNCATE);
                strlwr(low);
                if (contains[0] && !strstr(low, contains)) continue;
                ++matched;
                if (emitted >= limit) continue;
                char tmp[512];
                sprintf_s(tmp, "%s{\"name\":\"%s\",\"selected\":%s,\"visible\":%s}",
                    emitted ? "," : "", (*o)->GetName(),
                    (*o)->Selected() ? "true" : "false",
                    (*o)->Visible() ? "true" : "false");
                body += tmp;
                ++emitted;
            }
            if (!matched) continue;

            char head[256];
            sprintf_s(head, "%s{\"class\":\"%s\",\"count\":%d,\"total\":%d,\"objects\":[",
                (out[out.size() - 1] == '[') ? "" : ",", tools[g]->ClassName(), matched, int(lst.size()));
            out += head;
            out += body;
            out += "]}";
        }
        out += "]}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "list_objects"))
    {
        char cls_filter[128] = {}, contains[128] = {};
        XFinedMCP::GetArg(raw, "class", cls_filter, sizeof(cls_filter));
        XFinedMCP::GetArg(raw, "name_contains", contains, sizeof(contains));
        const int limit = GetArgInt(raw, "limit", 100);

        out = "{\"ok\":true,\"objects\":[";
        int emitted = 0, total = 0;
        for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
        {
            ESceneToolBase* t = it->second;
            if (!t || it->first == OBJCLASS_DUMMY || !Scene->GetOTool(it->first)) continue;
            if (cls_filter[0] && 0 != _stricmp(t->ClassName(), cls_filter)) continue;
            ObjectList& lst = Scene->ListObj(it->first);
            for (ObjectIt o = lst.begin(); o != lst.end(); ++o)
            {
                if (contains[0] && !strstr((*o)->GetName(), contains)) continue;
                ++total;
                if (emitted >= limit) continue;
                char tmp[512];
                sprintf_s(tmp, "%s{\"name\":\"%s\",\"class\":\"%s\"}",
                    emitted ? "," : "", (*o)->GetName(), t->ClassName());
                out += tmp;
                ++emitted;
            }
        }
        char tail[64];
        sprintf_s(tail, "],\"shown\":%d,\"total\":%d}", emitted, total);
        out += tail;
        return true;
    }

    if (0 == xr_strcmp(cmd, "focus_object"))
    {
        char name[256];
        if (!XFinedMCP::GetArg(raw, "name", name, sizeof(name)))
        {
            out = "{\"ok\":false,\"error\":\"missing 'name' argument\"}";
            return true;
        }
        CCustomObject* obj = FindObjectRelaxed(name);
        if (!obj)
        {
            out = "{\"ok\":false,\"error\":\"object not found\"}";
            return true;
        }

        if (GetArgBool(raw, "select", false))
        {
            // behave exactly like a manual pick: clear, select, switch target
            Scene->SelectObjects(false, OBJCLASS_DUMMY);
            obj->Select(TRUE);
            if (LTools->CurrentClassID() != obj->FClassID)
                LTools->SetTarget(obj->FClassID, 0);
        }

        // same math as the F key: frame the object's bounds, keep view direction
        Fbox box;
        if (!obj->GetBox(box))
        {
            const Fvector& p = obj->GetPosition();
            box.set(p, p);
            box.grow(2.f);
        }
        EDevice->m_Camera.ZoomExtents(box);
        UI->RedrawScene(true);	// forced frame so the RT is fresh even unfocused

        char tmp[256];
        const Fvector& cp = EDevice->m_Camera.GetPosition();
        sprintf_s(tmp, "{\"ok\":true,\"camera\":[%.2f,%.2f,%.2f]}", cp.x, cp.y, cp.z);
        out = tmp;
        return true;
    }

    if (0 == xr_strcmp(cmd, "scene_info"))
    {
        char tmp[512];
        sprintf_s(tmp, "{\"ok\":true,\"scene\":\"%s\",\"modified\":%s,\"tools\":[",
            Tools ? Tools->m_LastFileName.c_str() : "",
            UI->IsModified() ? "true" : "false");
        out = tmp;
        bool first = true;
        for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
        {
            ESceneToolBase* t = it->second;
            if (!t || it->first == OBJCLASS_DUMMY) continue;
            // AI map & co are not object tools — ListObj/ObjCount would assert
            const int cnt = Scene->GetOTool(it->first) ? Scene->ObjCount(it->first) : 0;
            sprintf_s(tmp, "%s{\"class\":\"%s\",\"objects\":%d}",
                first ? "" : ",", t->ClassName(), cnt);
            out += tmp;
            first = false;
        }
        out += "]}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "selection"))
    {
        out = "{\"ok\":true,\"selection\":[";
        bool first = true;
        for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
        {
            ESceneToolBase* t = it->second;
            if (!t || it->first == OBJCLASS_DUMMY || !Scene->GetOTool(it->first)) continue;
            ObjectList& lst = Scene->ListObj(it->first);
            for (ObjectIt o = lst.begin(); o != lst.end(); ++o)
            {
                if (!(*o)->Selected()) continue;
                if (!first) out += ",";
                AppendObjectJson(out, *o, t->ClassName());
                first = false;
            }
        }
        out += "]}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "object_info"))
    {
        char name[256];
        if (!XFinedMCP::GetArg(raw, "name", name, sizeof(name)))
        {
            out = "{\"ok\":false,\"error\":\"missing 'name' argument\"}";
            return true;
        }
        CCustomObject* obj = FindObjectRelaxed(name);
        if (!obj)
        {
            out = "{\"ok\":false,\"error\":\"object not found\"}";
            return true;
        }
        RespondObjectJson(out, obj);
        return true;
    }

    if (0 == xr_strcmp(cmd, "scene_stats"))
    {
        char tmp[512];
        // selection spans every object tool, not just the active target class
        int selected = 0;
        for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
        {
            if (!it->second || it->first == OBJCLASS_DUMMY || !Scene->GetOTool(it->first)) continue;
            ObjectList& lst = Scene->ListObj(it->first);
            for (ObjectIt o = lst.begin(); o != lst.end(); ++o)
                if ((*o)->Selected()) ++selected;
        }
        sprintf_s(tmp, "{\"ok\":true,\"scene\":\"%s\",\"modified\":%s,\"selected\":%d,",
            Tools ? Tools->m_LastFileName.c_str() : "",
            UI->IsModified() ? "true" : "false", selected);
        out = tmp;
        AppendCameraJson(out);
        out += ",\"tools\":[";
        bool first = true;
        for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
        {
            ESceneToolBase* t = it->second;
            if (!t || it->first == OBJCLASS_DUMMY) continue;
            const int cnt = Scene->GetOTool(it->first) ? Scene->ObjCount(it->first) : 0;
            sprintf_s(tmp, "%s{\"class\":\"%s\",\"objects\":%d}",
                first ? "" : ",", t->ClassName(), cnt);
            out += tmp;
            first = false;
        }
        out += "]}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "select_objects"))
    {
        char names[2048];
        if (!XFinedMCP::GetArg(raw, "names", names, sizeof(names)))
        {
            out = "{\"ok\":false,\"error\":\"missing 'names' argument (comma-separated list)\"}";
            return true;
        }
        Scene->SelectObjects(false, OBJCLASS_DUMMY);
        int found = 0;
        ObjClassID first_cls = OBJCLASS_DUMMY;
        xr_string missing = "[";
        char* ctx = 0;
        for (char* tok = strtok_s(names, ",", &ctx); tok; tok = strtok_s(0, ",", &ctx))
        {
            while (*tok == ' ') ++tok;
            for (char* e = tok + xr_strlen(tok); e > tok && e[-1] == ' ';) *--e = 0;
            if (!*tok) continue;
            CCustomObject* obj = FindObjectRelaxed(tok);
            if (obj)
            {
                obj->Select(TRUE);
                if (!found) first_cls = obj->FClassID;
                ++found;
            }
            else
            {
                if (missing.size() > 1) missing += ",";
                missing += "\"";
                missing += tok;
                missing += "\"";
            }
        }
        missing += "]";
        // behave like a manual pick: the first found class becomes the target
        if (found && LTools->CurrentClassID() != first_cls)
            LTools->SetTarget(first_cls, 0);
        UI->RedrawScene(true);
        char tmp[64];
        sprintf_s(tmp, "{\"ok\":true,\"selected\":%d,\"missing\":", found);
        out = tmp;
        out += missing;
        out += "}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "deselect_all"))
    {
        Scene->SelectObjects(false, OBJCLASS_DUMMY);
        UI->RedrawScene(true);
        out = "{\"ok\":true}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "object_set_position") ||
        0 == xr_strcmp(cmd, "object_set_rotation") ||
        0 == xr_strcmp(cmd, "object_set_scale"))
    {
        char name[256];
        if (!XFinedMCP::GetArg(raw, "name", name, sizeof(name)))
        {
            out = "{\"ok\":false,\"error\":\"missing 'name' argument\"}";
            return true;
        }
        CCustomObject* obj = FindObjectRelaxed(name);
        if (!obj)
        {
            out = "{\"ok\":false,\"error\":\"object not found\"}";
            return true;
        }
        const bool set_rot = (0 == xr_strcmp(cmd, "object_set_rotation"));
        const bool set_scl = (0 == xr_strcmp(cmd, "object_set_scale"));
        // absent components keep their current value
        Fvector v = set_rot ? obj->GetRotation() : set_scl ? obj->GetScale() : obj->GetPosition();
        bool any = false;
        any |= GetArgFloat(raw, "x", v.x);
        any |= GetArgFloat(raw, "y", v.y);
        any |= GetArgFloat(raw, "z", v.z);
        if (!any)
        {
            out = "{\"ok\":false,\"error\":\"pass at least one of x,y,z\"}";
            return true;
        }
        if (!_valid(v))
        {
            out = "{\"ok\":false,\"error\":\"non-finite component\"}";
            return true;
        }
        if      (set_rot)	obj->SetRotation(v);	// radians
        else if (set_scl)	obj->SetScale(v);
        else				obj->SetPosition(v);
        Scene->UndoSave();
        // the properties panel caches the transform on selection - without this
        // it keeps showing the pre-command values
        ExecCommand(COMMAND_UPDATE_PROPERTIES);
        UI->RedrawScene(true);
        RespondObjectJson(out, obj);
        return true;
    }

    if (0 == xr_strcmp(cmd, "place_object"))
    {
        if (Scene->locked())
        {
            out = "{\"ok\":false,\"error\":\"scene locked\"}";
            return true;
        }
        char ref[256];
        if (!XFinedMCP::GetArg(raw, "ref", ref, sizeof(ref)))
        {
            out = "{\"ok\":false,\"error\":\"missing 'ref' argument (library object, e.g. props\\\\box_1)\"}";
            return true;
        }
        for (char* p = ref; *p; ++p) if (*p == '/') *p = '\\';

        // full transform: position, rotation (radians), scale. Anything the
        // caller omits keeps its default; with no position at all the object
        // still lands in front of the camera so a bare place_object works.
        Fvector pos;
        pos.mad(EDevice->m_Camera.GetPosition(), EDevice->m_Camera.GetDirection(), 10.f);
        bool explicit_pos = false;
        explicit_pos |= GetArgFloat(raw, "x", pos.x);
        explicit_pos |= GetArgFloat(raw, "y", pos.y);
        explicit_pos |= GetArgFloat(raw, "z", pos.z);

        Fvector rot; rot.set(0.f, 0.f, 0.f);
        bool explicit_rot = false;
        explicit_rot |= GetArgFloat(raw, "rx", rot.x);
        explicit_rot |= GetArgFloat(raw, "ry", rot.y);
        explicit_rot |= GetArgFloat(raw, "rz", rot.z);

        Fvector scale; scale.set(1.f, 1.f, 1.f);
        bool explicit_scale = false;
        explicit_scale |= GetArgFloat(raw, "sx", scale.x);
        explicit_scale |= GetArgFloat(raw, "sy", scale.y);
        explicit_scale |= GetArgFloat(raw, "sz", scale.z);
        // uniform shorthand: scale = <f> sets all three
        float uniform_scale;
        if (GetArgFloat(raw, "scale", uniform_scale))
        {
            scale.set(uniform_scale, uniform_scale, uniform_scale);
            explicit_scale = true;
        }

        if (!_valid(pos) || !_valid(rot) || !_valid(scale))
        {
            out = "{\"ok\":false,\"error\":\"non-finite transform component\"}";
            return true;
        }
        if (explicit_scale && (fis_zero(scale.x) || fis_zero(scale.y) || fis_zero(scale.z)))
        {
            out = "{\"ok\":false,\"error\":\"zero scale component\"}";
            return true;
        }

        string256 namebuffer;
        char custom[256] = {};
        if (XFinedMCP::GetArg(raw, "name", custom, sizeof(custom)) && custom[0])
        {
            strlwr(custom);
            if (Scene->FindObjectByName(custom, (CCustomObject*)0))
            {
                out = "{\"ok\":false,\"error\":\"object name already used\"}";
                return true;
            }
            strncpy_s(namebuffer, sizeof(namebuffer), custom, _TRUNCATE);
        }
        else
            Scene->GenObjectName(OBJCLASS_SCENEOBJECT, namebuffer, ref);

        CSceneObject* obj = xr_new<CSceneObject>((LPVOID)0, namebuffer);
        if (!obj->SetReference(ref))
        {
            xr_delete(obj);
            out = "{\"ok\":false,\"error\":\"can't load reference object (check the library path)\"}";
            return true;
        }
        obj->SetPosition(pos);
        if (explicit_rot)   obj->SetRotation(rot);
        if (explicit_scale) obj->SetScale(scale);

        if (LTools->CurrentClassID() != OBJCLASS_SCENEOBJECT)
            LTools->SetTarget(OBJCLASS_SCENEOBJECT, 0);
        Scene->SelectObjects(false, OBJCLASS_SCENEOBJECT);
        Scene->AppendObject(obj);

        // optional: settle it on the surface below, like the End-key drop
        bool dropped = false;
        if (GetArgBool(raw, "snap_to_ground", false))
        {
            ObjectList batch;
            batch.push_back(obj);
            dropped = DropObjectsToGround(batch, GetArgBool(raw, "align_normal", false), false) > 0;
        }

        UI->RedrawScene(true);
        RespondObjectJson(out, obj);
        // splice the drop result in before the closing brace
        if (!out.empty() && out[out.size() - 1] == '}')
        {
            out.erase(out.size() - 1);
            out += dropped ? ",\"snapped_to_ground\":true}" : ",\"snapped_to_ground\":false}";
        }
        return true;
    }

    if (0 == xr_strcmp(cmd, "view_mode"))
    {
        // presets first, so explicit fields can still override them
        char preset[64] = {};
        if (XFinedMCP::GetArg(raw, "preset", preset, sizeof(preset)) && preset[0])
        {
            if (0 == _stricmp(preset, "lit"))
            {
                EDevice->dwFillMode = D3DFILL_SOLID;
                psDeviceFlags.set(rsLighting, TRUE);
                psDeviceFlags.set(rsRenderTextures, TRUE);
            }
            else if (0 == _stricmp(preset, "unlit"))
            {
                EDevice->dwFillMode = D3DFILL_SOLID;
                psDeviceFlags.set(rsLighting, FALSE);
                psDeviceFlags.set(rsRenderTextures, TRUE);
            }
            else if (0 == _stricmp(preset, "wireframe"))
            {
                EDevice->dwFillMode = D3DFILL_WIREFRAME;
            }
            else if (0 == _stricmp(preset, "point"))
            {
                EDevice->dwFillMode = D3DFILL_POINT;
            }
            else
            {
                out = "{\"ok\":false,\"error\":\"unknown preset\",\"presets\":"
                      "[\"unlit\",\"lit\",\"wireframe\",\"point\"]}";
                return true;
            }
        }

        char fill[64] = {};
        if (XFinedMCP::GetArg(raw, "fill", fill, sizeof(fill)) && fill[0])
        {
            if      (0 == _stricmp(fill, "solid"))     EDevice->dwFillMode = D3DFILL_SOLID;
            else if (0 == _stricmp(fill, "wireframe")) EDevice->dwFillMode = D3DFILL_WIREFRAME;
            else if (0 == _stricmp(fill, "point"))     EDevice->dwFillMode = D3DFILL_POINT;
            else
            {
                out = "{\"ok\":false,\"error\":\"fill must be solid, wireframe or point\"}";
                return true;
            }
        }

        char shade[64] = {};
        if (XFinedMCP::GetArg(raw, "shade", shade, sizeof(shade)) && shade[0])
        {
            if      (0 == _stricmp(shade, "flat"))    EDevice->dwShadeMode = D3DSHADE_FLAT;
            else if (0 == _stricmp(shade, "gouraud")) EDevice->dwShadeMode = D3DSHADE_GOURAUD;
            else
            {
                out = "{\"ok\":false,\"error\":\"shade must be flat or gouraud\"}";
                return true;
            }
        }

        // every render flag the editor's Options menu exposes
        struct SFlagArg { pcstr name; u32 mask; };
        static const SFlagArg kFlags[] =
        {
            { "lighting",		rsLighting },
            { "textures",		rsRenderTextures },
            { "edged_faces",	rsEdgedFaces },
            { "filter_linear",	rsFilterLinear },
            { "fog",			rsFog },
            { "environment",	rsEnvironment },
            { "grid",			rsDrawGrid },
            { "safe_rect",		rsDrawSafeRect },
        };
        for (const SFlagArg& f : kFlags)
        {
            const bool current = !!psDeviceFlags.test(f.mask);
            const bool wanted = GetArgBool(raw, f.name, current);
            if (wanted != current)
                psDeviceFlags.set(f.mask, wanted ? TRUE : FALSE);
        }

        UI->RedrawScene(true);

        pcstr fill_name = EDevice->dwFillMode == D3DFILL_WIREFRAME ? "wireframe"
                        : EDevice->dwFillMode == D3DFILL_POINT     ? "point" : "solid";
        pcstr shade_name = EDevice->dwShadeMode == D3DSHADE_FLAT ? "flat" : "gouraud";
        char tmp[1024];
        sprintf_s(tmp, sizeof(tmp),
            "{\"ok\":true,\"fill\":\"%s\",\"shade\":\"%s\","
            "\"lighting\":%s,\"textures\":%s,\"edged_faces\":%s,\"filter_linear\":%s,"
            "\"fog\":%s,\"environment\":%s,\"grid\":%s,\"safe_rect\":%s,"
            "\"presets\":[\"unlit\",\"lit\",\"wireframe\",\"point\"]}",
            fill_name, shade_name,
            psDeviceFlags.test(rsLighting) ? "true" : "false",
            psDeviceFlags.test(rsRenderTextures) ? "true" : "false",
            psDeviceFlags.test(rsEdgedFaces) ? "true" : "false",
            psDeviceFlags.test(rsFilterLinear) ? "true" : "false",
            psDeviceFlags.test(rsFog) ? "true" : "false",
            psDeviceFlags.test(rsEnvironment) ? "true" : "false",
            psDeviceFlags.test(rsDrawGrid) ? "true" : "false",
            psDeviceFlags.test(rsDrawSafeRect) ? "true" : "false");
        out = tmp;
        return true;
    }

    if (0 == xr_strcmp(cmd, "list_assets"))
    {
        char category[128] = {};
        int cat = 0; // Objects by default: the only placeable category
        if (XFinedMCP::GetArg(raw, "category", category, sizeof(category)) && category[0])
        {
            cat = UIContentBrowser::FindCategory(category);
            if (cat < 0)
            {
                out = "{\"ok\":false,\"error\":\"unknown category\",\"categories\":[";
                for (int i = 0; i < UIContentBrowser::CategoryCount(); ++i)
                {
                    if (i) out += ",";
                    out += "\"";
                    out += UIContentBrowser::CategoryName(i);
                    out += "\"";
                }
                out += "]}";
                return true;
            }
        }

        SChooseEvents* events = UIChooseForm::GetEvents(UIContentBrowser::CategoryId(cat));
        if (!events || !events->on_fill)
        {
            out = "{\"ok\":false,\"error\":\"category has no enumerator in this build\"}";
            return true;
        }
        ChooseItemVec items;
        events->on_fill(items, 0);

        char filter[256] = {};
        XFinedMCP::GetArg(raw, "filter", filter, sizeof(filter));
        strlwr(filter);
        const int limit = GetArgInt(raw, "limit", 200);

        u32 total = 0, shown = 0;
        xr_string list;
        for (ChooseItemVecIt it = items.begin(); it != items.end(); ++it)
        {
            if (filter[0])
            {
                string512 lowered;
                strncpy_s(lowered, sizeof(lowered), it->name.c_str(), _TRUNCATE);
                strlwr(lowered);
                if (!strstr(lowered, filter)) continue;
            }
            ++total;
            if (int(shown) >= limit) continue;
            if (shown) list += ",";
            list += "{\"name\":\"";
            list += JsonEscapePath(it->name.c_str());
            list += "\"}";
            ++shown;
        }
        char head[256];
        sprintf_s(head, sizeof(head), "{\"ok\":true,\"category\":\"%s\",\"shown\":%u,\"total\":%u,\"assets\":[",
            UIContentBrowser::CategoryName(cat), shown, total);
        out = head;
        out += list;
        out += "]}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "asset_preview"))
    {
        char name[512];
        if (!XFinedMCP::GetArg(raw, "name", name, sizeof(name)))
        {
            out = "{\"ok\":false,\"error\":\"missing 'name' argument (see list_assets)\"}";
            return true;
        }
        for (char* p = name; *p; ++p) if (*p == '/') *p = '\\';
        // GetArg hands over the raw JSON value, so an escaped "a\\b" arrives with
        // both slashes. Lookups that compare paths exactly - the DARF registry
        // does a binary search - miss on that, even though the FS itself shrugs
        // doubled separators off.
        {
            char* w = name;
            for (const char* r = name; *r; ++r)
                if (!(*r == '\\' && w > name && w[-1] == '\\')) *w++ = *r;
            *w = 0;
        }

        // source=visual|darf render the model instead of looking up a baked .thm,
        // which is the only way to preview a game mesh: .ogf files ship no
        // thumbnail and are not part of the editor object library
        char source[64] = {};
        XFinedMCP::GetArg(raw, "source", source, sizeof(source));
        if (0 == _stricmp(source, "object"))
        {
            // library .object, rendered rather than read from a baked .thm
            U32Vec pixels;
            if (!RenderObjectThumbnail(name, pixels))
            {
                out = "{\"ok\":false,\"error\":\"cannot render this library object\"}";
                return true;
            }
            void* tex = XFinedMCP::PixelsToTexture(pixels);
            xr_string png;
            const bool encoded = tex && XFinedMCP::TextureToPngBase64(tex, png);
            XFinedMCP::ReleaseTexture(tex);
            if (!encoded)
            {
                out = "{\"ok\":false,\"error\":\"cannot encode the rendered preview\"}";
                return true;
            }
            out = "{\"ok\":true,\"rendered\":true,\"name\":\"";
            out += JsonEscapePath(name);
            out += "\",\"png_base64\":\"";
            out += png;
            out += "\"}";
            return true;
        }

        if (0 == _stricmp(source, "visual") || 0 == _stricmp(source, "darf"))
        {
            const bool from_game = (0 == _stricmp(source, "darf"));
            U32Vec pixels;
            bool rendered = false;

            if (from_game)
            {
                // mounting is lazy: without this the first DARF request of a
                // session reports "not found" against an empty registry
                xr_string mount_err;
                if (!EditorGameContent::EnsureMounted(mount_err))
                {
                    out = "{\"ok\":false,\"error\":\"";
                    out += mount_err;
                    out += "\"}";
                    return true;
                }
                const int idx = EditorGameContent::Find(name);
                if (idx < 0)
                {
                    out = "{\"ok\":false,\"error\":\"not found in the linked game install (see darf_list)\"}";
                    return true;
                }
                u32 sz = 0;
                u8* bytes = EditorGameContent::ReadBytes(idx, sz);
                if (bytes && sz)
                    rendered = RenderVisualThumbnailFromMemory(name, bytes, sz, pixels);
                EditorGameContent::FreeBytes(bytes);
                if (!rendered)
                {
                    // same fallback as the browser: render the editor's own copy
                    // of that mesh path when loading from bytes is unavailable
                    char visual[MAX_PATH];
                    LPCSTR src = name;
                    if (0 == _strnicmp(src, "meshes\\", 7)) src += 7;
                    strncpy_s(visual, src, _TRUNCATE);
                    if (char* dot = strrchr(visual, '.')) *dot = 0;
                    rendered = RenderVisualThumbnail(visual, pixels);
                }
            }
            else
            {
                rendered = RenderVisualThumbnail(name, pixels);
            }

            if (!rendered)
            {
                out = "{\"ok\":false,\"error\":\"cannot render a preview for this model\"}";
                return true;
            }
            void* tex = XFinedMCP::PixelsToTexture(pixels);
            if (!tex)
            {
                out = "{\"ok\":false,\"error\":\"cannot upload the rendered preview\"}";
                return true;
            }
            xr_string png;
            const bool encoded = XFinedMCP::TextureToPngBase64(tex, png);
            XFinedMCP::ReleaseTexture(tex);
            if (!encoded)
            {
                out = "{\"ok\":false,\"error\":\"cannot encode the rendered preview\"}";
                return true;
            }
            out = "{\"ok\":true,\"rendered\":true,\"name\":\"";
            out += JsonEscapePath(name);
            out += "\",\"png_base64\":\"";
            out += png;
            out += "\"}";
            return true;
        }

        char category[128] = {};
        int cat = 0;
        if (XFinedMCP::GetArg(raw, "category", category, sizeof(category)) && category[0])
        {
            cat = UIContentBrowser::FindCategory(category);
            if (cat < 0)
            {
                out = "{\"ok\":false,\"error\":\"unknown category\"}";
                return true;
            }
        }

        SChooseEvents* events = UIChooseForm::GetEvents(UIContentBrowser::CategoryId(cat));
        if (!events || !events->on_get_texture)
        {
            out = "{\"ok\":false,\"error\":\"category has no thumbnails in this build\"}";
            return true;
        }
        // the adapter allocates a fresh texture per call - release it after use
        ImTextureID tex = 0;
        events->on_get_texture(name, tex);
        if (!tex)
        {
            out = "{\"ok\":false,\"error\":\"no preview for this asset (missing thumbnail or wrong name)\"}";
            return true;
        }
        xr_string png;
        const bool ok = XFinedMCP::TextureToPngBase64((void*)tex, png);
        XFinedMCP::ReleaseTexture((void*)tex);
        if (!ok)
        {
            out = "{\"ok\":false,\"error\":\"cannot encode the thumbnail\"}";
            return true;
        }
        out = "{\"ok\":true,\"name\":\"";
        out += JsonEscapePath(name);
        out += "\",\"png_base64\":\"";
        out += png;
        out += "\"}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "drop_objects"))
    {
        if (Scene->locked())
        {
            out = "{\"ok\":false,\"error\":\"scene locked\"}";
            return true;
        }
        ObjectList batch;
        char names[4096] = {};
        xr_string missing;
        if (XFinedMCP::GetArg(raw, "names", names, sizeof(names)) && names[0])
        {
            for (char* cursor = names; cursor && *cursor;)
            {
                char* comma = strchr(cursor, ',');
                if (comma) *comma = 0;
                while (*cursor == ' ') ++cursor;
                char* end = cursor + xr_strlen(cursor);
                while (end > cursor && end[-1] == ' ') *--end = 0;
                if (*cursor)
                {
                    if (CCustomObject* obj = FindObjectRelaxed(cursor))
                        batch.push_back(obj);
                    else
                    {
                        if (!missing.empty()) missing += ",";
                        missing += cursor;
                    }
                }
                cursor = comma ? comma + 1 : 0;
            }
        }
        else
            CollectSelectedObjects(batch);

        if (batch.empty())
        {
            out = "{\"ok\":false,\"error\":\"nothing to drop (pass 'names' or select objects)\"}";
            return true;
        }

        char mode[32] = {};
        XFinedMCP::GetArg(raw, "mode", mode, sizeof(mode));
        const bool line_trace = (0 == _stricmp(mode, "line"));

        const u32 moved = DropObjectsToGround(batch, GetArgBool(raw, "align_normal", false), line_trace);
        if (moved)
        {
            Scene->UndoSave();
            ExecCommand(COMMAND_UPDATE_PROPERTIES);
            UI->RedrawScene(true);
        }
        char tmp[512];
        sprintf_s(tmp, sizeof(tmp), "{\"ok\":true,\"mode\":\"%s\",\"requested\":%u,\"dropped\":%u,\"missing\":\"%s\"}",
            line_trace ? "line" : "box", u32(batch.size()), moved, missing.c_str());
        out = tmp;
        return true;
    }

    if (0 == xr_strcmp(cmd, "rename_object"))
    {
        char name[256], new_name[256];
        if (!XFinedMCP::GetArg(raw, "name", name, sizeof(name)) ||
            !XFinedMCP::GetArg(raw, "new_name", new_name, sizeof(new_name)) || !new_name[0])
        {
            out = "{\"ok\":false,\"error\":\"need 'name' and non-empty 'new_name'\"}";
            return true;
        }
        CCustomObject* obj = FindObjectRelaxed(name);
        if (!obj)
        {
            out = "{\"ok\":false,\"error\":\"object not found\"}";
            return true;
        }
        strlwr(new_name);
        if (Scene->FindObjectByName(new_name, (CCustomObject*)0))
        {
            out = "{\"ok\":false,\"error\":\"new name already used\"}";
            return true;
        }
        obj->SetName(new_name);
        Scene->UndoSave();
        ExecCommand(COMMAND_UPDATE_PROPERTIES);
        RespondObjectJson(out, obj);
        return true;
    }

    if (0 == xr_strcmp(cmd, "delete_selected"))
    {
        if (Scene->locked())
        {
            out = "{\"ok\":false,\"error\":\"scene locked\"}";
            return true;
        }
        int removed = 0;
        for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
        {
            if (!it->second || it->first == OBJCLASS_DUMMY || !Scene->GetOTool(it->first)) continue;
            ObjectList& lst = Scene->ListObj(it->first);
            for (ObjectIt o = lst.begin(); o != lst.end(); ++o)
                if ((*o)->Selected()) ++removed;
        }
        Scene->RemoveSelection(OBJCLASS_DUMMY);
        Scene->UndoSave();
        UI->RedrawScene(true);
        char tmp[64];
        sprintf_s(tmp, "{\"ok\":true,\"removed\":%d}", removed);
        out = tmp;
        return true;
    }

    if (0 == xr_strcmp(cmd, "save_scene"))
    {
        if (Scene->locked())
        {
            out = "{\"ok\":false,\"error\":\"scene locked\"}";
            return true;
        }
        char file[MAX_PATH] = {};
        XFinedMCP::GetArg(raw, "file", file, sizeof(file));
        xr_string fn = file[0] ? file : LTools->m_LastFileName.c_str();
        if (fn.empty())
        {
            out = "{\"ok\":false,\"error\":\"scene has no file yet - pass 'file' or Save As in the editor\"}";
            return true;
        }
        for (u32 i = 0; i < fn.size(); ++i) if (fn[i] == '/') fn[i] = '\\';
        ExecCommand(COMMAND_SAVE, fn);
        out = "{\"ok\":true,\"file\":\"";
        for (u32 i = 0; i < fn.size(); ++i) out += (fn[i] == '\\') ? '/' : fn[i];
        out += "\"}";
        return true;
    }

    if (0 == xr_strcmp(cmd, "undo") || 0 == xr_strcmp(cmd, "redo"))
    {
        if (Scene->locked())
        {
            out = "{\"ok\":false,\"error\":\"scene locked\"}";
            return true;
        }
        const bool is_undo = (0 == xr_strcmp(cmd, "undo"));
        const bool done = is_undo ? !!Scene->Undo() : !!Scene->Redo();
        if (!done)
        {
            out = is_undo ? "{\"ok\":false,\"error\":\"undo buffer empty\"}"
                          : "{\"ok\":false,\"error\":\"redo buffer empty\"}";
            return true;
        }
        LTools->Reset();
        ExecCommand(COMMAND_CHANGE_ACTION, etaSelect);
        UI->RedrawScene(true);
        out = "{\"ok\":true}";
        return true;
    }

    return false;
}

void CLevelMain::RegisterCommands()
{
	inherited::RegisterCommands	();
    // tools
	REGISTER_SUB_CMD_CE	(COMMAND_CHANGE_TARGET,             "Change Target", 		LTools,CLevelTool::CommandChangeTarget, true);
		APPEND_SUB_CMD	("Object", 							OBJCLASS_SCENEOBJECT,	0);
		APPEND_SUB_CMD	("Light", 							OBJCLASS_LIGHT, 		0);
		APPEND_SUB_CMD	("Sound Source",					OBJCLASS_SOUND_SRC, 	0);
		APPEND_SUB_CMD	("Sound Env", 		                OBJCLASS_SOUND_ENV, 	0);
		APPEND_SUB_CMD	("Glow", 			                OBJCLASS_GLOW, 			0);
		APPEND_SUB_CMD	("Shape", 			                OBJCLASS_SHAPE, 		0);
		APPEND_SUB_CMD	("Spawn Point", 	                OBJCLASS_SPAWNPOINT, 	0);
		APPEND_SUB_CMD	("Way", 			                OBJCLASS_WAY, 			0);
		APPEND_SUB_CMD	("Way Point", 		                OBJCLASS_WAY, 			1);
		APPEND_SUB_CMD	("Toggle Way Mode",	                OBJCLASS_WAY, 			2);
		APPEND_SUB_CMD	("Sector", 			                OBJCLASS_SECTOR, 		0);
		APPEND_SUB_CMD	("Portal", 			                OBJCLASS_PORTAL, 		0);
		APPEND_SUB_CMD	("Group", 			                OBJCLASS_GROUP, 		0);
		APPEND_SUB_CMD	("Particle System",                 OBJCLASS_PS, 			0);
		APPEND_SUB_CMD	("Detail Objects", 	                OBJCLASS_DO, 			0);
		APPEND_SUB_CMD	("AI Map", 			                OBJCLASS_AIMAP, 		0);
		APPEND_SUB_CMD	("Static Wallmark",                 OBJCLASS_WM, 			0);
    REGISTER_SUB_CMD_END;    
	REGISTER_CMD_C	    (COMMAND_ENABLE_TARGET,           	LTools,CLevelTool::CommandEnableTarget);
	REGISTER_CMD_C	    (COMMAND_SHOW_TARGET,           	LTools,CLevelTool::CommandShowTarget);
	REGISTER_CMD_C	    (COMMAND_READONLY_TARGET,          	LTools,CLevelTool::CommandReadonlyTarget);
	REGISTER_CMD_C	    (COMMAND_MULTI_RENAME_OBJECTS,     	LTools,CLevelTool::CommandMultiRenameObjects);

	REGISTER_CMD_CE	    (COMMAND_SHOW_OBJECTLIST,           "Scene\\Show Object List",		LTools,CLevelTool::CommandShowObjectList, false);
	// common
	REGISTER_CMD_S	    (COMMAND_LIBRARY_EDITOR,           	CommandLibraryEditor);
	REGISTER_CMD_S	    (COMMAND_LANIM_EDITOR,            	CommandLAnimEditor);
	REGISTER_CMD_SE	    (COMMAND_FILE_MENU,              	"File\\Menu",					CommandFileMenu, 		true);
    REGISTER_CMD_S		(COMMAND_LOAD_LEVEL_PART,			CommandLoadLevelPart);
    REGISTER_CMD_S		(COMMAND_UNLOAD_LEVEL_PART,			CommandUnloadLevelPart);
	REGISTER_CMD_SE	    (COMMAND_LOAD,              		"File\\Load Level", 			CommandLoad, 			true);
    REGISTER_SUB_CMD_SE (COMMAND_SAVE, 						"File",							CommandSave,			true);
    	APPEND_SUB_CMD	("Save",							0,								0);
    	APPEND_SUB_CMD	("Save As",							0,								1);
    REGISTER_SUB_CMD_END;
	REGISTER_CMD_S	    (COMMAND_SAVE_BACKUP,              	CommandSaveBackup);
	REGISTER_CMD_SE	    (COMMAND_CLEAR,              		"File\\Clear Scene", 			CommandClear,			true);
	REGISTER_CMD_SE	    (COMMAND_LOAD_FIRSTRECENT,          "File\\Load First Recent",		CommandLoadFirstRecent, true);
	REGISTER_CMD_S	    (COMMAND_CLEAR_DEBUG_DRAW, 		    CommandClearDebugDraw);
	REGISTER_CMD_S	    (COMMAND_IMPORT_COMPILER_ERROR,     CommandImportCompilerError);
	REGISTER_CMD_S	    (COMMAND_EXPORT_COMPILER_ERROR,     CommandExportCompilerError);
	REGISTER_CMD_S	    (COMMAND_VALIDATE_SCENE,            CommandValidateScene);
	REGISTER_CMD_S	    (COMMAND_CLEAN_LIBRARY,           	CommandCleanLibrary);
	REGISTER_CMD_S	    (COMMAND_RELOAD_OBJECTS,            CommandReloadObjects);
	REGISTER_CMD_SE	    (COMMAND_CUT,              			"Edit\\Cut",					CommandCut,false);
	REGISTER_CMD_SE	    (COMMAND_COPY,              		"Edit\\Copy",					CommandCopy,false);
	REGISTER_CMD_SE	    (COMMAND_PASTE,              		"Edit\\Paste",					CommandPaste,false);
	REGISTER_CMD_S	    (COMMAND_LOAD_SELECTION,            CommandLoadSelection);
	REGISTER_CMD_S	    (COMMAND_SAVE_SELECTION,            CommandSaveSelection);
	REGISTER_CMD_SE	    (COMMAND_UNDO,              		"Edit\\Undo",					CommandUndo,false);
	REGISTER_CMD_SE	    (COMMAND_REDO,              		"Edit\\Redo",					CommandRedo,false);
	REGISTER_CMD_S	    (COMMAND_CLEAR_SCENE_SUMMARY,	    CommandClearSceneSummary);
	REGISTER_CMD_S	    (COMMAND_COLLECT_SCENE_SUMMARY,     CommandCollectSceneSummary);
	REGISTER_CMD_S	    (COMMAND_SHOW_SCENE_SUMMARY,        CommandShowSceneSummary);
	REGISTER_CMD_S	    (COMMAND_EXPORT_SCENE_SUMMARY,      CommandExportSceneSummary);
	REGISTER_CMD_S	    (COMMAND_SCENE_HIGHLIGHT_TEXTURE,	CommandSceneHighlightTexture);
	REGISTER_CMD_SE	    (COMMAND_OPTIONS,              		"Scene\\Options",		        CommandOptions,false);
	REGISTER_CMD_SE	    (COMMAND_BUILD,              		"Compile\\Build",		        CommandBuild,false);
	REGISTER_CMD_SE	    (COMMAND_MAKE_GAME,              	"Compile\\Make Game",	        CommandMakeGame,false);
	REGISTER_CMD_SE	    (COMMAND_MAKE_AIMAP,              	"Compile\\Make AI Map",	        CommandMakeAIMap,false);
	REGISTER_CMD_SE	    (COMMAND_MAKE_DETAILS,              "Compile\\Make Details",        CommandMakeDetails,false);
	REGISTER_CMD_SE	    (COMMAND_MAKE_HOM,              	"Compile\\Make HOM",	        CommandMakeHOM,false);
	REGISTER_CMD_SE	    (COMMAND_MAKE_SOM,              	"Compile\\Make SOM",	        CommandMakeSOM,false);
	REGISTER_CMD_SE	    (COMMAND_INVERT_SELECTION_ALL,      "Selection\\Invert", 			CommandInvertSelectionAll,false);
	REGISTER_CMD_SE	    (COMMAND_SELECT_ALL,              	"Selection\\Select All", 		CommandSelectAll,false);
	REGISTER_CMD_SE	    (COMMAND_DESELECT_ALL,              "Selection\\Unselect All", 		CommandDeselectAll,false);
	REGISTER_CMD_SE	    (COMMAND_DELETE_SELECTION,          "Edit\\Delete", 				CommandDeleteSelection,false);
	REGISTER_CMD_SE	    (COMMAND_HIDE_UNSEL,              	"Visibility\\Hide Unselected",	CommandHideUnsel,false);
	REGISTER_CMD_SE	    (COMMAND_HIDE_SEL,              	"Visibility\\Hide Selected", 	CommandHideSel,false);
	REGISTER_CMD_SE	    (COMMAND_HIDE_ALL,              	"Visibility\\Hide All", 		CommandHideAll,false);
	REGISTER_CMD_S		(COMMAND_SET_SNAP_OBJECTS,          CommandSetSnapObjects);
	REGISTER_CMD_S	    (COMMAND_ADD_SEL_SNAP_OBJECTS,      CommandAddSelSnapObjects);
	REGISTER_CMD_S	    (COMMAND_DEL_SEL_SNAP_OBJECTS,      CommandDelSelSnapObjects);
	REGISTER_CMD_S	    (COMMAND_CLEAR_SNAP_OBJECTS,        CommandClearSnapObjects);
	REGISTER_CMD_S	    (COMMAND_SELECT_SNAP_OBJECTS,       CommandSelectSnapObjects);
	REGISTER_CMD_S	    (COMMAND_REFRESH_SNAP_OBJECTS,      CommandRefreshSnapObjects);
//	REGISTER_CMD_S	    (COMMAND_REFRESH_SOUND_ENVS,        CommandRefreshSoundEnvs);
	REGISTER_CMD_S	    (COMMAND_REFRESH_SOUND_ENV_GEOMETRY,CommandRefreshSoundEnvGeometry);
	REGISTER_SUB_CMD_SE (COMMAND_GIZMO_MODE,				"Gizmo",						CommandGizmoMode,false);
		APPEND_SUB_CMD	("Select",	u32(Gizmo::EType::None),	u32(0));
		APPEND_SUB_CMD	("Move",	u32(Gizmo::EType::Move),	u32(0));
		APPEND_SUB_CMD	("Rotate",	u32(Gizmo::EType::Rotate),	u32(0));
		APPEND_SUB_CMD	("Scale",	u32(Gizmo::EType::Scale),	u32(0));
		APPEND_SUB_CMD	("Cycle",	u32(Gizmo::EType::Count),	u32(0));
	REGISTER_SUB_CMD_END;
	REGISTER_CMD_S	    (COMMAND_CB_PLACE_ASSET,			CommandCBPlaceAsset);
	REGISTER_CMD_SE	    (COMMAND_DROP_TO_GROUND,			"Object\\Drop To Ground",		CommandDropToGround,false);
	REGISTER_CMD_SE	    (COMMAND_SHOW_CONTENT_BROWSER,		"Windows\\Content Browser",		CommandShowContentBrowser,false);
	REGISTER_CMD_SE	    (COMMAND_SHOW_WORLD_OUTLINER,		"Windows\\World Outliner",		CommandShowWorldOutliner,false);
	REGISTER_CMD_S	    (COMMAND_SHOWCONTEXTMENU,           CommandShowContextMenu);
	REGISTER_CMD_S	    (COMMAND_REFRESH_UI_BAR,            CommandRefreshUIBar);
	REGISTER_CMD_S	    (COMMAND_RESTORE_UI_BAR,            CommandRestoreUIBar);
	REGISTER_CMD_S	    (COMMAND_SAVE_UI_BAR,              	CommandSaveUIBar);
	REGISTER_CMD_S	    (COMMAND_UPDATE_TOOLBAR,            CommandUpdateToolBar);
	REGISTER_CMD_S	    (COMMAND_UPDATE_CAPTION,            CommandUpdateCaption);
	REGISTER_CMD_S	    (COMMAND_CREATE_SOUND_LIB,          CommandCreateSoundLib);
	REGISTER_CMD_SE	    (COMMAND_TOGGLE_AIMAP_VISIBILITY,   "Visibility\\Toggle AIMap",			CommandToggleAiMapVisibility,true);
	REGISTER_CMD_S	    (COMMAND_SHOW_CLIP_EDITOR,			CommandShowClipEditor);
    
}

char* CLevelMain::GetCaption()
{
	return Tools->m_LastFileName.empty()?"noname":Tools->m_LastFileName.c_str();
}

// Unreal-style default arrangement: the right column (tools + properties) runs
// the full height, the content browser only spans the area left of it, and the
// viewport keeps the centre. Built on a fresh install or on Windows > Reset
// Layout; the user's own arrangement is kept in level_imgui.ini afterwards.
void CLevelMain::BuildDefaultDockLayout(unsigned int dockspace_id)
{
    DockLayoutBegin(dockspace_id);
    DockLayoutPlace("Render",           DockCenter);
    // same slot as Render, so the model preview opens as a tab beside it
    DockLayoutPlace(UIVisualPreview::WindowID(), DockCenter);
    DockLayoutPlace("Content Browser",  DockBottom);
    DockLayoutPlace("World Outliner",   DockLeft);
    DockLayoutPlace("Object List",      DockLeft);
    DockLayoutPlace("LeftBar",          DockRight);
    DockLayoutPlace("Properties",       DockRightBottom);
    DockLayoutPlace("World Properties", DockRightBottom);
    DockLayoutEnd();
}

bool  CLevelMain::ApplyShortCut(DWORD Key, TShiftState Shift)
{
    if (Scene->IsPlayInEditor())return true;
    if (!EditorProject::Active())return true;	// project browser owns the screen
    return inherited::ApplyShortCut(Key,Shift);
}


bool  CLevelMain::ApplyGlobalShortCut(DWORD Key, TShiftState Shift)
{
    return inherited::ApplyGlobalShortCut(Key,Shift);
}

void RetrieveSceneObjPointAndNormal( Fvector& hitpoint, Fvector* hitnormal, const SRayPickInfo &pinf, int bSnap )
{
    if(pinf.e_mesh == 0)
    {
      hitpoint = pinf.pt;
       if (hitnormal && pinf.visual_inf.K )
        	*hitnormal = pinf.visual_inf.normal;
       return;
    }
    if (Tools->GetSettings(etfVSnap) && bSnap)
    {
        Fvector pn;
        float u = pinf.inf.u;
        float v = pinf.inf.v;
        float w = 1 - (u + v);
        Fvector verts[3];
        pinf.e_obj->GetFaceWorld(pinf.s_obj->_Transform(), pinf.e_mesh, pinf.inf.id, verts);

        if ((w > u) && (w > v))
            pn.set(verts[0]);
        else if ((u > w) && (u > v))
            pn.set(verts[1]);
        else
            pn.set(verts[2]);

        hitpoint.set(pinf.pt);

    }
    else
    {
    	hitpoint.set(pinf.pt);
    }

    if (hitnormal)
    {
    	Fvector verts[3];
        pinf.e_obj->GetFaceWorld(pinf.s_obj->_Transform(),pinf.e_mesh,pinf.inf.id,verts);
        hitnormal->mknormal(verts[0],verts[1],verts[2]);
    }
}


bool EditLibPickObjectGeometry(  Fvector& hitpoint,  const Fvector& start, const Fvector& direction, int bSnap, Fvector* hitnormal )
{
	SRayPickInfo pinf;
	/*if( TfrmEditLibrary::RayPick( start, direction, &pinf ) )
    {
    	RetrieveSceneObjPointAndNormal( hitpoint,  hitnormal, pinf, bSnap );
        return true;
    }*/
    return false;
}

bool ScenePickObjectGeometry( Fvector& hitpoint,  const Fvector& start, const Fvector& direction, int bSnap, Fvector* hitnormal )
{

	SRayPickInfo pinf;

   
    SRayPickInfo l_pinf;
    bool bResult = false;

    {
      SRayPickInfo l_pinf;
      bool l_bres = Scene->RayPickObject( l_pinf.inf.range, start,direction, OBJCLASS_SPAWNPOINT , &l_pinf, Scene->GetSnapList(false) );
      
      if( l_bres )
      {
          pinf = l_pinf;
          bResult = true;
      }
      
    }
    {
    
     SRayPickInfo l_pinf;
     bool l_bres = Scene->RayPickObject( l_pinf.inf.range, start, direction, OBJCLASS_SCENEOBJECT , &l_pinf, Scene->GetSnapList(false) );

     if( !bResult||(l_bres && l_pinf.inf.range < pinf.inf.range) )
          pinf = l_pinf;
     if( l_bres )
           bResult = true;
           
    }


     if( bResult )
     	    RetrieveSceneObjPointAndNormal( hitpoint,  hitnormal, pinf, bSnap );
            
     return  bResult;

}


bool PickObjectGeometry( EEditorState est, Fvector& hitpoint,  const Fvector& start, const Fvector& direction, int bSnap, Fvector* hitnormal )
{

	switch(est)
    {
       case esEditLibrary:
         	return EditLibPickObjectGeometry( hitpoint, start, direction, bSnap, hitnormal );
       case esEditScene:
         	return ScenePickObjectGeometry( hitpoint, start, direction, bSnap, hitnormal );
        default:
         	NODEFAULT;
    }
	return false;
}

bool PickGrid(  Fvector& hitpoint,  const Fvector& start, const Fvector& direction, int bSnap, Fvector* hitnormal )
{
     
    // pick grid
	Fvector normal;
	normal.set( 0, 1, 0 );
	float clcheck = direction.dotproduct( normal );

	if( fis_zero( clcheck ) )
    	 return false;

	float alpha = - start.dotproduct(normal) / clcheck;
    
	if( alpha <= 0 )
    	return false;

	hitpoint.x = start.x + direction.x * alpha;
	hitpoint.y = start.y + direction.y * alpha;
	hitpoint.z = start.z + direction.z * alpha;

    if (LTools->GetGimzo()->IsStepEnable(Gizmo::EType::Move) && bSnap)
    {
        hitpoint.x = snapto( hitpoint.x, LTools->GetGimzo()->GetStep(Gizmo::EType::Move));
        hitpoint.z = snapto( hitpoint.z, LTools->GetGimzo()->GetStep(Gizmo::EType::Move));
        hitpoint.y = 0.f;
    }
    
	if (hitnormal)
    	hitnormal->set(0,1,0);
	return true;
}

bool CLevelMain::PickGround(Fvector& hitpoint, const Fvector& start, const Fvector& direction, int bSnap, Fvector* hitnormal){
	VERIFY(m_bReady);
    
    EEditorState est = GetEState();
    if( est!= esEditLibrary && est != esEditScene )
    	return false;
   
    // pick object geometry
    if( (bSnap==-1) || ( Tools->GetSettings(etfOSnap) && (bSnap==1) ) )
    {
      bool b =  PickObjectGeometry( est, hitpoint, start, direction, bSnap,  hitnormal );
      if(b)
      return true;
    }

    return   PickGrid( hitpoint, start, direction, bSnap,  hitnormal );

}


bool CLevelMain::SelectionFrustum(CFrustum& frustum)
{
	VERIFY(m_bReady);
    Fvector st,d,p[4];
    Ivector2 pt[4];

    float depth = 0;

    float x1=m_StartCp.x, x2=m_CurrentCp.x;
    float y1=m_StartCp.y, y2=m_CurrentCp.y;

	if(!(x1!=x2&&y1!=y2)) return false;

	pt[0].set(_min(x1,x2),_min(y1,y2));
	pt[1].set(_max(x1,x2),_min(y1,y2));
	pt[2].set(_max(x1,x2),_max(y1,y2));
	pt[3].set(_min(x1,x2),_max(y1,y2));

    SRayPickInfo pinf;
    for (int i=0; i<4; i++){
	    EDevice->m_Camera.MouseRayFromPoint(st, d, pt[i]);
        if (EPrefs->bp_lim_depth){
			pinf.inf.range = EDevice->m_Camera._Zfar(); // max pick range
            if (Scene->RayPickObject(pinf.inf.range, st, d, OBJCLASS_SCENEOBJECT, &pinf, 0))
	            if (pinf.inf.range > depth) depth = pinf.inf.range;
        }
    }
    if (depth<EDevice->m_Camera._Znear()) depth = EDevice->m_Camera._Zfar();
    else depth += EPrefs->bp_depth_tolerance;

    for (int i=0; i<4; i++){
	    EDevice->m_Camera.MouseRayFromPoint(st, d, pt[i]);
        p[i].mad(st,d,depth);
    }

    Fvector pos = EDevice->m_Camera.GetPosition();
    frustum.CreateFromPoints(p,4,pos);

    Fplane P; P.build(p[0],p[1],p[2]);
    if (P.classify(st)>0) P.build(p[2],p[1],p[0]);
	frustum._add(P);

	return true;
}

void CLevelMain::RealUpdateScene()
{
	inherited::RealUpdateScene	();
	if (GetEState()==esEditScene)
    {
	    Scene->OnObjectsUpdate	();
    	LTools->OnObjectsUpdate	(); // �������� ��� ��� ���-�� ������� � ���������
	    RedrawScene				();
    }
}



void CLevelMain::ShowContextMenu(int cls)
{
	VERIFY(m_bReady);
   /* POINT pt;
    GetCursorPos(&pt);
    fraLeftBar->miProperties->Enabled = false;
    if (Scene->SelectionCount( true, cls )) fraLeftBar->miProperties->Enabled = true;
    RedrawScene(true);
    fraLeftBar->pmObjectContext->TrackButton = tbRightButton;
    fraLeftBar->pmObjectContext->Popup(pt.x,pt.y);*/
}






// Common

void CLevelMain::ResetStatus()
{
	VERIFY(m_bReady);
  /*  if (fraBottomBar->paStatus->Caption!=""){
	    fraBottomBar->paStatus->Caption=""; fraBottomBar->paStatus->Repaint();
    }*/
}
void CLevelMain::SetStatus(LPCSTR s, bool bOutLog)
{
	VERIFY(m_bReady);
  /*  if (fraBottomBar->paStatus->Caption!=s){
	    fraBottomBar->paStatus->Caption=s; fraBottomBar->paStatus->Repaint();
    	if (bOutLog&&s&&s[0]) ELog.Msg(mtInformation,s);
    }*/
}
void CLevelMain::ProgressDraw()
{
    inherited::ProgressDraw();
	//fraBottomBar->RedrawBar();
}

void CLevelMain::OutCameraPos()
{
	if (m_bReady){
        xr_string s;
        const Fvector& c 	= EDevice->m_Camera.GetPosition();
        s.sprintf("C: %3.1f, %3.1f, %3.1f",c.x,c.y,c.z);
    //	const Fvector& hpb 	= EDevice->m_Camera.GetHPB();
    //	s.sprintf(" Cam: %3.1f�, %3.1f�, %3.1f�",rad2deg(hpb.y),rad2deg(hpb.x),rad2deg(hpb.z));
       // fraBottomBar->paCamera->Caption=s; fraBottomBar->paCamera->Repaint();

    }
}

void CLevelMain::OutUICursorPos()
{
	/*VERIFY(fraBottomBar);
    xr_string s; POINT pt;
    GetCursorPos(&pt);
    s.sprintf("Cur: %d, %d",pt.x,pt.y);
    fraBottomBar->paUICursor->Caption=s; fraBottomBar->paUICursor->Repaint();*/
}

void CLevelMain::OutGridSize()
{
	/*VERIFY(fraBottomBar);
    xr_string s;
    s.sprintf("Grid: %1.1f",EPrefs->grid_cell_size);
    fraBottomBar->paGridSquareSize->Caption=s; fraBottomBar->paGridSquareSize->Repaint();*/
}

void CLevelMain::OutInfo()
{
	//fraBottomBar->paSel->Caption = Tools->GetInfo();
}

void CLevelMain::RealQuit()
{
	//frmMain->Close();
}


void CLevelMain::SaveSettings(CInifile* I)
{
	inherited::SaveSettings(I);
    SSceneSummary::Save(I);
}
void CLevelMain::LoadSettings(CInifile* I)
{
	inherited::LoadSettings(I);
    SSceneSummary::Load(I);
}

Ivector2 CLevelMain::GetRenderMousePosition() const
{
    return MainForm->GetRenderForm()->GetMousePos();
}

void CLevelMain::OnDrawUI()
{
    inherited::OnDrawUI();
    // floating panels stay hidden while the project browser or the blocking
    // Link Game page owns the screen
    if (EditorProject::Active() && EditorProject::GameLinked())
    {
        UIObjectList::Update();
        UIContentBrowser::Update();
        UIWorldOutliner::Update();
        UIVisualPreview::Update();
        EditorProject::DrawUI();
        EditorMod::DrawUI();
        EditorModScene::DrawUI();
    }
    if (LTools->GetToolForm())
    {
        LTools->GetToolForm()->OnDrawUI();
    }
}

bool CLevelMain::KeyDown(WORD Key, TShiftState Shift)
{
    return TUI::KeyDown(Key, Shift);
}

void CLevelMain::OnStats(CGameFont* font)
{
    float Height = font->GetHeight();
    font->SetColor(color_rgba(255, 0, 0, 255));
    font->SetHeight(14);
    if (!Scene->m_RTFlags.is(EScene::flIsBuildedCForm))
    {
		font->OutNext("NEED REBUILD CFORM");
    }
	if (!Scene->m_RTFlags.is(EScene::flIsBuildedAIMap))
	{
        font->OutNext("NEED REBUILD AIMAP");
	}
	if (!Scene->m_RTFlags.is(EScene::flIsBuildedGameGraph))
	{
        font->OutNext("NEED REBUILD GAME GRAPH");
	}


	font->SetHeight(Height);
}




bool CLevelMain::IsPlayInEditor()
{
	return Scene->IsPlayInEditor();
}
