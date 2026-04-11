/*
 * teste_placar.c
 * ---------------------------------------------------------------
 * Programa de teste para a 1ª entrega (M1) do programa "placar".
 *
 * O que este teste faz:
 *   1. Verifica se o Registry tem o nome do pipe (NPIPE).
 *   2. Escreve um alerta (MSG_ALERTA) no Registry com o nome do pipe.
 *   3. Sinaliza o evento "notificar" para que o placar o processe.
 *   4. Aguarda um pouco e repete com um segundo alerta.
 *   5. Permite ao utilizador verificar manualmente a saída do placar.
 *
 * Como usar:
 *   1. Compila o placar e lança-o numa consola separada:
 *         placar.exe tubo
 *      (ou qualquer outro nome de pipe)
 *   2. Na consola do placar, escreve "ligar" e verifica o ID.
 *   3. Compila e lança este teste:
 *         teste_placar.exe
 *   4. Observa a consola do placar - deve aparecer o alerta e depois '---'.
 *
 * Compilar (Developer Command Prompt do Visual Studio):
 *   cl teste_placar.c /W4 /Fe:teste_placar.exe
 * ---------------------------------------------------------------
 */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <strsafe.h>
#include <stdio.h>

/* Mesmas definições que o placar */
#define REG_KEY   _T("Software\\TrabSO2")
#define REG_VALUE _T("NPIPE")

typedef struct {
    BYTE  tipo;      /* 4 = novo alerta */
    TCHAR msg[140];
    DWORD duracao;
} MSG_ALERTA;

/* ------------------------------------------------------------------ */
/* Lê o nome do pipe do Registry (igual ao placar)                     */
/* ------------------------------------------------------------------ */
static BOOL lerNomePipe(TCHAR *buf, DWORD bufSize) {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        _tprintf(_T("[ERRO] Chave '%s' nao encontrada no Registry.\n"), REG_KEY);
        _tprintf(_T("       Lance primeiro o placar com:  placar.exe <nome_pipe>\n"));
        return FALSE;
    }
    DWORD tipo, tamanho = bufSize * sizeof(TCHAR);
    BOOL ok = (RegQueryValueEx(hKey, REG_VALUE, NULL, &tipo,
                               (LPBYTE)buf, &tamanho) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    if (!ok)
        _tprintf(_T("[ERRO] Valor NPIPE nao encontrado no Registry.\n"));
    return ok;
}

