#pragma once
#include <vector>

class tray_menu_t {
public:
	tray_menu_t( char* text ) : text(text),disabled(0),checked(0),callback(nullptr) {};
	tray_menu_t( char* text, void (*callback)() ) : text(text),disabled(0),checked(0),callback(callback) {};
	char *text;
	int disabled;
	int checked;
	void (*callback)();
};

class tray_t {
public:
	tray_t() : icon(nullptr),shutdown_task(nullptr) {};
	tray_t( char* icon, void (*shutdown_task)() ) : icon(icon),shutdown_task(shutdown_task) { menu.push_back(tray_menu_t("exit")); };
	tray_t( char* icon, void (*shutdown_task)(), std::vector<tray_menu_t>& menu ) : icon(icon),menu(menu),shutdown_task(shutdown_task) {};
	char *icon;
	std::vector<tray_menu_t> menu;
	void (*shutdown_task)();
};

#include <windows.h>
#include <shellapi.h>
#define WM_TRAY_CALLBACK_MESSAGE (WM_USER + 1)
#define WC_TRAY_CLASS_NAME "TRAY"
#define ID_TRAY_FIRST 1000

static tray_t m_tray;
static WNDCLASSEX wc;
static NOTIFYICONDATA nid;
static HWND hwnd;
static HMENU hmenu = NULL;

static LRESULT CALLBACK _tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_TRAY_CALLBACK_MESSAGE:
		if ( lparam == WM_LBUTTONUP || lparam == WM_RBUTTONUP ) {
			POINT p;
			GetCursorPos(&p);
			SetForegroundWindow(hwnd);
			WORD cmd = TrackPopupMenu(hmenu,TPM_LEFTALIGN|TPM_RIGHTBUTTON|TPM_RETURNCMD|TPM_NONOTIFY,p.x,p.y,0,hwnd,NULL);
			SendMessage(hwnd,WM_COMMAND,cmd,0);
			return 0;
		}
		break;
	case WM_COMMAND:
		if ( wparam >= ID_TRAY_FIRST ) {
			MENUITEMINFO item = {0};
			item.cbSize = sizeof(MENUITEMINFO);
			item.fMask = MIIM_ID | MIIM_DATA;
			if (GetMenuItemInfo(hmenu,wparam,FALSE,&item)) {
				int i = item.dwItemData;
				if ( strcmp(m_tray.menu[i].text,"exit") == 0 ) { SendMessage(hwnd,WM_CLOSE,0,0); } else { m_tray.menu[i].callback(); }
			}
			return 0;
		}
		break;
	case WM_ENDSESSION:
		if ( wparam == TRUE ) {
			m_tray.shutdown_task();
			return 0;
		}
		break;
	}
	return DefWindowProc(hwnd,msg,wparam,lparam);
}

static HMENU _tray_menu( std::vector<tray_menu_t>& l, UINT& id ) {
	HMENU hmenu = CreatePopupMenu();
	for (int i=0;i<l.size();i++) {
		tray_menu_t& m = l[i];
		MENUITEMINFO item;
		memset(&item,0,sizeof(item));
		item.cbSize = sizeof(MENUITEMINFO);
		item.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
		item.fType = 0;
		item.fState = 0;
		if ( m.disabled ) item.fState |= MFS_DISABLED;
		if ( m.checked ) item.fState |= MFS_CHECKED;
		item.wID = id++;
		item.dwTypeData = m.text;
		item.dwItemData = i;
		InsertMenuItem(hmenu,id,TRUE,&item);
	}
	return hmenu;
}

static void _tray_update() {
	HMENU prevmenu = hmenu;
	UINT id = ID_TRAY_FIRST;
	hmenu = _tray_menu(m_tray.menu,id);
	SendMessage(hwnd,WM_INITMENUPOPUP,(WPARAM)hmenu,0);
	HICON icon;
	ExtractIconEx(m_tray.icon,0,NULL,&icon,1);
	if ( nid.hIcon ) DestroyIcon(nid.hIcon);
	nid.hIcon = icon;
	Shell_NotifyIcon(NIM_MODIFY,&nid);
	if ( prevmenu != NULL ) DestroyMenu(prevmenu);
}

static bool tray_exist( const char* tray_name ) { return ( FindWindow(WC_TRAY_CLASS_NAME,tray_name) == NULL ? false : true ); }

static int tray_init( tray_t& tray, const char* tray_name ) {
	m_tray = tray;
	memset(&wc,0,sizeof(wc));
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = _tray_wnd_proc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = WC_TRAY_CLASS_NAME;
	if ( !RegisterClassEx(&wc) ) return -1;

	hwnd = CreateWindowEx(0,WC_TRAY_CLASS_NAME,tray_name,0,0,0,0,0,0,0,0,0);
	if ( hwnd == NULL ) return -1;
	UpdateWindow(hwnd);

	memset(&nid,0,sizeof(nid));
	nid.cbSize = sizeof(NOTIFYICONDATA);
	nid.hWnd = hwnd;
	nid.uID = 0;
	nid.uFlags = NIF_ICON|NIF_MESSAGE;
	nid.uCallbackMessage = WM_TRAY_CALLBACK_MESSAGE;
	Shell_NotifyIcon(NIM_ADD,&nid);

	_tray_update();
	return 0;
}

static int tray_loop( int blocking ) {
	MSG msg;
	if (blocking) { GetMessage(&msg,NULL,0,0); } else { PeekMessage(&msg,NULL,0,0,PM_REMOVE); }
	if ( msg.message == WM_QUIT ) {
		m_tray.shutdown_task();
		Shell_NotifyIcon(NIM_DELETE,&nid);
		if ( nid.hIcon != 0 ) DestroyIcon(nid.hIcon);
		if ( hmenu != 0 ) DestroyMenu(hmenu);
		UnregisterClass(WC_TRAY_CLASS_NAME,GetModuleHandle(NULL));
		return -1;
	}
	TranslateMessage(&msg);
	DispatchMessage(&msg);
	return 0;
}
