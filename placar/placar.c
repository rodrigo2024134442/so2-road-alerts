#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <strsafe.h>

#define REG_KEY     _T("Software\\TrabSO2")
#define REG_VALUE   _T("NPIPE")
#define MAX_PIPE    256

// Estrutura de novo alerta (igual à usada na comunicação central -> placar)
typedef struct {
    BYTE  tipo;       // 4 = novo alerta
    TCHAR msg[140];
    DWORD duracao;
} MSG_ALERTA;

// Handles globais partilhados entre threads
HANDLE gEventoTerminar = NULL;   // sinalizado quando utilizador escreve "desligar"
HANDLE gEventoNotificar = NULL;  // evento com nome "notificar" (reset manual)
TCHAR  gNomePipe[MAX_PIPE];      // nome do pipe lido do Registry / args

// Obtém a hora atual formatada
void obterHoraFormatada(TCHAR* buf, int bufSize) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    StringCchPrintf(buf, bufSize, _T("%02d/%02d/%04d (%02d:%02d:%02d)"),
        st.wDay, st.wMonth, st.wYear,
        st.wHour, st.wMinute, st.wSecond);
}

// Lê o nome do pipe do Registry
BOOL lerRegistryNPIPE(TCHAR* buf, DWORD bufSize) {
    HKEY hKey;
    if (RegOpenKeyEx(
        HKEY_CURRENT_USER, // chave raiz
        REG_KEY,           // subchave
        0,                 // opções
        KEY_READ,          // permissões
        &hKey              // handle
    ) != ERROR_SUCCESS)
        return FALSE;

    DWORD tipo, tamanho = bufSize * sizeof(TCHAR);
    BOOL ok = (RegQueryValueEx(
        hKey,        // chave
        REG_VALUE,   // nome do valor
        NULL,        // reservado
        &tipo,       // tipo de dados
        (LPBYTE)buf, // buffer destino
        &tamanho     // tamanho
    ) == ERROR_SUCCESS);

    RegCloseKey(hKey);
    return ok;
}

// Escreve o nome do pipe no Registry
BOOL escreverRegistryNPIPE(const TCHAR* valor) {
    HKEY hKey;
    DWORD disp;

    if (RegCreateKeyEx(
        HKEY_CURRENT_USER,       // chave raiz
        REG_KEY,                 // subchave
        0,                       // reservado
        NULL,                    // classe
        REG_OPTION_NON_VOLATILE, // persistente
        KEY_WRITE,               // permissões
        NULL,                    // segurança
        &hKey,                   // handle
        &disp                    // info criação
    ) != ERROR_SUCCESS)
        return FALSE;

    BOOL ok = (RegSetValueEx(
        hKey,                                          // chave
        REG_VALUE,                                     // nome
        0,                                             // reservado
        REG_SZ,                                        // tipo string
        (const BYTE*)valor,                            // dados
        (DWORD)((_tcslen(valor) + 1) * sizeof(TCHAR)) // tamanho
    ) == ERROR_SUCCESS);

    RegCloseKey(hKey);
    return ok;
}

// Lê um alerta do Registry
BOOL lerAlertaRegistry(MSG_ALERTA* alerta) {
    HKEY hKey;

    if (RegOpenKeyEx(
        HKEY_CURRENT_USER, // chave raiz
        REG_KEY,           // subchave
        0,                 // opções
        KEY_READ,          // permissões
        &hKey              // handle
    ) != ERROR_SUCCESS)
        return FALSE;

    DWORD tipo, tamanho = sizeof(MSG_ALERTA);
    BOOL ok = (RegQueryValueEx(
        hKey,           // chave
        gNomePipe,      // nome do valor (pipe)
        NULL,           // reservado
        &tipo,          // tipo
        (LPBYTE)alerta, // destino
        &tamanho        // tamanho
    ) == ERROR_SUCCESS);

    RegCloseKey(hKey);
    return ok;
}

