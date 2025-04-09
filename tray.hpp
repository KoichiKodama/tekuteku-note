#include <windows.h>
#include <shellapi.h>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"shell32.lib")

static int tray_exist( const char* tray_name );
static int tray_init( const char* tray_name, const char* icon_file_name );
static int tray_loop( int blocking );

#define WM_TRAY_CALLBACK_MESSAGE (WM_USER+1)
#define WC_TRAY_CLASS_NAME "TRAY"
#define ID_TRAY_FIRST 1000

static WNDCLASSEX wc;
static NOTIFYICONDATA nid;
static HWND hwnd;
static HMENU hmenu = NULL;

static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
			SendMessage(hwnd,WM_CLOSE,0,0);
			return 0;
		}
		break;
	case WM_ENDSESSION:
		if ( wparam == TRUE ) return 0;
		break;
	}
	return DefWindowProc(hwnd,msg,wparam,lparam);
}

static HMENU mk_exit_menu( UINT id ) {
	HMENU hmenu = CreatePopupMenu();
	MENUITEMINFO item;
	memset(&item,0,sizeof(item));
	item.cbSize = sizeof(MENUITEMINFO);
	item.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
	item.fType = 0;
	item.fState = 0;
	item.wID = id;
	item.dwTypeData = "exit";
	item.dwItemData = 0;
	InsertMenuItem(hmenu,id,TRUE,&item);
	return hmenu;
}

static int tray_exist( const char* tray_name ) { return ( FindWindow(WC_TRAY_CLASS_NAME,tray_name) == NULL ? 0 : 1 ); }

static int tray_init( const char* tray_name, const char* icon_file_name ) {
	memset(&wc,0,sizeof(wc));
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = tray_wnd_proc;
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
	HICON icon;
	ExtractIconEx(icon_file_name,0,NULL,&icon,1);
	nid.hIcon = icon;
	Shell_NotifyIcon(NIM_ADD,&nid);

	hmenu = mk_exit_menu(ID_TRAY_FIRST);
	SendMessage(hwnd,WM_INITMENUPOPUP,(WPARAM)hmenu,0);

	return 0;
}

static int tray_loop( int blocking ) {
	MSG msg;
	if (blocking) { GetMessage(&msg,NULL,0,0); } else { PeekMessage(&msg,NULL,0,0,PM_REMOVE); }
	if ( msg.message == WM_QUIT ) {
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
