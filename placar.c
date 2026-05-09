// placar.c - SO2 2025/26 - M2
// gcc -municode -DUNICODE -D_UNICODE -o placar.exe placar.c
//
// Papel do processo Placar:
// - lê o nome do pipe do Registry ou da linha de comando;
// - liga-se à Central via named pipe;
// - recebe alertas/cancelamentos/encerramento;
// - usa uma thread de receção e uma thread para o timer do alerta.
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define MAX_MSG     140
#define PIPE_BUFFER 4096
#define REG_KEY     _T("SOFTWARE\\TrabSO2")
#define REG_VALUE   _T("NPIPE")

#pragma pack(push, 1)
typedef struct { BYTE tipo;                                    } MSG_CMD;
typedef struct { BYTE tipo; TCHAR msg[MAX_MSG]; DWORD duracao; } MSG_ALERTA;
typedef struct { BYTE tipo; DWORD identificador;               } MSG_ID;
#pragma pack(pop)

typedef union {
    BYTE       tipo;
    MSG_CMD    cmd;
    MSG_ALERTA alerta;
    MSG_ID     id;
} MSG_ANY;

/* Estado global do cliente (placar). */
static HANDLE           hPipeCentral = INVALID_HANDLE_VALUE;
static DWORD            identificador = 0;
static BOOL             ligado    = FALSE;
static BOOL             terminar  = FALSE;
static BOOL             deveDesligar = FALSE;
static BOOL             temAlerta = FALSE;
static HANDLE           hTimerAlerta = NULL;
static HANDLE           hThreadRecepcao = NULL;
static CRITICAL_SECTION csEstado;
static TCHAR            nomePipe[256];
static BOOL             debugAtivo = FALSE; /* TRUE para mostrar logs [DEBUG] */

/* Filtra logs de debug sem alterar o restante output da aplicação. */
static int debugPrintf(const TCHAR* formato, ...) {
    int resultado;
    va_list args;

    if (!debugAtivo && formato && _tcsncmp(formato, _T("[DEBUG]"), 7) == 0)
        return 0;

    va_start(args, formato);
    resultado = _vtprintf(formato, args);
    va_end(args);
    return resultado;
}

#undef _tprintf
#define _tprintf debugPrintf

/* ---------- helpers de suporte ---------- */

/* Formata a hora local para mostrar mensagens no ecrã. */
static void formatarHora(TCHAR* buf, int sz) {
    SYSTEMTIME st; GetLocalTime(&st);
    _stprintf_s(buf, sz, _T("%02d/%02d/%04d (%02d:%02d:%02d)"),
        st.wDay, st.wMonth, st.wYear,
        st.wHour, st.wMinute, st.wSecond);
}

/* Lê o nome da pipe guardado no Registry. */
static BOOL lerRegistry(TCHAR* out, DWORD outSz) {
    HKEY hKey; DWORD tipo, sz = outSz * sizeof(TCHAR);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return FALSE;
    { BOOL ok = (RegQueryValueEx(hKey, REG_VALUE, NULL, &tipo, (LPBYTE)out, &sz) == ERROR_SUCCESS);
      RegCloseKey(hKey); return ok; }
}

/* Guarda o nome da pipe no Registry para reutilização futura. */
static void escreverRegistry(const TCHAR* val) {
    HKEY hKey;
    RegCreateKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueEx(hKey, REG_VALUE, 0, REG_SZ,
        (const BYTE*)val, (DWORD)((_tcslen(val)+1)*sizeof(TCHAR)));
    RegCloseKey(hKey);
}

/* Envia uma mensagem para a Central e mostra logs de debug. */
static BOOL enviar(HANDLE h, const MSG_ANY* buf) {
    if (h == INVALID_HANDLE_VALUE) {
        _tprintf(_T("[DEBUG] enviar() - handle invalido, descartando tipo=%d\n"), buf->tipo);
        return FALSE;
    }
    DWORD w;
    BOOL result = WriteFile(h, buf, sizeof(MSG_ANY), &w, NULL) && w == sizeof(MSG_ANY);
    _tprintf(_T("[DEBUG] enviar() - tipo=%d, bytes=%u, sucesso=%d, handle=%p"), buf->tipo, w, result, h);
    if (!result) _tprintf(_T(", erro=%u"), GetLastError());
    _tprintf(_T("\n"));
    fflush(stdout);
    return result;
}

