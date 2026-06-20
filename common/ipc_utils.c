/*
 * ipc_utils.c — Implementação dos wrappers de IPC System V.
 *
 * Regra de ouro do projeto (CLAUDE.md): nenhuma syscall de IPC pode ter seu
 * retorno -1 ignorado. Aqui concentramos toda a checagem de erro para que o
 * resto do código fique limpo e a política de tratamento seja consistente.
 */
#include "ipc_utils.h"

#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* A union semun, exigida por semctl(SETVAL), NÃO é mais declarada pelos headers
 * em sistemas modernos (conforme a página de manual de semctl) — cabe ao
 * programa defini-la. */
union semun {
    int               val;    /* valor para SETVAL */
    struct semid_ds  *buf;    /* buffer para IPC_STAT, IPC_SET */
    unsigned short   *array;  /* array para GETALL, SETALL */
};

key_t gerar_chave(const char *caminho, int id_projeto)
{
    /* ftok pode falhar, p.ex. se o arquivo-âncora não existir no diretório
     * atual. Tratamos como erro fatal: sem chave, nada funciona. */
    key_t chave = ftok(caminho, id_projeto);
    if (chave == (key_t) -1) {
        perror("ftok (gerar_chave) — verifique se o programa foi executado "
               "a partir da raiz do projeto, onde existe o arquivo .ipc_key");
        exit(EXIT_FAILURE);
    }
    return chave;
}

int abrir_fila(key_t chave, int flags)
{
    int msqid = msgget(chave, flags);
    if (msqid == -1) {
        perror("msgget (abrir_fila)");
        exit(EXIT_FAILURE);
    }
    return msqid;
}

void enviar_msg(int msqid, const void *msg, size_t tam)
{
    /* msgsnd retorna 0 em sucesso e -1 em erro. Sem IPC_NOWAIT, bloqueia se a
     * fila estiver cheia em vez de falhar — comportamento desejado aqui. */
    if (msgsnd(msqid, msg, tam, 0) == -1) {
        perror("msgsnd (enviar_msg)");
        exit(EXIT_FAILURE);
    }
}

ssize_t receber_msg(int msqid, void *msg, size_t tam, long tipo)
{
    /* msgrcv bloqueante: espera até chegar uma mensagem do tipo pedido. */
    ssize_t n = msgrcv(msqid, msg, tam, tipo, 0);
    if (n == -1) {
        perror("msgrcv (receber_msg)");
        exit(EXIT_FAILURE);
    }
    return n;
}

ssize_t tentar_receber_msg(int msqid, void *msg, size_t tam, long tipo)
{
    /* IPC_NOWAIT: se não houver mensagem do tipo pedido, msgrcv retorna -1 e
     * coloca errno == ENOMSG. Esse caso NÃO é fatal — devolvemos -1 para o
     * chamador decidir (tipicamente: dormir um pouco e tentar de novo, até
     * estourar o timeout). Qualquer outro errno é um erro real e abortamos. */
    ssize_t n = msgrcv(msqid, msg, tam, tipo, IPC_NOWAIT);
    if (n == -1) {
        if (errno == ENOMSG) {
            return -1;   /* fila vazia para esse tipo: situação esperada */
        }
        perror("msgrcv (tentar_receber_msg)");
        exit(EXIT_FAILURE);
    }
    return n;
}

void remover_fila(int msqid)
{
    /* IPC_RMID marca a fila para remoção. Recursos IPC System V sobrevivem ao
     * processo que os criou, então removê-los no encerramento é obrigatório
     * para não deixar filas órfãs (visíveis em `ipcs`). Aqui não usamos exit()
     * em caso de falha porque normalmente já estamos no caminho de saída. */
    if (msgctl(msqid, IPC_RMID, NULL) == -1) {
        perror("msgctl IPC_RMID (remover_fila)");
    }
}

/* ------------------------------------------------------------------------- *
 * Memória compartilhada
 * ------------------------------------------------------------------------- */

int criar_shm(key_t chave, size_t tam, int flags)
{
    int shmid = shmget(chave, tam, flags);
    if (shmid == -1) {
        perror("shmget (criar_shm)");
        exit(EXIT_FAILURE);
    }
    return shmid;
}

void *anexar_shm(int shmid)
{
    /* shmat retorna (void *) -1 em erro, e não NULL. */
    void *addr = shmat(shmid, NULL, 0);
    if (addr == (void *) -1) {
        perror("shmat (anexar_shm)");
        exit(EXIT_FAILURE);
    }
    return addr;
}

void desanexar_shm(const void *addr)
{
    if (shmdt(addr) == -1) {
        perror("shmdt (desanexar_shm)");
    }
}

void remover_shm(int shmid)
{
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl IPC_RMID (remover_shm)");
    }
}

/* ------------------------------------------------------------------------- *
 * Semáforos (conjunto com um único semáforo, usado como mutex binário)
 * ------------------------------------------------------------------------- */

int criar_sem(key_t chave, int flags)
{
    /* nsems = 1: um único semáforo no conjunto. */
    int semid = semget(chave, 1, flags);
    if (semid == -1) {
        perror("semget (criar_sem)");
        exit(EXIT_FAILURE);
    }
    return semid;
}

void inicializar_sem(int semid, int valor)
{
    union semun arg;
    arg.val = valor;
    if (semctl(semid, 0, SETVAL, arg) == -1) {
        perror("semctl SETVAL (inicializar_sem)");
        exit(EXIT_FAILURE);
    }
}

void sem_lock(int semid)
{
    /* Operação P: sem_op = -1 decrementa; se o valor for 0, BLOQUEIA até que
     * outro processo libere (V). SEM_UNDO faz o kernel desfazer a operação se
     * o processo morrer segurando o lock — evita travar a lista para sempre. */
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = -1;
    op.sem_flg = SEM_UNDO;
    if (semop(semid, &op, 1) == -1) {
        perror("semop P (sem_lock)");
        exit(EXIT_FAILURE);
    }
}

void sem_unlock(int semid)
{
    /* Operação V: sem_op = +1 incrementa, liberando quem espera. */
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = +1;
    op.sem_flg = SEM_UNDO;
    if (semop(semid, &op, 1) == -1) {
        perror("semop V (sem_unlock)");
        exit(EXIT_FAILURE);
    }
}

void remover_sem(int semid)
{
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("semctl IPC_RMID (remover_sem)");
    }
}
