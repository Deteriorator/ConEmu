﻿
/*
Copyright (c) 2009-2014 Maximus5
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#ifdef _DEBUG
//  Раскомментировать, чтобы сразу после загрузки плагина показать MessageBox, чтобы прицепиться дебаггером
//  #define SHOW_STARTED_MSGBOX
#endif

//#define TRUE_COLORER_OLD_SUPPORT

#define SHOWDEBUGSTR
//#define MCHKHEAP
#define DEBUGSTRMENU(s) //DEBUGSTR(s)
#define DEBUGSTRINPUT(s) //DEBUGSTR(s)
#define DEBUGSTRDLGEVT(s) //DEBUGSTR(s)
#define DEBUGSTRCMD(s) DEBUGSTR(s)
#define DEBUGSTRACTIVATE(s) DEBUGSTR(s)


#include <windows.h>
#include "../common/common.hpp"
#include "../common/MWow64Disable.h"
#include "../ConEmuHk/SetHook.h"
#ifdef _DEBUG
#pragma warning( disable : 4995 )
#endif
#include "../common/pluginW1761.hpp"
#ifdef _DEBUG
#pragma warning( default : 4995 )
#endif
#include "../common/ConEmuCheckEx.h"
#include "../common/ConsoleAnnotation.h"
#include "../common/SetEnvVar.h"
#include "../common/WinObjects.h"
#include "../common/WinConsole.h"
#include "../common/TerminalMode.h"
#include "../common/MFileMapping.h"
#include "../common/MSection.h"
#include "../common/FarVersion.h"
#include "../ConEmu/version.h"
#include "PluginHeader.h"
#include "ConEmuPluginBase.h"
#include "PluginBackground.h"
#include <Tlhelp32.h>

#ifndef __GNUC__
	#include <Dbghelp.h>
#else
#endif

#include "../common/ConEmuCheck.h"
#include "PluginSrv.h"

#define Free free
#define Alloc calloc

#define MAKEFARVERSION(major,minor,build) ( ((major)<<8) | (minor) | ((build)<<16))

//#define ConEmu_SysID 0x43454D55 // 'CEMU'
enum CallPluginCmdId
{
	// Add new items - before first numbered item!
	CE_CALLPLUGIN_UPDATEBG = 99,
	CE_CALLPLUGIN_SENDTABS = 100,
	SETWND_CALLPLUGIN_BASE /*= (CE_CALLPLUGIN_SENDTABS+1)*/
	// Following number are reserved for "SetWnd(idx)" switching
};
#define CHECK_RESOURCES_INTERVAL 5000
#define CHECK_FARINFO_INTERVAL 2000
#define ATTACH_START_SERVER_TIMEOUT 10000

#define CMD__EXTERNAL_CALLBACK 0x80001
struct SyncExecuteArg
{
	DWORD nCmd;
	HMODULE hModule;
	SyncExecuteCallback_t CallBack;
	LONG_PTR lParam;
};

#if defined(__GNUC__)
extern "C" {
	BOOL WINAPI DllMain(HANDLE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved);
	HWND WINAPI GetFarHWND();
	HWND WINAPI GetFarHWND2(int anConEmuOnly);
	void WINAPI GetFarVersion(FarVersion* pfv);
	int  WINAPI ProcessEditorInputW(void* Rec);
	void WINAPI SetStartupInfoW(void *aInfo);
	BOOL WINAPI IsTerminalMode();
	BOOL WINAPI IsConsoleActive();
	int  WINAPI RegisterPanelView(PanelViewInit *ppvi);
	int  WINAPI RegisterBackground(RegisterBackgroundArg *pbk);
	int  WINAPI ActivateConsole();
	int  WINAPI SyncExecute(HMODULE ahModule, SyncExecuteCallback_t CallBack, LONG_PTR lParam);
	void WINAPI GetPluginInfoWcmn(void *piv);
};
#endif


HMODULE ghPluginModule = NULL; // ConEmu.dll - сам плагин
HWND ghConEmuWndDC = NULL; // Содержит хэндл окна отрисовки. Это ДОЧЕРНЕЕ окно.
DWORD gdwPreDetachGuiPID = 0;
DWORD gdwServerPID = 0;
BOOL TerminalMode = FALSE;
HWND FarHwnd = NULL;
DWORD gnMainThreadId = 0, gnMainThreadIdInitial = 0;
HANDLE ghMonitorThread = NULL; DWORD gnMonitorThreadId = 0;
HANDLE ghSetWndSendTabsEvent = NULL;
FarVersion gFarVersion = {};
WCHAR gszDir1[CONEMUTABMAX], gszDir2[CONEMUTABMAX];
int maxTabCount = 0, lastWindowCount = 0, gnCurTabCount = 0;
CESERVER_REQ* gpTabs = NULL; //(ConEmuTab*) Alloc(maxTabCount, sizeof(ConEmuTab));
BOOL gbForceSendTabs = FALSE;
int  gnCurrentWindowType = 0; // WTYPE_PANELS / WTYPE_VIEWER / WTYPE_EDITOR
BOOL gbIgnoreUpdateTabs = FALSE; // выставляется на время CMD_SETWINDOW
BOOL gbRequestUpdateTabs = FALSE; // выставляется при получении события FOCUS/KILLFOCUS
CurPanelDirs gPanelDirs = {};
BOOL gbClosingModalViewerEditor = FALSE; // выставляется при закрытии модального редактора/вьювера
MOUSE_EVENT_RECORD gLastMouseReadEvent = {{0,0}};
BOOL gbUngetDummyMouseEvent = FALSE;
LONG gnAllowDummyMouseEvent = 0;
LONG gnDummyMouseEventFromMacro = 0;

extern HMODULE ghHooksModule;
extern BOOL gbHooksModuleLoaded; // TRUE, если был вызов LoadLibrary("ConEmuHk.dll"), тогда его нужно FreeLibrary при выходе


MSection *csData = NULL;
// результат выполнения команды (пишется функциями OutDataAlloc/OutDataWrite)
CESERVER_REQ* gpCmdRet = NULL;
// инициализируется как "gpData = gpCmdRet->Data;"
LPBYTE gpData = NULL, gpCursor = NULL;
DWORD  gnDataSize=0;

int gnPluginOpenFrom = -1;
DWORD gnReqCommand = -1;
LPVOID gpReqCommandData = NULL;
static HANDLE ghReqCommandEvent = NULL;
static BOOL   gbReqCommandWaiting = FALSE;


UINT gnMsgTabChanged = 0;
MSection *csTabs = NULL;
BOOL  gbPlugKeyChanged=FALSE;
HKEY  ghRegMonitorKey=NULL; HANDLE ghRegMonitorEvt=NULL;
HANDLE ghPluginSemaphore = NULL;
wchar_t gsFarLang[64] = {0};
BOOL FindServerCmd(DWORD nServerCmd, DWORD &dwServerPID, bool bFromAttach = false);
BOOL gbNeedPostTabSend = FALSE;
BOOL gbNeedPostEditCheck = FALSE; // проверить, может в активном редакторе изменился статус
int lastModifiedStateW = -1;
BOOL gbNeedPostReloadFarInfo = FALSE;
DWORD gnNeedPostTabSendTick = 0;
#define NEEDPOSTTABSENDDELTA 100
#define MONITORENVVARDELTA 1000
void UpdateEnvVar(const wchar_t* pszList);
BOOL StartupHooks();
MFileMapping<CESERVER_CONSOLE_MAPPING_HDR> *gpConMap;
const CESERVER_CONSOLE_MAPPING_HDR *gpConMapInfo = NULL;
//AnnotationInfo *gpColorerInfo = NULL;
BOOL gbStartedUnderConsole2 = FALSE;
BOOL ReloadFarInfo(BOOL abForce);
DWORD gnSelfPID = 0; //GetCurrentProcessId();
HANDLE ghFarInfoMapping = NULL;
CEFAR_INFO_MAPPING *gpFarInfo = NULL, *gpFarInfoMapping = NULL;
HANDLE ghFarAliveEvent = NULL;
PanelViewRegInfo gPanelRegLeft = {NULL};
PanelViewRegInfo gPanelRegRight = {NULL};
// Для плагинов PicView & MMView нужно знать, нажат ли CtrlShift при F3
HANDLE ghConEmuCtrlPressed = NULL, ghConEmuShiftPressed = NULL;
BOOL gbWaitConsoleInputEmpty = FALSE, gbWaitConsoleWrite = FALSE; //, gbWaitConsoleInputPeek = FALSE;
HANDLE ghConsoleInputEmpty = NULL, ghConsoleWrite = NULL; //, ghConsoleInputWasPeek = NULL;
DWORD GetMainThreadId();
int gnSynchroCount = 0;
bool gbSynchroProhibited = false;
bool gbInputSynchroPending = false;

struct HookModeFar gFarMode = {sizeof(HookModeFar), TRUE/*bFarHookMode*/};
extern SetFarHookMode_t SetFarHookMode;


PluginAndMenuCommands gpPluginMenu[menu_Last] =
{
	{CEMenuEditOutput, menu_EditConsoleOutput, pcc_EditConsoleOutput},
	{CEMenuViewOutput, menu_ViewConsoleOutput, pcc_ViewConsoleOutput},
	{0, menu_Separator1}, // Separator
	{CEMenuShowHideTabs, menu_SwitchTabVisible, pcc_SwitchTabVisible},
	{CEMenuNextTab, menu_SwitchTabNext, pcc_SwitchTabNext},
	{CEMenuPrevTab, menu_SwitchTabPrev, pcc_SwitchTabPrev},
	{CEMenuCommitTab, menu_SwitchTabCommit, pcc_SwitchTabCommit},
	{CEMenuShowTabsList, menu_ShowTabsList},
	{0, menu_Separator2},
	{CEMenuGuiMacro, menu_ConEmuMacro}, // должен вызываться "по настоящему", а не через callplugin
	{0, menu_Separator3},
	{CEMenuAttach, menu_AttachToConEmu, pcc_AttachToConEmu},
	{0, menu_Separator4},
	{CEMenuDebug, menu_StartDebug, pcc_StartDebug},
	{CEMenuConInfo, menu_ConsoleInfo, pcc_StartDebug},
};
bool pcc_Selected(PluginMenuCommands nMenuID)
{
	bool bSelected = false;
	switch (nMenuID)
	{
	case menu_EditConsoleOutput:
		if (ghConEmuWndDC && IsWindow(ghConEmuWndDC))
			bSelected = true;
		break;
	case menu_AttachToConEmu:
		if (!((ghConEmuWndDC && IsWindow(ghConEmuWndDC)) || IsTerminalMode()))
			bSelected = true;
		break;
	case menu_ViewConsoleOutput:
	case menu_SwitchTabVisible:
	case menu_SwitchTabNext:
	case menu_SwitchTabPrev:
	case menu_SwitchTabCommit:
	case menu_ConEmuMacro:
	case menu_StartDebug:
	case menu_ConsoleInfo:
		break;
	}
	return bSelected;
}
bool pcc_Disabled(PluginMenuCommands nMenuID)
{
	bool bDisabled = false;
	switch (nMenuID)
	{
	case menu_AttachToConEmu:
		if ((ghConEmuWndDC && IsWindow(ghConEmuWndDC)) || IsTerminalMode())
			bDisabled = true;
		break;
	case menu_StartDebug:
		if (IsDebuggerPresent() || IsTerminalMode())
			bDisabled = true;
		break;
	case menu_EditConsoleOutput:
	case menu_ViewConsoleOutput:
	case menu_SwitchTabVisible:
	case menu_SwitchTabNext:
	case menu_SwitchTabPrev:
	case menu_SwitchTabCommit:
	case menu_ConEmuMacro:
		if (!ghConEmuWndDC || !IsWindow(ghConEmuWndDC))
			bDisabled = true;
		break;
	case menu_ConsoleInfo:
		break;
	}
	return bDisabled;
}


// export
void WINAPI GetPluginInfoWcmn(void *piv)
{
	Plugin()->GetPluginInfo(piv);
}

HANDLE WINAPI OpenPluginW(int OpenFrom, INT_PTR Item)
{
	CPluginBase* p = Plugin();
	HANDLE hPlugin = p->OpenPluginCommon(OpenFrom, Item, ((OpenFrom & p->of_FromMacro) == p->of_FromMacro));
	return hPlugin;
}

void TouchReadPeekConsoleInputs(int Peek /*= -1*/)
{
#ifdef _DEBUG
	_ASSERTE(GetCurrentThreadId() == gnMainThreadId);
#endif

	if (!gpFarInfo || !gpFarInfoMapping || !gpConMapInfo)
	{
		_ASSERTE(gpFarInfo);
		return;
	}

	// Во время макросов - считаем, что Фар "думает"
	if (!Plugin()->IsMacroActive())
	{
		SetEvent(ghFarAliveEvent);
	}

	//gpFarInfo->nFarReadIdx++;
	//gpFarInfoMapping->nFarReadIdx = gpFarInfo->nFarReadIdx;
#ifdef _DEBUG

	if (Peek == -1)
		return;

	if ((GetKeyState(VK_SCROLL)&1) == 0)
		return;

	static DWORD nLastTick;
	DWORD nCurTick = GetTickCount();
	DWORD nDelta = nCurTick - nLastTick;
	static CONSOLE_SCREEN_BUFFER_INFO sbi;
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

	if (nDelta > 1000)
	{
		GetConsoleScreenBufferInfo(hOut, &sbi);
		nCurTick = nCurTick;
	}

	static wchar_t Chars[] = L"-\\|/-\\|/";
	int nNextChar = 0;

	if (Peek)
	{
		static int nPeekChar = 0;
		nNextChar = nPeekChar++;

		if (nPeekChar >= 8) nPeekChar = 0;
	}
	else
	{
		static int nReadChar = 0;
		nNextChar = nReadChar++;

		if (nReadChar >= 8) nReadChar = 0;
	}

	CHAR_INFO chi;
	chi.Char.UnicodeChar = Chars[nNextChar];
	chi.Attributes = 15;
	COORD crBufSize = {1,1};
	COORD crBufCoord = {0,0};
	// Cell[0] лучше не трогать - GUI ориентируется на наличие "1" в этой ячейке при проверке активности фара
	SHORT nShift = (Peek?1:2);
	SMALL_RECT rc = {sbi.srWindow.Left+nShift,sbi.srWindow.Bottom,sbi.srWindow.Left+nShift,sbi.srWindow.Bottom};
	WriteConsoleOutputW(hOut, &chi, crBufSize, crBufCoord, &rc);
#endif
}

