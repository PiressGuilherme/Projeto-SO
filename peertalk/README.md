# PeerTalk

Sistema de chat **descentralizado (peer-to-peer)**, sem servidor central. Cada
processo `peertalker` executa os próprios comandos, operando diretamente sobre:

- uma **lista de usuários logados em memória compartilhada** (`shmget`), que é
  uma **seção crítica** protegida por um **semáforo binário** (`semget`, usado
  como mutex);
- uma **fila de mensagens geral** (`msgget`), por onde os peers trocam mensagens
  diretas.

## Por que um binário de inicialização separado

Como não há servidor, nenhum `peertalker` deve ser dono da infraestrutura. Um
utilitário separado, **`peertalk_init`**, cria e inicializa os três recursos
(shm zerada, semáforo = 1, fila). Os peers apenas **anexam** os recursos já
existentes — mantendo a filosofia descentralizada.

## Concorrência: a lista é uma seção crítica

Logins e logouts simultâneos alteram a lista compartilhada. Sem proteção, dois
peers logando ao mesmo tempo poderiam corromper a lista ou cadastrar nomes
duplicados. Por isso **todo** acesso à lista (consultar/cadastrar/descadastrar)
ocorre entre `sem_lock()` e `sem_unlock()`:

```
sem_lock()                    // P: entra na seção crítica (bloqueia se ocupada)
  ... lê/altera a lista ...
sem_unlock()                  // V: libera a seção crítica
```

O semáforo usa `SEM_UNDO`, então se um peer morrer segurando o lock, o kernel o
libera automaticamente (evita travar a lista para sempre). Ver
[`ipc_utils.c`](../common/ipc_utils.c) e [`peertalker.c`](peertalker.c).

## Roteamento das mensagens (fila geral)

Conforme o enunciado:

- `mtype` = **PID do peer destino** (a fila entrega a cada peer só o que é dele);
- 1º campo de dados = **PID do peer origem** (+ nome, por conveniência);
- depois, o texto.

O comando `recv` retira da fila, de forma **não-bloqueante** (`IPC_NOWAIT`),
todas as mensagens com `mtype` igual ao próprio PID e as guarda numa **tabela
local**; `msgs` mostra a tabela local; `del msgs` limpa apenas a cópia local
(mensagens ainda na fila não são afetadas).

## Compilar

A partir da **raiz do projeto**:

```bash
make peertalk           # ou `make` para compilar tudo
```

Gera `bin/peertalk_init` e `bin/peertalker`.

## Executar

> Execute **a partir da raiz do projeto** (chaves IPC via `ftok("./.ipc_key", ...)`).

**1) Crie a infraestrutura (uma vez):**

```bash
./bin/peertalk_init
```

**2) Abra um peertalker por usuário (terminais diferentes):**

```bash
./bin/peertalker        # informe um nome único ao logar
```

**3) Ao final da sessão, remova os recursos IPC:**

```bash
./bin/peertalk_init clean      # ou ./scripts/limpa_ipc.sh
```

## Comandos

| Comando | Efeito |
|---|---|
| `send <nome> <texto>` | envia mensagem direta ao peer `<nome>` (se logado) |
| `recv` | busca na fila todas as mensagens destinadas a você e guarda localmente |
| `msgs` | mostra suas mensagens locais (recebidas via `recv`) |
| `del msgs` | apaga sua cópia local de mensagens |
| `users` | lista os usuários logados (lê a lista compartilhada) |
| `myid` | mostra seu nome e PID |
| `exit` | faz logout (descadastra da lista) e encerra |
| `help` | mostra a lista de comandos |

## Testes sugeridos

1. **Init + 3 peers:** `peertalk_init`, depois 3 `peertalker` (`ana`, `bia`,
   `caio`). Em qualquer um, `users` deve listar os três.
2. **Mensagem p2p:** `ana` faz `send bia oi!`. Em `bia`, `recv` busca a mensagem
   (`de ana (PID ...): oi!`) e `msgs` a exibe. `del msgs` em `bia` limpa.
3. **Destino offline:** `send ze ...` (inexistente) → erro.
4. **recv sem mensagens:** `recv` sem nada destinado a você → "nenhuma mensagem".
5. **Nome duplicado:** tente logar dois peers com o nome `ana` → o segundo é
   recusado (valida a seção crítica do login).
6. **Concorrência de login/logout:** abra vários peers quase ao mesmo tempo (ex.:
   em scripts) e faça logins/`exit` simultâneos; `users` deve permanecer
   consistente — foco no mutex da lista.
7. **Logout:** `exit` em `bia`; nos outros, `users` não deve mais listar `bia`.
8. **SIGINT:** `Ctrl+C` num peer também faz logout (descadastra) antes de sair.

## Limpeza de recursos IPC

Os peers **não** removem os recursos ao sair (só se descadastram da lista). Para
remover shm + sem + fila ao final:

```bash
./bin/peertalk_init clean      # remove os três recursos do PeerTalk
./scripts/limpa_ipc.sh         # alternativa: remove TODOS os IPC do usuário
ipcs                           # conferir o que está ativo
```
