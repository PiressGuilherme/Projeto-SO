# DynaThreadMaker

Sistema cliente/servidor para **criação dinâmica de threads simuladas**, usando
fila de mensagens IPC System V e `pthreads`.

- **Servidor** (`dyn_servidor`): lê a fila continuamente; para cada requisição
  cria uma thread secundária que imprime mensagens, dorme `T` segundos, aloca um
  vetor de `N` inteiros (1..N) e imprime o somatório.
- **Cliente** (`dyn_cliente`): interface de usuário; coleta os dados da thread,
  envia a requisição ao servidor (carimbada com o próprio PID) e espera a
  resposta com timeout.

## Conceito central: cópia de argumentos sob mutex

Os argumentos da nova thread são preparados pela **thread principal** do servidor
numa área de memória compartilhada por todas as threads do processo. Se a
principal seguisse para a próxima requisição e sobrescrevesse essa área antes de
a thread secundária copiá-la, haveria **condição de corrida**.

Por isso a área de argumentos é uma **seção crítica** protegida por
`pthread_mutex_t` + `pthread_cond_t` (semáforo binário de sinalização):

1. A principal preenche a área e cria a secundária.
2. A principal **espera** (em laço, na variável de condição) até a secundária
   sinalizar que copiou os argumentos para suas variáveis locais.
3. Só então a principal responde ao cliente e volta a ler a fila.

Ver comentários detalhados em [`servidor.c`](servidor.c).

## Compilar

A partir da **raiz do projeto** (onde está o `Makefile` e o arquivo `.ipc_key`):

```bash
make dynathreadmaker        # ou apenas `make`
```

Gera `bin/dyn_servidor` e `bin/dyn_cliente`.

## Executar

> **Importante:** rode os binários **a partir da raiz do projeto**. As chaves IPC
> são geradas com `ftok("./.ipc_key", ...)`, e esse caminho é relativo ao
> diretório de execução. Servidor e clientes precisam enxergar o mesmo `.ipc_key`.

**Terminal A — servidor** (inicie primeiro, deixe rodando/minimizado):

```bash
./bin/dyn_servidor
```

**Terminais B, C, … — clientes**:

```bash
./bin/dyn_cliente
```

Menu do cliente:

- `1` — criar nova thread (pede nome, tempo `T`, mensagem inicial, mensagem
  final e tamanho `N` do vetor);
- `2` — encerrar o servidor (envia o pedido e sai este cliente; demais clientes
  saem manualmente);
- `0` — sair apenas deste cliente (o servidor continua no ar).

## Testes sugeridos

1. **Fluxo básico:** um cliente cria uma thread com `T=3`, `N=10`. Observe no
   terminal do servidor as mensagens de início/fim e o somatório (`55`). O
   cliente deve receber `[OK] Thread criada com sucesso`.
2. **Concorrência:** com `T` grande (ex.: `15`), dispare várias criações em
   sequência rápida (de um ou mais clientes). Várias threads devem rodar em
   paralelo e **cada uma imprime os seus próprios** nome/mensagens/somatório —
   confirmando que a cópia sob mutex evitou corrida nos argumentos.
3. **Timeout:** crie uma thread e, antes de o servidor responder, mate o
   servidor (`Ctrl+C`). O cliente deve avisar `TIMEOUT` após alguns segundos e
   voltar ao menu.
4. **Encerramento limpo:** opção `2` no cliente. O servidor remove a fila e sai.
   Confira com `ipcs` que a fila **não** aparece mais.

## Limpeza de recursos IPC

- `Ctrl+C` no servidor **também** remove a fila (handler de `SIGINT`).
- Entre testes, para remover recursos órfãos: `./scripts/limpa_ipc.sh`.
- Listar recursos ativos: `ipcs`.
