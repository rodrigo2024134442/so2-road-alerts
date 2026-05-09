// central.c - SO2 2025/26 - M2
// gcc -municode -DUNICODE -D_UNICODE -o central.exe central.c
//
// Papel do processo Central:
// - aceita ligações de vários placares via named pipe;
// - mantém o estado de cada placar;
// - envia alertas, cancelamentos e encerramento;
// - usa uma thread por placar para não bloquear a receção.
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define MAX_PLACARES 20
#define MAX_MSG      140
#define PIPE_BUFFER  4096

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

typedef struct {
    BOOL  ativo;
    DWORD identificador;
    HANDLE hPipe;
    HANDLE hEventoFila;  /* Evento para sinalizar fila de envio */
    BOOL  temAlerta;
    TCHAR msgAlerta[MAX_MSG];
    DWORD duracaoAlerta;
    /* Fila de mensagens a enviar */
    MSG_ANY filaEnvio[50];
    int filaInicio;
    int filaFim;
} PLACAR_INFO;

/* Estado global do servidor central. */
static PLACAR_INFO      placares[MAX_PLACARES];
static DWORD            proximoId = 1;
static CRITICAL_SECTION csPlacares;
static BOOL             encerrar  = FALSE;
static HANDLE           hEventoEncerrar = NULL;
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

/* ---------- helpers de pesquisa/estado ---------- */

/* Procura um placar pelo handle do pipe associado. */
static int slotPorPipe(HANDLE h) {
    int i;
    for (i = 0; i < MAX_PLACARES; i++)
        if (placares[i].ativo && placares[i].hPipe == h) return i;
    return -1;
}
/* Procura um placar pelo identificador atribuído pela central. */
static int slotPorId(DWORD id) {
    int i;
    for (i = 0; i < MAX_PLACARES; i++)
        if (placares[i].ativo && placares[i].identificador == id) return i;
    return -1;
}
/* Encontra um índice livre na tabela de placares. */
static int slotLivre(void) {
    int i;
    for (i = 0; i < MAX_PLACARES; i++)
        if (!placares[i].ativo) return i;
    return -1;
}

/* Enfileira uma mensagem para um placar específico.
    A thread do placar vai consumir esta fila e enviar pela pipe. */
static BOOL adicionarFilaEnvio(int slot, const MSG_ANY* msg) {
    if (slot < 0 || slot >= MAX_PLACARES) return FALSE;
    int prox = (placares[slot].filaFim + 1) % 50;
    if (prox == placares[slot].filaInicio) return FALSE;  /* fila cheia */
    placares[slot].filaEnvio[placares[slot].filaFim] = *msg;
    placares[slot].filaFim = prox;
    _tprintf(_T("[DEBUG] Addicionada msg tipo=%d à fila do placar %u\n"), msg->tipo, placares[slot].identificador);
    /* Sinalizar evento para acordar a threadPlacar */
    if (placares[slot].hEventoFila) SetEvent(placares[slot].hEventoFila);
    return TRUE;
}

/* Envio com log de debug para facilitar a defesa e a depuração. */
static BOOL enviar(HANDLE h, const MSG_ANY* buf) {
    if (h == INVALID_HANDLE_VALUE) {
        _tprintf(_T("[DEBUG] enviar() - handle invalido, descartando tipo=%d\n"), buf->tipo);
        return FALSE;
    }
    DWORD w;
    _tprintf(_T("[DEBUG] enviar() ENTRADA - tipo=%d, handle=%p\n"), buf->tipo, h);
    fflush(stdout);
    BOOL result = WriteFile(h, buf, sizeof(MSG_ANY), &w, NULL) && w == sizeof(MSG_ANY);
    _tprintf(_T("[DEBUG] enviar() ESCRITA COMPLETA - tipo=%d, bytes=%u, sucesso=%d, handle=%p"), buf->tipo, w, result, h);
    if (!result) _tprintf(_T(", erro=%u"), GetLastError());
    _tprintf(_T("\n"));
    fflush(stdout);
    return result;
}
/* Receção com log de debug. Na prática, o central lê sempre na threadPlacar. */
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

/* ---------- thread por placar ----------
    Cada ligação ativa tem a sua própria thread.
    Vantagens:
    - a leitura fica isolada por placar;
    - o servidor mantém-se responsivo;
    - evita bloquear o processamento dos restantes placares. */

