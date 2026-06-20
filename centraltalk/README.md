# CentralTalk

Sistema de chat **centralizado** com troca de mensagens diretas e fórum público,
usando fila de mensagens IPC System V.

- **Servidor** (`chairman`): único processo que **executa todos os comandos** e
  mantém o estado — mensagens diretas por usuário, fórum público e lista de
  logados.
- **Cliente** (`speaker`): faz login, lê/valida comandos do usuário, empacota-os
  numa requisição (carimbada com o PID) e exibe as respostas. Não executa lógica.

## Modelo de concorrência

O `chairman` é **sequencial** (single-thread): atende uma requisição por vez num
único loop de `msgrcv`. Como cada comando é processado por completo antes do
próximo, não há acesso concorrente às listas — **sem necessidade de mutex**. A
concorrência do sistema está em vários `speaker` enviando requisições à mesma
fila; a fila serializa naturalmente o atendimento.

Respostas multilinha (`msgs`, `show`, `users`) são enviadas **uma linha por
mensagem**, com a última marcada por `fim = 1`. O `speaker` lê até receber
`fim = 1`. Ver [`protocol.h`](../common/protocol.h) e [`chairman.c`](chairman.c).

## Compilar

A partir da **raiz do projeto**:

```bash
make centraltalk        # ou `make` para compilar tudo
```

Gera `bin/chairman` e `bin/speaker`.

## Executar

> Execute **a partir da raiz do projeto** (as chaves IPC usam `ftok("./.ipc_key", ...)`).

**Terminal A — servidor** (inicie primeiro):

```bash
./bin/chairman
```

**Terminais B, C, … — clientes**:

```bash
./bin/speaker
```

No `speaker`, informe um **nome de usuário** (sem espaços, até 20 caracteres). Se
o nome já estiver em uso, o login é recusado e você pode tentar outro.

## Comandos

| Comando | Efeito |
|---|---|
| `send <nome> <texto>` | envia mensagem direta ao usuário `<nome>` (se logado) |
| `msgs` | lista as mensagens recebidas |
| `post <texto>` | publica no fórum público |
| `show` | lista o fórum público |
| `del msgs` | apaga todas as suas mensagens recebidas |
| `del post <n>` | remove o post de índice `<n>` do fórum |
| `del posts` | remove todos os posts do fórum |
| `users` | lista os usuários logados |
| `myid` | mostra seu nome e PID |
| `exit` | faz logout e encerra o speaker |
| `help` | mostra a lista de comandos |

## Testes sugeridos

1. **3 usuários:** abra 3 speakers (ex.: `ana`, `bia`, `caio`). Em `users`, todos
   devem aparecer.
2. **Mensagem direta:** `ana` faz `send bia oi tudo bem?`. Em `bia`, `msgs` mostra
   `de ana: oi tudo bem?`. `del msgs` em `bia` limpa a caixa.
3. **Destino offline:** `ana` faz `send ze ...` (usuário inexistente) → erro.
4. **Fórum:** `ana` faz `post bom dia!`, `caio` faz `post oi`. Qualquer um vê com
   `show`. `del post 1` remove o primeiro; `del posts` limpa tudo.
5. **Nome duplicado:** tente logar um 2º speaker com nome `ana` → login recusado.
6. **Logout:** `exit` em `bia`; nos outros, `users` não deve mais listar `bia`.
7. **Timeout:** mate o `chairman` (`Ctrl+C`) e tente um comando → o speaker avisa
   timeout após alguns segundos.
8. **Encerramento limpo:** `Ctrl+C` no `chairman` remove a fila; confira com `ipcs`.

## Limpeza de recursos IPC

- `Ctrl+C` no `chairman` remove a fila (handler de `SIGINT`).
- Entre testes: `./scripts/limpa_ipc.sh`. Listar: `ipcs`.