/* Recebe uma mensagem da Central com logs de debug. */
static BOOL receberMsg(HANDLE h, MSG_ANY* out) {
    DWORD r;
    _tprintf(_T("[DEBUG] receberMsg() ENTRADA - aguardando leitura..., handle=%p\n"), h);
    fflush(stdout);
    BOOL result = ReadFile(h, out, sizeof(MSG_ANY), &r, NULL) && r > 0;
    if (result) _tprintf(_T("[DEBUG] receberMsg() LEITURA COMPLETA - tipo=%d, bytes=%u, handle=%p\n"), out->tipo, r, h);
    else _tprintf(_T("[DEBUG] receberMsg() FALHA na leitura - bytes=%u, handle=%p\n"), r, h);
    fflush(stdout);
    return result;
}

/* Cancela e fecha o timer do alerta, se existir. */
static void cancelarTimer(void) {
    if (hTimerAlerta) {
        CancelWaitableTimer(hTimerAlerta);
        CloseHandle(hTimerAlerta);
        hTimerAlerta = NULL;
    }
}

/* ---------- thread do waitable timer ----------
    Esta thread acorda quando o alerta termina e avisa a Central. */

typedef struct { HANDLE hTimer; } TIMER_PARAM;

static DWORD WINAPI threadTimer(LPVOID param) {
    TIMER_PARAM* tp = (TIMER_PARAM*)param;
    HANDLE hTimer = tp->hTimer;
    free(tp);
    /* Espera até o timer expirar. */
    WaitForSingleObject(hTimer, INFINITE);
    EnterCriticalSection(&csEstado);
    if (temAlerta && !terminar) {
        TCHAR hora[64]; formatarHora(hora, 64);
        _tprintf(_T("\n%s: '---'\n> "), hora);
        temAlerta = FALSE;
        if (hPipeCentral != INVALID_HANDLE_VALUE) {
            MSG_CMD mc; mc.tipo = 3;
            MSG_ANY m_temp; m_temp.cmd = mc; enviar(hPipeCentral, &m_temp);
        }
        CloseHandle(hTimer);
        hTimerAlerta = NULL;
    }
    LeaveCriticalSection(&csEstado);
    return 0;
}

/* ---------- thread de receção ----------
    Recebe as mensagens da Central e atualiza o estado do placar. */