DWORD gnPeekReadCount = 0;

// Вызывается только в основной нити
// и ТОЛЬКО если фар считывает один (1) INPUT_RECORD
void OnConsolePeekReadInput(BOOL abPeek)
{
#ifdef _DEBUG
	DWORD nCurTID = GetCurrentThreadId();
	DWORD nCurMainTID = gnMainThreadId;
	if (nCurTID != nCurMainTID)
	{
		HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, nCurMainTID);
		if (hThread) SuspendThread(hThread);
		_ASSERTE(nCurTID == nCurMainTID);
		if (hThread) { ResumeThread(hThread); CloseHandle(hThread); }
	}
#endif
	bool lbNeedSynchro = false;

	// Для того, чтобы WaitPluginActivateion знал, живой фар, или не очень...
	gnPeekReadCount++;

	if (gpConMapInfo && gpFarInfo && gpFarInfoMapping)
		TouchReadPeekConsoleInputs(abPeek ? 1 : 0);

	if (/*gbNeedReloadFarInfo &&*/ abPeek == FALSE)
	{
		//gbNeedReloadFarInfo = FALSE;
		bool bNeedReload = false;

		if (gpConMapInfo)
		{
			DWORD nMapPID = gpConMapInfo->nActiveFarPID;
			static DWORD dwLastTickCount = 0;

			if (nMapPID == 0 || nMapPID != gnSelfPID)
			{
				//Выполнить команду в главном сервере, альтернативный не имеет права писать в мэппинг
				bNeedReload = true;
				dwLastTickCount = GetTickCount();
				CESERVER_REQ_HDR in;
				ExecutePrepareCmd(&in, CECMD_SETFARPID, sizeof(CESERVER_REQ_HDR));
				WARNING("Overhead and hung possibility");
				// Если ActiveServerPID() возвращает PID самого фара (current AlternativeServer) - то это overhead,
				// т.к. альт.сервер крутится в ЭТОМ же потоке, и его можно "позвать" напрямую.
				// Но дергать здесь ExecuteSrvCmd(gpConMapInfo->nServerPID) - НЕЛЬЗЯ, т.к.
				// в этом случае будет рассинхронизация серверных потоков этого процесса,
				// в итоге nActiveFarPID может никогда не обновиться...
				// Возможность подвисания - это если в nAltServerPID будет "зависший" или закрывающийся процесс (не мы).
				CESERVER_REQ *pOut = ExecuteSrvCmd(gpConMapInfo->ActiveServerPID(), (CESERVER_REQ*)&in, FarHwnd);
				if (pOut)
					ExecuteFreeResult(pOut);
			}
			else
			{
				DWORD dwCurTick = GetTickCount();

				if ((dwCurTick - dwLastTickCount) >= CHECK_FARINFO_INTERVAL)
				{
					bNeedReload = true;
					dwLastTickCount = dwCurTick;
				}
			}
		}

		if (bNeedReload)
		{
			//ReloadFarInfo(FALSE);
			gbNeedPostReloadFarInfo = TRUE;
		}
	}

	if (gbNeedPostReloadFarInfo || gbNeedPostEditCheck || gbRequestUpdateTabs || gbNeedPostTabSend || gbNeedBgActivate)
	{
		lbNeedSynchro = true;
	}

	// В некоторых случаях (CMD_LEFTCLKSYNC,CMD_CLOSEQSEARCH,...) нужно дождаться, пока очередь опустеет
	if (gbWaitConsoleInputEmpty)
	{
		DWORD nTestEvents = 0;
		HANDLE h = GetStdHandle(STD_INPUT_HANDLE);

		if (GetNumberOfConsoleInputEvents(h, &nTestEvents))
		{
			if (nTestEvents == 0)
			{
				gbWaitConsoleInputEmpty = FALSE;
				SetEvent(ghConsoleInputEmpty);
			}
		}
	}

	if (IS_SYNCHRO_ALLOWED)
	{
		// Требуется дернуть Synchro, чтобы корректно активироваться
		if (lbNeedSynchro && !gbInputSynchroPending)
		{
			gbInputSynchroPending = true;
			ExecuteSynchro();
		}
	}
	else
	{
		// Для Far1 зовем сразу
		_ASSERTE(gFarVersion.dwVerMajor == 1);
		OnMainThreadActivated();
	}
}

#ifdef _DEBUG
BOOL DebugGetKeyboardState(LPBYTE pKeyStates)
{
	short v = 0;
	BYTE b = 0;
	int nKeys[] = {VK_SHIFT,VK_LSHIFT,VK_RSHIFT,
	               VK_MENU,VK_LMENU,VK_RMENU,
	               VK_CONTROL,VK_LCONTROL,VK_RCONTROL,
	               VK_LWIN,VK_RWIN,
	               VK_CAPITAL,VK_NUMLOCK,VK_SCROLL
	              };
	int nKeyCount = sizeof(nKeys)/sizeof(nKeys[0]);

	for(int i=0; i<nKeyCount; i++)
	{
		v = GetAsyncKeyState(nKeys[i]);
		b = v & 1;

		if ((v & 0x8000) == 0x8000)
			b |= 0x80;

		pKeyStates[nKeys[i]] = b;
	}

	return TRUE;
}

typedef BOOL (__stdcall *FGetConsoleKeyboardLayoutName)(wchar_t*);
FGetConsoleKeyboardLayoutName pfnGetConsoleKeyboardLayoutName = NULL;

DWORD DebugCheckKeyboardLayout()
{
	DWORD dwLayout = 0x04090409;

	if (!pfnGetConsoleKeyboardLayoutName)
		pfnGetConsoleKeyboardLayoutName = (FGetConsoleKeyboardLayoutName)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetConsoleKeyboardLayoutNameW");

	if (pfnGetConsoleKeyboardLayoutName)
	{
		wchar_t szCurKeybLayout[KL_NAMELENGTH+1];

		if (pfnGetConsoleKeyboardLayoutName(szCurKeybLayout))
		{
			wchar_t *pszEnd = szCurKeybLayout+8;
			dwLayout = wcstoul(szCurKeybLayout, &pszEnd, 16);
		}
	}

	return dwLayout;
}

void __INPUT_RECORD_Dump(INPUT_RECORD *rec, wchar_t* pszRecord);
void DebugInputPrint(INPUT_RECORD r)
{
	static wchar_t szDbg[1100]; szDbg[0] = 0;
	SYSTEMTIME st; GetLocalTime(&st);
	_wsprintf(szDbg, SKIPLEN(countof(szDbg)) L"%02i:%02i:%02i.%03i ", st.wHour,st.wMinute,st.wSecond,st.wMilliseconds);
	__INPUT_RECORD_Dump(&r, szDbg+13);
	lstrcatW(szDbg, L"\n");
	DEBUGSTR(szDbg);
}
#endif





BOOL OnPanelViewCallbacks(HookCallbackArg* pArgs, PanelViewInputCallback pfnLeft, PanelViewInputCallback pfnRight)
{
	if (!pArgs->bMainThread || !(pfnLeft || pfnRight))
	{
		_ASSERTE(pArgs->bMainThread && (pfnLeft || pfnRight));
		return TRUE; // перехват делаем только в основной нити
	}

	BOOL lbNewResult = FALSE, lbContinue = TRUE;
	HANDLE hInput = (HANDLE)(pArgs->lArguments[0]);
	PINPUT_RECORD p = (PINPUT_RECORD)(pArgs->lArguments[1]);
	DWORD nBufSize = (DWORD)(pArgs->lArguments[2]);
	LPDWORD pCount = (LPDWORD)(pArgs->lArguments[3]);

	if (lbContinue && pfnLeft)
	{
		_ASSERTE(gPanelRegLeft.bRegister);
		lbContinue = pfnLeft(hInput,p,nBufSize,pCount,&lbNewResult);

		if (!lbContinue)
			*((BOOL*)pArgs->lpResult) = lbNewResult;
	}

	// Если есть только правая панель, или на правой панели задана другая функция
	if (lbContinue && pfnRight && pfnRight != pfnLeft)
	{
		_ASSERTE(gPanelRegRight.bRegister);
		lbContinue = pfnRight(hInput,p,nBufSize,pCount,&lbNewResult);

		if (!lbContinue)
			*((BOOL*)pArgs->lpResult) = lbNewResult;
	}

	return lbContinue;
}


VOID WINAPI OnShellExecuteExW_Except(HookCallbackArg* pArgs)
{
	if (pArgs->bMainThread)
	{
		ShowMessage(CEShellExecuteException,0);
	}

	*((LPBOOL*)pArgs->lpResult) = FALSE;
	SetLastError(E_UNEXPECTED);
}


// Для определения "живости" фара
VOID WINAPI OnGetNumberOfConsoleInputEventsPost(HookCallbackArg* pArgs)
{
	if (pArgs->bMainThread && gpConMapInfo && gpFarInfo && gpFarInfoMapping)
	{
		TouchReadPeekConsoleInputs(-1);
	}
}

BOOL UngetDummyMouseEvent(BOOL abRead, HookCallbackArg* pArgs)
{
	if (!(pArgs->lArguments[1] && pArgs->lArguments[2] && pArgs->lArguments[3]))
	{
		_ASSERTE(pArgs->lArguments[1] && pArgs->lArguments[2] && pArgs->lArguments[3]);
	}
	else if ((gLastMouseReadEvent.dwButtonState & (RIGHTMOST_BUTTON_PRESSED|FROM_LEFT_1ST_BUTTON_PRESSED)) || (gnDummyMouseEventFromMacro > 0))
	{
		// Такой финт нужен только в случае:
		// в редакторе идет скролл мышкой (скролл - зажатой кнопкой на заголовке/кейбаре)
		// нужно заставить фар остановить скролл, иначе активация Synchro невозможна

		// Или второй случай
		//FAR BUGBUG: Макрос не запускается на исполнение, пока мышкой не дернем :(
		//  Это чаще всего проявляется при вызове меню по RClick
		//  Если курсор на другой панели, то RClick сразу по пассивной
		//  не вызывает отрисовку :(

		if ((gnAllowDummyMouseEvent < 1) && (gnDummyMouseEventFromMacro < 1))
		{
			_ASSERTE(gnAllowDummyMouseEvent >= 1);
			if (gnAllowDummyMouseEvent < 0)
				gnAllowDummyMouseEvent = 0;
			gbUngetDummyMouseEvent = FALSE;
			return FALSE;
		}

		// Сообщить в GUI что мы "пустое" сообщение фару кидаем
		if (gFarMode.bFarHookMode && gFarMode.bMonitorConsoleInput)
		{
			CESERVER_REQ *pIn = ExecuteNewCmd(CECMD_PEEKREADINFO, sizeof(CESERVER_REQ_HDR)+sizeof(CESERVER_REQ_PEEKREADINFO));
			if (pIn)
			{
				pIn->PeekReadInfo.nCount = (WORD)1;
				pIn->PeekReadInfo.cPeekRead = '*';
				pIn->PeekReadInfo.cUnicode = 'U';
				pIn->PeekReadInfo.h = (HANDLE)pArgs->lArguments[1];
				pIn->PeekReadInfo.nTID = GetCurrentThreadId();
				pIn->PeekReadInfo.nPID = GetCurrentProcessId();
				pIn->PeekReadInfo.bMainThread = (pIn->PeekReadInfo.nTID == gnMainThreadId);

				pIn->PeekReadInfo.Buffer->EventType = MOUSE_EVENT;
				pIn->PeekReadInfo.Buffer->Event.MouseEvent = gLastMouseReadEvent;
				pIn->PeekReadInfo.Buffer->Event.MouseEvent.dwButtonState = 0;
				pIn->PeekReadInfo.Buffer->Event.MouseEvent.dwEventFlags = MOUSE_MOVED;

				CESERVER_REQ* pOut = ExecuteGuiCmd(FarHwnd, pIn, FarHwnd);
				if (pOut) ExecuteFreeResult(pOut);
				ExecuteFreeResult(pIn);
			}
		}

		PINPUT_RECORD p = (PINPUT_RECORD)(pArgs->lArguments[1]);
		LPDWORD pCount = (LPDWORD)(pArgs->lArguments[3]);
		*pCount = 1;
		p->EventType = MOUSE_EVENT;
		p->Event.MouseEvent = gLastMouseReadEvent;
		p->Event.MouseEvent.dwButtonState = 0;
		p->Event.MouseEvent.dwEventFlags = MOUSE_MOVED;
		*((LPBOOL)pArgs->lpResult) = TRUE;

		if ((gnDummyMouseEventFromMacro > 0) && abRead)
		{
			TODO("А если в очередь фара закинуто несколько макросов? По одному мышиному события выполнится только один, или все?");
			//InterlockedDecrement(&gnDummyMouseEventFromMacro);
			gnDummyMouseEventFromMacro = 0;
		}

		return TRUE;
	}
	else
	{
		gbUngetDummyMouseEvent = FALSE; // Не требуется, фар сам кнопку "отпустил"
	}
	return FALSE;
}

