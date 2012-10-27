/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sys_win.c -- Win32 system interface code

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#include "client.h"
#include "common.h"
#include "conproc.h"
#include "console.h"
#include "host.h"
#include "quakedef.h"
#include "resource.h"
#include "screen.h"
#include "sys.h"
#include "winquake.h"

#define MINIMUM_WIN_MEMORY	0x0C00000 /* 12 MB */
#define MAXIMUM_WIN_MEMORY	0x2000000 /* 32 MB */

#define CONSOLE_ERROR_TIMEOUT	60.0	// # of seconds to wait on Sys_Error
					// running dedicated before exiting

#define PAUSE_SLEEP	50	// sleep time on pause or minimization
#define NOT_FOCUS_SLEEP	20	// sleep time when not focus

qboolean ActiveApp;
qboolean WinNT;

static double timer_pfreq;
static int timer_lowshift;
static unsigned int timer_oldtime;
static qboolean timer_fallback;
static DWORD timer_fallback_start;

qboolean isDedicated;
static qboolean sc_return_on_enter = false;
static HANDLE hinput, houtput;

static HANDLE tevent;
static HANDLE hFile;
static HANDLE heventParent;
static HANDLE heventChild;

void MaskExceptions(void);
void Sys_PushFPCW_SetHigh(void);
void Sys_PopFPCW(void);

volatile int sys_checksum;

static void Print_Win32SystemError(DWORD err);

void
Sys_DebugLog(const char *file, const char *fmt, ...)
{
    va_list argptr;
    static char data[MAX_PRINTMSG];
    int fd;

    va_start(argptr, fmt);
    vsnprintf(data, sizeof(data), fmt, argptr);
    va_end(argptr);
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0666);
    write(fd, data, strlen(data));
    close(fd);
};

/*
================
Sys_PageIn
================
*/
void
Sys_PageIn(void *ptr, int size)
{
    byte *x;
    int m, n;

// touch all the memory to make sure it's there. The 16-page skip is to
// keep Win 95 from thinking we're trying to page ourselves in (we are
// doing that, of course, but there's no reason we shouldn't)
    x = (byte *)ptr;

    for (n = 0; n < 4; n++) {
	for (m = 0; m < (size - 16 * 0x1000); m += 4) {
	    sys_checksum += *(int *)&x[m];
	    sys_checksum += *(int *)&x[m + 16 * 0x1000];
	}
    }
}

/*
===============================================================================

FILE IO

===============================================================================
*/

int
Sys_FileTime(const char *path)
{
    FILE *f;
    int t, retval;

    t = VID_ForceUnlockedAndReturnState();

    f = fopen(path, "rb");

    if (f) {
	fclose(f);
	retval = 1;
    } else {
	retval = -1;
    }

    VID_ForceLockState(t);
    return retval;
}

void
Sys_mkdir(const char *path)
{
    _mkdir(path);
}


/*
===============================================================================

SYSTEM IO

===============================================================================
*/

/*
================
Sys_MakeCodeWriteable
================
*/
void
Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
    DWORD flOldProtect;

    if (!VirtualProtect
	((LPVOID)startaddr, length, PAGE_READWRITE, &flOldProtect))
	Sys_Error("Protection change failed");
}


static void
Sys_InitTimers(void)
{
    LARGE_INTEGER freq, pcount;
    unsigned int lowpart, highpart;

    MaskExceptions();
    Sys_SetFPCW();

    if (!QueryPerformanceFrequency(&freq)) {
	Con_Printf("WARNING: No hardware timer available, using fallback\n");
	timer_fallback = true;
	timer_fallback_start = timeGetTime();
	return;
    }

    /*
     * get 32 out of the 64 time bits such that we have around
     * 1 microsecond resolution
     */
    lowpart = (unsigned int)freq.LowPart;
    highpart = (unsigned int)freq.HighPart;
    timer_lowshift = 0;

    while (highpart || (lowpart > 2000000.0)) {
	timer_lowshift++;
	lowpart >>= 1;
	lowpart |= (highpart & 1) << 31;
	highpart >>= 1;
    }
    timer_pfreq = 1.0 / (double)lowpart;

    /* Do first time initialisation */
    Sys_PushFPCW_SetHigh();
    QueryPerformanceCounter(&pcount);
    timer_oldtime = (unsigned int)pcount.LowPart >> timer_lowshift;
    timer_oldtime |= (unsigned int)pcount.HighPart << (32 - timer_lowshift);
    Sys_PopFPCW();
}

