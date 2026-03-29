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

* `central/` – programa central
* `placar/` – programa dos placares
* `monitor/` – programa de monitorização
* `common/` – estruturas e definições partilhadas
* `docs/` – documentação e apontamentos

## 🚀 Execução

Cada programa deve ser executado em consola independente.

Ordem típica:

1. Executar o **central**
2. Executar um ou mais **placares**
3. Executar o **monitor** (opcional)

## 👥 Autores

* Nome 1 - João Carlos Lopes Marques
* Nome 2 - Rodrigo Beja da Costa
