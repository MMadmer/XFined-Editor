#pragma once
enum TShiftState_
{
	ssNone = 0,
	ssShift = 1,
	ssLeft = 2,
	ssRight = 4,
	ssCtrl = 8,
	ssAlt = 16,
	ssMiddle = 32,
};
using TShiftState = int;
constexpr int UIToolBarSize = 24;
class XREUI_API XrUIManager
{
public:
	XrUIManager();
	void Push(XrUI*ui,bool need_deleted =true);
	void Draw();
		
	virtual ~XrUIManager();

	LRESULT WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void Initialize(HWND hWnd, IDirect3DDevice9* device,const char*ini_path);
	void Destroy();

	void ResetBegin();
	void ResetEnd();
	virtual bool 	ApplyShortCut(DWORD Key, TShiftState Shift)=0;

	inline float GetMenuBarHeight()const { return m_MenuBarHeight; }
	inline TShiftState GetShiftState()const { return m_ShiftState; };
	virtual bool IsPlayInEditor() { return false; }
	// true while the viewport camera owns the mouse (UE-style fly/pan) —
	// keyboard shortcuts must not fire from WASD etc. during navigation
	virtual bool IsViewportNavigating() { return false; }

	// Rebuilds the editor's default panel arrangement on the next frame
	// (Windows > Reset Layout).
	void RequestDockLayoutReset() { m_ResetDockLayout = true; }

	// Where a window goes in the default layout. The right column is cut first
	// so it spans the full height; the bottom strip therefore stops at it,
	// exactly like Unreal's content drawer.
	enum EDockSlot
	{
		DockCenter,			// viewport
		DockLeft,			// narrow column left of the viewport
		DockRight,			// full-height right column, upper half
		DockRightBottom,	// full-height right column, lower half
		DockBottom,			// strip under the viewport only
	};
	// Splits the dockspace and remembers the node ids. Editors call these from
	// BuildDefaultDockLayout - the imgui docking internals stay in XrEUI,
	// because imgui_internal.h cannot be included next to imgui_user.h.
	void DockLayoutBegin(unsigned int dockspace_id);
	void DockLayoutPlace(const char* window_name, EDockSlot slot);
	void DockLayoutEnd();
protected:
	virtual void OnDrawUI();

	// Default dock layout. Built when the dockspace has no layout yet (fresh
	// install, no imgui ini) or when the user asks for a reset. The base does
	// nothing - each editor knows its own window names.
	virtual void BuildDefaultDockLayout(unsigned int /*dockspace_id*/) {}
private:
	unsigned int m_DockNodes[5]{};
	unsigned int m_DockRoot{0};
	bool m_ResetDockLayout{false};
	float m_MenuBarHeight;
	void ApplyShortCut(DWORD Key);
	TShiftState m_ShiftState;
	xr_vector<XrUI*> m_UIArray;
	string_path m_name_ini;
};