// Если функция возвращает FALSE - реальный ReadConsoleInput вызван не будет,
// и в вызывающую функцию (ФАРа?) вернется то, что проставлено в pArgs->lpResult & pArgs->lArguments[...]
BOOL WINAPI OnConsolePeekInput(HookCallbackArg* pArgs)
{
	if (!pArgs->bMainThread)
		return TRUE;  // обработку делаем только в основной нити

	if (gbUngetDummyMouseEvent)
	{
		if (UngetDummyMouseEvent(FALSE, pArgs))
			return FALSE; // реальный ReadConsoleInput вызван не будет
	}

	//// Выставить флажок "Жив" можно и при вызове из плагина
	//if (gpConMapInfo && gpFarInfo && gpFarInfoMapping)
	//	TouchReadPeekConsoleInputs(1);

	//if (pArgs->IsExecutable != HEO_Executable)
	//	return TRUE;  // и только при вызове из far.exe

	if (pArgs->lArguments[2] == 1)
	{
		OnConsolePeekReadInput(TRUE/*abPeek*/);
	}

	// Если зарегистрирован callback для графической панели
	if (gPanelRegLeft.pfnPeekPreCall || gPanelRegRight.pfnPeekPreCall)
	{
		// Если функция возвращает FALSE - реальное чтение не будет вызвано
		if (!OnPanelViewCallbacks(pArgs, gPanelRegLeft.pfnPeekPreCall, gPanelRegRight.pfnPeekPreCall))
			return FALSE;
	}

	return TRUE; // продолжить
}

VOID WINAPI OnConsolePeekInputPost(HookCallbackArg* pArgs)
{
	if (!pArgs->bMainThread) return;  // обработку делаем только в основной нити

#ifdef _DEBUG

	if (*(LPDWORD)(pArgs->lArguments[3]))
	{
		wchar_t szDbg[255];
		PINPUT_RECORD p = (PINPUT_RECORD)(pArgs->lArguments[1]);
		LPDWORD pCount = (LPDWORD)(pArgs->lArguments[3]);
		DWORD nLeft = 0; GetNumberOfConsoleInputEvents(GetStdHandle(STD_INPUT_HANDLE), &nLeft);
		_wsprintf(szDbg, SKIPLEN(countof(szDbg)) L"*** OnConsolePeekInputPost(Events=%i, KeyCount=%i, LeftInConBuffer=%i)\n",
		          *pCount, (p->EventType==KEY_EVENT) ? p->Event.KeyEvent.wRepeatCount : 0, nLeft);
		DEBUGSTRINPUT(szDbg);

		// Если под дебагом включен ScrollLock - вывести информацию о считанных событиях
		if (GetKeyState(VK_SCROLL) & 1)
		{
			PINPUT_RECORD p = (PINPUT_RECORD)(pArgs->lArguments[1]);
			LPDWORD pCount = (LPDWORD)(pArgs->lArguments[3]);
			_ASSERTE(*pCount <= pArgs->lArguments[2]);
			UINT nCount = *pCount;

			for(UINT i = 0; i < nCount; i++)
				DebugInputPrint(p[i]);
		}
	}

#endif

	// Если зарегистрирован callback для графической панели
	if (gPanelRegLeft.pfnPeekPostCall || gPanelRegRight.pfnPeekPostCall)
	{
		// Если функция возвращает FALSE - реальное чтение не будет вызвано
		if (!OnPanelViewCallbacks(pArgs, gPanelRegLeft.pfnPeekPostCall, gPanelRegRight.pfnPeekPostCall))
			return;
	}
}

// Если функция возвращает FALSE - реальный ReadConsoleInput вызван не будет,
// и в вызывающую функцию (ФАРа?) вернется то, что проставлено в pArgs->lpResult & pArgs->lArguments[...]
BOOL OnConsoleReadInputWork(HookCallbackArg* pArgs)
{
	if (!pArgs->bMainThread)
		return TRUE;  // обработку делаем только в основной нити

	if (gbUngetDummyMouseEvent)
	{
		if (UngetDummyMouseEvent(TRUE, pArgs))
		{
			gbUngetDummyMouseEvent = FALSE;
			gLastMouseReadEvent.dwButtonState = 0; // будем считать, что "мышиную блокировку" успешно сняли
			return FALSE; // реальный ReadConsoleInput вызван не будет
		}
		_ASSERTE(gbUngetDummyMouseEvent == FALSE);
	}

	//// Выставить флажок "Жив" можно и при вызове из плагина
	//if (gpConMapInfo && gpFarInfo && gpFarInfoMapping)
	//	TouchReadPeekConsoleInputs(0);
	//
	//if (pArgs->IsExecutable != HEO_Executable)
	//	return TRUE;  // и только при вызове из far.exe

	if (pArgs->lArguments[2] == 1)
	{
		OnConsolePeekReadInput(FALSE/*abPeek*/);
	}

	// Если зарегистрирован callback для графической панели
	if (gPanelRegLeft.pfnReadPreCall || gPanelRegRight.pfnReadPreCall)
	{
		// Если функция возвращает FALSE - реальное чтение не будет вызвано
		if (!OnPanelViewCallbacks(pArgs, gPanelRegLeft.pfnReadPreCall, gPanelRegRight.pfnReadPreCall))
		{
			// это вызвается перед реальным чтением - информация может быть разве что от "PanelViews"
			// Если под дебагом включен ScrollLock - вывести информацию о считанных событиях
			#ifdef _DEBUG
			if (GetKeyState(VK_SCROLL) & 1)
			{
				PINPUT_RECORD p = (PINPUT_RECORD)(pArgs->lArguments[1]);
				LPDWORD pCount = (LPDWORD)(pArgs->lArguments[3]);
				_ASSERTE(*pCount <= pArgs->lArguments[2]);
				UINT nCount = *pCount;

				for (UINT i = 0; i < nCount; i++)
					DebugInputPrint(p[i]);
			}
			#endif

			return FALSE;
		}
	}

	return TRUE; // продолжить
}

BOOL WINAPI OnConsoleReadInput(HookCallbackArg* pArgs)
{
	return OnConsoleReadInputWork(pArgs);
}

VOID WINAPI OnConsoleReadInputPost(HookCallbackArg* pArgs)
{
	if (!pArgs->bMainThread) return;  // обработку делаем только в основной нити

#ifdef _DEBUG
	{
		wchar_t szDbg[255];
		PINPUT_RECORD p = (PINPUT_RECORD)(pArgs->lArguments[1]);
		LPDWORD pCount = (LPDWORD)(pArgs->lArguments[3]);
		DWORD nLeft = 0; GetNumberOfConsoleInputEvents(GetStdHandle(STD_INPUT_HANDLE), &nLeft);
		_wsprintf(szDbg, SKIPLEN(countof(szDbg)) L"*** OnConsoleReadInputPost(Events=%i, KeyCount=%i, LeftInConBuffer=%i)\n",
		          *pCount, (p->EventType==KEY_EVENT) ? p->Event.KeyEvent.wRepeatCount : 0, nLeft);
		//if (*pCount) {
		//	wsprintfW(szDbg+lstrlen(szDbg), L", type=%i", p->EventType);
		//	if (p->EventType == MOUSE_EVENT) {
		//		wsprintfW(L", {%ix%i} BtnState:0x%08X, CtrlState:0x%08X, Flags:0x%08X",
		//			p->Event.MouseEvent.dwMousePosition.X, p->Event.MouseEvent.dwMousePosition.Y,
		//			p->Event.MouseEvent.dwButtonState, p->Event.MouseEvent.dwControlKeyState,
		//			p->Event.MouseEvent.dwEventFlags);
		//	} else if (p->EventType == KEY_EVENT) {
		//		wsprintfW(L", '%c' %s count=%i, VK=%i, SC=%i, CH=\\x%X, State=0x%08x %s",
		//			(p->Event.KeyEvent.uChar.UnicodeChar > 0x100) ? L'?' :
		//			(p->Event.KeyEvent.uChar.UnicodeChar
		//			? p->Event.KeyEvent.uChar.UnicodeChar : L' '),
		//			p->Event.KeyEvent.bKeyDown ? L"Down," : L"Up,  ",
		//			p->Event.KeyEvent.wRepeatCount,
		//			p->Event.KeyEvent.wVirtualKeyCode,
		//			p->Event.KeyEvent.wVirtualScanCode,
		//			p->Event.KeyEvent.uChar.UnicodeChar,
		//			p->Event.KeyEvent.dwControlKeyState,
		//			(p->Event.KeyEvent.dwControlKeyState & ENHANCED_KEY) ?
		//			L"<Enhanced>" : L"");
		//	}
		//}
		//lstrcatW(szDbg, L")\n");
		DEBUGSTRINPUT(szDbg);

		// Если под дебагом включен ScrollLock - вывести информацию о считанных событиях
		if (GetKeyState(VK_SCROLL) & 1)
		{
			PINPUT_RECORD p = (PINPUT_RECORD)(pArgs->lArguments[1]);
			LPDWORD pCount = (LPDWORD)(pArgs->lArguments[3]);
			_ASSERTE(*pCount <= pArgs->lArguments[2]);
			UINT nCount = *pCount;

			for(UINT i = 0; i < nCount; i++)
				DebugInputPrint(p[i]);
		}
	}
#endif

	HANDLE h = (HANDLE)(pArgs->lArguments[0]);
	PINPUT_RECORD p = (PINPUT_RECORD)(pArgs->lArguments[1]);
	LPDWORD pCount = (LPDWORD)(pArgs->lArguments[3]);

	//Чтобы не было зависаний при попытке активации плагина во время прокрутки
	//редактора, в плагине мониторить нажатие мыши. Если последнее МЫШИНОЕ событие
	//было с нажатой кнопкой - сначала пульнуть в консоль команду "отпускания" кнопки,
	//и только после этого - пытаться активироваться.
	if (pCount && *pCount)
	{
		for (int i = (*pCount) - 1; i >= 0; i--)
		{
			if (p[i].EventType == MOUSE_EVENT)
			{
				gLastMouseReadEvent = p[i].Event.MouseEvent;
				break;
			}
		}
	}

	// Если зарегистрирован callback для графической панели
	if (gPanelRegLeft.pfnReadPostCall || gPanelRegRight.pfnReadPostCall)
	{
		if (!OnPanelViewCallbacks(pArgs, gPanelRegLeft.pfnReadPostCall, gPanelRegRight.pfnReadPostCall))
			return;
	}

	// Чтобы ФАР сразу прекратил ходить по каталогам при отпускании Enter
	if (h != NULL)
	{
		if (*pCount == 1 && p->EventType == KEY_EVENT && p->Event.KeyEvent.bKeyDown
		        && (p->Event.KeyEvent.wVirtualKeyCode == VK_RETURN
		            || p->Event.KeyEvent.wVirtualKeyCode == VK_NEXT
		            || p->Event.KeyEvent.wVirtualKeyCode == VK_PRIOR)
		  )
		{
			INPUT_RECORD ir[10]; DWORD nRead = 0, nInc = 0;

			if (PeekConsoleInputW(h, ir, countof(ir), &nRead) && nRead)
			{
				for(DWORD n = 0; n < nRead; n++)
				{
					if (ir[n].EventType == KEY_EVENT && ir[n].Event.KeyEvent.bKeyDown
					        && ir[n].Event.KeyEvent.wVirtualKeyCode == p->Event.KeyEvent.wVirtualKeyCode
					        && ir[n].Event.KeyEvent.dwControlKeyState == p->Event.KeyEvent.dwControlKeyState)
					{
						nInc++;
					}
					else
					{
						break; // дубли в буфере кончились
					}
				}

				if (nInc > 0)
				{
					if (ReadConsoleInputW(h, ir, nInc, &nRead) && nRead)
					{
						p->Event.KeyEvent.wRepeatCount += (WORD)nRead;
					}
				}
			}
		}
	}
}


BOOL WINAPI OnWriteConsoleOutput(HookCallbackArg* pArgs)
{
	if (!pArgs->bMainThread)
		return TRUE;  // обработку делаем только в основной нити
	//if (pArgs->IsExecutable != HEO_Executable)
	//	return TRUE;  // и только при вызове из far.exe

	// Если зарегистрирован callback для графической панели
	if (gPanelRegLeft.pfnWriteCall || gPanelRegRight.pfnWriteCall)
	{
		HANDLE hOutput = (HANDLE)(pArgs->lArguments[0]);
		const CHAR_INFO *lpBuffer = (const CHAR_INFO *)(pArgs->lArguments[1]);
		COORD dwBufferSize = *(COORD*)(pArgs->lArguments[2]);
		COORD dwBufferCoord = *(COORD*)(pArgs->lArguments[3]);
		PSMALL_RECT lpWriteRegion = (PSMALL_RECT)(pArgs->lArguments[4]);

		if (gPanelRegLeft.pfnWriteCall)
		{
			_ASSERTE(gPanelRegLeft.bRegister);
			gPanelRegLeft.pfnWriteCall(hOutput,lpBuffer,dwBufferSize,dwBufferCoord,lpWriteRegion);
		}

		// Если есть только правая панель, или на правой панели задана другая функция
		if (gPanelRegRight.pfnWriteCall && gPanelRegRight.pfnWriteCall != gPanelRegLeft.pfnWriteCall)
		{
			_ASSERTE(gPanelRegRight.bRegister);
			gPanelRegRight.pfnWriteCall(hOutput,lpBuffer,dwBufferSize,dwBufferCoord,lpWriteRegion);
		}
	}

	//if (gpBgPlugin)
	//	gpBgPlugin->SetForceCheck();

	if (gbWaitConsoleWrite)
	{
		gbWaitConsoleWrite = FALSE;
		SetEvent(ghConsoleWrite);
	}

	return TRUE;
}

