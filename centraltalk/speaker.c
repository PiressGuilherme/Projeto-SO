/*
 * speaker.c — Processo CLIENTE (interface de usuário) do sistema CentralTalk.
 *
 * Papel: fazer login, ler os comandos do usuário, VALIDAR e separar seus
 * campos, empacotá-los numa requisição (carimbada com o próprio PID) e enviar
 * ao chairman. O speaker NÃO executa a lógica dos comandos — apenas exibe as
 * respostas que o chairman devolve.
 *
 * Roteamento (ver protocol.h):
 *   - requisição: mtype = MSGTYP_SERVIDOR (1);
 *   - resposta  : mtype = PID deste processo (pode vir em várias linhas, a
 *     última com fim = 1).
 */
#include "protocol.h"
#include "ipc_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* Timeout (s) para esperar a resposta do chairman. */
#define TIMEOUT_RESPOSTA_SEG 5

static int   g_msqid;
static pid_t g_pid;

/* ------------------------------------------------------------------------- *
 * Leitura de entrada
 * ------------------------------------------------------------------------- */
static int ler_linha(char *buf, size_t tam)
{
    if (fgets(buf, (int) tam, stdin) == NULL) {
        return 0;
    }
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') {
        buf[n - 1] = '\0';
    }
    return 1;
}

/* ------------------------------------------------------------------------- *
 * Envio da requisição + recebimento da resposta (com timeout)
 * ------------------------------------------------------------------------- *
 * A resposta pode ter várias linhas: lemos até receber uma com fim = 1. O
 * timeout vale para CADA linha; entre linhas o chairman é rápido, então não
 * há risco prático de fragmentação parcial. Retorna 1 se recebeu a resposta
 * completa, 0 em timeout.
 */
static int enviar_e_receber(const struct ct_msg_requisicao *req)
{
    enviar_msg(g_msqid, req, CT_REQ_BODY_SIZE);

    for (;;) {
        struct ct_msg_resposta resp;
        int recebeu = 0;

        for (int s = 0; s <= TIMEOUT_RESPOSTA_SEG; s++) {
            ssize_t n = tentar_receber_msg(g_msqid, &resp, CT_RESP_BODY_SIZE,
                                           (long) g_pid);
            if (n >= 0) { recebeu = 1; break; }
            sleep(1);
        }
        if (!recebeu) {
            printf(">> TIMEOUT: o chairman nao respondeu em %d s. "
                   "Verifique se o servidor esta rodando.\n",
                   TIMEOUT_RESPOSTA_SEG);
            return -1;   /* timeout */
        }

        /* Exibe a linha (prefixo de erro quando sucesso == 0). */
        printf("%s%s\n", resp.sucesso ? "" : "[ERRO] ", resp.texto);
        if (resp.fim) {
            return resp.sucesso;   /* ultima linha: devolve o status final (0/1) */
        }
    }
}

/* ------------------------------------------------------------------------- *
 * Login
 * ------------------------------------------------------------------------- *
 * Pede o nome (sem espaços, <= 20 chars) e tenta logar. Repete enquanto o
 * chairman recusar e o usuário quiser tentar de novo. Retorna 1 se logou.
 */
static int validar_nome(const char *nome)
{
    size_t n = strlen(nome);
    if (n == 0 || n > MAX_NOME - 1) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (isspace((unsigned char) nome[i])) {
            return 0;   /* nome não pode conter espaços em branco */
        }
    }
    return 1;
}

static int fazer_login(void)
{
    for (;;) {
        char nome[MAX_NOME * 2];   /* lê com folga para validar tamanho */
        printf("Nome de usuario (sem espacos, ate %d chars): ", MAX_NOME - 1);
        fflush(stdout);
        if (!ler_linha(nome, sizeof(nome))) {
            return 0;   /* EOF: desiste */
        }
        if (!validar_nome(nome)) {
            printf("Nome invalido (vazio, com espacos ou longo demais). "
                   "Tente novamente.\n");
            continue;
        }

        struct ct_msg_requisicao req;
        memset(&req, 0, sizeof(req));
        req.mtype       = MSGTYP_SERVIDOR;
        req.cmd         = CT_LOGIN;
        req.pid_cliente = g_pid;
        strncpy(req.arg_nome, nome, MAX_NOME);
        req.arg_nome[MAX_NOME - 1] = '\0';

        int r = enviar_e_receber(&req);   /* -1 timeout, 0 recusa, 1 sucesso */
        if (r == 1) {
            return 1;                     /* login aceito */
        }
        if (r == -1) {
            return 0;                     /* timeout: aborta */
        }

        /* r == 0: o chairman recusou (nome em uso ou limite). Pergunta se o
         * usuario quer tentar outro nome. */
        printf("Deseja tentar outro nome? (s/N): ");
        fflush(stdout);
        char resp[16];
        if (!ler_linha(resp, sizeof(resp))) return 0;
        if (resp[0] != 's' && resp[0] != 'S') {
            return 0;                     /* desiste do login */
        }
    }
}

