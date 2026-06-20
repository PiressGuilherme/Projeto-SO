/*
 * chairman.c — Processo SERVIDOR do sistema CentralTalk.
 *
 * O chairman é o único processo que executa os comandos dos usuários e mantém
 * todo o estado do sistema:
 *   - lista de usuários logados (nome + PID do speaker);
 *   - tabela de mensagens diretas recebidas por cada usuário;
 *   - fórum público de mensagens.
 *
 * Os clientes `speaker` apenas leem/validam comandos e os enviam pela fila de
 * mensagens. O chairman recebe a requisição (mtype = 1), executa o comando e
 * responde ao speaker (mtype = PID do speaker).
 *
 * MODELO DE CONCORRÊNCIA: o chairman é SEQUENCIAL (single-thread). Ele atende
 * uma requisição por vez num único loop de msgrcv. Como cada comando é
 * processado por completo antes de ler o próximo, NÃO há acesso concorrente às
 * listas — portanto não é preciso mutex aqui. A concorrência do sistema está
 * em haver vários speakers enviando requisições à mesma fila; a serialização
 * natural da fila de mensagens já ordena o atendimento.
 */
#include "protocol.h"
#include "ipc_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>

/* ------------------------------------------------------------------------- *
 * Estruturas de estado (arrays estáticos com limites)
 * ------------------------------------------------------------------------- */

/* Usuário logado: nome + PID do speaker (identificador único enquanto logado).
 * `ativo` marca posições ocupadas do array (slot livre quando ativo == 0). */
struct usuario {
    int   ativo;
    char  nome[MAX_NOME];
    pid_t pid;
    /* Tabela de mensagens diretas recebidas por este usuário. */
    char  msgs[CT_MAX_MSGS_POR_USUARIO][MAX_TEXTO];
    int   n_msgs;
};

static struct usuario usuarios[CT_MAX_USUARIOS];

/* Fórum público de mensagens (compartilhado por todos). */
static char forum[CT_MAX_POSTS][MAX_TEXTO];
static int  n_posts = 0;

/* id da fila — global para o handler de SIGINT poder removê-la. */
static int g_msqid = -1;

/* ------------------------------------------------------------------------- *
 * SIGINT: remove a fila e encerra (handler mínimo, async-signal-safe)
 * ------------------------------------------------------------------------- */
