#include "stdafx.h"
#pragma hdrstop

#ifndef _EDITOR

#include "xrdebug.h"
#include "resource.h"
#include "dbghelp.h"

#include "dxerr.h"

#ifdef __BORLANDC__
#include "d3d9.h"
#include "d3dx9.h"
#include "D3DX_Wrapper.h"
#pragma comment (lib,"EToolsB.lib")
static BOOL bException = TRUE;
#else
static BOOL bException = FALSE;
#endif

#ifdef _M_AMD64
#define DEBUG_INVOKE DebugBreak ()
#else
#define DEBUG_INVOKE __asm { int 3 }
#ifndef __BORLANDC__
#pragma comment (lib,"dxerr.lib")
#endif
#endif

XRCORE_API xrDebug Debug;

// Dialog support
static const char* dlgExpr = NULL;
static const char* dlgFile = NULL;
static char dlgLine[16];

static INT_PTR CALLBACK DialogProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		{
			if (dlgFile)
			{
				SetWindowText(GetDlgItem(hw, IDC_DESC), dlgExpr);
				SetWindowText(GetDlgItem(hw, IDC_FILE), dlgFile);
				SetWindowText(GetDlgItem(hw, IDC_LINE), dlgLine);
			}
			else
			{
				SetWindowText(GetDlgItem(hw, IDC_DESC), dlgExpr);
				SetWindowText(GetDlgItem(hw, IDC_FILE), "");
				SetWindowText(GetDlgItem(hw, IDC_LINE), "");
			}
		}
		break;
	case WM_DESTROY:
		break;
	case WM_CLOSE:
		EndDialog(hw, IDC_STOP);
		break;
	case WM_COMMAND:
		if (LOWORD(wp) == IDC_STOP)
		{
			EndDialog(hw, IDC_STOP);
		}
		if (LOWORD(wp) == IDC_DEBUG)
		{
			EndDialog(hw, IDC_DEBUG);
		}
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

void xrDebug::backend(const char* reason, const char* expression, const char* argument0, const char* argument1,
                      const char* file, int line, const char* function, bool& ignore_always)
{
	static xrCriticalSection CS;

	CS.Enter();

	// Log
	string1024 tmp;
	xr_sprintf(tmp, "***STOP*** file '%s', line %d.\n***Reason***: %s\n %s", file, line, reason, expression);
	Msg(tmp);
	xrLogger::FlushLog();
	if (handler) handler();

	if (IsDebuggerPresent())
		DebugBreak();

	// Call the dialog
	dlgExpr = reason;
	xr_sprintf()
	dlgFile = file;
	xr_sprintf(dlgLine, "%d", line);
	INT_PTR res = -1;
#ifdef XRCORE_STATIC
    MessageBox(NULL, tmp, "X-Ray error", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
#else
	res = DialogBox
	(
		GetModuleHandle(MODULE_NAME),
		MAKEINTRESOURCE(IDD_STOP),
		NULL,
		DialogProc
	);
#endif
	switch (res)
	{
	case -1:
	case IDC_STOP:
		if (bException) TerminateProcess(GetCurrentProcess(), 3);
		else RaiseException(0, 0, 0, NULL);
		break;
	case IDC_DEBUG:
		DEBUG_INVOKE;
		break;
	}

	CS.Leave();
}

LPCSTR xrDebug::error2string(long code)
{
	LPCSTR result = 0;
	static string1024 desc_storage;

#ifdef _M_AMD64
#else
    result = DXGetErrorDescription(code);
#endif
	if (0 == result)
	{
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, code, 0, desc_storage, sizeof(desc_storage) - 1, 0);
		result = desc_storage;
	}
	return result;
}

void xrDebug::error(long hr, const char* expr, const char* file, int line, const char* function, bool& ignore_always)
{
	backend(error2string(hr), expr, 0, 0, file, line, function, ignore_always);
}

