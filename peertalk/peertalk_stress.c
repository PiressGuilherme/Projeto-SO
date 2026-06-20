/*
 * peertalk_stress.c — Teste de concorrência da lista de usuários do PeerTalk.
 *
 * Objetivo: DEMONSTRAR (e validar) que o semáforo que protege a lista de
 * usuários em memória compartilhada evita corridas em logins simultâneos.
 *
 * Por que um programa separado (e não dirigir o peertalker interativo)? Porque
 * para exercitar o mutex de verdade precisamos de muitos processos chegando ao
 * cadastro EXATAMENTE no mesmo instante. Aqui isso é garantido por uma
 * "barreira de largada": todos os filhos giram esperando um sinal comum e só
 * então disparam o login juntos — colisão máxima, o pior caso para o mutex.
 *
 * O programa REUSA as structs (pt_lista_compartilhada) e os helpers de IPC já
 * existentes; não altera o peertalker.
 *
 * Uso:
 *   ./bin/peertalk_stress [N] [--no-lock]
 *      N          número de processos concorrentes (padrão 20).
 *      --no-lock  pula sem_lock/sem_unlock no cadastro — demonstra a CORRIDA
 *                 (lista corrompida / nomes duplicados aceitos). Sem essa flag,
 *                 o cadastro é protegido pelo semáforo (comportamento correto).
 *
 * Pré-requisito: rodar ./bin/peertalk_init antes (cria shm + semáforo + fila).
 * O stress ZERA a lista no início para que cada execução comece limpa.
 *
 * Cenário: os N processos tentam logar simultaneamente. Para provar a rejeição
 * de duplicados, vários deles usam nomes REPETIDOS de propósito (apenas
 * NOMES_DISTINTOS nomes diferentes giram entre os N). Com o mutex, cada nome é
 * cadastrado UMA única vez; sem o mutex, a corrida deixa passar duplicados.
 */
#include "protocol.h"
#include "ipc_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>

/* Quantidade de nomes DISTINTOS em rotação. Como N costuma ser maior, vários
 * processos recebem o mesmo nome — é isso que testa a rejeição de duplicados. */
#define NOMES_DISTINTOS 8

/* Barreira de largada compartilhada entre os processos (shm anônima própria do
 * teste, separada da lista do PeerTalk). `largada` vira 1 quando todos estão
 * prontos. `prontos` conta quantos filhos já chegaram à barreira. */
struct barreira {
    volatile int prontos;
    volatile int largada;
};

/* Relógio monotônico em milissegundos, para os logs de timestamp. */
static long agora_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------------- *
 * Cadastro de um peer na lista compartilhada.
 * ------------------------------------------------------------------------- *
 * Reproduz a lógica de login do peertalker (checa duplicado -> cadastra), mas
 * instrumentada com logs e com o lock OPCIONAL (para o modo --no-lock).
 *
 * Para tornar a janela de corrida bem visível no modo --no-lock, inserimos um
 * pequeno atraso ENTRE "verificar duplicado" e "gravar o cadastro": é nessa
 * janela que, sem o mutex, dois processos com o mesmo nome ambos veem "livre"
 * e ambos gravam. Com o mutex, a janela está protegida e isso não acontece.
 */
