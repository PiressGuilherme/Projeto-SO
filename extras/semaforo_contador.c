/*
 * semaforo_contador.c — Exemplo didático: SEMÁFORO CONTADOR vs. MUTEX.
 *
 * Este programa é material COMPLEMENTAR (não faz parte dos três exercícios do
 * trabalho). Ele existe para evidenciar a diferença conceitual entre:
 *
 *   - MUTEX (semáforo BINÁRIO, valor 0/1): garante EXCLUSÃO MÚTUA — só um
 *     processo por vez na seção crítica. É o que usamos no PeerTalk para
 *     proteger a lista de usuários.
 *
 *   - SEMÁFORO CONTADOR (valor pode ser > 1): conta RECURSOS disponíveis e
 *     SINCRONIZA produtores/consumidores. Não serve para exclusão mútua por si
 *     só; serve para "espere até haver algo / até haver espaço".
 *
 * Demonstração: o clássico PRODUTOR/CONSUMIDOR com buffer circular limitado,
 * usando TRÊS semáforos System V (um conjunto com 3 elementos):
 *
 *   sem[VAZIOS] : contador, inicia em TAM_BUFFER — nº de posições LIVRES.
 *                 O produtor faz P(vazios) antes de inserir (bloqueia se cheio).
 *   sem[CHEIOS] : contador, inicia em 0 — nº de itens PRONTOS para consumir.
 *                 O consumidor faz P(cheios) antes de retirar (bloqueia se vazio).
 *   sem[MUTEX]  : binário, inicia em 1 — protege o buffer em si (exclusão mútua).
 *
 * Repare: VAZIOS e CHEIOS são CONTADORES (chegam a valores > 1 e sincronizam o
 * ritmo entre produtor e consumidor); MUTEX é binário (exclusão mútua). É a
 * combinação correta — e mostra por que "semáforo" é mais geral que "mutex".
 *
 * O buffer compartilhado fica em memória compartilhada (shm), pois produtor e
 * consumidor são PROCESSOS distintos (fork).
 *
 * Uso:
 *   ./bin/semaforo_contador [n_itens]      (padrão: 10 itens)
 *
 * Os recursos IPC (shm + semáforos) são criados e REMOVIDOS por este próprio
 * programa ao final — é autocontido e não deixa órfãos.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>

#define TAM_BUFFER 4    /* propositalmente PEQUENO, para o buffer encher e o
                         * produtor ter que ESPERAR — evidencia o contador. */

/* Índices dos três semáforos dentro do conjunto. */
enum { VAZIOS = 0, CHEIOS = 1, MUTEX = 2 };