/*
================
Sys_Init
================
*/
void
Sys_Init(void)
{
    OSVERSIONINFO vinfo;

    vinfo.dwOSVersionInfoSize = sizeof(vinfo);

    if (!GetVersionEx(&vinfo))
	Sys_Error("Couldn't get OS info");

    if ((vinfo.dwMajorVersion < 4) ||
	(vinfo.dwPlatformId == VER_PLATFORM_WIN32s)) {
	Sys_Error("TyrQuake requires at least Win95 or NT 4.0");
    }

    if (vinfo.dwPlatformId == VER_PLATFORM_WIN32_NT)
	WinNT = true;
    else
	WinNT = false;
}


void
Sys_Error(const char *error, ...)
{
    va_list argptr;
    char text[MAX_PRINTMSG];
    char text2[MAX_PRINTMSG];
    char *text3 = "Press Enter to exit\n";
    char *text4 = "***********************************\n";
    char *text5 = "\n";
    DWORD dummy;
    double starttime;
    static int in_sys_error0 = 0;
    static int in_sys_error1 = 0;
    static int in_sys_error2 = 0;
    static int in_sys_error3 = 0;

    if (!in_sys_error3) {
	in_sys_error3 = 1;
	VID_ForceUnlockedAndReturnState();
    }

    va_start(argptr, error);
    vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);

    if (isDedicated) {
	snprintf(text2, sizeof(text2), "ERROR: %s\n", text);
	if (text2[sizeof(text2) - 2])
	    strcpy(text2 + sizeof(text2) - 2, "\n"); /* in case we truncated */
	WriteFile(houtput, text5, strlen(text5), &dummy, NULL);
	WriteFile(houtput, text4, strlen(text4), &dummy, NULL);
	WriteFile(houtput, text2, strlen(text2), &dummy, NULL);
	WriteFile(houtput, text3, strlen(text3), &dummy, NULL);
	WriteFile(houtput, text4, strlen(text4), &dummy, NULL);

	starttime = Sys_DoubleTime();
	sc_return_on_enter = true;	// so Enter will get us out of here

	while (!Sys_ConsoleInput() &&
	       ((Sys_DoubleTime() - starttime) < CONSOLE_ERROR_TIMEOUT)) {
	}
    } else {
	// switch to windowed so the message box is visible, unless we already
	// tried that and failed
	if (!in_sys_error0) {
	    in_sys_error0 = 1;
	    VID_SetDefaultMode();
	    MessageBox(NULL, text, "Quake Error",
		       MB_OK | MB_SETFOREGROUND | MB_ICONSTOP);
	} else {
	    MessageBox(NULL, text, "Double Quake Error",
		       MB_OK | MB_SETFOREGROUND | MB_ICONSTOP);
	}
    }

    if (!in_sys_error1) {
	in_sys_error1 = 1;
	Host_Shutdown();
    }
// shut down QHOST hooks if necessary
    if (!in_sys_error2) {
	in_sys_error2 = 1;
	DeinitConProc();
    }

    exit(1);
}

void
Sys_Printf(const char *fmt, ...)
{
    va_list argptr;
    char text[MAX_PRINTMSG];
    DWORD dummy;

    if (isDedicated) {
	va_start(argptr, fmt);
	vsnprintf(text, sizeof(text), fmt, argptr);
	va_end(argptr);

	WriteFile(houtput, text, strlen(text), &dummy, NULL);
    } else {
	va_start(argptr, fmt);
	vprintf(fmt, argptr);
	va_end(argptr);
    }
}

