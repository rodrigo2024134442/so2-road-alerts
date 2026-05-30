// monitor.c - SO2 2025/26 - M3
// gcc -municode -DUNICODE -D_UNICODE -o monitor.exe monitor.c -lcomctl32 -lgdi32
#include <windows.h>
#include <tchar.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdarg.h>

#define MAX_PLACARES 20
#define MAX_MSG      140

#define SHM_NAME_DEFAULT   _T("SO2_SHM")
#define SHM_MUTEX_DEFAULT  _T("SO2_SHM_MTX")
#define SHM_EVENT_DEFAULT  _T("SO2_SHM_EVT")

#define ID_LISTVIEW        100
#define ID_MENU_CONFIG     101
#define ID_MENU_ABOUT      102
#define ID_MENU_EXIT       103
#define ID_STATUSBAR       110

#define IDC_MAXALERTS      2001
#define IDC_SHMNAME        2002
#define IDC_MUTEXNAME      2003
#define IDC_EVENTNAME      2004

#define WM_APP_SHM_UPDATE  (WM_APP + 1)

typedef struct {
    DWORD identificador;
    DWORD duracao;
    TCHAR msg[MAX_MSG];
} SHM_PLACAR;

typedef struct {
    SHM_PLACAR placar[MAX_PLACARES];
    BOOL desligar;
    DWORD seq;
} SHM_ALERTA;

typedef struct {
    int maxAlertas;
    TCHAR shmName[128];
    TCHAR mutexName[128];
    TCHAR eventName[128];
} MON_CONFIG;

static HINSTANCE g_hInst = NULL;
static HWND g_hwndMain = NULL;
static HWND g_hwndList = NULL;
static HWND g_hwndStatus = NULL;
static MON_CONFIG g_cfg;

static HANDLE g_hMapFile = NULL;
static HANDLE g_hMutexShm = NULL;
static HANDLE g_hEventShm = NULL;
static SHM_ALERTA* g_shm = NULL;
static HANDLE g_hThread = NULL;
static HANDLE g_hStopEvent = NULL;

static int g_page = 0;
static BOOL g_debug = FALSE;
static HFONT g_hFont = NULL;
static HBRUSH g_hbrBg = NULL;

static const COLORREF CLR_BG = RGB(245, 242, 235);
static const COLORREF CLR_ROW_A = RGB(255, 253, 248);
static const COLORREF CLR_ROW_B = RGB(238, 234, 226);
static const COLORREF CLR_TEXT = RGB(34, 34, 34);

static void debugLog(const TCHAR* fmt, ...) {
    if (!g_debug || !fmt) return;
    {
        TCHAR buf[512];
        va_list args;
        va_start(args, fmt);
        _vstprintf_s(buf, 512, fmt, args);
        va_end(args);
        OutputDebugString(buf);
        OutputDebugString(_T("\n"));
    }
}

static void fecharShm(void) {
    debugLog(_T("[MON] fecharShm"));
    if (g_shm) { UnmapViewOfFile(g_shm); g_shm = NULL; }
    if (g_hMapFile) { CloseHandle(g_hMapFile); g_hMapFile = NULL; }
    if (g_hMutexShm) { CloseHandle(g_hMutexShm); g_hMutexShm = NULL; }
    if (g_hEventShm) { CloseHandle(g_hEventShm); g_hEventShm = NULL; }
}

static BOOL abrirShm(void) {
    debugLog(_T("[MON] abrirShm: shm='%s' mutex='%s' evento='%s'"), g_cfg.shmName, g_cfg.mutexName, g_cfg.eventName);
    fecharShm();
    g_hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, g_cfg.shmName);
    if (!g_hMapFile) return FALSE;
    g_shm = (SHM_ALERTA*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SHM_ALERTA));
    if (!g_shm) return FALSE;
    g_hMutexShm = OpenMutex(SYNCHRONIZE, FALSE, g_cfg.mutexName);
    if (!g_hMutexShm) return FALSE;
    g_hEventShm = OpenEvent(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, g_cfg.eventName);
    if (!g_hEventShm) return FALSE;
    debugLog(_T("[MON] abrirShm OK"));
    return TRUE;
}

