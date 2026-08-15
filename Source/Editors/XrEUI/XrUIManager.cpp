#include "stdafx.h"
#if defined(USE_DX11)
#include "imgui_impl_dx11.h"
#else
#include "imgui_impl_dx9.h"
#endif
#include "imgui_impl_win32.h"
#include "imgui_internal.h"	// DockBuilder* for the default layout
#include "spectrum.h"


XrUIManager::XrUIManager()
{
}

XrUIManager::~XrUIManager()
{
}

inline void Style()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    /// 0 = FLAT APPEARENCE
    /// 1 = MORE "3D" LOOK
    int is3D = 0;

    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.12f, 0.12f, 0.12f, 0.71f);
    colors[ImGuiCol_BorderShadow] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.42f, 0.42f, 0.42f, 0.54f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.42f, 0.42f, 0.42f, 0.40f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.67f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.17f, 0.17f, 0.17f, 0.90f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.335f, 0.335f, 0.335f, 1.000f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.24f, 0.24f, 0.24f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.64f, 0.64f, 0.64f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.54f, 0.54f, 0.54f, 0.35f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.52f, 0.52f, 0.52f, 0.59f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.47f, 0.47f, 0.47f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.76f, 0.76f, 0.76f, 0.77f);
    colors[ImGuiCol_Separator] = ImVec4(0.000f, 0.000f, 0.000f, 0.137f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.700f, 0.671f, 0.600f, 0.290f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.702f, 0.671f, 0.600f, 0.674f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.73f, 0.73f, 0.73f, 0.35f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);

    style.PopupRounding = 3;

    style.WindowPadding = ImVec2(4, 4);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(6, 2);

    style.ScrollbarSize = 18;

    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = is3D;

    style.WindowRounding = 3;
    style.ChildRounding = 3;
    style.FrameRounding = 3;
    style.ScrollbarRounding = 2;
    style.GrabRounding = 3;

#ifdef IMGUI_HAS_DOCK 
    style.TabBorderSize = is3D;
    style.TabRounding = 3;

    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.85f, 0.85f, 0.85f, 0.28f);

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
#endif
}
#if defined(USE_DX11)
void XrUIManager::Initialize(HWND hWnd, ID3D11Device* device, ID3D11DeviceContext* context, const char* ini_path)
#else
void XrUIManager::Initialize(HWND hWnd, IDirect3DDevice9* device, const char* ini_path)
#endif
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    xr_strcpy(m_name_ini, ini_path);
    io.IniFilename = m_name_ini;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
  //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
    // Setup Dear ImGui style
    ImGui::StyleColorsClassic();
    // Bake the monitor DPI into the font atlas and widget metrics so the UI
    // stays crisp and keeps its visual size on scaled displays
    const float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hWnd);
    ImGui::Spectrum::LoadFont(16.f * dpi_scale);
    Style();

    ImGui::GetStyle().ItemSpacing = ImVec2(3, 3);
    ImGui::GetStyle().FramePadding = ImVec2(3, 1);
    if (dpi_scale != 1.f)
        ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    ImGui_ImplWin32_Init(hWnd);
    #if defined(USE_DX11)
    ImGui_ImplDX11_Init(device, context);
#else
    ImGui_ImplDX9_Init(device);
#endif

}

