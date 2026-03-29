# Programa Placar

## 📌 Descrição

O programa placar simula um painel informativo de estrada.

Cada instância representa um placar independente.

## 🧠 Funcionalidades

* Ligação ao programa central
* Receção de identificador único
* Receção e apresentação de alertas
* Temporização da duração dos alertas
* Desligar do sistema

## 💻 Comandos disponíveis

* `ligar`
* `desligar`

## ⚙️ Funcionamento

* Utiliza duas threads:

  * Interface com o utilizador
  * Receção de alertas

## 📂 Estrutura

* `src/` – código fonte
* `include/` – headers

## ⚠️ Notas

* Comunicação com o central via Named Pipes
* Deve mostrar:

  * Identificador
  * Hora do sistema
  * Mensagens recebidas
