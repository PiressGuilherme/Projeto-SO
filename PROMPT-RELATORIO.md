# Prompt para o Claude web gerar o relatório (PDF)

> **Como usar:** anexe o ZIP do projeto na conversa do claude.ai e cole o texto
> abaixo (a partir de "INÍCIO DO PROMPT"). O Claude web vai ler os fontes do ZIP
> e produzir o relatório como um *artifact* que você pode exportar em PDF.

---

## INÍCIO DO PROMPT

Você vai escrever o **relatório final** de um trabalho prático de Sistemas
Operacionais. Anexei um ZIP com todo o código-fonte do projeto. **Leia os fontes
do ZIP** (arquivos `.c` e `.h`, os `README.md` de cada pasta e o `Makefile`) e
use-os como base — o relatório deve refletir fielmente o que o código faz.

### Formato de saída

- Produza o relatório como um **artifact** em formato adequado para exportar em
  **PDF** (pode ser um documento de texto/markdown bem formatado, ou HTML
  estilizado — o que gerar o melhor PDF).
- **Idioma:** português do Brasil.
- **Tamanho:** mínimo de **10 páginas**. Seja detalhado e didático, mas **não
  excessivamente técnico** — o leitor é um professor de graduação avaliando se os
  alunos entenderam concorrência e IPC. Explique o "porquê" das decisões, não só
  o "como".
- **Estilo:** artigo acadêmico, fortemente inspirado no formato SBC (mas não
  precisa ser idêntico ao template oficial). Inclua: título, nomes dos autores
  (deixe campos para preencher), resumo (em português) e *abstract* (em inglês),
  seções numeradas, figuras com legenda e referências.
- **Diagramas:** crie diagramas de arquitetura para cada sistema. Pode usar
  diagramas em ASCII art dentro do texto, ou descrições visuais claras — o que
  ficar bom no PDF. Mostre os processos, a fila de mensagens, a memória
  compartilhada, o semáforo e o sentido das setas (quem manda o quê).
- **Trechos de código:** inclua trechos curtos e comentados dos arquivos do ZIP
  para ilustrar os pontos de concorrência (não cole arquivos inteiros; escolha as
  partes que mostram a sincronização).

### Sobre o projeto

O trabalho consiste em **três sistemas concorrentes escritos em C**, usando os
mecanismos de **IPC System V** (filas de mensagens, memória compartilhada,
semáforos) e **pthreads**, rodando em Linux (Ubuntu). Os três sistemas são:

1. **DynaThreadMaker** — cliente/servidor para criação dinâmica de threads.
2. **CentralTalk** — chat centralizado (servidor `chairman` + clientes `speaker`).
3. **PeerTalk** — chat descentralizado peer-to-peer (sem servidor central).

Além dos três, há um **exemplo didático** (`extras/semaforo_contador.c`) e um
**utilitário de teste de concorrência** (`peertalk/peertalk_stress.c`) que devem
ser citados no relatório como evidência (detalhes abaixo).

### Decisões de projeto comuns (descreva na fundamentação)

- **Linguagem/compilação:** C11, compilado com `gcc -Wall -Wextra -std=c11
  -pthread`, tratando todo *warning* como erro a corrigir.
- **Chaves IPC:** geradas com `ftok()` a partir de um arquivo-âncora fixo e
  versionado (`.ipc_key`) + um id de projeto distinto por sistema. Isso garante
  que processos diferentes (binários separados) cheguem à mesma chave IPC. Como o
  caminho é relativo, os programas são executados a partir da raiz do projeto.
- **Convenção de `mtype` (tipo da mensagem na fila):** `mtype = 1` são mensagens
  destinadas ao servidor; `mtype = PID` do cliente são mensagens destinadas àquele
  cliente. (No PeerTalk, `mtype = PID do peer destino`.)
- **Tratamento de erro:** toda chamada de syscall IPC tem o retorno verificado;
  em erro, `perror()` + saída limpa. Os *wrappers* ficam centralizados em
  `common/ipc_utils.c` (e o contrato das mensagens em `common/protocol.h`).
