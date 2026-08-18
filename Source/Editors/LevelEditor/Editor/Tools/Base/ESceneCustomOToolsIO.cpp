#include "stdafx.h"

// chunks

static const u32 CHUNK_VERSION			= 0x0001;
static const u32 CHUNK_OBJECT_COUNT		= 0x0002;
static const u32 CHUNK_OBJECTS			= 0x0003;
static const u32 CHUNK_FLAGS			= 0x0004;


bool ESceneCustomOTool::OnLoadSelectionAppendObject(CCustomObject* obj)
{
    string256 				buf;
    Scene->GenObjectName	(obj->FClassID,buf,obj->GetName());
    obj->SetName(buf);
    Scene->AppendObject		(obj, false);
    return					true;
}


bool ESceneCustomOTool::OnLoadAppendObject(CCustomObject* O)
{
	Scene->AppendObject	(O,false);
    return true;
}


bool ESceneCustomOTool::LoadSelection(IReader& F)
{
    int count					= 0;
	if (F.find_chunk(CHUNK_OBJECT_COUNT) != sizeof(count))
        return false;
	F.r						(&count, sizeof(count));
    if (count < 0 || (count && !F.find_chunk(CHUNK_OBJECTS)))
        return false;

    // Empty tools are common, and opening a console for them costs more than the load.
    SPBItem* pb 				= count ? UI->ProgressStart(count,xr_string().sprintf("Loading %s(stream)...",ClassDesc()).c_str()) : nullptr;
    u32 decodedCount = 0;
    const bool loaded = Scene->ReadObjectsStream(F, CHUNK_OBJECTS,
        EScene::TAppendObject(this, &ESceneCustomOTool::OnLoadSelectionAppendObject), pb, &decodedCount);
    if (pb) UI->ProgressEnd	(pb);

    return loaded && decodedCount == static_cast<u32>(count);
}


void ESceneCustomOTool::SaveSelection(IWriter& F)
{
	F.open_chunk	(CHUNK_OBJECTS);
    int count		= 0;
    for(ObjectIt it = m_Objects.begin();it!=m_Objects.end();++it)
    {
    	if ((*it)->Selected() && !(*it)->IsDeleted())
        {
	        F.open_chunk(count++);
    	    Scene->SaveObjectStream(*it,F);
        	F.close_chunk();
        }
    }
	F.close_chunk	();

	F.w_chunk		(CHUNK_OBJECT_COUNT,&count,sizeof(count));
}

bool ESceneCustomOTool::LoadLTX(CInifile& ini)
{
	if (!inherited::LoadLTX(ini))
    {
        ELog.Msg(mtError, "%s tools: required metadata is missing", ClassDesc());
        return false;
    }

    if (!ini.line_exist("main", "objects_count"))
    {
        ELog.Msg(mtError, "%s tools: required objects_count is missing", ClassDesc());
        return false;
    }

    u32 count			= ini.r_u32("main", "objects_count");

	SPBItem* pb 		= count ? UI->ProgressStart(count,xr_string().sprintf("Loading %s(ltx)...",ClassDesc()).c_str()) : nullptr;

    string128			buff;
    bool loaded = true;

	for (u32 i = 0; i < count; ++i)
    {
        CCustomObject* obj = nullptr;
        sprintf(buff, "object_%d", i);
        if (!ini.section_exist(buff))
        {
            ELog.Msg(mtError, "%s tools: required section '%s' is missing", ClassDesc(), buff);
            loaded = false;
        }
        else if (!Scene->ReadObjectLTX(ini, buff, obj))
        {
            ELog.Msg(mtError, "%s tools: object section '%s' is invalid", ClassDesc(), buff);
            loaded = false;
        }
        else if (!OnLoadAppendObject(obj))
        {
            ELog.Msg(mtError, "%s tools: object section '%s' was rejected", ClassDesc(), buff);
            xr_delete(obj);
            loaded = false;
        }

        if (pb) pb->Inc();
		UI->ProgressCheckpoint();
        if (!loaded)
            break;
    }

	if (pb) UI->ProgressEnd(pb);

    return loaded;
}

bool ESceneCustomOTool::LoadStream(IReader& F)
{
	if (!inherited::LoadStream(F))
        return false;

    int count					= 0;
	if (F.find_chunk(CHUNK_OBJECT_COUNT) != sizeof(count))
        return false;
	F.r						(&count, sizeof(count));
    if (count < 0 || (count && !F.find_chunk(CHUNK_OBJECTS)))
        return false;

    SPBItem* pb 				= count ? UI->ProgressStart(count,xr_string().sprintf("Loading %s...",ClassDesc()).c_str()) : nullptr;
    u32 decodedCount = 0;
    const bool loaded = Scene->ReadObjectsStream(F, CHUNK_OBJECTS,
        EScene::TAppendObject(this, &ESceneCustomOTool::OnLoadAppendObject), pb, &decodedCount);
    if (pb) UI->ProgressEnd	(pb);

    return loaded && (!count || decodedCount == static_cast<u32>(count));
}


void ESceneCustomOTool::SaveLTX(CInifile& ini, int id)
{
	inherited::SaveLTX	(ini, id);

	u32 count			= 0;
    for(ObjectIt it=m_Objects.begin(); it!=m_Objects.end(); ++it)
	{
    	CCustomObject* O = (*it);
        if(O->save_id!=id)
        	continue;
            
    	if (O->IsDeleted() || O->m_CO_Flags.test(CCustomObject::flObjectInGroup) )
        	continue;
            
        string128				buff;
        sprintf					(buff,"object_%d",count);
        Scene->SaveObjectLTX	(*it,  buff, ini);
        count++;
	}

	ini.w_u32			("main", "objects_count", count);
}

void ESceneCustomOTool::SaveStream(IWriter& F)
{
	inherited::SaveStream	(F);

	F.open_chunk		(CHUNK_OBJECTS);
    int count			= 0;
    for(ObjectIt it = m_Objects.begin();it!=m_Objects.end();++it)
	{
    	if ( (*it)->IsDeleted() || (*it)->m_CO_Flags.test(CCustomObject::flObjectInGroup) )
        continue;

        F.open_chunk			(count++);
        Scene->SaveObjectStream	(*it,F);
        F.close_chunk			();
	}
	F.close_chunk	();

	F.w_chunk		(CHUNK_OBJECT_COUNT, &count, sizeof(count));
}


bool ESceneCustomOTool::Export(LPCSTR path)
{
	return true;
}

 
bool ESceneCustomOTool::ExportGame(SExportStreams* F)
{
	bool bres=true;
    for(ObjectIt it = m_Objects.begin();it!=m_Objects.end();it++)
        if (!(*it)->ExportGame(F)) bres=false;
	return bres;
}


bool ESceneCustomOTool::ExportStatic(SceneBuilder* B, bool b_selected_only)
{
	return B->ParseStaticObjects(m_Objects, NULL, b_selected_only);
}
 BOOL GetStaticCformData   ( ObjectList& lst, mesh_build_data &data, bool b_selected_only );
bool ESceneCustomOTool::GetStaticCformData( mesh_build_data &data, bool b_selected_only ) //b_vertex* verts, int& vert_cnt, int& vert_it,b_face* faces, int& face_cnt, int& face_it,
{
      return    ::GetStaticCformData(  m_Objects, data, b_selected_only );
}

 