static DWORD WINAPI threadRecepcao(LPVOID param) {
    MSG_ANY m;
    OVERLAPPED ov = {0};
    (void)param;
    /* DEBUG: Thread de receção iniciada */
    _tprintf(_T("[DEBUG] threadRecepcao iniciada\n"));
    
    /* Evento usado para aguardar leituras overlapped com timeout curto. */
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    while (!terminar) {
        /* Primeiro, verificar se a interface pediu desligar. */
        if (deveDesligar) {
            _tprintf(_T("[DEBUG] deveDesligar=TRUE, saindo\n"));
            fflush(stdout);
            terminar = TRUE;
            break;
        }
        /* Leitura overlapped com timeout para não bloquear a interface. */
        DWORD r;
        ResetEvent(ov.hEvent);
        if (!ReadFile(hPipeCentral, &m, sizeof(MSG_ANY), &r, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                /* A operação ainda não terminou; esperar um pouco e tentar de novo. */
                DWORD waitRes = WaitForSingleObject(ov.hEvent, 100);
                if (waitRes == WAIT_TIMEOUT) {
                    continue;  /* Volta para verificar deveDesligar */
                } else if (waitRes != WAIT_OBJECT_0) {
                    _tprintf(_T("[DEBUG] receberMsg falhou (wait)\n"));
                    terminar = TRUE;
                    break;
                }
                GetOverlappedResult(hPipeCentral, &ov, &r, FALSE);
                if (r == 0) {
                    _tprintf(_T("[DEBUG] Conexao fechada\n"));
                    terminar = TRUE;
                    break;
                }
            } else {
                _tprintf(_T("[DEBUG] receberMsg falhou (erro=%u)\n"), err);
                fflush(stdout);
                if (!terminar && !deveDesligar) _tprintf(_T("\n[Placar] Ligacao perdida.\n"));
                terminar = TRUE;
                break;
            }
        } else if (r == 0) {
            _tprintf(_T("[DEBUG] Conexao fechada\n"));
            terminar = TRUE;
            break;
        }
        
        _tprintf(_T("[DEBUG] threadRecepcao processando tipo=%d\n"), m.tipo);

        if (m.tipo == 4) {                    /* novo alerta */
            TIMER_PARAM* tp; HANDLE ht; LARGE_INTEGER li;
            /* DEBUG: Recebida mensagem de novo alerta (tipo=4) */
            _tprintf(_T("[DEBUG] >>>>>> NOVO ALERTA RECEBIDO (tipo=4) <<<<<<\n"));
            _tprintf(_T("[DEBUG] Mensagem: '%s'\n"), m.alerta.msg);
            _tprintf(_T("[DEBUG] Duracao: %u segundos\n"), m.alerta.duracao);
            /* Confirmação do alerta enviada de volta à Central. */
            _tprintf(_T("[DEBUG] Enviando confirmacao de alerta (tipo=4)...\n"));
            enviar(hPipeCentral, &m);
            EnterCriticalSection(&csEstado);
            cancelarTimer(); temAlerta = TRUE;
            { TCHAR hora[64]; formatarHora(hora, 64);
              _tprintf(_T("\n%s: '%s'\n> "), hora, m.alerta.msg); }
            hTimerAlerta = CreateWaitableTimer(NULL, TRUE, NULL);
            li.QuadPart  = -(LONGLONG)m.alerta.duracao * 10000000LL;
            SetWaitableTimer(hTimerAlerta, &li, 0, NULL, NULL, FALSE);
            tp = (TIMER_PARAM*)malloc(sizeof(TIMER_PARAM));
            tp->hTimer = hTimerAlerta;
            ht = CreateThread(NULL, 0, threadTimer, (LPVOID)tp, 0, NULL);
            if (ht) CloseHandle(ht);
            LeaveCriticalSection(&csEstado);

        } else if (m.tipo == 5) {             /* cancelar */
            TCHAR hora[64];
            /* DEBUG: Recebida mensagem de cancelamento de alerta (tipo=5) */
            _tprintf(_T("[DEBUG] Cancelamento de alerta recebido\n"));
            EnterCriticalSection(&csEstado);
            cancelarTimer(); temAlerta = FALSE;
            LeaveCriticalSection(&csEstado);
            MSG_ANY m_temp; m_temp.cmd = m.cmd; enviar(hPipeCentral, &m_temp);
            formatarHora(hora, 64);
            _tprintf(_T("\n%s: '---'\n> "), hora);

        } else if (m.tipo == 2) {             /* confirmacao de desligar */
            _tprintf(_T("[DEBUG] Confirmacao de desligar recebida\n"));
            terminar = TRUE; break;

        } else if (m.tipo == 6) {             /* encerrar */
            /* DEBUG: Recebida mensagem de encerramento (tipo=6) */
            _tprintf(_T("[DEBUG] Mensagem de encerramento recebida\n"));
            _tprintf(_T("\n[Placar] Central encerrou. A terminar...\n"));
            terminar = TRUE; break;
        }
    }
    cancelarTimer();
    if (ov.hEvent) CloseHandle(ov.hEvent);
    return 0;
}

/* ---------- comandos do instalador ----------
    A interface só precisa de dois comandos: ligar e desligar. */