void
Sys_Quit(void)
{
    VID_ForceUnlockedAndReturnState();

    Host_Shutdown();

    if (tevent)
	CloseHandle(tevent);

    if (isDedicated)
	FreeConsole();

// shut down QHOST hooks if necessary
    DeinitConProc();

    exit(0);
}


/*
================
Sys_DoubleTime
================
*/
double
Sys_DoubleTime(void)
{
    static double curtime = 0.0;
    static double lastcurtime = 0.0;
    static int sametimecount;

    LARGE_INTEGER pcount;
    unsigned int temp, t2;
    double time;

    if (timer_fallback) {
	DWORD now = timeGetTime();
	if (now < timer_fallback_start)	/* wrapped */
	    return (now + (LONG_MAX - timer_fallback_start)) / 1000.0;
	return (now - timer_fallback_start) / 1000.0;
    }

    Sys_PushFPCW_SetHigh();

    QueryPerformanceCounter(&pcount);

    temp = (unsigned int)pcount.LowPart >> timer_lowshift;
    temp |= (unsigned int)pcount.HighPart << (32 - timer_lowshift);

    /* check for turnover or backward time */
    if ((temp <= timer_oldtime) && ((timer_oldtime - temp) < 0x10000000)) {
	timer_oldtime = temp;	/* so we don't get stuck */
    } else {
	t2 = temp - timer_oldtime;
	time = (double)t2 * timer_pfreq;
	timer_oldtime = temp;
	curtime += time;
	if (curtime == lastcurtime) {
	    sametimecount++;
	    if (sametimecount > 100000) {
		curtime += 1.0;
		sametimecount = 0;
	    }
	} else {
	    sametimecount = 0;
	}
	lastcurtime = curtime;
    }

    Sys_PopFPCW();

    return curtime;
}


char *
Sys_ConsoleInput(void)
{
    static char text[256];
    static int len;
    INPUT_RECORD recs[1024];
    DWORD dummy;
    int ch;
    DWORD numread, numevents;

    if (!isDedicated)
	return NULL;


    for (;;) {
	if (!GetNumberOfConsoleInputEvents(hinput, &numevents)) {
	    DWORD err = GetLastError();

	    printf("GetNumberOfConsoleInputEvents: ");
	    Print_Win32SystemError(err);
	    Sys_Error("Error getting # of console events");
	}

	if (numevents <= 0)
	    break;

	if (!ReadConsoleInput(hinput, recs, 1, &numread))
	    Sys_Error("Error reading console input");

	if (numread != 1)
	    Sys_Error("Couldn't read console input");

	if (recs[0].EventType == KEY_EVENT) {
	    if (!recs[0].Event.KeyEvent.bKeyDown) {
		ch = recs[0].Event.KeyEvent.uChar.AsciiChar;

		switch (ch) {
		case '\r':
		    WriteFile(houtput, "\r\n", 2, &dummy, NULL);

		    if (len) {
			text[len] = 0;
			len = 0;
			return text;
		    } else if (sc_return_on_enter) {
			// special case to allow exiting from the error handler on Enter
			text[0] = '\r';
			len = 0;
			return text;
		    }

		    break;

		case '\b':
		    WriteFile(houtput, "\b \b", 3, &dummy, NULL);
		    if (len) {
			len--;
		    }
		    break;

		default:
		    if (ch >= ' ') {
			WriteFile(houtput, &ch, 1, &dummy, NULL);
			text[len] = ch;
			len = (len + 1) & 0xff;
		    }

		    break;

		}
	    }
	}
    }

    return NULL;
}

void
Sys_Sleep(void)
{
    Sleep(1);
}


void
Sys_SendKeyEvents(void)
{
    MSG msg;

    while (PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE)) {
	// we always update if there are any event, even if we're paused
	scr_skipupdate = 0;

	if (!GetMessage(&msg, NULL, 0, 0))
	    Sys_Quit();

	TranslateMessage(&msg);
	DispatchMessage(&msg);
    }
}


