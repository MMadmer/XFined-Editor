#pragma once

// Windows resource ids shared by the editor executables and the code that loads
// them. The application icon deliberately carries the LOWEST id: the shell picks
// the lowest-numbered ICON of an executable as its file icon, so a resource
// added later can never take the taskbar and Explorer away from the logo.
#define IDI_XFINED_EDITOR	1