static void copiarAlertas(SHM_PLACAR* out, int* outCount, BOOL* outDesligar) {
    int i, count = 0;
    if (!g_shm || !g_hMutexShm) { *outCount = 0; *outDesligar = FALSE; return; }
    WaitForSingleObject(g_hMutexShm, INFINITE);
    *outDesligar = g_shm->desligar ? TRUE : FALSE;
    for (i = 0; i < MAX_PLACARES; i++) {
        if (g_shm->placar[i].identificador != 0 && g_shm->placar[i].msg[0] != _T('\0')) {
            out[count++] = g_shm->placar[i];
        }
    }
    ReleaseMutex(g_hMutexShm);
    *outCount = count;
    debugLog(_T("[MON] copiarAlertas: total=%d desligar=%d"), count, *outDesligar);
}

static void atualizarLista(void) {
    SHM_PLACAR ativos[MAX_PLACARES];
    int total = 0;
    BOOL desligar = FALSE;
    int i, start, end;
    int totalPages;
    TCHAR buf[64];

    copiarAlertas(ativos, &total, &desligar);

    static int ultimoTotal = -1;
    if (total != ultimoTotal) { g_page = 0; ultimoTotal = total; }
    
    if (desligar) {
        debugLog(_T("[MON] desligar=TRUE, a terminar"));
        MessageBox(g_hwndMain, _T("A central encerrou. A terminar..."), _T("Monitor"), MB_OK | MB_ICONINFORMATION);
        PostQuitMessage(0);
        return;
    }

    totalPages = (g_cfg.maxAlertas > 0) ? (total + g_cfg.maxAlertas - 1) / g_cfg.maxAlertas : 1;
    if (totalPages <= 0) totalPages = 1;
    if (g_page >= totalPages) g_page = totalPages - 1;
    if (g_page < 0) g_page = 0;
    debugLog(_T("[MON] atualizarLista: total=%d page=%d/%d max=%d"), total, g_page + 1, totalPages, g_cfg.maxAlertas);

    ListView_DeleteAllItems(g_hwndList);

    start = g_page * g_cfg.maxAlertas;
    end = start + g_cfg.maxAlertas;
    if (end > total) end = total;

    for (i = start; i < end; i++) {
        LVITEM item;
        int idx = i - start;
        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.iSubItem = 0;
        _stprintf_s(buf, 64, _T("%u"), ativos[i].identificador);
        item.pszText = buf;
        ListView_InsertItem(g_hwndList, &item);

        ListView_SetItemText(g_hwndList, idx, 1, ativos[i].msg);
        _stprintf_s(buf, 64, _T("%u"), ativos[i].duracao);
        ListView_SetItemText(g_hwndList, idx, 2, buf);
    }

    if (g_hwndStatus) {
        TCHAR status[128];
        _stprintf_s(status, 128, _T("Alertas: %d | Pagina: %d/%d"), total, g_page + 1, totalPages);
        SendMessage(g_hwndStatus, SB_SETTEXT, 0, (LPARAM)status);
    }
}

