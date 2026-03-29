# Programa Central

## 📌 Descrição

O programa central é o núcleo da plataforma. Responsável por:

* Registar placares
* Atribuir identificadores únicos
* Enviar e cancelar alertas
* Encerrar o sistema de forma controlada

## 🧠 Funcionalidades

* Gestão de múltiplos placares (até 20)
* Envio de alertas para um ou vários placares
* Listagem de placares ativos
* Comunicação via Named Pipes
* Partilha de dados com monitor via memória partilhada

## 💻 Comandos disponíveis

* `alerta <msg> <duracao> <id>`
* `cancelar <id>`
* `listar`
* `encerrar`

## 📂 Estrutura

* `src/` – código fonte
* `include/` – headers

## ⚠️ Notas

* Deve existir apenas uma instância do central em execução
* Sem o central, nenhum outro programa funciona