/*
==============================================================================

 WINDOWS CRAP

==============================================================================
*/


/*
==================
WinMain
==================
*/
void
SleepUntilInput(int time)
{
    MsgWaitForMultipleObjects(1, &tevent, FALSE, time, QS_ALLINPUT);
}

/*
 * For debugging - Print a Win32 system error string to stdout
 */
static void
Print_Win32SystemError(DWORD err)
{
    static PVOID buf;

    if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER
		      | FORMAT_MESSAGE_FROM_SYSTEM,
		      NULL, err, 0, (LPTSTR)(&buf), 0, NULL)) {
	printf("%s: %s\n", __func__, (LPTSTR)buf);
	fflush(stdout);
	LocalFree(buf);
    }
}

/*
==================
WinMain
==================
*/
HINSTANCE global_hInstance;
int global_nCmdShow;
char *argv[MAX_NUM_ARGVS];
static char *empty_string = "";
HWND hwnd_dialog;


int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine,
	int nCmdShow)
{
    quakeparms_t parms;
    double time, oldtime, newtime;
    MEMORYSTATUS lpBuffer;
    static char cwd[1024];
    int t;
    RECT rect;
    DWORD err;

    /* previous instances do not exist in Win32 */
    if (hPrevInstance)
	return 0;

    global_hInstance = hInstance;
    global_nCmdShow = nCmdShow;

    lpBuffer.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&lpBuffer);

    if (!GetCurrentDirectory(sizeof(cwd), cwd))
	Sys_Error("Couldn't determine current directory");

    if (cwd[strlen(cwd) - 1] == '/')
	cwd[strlen(cwd) - 1] = 0;

    parms.basedir = cwd;
    parms.cachedir = NULL;

    parms.argc = 1;
    argv[0] = empty_string;

    while (*lpCmdLine && (parms.argc < MAX_NUM_ARGVS)) {
	while (*lpCmdLine && ((*lpCmdLine <= 32) || (*lpCmdLine > 126)))
	    lpCmdLine++;

	if (*lpCmdLine) {
	    argv[parms.argc] = lpCmdLine;
	    parms.argc++;

	    while (*lpCmdLine && ((*lpCmdLine > 32) && (*lpCmdLine <= 126)))
		lpCmdLine++;

	    if (*lpCmdLine) {
		*lpCmdLine = 0;
		lpCmdLine++;
	    }

	}
    }

    parms.argv = argv;

    COM_InitArgv(parms.argc, parms.argv);

    parms.argc = com_argc;
    parms.argv = com_argv;

    isDedicated = (COM_CheckParm("-dedicated") != 0);

    if (!isDedicated) {
	hwnd_dialog =
	    CreateDialog(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), NULL, NULL);

	if (hwnd_dialog) {
	    if (GetWindowRect(hwnd_dialog, &rect)) {
		if (rect.left > (rect.top * 2)) {
		    SetWindowPos(hwnd_dialog, 0,
				 (rect.left / 2) -
				 ((rect.right - rect.left) / 2), rect.top, 0,
				 0, SWP_NOZORDER | SWP_NOSIZE);
		}
	    }

	    ShowWindow(hwnd_dialog, SW_SHOWDEFAULT);
	    UpdateWindow(hwnd_dialog);
	    SetForegroundWindow(hwnd_dialog);
	}
    }

    /*
     * Take the greater of all the available memory or half the total
     * memory, but at least MINIMUM_WIN_MEMORY and no more than
     * MAXIMUM_WIN_MEMORY, unless explicitly requested otherwise
     */
    parms.memsize = lpBuffer.dwAvailPhys;

    if (parms.memsize < MINIMUM_WIN_MEMORY)
	parms.memsize = MINIMUM_WIN_MEMORY;

    if (parms.memsize < (lpBuffer.dwTotalPhys >> 1))
	parms.memsize = lpBuffer.dwTotalPhys >> 1;

    if (parms.memsize > MAXIMUM_WIN_MEMORY)
	parms.memsize = MAXIMUM_WIN_MEMORY;

    if (COM_CheckParm("-heapsize")) {
	t = COM_CheckParm("-heapsize") + 1;

	if (t < com_argc)
	    parms.memsize = Q_atoi(com_argv[t]) * 1024;
    }

    parms.membase = malloc(parms.memsize);

    if (!parms.membase)
	Sys_Error("Not enough memory free; check disk space");

    Sys_PageIn(parms.membase, parms.memsize);

    tevent = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!tevent)
	Sys_Error("Couldn't create event");

    if (isDedicated) {
	if (!AllocConsole()) {
	    DWORD err = GetLastError();

	    printf("AllocConsole Failed: ");
	    Print_Win32SystemError(err);

	    // Already have one? - Try free it and get a new one...
	    // FIXME - Keep current console or get new one...
	    FreeConsole();
	    if (!AllocConsole()) {
		err = GetLastError();
		printf("AllocConsole (2nd try): Error %i\n", (int)err);
		fflush(stdout);

		// FIXME - might not have stdout or stderr here for Sys_Error.
		Sys_Error("Couldn't create dedicated server console");
	    }
	}
	// FIXME - these can fail...
	// FIXME - the whole console creation thing is pretty screwy...
	// FIXME - well, at least from cygwin rxvt it sucks...
	hinput = GetStdHandle(STD_INPUT_HANDLE);
	if (!hinput) {
	    err = GetLastError();
	    printf("GetStdHandle(STD_INPUT_HANDLE): Error %i\n", (int)err);
	    fflush(stdout);
	}
	houtput = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!hinput) {
	    err = GetLastError();
	    printf("GetStdHandle(STD_OUTPUT_HANDLE): Error %i\n", (int)err);
	    fflush(stdout);
	}
	// give QHOST a chance to hook into the console
	// FIXME - What is QHOST?
	if ((t = COM_CheckParm("-HFILE")) > 0) {
	    if (t < com_argc)
		hFile = (HANDLE)Q_atoi(com_argv[t + 1]);
	}

	if ((t = COM_CheckParm("-HPARENT")) > 0) {
	    if (t < com_argc)
		heventParent = (HANDLE)Q_atoi(com_argv[t + 1]);
	}

	if ((t = COM_CheckParm("-HCHILD")) > 0) {
	    if (t < com_argc)
		heventChild = (HANDLE)Q_atoi(com_argv[t + 1]);
	}

	InitConProc(hFile, heventParent, heventChild);
    }

    Sys_Init();
    Sys_InitTimers();