static void iniciarLista(HWND hwndParent) {
    LVCOLUMN col;
    g_hwndList = CreateWindowEx(0, WC_LISTVIEW, _T(""),
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwndParent, (HMENU)ID_LISTVIEW, g_hInst, NULL);

    ListView_SetExtendedListViewStyle(g_hwndList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(g_hwndList, CLR_BG);
    ListView_SetTextBkColor(g_hwndList, CLR_BG);
    ListView_SetTextColor(g_hwndList, CLR_TEXT);
    debugLog(_T("[MON] ListView criada"));

    col.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    col.cx = 80; col.pszText = _T("ID"); col.iSubItem = 0;
    ListView_InsertColumn(g_hwndList, 0, &col);
    col.cx = 420; col.pszText = _T("Mensagem"); col.iSubItem = 1;
    ListView_InsertColumn(g_hwndList, 1, &col);
    col.cx = 120; col.pszText = _T("Duracao"); col.iSubItem = 2;
    ListView_InsertColumn(g_hwndList, 2, &col);

    if (g_hFont) {
        SendMessage(g_hwndList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        {
            HWND hHdr = ListView_GetHeader(g_hwndList);
            if (hHdr) SendMessage(hHdr, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }
    }
}

static void iniciarStatus(HWND hwndParent) {
    g_hwndStatus = CreateWindowEx(0, STATUSCLASSNAME, _T(""),
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwndParent, (HMENU)ID_STATUSBAR, g_hInst, NULL);
    if (g_hFont) SendMessage(g_hwndStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

static void ajustarLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (g_hwndStatus) {
        SendMessage(g_hwndStatus, WM_SIZE, 0, 0);
        {
            RECT rcStatus;
            GetWindowRect(g_hwndStatus, &rcStatus);
            rc.bottom -= (rcStatus.bottom - rcStatus.top);
        }
    }
    MoveWindow(g_hwndList, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

static HMENU criarMenu(void) {
    HMENU hMenu = CreateMenu();
    HMENU hFile = CreateMenu();
    AppendMenu(hFile, MF_STRING, ID_MENU_CONFIG, _T("Configuracao"));
    AppendMenu(hFile, MF_STRING, ID_MENU_ABOUT, _T("Acerca"));
    AppendMenu(hFile, MF_STRING, ID_MENU_EXIT, _T("Sair"));
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hFile, _T("Ficheiro"));
    return hMenu;
}

static DLGTEMPLATE* criarTemplateConfig(void) {
    static BYTE buffer[2048];
    DLGTEMPLATE* dlg = (DLGTEMPLATE*)buffer;
    BYTE* p = buffer;
    WORD* w;
    const TCHAR* fontName = _T("MS Shell Dlg");

    ZeroMemory(buffer, sizeof(buffer));
    dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dlg->cdit = 10;
    dlg->x = 10; dlg->y = 10; dlg->cx = 220; dlg->cy = 150;
    p += sizeof(DLGTEMPLATE);

    w = (WORD*)p; *w++ = 0; *w++ = 0; *w++ = 0; /* menu, class, title */
    *w++ = 8; /* font size */
    lstrcpy((TCHAR*)w, fontName);
    w += lstrlen(fontName) + 1;
    p = (BYTE*)w;

    /* helper macro for alignment */
    #define ALIGN_DWORD(x) (BYTE*)(((ULONG_PTR)(x) + 3) & ~3)

    /* Label Max alertas */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE;
        it->x = 6; it->y = 8; it->cx = 90; it->cy = 12; it->id = -1;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0082; /* STATIC */
        lstrcpy((TCHAR*)w, _T("Max alertas:"));
        w += lstrlen(_T("Max alertas:")) + 1; *w++ = 0;
        p = (BYTE*)w;
    }
    /* Edit Max alertas */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT;
        it->x = 110; it->y = 6; it->cx = 80; it->cy = 12; it->id = IDC_MAXALERTS;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0081; /* EDIT */
        *w++ = 0; *w++ = 0;
        p = (BYTE*)w;
    }
    /* Label SHM */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE;
        it->x = 6; it->y = 26; it->cx = 90; it->cy = 12; it->id = -1;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0082;
        lstrcpy((TCHAR*)w, _T("SHM:"));
        w += lstrlen(_T("SHM:")) + 1; *w++ = 0;
        p = (BYTE*)w;
    }
    /* Edit SHM */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT;
        it->x = 110; it->y = 24; it->cx = 90; it->cy = 12; it->id = IDC_SHMNAME;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0081;
        *w++ = 0; *w++ = 0;
        p = (BYTE*)w;
    }
    /* Label Mutex */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE;
        it->x = 6; it->y = 44; it->cx = 90; it->cy = 12; it->id = -1;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0082;
        lstrcpy((TCHAR*)w, _T("Mutex:"));
        w += lstrlen(_T("Mutex:")) + 1; *w++ = 0;
        p = (BYTE*)w;
    }
    /* Edit Mutex */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT;
        it->x = 110; it->y = 42; it->cx = 90; it->cy = 12; it->id = IDC_MUTEXNAME;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0081;
        *w++ = 0; *w++ = 0;
        p = (BYTE*)w;
    }
    /* Label Evento */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE;
        it->x = 6; it->y = 62; it->cx = 90; it->cy = 12; it->id = -1;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0082;
        lstrcpy((TCHAR*)w, _T("Evento:"));
        w += lstrlen(_T("Evento:")) + 1; *w++ = 0;
        p = (BYTE*)w;
    }
    /* Edit Evento */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT;
        it->x = 110; it->y = 60; it->cx = 90; it->cy = 12; it->id = IDC_EVENTNAME;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0081;
        *w++ = 0; *w++ = 0;
        p = (BYTE*)w;
    }
    /* OK button */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON;
        it->x = 40; it->y = 100; it->cx = 60; it->cy = 14; it->id = IDOK;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0080;
        lstrcpy((TCHAR*)w, _T("OK"));
        w += lstrlen(_T("OK")) + 1; *w++ = 0;
        p = (BYTE*)w;
    }
    /* Cancel button */
    p = ALIGN_DWORD(p);
    {
        DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
        it->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
        it->x = 120; it->y = 100; it->cx = 60; it->cy = 14; it->id = IDCANCEL;
        p += sizeof(DLGITEMTEMPLATE);
        w = (WORD*)p; *w++ = 0xFFFF; *w++ = 0x0080;
        lstrcpy((TCHAR*)w, _T("Cancelar"));
        w += lstrlen(_T("Cancelar")) + 1; *w++ = 0;
        p = (BYTE*)w;
    }

    #undef ALIGN_DWORD
    return (DLGTEMPLATE*)buffer;
}

