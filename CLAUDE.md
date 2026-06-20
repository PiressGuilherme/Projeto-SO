# Planejamento de Desenvolvimento — Trabalho Prático de Sistemas Operacionais

> **Como usar este arquivo:** salve-o como `CLAUDE.md` na raiz do projeto. O Claude Code vai
> lê-lo automaticamente no início de cada sessão e usá-lo como contexto persistente. As seções
> "Contexto", "Regras de implementação" e "Estado atual" são as mais importantes para manter.

---

## 1. Contexto do projeto

Trabalho prático da disciplina de Sistemas Operacionais (UFCSPA, Informática Biomédica,
Prof. João Gluz). Objetivo: desenvolver três sistemas concorrentes em **C** usando os
mecanismos de **IPC System V** (filas de mensagens, memória compartilhada, semáforos) e
**pthreads**, conforme visto em aula.

Os três sistemas a entregar:

1. **DynaThreadMaker** — cliente/servidor; servidor cria threads dinamicamente sob demanda
   via fila de mensagens; sincronização de cópia de argumentos por mutex.
2. **CentralTalk** — chat centralizado; clientes (`speaker`) + servidor (`chairman`); toda
   lógica de comando executada no servidor.
3. **PeerTalk** — chat descentralizado peer-to-peer; sem servidor central; lista de usuários
   em memória compartilhada protegida por mutex; comandos executados em cada peer.

**Entrega:** arquivo ZIP com todos os fontes + relatório PDF (formato artigo SBC, mínimo 10
páginas) demonstrando o desenvolvimento de cada projeto.

## 2. Ambiente e fluxo de trabalho

- **Desenvolvimento:** PC desktop Windows + VSCode com extensão **Remote-SSH** conectando na
  VM Ubuntu. O código vive na VM; compilação e execução são sempre dentro do Linux.
- **VM:** VirtualBox + **Ubuntu 24.04 LTS (Noble Numbat)**. (System V IPC e pthreads são
  estáveis; qualquer LTS ≥ 20.04 serve, 24.04 é o equilíbrio entre moderno e estável.)
- **Apresentação:** notebook com a mesma VM Ubuntu. O código é trazido via **Git** (`git pull`),
  recompilado com `make` e executado. As duas VMs só precisam do toolchain de build.
- **Versionamento:** Git desde o primeiro commit. Repositório no GitHub (privado).

### Setup inicial da VM (rodar uma vez em cada máquina)

```bash
sudo apt update
sudo apt install -y build-essential git openssh-server gdb valgrind
sudo systemctl enable --now ssh
ip addr            # anotar o IP da VM (para o Remote-SSH no Windows)
```

No VirtualBox, configurar adaptador **Host-Only** (ou Port Forwarding no NAT) para o Windows
enxergar a VM. No Windows: instalar VSCode + extensão "Remote - SSH" e conectar em
`usuario@IP-da-VM`.

## 3. Estrutura de diretórios

```
trabalho-so/
├── CLAUDE.md                  # este arquivo
├── README.md                  # instruções de build/execução para a banca
├── Makefile                   # build de todos os sistemas (alvos individuais + 'all')
├── common/                    # código compartilhado entre os três sistemas
│   ├── ipc_utils.c/.h         # wrappers de msgget/shmget/semget + tratamento de erro
│   └── protocol.h             # structs de mensagem, msgtyp, constantes (chaves IPC, limites)
├── dynathreadmaker/
│   ├── servidor.c
│   ├── cliente.c
│   └── README.md
├── centraltalk/
│   ├── chairman.c             # servidor
│   ├── speaker.c              # cliente
│   └── README.md
├── peertalk/
│   ├── peertalker.c
│   └── README.md
├── docs/
│   └── relatorio/             # fontes do relatório SBC (LaTeX) + figuras
└── scripts/
    └── limpa_ipc.sh           # ipcrm de filas/shm/sem órfãos entre testes
```

## 4. Regras de implementação (IMPORTANTES — seguir sempre)

- **Linguagem:** C puro (C11). Compilar com `gcc -Wall -Wextra -std=c11 -pthread`.
  Tratar todo warning como bug a resolver.
- **IPC System V** (não POSIX, salvo se a aula tiver usado POSIX — confirmar): `msgget`,
  `msgsnd`, `msgrcv`, `shmget`, `shmat`, `semget`, `semop`, `semctl`.
- **Chaves IPC:** gerar com `ftok()` a partir de um caminho fixo do projeto + id de projeto,
  centralizadas em `common/protocol.h`. Documentar.
- **msgtyp (convenção comum aos sistemas):** tipo `1` = mensagens para o servidor; tipo = `PID`
  do cliente = mensagens destinadas àquele cliente. No PeerTalk, msgtyp = PID do peer destino,
  e o primeiro campo de dados carrega o PID do peer originador.
- **Tratamento de erro:** TODA chamada de syscall IPC verifica retorno; em erro, `perror()` +
  saída limpa. Nunca ignorar `-1`.
- **Limpeza de recursos IPC:** o sistema que cria a fila/shm/sem é responsável por removê-los
  (`msgctl(IPC_RMID)`, `shmctl(IPC_RMID)`, `semctl(IPC_RMID)`) no encerramento e em handler de
  `SIGINT`. Recursos IPC System V sobrevivem ao processo — vazamento é erro comum. Usar
  `scripts/limpa_ipc.sh` + `ipcs` durante os testes.
- **Mutex/semáforo binário:** no DynaThreadMaker, a thread principal só libera para tratar a
  próxima requisição depois que a thread secundária sinalizar (via mutex/sem) que copiou os
  argumentos para variáveis locais. No PeerTalk, todo acesso à lista de usuários em shm é
  seção crítica protegida por semáforo.