/* ------------------------------------------------------------------------- *
 * Parsing e envio de cada comando
 * ------------------------------------------------------------------------- *
 * O speaker separa os campos do comando digitado e preenche a requisição.
 * Comandos suportados:
 *   send <nome> <texto> | msgs | post <texto> | show | del msgs |
 *   del post <n> | del posts | users | myid | exit | help
 */
static void processar_comando(const char *linha, int *sair)
{
    struct ct_msg_requisicao req;
    memset(&req, 0, sizeof(req));
    req.mtype       = MSGTYP_SERVIDOR;
    req.pid_cliente = g_pid;

    /* Copia editável da linha para tokenizar sem destruir o original. */
    char buf[MAX_TEXTO * 2];
    strncpy(buf, linha, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    /* Primeiro token = nome do comando. */
    char *cmd = strtok(buf, " ");
    if (cmd == NULL) {
        return;   /* linha vazia: ignora */
    }

    if (strcmp(cmd, "send") == 0) {
        /* send <nome> <texto...> — o texto pode conter espaços. */
        char *nome  = strtok(NULL, " ");
        char *texto = strtok(NULL, "");   /* resto da linha */
        if (nome == NULL || texto == NULL) {
            printf("Uso: send <nome-usuario> <texto>\n");
            return;
        }
        while (*texto == ' ') texto++;    /* remove espaços iniciais do texto */
        req.cmd = CT_SEND;
        strncpy(req.arg_nome,  nome,  MAX_NOME);  req.arg_nome[MAX_NOME - 1]   = '\0';
        strncpy(req.arg_texto, texto, MAX_TEXTO); req.arg_texto[MAX_TEXTO - 1] = '\0';

    } else if (strcmp(cmd, "msgs") == 0) {
        req.cmd = CT_MSGS;

    } else if (strcmp(cmd, "post") == 0) {
        char *texto = strtok(NULL, "");
        if (texto == NULL) {
            printf("Uso: post <texto>\n");
            return;
        }
        while (*texto == ' ') texto++;
        req.cmd = CT_POST;
        strncpy(req.arg_texto, texto, MAX_TEXTO); req.arg_texto[MAX_TEXTO - 1] = '\0';

    } else if (strcmp(cmd, "show") == 0) {
        req.cmd = CT_SHOW;

    } else if (strcmp(cmd, "users") == 0) {
        req.cmd = CT_USERS;

    } else if (strcmp(cmd, "myid") == 0) {
        req.cmd = CT_MYID;

    } else if (strcmp(cmd, "del") == 0) {
        /* del msgs | del post <n> | del posts */
        char *sub = strtok(NULL, " ");
        if (sub == NULL) {
            printf("Uso: del msgs | del post <n> | del posts\n");
            return;
        }
        if (strcmp(sub, "msgs") == 0) {
            req.cmd = CT_DEL_MSGS;
        } else if (strcmp(sub, "posts") == 0) {
            req.cmd = CT_DEL_POSTS;
        } else if (strcmp(sub, "post") == 0) {
            char *num = strtok(NULL, " ");
            if (num == NULL) {
                printf("Uso: del post <n>\n");
                return;
            }
            req.cmd     = CT_DEL_POST;
            req.arg_num = atoi(num);
        } else {
            printf("Subcomando de 'del' invalido. Use: msgs | post <n> | posts\n");
            return;
        }

    } else if (strcmp(cmd, "exit") == 0) {
        req.cmd = CT_EXIT;
        *sair = 1;

    } else if (strcmp(cmd, "help") == 0) {
        printf("Comandos: send <nome> <texto> | msgs | post <texto> | show |\n"
               "          del msgs | del post <n> | del posts | users | myid | exit\n");
        return;

    } else {
        printf("Comando desconhecido: \"%s\". Digite 'help' para a lista.\n", cmd);
        return;
    }

    enviar_e_receber(&req);
}

/* ------------------------------------------------------------------------- *
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    g_pid = getpid();

    /* Abre a fila JÁ EXISTENTE (sem IPC_CREAT). Falha clara se o chairman
     * não estiver no ar. */
    key_t chave = gerar_chave(IPC_KEY_PATH, PROJ_ID_CENTRALTALK);
    g_msqid = abrir_fila(chave, 0);

    printf("=== CentralTalk — Speaker (PID %d) ===\n", (int) g_pid);

    if (!fazer_login()) {
        printf("Login nao concluido. Encerrando.\n");
        return EXIT_SUCCESS;
    }

    printf("\nDigite comandos ('help' para a lista, 'exit' para sair).\n");

    int sair = 0;
    while (!sair) {
        printf("\n%d> ", (int) g_pid);
        fflush(stdout);

        char linha[MAX_TEXTO * 2];
        if (!ler_linha(linha, sizeof(linha))) {
            /* EOF: faz logout limpo antes de sair. */
            struct ct_msg_requisicao req;
            memset(&req, 0, sizeof(req));
            req.mtype = MSGTYP_SERVIDOR;
            req.cmd = CT_EXIT;
            req.pid_cliente = g_pid;
            enviar_e_receber(&req);
            break;
        }
        processar_comando(linha, &sair);
    }

    printf("Speaker encerrado.\n");
    return EXIT_SUCCESS;
}