/* COMMON - end */



void WINAPI ExitFARW(void);
void WINAPI ExitFARW3(void*);

#include "../common/SetExport.h"
ExportFunc Far3Func[] =
{
	{"ExitFARW", (void*)ExitFARW, (void*)ExitFARW3},
	{"ProcessEditorEventW", (void*)ProcessEditorEventW, (void*)ProcessEditorEventW3},
	{"ProcessViewerEventW", (void*)ProcessViewerEventW, (void*)ProcessViewerEventW3},
	{"ProcessSynchroEventW", (void*)ProcessSynchroEventW, (void*)ProcessSynchroEventW3},
	{NULL}
};

bool gbExitFarCalled = false;

BOOL WINAPI DllMain(HANDLE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	switch(ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
		{
			ghPluginModule = (HMODULE)hModule;
			ghWorkingModule = (u64)hModule;
			gnSelfPID = GetCurrentProcessId();
			HeapInitialize();
			_ASSERTE(FAR_X_VER<FAR_Y1_VER && FAR_Y1_VER<FAR_Y2_VER);
#ifdef SHOW_STARTED_MSGBOX

			if (!IsDebuggerPresent())
				MessageBoxA(NULL, "ConEmu*.dll loaded", "ConEmu plugin", 0);

#endif
			//#if defined(__GNUC__)
			//GetConsoleWindow = (FGetConsoleWindow)GetProcAddress(GetModuleHandle(L"kernel32.dll"),"GetConsoleWindow");
			//#endif
			gpLocalSecurity = LocalSecurity();
			csTabs = new MSection();
			csData = new MSection();
			gPanelDirs.ActiveDir = new CmdArg();
			gPanelDirs.PassiveDir = new CmdArg();
			PlugServerInit();
			//HWND hConWnd = GetConEmuHWND(2);
			// Текущая нить не обязана быть главной! Поэтому ищем первую нить процесса!
			gnMainThreadId = gnMainThreadIdInitial = GetMainThreadId();
			CPluginBase::InitHWND(/*hConWnd*/);
			//TODO("перенести инициализацию фаровских callback'ов в SetStartupInfo, т.к. будет грузиться как Inject!");
			//if (!StartupHooks(ghPluginModule)) {
			//	_ASSERTE(FALSE);
			//	DEBUGSTR(L"!!! Can't install injects!!!\n");
			//}
			// Check Terminal mode
			TerminalMode = isTerminalMode();
			//TCHAR szVarValue[MAX_PATH];
			//szVarValue[0] = 0;
			//if (GetEnvironmentVariable(L"TERM", szVarValue, 63)) {
			//    TerminalMode = TRUE;
			//}
			//2010-01-29 ConMan давно не поддерживается - все встроено
			//if (!TerminalMode) {
			//	// FarHints fix for multiconsole mode...
			//	if (GetModuleFileName((HMODULE)hModule, szVarValue, MAX_PATH)) {
			//		WCHAR *pszSlash = wcsrchr(szVarValue, L'\\');
			//		if (pszSlash) pszSlash++; else pszSlash = szVarValue;
			//		lstrcpyW(pszSlash, L"infis.dll");
			//		ghFarHintsFix = LoadLibrary(szVarValue);
			//	}
			//}

			if (!TerminalMode)
			{
				if (!StartupHooks(ghPluginModule))
				{
					if (ghConEmuWndDC)
					{
						_ASSERTE(FALSE);
						DEBUGSTR(L"!!! Can't install injects!!!\n");
					}
					else
					{
						DEBUGSTR(L"No GUI, injects was not installed!\n");
					}
				}
			}
		}
		break;
		case DLL_PROCESS_DETACH:
			CPluginBase::ShutdownPluginStep(L"DLL_PROCESS_DETACH");

			if (!gbExitFarCalled)
			{
				_ASSERTE(FALSE && "ExitFar was not called. Unsupported Far<->Plugin builds?");
				Plugin()->ExitFarCommon();
			}

			if (gnSynchroCount > 0)
			{
				//if (gFarVersion.dwVerMajor == 2 && gFarVersion.dwBuild < 1735) -- в фаре пока не чинили, поэтому всегда ругаемся, если что...
				BOOL lbSynchroSafe = FALSE;
				if ((gFarVersion.dwVerMajor == 2 && gFarVersion.dwVerMinor >= 1) || (gFarVersion.dwVerMajor >= 3))
					lbSynchroSafe = TRUE;
				if (!lbSynchroSafe)
				{
					MessageBox(NULL, L"Syncho events are pending!\nFar may crash after unloading plugin", L"ConEmu plugin", MB_OK|MB_ICONEXCLAMATION|MB_SETFOREGROUND|MB_SYSTEMMODAL);
				}
			}

			//if (ghFarHintsFix) {
			//	FreeLibrary(ghFarHintsFix);
			//	ghFarHintsFix = NULL;
			//}
			if (csTabs)
			{
				delete csTabs;
				csTabs = NULL;
			}

			if (csData)
			{
				delete csData;
				csData = NULL;
			}

			PlugServerStop(true);

			if (gpBgPlugin)
			{
				delete gpBgPlugin;
				gpBgPlugin = NULL;
			}

			HeapDeinitialize();

			CPluginBase::ShutdownPluginStep(L"DLL_PROCESS_DETACH - done");
			break;
	}

	return TRUE;
}

#if defined(CRTSTARTUP)
#pragma message("!!!CRTSTARTUP defined!!!")
extern "C" {
	BOOL WINAPI _DllMainCRTStartup(HANDLE hDll,DWORD dwReason,LPVOID lpReserved);
};

BOOL WINAPI _DllMainCRTStartup(HANDLE hDll,DWORD dwReason,LPVOID lpReserved)
{
	DllMain(hDll, dwReason, lpReserved);
	return TRUE;
}
#endif


BOOL WINAPI IsConsoleActive()
{
	if (ghConEmuWndDC)
	{
		if (IsWindow(ghConEmuWndDC))
		{
			HWND hParent = GetParent(ghConEmuWndDC);

			if (hParent)
			{
				HWND hTest = (HWND)GetWindowLongPtr(hParent, GWLP_USERDATA);
				return (hTest == FarHwnd);
			}
		}
	}

	return TRUE;
}

// anConEmuOnly
//	0 - если в ConEmu - вернуть окно отрисовки, иначе - вернуть окно консоли
//	1 - вернуть окно отрисовки
//	2 - вернуть главное окно ConEmu
//	3 - вернуть окно консоли
HWND WINAPI GetFarHWND2(int anConEmuOnly)
{
	// Если просили реальное окно консоли - вернем сразу
	if (anConEmuOnly == 3)
	{
		return FarHwnd;
	}

	if (ghConEmuWndDC)
	{
		if (IsWindow(ghConEmuWndDC))
		{
			if (anConEmuOnly == 2)
				return GetConEmuHWND(1);
			return ghConEmuWndDC;
		}

		//
		ghConEmuWndDC = NULL;
		//
		SetConEmuEnvVar(NULL);
		SetConEmuEnvVarChild(NULL,NULL);
	}

	if (anConEmuOnly)
		return NULL;

	return FarHwnd;
}

HWND WINAPI GetFarHWND()
{
	return GetFarHWND2(FALSE);
}

BOOL WINAPI IsTerminalMode()
{
	return TerminalMode;
}

void WINAPI GetFarVersion(FarVersion* pfv)
{
	if (!pfv)
		return;

	*pfv = gFarVersion;
}

BOOL LoadFarVersion()
{
	wchar_t ErrText[512]; ErrText[0] = 0;
	BOOL lbRc = LoadFarVersion(gFarVersion, ErrText);

	if (ErrText[0])
	{
		MessageBox(0, ErrText, L"ConEmu plugin", MB_OK|MB_ICONSTOP|MB_SETFOREGROUND);
	}

	if (!lbRc)
	{
		gFarVersion.dwVerMajor = 2;
		gFarVersion.dwVerMinor = 0;
		gFarVersion.dwBuild = FAR_X_VER;
	}

	return lbRc;
}

int WINAPI RegisterPanelView(PanelViewInit *ppvi)
{
	if (!ppvi)
	{
		_ASSERTE(ppvi->cbSize == sizeof(PanelViewInit));
		return -2;
	}

	if (ppvi->cbSize != sizeof(PanelViewInit))
	{
		_ASSERTE(ppvi->cbSize == sizeof(PanelViewInit));
		return -2;
	}

	PanelViewRegInfo *pp = (ppvi->bLeftPanel) ? &gPanelRegLeft : &gPanelRegRight;

	if (ppvi->bRegister)
	{
		pp->pfnPeekPreCall = ppvi->pfnPeekPreCall.f;
		pp->pfnPeekPostCall = ppvi->pfnPeekPostCall.f;
		pp->pfnReadPreCall = ppvi->pfnReadPreCall.f;
		pp->pfnReadPostCall = ppvi->pfnReadPostCall.f;
		pp->pfnWriteCall = ppvi->pfnWriteCall.f;
	}
	else
	{
		pp->pfnPeekPreCall = pp->pfnPeekPostCall = pp->pfnReadPreCall = pp->pfnReadPostCall = NULL;
		pp->pfnWriteCall = NULL;
	}

	pp->bRegister = ppvi->bRegister;
	CESERVER_REQ In;
	int nSize = sizeof(CESERVER_REQ_HDR) + sizeof(In.PVI);
	ExecutePrepareCmd(&In, CECMD_REGPANELVIEW, nSize);
	In.PVI = *ppvi;
	CESERVER_REQ* pOut = ExecuteGuiCmd(FarHwnd, &In, FarHwnd);

	if (!pOut)
	{
		pp->pfnPeekPreCall = pp->pfnPeekPostCall = pp->pfnReadPreCall = pp->pfnReadPostCall = NULL;
		pp->pfnWriteCall = NULL;
		pp->bRegister = FALSE;
		return -3;
	}

	*ppvi = pOut->PVI;
	ExecuteFreeResult(pOut);

	if (ppvi->cbSize == 0)
	{
		pp->pfnPeekPreCall = pp->pfnPeekPostCall = pp->pfnReadPreCall = pp->pfnReadPostCall = NULL;
		pp->pfnWriteCall = NULL;
		pp->bRegister = FALSE;
		return -1;
	}

	return 0;
}



//struct RegisterBackgroundArg gpBgPlugin = NULL;
//int gnBgPluginsCount = 0, gnBgPluginsMax = 0;
//MSection *csBgPlugins = NULL;

int WINAPI RegisterBackground(RegisterBackgroundArg *pbk)
{
	if (!pbk)
	{
		_ASSERTE(pbk != NULL);
		return esbr_InvalidArg;
	}

	if (!gbBgPluginsAllowed)
	{
		_ASSERTE(gbBgPluginsAllowed == TRUE);
		return esbr_PluginForbidden;
	}

	if (pbk->cbSize != sizeof(*pbk))
	{
		_ASSERTE(pbk->cbSize == sizeof(*pbk));
		return esbr_InvalidArgSize;
	}

#ifdef _DEBUG

	if (pbk->Cmd == rbc_Register)
	{
		_ASSERTE(pbk->dwPlaces != 0);
	}

#endif

	if (gpBgPlugin == NULL)
	{
		gpBgPlugin = new CPluginBackground;
	}

	return gpBgPlugin->RegisterSubplugin(pbk);
}

// Возвращает TRUE в случае успешного выполнения
// (удалось активировать главную нить и выполнить в ней функцию CallBack)
// FALSE - в случае ошибки.
int WINAPI SyncExecute(HMODULE ahModule, SyncExecuteCallback_t CallBack, LONG_PTR lParam)
{
	BOOL bResult = FALSE;
	SyncExecuteArg args = {CMD__EXTERNAL_CALLBACK, ahModule, CallBack, lParam};
	bResult = ProcessCommand(CMD__EXTERNAL_CALLBACK, TRUE/*bReqMainThread*/, &args);
	return bResult;
}

// Активировать текущую консоль в ConEmu
int WINAPI ActivateConsole()
{
	CESERVER_REQ In;
	int nSize = sizeof(CESERVER_REQ_HDR) + sizeof(In.ActivateCon);
	ExecutePrepareCmd(&In, CECMD_ACTIVATECON, nSize);
	In.ActivateCon.hConWnd = FarHwnd;
	CESERVER_REQ* pOut = ExecuteGuiCmd(FarHwnd, &In, FarHwnd);

	if (!pOut)
	{
		return FALSE;
	}

	BOOL lbSucceeded = (pOut->ActivateCon.hConWnd == FarHwnd);
	ExecuteFreeResult(pOut);
	return lbSucceeded;
}