static INT_PTR CALLBACK dlgConfigProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    TCHAR buf[256];
    switch (msg) {
    case WM_INITDIALOG:
        _stprintf_s(buf, 256, _T("%d"), g_cfg.maxAlertas);
        SetDlgItemText(hDlg, IDC_MAXALERTS, buf);
        SetDlgItemText(hDlg, IDC_SHMNAME, g_cfg.shmName);
        SetDlgItemText(hDlg, IDC_MUTEXNAME, g_cfg.mutexName);
        SetDlgItemText(hDlg, IDC_EVENTNAME, g_cfg.eventName);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            GetDlgItemText(hDlg, IDC_MAXALERTS, buf, 256);
            g_cfg.maxAlertas = _ttoi(buf);
            if (g_cfg.maxAlertas <= 0) g_cfg.maxAlertas = 1;
            if (g_cfg.maxAlertas > MAX_PLACARES) g_cfg.maxAlertas = MAX_PLACARES;
            GetDlgItemText(hDlg, IDC_SHMNAME, g_cfg.shmName, 128);
            GetDlgItemText(hDlg, IDC_MUTEXNAME, g_cfg.mutexName, 128);
            GetDlgItemText(hDlg, IDC_EVENTNAME, g_cfg.eventName, 128);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

static void abrirConfig(HWND hwnd) {
    DLGTEMPLATE* dlg = criarTemplateConfig();
    INT_PTR res = DialogBoxIndirectParam(g_hInst, dlg, hwnd, dlgConfigProc, 0);
    if (res == IDOK) {
        debugLog(_T("[MON] Config OK: max=%d shm='%s' mutex='%s' evt='%s'"), g_cfg.maxAlertas, g_cfg.shmName, g_cfg.mutexName, g_cfg.eventName);
        if (!abrirShm()) {
            MessageBox(hwnd, _T("Nao foi possivel ligar aos recursos do central."), _T("Monitor"), MB_OK | MB_ICONWARNING);
        }
        g_page = 0;
        atualizarLista();
    }
}

static DWORD WINAPI threadShm(LPVOID param) {
    HANDLE handles[2];
    (void)param;
    handles[0] = g_hEventShm;
    handles[1] = g_hStopEvent;
    debugLog(_T("[MON] threadShm iniciada"));
    while (1) {
        DWORD waitRes = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (waitRes == WAIT_OBJECT_0 + 1) break;
        if (waitRes == WAIT_OBJECT_0) {
            debugLog(_T("[MON] evento SHM sinalizado"));
            PostMessage(g_hwndMain, WM_APP_SHM_UPDATE, 0, 0);
            ResetEvent(g_hEventShm);
        }
    }
    debugLog(_T("[MON] threadShm a sair"));
    return 0;
}

static void iniciarThreadShm(void) {
    if (!g_hEventShm) return;
    if (g_hStopEvent) CloseHandle(g_hStopEvent);
    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_hThread = CreateThread(NULL, 0, threadShm, NULL, 0, NULL);
    debugLog(_T("[MON] threadShm criada"));
}

static void pararThreadShm(void) {
    if (g_hStopEvent) SetEvent(g_hStopEvent);
    if (g_hThread) {
        WaitForSingleObject(g_hThread, 3000);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    if (g_hStopEvent) { CloseHandle(g_hStopEvent); g_hStopEvent = NULL; }
    debugLog(_T("[MON] threadShm terminada"));
}

static void mostrarAcerca(HWND hwnd) {
    MessageBox(hwnd,
        _T("Autores:\n")
    _T("Joao Carlos Lopes Marques - 2024132275\n")
    _T("Rodrigo Beja da Costa - 2024134442\n"),
        _T("Acerca"),
        MB_OK | MB_ICONINFORMATION);
}

static void tratarPagina(int delta) {
    g_page += delta;
    if (g_page < 0) g_page = 0;
    debugLog(_T("[MON] paginacao: delta=%d page=%d"), delta, g_page);
    atualizarLista();
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        iniciarLista(hwnd);
        iniciarStatus(hwnd);
        ajustarLayout(hwnd);
        if (!abrirShm()) {
            MessageBox(hwnd, _T("Central nao esta a correr ou recursos nao existem."), _T("Monitor"), MB_OK | MB_ICONWARNING);
        }
        iniciarThreadShm();
        atualizarLista();
        return 0;
    case WM_SIZE:
        ajustarLayout(hwnd);
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lParam)->idFrom == ID_LISTVIEW && ((LPNMHDR)lParam)->code == LVN_KEYDOWN) {
            NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lParam;
            if (kd->wVKey == VK_NEXT) { tratarPagina(1); return 0; }
            if (kd->wVKey == VK_PRIOR) { tratarPagina(-1); return 0; }
        }
        if (((LPNMHDR)lParam)->idFrom == ID_LISTVIEW && ((LPNMHDR)lParam)->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int row = (int)cd->nmcd.dwItemSpec;
                cd->clrText = CLR_TEXT;
                cd->clrTextBk = (row % 2 == 0) ? CLR_ROW_A : CLR_ROW_B;
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_MENU_CONFIG:
            abrirConfig(hwnd);
            return 0;
        case ID_MENU_ABOUT:
            mostrarAcerca(hwnd);
            return 0;
        case ID_MENU_EXIT:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return 0;
    case WM_APP_SHM_UPDATE:
        atualizarLista();
        return 0;
    case WM_DESTROY:
        pararThreadShm();
        fecharShm();
        if (g_hFont) { DeleteObject(g_hFont); g_hFont = NULL; }
        if (g_hbrBg) { DeleteObject(g_hbrBg); g_hbrBg = NULL; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPTSTR lpCmdLine, int nCmdShow) {
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    (void)hPrev; (void)lpCmdLine;

    g_hInst = hInstance;
    g_cfg.maxAlertas = 10;
    _tcsncpy_s(g_cfg.shmName, 128, SHM_NAME_DEFAULT, _TRUNCATE);
    _tcsncpy_s(g_cfg.mutexName, 128, SHM_MUTEX_DEFAULT, _TRUNCATE);
    _tcsncpy_s(g_cfg.eventName, 128, SHM_EVENT_DEFAULT, _TRUNCATE);

    g_hFont = CreateFont(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_ROMAN, _T("Georgia"));
    g_hbrBg = CreateSolidBrush(CLR_BG);

    {
        TCHAR dbg[8];
        DWORD len = GetEnvironmentVariable(_T("MONITOR_DEBUG"), dbg, 8);
        g_debug = (len > 0);
    }
    debugLog(_T("[MON] debug ativo"));

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hbrBg;
    wc.lpszClassName = _T("SO2MonitorWnd");

    if (!RegisterClass(&wc)) return 1;

    hwnd = CreateWindow(wc.lpszClassName, _T("Monitor de Alertas"),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 400,
        NULL, criarMenu(), hInstance, NULL);
    if (!hwnd) return 1;
    g_hwndMain = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