- **Timeout no cliente:** clientes que esperam resposta do servidor devem ter timeout
  (`msgrcv` com `IPC_NOWAIT` em loop com `sleep`, ou `alarm()` + handler). Avisar o usuário no
  timeout e voltar ao loop de comandos.
- **Comentários:** em português, explicando o "porquê" das decisões de concorrência
  (servirão de base para o relatório).

## 5. Fases de desenvolvimento

Desenvolver e testar **na ordem abaixo** — cada fase só começa com a anterior funcionando.

### Fase 0 — Esqueleto e ambiente
- [ ] Criar estrutura de diretórios + `git init` + repositório remoto.
- [ ] `Makefile` com alvos `dynathreadmaker`, `centraltalk`, `peertalk`, `all`, `clean`.
- [ ] `common/ipc_utils` + `common/protocol.h` com wrappers e structs.
- [ ] `scripts/limpa_ipc.sh`. Validar com um "hello fila de mensagens" descartável.

### Fase 1 — DynaThreadMaker (o mais simples conceitualmente, melhor para validar o ambiente)
- [ ] Servidor: cria/abre fila, loop de `msgrcv`, cria thread por requisição, sincroniza cópia
      de args por mutex, responde ao cliente, trata mensagem de encerramento.
- [ ] Thread secundária: copia args → sinaliza mutex → imprime nome+msg inicial → `sleep(T)` →
      aloca vetor de N ints, inicializa 1..N, imprime somatório → imprime msg final → encerra.
- [ ] Cliente: menu (criar thread / encerrar servidor), coleta campos, envia, espera resposta
      com timeout.
- [ ] Testar com múltiplos clientes simultâneos e threads concorrentes.

### Fase 2 — CentralTalk
- [ ] `chairman`: login (nomes únicos ≤20 chars), tabela de mensagens por usuário, fórum
      público, lista de logados. Executa todos os comandos.
- [ ] Comandos: `send`, `msgs`, `post`, `show`, `del msgs`, `del post <n>`, `del posts`,
      `users`, `myid`, `exit`.
- [ ] `speaker`: só lê/valida comandos, empacota em requisição (com PID), envia, exibe resposta.
- [ ] Testar 3+ usuários trocando mensagens diretas e no fórum.

### Fase 3 — PeerTalk
- [ ] Lista de usuários logados em memória compartilhada, protegida por mutex.
- [ ] Login: lock → checa nome duplicado → cadastra (nome+PID) → unlock.
- [ ] Fila geral; mensagem com msgtyp = PID destino, 1º campo = PID origem.
- [ ] Cada `peertalker` executa os comandos: `send`, `recv`, `msgs`, `del msgs`, `users`,
      `myid`, `exit` (com descadastro da lista).
- [ ] Testar concorrência de login/logout simultâneos (foco no mutex da lista).

### Fase 4 — Relatório e empacotamento
- [ ] Relatório no formato SBC (LaTeX, template oficial), mín. 10 páginas: arquitetura de cada
      sistema, decisões de IPC/sincronização, diagramas, trechos de código comentados, testes.
- [ ] `README.md` final com passo a passo de build/execução para a banca.
- [ ] `make clean`, gerar ZIP com fontes + PDF.

## 6. Comandos úteis

```bash
make all                       # compila tudo
make clean                     # remove binários
ipcs                           # lista filas/shm/sem ativos
./scripts/limpa_ipc.sh         # remove recursos IPC órfãos
valgrind --leak-check=full ./bin/servidor   # checar vazamento de memória
gdb ./bin/chairman             # depurar
```

## 7. Estado atual

> Atualizar esta seção ao fim de cada sessão de trabalho para dar continuidade.

- **Fase atual:** Fase 1 concluída (DynaThreadMaker). Próxima: Fase 2 (CentralTalk).
- **Feito:**
  - Fase 0 — infra: `.gitignore`, `.ipc_key` (âncora ftok), `common/protocol.h`,
    `common/ipc_utils.{h,c}` (wrappers de fila com tratamento de erro), `Makefile`
    (`-Wall -Wextra -std=c11 -pthread`, alvos `all`/`dynathreadmaker`/`clean`),
    `scripts/limpa_ipc.sh`, README raiz.
  - Fase 1 — DynaThreadMaker: `dynathreadmaker/servidor.c` e `cliente.c` + README.
    Servidor com loop de `msgrcv`, criação de thread por requisição, cópia de
    argumentos protegida por `pthread_mutex_t` + `pthread_cond_t` (a principal só
    prossegue após a secundária sinalizar a cópia), handler de SIGINT removendo a
    fila, e mensagem de encerramento. Cliente com menu, montagem da requisição
    (carimbada com o PID) e espera de resposta com timeout (IPC_NOWAIT em laço).
- **Decisão tomada:** IPC **System V** (confirmado com o usuário), conforme o
  vocabulário do enunciado (msgtyp tipo 1 / PID).
- **Próximo passo:** Fase 2 — CentralTalk (`chairman` + `speaker`). Reusar
  `common/protocol.h` (PROJ_ID_CENTRALTALK já reservado) e `ipc_utils`.
- **Pendências/dúvidas:** validar a compilação e os testes do DynaThreadMaker na
  VM Linux (o desenvolvimento é feito no Windows; build/execução só no Linux).
- **Convenção de binários:** `bin/dyn_servidor`, `bin/dyn_cliente`. Executar
  sempre a partir da raiz do projeto (ftok usa caminho relativo `./.ipc_key`).