static DWORD WINAPI threadPlacar(LPVOID param) {
    HANDLE  hPipe = (HANDLE)param;
    MSG_ANY m;
    OVERLAPPED ov = {0};
    /* DEBUG: Thread de placar iniciada para novo cliente */
    _tprintf(_T("[DEBUG] threadPlacar iniciada\n"));
    
    /* Evento usado para esperar leituras assíncronas com timeout. */
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    while (!encerrar) {
        /* 1) Processar primeiro a fila de mensagens pendentes. */
        EnterCriticalSection(&csPlacares);
        { int idx = slotPorPipe(hPipe);
          if (idx >= 0) {
              while (placares[idx].filaInicio != placares[idx].filaFim) {
                  MSG_ANY filaMsg = placares[idx].filaEnvio[placares[idx].filaInicio];
                  placares[idx].filaInicio = (placares[idx].filaInicio + 1) % 50;
                  ResetEvent(placares[idx].hEventoFila);
                  LeaveCriticalSection(&csPlacares);
                  _tprintf(_T("[DEBUG] Enviando fila: tipo=%d, handle=%p\n"), filaMsg.tipo, hPipe);
                  enviar(hPipe, &filaMsg);
                  EnterCriticalSection(&csPlacares);
                  idx = slotPorPipe(hPipe);
                  if (idx < 0) break;  /* placar desligou */
              }
          }
        }
        LeaveCriticalSection(&csPlacares);
        
        /* 2) Aguardar nova mensagem do placar sem bloquear indefinidamente. */
        DWORD r;
        ResetEvent(ov.hEvent);
        if (!ReadFile(hPipe, &m, sizeof(MSG_ANY), &r, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                /* Leitura pendente: espera curta para voltar à fila de envio se necessário. */
                DWORD waitRes = WaitForSingleObject(ov.hEvent, 100);  /* 100ms timeout */
                if (waitRes == WAIT_TIMEOUT) {
                    continue;  /* Volta para processar fila novamente */
                } else if (waitRes != WAIT_OBJECT_0) {
                    /* Erro na espera: marcar o placar como desligado. */
                    EnterCriticalSection(&csPlacares);
                    { int idx = slotPorPipe(hPipe);
                      if (idx >= 0) {
                          _tprintf(_T("[Central] Placar %u perdeu ligacao (wait erro).\n"), placares[idx].identificador);
                          placares[idx].ativo = FALSE;
                          if (placares[idx].hEventoFila) { CloseHandle(placares[idx].hEventoFila); placares[idx].hEventoFila = NULL; }
                          CloseHandle(placares[idx].hPipe);
                          placares[idx].hPipe = INVALID_HANDLE_VALUE;
                      } }
                    LeaveCriticalSection(&csPlacares);
                    break;
                }
                /* Leitura concluída com sucesso: obter número de bytes. */
                GetOverlappedResult(hPipe, &ov, &r, FALSE);
                if (r == 0) {
                    /* Pipe fechado pelo cliente. */
                    EnterCriticalSection(&csPlacares);
                    { int idx = slotPorPipe(hPipe);
                      if (idx >= 0) {
                          _tprintf(_T("[Central] Placar %u perdeu ligacao.\n"), placares[idx].identificador);
                          placares[idx].ativo = FALSE;
                          if (placares[idx].hEventoFila) { CloseHandle(placares[idx].hEventoFila); placares[idx].hEventoFila = NULL; }
                          CloseHandle(placares[idx].hPipe);
                          placares[idx].hPipe = INVALID_HANDLE_VALUE;
                      } }
                    LeaveCriticalSection(&csPlacares);
                    break;
                }
            } else {
                /* Erro de leitura imediato. */
                EnterCriticalSection(&csPlacares);
                { int idx = slotPorPipe(hPipe);
                  if (idx >= 0) {
                      _tprintf(_T("[Central] Placar %u perdeu ligacao (erro=%u).\n"), placares[idx].identificador, err);
                      placares[idx].ativo = FALSE;
                      if (placares[idx].hEventoFila) { CloseHandle(placares[idx].hEventoFila); placares[idx].hEventoFila = NULL; }
                      CloseHandle(placares[idx].hPipe);
                      placares[idx].hPipe = INVALID_HANDLE_VALUE;
                  } }
                LeaveCriticalSection(&csPlacares);
                break;
            }
        } else if (r == 0) {
            /* Pipe fechado pelo cliente. */
            EnterCriticalSection(&csPlacares);
            { int idx = slotPorPipe(hPipe);
              if (idx >= 0) {
                  _tprintf(_T("[Central] Placar %u perdeu ligacao.\n"), placares[idx].identificador);
                  placares[idx].ativo = FALSE;
                  if (placares[idx].hEventoFila) { CloseHandle(placares[idx].hEventoFila); placares[idx].hEventoFila = NULL; }
                  CloseHandle(placares[idx].hPipe);
                  placares[idx].hPipe = INVALID_HANDLE_VALUE;
              } }
            LeaveCriticalSection(&csPlacares);
            break;
        }

        /* 3) Processamento do protocolo de mensagens. */
        if (m.tipo == 1) {                       /* ligar */
            DWORD  id;
            MSG_ID mi;
            /* DEBUG: Recebida mensagem de ligação (tipo=1) */
            _tprintf(_T("[DEBUG] Mensagem tipo 1 (ligar) recebida\n"));
            EnterCriticalSection(&csPlacares);
            { int idx = slotLivre();
              if (idx < 0) {
                  LeaveCriticalSection(&csPlacares);
                  _tprintf(_T("[Central] Sem slots!\n"));
                  CloseHandle(hPipe); return 0;
              }
              placares[idx].ativo         = TRUE;
              placares[idx].hPipe         = hPipe;
              placares[idx].temAlerta     = FALSE;
              placares[idx].filaInicio    = 0;
              placares[idx].filaFim       = 0;
              placares[idx].hEventoFila   = CreateEvent(NULL, TRUE, FALSE, NULL);
              placares[idx].identificador = proximoId++;
              id = placares[idx].identificador; }
            LeaveCriticalSection(&csPlacares);
            /* Resposta com o ID atribuído ao novo placar. */
            mi.tipo = 7; mi.identificador = id;
            { MSG_ANY m_temp; m_temp.id = mi; enviar(hPipe, &m_temp); }
            _tprintf(_T("[Central] Placar ligado: ID=%u\n"), id);

        } else if (m.tipo == 2) {                /* desligar */
            DWORD id;
            /* DEBUG: Recebida mensagem de desligação (tipo=2) */
            _tprintf(_T("[DEBUG] Mensagem tipo 2 (desligar) recebida\n"));
            EnterCriticalSection(&csPlacares);
            { int idx = slotPorPipe(hPipe);
              id = (idx >= 0) ? placares[idx].identificador : 0;
              if (idx >= 0) { placares[idx].ativo = FALSE; placares[idx].hPipe = INVALID_HANDLE_VALUE; } }
            LeaveCriticalSection(&csPlacares);
            /* Confirmação enviada antes de fechar o handle. */
            { MSG_ANY m_temp; m_temp.cmd.tipo = 2; enviar(hPipe, &m_temp); }
            _tprintf(_T("[Central] Placar %u desligado.\n"), id);
            CloseHandle(hPipe);
            return 0;

        } else if (m.tipo == 3) {                /* fim do alerta */
            /* DEBUG: Recebida mensagem de fim de alerta (tipo=3) */
            _tprintf(_T("[DEBUG] Mensagem tipo 3 (fim alerta) recebida\n"));
            EnterCriticalSection(&csPlacares);
            { int idx = slotPorPipe(hPipe);
              if (idx >= 0) {
                  placares[idx].temAlerta = FALSE;
                  _tprintf(_T("[Central] Placar %u: alerta terminou.\n"),
                           placares[idx].identificador);
              } }
            LeaveCriticalSection(&csPlacares);

        } else if (m.tipo == 4) {                /* confirmacao de alerta */
            /* DEBUG: Recebida confirmação de alerta (tipo=4) */
            _tprintf(_T("[DEBUG] Mensagem tipo 4 (confirmacao alerta) recebida\n"));
            EnterCriticalSection(&csPlacares);
            { int idx = slotPorPipe(hPipe);
              if (idx >= 0) {
                  placares[idx].temAlerta = TRUE;
                  _tcsncpy_s(placares[idx].msgAlerta, MAX_MSG, m.alerta.msg, _TRUNCATE);
                  placares[idx].duracaoAlerta = m.alerta.duracao;
                  _tprintf(_T("[Central] Placar %u confirmou alerta.\n"),
                           placares[idx].identificador);
              } }
            LeaveCriticalSection(&csPlacares);

        } else if (m.tipo == 5) {                /* confirmacao de cancelamento */
            /* DEBUG: Recebida confirmação de cancelamento (tipo=5) */
            _tprintf(_T("[DEBUG] Mensagem tipo 5 (confirmacao cancelamento) recebida\n"));
            EnterCriticalSection(&csPlacares);
            { int idx = slotPorPipe(hPipe);
              if (idx >= 0) {
                  placares[idx].temAlerta = FALSE;
                  _tprintf(_T("[Central] Placar %u confirmou cancelamento.\n"),
                           placares[idx].identificador);
              } }
            LeaveCriticalSection(&csPlacares);
        }
    }
    /* Limpeza do evento da operação assíncrona. */
    if (ov.hEvent) CloseHandle(ov.hEvent);
    return 0;
}