/* ------------------------------------------------------------------ */
/* Escreve um MSG_ALERTA no Registry com a chave = nomePipe            */
/* ------------------------------------------------------------------ */
static BOOL escreverAlertaRegistry(const TCHAR *nomePipe, const MSG_ALERTA *alerta) {
    HKEY hKey;
    DWORD disp;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE,
                       NULL, &hKey, &disp) != ERROR_SUCCESS) {
        _tprintf(_T("[ERRO] Nao foi possivel abrir/criar a chave do Registry.\n"));
        return FALSE;
    }
    BOOL ok = (RegSetValueEx(hKey, nomePipe, 0, REG_BINARY,
                             (const BYTE *)alerta,
                             sizeof(MSG_ALERTA)) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Envia um alerta ao placar                                           */
/* ------------------------------------------------------------------ */
static void enviarAlerta(const TCHAR *nomePipe,
                         const TCHAR *msg, DWORD duracao,
                         int numero) {
    MSG_ALERTA alerta;
    ZeroMemory(&alerta, sizeof(alerta));
    alerta.tipo    = 4; /* novo alerta */
    alerta.duracao = duracao;
    StringCchCopy(alerta.msg, 140, msg);

    _tprintf(_T("\n[TESTE %d] A enviar alerta:\n"), numero);
    _tprintf(_T("          Mensagem : '%s'\n"), alerta.msg);
    _tprintf(_T("          Duracao  : %u segundos\n"), alerta.duracao);

    /* 1. Escreve no Registry */
    if (!escreverAlertaRegistry(nomePipe, &alerta)) {
        _tprintf(_T("[ERRO] Falha ao escrever alerta no Registry.\n"));
        return;
    }
    _tprintf(_T("          Registry : OK\n"));

    /* 2. Abre o evento "notificar" (deve existir - criado pelo placar) */
    HANDLE hEvento = OpenEvent(EVENT_MODIFY_STATE, FALSE, _T("notificar"));
    if (!hEvento) {
        _tprintf(_T("[ERRO] Evento 'notificar' nao encontrado.\n"));
        _tprintf(_T("       Certifica-te que o placar esta a correr.\n"));
        return;
    }

    /* 3. Sinaliza o evento */
    if (!SetEvent(hEvento)) {
        _tprintf(_T("[ERRO] Falha ao sinalizar o evento 'notificar'.\n"));
    } else {
        _tprintf(_T("          Evento   : sinalizado\n"));
        _tprintf(_T("          -> Verifica a consola do placar!\n"));
    }
    CloseHandle(hEvento);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int _tmain(void) {
    _tprintf(_T("==============================================\n"));
    _tprintf(_T("  Teste M1 - Placar Informativo (SO2 25/26)  \n"));
    _tprintf(_T("==============================================\n\n"));

    /* Passo 1: ler nome do pipe do Registry */
    TCHAR nomePipe[256] = {0};
    if (!lerNomePipe(nomePipe, 256))
        return 1;
    _tprintf(_T("[INFO] Nome do pipe lido do Registry: '%s'\n"), nomePipe);

    /* Passo 2: verificar que o evento "notificar" existe */
    {
        HANDLE hTmp = OpenEvent(EVENT_MODIFY_STATE, FALSE, _T("notificar"));
        if (!hTmp) {
            _tprintf(_T("\n[ERRO] O evento 'notificar' nao existe.\n"));
            _tprintf(_T("       O placar esta a correr? Certifica-te que lancaste:\n"));
            _tprintf(_T("           placar.exe %s\n"), nomePipe);
            return 1;
        }
        CloseHandle(hTmp);
        _tprintf(_T("[INFO] Evento 'notificar' encontrado: OK\n"));
    }

    /* ============================================================
       TESTE 1 - Alerta simples com duracao curta (5 segundos)
       Esperado no placar:
         DD/MM/YYYY (HH:MM:SS): 'Teste 1 - Acidente na A1'
         (5 segundos depois...)
         DD/MM/YYYY (HH:MM:SS): '---'
       ============================================================ */
    enviarAlerta(nomePipe, _T("Teste 1 - Acidente na A1"), 5, 1);

    _tprintf(_T("\n[AGUARDA] A esperar 3 segundos antes do proximo teste...\n"));
    Sleep(3000);

    /* ============================================================
       TESTE 2 - Alerta que substitui o anterior (chega antes de '---')
       O alerta anterior (5s) ainda nao terminou.
       Esperado no placar:
         DD/MM/YYYY (HH:MM:SS): 'Teste 2 - Substituicao de alerta'
         (8 segundos depois...)
         DD/MM/YYYY (HH:MM:SS): '---'
       O timer anterior deve ser cancelado e substituido pelo novo.
       ============================================================ */
    enviarAlerta(nomePipe, _T("Teste 2 - Substituicao de alerta"), 8, 2);

    _tprintf(_T("\n[AGUARDA] A esperar 10 segundos para o alerta expirar...\n"));
    Sleep(10000);

    /* ============================================================
       TESTE 3 - Alerta com mensagem maxima (139 chars + '\0')
       ============================================================ */
    TCHAR msgLonga[140];
    /* Preenche com 139 'A' */
    for (int i = 0; i < 139; i++) msgLonga[i] = _T('A');
    msgLonga[139] = _T('\0');
    enviarAlerta(nomePipe, msgLonga, 4, 3);

    _tprintf(_T("\n[AGUARDA] A esperar 5 segundos para o alerta expirar...\n"));
    Sleep(5000);

    _tprintf(_T("\n==============================================\n"));
    _tprintf(_T("  Testes concluidos.                         \n"));
    _tprintf(_T("  Verifica a consola do placar e confirma:   \n"));
    _tprintf(_T("  [OK] Teste 1: alerta apareceu              \n"));
    _tprintf(_T("  [OK] Teste 1: '---' nao apareceu (foi      \n"));
    _tprintf(_T("       substituido pelo Teste 2)             \n"));
    _tprintf(_T("  [OK] Teste 2: alerta apareceu              \n"));
    _tprintf(_T("  [OK] Teste 2: '---' apareceu ~8s depois    \n"));
    _tprintf(_T("  [OK] Teste 3: mensagem longa apareceu      \n"));
    _tprintf(_T("  [OK] Teste 3: '---' apareceu ~4s depois    \n"));
    _tprintf(_T("==============================================\n"));

    return 0;
}