void XrUIManager::Destroy()
{
    #if defined(USE_DX11)
    ImGui_ImplDX11_Shutdown();
#else
    ImGui_ImplDX9_Shutdown();
#endif
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void XrUIManager::ResetBegin()
{
    #if defined(USE_DX11)
    ImGui_ImplDX11_InvalidateDeviceObjects();
#else
    ImGui_ImplDX9_InvalidateDeviceObjects();
#endif

}

void XrUIManager::ResetEnd()
{
    #if defined(USE_DX11)
    ImGui_ImplDX11_CreateDeviceObjects();
#else
    ImGui_ImplDX9_CreateDeviceObjects();
#endif
}


void XrUIManager::OnDrawUI()
{
}

void XrUIManager::ApplyShortCut(DWORD Key)
{
    if ((ImGui::GetIO().WantTextInput))return;
	bool IsFail = true;
	if (Key >= 'A' && Key <= 'Z')
	{
		IsFail = false;
	}
	else if (Key >= '0' && Key <= '9')
	{
		IsFail = false;
	}
    else
    {
        switch (Key)
        {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_NUMPAD0:
        case VK_NUMPAD1:
        case VK_NUMPAD2:
        case VK_NUMPAD3:
        case VK_NUMPAD4:
        case VK_NUMPAD5:
        case VK_NUMPAD6:
        case VK_NUMPAD7:
        case VK_NUMPAD8:
        case VK_NUMPAD9:
        case VK_F1:
        case VK_F2:
        case VK_F3:
        case VK_F4:
        case VK_F5:
        case VK_F6:
        case VK_F7:
        case VK_F8:
        case VK_F9:
        case VK_F10:
        case VK_F11:
        case VK_F12:
        case VK_DELETE:
        case VK_ADD:
        case VK_SUBTRACT:
        case VK_MULTIPLY:
        case VK_DIVIDE:
        case VK_OEM_PLUS:
        case VK_OEM_MINUS:
        case VK_OEM_1:
        case VK_OEM_COMMA:
        case VK_OEM_PERIOD:
        case VK_OEM_2:
        case VK_OEM_4:
        case VK_OEM_5:
        case VK_OEM_6:
        case VK_OEM_7:
        case VK_SPACE:
        case VK_CANCEL:
        case VK_RETURN:
        case VK_ESCAPE:
        // navigation block: End drops the selection to the ground, the rest
        // are free for user bindings
        case VK_END:
        case VK_HOME:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_INSERT:
            IsFail = false;
            break;
        default:
            break;
        }
    }
    if (IsFail)return;

    int ShiftState = ssNone;

    if (ImGui::GetIO().KeyShift)ShiftState |= ssShift;
    if (ImGui::GetIO().KeyCtrl)ShiftState |= ssCtrl;
    if (ImGui::GetIO().KeyAlt)ShiftState |= ssAlt;


    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))ShiftState |= ssLeft;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right))ShiftState |= ssRight;
    ApplyShortCut(Key, ShiftState);
}



void XrUIManager::Push(XrUI* ui, bool need_deleted)
{
	m_UIArray.push_back(ui);
	ui->Flags.set(!need_deleted, XrUI::F_NoDelete);
}

void XrUIManager::DockLayoutBegin(unsigned int dockspace_id)
{
	const ImGuiID dockspace = (ImGuiID)dockspace_id;
	ImGui::DockBuilderRemoveNode(dockspace);
	ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);

	// order matters: the right column is taken from the root first, so it keeps
	// the full height and the bottom strip below cannot run under it
	ImGuiID centre = dockspace;
	ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, NULL, &centre);
	ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.28f, NULL, &centre);
	ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.18f, NULL, &centre);
	ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, NULL, &right);

	// DockBuilderFinish must run on the dockspace id itself: the splits above
	// reassign 'centre' to the leftover child, and finishing on that leaves the
	// tree half-built (every window ends up undocked and the UI renders empty)
	m_DockRoot = (unsigned int)dockspace;
	m_DockNodes[DockCenter] = (unsigned int)centre;
	m_DockNodes[DockLeft] = (unsigned int)left;
	m_DockNodes[DockRight] = (unsigned int)right;
	m_DockNodes[DockRightBottom] = (unsigned int)right_bottom;
	m_DockNodes[DockBottom] = (unsigned int)bottom;
}

void XrUIManager::DockLayoutPlace(const char* window_name, EDockSlot slot)
{
	if (!m_DockRoot || !window_name) return;
	ImGui::DockBuilderDockWindow(window_name, (ImGuiID)m_DockNodes[slot]);
}

void XrUIManager::DockNextWindowWith(const char* next_to)
{
	if (!next_to) return;
	ImGuiWindow* w = ImGui::FindWindowByName(next_to);
	if (w && w->DockId) ImGui::SetNextWindowDockID(w->DockId, ImGuiCond_FirstUseEver);
}

void XrUIManager::DockLayoutEnd()
{
	if (!m_DockRoot) return;
	ImGui::DockBuilderFinish((ImGuiID)m_DockRoot);
	m_DockRoot = 0;
}

