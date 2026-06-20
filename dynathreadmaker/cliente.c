/*
 * cliente.c — Processo CLIENTE (interface de usuário) do DynaThreadMaker.
 *
 * Papel: apresentar um menu ao usuário, coletar os dados de uma nova thread,
 * montar a mensagem de requisição (carimbada com o PRÓPRIO PID) e enviá-la ao
 * servidor pela fila de mensagens. Depois espera a resposta do servidor por um
 * tempo máximo (timeout); se chegar, exibe; se estourar o tempo, avisa o
 * usuário e volta ao menu.
 *
 * O cliente NÃO cria threads nem executa a lógica — isso é responsabilidade do
 * servidor. Aqui só há leitura/validação de entrada e troca de mensagens.
 *
 * Roteamento das mensagens (ver protocol.h):
 *   - requisição enviada com mtype = MSGTYP_SERVIDOR (1);
 *   - resposta esperada  com mtype = PID deste processo.
 */
#include "protocol.h"
#include "ipc_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Tempo máximo (em segundos) que o cliente espera pela resposta do servidor
 * antes de declarar timeout. A criação da thread no servidor é praticamente
 * imediata (o sleep da thread roda em paralelo, não bloqueia a resposta),
 * então 5 s é folgado. */
#define TIMEOUT_RESPOSTA_SEG 5

/* ------------------------------------------------------------------------- *
 * Leitura de entrada — helpers
 * ------------------------------------------------------------------------- */

/*
 * Lê uma linha do teclado para `buf` (até `tam`-1 chars), removendo o '\n'
 * final. Retorna 1 em sucesso, 0 se chegou EOF (Ctrl+D). Usar fgets evita os
 * problemas clássicos de misturar scanf com leitura de strings com espaços.
 */
static int ler_linha(char *buf, size_t tam)
{
    if (fgets(buf, (int) tam, stdin) == NULL) {
        return 0;  /* EOF ou erro de leitura */
    }
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') {
        buf[n - 1] = '\0';
    }
    return 1;
}

/*
 * Lê uma linha e converte para inteiro. Em entrada inválida, devolve `padrao`.
 */
static int ler_inteiro(int padrao)
{
    char linha[64];
    if (!ler_linha(linha, sizeof(linha))) {
        return padrao;
    }
    char *fim = NULL;
    long valor = strtol(linha, &fim, 10);
    if (fim == linha) {       /* nada numérico foi lido */
        return padrao;
    }
    return (int) valor;
}

/* ------------------------------------------------------------------------- *
 * Espera a resposta do servidor com timeout
 * ------------------------------------------------------------------------- *
 * Estratégia (conforme CLAUDE.md): msgrcv com IPC_NOWAIT em laço, dormindo 1 s
 * entre tentativas, até receber a resposta ou estourar o tempo máximo. Mais
 * simples e portável do que usar alarm()+handler, e não corre risco de
 * interferir com outros sinais.
 */
static void esperar_resposta(int msqid, pid_t meu_pid)
{
    struct dyn_msg_resposta resp;

    for (int decorrido = 0; decorrido <= TIMEOUT_RESPOSTA_SEG; decorrido++) {
        ssize_t n = tentar_receber_msg(msqid, &resp, DYN_RESP_BODY_SIZE,
                                       (long) meu_pid);
        if (n >= 0) {
            /* Recebeu a resposta destinada a este cliente. */
            printf("\n>> Resposta do servidor: %s%s\n",
                   resp.sucesso ? "[OK] " : "[ERRO] ", resp.texto);
            return;
        }
        /* n == -1: ainda não chegou nada para este PID; espera e tenta de novo. */
        sleep(1);
    }

    printf("\n>> TIMEOUT: o servidor nao respondeu em %d s. "
           "Verifique se o servidor esta rodando. Voltando ao menu.\n",
           TIMEOUT_RESPOSTA_SEG);
}