/* Liga o placar à Central, recebe o ID e arranca a thread de receção. */
static void cmdLigar(void) {
    TCHAR fullName[300]; DWORD mode; MSG_CMD mc; MSG_ANY resp; HANDLE ht;
    /* DEBUG: Comando ligar executado */
    _tprintf(_T("[DEBUG] Tentando ligar ao pipe '%s'...\n"), nomePipe);
    if (ligado) { _tprintf(_T("[Placar] Ja ligado (ID=%u).\n"), identificador); return; }
    _stprintf_s(fullName, 300, _T("\\\\.\\pipe\\%s"), nomePipe);
    hPipeCentral = CreateFile(fullName, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    /* DEBUG: CreateFile chamado para '%s' */
    _tprintf(_T("[DEBUG] CreateFile: handle=%p\n"), hPipeCentral);
    if (hPipeCentral == INVALID_HANDLE_VALUE) {
        _tprintf(_T("[Placar] Erro ao ligar: %u\n"), GetLastError()); return;
    }
    mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipeCentral, &mode, NULL, NULL);
    /* DEBUG: SetNamedPipeHandleState chamado */
    _tprintf(_T("[DEBUG] Mode=PIPE_READMODE_MESSAGE definido\n"));
    mc.tipo = 1;
    /* DEBUG: Enviando mensagem de ligação (tipo=1) */
    _tprintf(_T("[DEBUG] Enviando mensagem tipo 1 (ligar)\n"));
    { MSG_ANY m_temp; m_temp.cmd = mc; enviar(hPipeCentral, &m_temp); }
    if (!receberMsg(hPipeCentral, &resp) || resp.tipo != 7) {
        /* DEBUG: Falha ao receber resposta do central (tipo=7 esperado) */
        _tprintf(_T("[DEBUG] Erro ao receber mensagem de resposta. tipo=%d\n"), resp.tipo);
        _tprintf(_T("[Placar] Erro ao receber ID.\n"));
        CloseHandle(hPipeCentral); hPipeCentral = INVALID_HANDLE_VALUE; return;
    }
    identificador = resp.id.identificador;
    ligado = TRUE;
    /* DEBUG: Ligação bem-sucedida, recebida ID */
    _tprintf(_T("[DEBUG] Resposta recebida: tipo=7, ID=%u\n"), identificador);
    _tprintf(_T("Identificador = %u\n"), identificador);
    hThreadRecepcao = CreateThread(NULL, 0, threadRecepcao, NULL, 0, NULL);
    /* DEBUG: Thread de receção criada */
    _tprintf(_T("[DEBUG] threadRecepcao criada\n"));
}

/* Desliga de forma ordenada e espera a thread de receção terminar. */
static void cmdDesligar(void) {
    if (!ligado) { _tprintf(_T("[Placar] Nao ligado.\n")); terminar = TRUE; return; }
    _tprintf(_T("[Placar] A desligar...\n"));
    fflush(stdout);

    /* Enviar pedido de desligar ao central (tipo 2) */
    if (hPipeCentral != INVALID_HANDLE_VALUE) {
        MSG_ANY m_temp; m_temp.cmd.tipo = 2;
        enviar(hPipeCentral, &m_temp);
    }

    /* Sinalizar à threadRecepcao para terminar (caso central não responda) */
    deveDesligar = TRUE;

    /* Aguardar que a threadRecepcao termine (recebeu tipo 2 ou timeout) */
    if (hThreadRecepcao != NULL) {
        WaitForSingleObject(hThreadRecepcao, 3000);
        CloseHandle(hThreadRecepcao);
        hThreadRecepcao = NULL;
    }

    /* Fechar o pipe */
    if (hPipeCentral != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipeCentral);
        hPipeCentral = INVALID_HANDLE_VALUE;
    }

    ligado = FALSE;
    terminar = TRUE;
    _tprintf(_T("[Placar] Desligado.\n"));
    fflush(stdout);
}

/* ---------- main ---------- */

int _tmain(int argc, TCHAR* argv[]) {
    TCHAR linha[256];
    if (argc >= 2) { _tcsncpy_s(nomePipe, 256, argv[1], _TRUNCATE); escreverRegistry(nomePipe); }
    else if (!lerRegistry(nomePipe, 256)) {
        _tprintf(_T("[Placar] Erro: pipe nao especificado e nao existe no Registry.\n")); return 1;
    }
    _tprintf(_T("[Placar] NamedPipe = '%s'\n"), nomePipe);
    _tprintf(_T("Comandos: ligar | desligar\n\n"));
    InitializeCriticalSection(&csEstado);

    /* Ciclo simples de comandos do utilizador. */
    while (!terminar) {
        TCHAR* nl;
        _tprintf(_T("> "));
        if (!_fgetts(linha, 256, stdin)) break;
        nl = _tcschr(linha, _T('\n')); if (nl) *nl = _T('\0');
        if (!_tcslen(linha)) continue;
        if      (!_tcsicmp(linha, _T("ligar")))    cmdLigar();
        else if (!_tcsicmp(linha, _T("desligar"))) { cmdDesligar(); break; }
        else _tprintf(_T("Comandos validos: ligar, desligar\n"));
    }
    cancelarTimer();
    if (hPipeCentral != INVALID_HANDLE_VALUE) CloseHandle(hPipeCentral);
    DeleteCriticalSection(&csEstado);
    return 0;
}