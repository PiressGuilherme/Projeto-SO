/*
 * peertalk_init.c — Utilitário de inicialização/limpeza do sistema PeerTalk.
 *
 * Como o PeerTalk é descentralizado (sem servidor), os processos `peertalker`
 * não devem ser responsáveis por criar a infraestrutura IPC. Este utilitário
 * separado cuida disso:
 *
 *   ./bin/peertalk_init          -> CRIA e inicializa os recursos:
 *                                    - memória compartilhada (lista de usuários,
 *                                      zerada = todos os slots livres);
 *                                    - semáforo binário (valor 1 = mutex livre);
 *                                    - fila de mensagens geral.
 *
 *   ./bin/peertalk_init clean    -> REMOVE os três recursos (shm + sem + fila).
 *
 * Rode `peertalk_init` UMA vez antes de abrir os peertalkers, e
 * `peertalk_init clean` ao final da sessão para não deixar recursos órfãos
 * (alternativa: scripts/limpa_ipc.sh).
 */
#include "protocol.h"
#include "ipc_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>

/* Abre os três recursos em modo "anexar" (sem criar) para depois removê-los.
 * Usado no modo clean. Retorna 0 se removeu tudo; ignora os que não existirem. */
static int limpar_recursos(key_t k_shm, key_t k_sem, key_t k_msg)
{
    int algum = 0;

    int shmid = shmget(k_shm, 0, 0);
    if (shmid != -1) { remover_shm(shmid); algum = 1;
        printf("[init] memoria compartilhada removida (shmid=%d)\n", shmid); }

    int semid = semget(k_sem, 0, 0);
    if (semid != -1) { remover_sem(semid); algum = 1;
        printf("[init] semaforo removido (semid=%d)\n", semid); }

    int msqid = msgget(k_msg, 0);
    if (msqid != -1) { remover_fila(msqid); algum = 1;
        printf("[init] fila de mensagens removida (msqid=%d)\n", msqid); }

    if (!algum) {
        printf("[init] nenhum recurso do PeerTalk encontrado para remover.\n");
    }
    return 0;
}

int main(int argc, char *argv[])
{
    /* Gera as três chaves a partir do mesmo arquivo-âncora, com ids distintos. */
    key_t k_shm = gerar_chave(IPC_KEY_PATH, PROJ_ID_PEERTALK_SHM);
    key_t k_sem = gerar_chave(IPC_KEY_PATH, PROJ_ID_PEERTALK_SEM);
    key_t k_msg = gerar_chave(IPC_KEY_PATH, PROJ_ID_PEERTALK_MSG);

    /* Modo limpeza. */
    if (argc >= 2 && strcmp(argv[1], "clean") == 0) {
        printf("[init] Removendo recursos IPC do PeerTalk...\n");
        return limpar_recursos(k_shm, k_sem, k_msg);
    }

    /* Modo criação. IPC_EXCL garante que falhamos se já existirem — evita
     * recriar por cima de uma sessão ativa. Se já existirem, oriente a usar
     * `clean` antes. */
    int flags_criacao = IPC_CREAT | IPC_EXCL | 0666;

    int shmid = criar_shm(k_shm, sizeof(struct pt_lista_compartilhada),
                          flags_criacao);
    /* Anexa para inicializar a lista (todos os slots livres). */
    struct pt_lista_compartilhada *lista = anexar_shm(shmid);
    memset(lista, 0, sizeof(*lista));   /* ativo = 0 em todos os slots */
    desanexar_shm(lista);
    printf("[init] memoria compartilhada criada e zerada (shmid=%d, %zu bytes)\n",
           shmid, sizeof(struct pt_lista_compartilhada));

    /* Semáforo binário inicializado em 1 (mutex livre). */
    int semid = criar_sem(k_sem, flags_criacao);
    inicializar_sem(semid, 1);
    printf("[init] semaforo (mutex) criado e inicializado em 1 (semid=%d)\n",
           semid);

    /* Fila de mensagens geral. */
    int msqid = abrir_fila(k_msg, flags_criacao);
    printf("[init] fila de mensagens criada (msqid=%d)\n", msqid);

    printf("[init] PeerTalk pronto. Agora abra os peertalkers com ./bin/peertalker\n");
    printf("[init] Ao final, remova os recursos com: ./bin/peertalk_init clean\n");
    return EXIT_SUCCESS;
}