// Внимание! Теоретически, из этой функции Far2 может сразу вызвать ProcessSynchroEventW.
// Но в текущей версии Far2 она работает асинхронно и сразу выходит, а сама
// ProcessSynchroEventW зовется потом в главной нити (где-то при чтении буфера консоли)
void ExecuteSynchro()
{
	WARNING("Нет способа определить, будет ли фар вызывать наш ProcessSynchroEventW и в какой момент");
	// Например, если в фаре выставлен ProcessException - то никакие плагины больше не зовутся

	if (IS_SYNCHRO_ALLOWED)
	{
		if (gbSynchroProhibited)
		{
			_ASSERTE(gbSynchroProhibited==false);
			return;
		}

		//Чтобы не было зависаний при попытке активации плагина во время прокрутки
		//редактора, в плагине мониторить нажатие мыши. Если последнее МЫШИНОЕ событие
		//было с нажатой кнопкой - сначала пульнуть в консоль команду "отпускания" кнопки,
		//и только после этого - пытаться активироваться.
		if ((gnAllowDummyMouseEvent > 0) && (gLastMouseReadEvent.dwButtonState & (RIGHTMOST_BUTTON_PRESSED|FROM_LEFT_1ST_BUTTON_PRESSED)))
		{
			//_ASSERTE(!(gLastMouseReadEvent.dwButtonState & (RIGHTMOST_BUTTON_PRESSED|FROM_LEFT_1ST_BUTTON_PRESSED)));
			int nWindowType = Plugin()->GetActiveWindowType();
			// "Зависания" возможны (вроде) только при прокрутке зажатой кнопкой мышки
			// редактора или вьювера. Так что в других областях - не дергаться.
			if (nWindowType == WTYPE_EDITOR || nWindowType == WTYPE_VIEWER)
			{
				gbUngetDummyMouseEvent = TRUE;
			}
		}

		//psi.AdvControl(psi.ModuleNumber,ACTL_SYNCHRO,NULL);
		Plugin()->ExecuteSynchro();
	}
}

static DWORD WaitPluginActivateion(DWORD nCount, HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds)
{
	DWORD nWait = WAIT_TIMEOUT;
	if (IS_SYNCHRO_ALLOWED)
	{
		DWORD nStepWait = 1000;
		DWORD nPrevCount = gnPeekReadCount;

		#ifdef _DEBUG
		if (IsDebuggerPresent())
		{
			nStepWait = 30000;
			if (dwMilliseconds && (dwMilliseconds < 60000))
				dwMilliseconds = 60000;
		}
		#endif

		DWORD nStartTick = GetTickCount(), nCurrentTick = 0;
		DWORD nTimeout = nStartTick + dwMilliseconds;
		do {
			nWait = WaitForMultipleObjects(nCount, lpHandles, bWaitAll, min(dwMilliseconds,nStepWait));
			if (((nWait >= WAIT_OBJECT_0) && (nWait < (WAIT_OBJECT_0+nCount))) || (nWait != WAIT_TIMEOUT))
			{
				_ASSERTE((nWait >= WAIT_OBJECT_0) && (nWait < (WAIT_OBJECT_0+nCount)));
				break; // Succeded
			}

			nCurrentTick = GetTickCount();

			if ((nWait == WAIT_TIMEOUT) && (nPrevCount == gnPeekReadCount) && (dwMilliseconds > 1000)
				#ifdef _DEBUG
				&& (!IsDebuggerPresent() || (nCurrentTick > (nStartTick + nStepWait)))
				#endif
				)
			{
				// Ждать дальше смысла видимо нет, фар не дергает (Peek/Read)Input
				break;
			}
			// Если вдруг произошел облом с Syncho (почему?), дернем еще раз
			ExecuteSynchro();
		} while (dwMilliseconds && ((dwMilliseconds == INFINITE) || (nCurrentTick <= nTimeout)));

		#ifdef _DEBUG
		if (nWait == WAIT_TIMEOUT)
		{
			DEBUGSTRACTIVATE(L"ConEmu plugin activation failed");
		}
		#endif
	}
	else
	{
		nWait = WaitForMultipleObjects(nCount, lpHandles, bWaitAll, dwMilliseconds);
	}
	return nWait;
}

// Должна вызываться ТОЛЬКО из нитей уже заблокированных семафором ghPluginSemaphore
static BOOL ActivatePlugin(
    DWORD nCmd, LPVOID pCommandData,
    DWORD nTimeout = CONEMUFARTIMEOUT // Release=10сек, Debug=2мин.
)
{
	BOOL lbRc = FALSE;
	ResetEvent(ghReqCommandEvent);
	//gbCmdCallObsolete = FALSE;
	gnReqCommand = nCmd; gpReqCommandData = pCommandData;
	gnPluginOpenFrom = -1;
	// Нужен вызов плагина в остновной нити
	gbReqCommandWaiting = TRUE;
	DWORD nWait = 100; // если тут останется (!=0) - функция вернут ошибку
	HANDLE hEvents[] = {ghServerTerminateEvent, ghReqCommandEvent};
	int nCount = countof(hEvents);
	DEBUGSTRMENU(L"*** Waiting for plugin activation\n");

	if (nCmd == CMD_REDRAWFAR || nCmd == CMD_FARPOST)
	{
		WARNING("Оптимизировать!");
		nTimeout = min(1000,nTimeout); // чтобы не зависало при попытке ресайза, если фар не отзывается.
	}

	if (gbSynchroProhibited)
	{
		nWait = WAIT_TIMEOUT;
	}
	// Если есть ACTL_SYNCHRO - позвать его, иначе - "активация" в главной нити
	// выполняется тогда, когда фар зовет ReadConsoleInput(1).
	//if (gFarVersion.dwVerMajor = 2 && gFarVersion.dwBuild >= 1006)
	else if (IS_SYNCHRO_ALLOWED)
	{
		#ifdef _DEBUG
		int iArea = Plugin()->GetMacroArea();
		#endif

		InterlockedIncrement(&gnAllowDummyMouseEvent);
		ExecuteSynchro();

		if (!gbUngetDummyMouseEvent && gLastMouseReadEvent.dwButtonState & (RIGHTMOST_BUTTON_PRESSED|FROM_LEFT_1ST_BUTTON_PRESSED))
		{
			// Страховка от зависаний
			nWait = WaitForMultipleObjects(nCount, hEvents, FALSE, min(1000,max(250,nTimeout)));
			if (nWait == WAIT_TIMEOUT)
			{
				if (!gbUngetDummyMouseEvent && gLastMouseReadEvent.dwButtonState & (RIGHTMOST_BUTTON_PRESSED|FROM_LEFT_1ST_BUTTON_PRESSED))
				{
					gbUngetDummyMouseEvent = TRUE;
					// попытаться еще раз
					nWait = WaitPluginActivateion(nCount, hEvents, FALSE, nTimeout);
				}
			}
		}
		else
		{
			// Подождать активации. Сколько ждать - может указать вызывающая функция
			nWait = WaitPluginActivateion(nCount, hEvents, FALSE, nTimeout);
		}

		if (gnAllowDummyMouseEvent > 0)
		{
			InterlockedDecrement(&gnAllowDummyMouseEvent);
		}
		else
		{
			_ASSERTE(gnAllowDummyMouseEvent >= 0);
			if (gnAllowDummyMouseEvent < 0)
				gnAllowDummyMouseEvent = 0;
		}

	}
	else
	{
		// Подождать активации. Сколько ждать - может указать вызывающая функция
		nWait = WaitPluginActivateion(nCount, hEvents, FALSE, nTimeout);
	}


	if (nWait != WAIT_OBJECT_0 && nWait != (WAIT_OBJECT_0+1))
	{
		//110712 - если CMD_REDRAWFAR, то показывать Assert смысла мало, фар может быть занят
		//  например чтением панелей?
		//На CMD_SETWINDOW тоже ругаться не будем - окошко может быть заблокировано, или фар занят.
		_ASSERTE(nWait==WAIT_OBJECT_0 || (nCmd==CMD_REDRAWFAR) || (nCmd==CMD_SETWINDOW));

		if (nWait == (WAIT_OBJECT_0+1))
		{
			if (!gbReqCommandWaiting)
			{
				// Значит плагин в основной нити все-таки активировался, подождем еще?
				DEBUGSTR(L"!!! Plugin execute timeout !!!\n");
				nWait = WaitForMultipleObjects(nCount, hEvents, FALSE, nTimeout);
			}

			//// Таймаут, эту команду плагин должен пропустить, когда фар таки соберется ее выполнить
			//Param->Obsolete = TRUE;
		}
	}
	else
	{
		DEBUGSTRMENU(L"*** DONE\n");
	}

	lbRc = (nWait == (WAIT_OBJECT_0+1));

	if (!lbRc)
	{
		// Сразу сбросим, вдруг не дождались?
		gbReqCommandWaiting = FALSE;
		ResetEvent(ghReqCommandEvent);
	}

	gpReqCommandData = NULL;
	gnReqCommand = -1; gnPluginOpenFrom = -1;
	return lbRc;
}

typedef HANDLE(WINAPI *OpenPlugin_t)(int OpenFrom,INT_PTR Item);

