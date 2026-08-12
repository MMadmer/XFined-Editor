#pragma once

// Texture viewer: the double-click destination for image assets, next to the
// model preview window. Modelled on Unreal's Texture Editor - the picture plus
// the facts you need about it: format, size, mips (switchable), alpha, sRGB,
// what it costs on the GPU and on disk, and where it came from.
//
// It owns the resources it shows, so closing the window (or opening another
// texture) frees them - nothing stays resident behind the user's back.

class UIImagePreview : public XrUI
{
public:
					UIImagePreview		();
	virtual			~UIImagePreview		();

	// Opens/focuses the window on `name`. `source` is the content browser's:
	// 0 project (a path under the project root), 1 the shared SDK library (an
	// FS-resolvable texture name), 2 the linked game install (may live inside an
	// archive). Returns false and fills `err` when the image cannot be decoded.
	static bool		Show				(LPCSTR name, int source, xr_string* err);
	static void		Close				();
	static IC bool	IsOpen				() { return !!Form; }
	static void		Update				();		// draws if open
	virtual void	Draw				();

	// the label carries the texture name, so the imgui id is pinned with "###"
	static LPCSTR	WindowID			() { return "###ImagePreview"; }

private:
	static UIImagePreview*	Form;

	// the decoded texture and the view of the mip currently on screen. Both are
	// only ever swapped OUTSIDE this window's own draw (double-click runs in the
	// browser's draw, which precedes this window; MCP runs between frames; the
	// mip slider changes before DrawImage records anything) - so the old view is
	// never sitting in an unrendered ImGui draw list when it is released.
	ID3D11Texture2D*		m_Tex;
	ID3D11ShaderResourceView* m_View;
	D3D11_TEXTURE2D_DESC	m_Desc;

	xr_string		m_Name;
	xr_string		m_File;			// where it came from, or "<linked game install>"
	u32				m_FileSize;
	xr_string		m_Error;

	u32				m_Mip;			// which mip the view shows
	float			m_Zoom;			// 0 = fit to the window
	bool			m_Alpha;		// blend against the checkerboard
	int				m_Channels;		// 0 RGB, 1 R, 2 G, 3 B
	bool			m_Focus;

	void			Release				();
	bool			Adopt				(ID3D11ShaderResourceView* srv);
	bool			ViewMip				(u32 mip);
	void			DrawToolBar			();
	void			DrawInfo			();
	void			DrawImage			();
	// size of the mip currently displayed
	IC u32			MipW				() const { u32 w = m_Desc.Width  >> m_Mip; return w ? w : 1; }
	IC u32			MipH				() const { u32 h = m_Desc.Height >> m_Mip; return h ? h : 1; }
};