static void tratar_sigint(int sig)
{
    (void) sig;
    const char aviso[] = "\n[chairman] SIGINT: removendo a fila e encerrando.\n";
    write(STDOUT_FILENO, aviso, sizeof(aviso) - 1);
    if (g_msqid != -1) {
        msgctl(g_msqid, IPC_RMID, NULL);
    }
    _exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------------- *
 * Envio de respostas ao speaker
 * ------------------------------------------------------------------------- *
 * Uma resposta pode ter várias linhas. Enviamos uma mensagem por linha; a
 * última leva fim = 1, sinalizando ao speaker que a resposta acabou.
 */
static void enviar_linha(pid_t pid, int sucesso, int fim, const char *texto)
{
    struct ct_msg_resposta resp;
    memset(&resp, 0, sizeof(resp));
    resp.mtype   = (long) pid;
    resp.sucesso = sucesso;
    resp.fim     = fim;
    strncpy(resp.texto, texto, MAX_TEXTO);
    resp.texto[MAX_TEXTO - 1] = '\0';
    enviar_msg(g_msqid, &resp, CT_RESP_BODY_SIZE);
}

/* Atalho para respostas de uma única linha (o caso mais comum). */
static void responder(pid_t pid, int sucesso, const char *texto)
{
    enviar_linha(pid, sucesso, 1, texto);
}

/* ------------------------------------------------------------------------- *
 * Helpers de busca na lista de usuários
 * ------------------------------------------------------------------------- */
static int achar_por_pid(pid_t pid)
{
    for (int i = 0; i < CT_MAX_USUARIOS; i++) {
        if (usuarios[i].ativo && usuarios[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static int achar_por_nome(const char *nome)
{
    for (int i = 0; i < CT_MAX_USUARIOS; i++) {
        if (usuarios[i].ativo && strcmp(usuarios[i].nome, nome) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- *
 * Comandos
 * ------------------------------------------------------------------------- */

static void cmd_login(const struct ct_msg_requisicao *req)
{
    /* Nome único: rejeita se já houver alguém logado com o mesmo nome. */
    if (achar_por_nome(req->arg_nome) != -1) {
        responder(req->pid_cliente, 0,
                  "Login recusado: ja existe um usuario com esse nome.");
        return;
    }
    /* Procura um slot livre. */
    int slot = -1;
    for (int i = 0; i < CT_MAX_USUARIOS; i++) {
        if (!usuarios[i].ativo) { slot = i; break; }
    }
    if (slot == -1) {
        responder(req->pid_cliente, 0,
                  "Login recusado: limite de usuarios logados atingido.");
        return;
    }
    /* Cadastra o usuário. */
    memset(&usuarios[slot], 0, sizeof(struct usuario));
    usuarios[slot].ativo = 1;
    usuarios[slot].pid   = req->pid_cliente;
    strncpy(usuarios[slot].nome, req->arg_nome, MAX_NOME);
    usuarios[slot].nome[MAX_NOME - 1] = '\0';
    usuarios[slot].n_msgs = 0;

    printf("[chairman] login: \"%s\" (PID %d)\n",
           usuarios[slot].nome, (int) req->pid_cliente);
    fflush(stdout);

    responder(req->pid_cliente, 1, "Login efetuado com sucesso.");
}

static void cmd_send(const struct ct_msg_requisicao *req, int idx_origem)
{
    int idx_dest = achar_por_nome(req->arg_nome);
    if (idx_dest == -1) {
        responder(req->pid_cliente, 0,
                  "Erro: usuario de destino nao esta logado.");
        return;
    }
    struct usuario *dest = &usuarios[idx_dest];
    if (dest->n_msgs >= CT_MAX_MSGS_POR_USUARIO) {
        responder(req->pid_cliente, 0,
                  "Erro: caixa de mensagens do destinatario esta cheia.");
        return;
    }
    /* Guarda a mensagem na caixa do destinatário, prefixada pelo remetente.
     * A mensagem só será mostrada quando o destinatário usar `msgs`. */
    char linha[MAX_TEXTO];
    snprintf(linha, sizeof(linha), "de %s: %s",
             usuarios[idx_origem].nome, req->arg_texto);
    strncpy(dest->msgs[dest->n_msgs], linha, MAX_TEXTO);
    dest->msgs[dest->n_msgs][MAX_TEXTO - 1] = '\0';
    dest->n_msgs++;

    responder(req->pid_cliente, 1, "Mensagem enviada.");
}

static void cmd_msgs(const struct ct_msg_requisicao *req, int idx)
{
    struct usuario *u = &usuarios[idx];
    if (u->n_msgs == 0) {
        responder(req->pid_cliente, 1, "Voce nao tem mensagens.");
        return;
    }
    char linha[MAX_TEXTO];
    /* Envia cabeçalho + cada mensagem; a última leva fim = 1. */
    snprintf(linha, sizeof(linha), "Voce tem %d mensagem(ns):", u->n_msgs);
    enviar_linha(req->pid_cliente, 1, 0, linha);
    for (int i = 0; i < u->n_msgs; i++) {
        snprintf(linha, sizeof(linha), "  [%d] %s", i + 1, u->msgs[i]);
        enviar_linha(req->pid_cliente, 1, (i == u->n_msgs - 1), linha);
    }
}

static void cmd_post(const struct ct_msg_requisicao *req, int idx)
{
    if (n_posts >= CT_MAX_POSTS) {
        responder(req->pid_cliente, 0, "Erro: o forum esta cheio.");
        return;
    }
    /* Publica no fórum, prefixado pelo autor. */
    snprintf(forum[n_posts], MAX_TEXTO, "%s: %s",
             usuarios[idx].nome, req->arg_texto);
    n_posts++;
    responder(req->pid_cliente, 1, "Mensagem publicada no forum.");
}

static void cmd_show(const struct ct_msg_requisicao *req)
{
    if (n_posts == 0) {
        responder(req->pid_cliente, 1, "O forum esta vazio.");
        return;
    }
    char linha[MAX_TEXTO];
    enviar_linha(req->pid_cliente, 1, 0, "Forum publico:");
    for (int i = 0; i < n_posts; i++) {
        snprintf(linha, sizeof(linha), "  [%d] %s", i + 1, forum[i]);
        enviar_linha(req->pid_cliente, 1, (i == n_posts - 1), linha);
    }
}

static void cmd_del_msgs(const struct ct_msg_requisicao *req, int idx)
{
    usuarios[idx].n_msgs = 0;
    responder(req->pid_cliente, 1, "Suas mensagens recebidas foram apagadas.");
}

static void cmd_del_post(const struct ct_msg_requisicao *req)
{
    int n = req->arg_num;            /* índice 1-based como exibido no `show` */
    if (n < 1 || n > n_posts) {
        responder(req->pid_cliente, 0, "Erro: indice de post invalido.");
        return;
    }
    /* Remove deslocando os posts seguintes uma posição para trás. */
    for (int i = n - 1; i < n_posts - 1; i++) {
        strncpy(forum[i], forum[i + 1], MAX_TEXTO);
    }
    n_posts--;
    responder(req->pid_cliente, 1, "Post removido do forum.");
}

static void cmd_del_posts(const struct ct_msg_requisicao *req)
{
    n_posts = 0;
    responder(req->pid_cliente, 1, "Todos os posts do forum foram removidos.");
}

static void cmd_users(const struct ct_msg_requisicao *req)
{
    /* Conta logados para decidir a flag `fim` corretamente. */
    int total = 0;
    for (int i = 0; i < CT_MAX_USUARIOS; i++) {
        if (usuarios[i].ativo) total++;
    }
    if (total == 0) {
        responder(req->pid_cliente, 1, "Nenhum usuario logado.");
        return;
    }
    char linha[MAX_TEXTO];
    enviar_linha(req->pid_cliente, 1, 0, "Usuarios logados:");
    int enviados = 0;
    for (int i = 0; i < CT_MAX_USUARIOS; i++) {
        if (!usuarios[i].ativo) continue;
        enviados++;
        snprintf(linha, sizeof(linha), "  %s (PID %d)",
                 usuarios[i].nome, (int) usuarios[i].pid);
        enviar_linha(req->pid_cliente, 1, (enviados == total), linha);
    }
}

static void cmd_myid(const struct ct_msg_requisicao *req, int idx)
{
    char linha[MAX_TEXTO];
    snprintf(linha, sizeof(linha), "Voce e \"%s\" (PID %d).",
             usuarios[idx].nome, (int) usuarios[idx].pid);
    responder(req->pid_cliente, 1, linha);
}

static void cmd_exit(const struct ct_msg_requisicao *req, int idx)
{
    printf("[chairman] logout: \"%s\" (PID %d)\n",
           usuarios[idx].nome, (int) usuarios[idx].pid);
    fflush(stdout);
    /* Libera o slot do usuário (descadastra). */
    usuarios[idx].ativo = 0;
    responder(req->pid_cliente, 1, "Logout efetuado. Ate logo!");
}

/* ------------------------------------------------------------------------- *
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tratar_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }

    key_t chave = gerar_chave(IPC_KEY_PATH, PROJ_ID_CENTRALTALK);
    g_msqid = abrir_fila(chave, IPC_CREAT | 0666);

    printf("[chairman] CentralTalk no ar. Fila msqid=%d (chave 0x%x).\n",
           g_msqid, (unsigned int) chave);
    printf("[chairman] Aguardando speakers... (Ctrl+C encerra e limpa a fila)\n");
    fflush(stdout);

    for (;;) {
        struct ct_msg_requisicao req;
        receber_msg(g_msqid, &req, CT_REQ_BODY_SIZE, MSGTYP_SERVIDOR);

        /* O login é o único comando aceito de quem ainda não está logado. */
        if (req.cmd == CT_LOGIN) {
            cmd_login(&req);
            continue;
        }

        /* Para os demais comandos, o remetente precisa estar logado. */
        int idx = achar_por_pid(req.pid_cliente);
        if (idx == -1) {
            responder(req.pid_cliente, 0,
                      "Voce nao esta logado. Faca login primeiro.");
            continue;
        }

        switch (req.cmd) {
            case CT_SEND:      cmd_send(&req, idx);      break;
            case CT_MSGS:      cmd_msgs(&req, idx);      break;
            case CT_POST:      cmd_post(&req, idx);      break;
            case CT_SHOW:      cmd_show(&req);           break;
            case CT_DEL_MSGS:  cmd_del_msgs(&req, idx);  break;
            case CT_DEL_POST:  cmd_del_post(&req);       break;
            case CT_DEL_POSTS: cmd_del_posts(&req);      break;
            case CT_USERS:     cmd_users(&req);          break;
            case CT_MYID:      cmd_myid(&req, idx);      break;
            case CT_EXIT:      cmd_exit(&req, idx);      break;
            default:
                responder(req.pid_cliente, 0, "Comando desconhecido.");
                break;
        }
    }

    /* (Inalcançável: o encerramento ocorre via SIGINT.) */
    return EXIT_SUCCESS;
}