/* union semun: precisa ser declarada pelo programa (não vem nos headers). */
union semun {
    int             val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/* Buffer circular compartilhado entre os processos. */
struct buffer_compartilhado {
    int dados[TAM_BUFFER];
    int in;     /* próxima posição de escrita (produtor) */
    int out;    /* próxima posição de leitura (consumidor) */
};

static int g_semid;
static struct buffer_compartilhado *g_buf;

/* ------------------------------------------------------------------------- *
 * Operações P (espera/decrementa) e V (sinaliza/incrementa) num semáforo do
 * conjunto, identificado pelo índice. São as primitivas fundamentais.
 * ------------------------------------------------------------------------- */
static void P(int indice)
{
    struct sembuf op = { (unsigned short) indice, -1, 0 };
    if (semop(g_semid, &op, 1) == -1) {
        perror("semop P");
        exit(EXIT_FAILURE);
    }
}

static void V(int indice)
{
    struct sembuf op = { (unsigned short) indice, +1, 0 };
    if (semop(g_semid, &op, 1) == -1) {
        perror("semop V");
        exit(EXIT_FAILURE);
    }
}

/* Pequena pausa aleatória, só para o produtor e o consumidor andarem em ritmos
 * diferentes e o buffer ora encher, ora esvaziar (deixa a demonstração viva). */
static void pausa_curta(unsigned int *semente)
{
    int ms = rand_r(semente) % 120;
    struct timespec t = { 0, (long) ms * 1000 * 1000 };
    nanosleep(&t, NULL);
}

/* ------------------------------------------------------------------------- *
 * Produtor: insere n_itens no buffer.
 * ------------------------------------------------------------------------- */
static void produtor(int n_itens)
{
    unsigned int semente = (unsigned int) getpid();
    for (int i = 1; i <= n_itens; i++) {
        pausa_curta(&semente);

        P(VAZIOS);                 /* espera haver posição livre (CONTADOR) */
        P(MUTEX);                  /* entra na seção crítica do buffer (MUTEX) */

        g_buf->dados[g_buf->in] = i;
        printf("[produtor] inseriu %2d na posicao %d\n", i, g_buf->in);
        fflush(stdout);
        g_buf->in = (g_buf->in + 1) % TAM_BUFFER;

        V(MUTEX);                  /* sai da seção crítica */
        V(CHEIOS);                 /* avisa que há +1 item para consumir */
    }
    printf("[produtor] terminei de produzir %d itens.\n", n_itens);
    fflush(stdout);
}

/* ------------------------------------------------------------------------- *
 * Consumidor: retira n_itens do buffer.
 * ------------------------------------------------------------------------- */
static void consumidor(int n_itens)
{
    unsigned int semente = (unsigned int) getpid();
    for (int i = 1; i <= n_itens; i++) {
        pausa_curta(&semente);

        P(CHEIOS);                 /* espera haver item disponível (CONTADOR) */
        P(MUTEX);                  /* entra na seção crítica do buffer (MUTEX) */

        int item = g_buf->dados[g_buf->out];
        printf("            [consumidor] retirou %2d da posicao %d\n",
               item, g_buf->out);
        fflush(stdout);
        g_buf->out = (g_buf->out + 1) % TAM_BUFFER;

        V(MUTEX);                  /* sai da seção crítica */
        V(VAZIOS);                 /* avisa que abriu +1 posição livre */
    }
    printf("            [consumidor] terminei de consumir %d itens.\n", n_itens);
    fflush(stdout);
}

/* ------------------------------------------------------------------------- *
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int n_itens = 10;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v > 0) n_itens = v;
    }

    printf("=== Exemplo: SEMAFORO CONTADOR (produtor/consumidor) ===\n");
    printf("Buffer com %d posicoes; produzir/consumir %d itens.\n", TAM_BUFFER,
           n_itens);
    printf("VAZIOS e CHEIOS sao CONTADORES; MUTEX e binario (exclusao mutua).\n\n");

    /* Cria a memória compartilhada do buffer (IPC_PRIVATE: anônima, herdada via
     * fork). Inicializa in/out. */
    int shmid = shmget(IPC_PRIVATE, sizeof(struct buffer_compartilhado),
                       IPC_CREAT | 0600);
    if (shmid == -1) { perror("shmget"); exit(EXIT_FAILURE); }
    g_buf = shmat(shmid, NULL, 0);
    if (g_buf == (void *) -1) { perror("shmat"); exit(EXIT_FAILURE); }
    memset(g_buf, 0, sizeof(*g_buf));

    /* Cria o conjunto com 3 semáforos. */
    g_semid = semget(IPC_PRIVATE, 3, IPC_CREAT | 0600);
    if (g_semid == -1) { perror("semget"); exit(EXIT_FAILURE); }

    /* Inicializa os valores: VAZIOS = TAM_BUFFER (tudo livre), CHEIOS = 0
     * (nada a consumir), MUTEX = 1 (livre). */
    union semun arg;
    arg.val = TAM_BUFFER; if (semctl(g_semid, VAZIOS, SETVAL, arg) == -1) { perror("semctl VAZIOS"); exit(EXIT_FAILURE); }
    arg.val = 0;          if (semctl(g_semid, CHEIOS, SETVAL, arg) == -1) { perror("semctl CHEIOS"); exit(EXIT_FAILURE); }
    arg.val = 1;          if (semctl(g_semid, MUTEX,  SETVAL, arg) == -1) { perror("semctl MUTEX");  exit(EXIT_FAILURE); }

    /* Fork: filho = consumidor, pai = produtor. */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }

    if (pid == 0) {
        consumidor(n_itens);
        shmdt(g_buf);
        _exit(EXIT_SUCCESS);
    } else {
        produtor(n_itens);
        wait(NULL);   /* espera o consumidor terminar */

        /* Limpeza: remove shm e semáforos (autocontido, sem órfãos). */
        shmdt(g_buf);
        shmctl(shmid, IPC_RMID, NULL);
        semctl(g_semid, 0, IPC_RMID);

        printf("\nResumo: o produtor BLOQUEIA em P(VAZIOS) quando o buffer enche,\n");
        printf("e o consumidor BLOQUEIA em P(CHEIOS) quando o buffer esvazia.\n");
        printf("Esse 'contar recursos e esperar' e o papel do SEMAFORO CONTADOR,\n");
        printf("diferente do MUTEX, que apenas garante um-por-vez na area critica.\n");
    }

    return EXIT_SUCCESS;
}