WARNING("Обязательно сделать возможность отваливаться по таймауту, если плагин не удалось активировать");
// Проверку можно сделать чтением буфера ввода - если там еще есть событие отпускания F11 - значит
// меню плагинов еще загружается. Иначе можно еще чуть-чуть подождать, и отваливаться - активироваться не получится
BOOL ProcessCommand(DWORD nCmd, BOOL bReqMainThread, LPVOID pCommandData, CESERVER_REQ** ppResult /*= NULL*/, bool bForceSendTabs /*= false*/)
{
	BOOL lbSucceeded = FALSE;
	CESERVER_REQ* pCmdRet = NULL;

	if (ppResult)  // сначала - сбросить
		*ppResult = NULL;

	// Некоторые команды можно выполнять в любой нити
	if (nCmd == CMD_SET_CON_FONT || nCmd == CMD_GUICHANGED)
	{
		bReqMainThread = FALSE;
	}

	//Это нужно делать только тогда, когда семафор уже заблокирован!
	//if (gpCmdRet) { Free(gpCmdRet); gpCmdRet = NULL; }
	//gpData = NULL; gpCursor = NULL;
	WARNING("Тут нужно сделать проверку содержимого консоли");
	// Если отображено меню - плагин не запустится
	// Не перепутать меню с пустым экраном (Ctrl-O)

	if (bReqMainThread && (gnMainThreadId != GetCurrentThreadId()))
	{
		_ASSERTE(ghPluginSemaphore!=NULL);
		_ASSERTE(ghServerTerminateEvent!=NULL);

		// Issue 198: Redraw вызывает отрисовку фаром (1.7x) UserScreen-a (причем без кейбара)
		if (gFarVersion.dwVerMajor < 2 && nCmd == CMD_REDRAWFAR)
		{
			return FALSE; // лучше его просто пропустить
		}

		if (nCmd == CMD_FARPOST)
		{
			return FALSE; // Это просто проверка, что фар отработал цикл
		}

		// Запомним, чтобы знать, были ли созданы данные?
		#ifdef _DEBUG
		CESERVER_REQ* pOldCmdRet = gpCmdRet;
		#endif

		//// Некоторые команды можно выполнить сразу
		//if (nCmd == CMD_SETSIZE) {
		//	DWORD nHILO = *((DWORD*)pCommandData);
		//	SHORT nWidth = LOWORD(nHILO);
		//	SHORT nHeight = HIWORD(nHILO);
		//	WARNING("Низя CONOUT$ открывать/закрывать - у Win7 крышу сносит");
		//	MConHandle hConOut ( L"CONOUT$" );
		//	CONSOLE_SCREEN_BUFFER_INFO csbi = {{0,0}};
		//	BOOL lbRc = GetConsoleScreenBufferInfo(hConOut, &csbi);
		//	hConOut.Close();
		//	if (lbRc) {
		//		// Если размер консоли менять вообще не нужно
		//		if (csbi.dwSize.X == nWidth && csbi.dwSize.Y == nHeight) {
		//			OutDataAlloc(sizeof(nHILO));
		//			OutDataWrite(&nHILO, sizeof(nHILO));
		//			return gpCmdRet;
		//		}
		//	}
		//}

		if (/*nCmd == CMD_LEFTCLKSYNC ||*/ nCmd == CMD_CLOSEQSEARCH)
		{
			ResetEvent(ghConsoleWrite);
			gbWaitConsoleWrite = TRUE;
		}

		// Засемафорить, чтобы несколько команд одновременно не пошли...
		{
			HANDLE hEvents[2] = {ghServerTerminateEvent, ghPluginSemaphore};
			DWORD dwWait = WaitForMultipleObjects(2, hEvents, FALSE, INFINITE);

			if (dwWait == WAIT_OBJECT_0)
			{
				// Плагин завершается
				return FALSE;
			}

			if (nCmd == CMD_REDRAWFAR)
				gbNeedBgActivate = TRUE;

			lbSucceeded = ActivatePlugin(nCmd, pCommandData);

			if (lbSucceeded && /*pOldCmdRet !=*/ gpCmdRet)
			{
				pCmdRet = gpCmdRet; // запомнить результат!

				if (ppResult != &gpCmdRet)
					gpCmdRet = NULL;
			}

			ReleaseSemaphore(ghPluginSemaphore, 1, NULL);
		}
		// конец семафора

		if (nCmd == CMD_LEFTCLKSYNC || nCmd == CMD_CLOSEQSEARCH)
		{
			ResetEvent(ghConsoleInputEmpty);
			gbWaitConsoleInputEmpty = TRUE;
			DWORD nWait = WaitForSingleObject(ghConsoleInputEmpty, 2000);

			if (nWait == WAIT_OBJECT_0)
			{
				if (nCmd == CMD_CLOSEQSEARCH)
				{
					// И подождать, пока Фар обработает это событие (то есть до следующего чтения [Peek])
					nWait = WaitForSingleObject(ghConsoleWrite, 1000);
					lbSucceeded = (nWait == WAIT_OBJECT_0);
				}
			}
			else
			{
#ifdef _DEBUG
				DEBUGSTRMENU((nWait != 0) ? L"*** QUEUE IS NOT EMPTY\n" : L"*** QUEUE IS EMPTY\n");
#endif
				gbWaitConsoleInputEmpty = FALSE;
				lbSucceeded = (nWait == WAIT_OBJECT_0);
			}

			//DWORD nTestEvents = 0, dwTicks = GetTickCount();
			//DEBUGSTRMENU(L"*** waiting for queue empty\n");
			//HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
			//GetNumberOfConsoleInputEvents(h, &nTestEvents);
			//while (nTestEvents > 0 /*&& (dwTicks - GetTickCount()) < 300*/) {
			//	Sleep(10);
			//	GetNumberOfConsoleInputEvents(h, &nTestEvents);
			//	DWORD nCurTick = GetTickCount();
			//	if ((nCurTick - dwTicks) > 300)
			//		break;
			//}
		}

		// Собственно Redraw фар выполнит не тогда, когда его функцию позвали,
		// а когда к нему управление вернется
		if (nCmd == CMD_REDRAWFAR)
		{
			HANDLE hEvents[2] = {ghServerTerminateEvent, ghPluginSemaphore};
			DWORD dwWait = WaitForMultipleObjects(2, hEvents, FALSE, INFINITE);

			if (dwWait == WAIT_OBJECT_0)
			{
				// Плагин завершается
				return FALSE;
			}

			// Передернуть Background плагины
			if (gpBgPlugin) gpBgPlugin->SetForceUpdate();

			WARNING("После перехода на Synchro для FAR2 есть опасение, что следующий вызов может произойти до окончания предыдущего цикла обработки Synchro в Far");
			lbSucceeded = ActivatePlugin(CMD_FARPOST, NULL);

			if (lbSucceeded && /*pOldCmdRet !=*/ gpCmdRet)
			{
				pCmdRet = gpCmdRet; // запомнить результат!

				if (ppResult != &gpCmdRet)
					gpCmdRet = NULL;
			}

			ReleaseSemaphore(ghPluginSemaphore, 1, NULL);
		}

		if (ppResult)
		{
			if (ppResult != &gpCmdRet)
			{
				*ppResult = pCmdRet;
			}
		}
		else
		{
			if (pCmdRet && pCmdRet != gpTabs && pCmdRet != gpCmdRet)
			{
				Free(pCmdRet);
			}
		}

		//gpReqCommandData = NULL;
		//gnReqCommand = -1; gnPluginOpenFrom = -1;
		return lbSucceeded; // Результат выполнения команды
	}

	/*if (gbPlugKeyChanged) {
		gbPlugKeyChanged = FALSE;
		CheckMacro(TRUE);
		gbPlugKeyChanged = FALSE;
	}*/

	if (gnPluginOpenFrom == 0)
	{
		switch (Plugin()->GetActiveWindowType())
		{
		case WTYPE_PANELS:
			gnPluginOpenFrom = OPEN_FILEPANEL; break;
		case WTYPE_EDITOR:
			gnPluginOpenFrom = OPEN_EDITOR; break;
		case WTYPE_VIEWER:
			gnPluginOpenFrom = OPEN_VIEWER; break;
		}
	}

	// Некоторые команды "асинхронные", блокировки не нужны
	if (//nCmd == CMD_LOG_SHELL
	        nCmd == CMD_SET_CON_FONT
	        || nCmd == CMD_GUICHANGED
	        )
	{
		//if (nCmd == CMD_LOG_SHELL)
		//{
		//	TODO("Путь передается аргументом через pipe!");
		//	LogCreateProcessCheck((wchar_t*)pCommandData);
		//}
		//else
		if (nCmd == CMD_SET_CON_FONT)
		{
			CESERVER_REQ_SETFONT* pFont = (CESERVER_REQ_SETFONT*)pCommandData;

			if (pFont && pFont->cbSize == sizeof(CESERVER_REQ_SETFONT))
			{
				SetConsoleFontSizeTo(GetConEmuHWND(2), pFont->inSizeY, pFont->inSizeX, pFont->sFontName);
			}
		}
		else if (nCmd == CMD_GUICHANGED)
		{
			CESERVER_REQ_GUICHANGED *pWindows = (CESERVER_REQ_GUICHANGED*)pCommandData;

			if (gpBgPlugin)
				gpBgPlugin->SetForceThLoad();

			if (pWindows && pWindows->cbSize == sizeof(CESERVER_REQ_GUICHANGED))
			{
				UINT nConEmuSettingsMsg = RegisterWindowMessage(CONEMUMSG_PNLVIEWSETTINGS);

				if (pWindows->hLeftView && IsWindow(pWindows->hLeftView))
				{
					PostMessage(pWindows->hLeftView, nConEmuSettingsMsg, pWindows->nGuiPID, 0);
				}

				if (pWindows->hRightView && IsWindow(pWindows->hRightView))
				{
					PostMessage(pWindows->hRightView, nConEmuSettingsMsg, pWindows->nGuiPID, 0);
				}
			}
		}

		// Ставим и выходим
		if (ghReqCommandEvent)
			SetEvent(ghReqCommandEvent);

		return TRUE;
	}

	MSectionLock CSD; CSD.Lock(csData, TRUE);
	//if (gpCmdRet) { Free(gpCmdRet); gpCmdRet = NULL; } // !!! Освобождается ТОЛЬКО вызывающей функцией!
	gpCmdRet = NULL; gpData = NULL; gpCursor = NULL;
	// Раз дошли сюда - считаем что OK
	lbSucceeded = TRUE;

	switch (nCmd)
	{
		case CMD__EXTERNAL_CALLBACK:
		{
			lbSucceeded = FALSE;

			if (pCommandData
			        && ((SyncExecuteArg*)pCommandData)->nCmd == CMD__EXTERNAL_CALLBACK
			        && ((SyncExecuteArg*)pCommandData)->CallBack != NULL)
			{
				SyncExecuteArg* pExec = (SyncExecuteArg*)pCommandData;
				BOOL lbCallbackValid = CheckCallbackPtr(pExec->hModule, 1, (FARPROC*)&pExec->CallBack, FALSE, FALSE, FALSE);

				if (lbCallbackValid)
				{
					pExec->CallBack(pExec->lParam);
					lbSucceeded = TRUE;
				}
				else
				{
					lbSucceeded = FALSE;
				}
			}

			break;
		}
		case CMD_DRAGFROM:
		{
			//BOOL  *pbClickNeed = (BOOL*)pCommandData;
			//COORD *crMouse = (COORD *)(pbClickNeed+1);
			//ProcessCommand(CMD_LEFTCLKSYNC, TRUE/*bReqMainThread*/, pCommandData);
			Plugin()->ProcessDragFrom();
			Plugin()->ProcessDragTo();

			break;
		}
		case CMD_DRAGTO:
		{
			Plugin()->ProcessDragTo();

			break;
		}
		case CMD_SETWINDOW:
		{
			int nTab = 0;

			// Для Far1 мы сюда попадаем обычным образом, при обработке команды пайпом
			// Для Far2 и выше - через макрос (проверяющий допустимость смены) и callplugin
			DEBUGSTRCMD(L"Plugin: ACTL_SETCURRENTWINDOW\n");

			// Окно мы можем сменить только если:
			if (gnPluginOpenFrom == OPEN_VIEWER || gnPluginOpenFrom == OPEN_EDITOR
			        || gnPluginOpenFrom == OPEN_PLUGINSMENU
					|| gnPluginOpenFrom == OPEN_FILEPANEL)
			{
				_ASSERTE(pCommandData!=NULL);

				if (pCommandData!=NULL)
					nTab = *((DWORD*)pCommandData);

				gbIgnoreUpdateTabs = TRUE;

				Plugin()->SetWindow(nTab);

				DEBUGSTRCMD(L"Plugin: ACTL_COMMIT finished\n");

				gbIgnoreUpdateTabs = FALSE;
				Plugin()->UpdateConEmuTabs(bForceSendTabs);

				DEBUGSTRCMD(L"Plugin: Tabs updated\n");
			}

			//SendTabs(gnCurTabCount, FALSE); // Обновить размер передаваемых данных
			pCmdRet = gpTabs;
			break;
		}
		case CMD_POSTMACRO:
		{
			_ASSERTE(pCommandData!=NULL);

			if (pCommandData!=NULL)
				PostMacro((wchar_t*)pCommandData, NULL);

			break;
		}
		case CMD_CLOSEQSEARCH:
		{
			if (!gFarVersion.IsFarLua())
				PostMacro(L"$if (Search) Esc $end", NULL);
			else
				PostMacro(L"if Area.Search Keys(\"Esc\") end", NULL);
			break;
		}
		case CMD_LEFTCLKSYNC:
		{
			BOOL  *pbClickNeed = (BOOL*)pCommandData;
			COORD *crMouse = (COORD *)(pbClickNeed+1);

			// Для Far3 - координаты вроде можно сразу в макрос кинуть
			if (gFarVersion.dwVer >= 3)
			{
				INPUT_RECORD r = {MOUSE_EVENT};
				r.Event.MouseEvent.dwButtonState = FROM_LEFT_1ST_BUTTON_PRESSED;
				r.Event.MouseEvent.dwMousePosition = *crMouse;
				#ifdef _DEBUG
				//r.Event.MouseEvent.dwMousePosition.X = 5;
				#endif

				PostMacro((gFarVersion.dwBuild <= 2850) ? L"MsLClick" : L"Keys('MsLClick')", &r);
			}
			else
			{
				INPUT_RECORD clk[2] = {{MOUSE_EVENT},{MOUSE_EVENT}};
				int i = 0;

				if (*pbClickNeed)
				{
					clk[i].Event.MouseEvent.dwButtonState = FROM_LEFT_1ST_BUTTON_PRESSED;
					clk[i].Event.MouseEvent.dwMousePosition = *crMouse;
					i++;
				}

				clk[i].Event.MouseEvent.dwMousePosition = *crMouse;
				i++;
				DWORD cbWritten = 0;
				HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
				_ASSERTE(h!=INVALID_HANDLE_VALUE && h!=NULL);
				BOOL fSuccess = WriteConsoleInput(h, clk, 2, &cbWritten);

				if (!fSuccess || cbWritten != 2)
				{
					_ASSERTE(fSuccess && cbWritten==2);
				}
			}
			break;
		}
		case CMD_EMENU:  //RMENU
		{
			COORD *crMouse = (COORD *)pCommandData;
			const wchar_t *pszUserMacro = (wchar_t*)(crMouse+1);

			// Т.к. вызов идет через макрос и "rclk_gui:", то настройки emenu трогать нельзя!
			//// Чтобы на чистой системе менюшка всплывала под курсором и не выскакивало сообщение ""
			//HKEY hRClkKey = NULL;
			//DWORD disp = 0;
			//WCHAR szEMenuKey[MAX_PATH*2+64];
			//lstrcpyW(szEMenuKey, ms_RootRegKey);
			//lstrcatW(szEMenuKey, L"\\Plugins\\RightClick");

			//// Ключа может и не быть, если настройки ни разу не сохранялись
			//if (0 == RegCreateKeyExW(HKEY_CURRENT_USER, szEMenuKey, 0, 0, 0, KEY_ALL_ACCESS, 0, &hRClkKey, &disp))
			//{
			//	if (disp == REG_CREATED_NEW_KEY)
			//	{
			//		RegSetValueExW(hRClkKey, L"WaitToContinue", 0, REG_DWORD, (LPBYTE)&(disp = 0), sizeof(disp));
			//		RegSetValueExW(hRClkKey, L"GuiPos", 0, REG_DWORD, (LPBYTE)&(disp = 0), sizeof(disp));
			//	}

			//	RegCloseKey(hRClkKey);
			//}

			// Иначе в некторых случаях (Win7 & FAR2x64) не отрисовывается сменившийся курсор
			// В FAR 1.7x это приводит к зачернению экрана??? Решается посылкой
			// "пустого" события движения мышки в консоль сразу после ACTL_KEYMACRO
			Plugin()->RedrawAll();
			//PostMacro((wchar_t*)L"@F11 %N=Menu.Select(\"EMenu\",0); $if (%N==0) %N=Menu.Select(\"EMenu\",2); $end $if (%N>0) Enter $while (Menu) Enter $end $else $MMode 1 MsgBox(\"ConEmu\",\"EMenu not found in F11\",0x00010001) $end");

			const wchar_t* pszMacro = NULL;

			if (pszUserMacro && *pszUserMacro)
				pszMacro = pszUserMacro;
			else
				pszMacro = gFarVersion.IsFarLua() ? FarRClickMacroDefault3 : FarRClickMacroDefault2; //L"@$If (!CmdLine.Empty) %Flg_Cmd=1; %CmdCurPos=CmdLine.ItemCount-CmdLine.CurPos+1; %CmdVal=CmdLine.Value; Esc $Else %Flg_Cmd=0; $End $Text \"rclk_gui:\" Enter $If (%Flg_Cmd==1) $Text %CmdVal %Flg_Cmd=0; %Num=%CmdCurPos; $While (%Num!=0) %Num=%Num-1; CtrlS $End $End";

			INPUT_RECORD r = {MOUSE_EVENT};
			r.Event.MouseEvent.dwButtonState = FROM_LEFT_1ST_BUTTON_PRESSED;
			r.Event.MouseEvent.dwMousePosition = *crMouse;
			#ifdef _DEBUG
			//r.Event.MouseEvent.dwMousePosition.X = 5;
			#endif

			if (SetFarHookMode)
			{
				// Сказать библиотеке хуков (ConEmuHk.dll), что меню нужно показать в позиции курсора мыши
				gFarMode.bPopupMenuPos = TRUE;
				SetFarHookMode(&gFarMode);
			}

			PostMacro((wchar_t*)pszMacro, &r);
			//// Чтобы GUI не дожидался окончания всплытия EMenu
			//SetEvent(ghReqCommandEvent);
			////
			//HMODULE hEMenu = GetModuleHandle(L"emenu.dll");
			//if (!hEMenu)
			//{
			//	if (gFarVersion.dwVerMajor==2) {
			//		TCHAR temp[NM*5];
			//		ExpandEnvironmentStringsW(L"%FARHOME%\\Plugins\\emenu\\EMenu.dll",temp,countof(temp));
			//		if (gFarVersion.dwBuild>=FAR_Y_VER) {
			//			FUNC_Y(LoadPlugin)(temp);
			//		} else {
			//			FUNC_X(LoadPlugin)(temp);
			//		}
			//		// Фактически FAR НЕ загружает длл-ку к сожалению, так что тут мы обломаемся
			//		hEMenu = GetModuleHandle(L"emenu.dll");
			//	}
			//
			//	if (!hEMenu) {
			//		PostMacro((wchar_t*)L"@F11 %N=Menu.Select(\"EMenu\",0); $if (%N==0) %N=Menu.Select(\"EMenu\",2); $end $if (%N>0) Enter $while (Menu) Enter $end $else $MMode 1 MsgBox(\"ConEmu\",\"EMenu not found in F11\",0x00010001) $end");
			//		break; // уже все что мог - сделал макрос
			//	}
			//}
			//if (hEMenu)
			//{
			//	OpenPlugin_t fnOpenPluginW = (OpenPlugin_t)GetProcAddress(hEMenu, (gFarVersion.dwVerMajor==1) ? "OpenPlugin" : "OpenPluginW");
			//	_ASSERTE(fnOpenPluginW);
			//	if (fnOpenPluginW) {
			//		if (gFarVersion.dwVerMajor==1) {
			//			fnOpenPluginW(OPEN_COMMANDLINE, (INT_PTR)"rclk_gui:");
			//		} else {
			//			fnOpenPluginW(OPEN_COMMANDLINE, (INT_PTR)L"rclk_gui:");
			//		}
			//	} else {
			//		// Ругнуться?
			//	}
			//}
			//return NULL;
			break;
		}
		case CMD_REDRAWFAR:
			// В Far 1.7x были глюки с отрисовкой?
			if (gFarVersion.dwVerMajor>=2)
				Plugin()->RedrawAll();

			break;
		case CMD_CHKRESOURCES:
			CheckResources(true);
			break;
		//case CMD_SETSIZE:
		//{
		//	_ASSERTE(pCommandData!=NULL);
		//	//BOOL lbNeedChange = TRUE;
		//	DWORD nHILO = *((DWORD*)pCommandData);
		//	SHORT nWidth = LOWORD(nHILO);
		//	SHORT nHeight = HIWORD(nHILO);
		//	BOOL lbRc = SetConsoleSize(nWidth, nHeight);
		//	MConHandle hConOut ( L"CONOUT$" );
		//	CONSOLE_SCREEN_BUFFER_INFO csbi = {{0,0}};
		//	lbRc = GetConsoleScreenBufferInfo(hConOut, &csbi);
		//	hConOut.Close();
		//	if (lbRc) {
		//		OutDataAlloc(sizeof(nHILO));
		//		nHILO = ((WORD)csbi.dwSize.X) | (((DWORD)(WORD)csbi.dwSize.Y) << 16);
		//		OutDataWrite(&nHILO, sizeof(nHILO));
		//	}
		//	//REDRAWALL
		//}
		case CMD_FARPOST:
		{
			// просто сигнализация о том, что фар получил управление.
			lbSucceeded = TRUE;
			break;
		}
		case CMD_OPENEDITORLINE:
		{
			lbSucceeded = TRUE;
			// Может потом на API переделать?
			CESERVER_REQ_FAREDITOR *pCmd = (CESERVER_REQ_FAREDITOR*)pCommandData;
			LPCWSTR pSrc = pCmd->szFile;
			INT_PTR cchMax = MAX_PATH*4 + lstrlenW(pSrc); //-V112
			wchar_t* pszMacro = (wchar_t*)malloc(cchMax*sizeof(*pszMacro));
			if (!pszMacro)
			{
				_ASSERTE(pszMacro!=NULL)
			}
			else
			{
				// Добавим префикс "^", чтобы не вообще посылать "нажатия кнопок" в плагины
				// Иначе, если например активна панель с RegEditor'ом, то результат будет неожиданным )
				if (gFarVersion.dwVerMajor==1)
					_wcscpy_c(pszMacro, cchMax, L"@^$if(Viewer || Editor) F12 0 $end $if(Shell) ShiftF4 \"");
				else if (!gFarVersion.IsFarLua())
					_wcscpy_c(pszMacro, cchMax, L"@^$if(Viewer || Editor) F12 0 $end $if(Shell) ShiftF4 print(\"");
				else if (gFarVersion.IsDesktop()) // '0' is 'Desktop' now
					_wcscpy_c(pszMacro, cchMax, L"@^if Area.Viewer or Area.Editor then Keys(\"F12 1\") end if Area.Shell then Keys(\"ShiftF4\") print(\"");
				else
					_wcscpy_c(pszMacro, cchMax, L"@^if Area.Viewer or Area.Editor then Keys(\"F12 0\") end if Area.Shell then Keys(\"ShiftF4\") print(\"");
				wchar_t* pDst = pszMacro + lstrlen(pszMacro);
				while (*pSrc)
				{
					*(pDst++) = *pSrc;
					if (*pSrc == L'\\')
						*(pDst++) = L'\\';
					pSrc++;
				}
				*pDst = 0;
				if (gFarVersion.dwVerMajor==1)
					_wcscat_c(pszMacro, cchMax, L"\" Enter ");
				else if (!gFarVersion.IsFarLua())
					_wcscat_c(pszMacro, cchMax, L"\") Enter ");
				else
					_wcscat_c(pszMacro, cchMax, L"\") Keys(\"Enter\") ");

				if (pCmd->nLine > 0)
				{
					int nCurLen = lstrlen(pszMacro);
					if (gFarVersion.dwVerMajor==1)
						_wsprintf(pszMacro+nCurLen, SKIPLEN(cchMax-nCurLen) L" $if(Editor) AltF8 \"%i:%i\" Enter $end", pCmd->nLine, pCmd->nColon);
					else if (!gFarVersion.IsFarLua())
						_wsprintf(pszMacro+nCurLen, SKIPLEN(cchMax-nCurLen) L" $if(Editor) AltF8 print(\"%i:%i\") Enter $end", pCmd->nLine, pCmd->nColon);
					else
						_wsprintf(pszMacro+nCurLen, SKIPLEN(cchMax-nCurLen) L" if Area.Editor then Keys(\"AltF8\") print(\"%i:%i\") Keys(\"Enter\") end", pCmd->nLine, pCmd->nColon);
				}

				_wcscat_c(pszMacro, cchMax, (!gFarVersion.IsFarLua()) ? L" $end" : L" end");
				PostMacro(pszMacro, NULL);
				free(pszMacro);
			}
			break;
		}
		default:
			// Неизвестная команда!
			_ASSERTE(nCmd == 1);
			lbSucceeded = FALSE;
	}

	// Функция выполняется в том время, пока заблокирован ghPluginSemaphore,
	// поэтому gpCmdRet можно пользовать
	if (lbSucceeded && !pCmdRet)  // pCmdRet может уже содержать gpTabs
	{
		pCmdRet = gpCmdRet;
		gpCmdRet = NULL;
	}

	if (ppResult)
	{
		*ppResult = pCmdRet;
	}
	else if (pCmdRet && pCmdRet != gpTabs)
	{
		Free(pCmdRet);
	}

	CSD.Unlock();
#ifdef _DEBUG
	_ASSERTE(_CrtCheckMemory());
#endif

	if (ghReqCommandEvent)
		SetEvent(ghReqCommandEvent);

	return lbSucceeded;
}