void XrUIManager::Draw()
{
    // guards the whole frame, so a panel that asks for a forced redraw while
    // drawing gets the deferred one instead of a nested (fatal) ImGui frame
    m_InUIPass = true;

    #if defined(USE_DX11)
    ImGui_ImplDX11_NewFrame();
#else
    ImGui_ImplDX9_NewFrame();
#endif
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
   // ImGui::DockSpaceOverViewport();
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos( ImVec2(viewport->Pos.x, viewport->Pos.y+ UIToolBarSize));
        ImGui::SetNextWindowSize( ImVec2(viewport->Size.x, viewport->Size.y- UIToolBarSize));
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGuiWindowFlags window_flags = 0
            | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 2));
        ImGui::Begin("Master DockSpace", NULL, window_flags);
        ImGuiID dockMain = ImGui::GetID("MyDockspace");

        m_MenuBarHeight = ImGui::GetWindowBarHeight();
        // Save off menu bar height for later.

        // no layout yet (fresh install) or an explicit reset: lay the panels out
        // before DockSpace() consumes the id
        if (m_ResetDockLayout || ImGui::DockBuilderGetNode(dockMain) == NULL)
        {
            m_ResetDockLayout = false;
            BuildDefaultDockLayout(dockMain);
        }

        ImGui::DockSpace(dockMain);
        ImGui::End();
        ImGui::PopStyleVar(4);

    }
	for (XrUI* ui : m_UIArray)
	{
		ui->Draw();
	}

    OnDrawUI();
    ImGui::EndFrame();
    ImGui::Render();
    #if defined(USE_DX11)
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#else
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
#endif
    // cleared here rather than at the end: the pruning below makes no ImGui
    // calls and can return early
    m_InUIPass = false;

	for (size_t i = m_UIArray.size(); i > 0; i--)
	{
		if (m_UIArray[i - 1]->IsClosed())
		{
			if (!m_UIArray[i - 1]->Flags.test(XrUI::F_NoDelete))
			{
				xr_delete(m_UIArray[i - 1]);
			}
			m_UIArray.erase(m_UIArray.begin() + (i - 1));
			i = m_UIArray.size();
			if (i == 0)return;
		}
	}
}
static bool ImGui_ImplWin32_UpdateMouseCursor()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
        return false;

    ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
    if (imgui_cursor == ImGuiMouseCursor_None || io.MouseDrawCursor)
    {
        // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
        ::SetCursor(NULL);
    }
    else
    {
        // Show OS mouse cursor
        LPTSTR win32_cursor = IDC_ARROW;
        switch (imgui_cursor)
        {
        case ImGuiMouseCursor_Arrow:        win32_cursor = IDC_ARROW; break;
        case ImGuiMouseCursor_TextInput:    win32_cursor = IDC_IBEAM; break;
        case ImGuiMouseCursor_ResizeAll:    win32_cursor = IDC_SIZEALL; break;
        case ImGuiMouseCursor_ResizeEW:     win32_cursor = IDC_SIZEWE; break;
        case ImGuiMouseCursor_ResizeNS:     win32_cursor = IDC_SIZENS; break;
        case ImGuiMouseCursor_ResizeNESW:   win32_cursor = IDC_SIZENESW; break;
        case ImGuiMouseCursor_ResizeNWSE:   win32_cursor = IDC_SIZENWSE; break;
        case ImGuiMouseCursor_Hand:         win32_cursor = IDC_HAND; break;
        case ImGuiMouseCursor_NotAllowed:   win32_cursor = IDC_NO; break;
        }
        ::SetCursor(::LoadCursor(NULL, win32_cursor));
    }
    return true;
}
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef DBT_DEVNODES_CHANGED
#define DBT_DEVNODES_CHANGED 0x0007
#endif
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT XrUIManager::WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        switch (wParam)
        {
        case VK_MENU:
        case VK_CONTROL:
        case VK_SHIFT:
            break;
        default:
            if(!IsPlayInEditor() && !IsViewportNavigating())   ApplyShortCut(wParam);
            break;
        }
    default:
        break;
    }
    if (!IsPlayInEditor())
        return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);;
    return 0;
}