- **Limpeza de recursos:** recursos IPC System V sobrevivem ao processo, então
  quem cria a fila/shm/semáforo é responsável por removê-los no encerramento e em
  `SIGINT` (`IPC_RMID`). Há um script `scripts/limpa_ipc.sh` para remover órfãos.

### Sistema 1 — DynaThreadMaker (arquivos: `dynathreadmaker/`)

Cliente/servidor. O servidor cria uma **thread** por requisição recebida na fila
de mensagens. O cliente é a interface de usuário (menu para criar thread ou
encerrar o servidor). Os campos de uma thread: nome, tempo `T` de execução
(`sleep(T)`), mensagem inicial, mensagem final e tamanho `N` de um vetor de
inteiros que a thread aloca, inicializa com 1..N e soma.

**O ponto central de concorrência (destaque isto no relatório):** os argumentos
da nova thread ficam numa área compartilhada por todas as threads do processo. Se
a thread principal seguisse para a próxima requisição e sobrescrevesse essa área
antes de a thread secundária copiá-la, haveria **condição de corrida**. A solução
é tratar essa área como **seção crítica**: a thread principal cria a secundária e
**espera** (com `pthread_mutex_t` + `pthread_cond_t`) até a secundária sinalizar
que já copiou os argumentos para variáveis locais; só então a principal responde
ao cliente e passa à próxima requisição. (A variável de condição faz o papel de
"semáforo de sinalização" entre as threads.) O cliente espera a resposta com
**timeout** e volta ao menu se o servidor não responder. Veja `servidor.c`
(função da thread secundária e o laço principal) e `cliente.c`.

### Sistema 2 — CentralTalk (arquivos: `centraltalk/`)

Chat centralizado. O servidor `chairman` executa **todos** os comandos e mantém o
estado: lista de usuários logados, caixa de mensagens diretas por usuário e um
fórum público. O cliente `speaker` apenas lê/valida comandos, empacota-os numa
mensagem (com seu PID) e exibe as respostas — **não executa lógica**.

Comandos: `send <nome> <texto>`, `msgs`, `post <texto>`, `show`, `del msgs`,
`del post <n>`, `del posts`, `users`, `myid`, `exit`, além de `login` (nome único,
até 20 caracteres, sem espaços).

**Decisão de concorrência (explique o porquê):** o `chairman` é **sequencial**
(*single-thread*) — atende uma requisição por vez num único laço de `msgrcv`. Como
cada comando é processado por completo antes do próximo, **não há acesso
concorrente às estruturas internas**, dispensando mutex no servidor. A
concorrência do sistema está em vários `speaker` enviando à mesma fila, que é
naturalmente serializada. Respostas de várias linhas (ex.: `msgs`, `show`,
`users`) são enviadas **uma linha por mensagem**, com a última marcada por um
campo `fim` — o `speaker` lê até receber `fim = 1`. Veja `chairman.c` e
`speaker.c`.

### Sistema 3 — PeerTalk (arquivos: `peertalk/`)

Chat **peer-to-peer**, sem servidor. Cada processo `peertalker` executa os
próprios comandos sobre recursos compartilhados:

- a **lista de usuários logados** vive em **memória compartilhada** (`shmget`);
- essa lista é uma **seção crítica** protegida por um **semáforo binário**
  (`semget`, usado como mutex, com `SEM_UNDO` para liberar o lock se o processo
  morrer segurando-o);
- as mensagens trafegam por uma **fila geral** com `mtype = PID do destino` e o
  `PID de origem` no corpo (comunicação direta entre dois peers).

**Decisão importante (explique):** como não há servidor, um utilitário separado
**`peertalk_init`** cria e inicializa os recursos IPC (memória compartilhada,
semáforo e fila), e `peertalk_init clean` os remove. Assim os peers ficam livres
de responsabilidade sobre a infraestrutura — coerente com a filosofia
descentralizada. No login, o peer faz `lock → verifica nome duplicado → cadastra
nome + PID → unlock`. O comando `recv` retira da fila, de forma **não-bloqueante**
(`IPC_NOWAIT`), todas as mensagens destinadas ao próprio PID e as guarda numa
tabela local; `msgs` mostra a tabela local; `del msgs` limpa só a cópia local.
Comandos: `send`, `recv`, `msgs`, `del msgs`, `users`, `myid`, `exit` (com
descadastro da lista). Veja `peertalker.c` e `peertalk_init.c`.