// Изменить размер консоли. Собственно сам ресайз - выполняется сервером!
BOOL FarSetConsoleSize(SHORT nNewWidth, SHORT nNewHeight)
{
	BOOL lbRc = FALSE;

	if (!gdwServerPID)
	{
		_ASSERTE(gdwServerPID!=0);
	}
	else
	{
		CESERVER_REQ In;
		ExecutePrepareCmd(&In, CECMD_SETSIZENOSYNC, sizeof(CESERVER_REQ_HDR)+sizeof(CESERVER_REQ_SETSIZE));
		memset(&In.SetSize, 0, sizeof(In.SetSize));
		// Для 'far /w' нужно оставить высоту буфера!
		In.SetSize.nBufferHeight = gpFarInfo->bBufferSupport ? -1 : 0;
		In.SetSize.size.X = nNewWidth; In.SetSize.size.Y = nNewHeight;
		DWORD nSrvPID = (gpConMapInfo && gpConMapInfo->nAltServerPID) ? gpConMapInfo->nAltServerPID : gdwServerPID;
		CESERVER_REQ* pOut = ExecuteSrvCmd(nSrvPID, &In, GetConEmuHWND(2));

		if (pOut)
		{
			ExecuteFreeResult(pOut);
		}

		Plugin()->RedrawAll();
	}

//#ifdef _DEBUG
//	if (GetCurrentThreadId() != gnMainThreadId) {
//		_ASSERTE(GetCurrentThreadId() == gnMainThreadId);
//	}
//#endif
//
//	BOOL lbRc = FALSE, lbNeedChange = TRUE;
//	SHORT nWidth = nNewWidth; if (nWidth</*4*/MIN_CON_WIDTH) nWidth = /*4*/MIN_CON_WIDTH;
//	SHORT nHeight = nNewHeight; if (nHeight</*3*/MIN_CON_HEIGHT) nHeight = /*3*/MIN_CON_HEIGHT;
//	MConHandle hConOut ( L"CONOUT$" );
//	COORD crMax = MyGetLargestConsoleWindowSize(hConOut);
//	if (crMax.X && nWidth > crMax.X) nWidth = crMax.X;
//	if (crMax.Y && nHeight > crMax.Y) nHeight = crMax.Y;
//
//	CONSOLE_SCREEN_BUFFER_INFO csbi = {{0,0}};
//	if (GetConsoleScreenBufferInfo(hConOut, &csbi)) {
//		if (csbi.dwSize.X == nWidth && csbi.dwSize.Y == nHeight
//			&& csbi.srWindow.Top == 0 && csbi.srWindow.Left == 0
//			&& csbi.srWindow.Bottom == (nWidth-1)
//			&& csbi.srWindow.Bottom == (nHeight-1))
//		{
//			lbNeedChange = FALSE;
//		}
//	}
//
//	if (lbNeedChange) {
//		DWORD dwErr = 0;
//
//		// Если этого не сделать - размер консоли нельзя УМЕНЬШИТЬ
//		RECT rcConPos = {0}; GetWindowRect(FarHwnd, &rcConPos);
//		MoveWindow(FarHwnd, rcConPos.left, rcConPos.top, 1, 1, 1);
//
//		//specified width and height cannot be less than the width and height of the console screen buffer's window
//		COORD crNewSize = {nWidth, nHeight};
//		lbRc = SetConsoleScreenBufferSize(hConOut, crNewSize);
//		if (!lbRc) dwErr = GetLastError();
//
//		SMALL_RECT rNewRect = {0,0,nWidth-1,nHeight-1};
//		SetConsoleWindowInfo(hConOut, TRUE, &rNewRect);
//
//		RedrawAll();
//	}
	return lbRc;
}

static BOOL gbTryOpenMapHeader = FALSE;
static BOOL gbStartupHooksAfterMap = FALSE;
int OpenMapHeader();
void CloseMapHeader();

BOOL gbWasDetached = FALSE;
CONSOLE_SCREEN_BUFFER_INFO gsbiDetached;

BOOL WINAPI OnConsoleDetaching(HookCallbackArg* pArgs)
{
	if (ghMonitorThread)
	{
		SuspendThread(ghMonitorThread);
		// ResumeThread выполняется в конце OnConsoleWasAttached
	}

	// Выполним сразу после SuspendThread, чтобы нить не посчитала, что мы подцепились обратно
	gbWasDetached = (ghConEmuWndDC!=NULL && IsWindow(ghConEmuWndDC));

	if (ghConEmuWndDC)
	{
		// Запомним, для удобства аттача
		if (!GetWindowThreadProcessId(ghConEmuWndDC, &gdwPreDetachGuiPID))
			gdwPreDetachGuiPID = 0;
	}

	if (gbWasDetached)
	{
		HANDLE hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		GetConsoleScreenBufferInfo(hOutput, &gsbiDetached);

		// Нужно уведомить ТЕКУЩИЙ сервер, что закрываться по окончании команды не нужно
		if (gdwServerPID == 0)
		{
			_ASSERTE(gdwServerPID != NULL);
		}
		else
		{
			CESERVER_REQ In, *pOut = NULL;
			ExecutePrepareCmd(&In, CECMD_FARDETACHED, sizeof(CESERVER_REQ_HDR));
			pOut = ExecuteSrvCmd(gdwServerPID, &In, FarHwnd);

			if (pOut) ExecuteFreeResult(pOut);
		}
	}

	// -- теперь мэппинги создает GUI
	//CloseColorerHeader(); // Если было

	CloseMapHeader();
	ghConEmuWndDC = NULL;
	SetConEmuEnvVar(NULL);
	SetConEmuEnvVarChild(NULL,NULL);
	// Потом еще и FarHwnd сбросить нужно будет... Ну этим MonitorThreadProcW займется
	return TRUE; // продолжить выполнение функции
}
// Функции вызываются в основной нити, вполне можно дергать FAR-API
VOID WINAPI OnConsoleWasAttached(HookCallbackArg* pArgs)
{
	FarHwnd = GetConEmuHWND(2);

	if (gbWasDetached)
	{
		// Сразу спрятать окошко
		//apiShowWindow(FarHwnd, SW_HIDE);
	}

	// -- теперь мэппинги создает GUI
	//// Если ранее были созданы мэппинги для цвета - пересоздать
	//CreateColorerHeader();

	if (gbWasDetached)
	{
		/*
		HANDLE hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleScreenBufferSize(hOutput,sbi.dwSize);
		SetConsoleWindowInfo(hOutput,TRUE,&sbi.srWindow);
		SetConsoleScreenBufferSize(hOutput,sbi.dwSize);
		*/

		// сразу переподцепимся к GUI
		if (!Attach2Gui())
		{
			EmergencyShow(FarHwnd);
		}

		// Сбрасываем после Attach2Gui, чтобы MonitorThreadProcW случайно
		// не среагировал раньше времени
		gbWasDetached = FALSE;
	}

	if (ghMonitorThread)
		ResumeThread(ghMonitorThread);
}