void xrDebug::error(long hr, const char* expr, const char* e2, const char* file, int line, const char* function,
                    bool& ignore_always)
{
	backend(error2string(hr), expr, e2, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* file, int line, const char* function, bool& ignore_always)
{
	backend("assertion failed", e1, 0, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* file, int line, const char* function,
                   bool& ignore_always)
{
	backend(e1, e2, 0, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* e3, const char* file, int line, const char* function,
                   bool& ignore_always)
{
	backend(e1, e2, e3, 0, file, line, function, ignore_always);
}

void xrDebug::fail(const char* e1, const char* e2, const char* e3, const char* e4, const char* file, int line,
                   const char* function, bool& ignore_always)
{
	backend(e1, e2, e3, e4, file, line, function, ignore_always);
}

void __cdecl xrDebug::fatal(const char* file, int line, const char* function, const char* F, ...)
{
	string1024 buffer;

	va_list p;
	va_start(p, F);
	vsprintf(buffer, F, p);
	va_end(p);

	bool ignore_always = true;

	backend("fatal error", "<no expression>", buffer, 0, file, line, function, ignore_always);
}

void xrDebug::do_exit(const std::string& message)
{
	xrLogger::FlushLog();
	MessageBox(NULL, message.c_str(), "Error", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
	TerminateProcess(GetCurrentProcess(), 1);
}

int __cdecl _out_of_memory(size_t size)
{
	Debug.fatal(DEBUG_INFO, "Out of memory. Memory request: %d K", size / 1024);
	return 1;
}

void __cdecl _terminate()
{
	FATAL("Unexpected application termination");
}

// based on dbghelp.h
typedef BOOL (WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                         CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                         CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                         CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
);


// ---------------------------------------------------------------------------------------------
// How much the crash dump contains (see g_crash_dump_level in xrDebug.h).
//
//   0  Default. Threads, call stacks and the module list only: a small file (about 1 MB or less).
//      Gives a call stack, but none of the game's own memory, so the objects that were involved
//      in the crash cannot be read from it.
//   1  "-big_dump" on the command line, or "crash_dump_level 1" in the console. Adds the global
//      variables of every module and the memory that the stacks and registers point at.
//      Expect tens to a few hundred MB.
//   2  "-full_dump" on the command line, or "crash_dump_level 2" in the console. All memory of
//      the process, so any object can be inspected afterwards. The file is about as large as the
//      memory the game was using when it crashed (several GB); on a slow disk writing it takes
//      minutes and the game window stays frozen until it is done.
//
// The console variable is saved in user.ltx. The command line switches also cover a crash that
// happens before user.ltx has been read. The higher of the two settings is used.
// Level 2 uses the full-memory format on purpose: it is the minidump layout made for files
// larger than 4 GB. If dbghelp.dll refuses a level, a simpler one is tried, down to the default dump.
// The sizes above are estimates, they have not been measured on this build.
// ---------------------------------------------------------------------------------------------
XRCORE_API int g_crash_dump_level = 0;

static int crash_dump_level()
{
	int level = g_crash_dump_level;
	if (Core.Params)
	{
		if (level < 1 && strstr(Core.Params, "-big_dump")) level = 1;
		if (level < 2 && strstr(Core.Params, "-full_dump")) level = 2;
	}
	if (level < 0) level = 0;
	if (level > 2) level = 2;
	return level;
}

static LPCSTR crash_dump_level_info(int level)
{
	switch (level)
	{
	case 1: return "level 1: call stacks, globals and the memory the stacks point at; tens to a few hundred MB";
	case 2: return "level 2: all memory of the game; several GB, can take minutes on a slow disk";
	default: return "level 0: call stacks only; about 1 MB";
	}
}

LONG WINAPI UnhandledFilter(struct _EXCEPTION_POINTERS* pExceptionInfo)
{
	LONG retval = EXCEPTION_CONTINUE_SEARCH;
	bException = TRUE;

	// firstly see if dbghelp.dll is around and has the function we need
	// look next to the EXE first, as the one in System32 might be old
	// (e.g. Windows 2000)
	HMODULE hDll = NULL;
	string_path szDbgHelpPath;

	if (GetModuleFileName(NULL, szDbgHelpPath, _MAX_PATH))
	{
		char* pSlash = strchr(szDbgHelpPath, '\\');
		if (pSlash)
		{
			xr_strcpy(pSlash + 1, "DBGHELP.DLL");
			hDll = ::LoadLibrary(szDbgHelpPath);
		}
	}

	if (hDll == NULL)
	{
		// load any version we can
		hDll = ::LoadLibrary("DBGHELP.DLL");
	}

	LPCTSTR szResult = NULL;

	if (hDll)
	{
		MINIDUMPWRITEDUMP pDump = (MINIDUMPWRITEDUMP)::GetProcAddress(hDll, "MiniDumpWriteDump");
		if (pDump)
		{
			string_path szDumpPath;
			string_path szScratch;
			string64 t_stemp;

			// work out a good place for the dump file
			timestamp(t_stemp);
			xr_strcpy(szDumpPath, "logs\\");
			xr_strcat(szDumpPath, Core.ApplicationName);
			xr_strcat(szDumpPath, "_");
			xr_strcat(szDumpPath, Core.UserName);
			xr_strcat(szDumpPath, "_");
			xr_strcat(szDumpPath, t_stemp);
			xr_strcat(szDumpPath, ".mdmp");

			// create the file
			HANDLE hFile = ::CreateFile(szDumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
			                            FILE_ATTRIBUTE_NORMAL, NULL);
			if (INVALID_HANDLE_VALUE == hFile)
			{
				// try to place into current directory
				MoveMemory(szDumpPath, szDumpPath + 5, strlen(szDumpPath));
				hFile = ::CreateFile(szDumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
				                     FILE_ATTRIBUTE_NORMAL, NULL);
			}
			if (hFile != INVALID_HANDLE_VALUE)
			{
				_MINIDUMP_EXCEPTION_INFORMATION ExInfo;

				ExInfo.ThreadId = ::GetCurrentThreadId();
				ExInfo.ExceptionPointers = pExceptionInfo;
				ExInfo.ClientPointers = NULL;

				// write the dump
				// Level 0 is exactly what this handler has always written. The bigger levels are
				// opt-in, see the comment above crash_dump_level().
				const MINIDUMP_TYPE flags_default = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpFilterMemory | MiniDumpScanMemory);
				const int dump_level = crash_dump_level();

				// What to try, most detailed first; the last entry is always the default dump.
				MINIDUMP_TYPE attempt_flags[4];
				int attempt_level[4];
				int attempt_count = 0;
				if (dump_level >= 2)
				{
					// with the extra bookkeeping streams first, then plain full memory in case the
					// dbghelp.dll that got loaded is too old to know the extra flags
					attempt_flags[attempt_count] = MINIDUMP_TYPE(MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo |
						MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpIgnoreInaccessibleMemory);
					attempt_level[attempt_count++] = 2;
					attempt_flags[attempt_count] = MINIDUMP_TYPE(MiniDumpWithFullMemory);
					attempt_level[attempt_count++] = 2;
				}
				if (dump_level >= 1)
				{
					attempt_flags[attempt_count] = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithDataSegs |
						MiniDumpWithIndirectlyReferencedMemory);
					attempt_level[attempt_count++] = 1;
				}
				attempt_flags[attempt_count] = flags_default;
				attempt_level[attempt_count++] = 0;

				if (dump_level > 0)
				{
					Msg("* Writing crash dump '%s' (%s). The game is not hung, wait for it to finish.",
					    szDumpPath, crash_dump_level_info(dump_level));
					xrLogger::FlushLog();
				}

				BOOL bOK = FALSE;
				int written_level = 0;
				for (int attempt = 0; attempt < attempt_count && !bOK; ++attempt)
				{
					if (attempt > 0)
					{
						// the previous attempt failed: start the file again from empty
						::SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
						::SetEndOfFile(hFile);
					}
					written_level = attempt_level[attempt];
					bOK = pDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, attempt_flags[attempt], &ExInfo, NULL, NULL);
					if (!bOK && dump_level > 0)
						Msg("! Crash dump level %d failed (error 0x%08x)", written_level, GetLastError());
				}

				if (bOK)
				{
					if (dump_level > 0)
					{
						LARGE_INTEGER dump_size;
						dump_size.QuadPart = 0;
						::GetFileSizeEx(hFile, &dump_size);
						xr_sprintf(szScratch, "Saved dump file to '%s' (%u MB, %s)", szDumpPath,
						           u32(dump_size.QuadPart / (1024 * 1024)), crash_dump_level_info(written_level));
					}
					else
						xr_sprintf(szScratch, "Saved dump file to '%s'", szDumpPath);
					szResult = szScratch;
					retval = EXCEPTION_EXECUTE_HANDLER;
				}
				else
				{
					xr_sprintf(szScratch, "Failed to save dump file to '%s' (error %d)", szDumpPath, GetLastError());
					szResult = szScratch;
				}
				::CloseHandle(hFile);
			}
			else
			{
				xr_sprintf(szScratch, "Failed to create dump file '%s' (error %d)", szDumpPath, GetLastError());
				szResult = szScratch;
			}
		}
		else
		{
			szResult = "DBGHELP.DLL too old";
		}
	}
	else
	{
		szResult = "DBGHELP.DLL not found";
	}

	if (IsDebuggerPresent())
		DebugBreak();

	string1024 reason;
	xr_sprintf(reason, "*** Internal Error ***\n%s", szResult);
	bool ref = false;
	Debug.backend(reason, 0, 0, 0, 0, 0, 0, ref);

	return retval;
}

//////////////////////////////////////////////////////////////////////
#ifdef M_BORLAND
// typedef void ( _RTLENTRY *___new_handler) ();
namespace std
{
extern new_handler _RTLENTRY _EXPFUNC set_new_handler(new_handler new_p);
};

// typedef int (__stdcall * _PNH)( size_t );
// _CRTIMP int __cdecl _set_new_mode( int );
// _PNH __cdecl set_new_handler( _PNH );
// typedef void (new * new_handler)();
// new_handler set_new_handler(new_handler my_handler);
static void __cdecl def_new_handler()
{
    FATAL("Out of memory.");
}

void xrDebug::_initialize(const bool& dedicated)
{
    // std::set_new_mode (1); // gen exception if can't allocate memory
    std::set_new_handler(def_new_handler); // exception-handler for 'out of memory' condition
    ::SetUnhandledExceptionFilter(UnhandledFilter); // exception handler to all "unhandled" exceptions
}
#else
typedef int (__cdecl* _PNH)(size_t);
_CRTIMP int __cdecl _set_new_mode(int);
_CRTIMP _PNH __cdecl _set_new_handler(_PNH);

void xrDebug::_initialize(const bool& dedicated)
{
	PROF_EVENT("xrDebug::_initialize");
	handler = 0;
	_set_new_mode(1); // gen exception if can't allocate memory
	_set_new_handler(_out_of_memory); // exception-handler for 'out of memory' condition
	std::set_terminate(_terminate);
	std::set_unexpected(_terminate);
	::SetUnhandledExceptionFilter(UnhandledFilter); // exception handler to all "unhandled" exceptions
}

#endif

#endif
