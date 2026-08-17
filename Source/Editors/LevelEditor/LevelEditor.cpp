// LevelEditor.cpp : Определяет точку входа для приложения.
//
#include "stdafx.h"
#include "..\..\XrAPI\xrGameManager.h"
#include "Engine/XrGameManager.h"
#include "..\XrEngine\std_classes.h"
#include "..\XrEngine\IGame_Persistent.h"
#include "..\XrEngine\XR_IOConsole.h"
#include "..\XrEngine\IGame_Level.h"
#include "..\XrEngine\x_ray.h"
#include "Engine/XRayEditor.h"
#include "..\XrECore\Editor\EditorProject.h"
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    if (!IsDebuggerPresent()) Debug._initialize(false);
    const char* FSName = "fs.ltx";
    bool bDefaultFS = true;
    {
        if (strstr(GetCommandLine(), "-soc_14") || strstr(GetCommandLine(), "-soc_10004"))
        {
            FSName = "fs_soc.ltx";
            bDefaultFS = false;
        }
        else if (strstr(GetCommandLine(), "-soc"))
        {
            FSName = "fs_soc.ltx";
            bDefaultFS = false;
        }
        else if (strstr(GetCommandLine(), "-cs"))
        {
            FSName = "fs_cs.ltx";
            bDefaultFS = false;
        }
    }
    // project selection is now an in-editor browser page (see EditorProject):
    // the FS boots on the plain SDK config and gets remounted at open time
    (void)bDefaultFS;
    Core._initialize("LevelEditor", ELogCallback, 1, FSName, true);
    Tools = xr_new<CLevelTool>();
    LTools = (CLevelTool*)Tools;

    UI = xr_new<CLevelMain>();
    UI->RegisterCommands();
    LUI = (CLevelMain*)UI;

    Scene = xr_new<EScene>();
    EditorScene = Scene;
    EditorProject::SetLiveSceneQuery([]
    {
        return Scene && (Scene->ObjCount() || Scene->IsUnsaved() || Scene->IsModified());
    });
    UIMainForm* MainForm = xr_new< UIMainForm>();
    pApp = xr_new<XRayEditor>();
    g_XrGameManager = xr_new<XrGameManager>();
    g_SEFactoryManager = xr_new<XrSEFactoryManager>();
    /*
    
           */
    g_pGamePersistent = (IGame_Persistent*)g_XrGameManager->Create(CLSID_GAME_PERSISTANT);
    EDevice->seqAppStart.Process(rp_AppStart);
    Console->Execute("default_controls");
    Console->Hide();
    ::MainForm = MainForm;
    UI->Push(MainForm, false);
    while (MainForm->Frame())
    {
    }
	xr_delete(MainForm);
    EditorProject::SetLiveSceneQuery(nullptr);
	xr_delete(pApp);
	xr_delete(g_XrGameManager);
	xr_delete(g_SEFactoryManager);

    Core._destroy();
    return 0;
}