// because sound is off until we become active
    S_BlockSound();

    Sys_Printf("Host_Init\n");
    Host_Init(&parms);

    oldtime = Sys_DoubleTime();

    /* main window message loop */
    while (1) {
	if (isDedicated) {
	    newtime = Sys_DoubleTime();
	    time = newtime - oldtime;

	    while (time < sys_ticrate.value) {
		Sys_Sleep();
		newtime = Sys_DoubleTime();
		time = newtime - oldtime;
	    }
	} else {
	    // yield the CPU for a little while when paused, minimized, or not the focus
	    if ((cl.paused && (!ActiveApp && !DDActive)) || !window_visible()
		|| block_drawing) {
		SleepUntilInput(PAUSE_SLEEP);
		scr_skipupdate = 1;	// no point in bothering to draw
	    } else if (!ActiveApp && !DDActive) {
		SleepUntilInput(NOT_FOCUS_SLEEP);
	    }

	    newtime = Sys_DoubleTime();
	    time = newtime - oldtime;
	}

	Host_Frame(time);
	oldtime = newtime;
    }

    /* return success of application */
    return TRUE;
}

#ifndef USE_X86_ASM
void
Sys_HighFPPrecision(void)
{
}

void
Sys_LowFPPrecision(void)
{
}

void
Sys_SetFPCW(void)
{
}

void
Sys_PushFPCW_SetHigh(void)
{
}

void
Sys_PopFPCW(void)
{
}

void
MaskExceptions(void)
{
}
#endif