static void cadastrar(struct pt_lista_compartilhada *lista, int semid,
                      const char *nome, pid_t pid, int usar_lock)
{
    if (usar_lock) sem_lock(semid);

    long t_in = agora_ms();
    printf("[%6ld ms] pid %-7d nome=%-10s -> ENTROU na secao critica%s\n",
           t_in, (int) pid, nome, usar_lock ? "" : " (SEM lock!)");
    fflush(stdout);

    /* 1) Verifica se o nome já existe. */
    int duplicado = 0;
    for (int i = 0; i < PT_MAX_USUARIOS; i++) {
        if (lista->usuarios[i].ativo &&
            strcmp(lista->usuarios[i].nome, nome) == 0) {
            duplicado = 1;
            break;
        }
    }

    /* Janela de corrida proposital: dá tempo de outro processo entrar aqui
     * quando o lock está desligado. Com lock, ninguém mais entra. */
    struct timespec atraso = { 0, 2 * 1000 * 1000 };  /* 2 ms */
    nanosleep(&atraso, NULL);

    if (duplicado) {
        printf("[%6ld ms] pid %-7d nome=%-10s -> RECUSADO (nome em uso)\n",
               agora_ms(), (int) pid, nome);
    } else {
        /* 2) Cadastra no primeiro slot livre. */
        int slot = -1;
        for (int i = 0; i < PT_MAX_USUARIOS; i++) {
            if (!lista->usuarios[i].ativo) { slot = i; break; }
        }
        if (slot == -1) {
            printf("[%6ld ms] pid %-7d nome=%-10s -> SEM SLOT (lista cheia)\n",
                   agora_ms(), (int) pid, nome);
        } else {
            lista->usuarios[slot].ativo = 1;
            lista->usuarios[slot].pid   = pid;
            strncpy(lista->usuarios[slot].nome, nome, MAX_NOME);
            lista->usuarios[slot].nome[MAX_NOME - 1] = '\0';
            printf("[%6ld ms] pid %-7d nome=%-10s -> CADASTRADO no slot %d\n",
                   agora_ms(), (int) pid, nome, slot);
        }
    }
    fflush(stdout);

    if (usar_lock) sem_unlock(semid);
}

/* ------------------------------------------------------------------------- *
 * Relatório final: conta cadastros por nome e detecta duplicados.
 * ------------------------------------------------------------------------- */
static int conferir_lista(struct pt_lista_compartilhada *lista)
{
    int total = 0;
    int duplicados = 0;

    printf("\n===== ESTADO FINAL DA LISTA =====\n");
    for (int i = 0; i < PT_MAX_USUARIOS; i++) {
        if (lista->usuarios[i].ativo) {
            total++;
            /* Conta quantas vezes este nome aparece (a partir daqui). */
            int repete = 0;
            for (int j = i + 1; j < PT_MAX_USUARIOS; j++) {
                if (lista->usuarios[j].ativo &&
                    strcmp(lista->usuarios[j].nome,
                           lista->usuarios[i].nome) == 0) {
                    repete++;
                }
            }
            printf("  slot %2d: %-10s (pid %d)%s\n",
                   i, lista->usuarios[i].nome, (int) lista->usuarios[i].pid,
                   repete ? "   <-- DUPLICADO!" : "");
            if (repete) duplicados++;
        }
    }
    printf("Total de cadastros ativos: %d (esperado: %d nomes distintos)\n",
           total, NOMES_DISTINTOS);
    if (duplicados) {
        printf(">>> CORRIDA DETECTADA: %d nome(s) aparecem DUPLICADOS na lista!\n",
               duplicados);
        printf(">>> Isso e exatamente o bug que o semaforo (mutex) previne.\n");
    } else {
        printf(">>> Lista consistente: nenhum nome duplicado. Mutex fez seu papel.\n");
    }
    return duplicados;
}