void WINAPI SetStartupInfoW(void *aInfo)
{
	#ifdef _DEBUG
	HMODULE h = LoadLibrary (L"Kernel32.dll");
	FreeLibrary(h);
	#endif

	Plugin()->SetStartupInfo(aInfo);

	Plugin()->CommonPluginStartup();
}

//#define CREATEEVENT(fmt,h)
//		_wsprintf(szEventName, SKIPLEN(countof(szEventName)) fmt, dwCurProcId );
//		h = CreateEvent(NULL, FALSE, FALSE, szEventName);
//		if (h==INVALID_HANDLE_VALUE) h=NULL;

BOOL ReloadFarInfo(BOOL abForce)
{
	if (!gpFarInfoMapping)
	{
		DWORD dwErr = 0;
		// Создать мэппинг для gpFarInfoMapping
		wchar_t szMapName[MAX_PATH];
		_wsprintf(szMapName, SKIPLEN(countof(szMapName)) CEFARMAPNAME, gnSelfPID);
		DWORD nMapSize = sizeof(CEFAR_INFO_MAPPING);
		TODO("Заменить на MFileMapping");
		ghFarInfoMapping = CreateFileMapping(INVALID_HANDLE_VALUE,
		                                     gpLocalSecurity, PAGE_READWRITE, 0, nMapSize, szMapName);

		if (!ghFarInfoMapping)
		{
			dwErr = GetLastError();
			//TODO("Показать ошибку создания MAP для ghFarInfoMapping");
			_ASSERTE(ghFarInfoMapping!=NULL);
		}
		else
		{
			gpFarInfoMapping = (CEFAR_INFO_MAPPING*)MapViewOfFile(ghFarInfoMapping, FILE_MAP_ALL_ACCESS,0,0,0);

			if (!gpFarInfoMapping)
			{
				dwErr = GetLastError();
				CloseHandle(ghFarInfoMapping); ghFarInfoMapping = NULL;
				//TODO("Показать ошибку создания MAP для ghFarInfoMapping");
				_ASSERTE(gpFarInfoMapping!=NULL);
			}
			else
			{
				gpFarInfoMapping->cbSize = 0;
			}
		}
	}

	if (!ghFarAliveEvent)
	{
		wchar_t szEventName[64];
		_wsprintf(szEventName, SKIPLEN(countof(szEventName)) CEFARALIVEEVENT, gnSelfPID);
		ghFarAliveEvent = CreateEvent(gpLocalSecurity, FALSE, FALSE, szEventName);
	}

	if (!gpFarInfo)
	{
		gpFarInfo = (CEFAR_INFO_MAPPING*)Alloc(sizeof(CEFAR_INFO_MAPPING),1);

		if (!gpFarInfo)
		{
			_ASSERTE(gpFarInfo!=NULL);
			return FALSE;
		}

		gpFarInfo->cbSize = sizeof(CEFAR_INFO_MAPPING);
		gpFarInfo->nFarInfoIdx = 0;
		gpFarInfo->FarVer = gFarVersion;
		gpFarInfo->nFarPID = gnSelfPID;
		gpFarInfo->nFarTID = gnMainThreadId;
		gpFarInfo->nProtocolVersion = CESERVER_REQ_VER;

		if (gFarVersion.dwVerMajor < 2 || (gFarVersion.dwVerMajor == 2 && gFarVersion.dwBuild < 1564))
		{
			gpFarInfo->bBufferSupport = FALSE;
		}
		else
		{
			// Нужно проверить
			gpFarInfo->bBufferSupport = Plugin()->CheckBufferEnabled();
		}

		// Загрузить из реестра настройки PanelTabs
		gpFarInfo->PanelTabs.SeparateTabs = gpFarInfo->PanelTabs.ButtonColor = -1;
		LoadPanelTabsFromRegistry();
	}

	BOOL lbChanged = FALSE, lbSucceded = FALSE;

	lbSucceded = Plugin()->ReloadFarInfo();

	if (lbSucceded)
	{
		if (abForce || memcmp(gpFarInfoMapping, gpFarInfo, sizeof(CEFAR_INFO_MAPPING))!=0)
		{
			lbChanged = TRUE;
			gpFarInfo->nFarInfoIdx++;
			*gpFarInfoMapping = *gpFarInfo;
		}
	}

	return lbChanged;
}

VOID WINAPI OnCurDirChanged()
{
	if ((gnCurrentWindowType == WTYPE_PANELS) && (IS_SYNCHRO_ALLOWED))
	{
		// Требуется дернуть Synchro, чтобы корректно активироваться
		if (!gbInputSynchroPending)
		{
			gbInputSynchroPending = true;
			ExecuteSynchro();
		}
	}
}

//#ifndef max
//#define max(a,b)            (((a) > (b)) ? (a) : (b))
//#endif
//
//#ifndef min
//#define min(a,b)            (((a) < (b)) ? (a) : (b))
//#endif


// watch non-modified -> modified editor status change

//int lastModifiedStateW = -1;
//bool gbHandleOneRedraw = false; //, gbHandleOneRedrawCh = false;

int WINAPI ProcessEditorInputW(void* Rec)
{
	// Даже если мы не под эмулятором - просто запомним текущее состояние
	//if (!ghConEmuWndDC) return 0; // Если мы не под эмулятором - ничего
	return Plugin()->ProcessEditorInput((LPCVOID)Rec);
}

void FillLoadedParm(struct ConEmuLoadedArg* pArg, HMODULE hSubPlugin, BOOL abLoaded)
{
	memset(pArg, 0, sizeof(struct ConEmuLoadedArg));
	pArg->cbSize = (DWORD)sizeof(struct ConEmuLoadedArg);
	//#define D(N) (1##N-100)
	// nBuildNo в формате YYMMDDX (YY - две цифры года, MM - месяц, DD - день, X - 0 и выше-номер подсборки)
	pArg->nBuildNo = ((MVV_1 % 100)*100000) + (MVV_2*1000) + (MVV_3*10) + (MVV_4 % 10);
	pArg->hConEmu = ghPluginModule;
	pArg->hPlugin = hSubPlugin;
	pArg->bLoaded = abLoaded;
	pArg->bGuiActive = abLoaded && (ghConEmuWndDC != NULL);

	// Сервисные функции
	if (abLoaded)
	{
		pArg->GetFarHWND = GetFarHWND;
		pArg->GetFarHWND2 = GetFarHWND2;
		pArg->GetFarVersion = GetFarVersion;
		pArg->IsTerminalMode = IsTerminalMode;
		pArg->IsConsoleActive = IsConsoleActive;
		pArg->RegisterPanelView = RegisterPanelView;
		pArg->RegisterBackground = RegisterBackground;
		pArg->ActivateConsole = ActivateConsole;
		pArg->SyncExecute = SyncExecute;
	}
}

void NotifyConEmuUnloaded()
{
	OnConEmuLoaded_t fnOnConEmuLoaded = NULL;
	BOOL lbSucceded = FALSE;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);

	if (snapshot != INVALID_HANDLE_VALUE)
	{
		MODULEENTRY32 module = {sizeof(MODULEENTRY32)};

		for (BOOL res = Module32First(snapshot, &module); res; res = Module32Next(snapshot, &module))
		{
			if ((fnOnConEmuLoaded = (OnConEmuLoaded_t)GetProcAddress(module.hModule, "OnConEmuLoaded")) != NULL)
			{
				// Наверное, только для плагинов фара
				if (GetProcAddress(module.hModule, "SetStartupInfoW") || GetProcAddress(module.hModule, "SetStartupInfo"))
				{
					struct ConEmuLoadedArg arg = {sizeof(struct ConEmuLoadedArg)};
					FillLoadedParm(&arg, module.hModule, FALSE); // плагин conemu.dll выгружается!
					//arg.hPlugin = module.hModule;
					//arg.nBuildNo = ((MVV_1 % 100)*10000) + (MVV_2*100) + (MVV_3);
					//arg.hConEmu = ghPluginModule;
					//arg.bLoaded = FALSE;
					lbSucceded = FALSE;
					SAFETRY
					{
						fnOnConEmuLoaded(&arg);
						lbSucceded = TRUE;
					} SAFECATCH
					{
						// Failed
						_ASSERTE(lbSucceded == TRUE);
					}
				}
			}
		}

		CloseHandle(snapshot);
	}
}


HANDLE WINAPI OpenW(const void* Info)
{
	return Plugin()->Open(Info);
}

#if 0
INT_PTR WINAPI ProcessConsoleInputW(void *Info)
{
	if (gFarVersion.dwBuild>=FAR_Y2_VER)
		return FUNC_Y2(ProcessConsoleInputW)(Info);
	else //if (gFarVersion.dwBuild>=FAR_Y1_VER)
		return FUNC_Y1(ProcessConsoleInputW)(Info);
}
#endif

void WINAPI ExitFARW(void)
{
	CPluginBase::ShutdownPluginStep(L"ExitFARW");

	Plugin()->ExitFarCommon();
	Plugin()->ExitFAR();

	CPluginBase::ShutdownPluginStep(L"ExitFARW - done");
}

void WINAPI ExitFARW3(void*)
{
	CPluginBase::ShutdownPluginStep(L"ExitFARW3");

	Plugin()->ExitFarCommon();
	Plugin()->ExitFAR();

	CPluginBase::ShutdownPluginStep(L"ExitFARW3 - done");
}

// Определены в SetHook.h
//typedef void (WINAPI* OnLibraryLoaded_t)(HMODULE ahModule);
//extern OnLibraryLoaded_t gfOnLibraryLoaded;

// Вызывается при загрузке dll
void WINAPI OnLibraryLoaded(HMODULE ahModule)
{
	WARNING("Проверить, чтобы после новых хуков это два раза на один модуль не вызывалось");

	//#ifdef _DEBUG
	//wchar_t szModulePath[MAX_PATH]; szModulePath[0] = 0;
	//GetModuleFileName(ahModule, szModulePath, MAX_PATH);
	//#endif

	//// Если GUI неактивно (запущен standalone FAR) - сразу выйти
	//if (ghConEmuWndDC == NULL)
	//{
	//	return;
	//}
	WARNING("Нужно специально вызвать OnLibraryLoaded при аттаче к GUI");
	// Если определен калбэк инициализации ConEmu
	OnConEmuLoaded_t fnOnConEmuLoaded = NULL;
	BOOL lbSucceeded = FALSE;

	if ((fnOnConEmuLoaded = (OnConEmuLoaded_t)GetProcAddress(ahModule, "OnConEmuLoaded")) != NULL)
	{
		// Наверное, только для плагинов фара
		if (GetProcAddress(ahModule, "SetStartupInfoW") || GetProcAddress(ahModule, "SetStartupInfo"))
		{
			struct ConEmuLoadedArg arg; // = {sizeof(struct ConEmuLoadedArg)};
			FillLoadedParm(&arg, ahModule, TRUE);
			//arg.hPlugin = ahModule;
			//arg.hConEmu = ghPluginModule;
			//arg.hPlugin = ahModule;
			//arg.bLoaded = TRUE;
			//arg.bGuiActive = (ghConEmuWndDC != NULL);
			//// Сервисные функции
			//arg.GetFarHWND = GetFarHWND;
			//arg.GetFarHWND2 = GetFarHWND2;
			//arg.GetFarVersion = GetFarVersion;
			//arg.IsTerminalMode = IsTerminalMode;
			//arg.IsConsoleActive = IsConsoleActive;
			//arg.RegisterPanelView = RegisterPanelView;
			//arg.RegisterBackground = RegisterBackground;
			//arg.ActivateConsole = ActivateConsole;
			//arg.SyncExecute = SyncExecute;
			SAFETRY
			{
				fnOnConEmuLoaded(&arg);
				lbSucceeded = TRUE;
			} SAFECATCH
			{
				// Failed
				_ASSERTE(lbSucceeded == TRUE);
			}
		}
	}
}


/* Используются как extern в ConEmuCheck.cpp */
LPVOID _calloc(size_t nCount,size_t nSize)
{
	return calloc(nCount,nSize);
}
LPVOID _malloc(size_t nCount)
{
	return malloc(nCount);
}
void   _free(LPVOID ptr)
{
	free(ptr);
}
