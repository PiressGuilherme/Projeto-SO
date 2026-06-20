/*
 * protocol.h — Definições compartilhadas pelos três sistemas do Trabalho Prático.
 *
 * Centraliza, em um único lugar:
 *   - a geração das chaves IPC (ftok) usadas por cada sistema;
 *   - a convenção de msgtyp (tipo de mensagem) comum aos sistemas;
 *   - os limites de tamanho de campos (nome, texto);
 *   - as structs de mensagem trocadas via fila de mensagens.
 *
 * Manter este contrato em um header único garante que servidor e clientes
 * (que são binários separados) concordem byte a byte sobre o formato das
 * mensagens. Qualquer divergência aqui causa corrupção silenciosa de dados.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <sys/types.h>   /* pid_t, key_t */

/* ------------------------------------------------------------------------- *
 * Geração das chaves IPC (ftok)
 * ------------------------------------------------------------------------- *
 * ftok(caminho, id_projeto) gera uma key_t a partir do inode do arquivo
 * apontado por `caminho` + os 8 bits menos significativos de `id_projeto`.
 * Todos os processos de um mesmo sistema devem usar o MESMO caminho e o
 * MESMO id de projeto para obter a mesma chave (e, logo, o mesmo recurso).
 *
 * Usamos um arquivo-âncora fixo e versionado na raiz do projeto. O caminho
 * é RELATIVO, então o servidor e os clientes precisam ser executados a
 * partir da raiz do projeto (onde o arquivo .ipc_key existe). O README
 * documenta isso. Cada sistema recebe um id de projeto distinto para que
 * suas chaves não colidam, mesmo compartilhando o mesmo arquivo-âncora.
 */
#define IPC_KEY_PATH        "./.ipc_key"   /* arquivo-âncora (raiz do projeto) */

#define PROJ_ID_DYNATHREAD  'D'   /* DynaThreadMaker */
#define PROJ_ID_CENTRALTALK 'C'   /* CentralTalk     (Fase 2) */
#define PROJ_ID_PEERTALK    'P'   /* PeerTalk        (Fase 3) */

/* ------------------------------------------------------------------------- *
 * Convenção de msgtyp (campo `mtype` da mensagem System V)
 * ------------------------------------------------------------------------- *
 * O campo mtype DEVE ser sempre > 0 (exigência do System V).
 *
 *   mtype == MSGTYP_SERVIDOR (1)  -> mensagens destinadas ao servidor.
 *   mtype == PID do cliente       -> mensagens destinadas àquele cliente.
 *
 * Como o PID de um processo é sempre > 1 no Linux (o init é o PID 1, e
 * processos de usuário recebem PIDs maiores), não há conflito entre o tipo
 * reservado para o servidor (1) e os tipos usados para os clientes (PIDs).
 */
#define MSGTYP_SERVIDOR     1L

/* ------------------------------------------------------------------------- *
 * Limites de tamanho de campos (comuns aos sistemas)
 * ------------------------------------------------------------------------- *
 * O enunciado define nomes de usuário com até 20 caracteres. Reservamos +1
 * para o terminador '\0'. Os textos de mensagem têm um limite generoso o
 * suficiente para as mensagens de início/fim das threads e para os textos
 * de chat das fases seguintes.
 */
#define MAX_NOME            21    /* 20 caracteres + terminador '\0' */
#define MAX_TEXTO           256   /* mensagem de texto + terminador  */

/* ========================================================================= *
 *                         SISTEMA DynaThreadMaker
 * ========================================================================= */

/*
 * Tipos de operação que um cliente pode solicitar ao servidor. Vai dentro
 * do corpo da mensagem (campo `op`), NÃO no mtype — o mtype é usado apenas
 * para roteamento (servidor vs. cliente), conforme a convenção acima.
 */
enum dyn_operacao {
    DYN_OP_CRIAR_THREAD = 0,   /* pedido de criação de uma nova thread     */
    DYN_OP_ENCERRAR     = 1    /* pedido de encerramento do servidor       */
};

/*
 * Mensagem CLIENTE -> SERVIDOR (mtype = MSGTYP_SERVIDOR).
 *
 * Em uma fila System V, a mensagem é { long mtype; char mtext[...]; }.
 * Definimos `mtype` como primeiro campo e, em seguida, o corpo. O tamanho
 * passado a msgsnd/msgrcv é sizeof(struct) - sizeof(long) (apenas o corpo).
 *
 * Os campos de criação de thread correspondem 1:1 ao que o enunciado pede:
 * nome, tempo de execução T, mensagem inicial, mensagem final e tamanho N
 * do vetor de inteiros a ser alocado dinamicamente.
 */
struct dyn_msg_requisicao {
    long  mtype;                 /* sempre MSGTYP_SERVIDOR (1) */

    int   op;                    /* enum dyn_operacao */
    pid_t pid_cliente;           /* PID do cliente; usado no mtype da resposta */

    /* Campos válidos somente quando op == DYN_OP_CRIAR_THREAD: */
    char  nome[MAX_NOME];        /* nome da nova thread */
    int   tempo_seg;             /* T: segundos de sleep da thread */
    char  msg_inicial[MAX_TEXTO];/* texto impresso ao iniciar */
    char  msg_final[MAX_TEXTO];  /* texto impresso ao finalizar */
    int   tam_vetor;             /* N: tamanho do vetor de inteiros */
};

/*
 * Mensagem SERVIDOR -> CLIENTE (mtype = PID do cliente).
 *
 * Informa ao cliente o resultado da requisição (sucesso ao criar a thread
 * ou descrição do problema). `sucesso` é 1 em caso de êxito, 0 em erro.
 */
struct dyn_msg_resposta {
    long mtype;                  /* = PID do cliente destino */

    int  sucesso;                /* 1 = ok, 0 = erro */
    char texto[MAX_TEXTO];       /* mensagem legível para exibir ao usuário */
};

/* Tamanho do CORPO da mensagem (o que msgsnd/msgrcv transmitem, sem o mtype). */
#define DYN_REQ_BODY_SIZE  (sizeof(struct dyn_msg_requisicao) - sizeof(long))
#define DYN_RESP_BODY_SIZE (sizeof(struct dyn_msg_resposta)   - sizeof(long))

#endif /* PROTOCOL_H */
