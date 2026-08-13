#ifndef xrDebugH
#define xrDebugH
#pragma once

typedef	void		crashhandler		(void);
typedef	void		on_dialog			(bool before);

class XRCORE_API	xrDebug
{
private:
	crashhandler*	handler	;
	on_dialog*		m_on_dialog;

public:
	void			_initialize			(const bool &dedicated);
	void			_destroy			();
	
public:
	crashhandler*	get_crashhandler	()							{ return handler;	};
	void			set_crashhandler	(crashhandler* _handler)	{ handler=_handler;	};

	on_dialog*		get_on_dialog		()							{ return m_on_dialog;	}
	void			set_on_dialog		(on_dialog* on_dialog)		{ m_on_dialog = on_dialog;	}

	LPCSTR			error2string		(long  code	);

	void			gather_info			(const char *expression, const char *description, const char *argument0, const char *argument1, const char *file, int line, const char *function, LPSTR assertion_info, unsigned int assertion_info_size);
	template <int count>
	inline void		gather_info			(const char *expression, const char *description, const char *argument0, const char *argument1, const char *file, int line, const char *function, char (&assertion_info)[count])
	{
		gather_info	( expression, description, argument0, argument1, file, line, function, assertion_info, count);
	}

	void			fail				(const char *e1, const char *file, int line, const char *function, bool &ignore_always);
	void			fail				(const char *e1, const std::string &e2, const char *file, int line, const char *function, bool &ignore_always);
	void			fail				(const char *e1, const char *e2, const char *file, int line, const char *function, bool &ignore_always);
	void			fail				(const char *e1, const char *e2, const char *e3, const char *file, int line, const char *function, bool &ignore_always);
	void			fail				(const char *e1, const char *e2, const char *e3, const char *e4, const char *file, int line, const char *function, bool &ignore_always);
	void			error				(long  code, const char* e1, const char *file, int line, const char *function, bool &ignore_always);
	void			error				(long  code, const char* e1, const char* e2, const char *file, int line, const char *function, bool &ignore_always);
	void _cdecl		fatal				(const char *file, int line, const char *function, const char* F,...);
	void			backend				(const char* reason, const char* expression, const char *argument0, const char *argument1, const char* file, int line, const char *function, bool &ignore_always);
	void			do_exit				(const std::string &message);

	// ---- soft asserts -------------------------------------------------------
	// While a thread holds a scope, an engine assert / CHK_DX on THAT thread
	// throws soft_assert_error instead of raising the fatal dialog. For code
	// that feeds ARBITRARY bytes into engine loaders - the content browser
	// previewing a linked game's files is the case - where "this file is bad"
	// must mean a failed preview, never a dead editor. Per-thread and counted,
	// so scopes nest and other threads keep the normal fatal behaviour.
	struct soft_assert_error {};
	static void		soft_asserts_push	();
	static void		soft_asserts_pop	();
	static bool		soft_asserts_active	();
	struct soft_assert_scope
	{
					soft_assert_scope	()	{ soft_asserts_push(); }
					~soft_assert_scope	()	{ soft_asserts_pop(); }
	};
};

// warning
// this function can be used for debug purposes only
IC	std::string __cdecl	make_string		(LPCSTR format,...)
{
	va_list		args;
	va_start	(args,format);

	char		temp[4096];
	vsprintf	(temp,format,args);

	return		std::string(temp);
}

extern XRCORE_API	xrDebug		Debug;

XRCORE_API void LogStackTrace	(LPCSTR header);

#include "xrDebug_macros.h"

#endif // xrDebugH