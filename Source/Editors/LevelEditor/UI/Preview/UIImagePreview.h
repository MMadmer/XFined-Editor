#pragma once

// Texture viewer: the double-click destination for image assets, next to the
// model preview window. It owns the shader resource view it shows, so closing
// the window (or opening another texture) frees it - nothing stays resident
// behind the user's back.

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

	ImTextureID		m_Tex;
	xr_string		m_Name;
	xr_string		m_Error;
	u32				m_W, m_H;
	float			m_Zoom;			// 0 = fit to the window
	bool			m_Alpha;		// blend against the checkerboard
	bool			m_Focus;

	void			Release				();
	void			DrawToolBar			();
	void			DrawImage			();
};
