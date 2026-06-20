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

- **Fase atual:** Fases 1, 2 e 3 (código) concluídas. DynaThreadMaker e CentralTalk
  testados e OK na VM. PeerTalk aguardando teste na VM. Próxima: Fase 4 (relatório).
- **Feito:**
  - Fase 0 — infra: `.gitignore`, `.ipc_key` (âncora ftok), `common/protocol.h`,
    `common/ipc_utils.{h,c}` (wrappers de fila/shm/sem com tratamento de erro),
    `Makefile` (`-Wall -Wextra -std=c11 -pthread`, alvos `all`/`dynathreadmaker`/
    `centraltalk`/`peertalk`/`clean`), `scripts/limpa_ipc.sh`, README raiz.
  - Fase 1 — DynaThreadMaker (`servidor.c` + `cliente.c`): **testado OK na VM**.
    Cópia de argumentos protegida por `pthread_mutex_t` + `pthread_cond_t`, SIGINT
    remove a fila, cliente com timeout. Correção: incluir `<sys/ipc.h>`/`<sys/msg.h>`.
  - Fase 2 — CentralTalk (`chairman.c` + `speaker.c`): **testado OK na VM**. Chairman
    sequencial single-thread (arrays estáticos), todos os comandos + login único,
    respostas multilinha via flag `fim`. Correção: buffers `MAX_LINHA` para evitar
    `-Wformat-truncation` ao compor "prefixo + texto".
  - Fase 3 — PeerTalk (`peertalk_init.c` + `peertalker.c`): lista de usuários em
    **memória compartilhada (shmget)** protegida por **semáforo binário (semget,
    SEM_UNDO)** — toda operação na lista entre `sem_lock`/`sem_unlock`. `peertalk_init`
    cria/zera/remove os recursos (`clean`); peers só anexam (não removem ao sair, só
    descadastram). Fila p2p: mtype = PID destino, `pid_origem` no corpo. `recv` drena
    a fila por PID com IPC_NOWAIT para tabela local; `msgs`/`del msgs` operam local.
    `ipc_utils` ganhou helpers de shm/sem + `union semun`.
  - Extra — `peertalk/peertalk_stress.c` (`bin/peertalk_stress`): demonstra o mutex.
    Fork de N (padrão 20) processos que largam juntos numa barreira (shm
    IPC_PRIVATE) e tentam logar com nomes repetidos; loga entrada/saída da seção
    crítica com timestamp ms e confere duplicados. Flag `--no-lock` pula o semáforo
    para evidenciar a corrida. Não altera o `peertalker`. Evidência p/ o relatório.
    Correção pós-teste: o busy-wait da barreira (`while(!largada){}`) travava a VM
    com N processos a 100% de CPU — trocado por `sched_yield()`.
  - Extra — `extras/semaforo_contador.c` (`bin/semaforo_contador`, alvo `extras`):
    exemplo didático de **semáforo CONTADOR** (produtor/consumidor com 3 semáforos:
    VAZIOS/CHEIOS contadores + MUTEX binário). Mostra a diferença mutex vs. semáforo
    pedida pelo usuário. Autocontido (IPC_PRIVATE, cria e remove os próprios
    recursos), não usa `ipc_utils`. Os 3 exercícios usam semáforo no papel de mutex;
    este torna explícito o papel de contador/sincronização (e conecta com o
    mutex+cond do DynaThreadMaker).
- **Decisões tomadas:** IPC **System V**. CentralTalk: chairman sequencial + arrays
  estáticos. PeerTalk: binário separado `peertalk_init` (peers livres da infra);
  limpeza manual (`peertalk_init clean`/`limpa_ipc.sh`); `recv` não-bloqueante.
- **Próximo passo:** (1) testar PeerTalk na VM; (2) Fase 4 — relatório SBC (LaTeX,
  ≥10 págs) + empacotar ZIP com fontes + PDF. `make clean` antes de zipar.
- **Pendências/dúvidas:** validar compilação/testes do PeerTalk na VM (foco no mutex
  da lista em logins/logouts simultâneos).
- **Convenção de binários:** `bin/dyn_servidor`, `bin/dyn_cliente`, `bin/chairman`,
  `bin/speaker`, `bin/peertalk_init`, `bin/peertalker`, `bin/peertalk_stress`.
  Executar sempre a partir da raiz do projeto (ftok usa `./.ipc_key`).