### Fundamentação — Mutex vs. Semáforo contador (use o exemplo extra)

Inclua uma subseção conceitual diferenciando:

- **Mutex** (semáforo binário, 0/1): garante **exclusão mútua** — um por vez na
  seção crítica. É o papel usado para proteger a lista no PeerTalk e os argumentos
  no DynaThreadMaker.
- **Semáforo contador** (valor pode ser > 1): conta **recursos** e **sincroniza**
  (esperar até haver espaço/itens).

Para ilustrar, use o exemplo `extras/semaforo_contador.c`: um
**produtor/consumidor** com buffer limitado e três semáforos — `VAZIOS` e
`CHEIOS` (contadores) e `MUTEX` (binário). Explique, de forma simples, que
`VAZIOS`/`CHEIOS` fazem o produtor/consumidor *esperarem* quando o buffer enche
ou esvazia (sincronização), enquanto o `MUTEX` apenas garante um-por-vez no
buffer (exclusão mútua). Conclua que "mutex" é um caso particular de semáforo
(binário), e o contador é a forma mais geral.

### Evidência de concorrência — `peertalk_stress`

Cite o utilitário `peertalk/peertalk_stress.c` como **prova prática** do papel do
mutex: ele dispara N processos que tentam logar no mesmo instante (barreira de
largada), com nomes repetidos de propósito. **Com o mutex**, cada nome é
cadastrado uma única vez (lista consistente); com a opção **`--no-lock`** (que
pula o semáforo), a corrida deixa passar nomes duplicados. Apresente isso como o
experimento que valida a necessidade da seção crítica. (Se quiser, descreva a
saída esperada: timestamps mostrando que, com o mutex, as entradas na seção
crítica não se sobrepõem.)

### Estrutura sugerida do relatório (ajuste à vontade)

1. **Introdução** — programação concorrente, objetivos, os três sistemas, e como
   o artigo está organizado.
2. **Ambiente e metodologia** — C11, gcc com `-Wall -Wextra`, Makefile, geração
   de chaves IPC com `ftok`, organização do código (common/ compartilhado).
3. **Fundamentação teórica** — filas de mensagens, memória compartilhada,
   semáforos/mutexes; a subseção mutex vs. semáforo contador (produtor/consumidor).
4. **DynaThreadMaker** — arquitetura (diagrama), o problema da cópia de argumentos
   e a solução com mutex + variável de condição, trechos de código, testes.
5. **CentralTalk** — arquitetura (diagrama), modelo sequencial do chairman,
   comandos, respostas multilinha, trechos de código, testes.
6. **PeerTalk** — arquitetura (diagrama), a lista em memória compartilhada como
   seção crítica protegida por semáforo, o utilitário de init, roteamento p2p das
   mensagens, o teste de stress como evidência, trechos de código, testes.
7. **Discussão** — comparação das três arquiteturas: onde está a concorrência em
   cada uma e que mecanismo de sincronização foi necessário (e por quê). Use uma
   tabela comparativa.
8. **Conclusão** — aprendizados, dificuldades, possíveis extensões.
9. **Referências** — sugestões: TANENBAUM (*Sistemas Operacionais Modernos*),
   STEVENS (*Advanced Programming in the UNIX Environment*), páginas de manual de
   `msgget(2)`, `shmget(2)`, `semget(2)`, `pthreads(7)`, material da disciplina.

### Dicas finais

- Para chegar às 10 páginas com qualidade: dê espaço aos diagramas, aos trechos
  de código comentados e às seções de teste (descreva cenários e o que se observa
  no terminal). Evite encher linguiça — prefira explicar bem cada decisão de
  concorrência.
- Mantenha um tom didático: sempre que introduzir um mecanismo (mutex, semáforo,
  fila), explique em uma frase o que ele resolve antes de mostrar o código.
- Deixe **campos marcados para os autores preencherem** os nomes e a data.
- Não invente funcionalidades que não estão no código. Se algo não estiver claro
  nos fontes, descreva de forma conservadora ou deixe uma observação.

## FIM DO PROMPT
