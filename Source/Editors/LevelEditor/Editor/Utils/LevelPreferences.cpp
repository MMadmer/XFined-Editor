#include "stdafx.h"

void CLevelPreferences::Load(CInifile* I)
{
	inherited::Load		(I);        
    {
        OpenObjectList = R_BOOL_SAFE("windows", "object_list", false);
     
    }
    {
        OpenProperties = R_BOOL_SAFE("windows", "properties", true);
      
    }
	{
		OpenWorldProperties = R_BOOL_SAFE("windows", "world_properties", true);

	}
    {
        OpenContentBrowser = R_BOOL_SAFE("windows", "content_browser", true);
    }
    {
        OpenWorldOutliner = R_BOOL_SAFE("windows", "world_outliner", true);
    }
    {
        ContentBrowserTreeWidth = R_U32_SAFE("windows", "content_browser_tree_width", 220);
    }
    {
        QuestInspectorSplit = R_U32_SAFE("windows", "quest_inspector_split", 320);
        QuestVarsSplit = R_U32_SAFE("windows", "quest_vars_split", 300);
    }
    {
        // an empty value reads back as a null pointer, not as "" - assigning that
        // to a string is what made the editor die on startup.
        // r_string_wb, not r_string: adapter names contain spaces, and the ini
        // parser eats spaces in unquoted values - the value is stored quoted,
        // same as recent_files.
        shared_str gpu = I->line_exist("render", "gpu_adapter") ? I->r_string_wb("render", "gpu_adapter") : shared_str("");
        GpuAdapter = gpu.size() ? gpu.c_str() : "";
        // hand it to the render layer before the device is created
        SetPreferredGpu(GpuAdapter.c_str());
    }
    SceneToolsMapPairIt _I 	= Scene->FirstTool();
    SceneToolsMapPairIt _E 	= Scene->LastTool();
    for (; _I!=_E; _I++)
        if (_I->second&&(_I->first!=OBJCLASS_DUMMY))
        	_I->second->m_EditFlags.flags = R_U32_SAFE("targets",_I->second->ClassName(),_I->second->m_EditFlags.flags);
}

void CLevelPreferences::Save(CInifile* I)
{
	inherited::Save		(I);
    I->w_bool("windows", "object_list", OpenObjectList);
	I->w_bool("windows", "properties", OpenProperties);
	I->w_bool("windows", "world_properties", OpenWorldProperties);
    I->w_bool("windows", "content_browser", OpenContentBrowser);
    I->w_bool("windows", "world_outliner", OpenWorldOutliner);
    I->w_u32("windows", "content_browser_tree_width", ContentBrowserTreeWidth);
    I->w_u32("windows", "quest_inspector_split", QuestInspectorSplit);
    I->w_u32("windows", "quest_vars_split", QuestVarsSplit);
    // only write a real choice: a key with an empty value is what the reader
    // above chokes on, and "no key" already means "system default".
    // quoted on disk - the ini parser eats spaces in unquoted values, and
    // adapter names are full of them
    if (!GpuAdapter.empty())
    {
        xr_string quoted; quoted.sprintf("\"%s\"", GpuAdapter.c_str());
        I->w_string("render", "gpu_adapter", quoted.c_str());
    }
    SceneToolsMapPairIt _I 	= Scene->FirstTool();
    SceneToolsMapPairIt _E 	= Scene->LastTool();
    for (; _I!=_E; _I++)
        if (_I->second&&(_I->first!=OBJCLASS_DUMMY))	I->w_u32	("targets",_I->second->ClassName(),_I->second->m_EditFlags.get());
}

void CLevelPreferences::OnEnabledChange(PropValue* prop)
{
	ESceneToolBase* M		= Scene->GetTool(prop->tag); VERIFY(M);
	ExecCommand				(COMMAND_ENABLE_TARGET,prop->tag,M->IsEnabled());
}

void CLevelPreferences::OnReadonlyChange(PropValue* prop)
{
	ESceneToolBase* M		= Scene->GetTool(prop->tag); VERIFY(M);
	ExecCommand				(COMMAND_READONLY_TARGET,prop->tag,M->IsForceReadonly());
}

void CLevelPreferences::FillProp(PropItemVec& items)
{
	inherited::FillProp	(items);
    SceneToolsMapPairIt _I 	= Scene->FirstTool();
    SceneToolsMapPairIt _E 	= Scene->LastTool();
    for (; _I!=_E; _I++)
        if (_I->second&&(_I->first!=OBJCLASS_DUMMY)){
        	if (_I->second->AllowEnabling()){
                PropValue* V 	= PHelper().CreateFlag32(items,PrepareKey("Scene\\Targets\\Enable",_I->second->ClassDesc()),	&_I->second->m_EditFlags, ESceneToolBase::flEnable);
                V->tag			= _I->second->FClassID;
                V->OnChangeEvent.bind(this,&CLevelPreferences::OnEnabledChange);
            }
		    PropValue* V		= PHelper().CreateFlag32(items,PrepareKey("Scene\\Targets\\Read Only",_I->second->ClassDesc()),	&_I->second->m_EditFlags, ESceneToolBase::flForceReadonly);
            V->tag				= _I->second->FClassID;
            V->OnChangeEvent.bind(this,&CLevelPreferences::OnReadonlyChange);
        }
}

