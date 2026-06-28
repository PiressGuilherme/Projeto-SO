# Projeto SO — Trabalho Prático de Sistemas Operacionais (UFCSPA)

Três sistemas concorrentes em **C** usando **IPC System V** (filas de mensagens,
memória compartilhada, semáforos) e **pthreads**. Desenvolvido em VM Linux (Ubuntu 24.04).

| Sistema | Descrição | Estado |
|---|---|---|
| **DynaThreadMaker** | Cliente/servidor; servidor cria threads sob demanda via fila de mensagens; cópia de argumentos sincronizada por mutex. 
| **CentralTalk** | Chat centralizado (`chairman` + `speaker`); lógica de comandos no servidor.
| **PeerTalk** | Chat peer-to-peer; lista de usuários em memória compartilhada protegida por mutex.

## Requisitos (na VM Linux)

```bash
sudo apt update
sudo apt install -y build-essential git gdb valgrind
```

## Build

A partir da **raiz do projeto**:

```bash
make            # compila os três sistemas + exemplos extras
make clean      # remove binários (bin/) e objetos
```

Alvos individuais também disponíveis: `make dynathreadmaker`, `make centraltalk`,
`make peertalk`, `make extras`. Os binários são gerados em `bin/`.

> **Importante:** os programas geram as chaves IPC com `ftok("./.ipc_key", ...)`,
> um caminho **relativo**. Por isso, execute os binários **a partir da raiz do
> projeto** (onde existe o arquivo `.ipc_key`), para que servidor e clientes
> compartilhem a mesma chave.

## Executar

Cada sistema tem instruções próprias:

- DynaThreadMaker → [`dynathreadmaker/README.md`](dynathreadmaker/README.md)
- CentralTalk → [`centraltalk/README.md`](centraltalk/README.md)
- PeerTalk → [`peertalk/README.md`](peertalk/README.md)

Resumo do DynaThreadMaker:

```bash
# Terminal A (servidor — iniciar primeiro):
./bin/dyn_servidor
# Terminal B, C, ... (clientes):
./bin/dyn_cliente
```

Resumo do CentralTalk:

```bash
# Terminal A (servidor — iniciar primeiro):
./bin/chairman
# Terminal B, C, ... (clientes):
./bin/speaker
```

Resumo do PeerTalk (sem servidor — crie a infra primeiro):

```bash
./bin/peertalk_init          # cria shm + semáforo + fila (uma vez)
./bin/peertalker             # um por usuário, em terminais diferentes
./bin/peertalk_init clean    # remove os recursos IPC ao final
```

Demonstrações de concorrência (opcionais, descritas nos READMEs de cada pasta):

```bash
./bin/peertalk_stress 20            # PeerTalk: logins simultâneos com mutex
./bin/peertalk_stress 20 --no-lock  # mesmo teste sem o mutex (mostra a corrida)
./bin/semaforo_contador             # extras: produtor/consumidor (semáforo contador)
```

## Estrutura do repositório

```
.
├── Makefile                 # build de todos os sistemas
├── .ipc_key                 # arquivo-âncora para ftok() (NÃO remover)
├── common/
│   ├── protocol.h           # chaves IPC, msgtyp, structs de mensagem, limites
│   ├── ipc_utils.h/.c       # wrappers de IPC com tratamento de erro
├── dynathreadmaker/
│   ├── servidor.c
│   ├── cliente.c
│   └── README.md
├── centraltalk/
│   ├── chairman.c           # servidor
│   ├── speaker.c            # cliente
│   └── README.md
├── peertalk/
│   ├── peertalk_init.c      # cria/remove os recursos IPC (shm+sem+fila)
│   ├── peertalker.c         # processo peer (login, comandos, p2p)
│   ├── peertalk_stress.c    # demonstração do mutex da lista (login concorrente)
│   └── README.md
├── extras/
│   ├── semaforo_contador.c  # exemplo didático: semáforo contador vs. mutex
│   └── README.md
├── scripts/
│   └── limpa_ipc.sh         # remove recursos IPC órfãos entre testes
└── bin/                     # binários gerados (git-ignored)
```

## Limpeza de recursos IPC

Recursos IPC System V sobrevivem ao processo. Para remover órfãos entre testes:

```bash
./scripts/limpa_ipc.sh      # remove filas/shm/sem do usuário atual
ipcs                        # lista recursos IPC ativos
```
