# Extras — exemplos didáticos

Material **complementar** (não faz parte dos três exercícios do trabalho), criado
para evidenciar conceitos de sincronização discutidos no relatório.

## `semaforo_contador` — semáforo contador vs. mutex

Os três sistemas do trabalho usam semáforos/mutexes no papel de **exclusão
mútua** (binários, valor 0/1). Este exemplo mostra o outro papel do semáforo: o
**semáforo contador** (valor > 1), que conta recursos e sincroniza ritmos.

Implementa o clássico **produtor/consumidor** com buffer circular limitado, em
dois processos (`fork`), usando um conjunto de **três semáforos System V**:

| Semáforo | Tipo | Valor inicial | Papel |
|---|---|---|---|
| `VAZIOS` | **contador** | `TAM_BUFFER` | nº de posições livres; o produtor faz `P(VAZIOS)` e **bloqueia se o buffer encher** |
| `CHEIOS` | **contador** | `0` | nº de itens prontos; o consumidor faz `P(CHEIOS)` e **bloqueia se o buffer esvaziar** |
| `MUTEX` | **binário** | `1` | exclusão mútua no acesso ao buffer |

A diferença fica visível na execução: `VAZIOS` e `CHEIOS` assumem valores `> 1`
e regulam o ritmo entre produtor e consumidor (sincronização), enquanto `MUTEX`
apenas garante um-por-vez na seção crítica (exclusão mútua). É a mesma primitiva
`semop()`, em dois usos conceitualmente distintos.

> **Como isso se relaciona com o DynaThreadMaker (exercício 1)?** Lá, a thread
> principal só prossegue quando a secundária **sinaliza** que copiou os
> argumentos — um uso de **sincronização por sinalização**, implementado com
> `pthread_mutex_t` + `pthread_cond_t` (a variável de condição é o equivalente,
> em pthreads, de um semáforo de sinalização). Este exemplo torna explícita a
> primitiva *semáforo* nesse papel mais geral.

### Compilar e executar

A partir da **raiz do projeto**:

```bash
make extras
./bin/semaforo_contador          # 10 itens (padrão)
./bin/semaforo_contador 20       # 20 itens
```

O programa é **autocontido**: cria e remove seus próprios recursos IPC (shm +
semáforos anônimos via `IPC_PRIVATE`), sem deixar órfãos. Saída típica intercala
inserções do produtor e retiradas do consumidor; quando o buffer (tamanho 4)
enche, o produtor pausa em `P(VAZIOS)` até o consumidor abrir espaço — e
vice-versa.