/* ------------------------------------------------------------------------- *
 * Monta e envia a requisição de criação de thread
 * ------------------------------------------------------------------------- */
static void solicitar_criar_thread(int msqid, pid_t meu_pid)
{
    struct dyn_msg_requisicao req;
    memset(&req, 0, sizeof(req));
    req.mtype       = MSGTYP_SERVIDOR;     /* destinada ao servidor */
    req.op          = DYN_OP_CRIAR_THREAD;
    req.pid_cliente = meu_pid;             /* para o servidor saber a quem responder */

    printf("Nome da nova thread: ");
    fflush(stdout);
    ler_linha(req.nome, MAX_NOME);

    printf("Tempo de execucao em segundos (T): ");
    fflush(stdout);
    req.tempo_seg = ler_inteiro(0);

    printf("Mensagem impressa no INICIO da thread: ");
    fflush(stdout);
    ler_linha(req.msg_inicial, MAX_TEXTO);

    printf("Mensagem impressa no FIM da thread: ");
    fflush(stdout);
    ler_linha(req.msg_final, MAX_TEXTO);

    printf("Tamanho N do vetor de inteiros a alocar: ");
    fflush(stdout);
    req.tam_vetor = ler_inteiro(0);

    /* Envia a requisição e espera a confirmação (com timeout). */
    enviar_msg(msqid, &req, DYN_REQ_BODY_SIZE);
    printf("Requisicao enviada ao servidor. Aguardando resposta...\n");
    esperar_resposta(msqid, meu_pid);
}

/* ------------------------------------------------------------------------- *
 * Envia a requisição de encerramento do servidor
 * ------------------------------------------------------------------------- */
static void solicitar_encerrar_servidor(int msqid, pid_t meu_pid)
{
    struct dyn_msg_requisicao req;
    memset(&req, 0, sizeof(req));
    req.mtype       = MSGTYP_SERVIDOR;
    req.op          = DYN_OP_ENCERRAR;
    req.pid_cliente = meu_pid;

    enviar_msg(msqid, &req, DYN_REQ_BODY_SIZE);
    printf("Pedido de encerramento enviado ao servidor. Este cliente vai sair.\n");
    printf("(Outros clientes, se existirem, precisam ser encerrados manualmente.)\n");
}

/* ------------------------------------------------------------------------- *
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    pid_t meu_pid = getpid();

    /* Abre a fila JÁ EXISTENTE (sem IPC_CREAT). Se o servidor não estiver no
     * ar, msgget falha e o wrapper encerra com mensagem clara. */
    key_t chave = gerar_chave(IPC_KEY_PATH, PROJ_ID_DYNATHREAD);
    int msqid = abrir_fila(chave, 0);

    printf("=== DynaThreadMaker — Cliente (PID %d) ===\n", (int) meu_pid);

    for (;;) {
        printf("\n----------------------------------------\n");
        printf(" 1) Criar nova thread\n");
        printf(" 2) Encerrar o SERVIDOR (e sair)\n");
        printf(" 0) Sair apenas deste cliente\n");
        printf("Escolha uma opcao: ");
        fflush(stdout);

        char opcao[16];
        if (!ler_linha(opcao, sizeof(opcao))) {
            /* EOF (Ctrl+D): sai apenas deste cliente. */
            printf("\nEntrada encerrada. Saindo deste cliente.\n");
            break;
        }

        if (strcmp(opcao, "1") == 0) {
            solicitar_criar_thread(msqid, meu_pid);
        } else if (strcmp(opcao, "2") == 0) {
            solicitar_encerrar_servidor(msqid, meu_pid);
            break;  /* depois de mandar encerrar, este cliente também sai */
        } else if (strcmp(opcao, "0") == 0) {
            printf("Saindo apenas deste cliente. O servidor continua rodando.\n");
            break;
        } else {
            printf("Opcao invalida. Tente novamente.\n");
        }
    }

    return EXIT_SUCCESS;
}
