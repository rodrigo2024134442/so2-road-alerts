# Placar Informativo

Projeto desenvolvido no âmbito da unidade curricular de Sistemas Operativos 2 (2025/26).

## 📌 Descrição

Este projeto consiste na implementação de uma plataforma de divulgação de alertas para vias rodoviárias, baseada em comunicação entre processos.

A plataforma é composta por três programas principais:

* **Central** – responsável pela gestão do sistema
* **Placar** – representa os painéis informativos
* **Monitor** – interface gráfica para visualização dos alertas

## ⚙️ Tecnologias utilizadas

* Linguagem C/C++
* Windows API
* Named Pipes
* Memória Partilhada
* Threads

## 📂 Estrutura do projeto

* `central.c` – programa central
* `placar.c` – programa dos placares
* `monitor.c` – programa de monitorização

## 🚀 Execução

Cada programa deve ser executado em consola independente.

Ordem típica:

1. Executar o **central**
2. Executar um ou mais **placares**
3. Executar o **monitor** (opcional)

## 🧪 Compilacao rapida (Windows + MinGW)

```bash
gcc -municode -DUNICODE -D_UNICODE -o central.exe central.c
gcc -municode -DUNICODE -D_UNICODE -o placar.exe placar.c
gcc -municode -DUNICODE -D_UNICODE -o monitor.exe monitor.c -lcomctl32 -lgdi32
```

## ✅ Entrega M3 (31 maio)

- Monitor Win32 com menu "Ficheiro" (Configuracao/Acerca/Sair)
- Lista de alertas ativos (ID, mensagem, duracao)
- Paginacao com Page Up / Page Down
- Memoria partilhada (SHM) e sincronizacao com mutex + evento
- Atualizacao imediata sem polling

Defesa individual presencial (oral): Não

## 👥 Autores

* Nome 1 - Joao Carlos Lopes Marques (2024132275)
* Nome 2 - Rodrigo Beja da Costa (2024134442)