/* ---------- thread de aceitação ----------
   Fica responsável por aceitar novas ligações no named pipe. */

static DWORD WINAPI threadAceitar(LPVOID param) {
    TCHAR fullName[300];
    (void)param;
    _stprintf_s(fullName, 300, _T("\\\\.\\pipe\\%s"), nomePipe);

    while (!encerrar) {
        OVERLAPPED ov = {0};
        HANDLE waitHandles[2];

        HANDLE hPipe = CreateNamedPipe(
            fullName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            MAX_PLACARES, PIPE_BUFFER, PIPE_BUFFER, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) break;

        ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        /* Tentativa de aceitação assíncrona do cliente. */
        if (!ConnectNamedPipe(hPipe, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                /* Espera pela ligação ou pelo pedido de encerramento global. */
                waitHandles[0] = ov.hEvent;
                waitHandles[1] = hEventoEncerrar;
                DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (waitRes != WAIT_OBJECT_0) {
                    /* Encerramento solicitado. */
                    CancelIo(hPipe);
                    CloseHandle(ov.hEvent);
                    CloseHandle(hPipe);
                    break;
                }
                DWORD r;
                if (!GetOverlappedResult(hPipe, &ov, &r, FALSE) &&
                    GetLastError() != ERROR_PIPE_CONNECTED) {
                    CloseHandle(ov.hEvent);
                    CloseHandle(hPipe);
                    continue;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(ov.hEvent);
                CloseHandle(hPipe);
                continue;
            }
        }
        CloseHandle(ov.hEvent);

        if (encerrar) { CloseHandle(hPipe); break; }

        /* Criar a thread dedicada ao novo placar. */
        { HANDLE ht = CreateThread(NULL, 0, threadPlacar, (LPVOID)hPipe, 0, NULL);
          /* DEBUG: Thread de placar criada */
          _tprintf(_T("[DEBUG] Nova thread de placar criada para handle %p\n"), hPipe);
          if (ht) CloseHandle(ht); }
    }
    return 0;
}

    /* ---------- comandos do administrador ----------
       Estes comandos só escrevem na fila/pipe; a leitura fica na threadPlacar. */

static void cmdAlerta(TCHAR* args) {
    TCHAR  buf[512], msg[MAX_MSG];
    TCHAR* tokens[128]; int n = 0, i;
    TCHAR* ctx = NULL; TCHAR* tok;
    DWORD  idPlacar, duracao;
    MSG_ALERTA ma;
    /* DEBUG: Comando alerta recebido */
    _tprintf(_T("[DEBUG] Comando alerta: args='%s'\n"), args ? args : _T("(null)"));

    if (!args || !_tcslen(args)) { _tprintf(_T("Uso: alerta <msg> <duracao> <id|0>\n")); return; }
    _tcsncpy_s(buf, 512, args, _TRUNCATE);
    tok = _tcstok_s(buf, _T(" \t"), &ctx);
    while (tok && n < 128) { tokens[n++] = tok; tok = _tcstok_s(NULL, _T(" \t"), &ctx); }
    if (n < 3) { _tprintf(_T("Uso: alerta <msg> <duracao> <id|0>\n")); return; }

    idPlacar = (DWORD)_ttoi(tokens[n-1]);
    duracao  = (DWORD)_ttoi(tokens[n-2]);
    msg[0] = _T('\0');
    for (i = 0; i < n-2; i++) {
        if (i) _tcscat_s(msg, MAX_MSG, _T(" "));
        _tcscat_s(msg, MAX_MSG, tokens[i]);
    }
    ma.tipo = 4;
    _tcsncpy_s(ma.msg, MAX_MSG, msg, _TRUNCATE);
    ma.duracao = duracao;

    /* O alerta vai para todos os placares ou apenas para um ID específico. */
    _tprintf(_T("[DEBUG] Adicionando alerta à fila de envio\n"));
    EnterCriticalSection(&csPlacares);
    if (idPlacar == 0) {
        _tprintf(_T("[DEBUG] Adicionando para TODOS (id=0)\n"));
        for (i = 0; i < MAX_PLACARES; i++) {
            if (!placares[i].ativo) continue;
            _tprintf(_T("[DEBUG] Adicionando à fila do placar index=%d\n"), i);
            /* Se já existir alerta ativo, o novo alerta substitui o anterior. */
            if (placares[i].temAlerta) {
                MSG_ANY m_cancel; m_cancel.cmd.tipo = 5;
                adicionarFilaEnvio(i, &m_cancel);
            }
            adicionarFilaEnvio(i, (MSG_ANY*)&ma);
            placares[i].temAlerta = TRUE;
            _tcsncpy_s(placares[i].msgAlerta, MAX_MSG, ma.msg, _TRUNCATE);
            placares[i].duracaoAlerta = ma.duracao;
        }
    } else {
        int idx = slotPorId(idPlacar);
        _tprintf(_T("[DEBUG] Procurando placar ID=%u, slot=%d\n"), idPlacar, idx);
        if (idx < 0) { LeaveCriticalSection(&csPlacares); _tprintf(_T("[Central] Placar %u nao existe.\n"), idPlacar); return; }
        _tprintf(_T("[DEBUG] Adicionado alerta à fila do placar ID=%u\n"), idPlacar);
        /* Se já existir alerta ativo, o novo alerta substitui o anterior. */
        if (placares[idx].temAlerta) {
            MSG_ANY m_cancel; m_cancel.cmd.tipo = 5;
            adicionarFilaEnvio(idx, &m_cancel);
        }
        adicionarFilaEnvio(idx, (MSG_ANY*)&ma);
        placares[idx].temAlerta = TRUE;
        _tcsncpy_s(placares[idx].msgAlerta, MAX_MSG, ma.msg, _TRUNCATE);
        placares[idx].duracaoAlerta = ma.duracao;
    }
    LeaveCriticalSection(&csPlacares);
    _tprintf(_T("[DEBUG] Alerta adicionado à fila com sucesso\n"));
}

static void cmdCancelar(TCHAR* args) {
    DWORD id; int idx; MSG_CMD mc;
    /* DEBUG: Comando cancelar recebido */
    _tprintf(_T("[DEBUG] Comando cancelar: args='%s'\n"), args ? args : _T("(null)"));
    if (!args || !_tcslen(args)) { _tprintf(_T("Uso: cancelar <id>\n")); return; }
    id = (DWORD)_ttoi(args);
    mc.tipo = 5;
    
    /* Só faz sentido cancelar se o placar existir e tiver alerta ativo. */
    EnterCriticalSection(&csPlacares);
    idx = slotPorId(id);
    if (idx < 0) { LeaveCriticalSection(&csPlacares); _tprintf(_T("[Central] Placar %u nao existe.\n"), id); return; }
    if (!placares[idx].temAlerta) { LeaveCriticalSection(&csPlacares); _tprintf(_T("[Central] Placar %u sem alerta.\n"), id); return; }
    _tprintf(_T("[DEBUG] Adicionado cancelamento à fila do placar ID=%u\n"), id);
    adicionarFilaEnvio(idx, (MSG_ANY*)&mc);
    LeaveCriticalSection(&csPlacares);
}

/* Lista os placares ativos e o respetivo estado atual. */
static void cmdListar(void) {
    int i, c = 0;
    EnterCriticalSection(&csPlacares);
    _tprintf(_T("\n--- Placares registados ---\n"));
    for (i = 0; i < MAX_PLACARES; i++) {
        if (!placares[i].ativo) continue; c++;
        if (placares[i].temAlerta)
            _tprintf(_T("  ID=%u | '%s' (%us)\n"),
                placares[i].identificador, placares[i].msgAlerta, placares[i].duracaoAlerta);
        else
            _tprintf(_T("  ID=%u | sem alerta\n"), placares[i].identificador);
    }
    if (!c) _tprintf(_T("  (nenhum placar ligado)\n"));
    _tprintf(_T("---------------------------\n\n"));
    LeaveCriticalSection(&csPlacares);
}

/* Encerramento ordenado:
    1) enfileira o comando de fecho;
    2) dá tempo para as threads enviarem;
    3) sinaliza a aceitação e limpa os handles. */
static void cmdEncerrar(void) {
    int i; MSG_CMD mc; mc.tipo = 6;
    _tprintf(_T("[Central] A encerrar...\n"));

    /* Adicionar tipo 6 à fila de cada placar ANTES de setar encerrar=TRUE,
       para que as threadPlacar ainda estejam a correr e possam enviar */
    EnterCriticalSection(&csPlacares);
    for (i = 0; i < MAX_PLACARES; i++) {
        if (!placares[i].ativo) continue;
        MSG_ANY m_temp; m_temp.cmd = mc;
        adicionarFilaEnvio(i, &m_temp);
    }
    LeaveCriticalSection(&csPlacares);

    /* Dar tempo às threadPlacar para enviarem o tipo 6 */
    Sleep(300);

    /* Agora setar encerrar e sinalizar threadAceitar */
    encerrar = TRUE;
    if (hEventoEncerrar) SetEvent(hEventoEncerrar);

    /* Fechar handles dos placares ainda ativos */
    EnterCriticalSection(&csPlacares);
    for (i = 0; i < MAX_PLACARES; i++) {
        if (placares[i].hEventoFila) {
            CloseHandle(placares[i].hEventoFila);
            placares[i].hEventoFila = NULL;
        }
        if (placares[i].hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(placares[i].hPipe);
            placares[i].hPipe = INVALID_HANDLE_VALUE;
        }
        placares[i].ativo = FALSE;
    }
    LeaveCriticalSection(&csPlacares);
}

/* ---------- main ---------- */

int _tmain(int argc, TCHAR* argv[]) {
    HANDLE hAceitar; TCHAR linha[512]; int i;
    if (argc < 2) { _tprintf(_T("Uso: central.exe <nome_pipe>\n")); return 1; }
    _tcsncpy_s(nomePipe, 256, argv[1], _TRUNCATE);
    _tprintf(_T("[Central] Named pipe: \\\\.\\pipe\\%s\n"), nomePipe);
    _tprintf(_T("Comandos: alerta <msg> <dur> <id|0>  cancelar <id>  listar  encerrar\n\n"));

    memset(placares, 0, sizeof(placares));
    for (i = 0; i < MAX_PLACARES; i++) placares[i].hPipe = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&csPlacares);
    hEventoEncerrar = CreateEvent(NULL, TRUE, FALSE, NULL);
    hAceitar = CreateThread(NULL, 0, threadAceitar, NULL, 0, NULL);

    /* Ciclo de comandos do administrador. */
    while (!encerrar) {
        TCHAR* esp; TCHAR* args; TCHAR* nl;
        _tprintf(_T("> "));
        if (!_fgetts(linha, 512, stdin)) break;
        nl = _tcschr(linha, _T('\n')); if (nl) *nl = _T('\0');
        if (!_tcslen(linha)) continue;
        esp = _tcschr(linha, _T(' ')); args = NULL;
        if (esp) { *esp = _T('\0'); args = esp+1; while(*args==_T(' ')) args++; }

        if      (!_tcsicmp(linha, _T("alerta")))   cmdAlerta(args);
        else if (!_tcsicmp(linha, _T("cancelar"))) cmdCancelar(args);
        else if (!_tcsicmp(linha, _T("listar")))   cmdListar();
        else if (!_tcsicmp(linha, _T("encerrar"))) { cmdEncerrar(); break; }
        else _tprintf(_T("Comando desconhecido.\n"));
    }
    /* Espera curta para a thread de aceitação terminar após o encerramento. */
    WaitForSingleObject(hAceitar, 3000);
    CloseHandle(hAceitar);
    if (hEventoEncerrar) { CloseHandle(hEventoEncerrar); hEventoEncerrar = NULL; }
    DeleteCriticalSection(&csPlacares);
    return 0;
}