// Thread que trata dos alertas:
//   - espera pelo evento "notificar", waitable timer ou evento de terminação
//   - ao receber notificar: lê alerta do Registry, mostra, arranca timer
//   - ao timer disparar: mostra '---'
//   - ao receber terminação: sai
DWORD WINAPI threadAlertas(LPVOID param) {

    HANDLE hTimer = CreateWaitableTimer(
        NULL,  // segurança
        FALSE, // auto-reset
        NULL   // sem nome
    );

    if (!hTimer) {
        _tprintf(_T("[ERRO] Nao foi possivel criar o waitable timer.\n"));
        return 1;
    }

    // Ordem dos handles (importante para interpretar o retorno do Wait)
    HANDLE handles[3] = {
        gEventoNotificar, // [0] evento de novo alerta
        hTimer,           // [1] timer de alerta
        gEventoTerminar   // [2] evento de terminar
    };

    TCHAR horaBuf[64];

    while (1) {
        DWORD ret = WaitForMultipleObjects(
            3,        // nº de handles
            handles,  // array
            FALSE,    // espera por um
            INFINITE  // tempo
        );

        if (ret == WAIT_OBJECT_0) {
            // Evento "notificar" — ler alerta do Registry
            MSG_ALERTA alerta;

            if (lerAlertaRegistry(&alerta)) {
                obterHoraFormatada(horaBuf, 64);
                _tprintf(_T("%s: '%s'\n"), horaBuf, alerta.msg);

                // (Re)armar o waitable timer com a duração do alerta
                LARGE_INTEGER li;
                li.QuadPart = -(LONGLONG)alerta.duracao * 10000000LL; // unidades de 100ns

                SetWaitableTimer(
                    hTimer, // timer
                    &li,    // tempo
                    0,      // sem repetição
                    NULL,   // callback
                    NULL,   // argumento
                    FALSE   // resume
                );
            }

            // Reset manual — temos de fazer ResetEvent explicitamente
            ResetEvent(gEventoNotificar);

        } else if (ret == WAIT_OBJECT_0 + 1) {
            // Waitable timer disparou — alerta expirou
            obterHoraFormatada(horaBuf, 64);
            _tprintf(_T("%s: '---'\n"), horaBuf);

        } else {
            // Evento de terminação (ou erro) — sair
            break;
        }
    }

    CancelWaitableTimer(hTimer);
    CloseHandle(hTimer);
    return 0;
}

// Função principal
int _tmain(int argc, TCHAR* argv[]) {

    // Nome do pipe
    if (argc >= 2) {
        StringCchCopy(gNomePipe, MAX_PIPE, argv[1]);
        escreverRegistryNPIPE(gNomePipe);
    } else {
        if (!lerRegistryNPIPE(gNomePipe, MAX_PIPE)) {
            _tprintf(_T("[ERRO] Nome do named pipe nao especificado e nao existe no Registry.\n"));
            return 1;
        }
    }

    _tprintf(_T("NamedPipe = '%s'\n"), gNomePipe);

    // Evento de terminação: reset manual, não sinalizado
    gEventoTerminar = CreateEvent(
        NULL,  // segurança
        TRUE,  // reset manual
        FALSE, // estado inicial
        NULL   // sem nome
    );

    if (!gEventoTerminar) {
        _tprintf(_T("[ERRO] Nao foi possivel criar evento de terminacao.\n"));
        return 1;
    }

    // Evento "notificar": reset manual, não sinalizado, com nome público
    gEventoNotificar = CreateEvent(
        NULL,           // segurança
        TRUE,           // reset manual
        FALSE,          // estado inicial
        _T("notificar") // nome
    );

    if (!gEventoNotificar) {
        _tprintf(_T("[ERRO] Nao foi possivel abrir/criar evento 'notificar'.\n"));
        CloseHandle(gEventoTerminar);
        return 1;
    }

    // Criar thread
    HANDLE hThread = CreateThread(
        NULL,          // segurança
        0,             // stack
        threadAlertas, // função
        NULL,          // argumento
        0,             // flags
        NULL           // id
    );

    if (!hThread) {
        _tprintf(_T("[ERRO] Nao foi possivel criar thread de alertas.\n"));
        CloseHandle(gEventoNotificar);
        CloseHandle(gEventoTerminar);
        return 1;
    }

    // Loop de comandos
    TCHAR cmd[64] = { 0 };

    while (1) {
        if (_tscanf_s(_T("%63s"), cmd, (unsigned)_countof(cmd)) != 1)
            continue;

        if (_tcscmp(cmd, _T("ligar")) == 0) {
            srand((unsigned)time(NULL));
            int id = rand() % 20 + 1; // identificador entre 1 e 20
            _tprintf(_T("Identificador = %d\n"), id);

        } else if (_tcscmp(cmd, _T("desligar")) == 0) {
            SetEvent(gEventoTerminar);          // avisar thread de alertas
            WaitForSingleObject(hThread, INFINITE); // esperar que termine
            break;

        } else {
            _tprintf(_T("Comando desconhecido. Comandos disponiveis: ligar, desligar\n"));
        }
    }

    // Limpeza
    CloseHandle(hThread);
    CloseHandle(gEventoNotificar);
    CloseHandle(gEventoTerminar);

    _tprintf(_T("Placar terminado.\n"));
    return 0;
}