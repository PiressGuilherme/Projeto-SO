/*
 * ipc_utils.h — Wrappers para as syscalls de IPC System V.
 *
 * Objetivo: centralizar a checagem de erro exigida pelo projeto. TODA syscall
 * de IPC tem seu retorno verificado; em erro chamamos perror() e encerramos o
 * processo de forma limpa (ou retornamos um código que o chamador trata). Isso
 * evita repetir o mesmo "if (rc == -1) { perror(...); exit(...); }" em cada
 * ponto de uso e garante que nenhum -1 seja ignorado silenciosamente.
 *
 * Cobertura atual: filas de mensagens (Fase 1). Helpers de memória
 * compartilhada e semáforos serão adicionados nas Fases 2/3.
 */
#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include <sys/types.h>   /* key_t, pid_t, size_t, ssize_t */

/* ------------------------------------------------------------------------- *
 * Geração de chave
 * ------------------------------------------------------------------------- */

/*
 * Gera a chave IPC com ftok(caminho, id_projeto). Em erro, imprime mensagem
 * via perror() e encerra o processo (a falha aqui é fatal: sem chave não há
 * como abrir o recurso). O caminho deve apontar para um arquivo existente.
 */
key_t gerar_chave(const char *caminho, int id_projeto);

/* ------------------------------------------------------------------------- *
 * Filas de mensagens
 * ------------------------------------------------------------------------- */

/*
 * Envolve msgget(). `flags` controla criação/permissões, por exemplo:
 *   - servidor: IPC_CREAT | 0666  (cria a fila se não existir)
 *   - cliente : 0                 (apenas abre uma fila já existente)
 * Em erro, perror() + exit(). Retorna o id da fila (msqid).
 */
int abrir_fila(key_t chave, int flags);

/*
 * Envolve msgsnd(). Envia `tam` bytes de corpo (sem contar o campo mtype) a
 * partir de `msg` (que aponta para uma struct cujo primeiro campo é o mtype).
 * Em erro, perror() + exit(). Bloqueia se a fila estiver cheia (sem IPC_NOWAIT).
 */
void enviar_msg(int msqid, const void *msg, size_t tam);

/*
 * Envolve msgrcv() em modo BLOQUEANTE. Recebe uma mensagem de tipo `tipo`
 * (ou a primeira de qualquer tipo, se tipo == 0) para o buffer `msg`, lendo
 * no máximo `tam` bytes de corpo. Em erro, perror() + exit().
 * Retorna o número de bytes de corpo efetivamente recebidos.
 */
ssize_t receber_msg(int msqid, void *msg, size_t tam, long tipo);

/*
 * Versão NÃO BLOQUEANTE de msgrcv (usa IPC_NOWAIT). Útil para implementar
 * timeout no cliente: tenta receber sem bloquear.
 *   - retorna >= 0  : bytes de corpo recebidos (sucesso);
 *   - retorna -1    : não havia mensagem do tipo pedido (errno == ENOMSG) —
 *                     NÃO é tratado como erro fatal; o chamador decide o que
 *                     fazer (ex.: esperar e tentar de novo, ou desistir).
 * Qualquer outro erro de msgrcv é fatal (perror() + exit()).
 */
ssize_t tentar_receber_msg(int msqid, void *msg, size_t tam, long tipo);

/*
 * Remove a fila de mensagens (msgctl IPC_RMID). Deve ser chamada pelo
 * processo responsável pela fila no encerramento. Em erro, perror() (mas NÃO
 * encerra: estamos provavelmente já no caminho de saída/limpeza).
 */
void remover_fila(int msqid);

/* ------------------------------------------------------------------------- *
 * Memória compartilhada (usada pelo PeerTalk — Fase 3)
 * ------------------------------------------------------------------------- */

/*
 * Envolve shmget(). `flags` controla criação/permissões:
 *   - criador: IPC_CREAT | IPC_EXCL | 0666  (falha se já existir)
 *   - anexante: 0                            (apenas abre uma já existente)
 * Em erro, perror() + exit(). Retorna o id do segmento (shmid).
 */
int criar_shm(key_t chave, size_t tam, int flags);

/*
 * Envolve shmat(): anexa o segmento ao espaço de endereçamento do processo.
 * Em erro, perror() + exit(). Retorna o ponteiro para a região compartilhada.
 */
void *anexar_shm(int shmid);

/*
 * Envolve shmdt(): desanexa a região (não remove o segmento). Em erro, perror().
 */
void desanexar_shm(const void *addr);

/*
 * Remove o segmento de memória compartilhada (shmctl IPC_RMID). Em erro, perror().
 */
void remover_shm(int shmid);

/* ------------------------------------------------------------------------- *
 * Semáforos (usado pelo PeerTalk — Fase 3)
 * ------------------------------------------------------------------------- *
 * Usamos um conjunto com UM semáforo, operado como mutex binário (valor 1 =
 * livre, 0 = ocupado) para proteger a lista de usuários na memória compartilhada.
 */

/*
 * Envolve semget(). `flags` como em criar_shm. Em erro, perror() + exit().
 * Retorna o id do conjunto de semáforos (semid).
 */
int criar_sem(key_t chave, int flags);

/*
 * Inicializa o semáforo (índice 0 do conjunto) com `valor` via semctl(SETVAL).
 * Deve ser chamada APENAS pelo processo criador, uma vez. Em erro, perror() + exit().
 */
void inicializar_sem(int semid, int valor);

/*
 * Operação P (wait/lock): decrementa o semáforo, bloqueando se já está em 0.
 * Marca a ENTRADA na seção crítica. Em erro, perror() + exit().
 */
void sem_lock(int semid);

/*
 * Operação V (signal/unlock): incrementa o semáforo, liberando a seção crítica.
 * Em erro, perror() + exit().
 */
void sem_unlock(int semid);

/*
 * Remove o conjunto de semáforos (semctl IPC_RMID). Em erro, perror().
 */
void remover_sem(int semid);

#endif /* IPC_UTILS_H */