/* ------------------------------------------------------------------------- *
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int n = 20;
    int usar_lock = 1;

    /* Parsing simples dos argumentos: [N] e/ou --no-lock, em qualquer ordem. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-lock") == 0) {
            usar_lock = 0;
        } else {
            int v = atoi(argv[i]);
            if (v > 0) n = v;
        }
    }
    if (n > PT_MAX_USUARIOS) {
        printf("Aviso: N=%d excede PT_MAX_USUARIOS=%d; limitando a %d.\n",
               n, PT_MAX_USUARIOS, PT_MAX_USUARIOS);
        n = PT_MAX_USUARIOS;
    }

    printf("=== PeerTalk STRESS — %d processos, mutex %s ===\n",
           n, usar_lock ? "LIGADO" : "DESLIGADO (--no-lock)");
    printf("(Pre-requisito: ./bin/peertalk_init ja executado.)\n\n");

    /* Anexa a lista compartilhada e o semáforo já criados pelo peertalk_init. */
    key_t k_shm = gerar_chave(IPC_KEY_PATH, PROJ_ID_PEERTALK_SHM);
    key_t k_sem = gerar_chave(IPC_KEY_PATH, PROJ_ID_PEERTALK_SEM);
    int shmid = criar_shm(k_shm, sizeof(struct pt_lista_compartilhada), 0);
    struct pt_lista_compartilhada *lista = anexar_shm(shmid);
    int semid = criar_sem(k_sem, 0);

    /* Zera a lista para começar limpo (idealmente nenhum peertalker logado). */
    memset(lista, 0, sizeof(*lista));

    /* Cria a barreira de largada numa shm anônima (IPC_PRIVATE), herdada pelos
     * filhos via fork. Garante que todos disparem o login no mesmo instante. */
    int bid = shmget(IPC_PRIVATE, sizeof(struct barreira), IPC_CREAT | 0600);
    if (bid == -1) { perror("shmget(barreira)"); exit(EXIT_FAILURE); }
    struct barreira *bar = shmat(bid, NULL, 0);
    if (bar == (void *) -1) { perror("shmat(barreira)"); exit(EXIT_FAILURE); }
    bar->prontos = 0;
    bar->largada = 0;

    /* Dispara N filhos. Cada um recebe um nome em rotação (gera duplicados). */
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }

        if (pid == 0) {
            /* ---- Processo FILHO ---- */
            char nome[MAX_NOME];
            snprintf(nome, sizeof(nome), "user%d", i % NOMES_DISTINTOS);

            /* Chega à barreira e espera a largada. IMPORTANTE: cedemos a CPU
             * com sched_yield() a cada iteração em vez de girar a 100%. Com N
             * processos esperando ao mesmo tempo, um busy-wait puro satura a
             * máquina (foi o que travou a VM). sched_yield mantém a largada
             * praticamente simultânea, mas sem monopolizar o processador. */
            __sync_fetch_and_add(&bar->prontos, 1);
            while (!bar->largada) {
                sched_yield();
            }

            cadastrar(lista, semid, nome, getpid(), usar_lock);

            shmdt(bar);
            desanexar_shm(lista);
            _exit(EXIT_SUCCESS);
        }
        /* ---- Processo PAI segue criando os demais filhos ---- */
    }

    /* Espera todos os filhos chegarem à barreira, então dá a largada. */
    while (bar->prontos < n) {
        struct timespec t = { 0, 1000 * 1000 };  /* 1 ms */
        nanosleep(&t, NULL);
    }
    printf("--- Todos os %d processos prontos. LARGADA! ---\n\n", n);
    bar->largada = 1;

    /* Aguarda o término de todos os filhos. */
    for (int i = 0; i < n; i++) {
        wait(NULL);
    }

    /* Confere o resultado e resume. */
    int dup = conferir_lista(lista);

    /* Limpeza da barreira (a lista/semáforo do PeerTalk NÃO são removidos aqui:
     * cabem ao peertalk_init clean / limpa_ipc.sh). */
    shmdt(bar);
    shmctl(bid, IPC_RMID, NULL);
    desanexar_shm(lista);

    printf("\nDica: rode com e sem '--no-lock' e compare.\n");
    printf("  ./bin/peertalk_init        # zere os recursos antes, se preciso\n");
    printf("  ./bin/peertalk_stress %d            # com mutex  -> lista consistente\n", n);
    printf("  ./bin/peertalk_stress %d --no-lock  # sem mutex  -> tende a corromper\n", n);

    /* Código de saída: 0 se consistente; 1 se detectou duplicados (útil em CI/
     * scripts para evidenciar a diferença com vs. sem mutex). */
    return dup ? 1 : 0;
}
