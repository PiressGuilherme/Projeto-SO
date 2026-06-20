/*
 * peertalker.c — Processo do sistema PeerTalk (arquitetura peer-to-peer).
 *
 * Diferente do `speaker` do CentralTalk (que só lê comandos e delega tudo a um
 * servidor), o `peertalker` EXECUTA ele mesmo todos os comandos, operando
 * diretamente sobre os recursos IPC compartilhados:
 *
 *   - LISTA DE USUÁRIOS em memória compartilhada (shm), protegida por um
 *     SEMÁFORO binário (mutex). Toda leitura/escrita da lista acontece dentro
 *     de uma seção crítica sem_lock()/sem_unlock(), para evitar corridas em
 *     logins/logouts simultâneos.
 *   - FILA DE MENSAGENS geral: mensagens vão com mtype = PID do destino e
 *     carregam o PID de origem no corpo (comunicação p2p).
 *
 * Os recursos IPC já devem ter sido criados por ./bin/peertalk_init.
 *
 * Comandos: send <nome> <texto> | recv | msgs | del msgs | users | myid | exit
 */
#include "protocol.h"
#include "ipc_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>

/* Recursos IPC (anexados no início). */
static int   g_shmid, g_semid, g_msqid;
static struct pt_lista_compartilhada *g_lista;  /* região compartilhada anexada */
static pid_t g_pid;
static char  g_nome[MAX_NOME];

/* Tabela LOCAL de mensagens recebidas (preenchida por `recv`). */
static char  g_msgs_locais[PT_MAX_MSGS_LOCAIS][MAX_LINHA];
static int   g_n_msgs_locais = 0;

/* Flag para o loop principal saber que deve sair (setada pelo handler). */
static volatile sig_atomic_t g_encerrar = 0;

/* ------------------------------------------------------------------------- *
 * Operações sobre a lista compartilhada (SEMPRE dentro da seção crítica)
 * ------------------------------------------------------------------------- */

/* Procura um usuário pelo nome. Pré-condição: chamador já tem o lock. Retorna
 * o índice na lista, ou -1 se não encontrado. */
static int lista_achar_nome(const char *nome)
{
    for (int i = 0; i < PT_MAX_USUARIOS; i++) {
        if (g_lista->usuarios[i].ativo &&
            strcmp(g_lista->usuarios[i].nome, nome) == 0) {
            return i;
        }
    }
    return -1;
}

/* Remove este peer da lista (logout). Protegido por lock interno. */
static void descadastrar(void)
{
    sem_lock(g_semid);
    for (int i = 0; i < PT_MAX_USUARIOS; i++) {
        if (g_lista->usuarios[i].ativo && g_lista->usuarios[i].pid == g_pid) {
            g_lista->usuarios[i].ativo = 0;
            break;
        }
    }
    sem_unlock(g_semid);
}

/* ------------------------------------------------------------------------- *
 * SIGINT: descadastra e desanexa antes de sair (NÃO remove os recursos IPC —
 * isso é responsabilidade do peertalk_init clean / limpa_ipc.sh).
 * ------------------------------------------------------------------------- *
 * Mantemos o handler curto: apenas sinaliza o loop principal via flag. A
 * limpeza (que usa semáforo e shm, não async-signal-safe) é feita no fluxo
 * normal ao detectar a flag.
 */
static void tratar_sigint(int sig)
{
    (void) sig;
    g_encerrar = 1;
}

/* ------------------------------------------------------------------------- *
 * Login: seção crítica sobre a lista compartilhada
 * ------------------------------------------------------------------------- *
 * Conforme o enunciado: obter acesso exclusivo -> verificar nome duplicado ->
 * se livre, cadastrar (nome + PID) -> liberar. Retorna 1 se logou, 0 se o nome
 * já está em uso ou a lista está cheia.
 */
static int fazer_login(const char *nome)
{
    int ok = 0;

    sem_lock(g_semid);                       /* --- ENTRA na seção crítica --- */

    if (lista_achar_nome(nome) != -1) {
        /* Nome já em uso por outro peer logado. */
        ok = 0;
    } else {
        /* Procura um slot livre e cadastra. */
        int slot = -1;
        for (int i = 0; i < PT_MAX_USUARIOS; i++) {
            if (!g_lista->usuarios[i].ativo) { slot = i; break; }
        }
        if (slot == -1) {
            ok = 0;                          /* lista cheia */
        } else {
            g_lista->usuarios[slot].ativo = 1;
            g_lista->usuarios[slot].pid   = g_pid;
            strncpy(g_lista->usuarios[slot].nome, nome, MAX_NOME);
            g_lista->usuarios[slot].nome[MAX_NOME - 1] = '\0';
            ok = 1;
        }
    }

    sem_unlock(g_semid);                     /* --- SAI da seção crítica ----- */
    return ok;
}

/* ------------------------------------------------------------------------- *
 * Comandos
 * ------------------------------------------------------------------------- */

/* send <nome> <texto>: consulta a lista (seção crítica) para achar o PID do
 * destino e envia a mensagem pela fila (mtype = PID destino, origem no corpo). */
static void cmd_send(const char *nome_dest, const char *texto)
{
    /* Seção crítica curta: só para descobrir o PID do destino. Copiamos o PID
     * e saímos do lock antes de mexer na fila (boa prática: minimizar o tempo
     * de posse do mutex). */
    sem_lock(g_semid);
    int idx = lista_achar_nome(nome_dest);
    pid_t pid_dest = (idx != -1) ? g_lista->usuarios[idx].pid : 0;
    sem_unlock(g_semid);

    if (idx == -1) {
        printf("Erro: usuario \"%s\" nao esta logado.\n", nome_dest);
        return;
    }

    struct pt_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype      = (long) pid_dest;        /* roteia para o destino */
    msg.pid_origem = g_pid;                  /* 1º campo de dados: PID origem */
    strncpy(msg.nome_origem, g_nome, MAX_NOME);
    msg.nome_origem[MAX_NOME - 1] = '\0';
    strncpy(msg.texto, texto, MAX_TEXTO);
    msg.texto[MAX_TEXTO - 1] = '\0';

    enviar_msg(g_msqid, &msg, PT_MSG_BODY_SIZE);
    printf("Mensagem enviada para \"%s\".\n", nome_dest);
}

/* recv: retira da fila TODAS as mensagens com mtype == meu PID e as acumula na
 * tabela local. Não-bloqueante (IPC_NOWAIT): drena o que houver e para. */
static void cmd_recv(void)
{
    int recebidas = 0;
    for (;;) {
        struct pt_msg msg;
        ssize_t n = tentar_receber_msg(g_msqid, &msg, PT_MSG_BODY_SIZE,
                                       (long) g_pid);
        if (n < 0) {
            break;   /* fila sem mais mensagens para este PID */
        }
        if (g_n_msgs_locais >= PT_MAX_MSGS_LOCAIS) {
            printf("Aviso: tabela local cheia; mensagens restantes ficam na fila.\n");
            break;
        }
        /* Guarda no fim da tabela local, identificando o remetente. */
        snprintf(g_msgs_locais[g_n_msgs_locais], MAX_LINHA, "de %s (PID %d): %s",
                 msg.nome_origem, (int) msg.pid_origem, msg.texto);
        g_n_msgs_locais++;
        recebidas++;
    }

    if (recebidas == 0) {
        printf("Nenhuma mensagem nova destinada a voce.\n");
    } else {
        printf("%d mensagem(ns) recebida(s) e guardada(s) localmente. "
               "Use 'msgs' para ver.\n", recebidas);
    }
}

/* msgs: mostra a tabela local de mensagens recebidas. */
static void cmd_msgs(void)
{
    if (g_n_msgs_locais == 0) {
        printf("Voce nao tem mensagens armazenadas. (Use 'recv' para buscar.)\n");
        return;
    }
    printf("Suas mensagens (%d):\n", g_n_msgs_locais);
    for (int i = 0; i < g_n_msgs_locais; i++) {
        printf("  [%d] %s\n", i + 1, g_msgs_locais[i]);
    }
}

/* del msgs: apaga apenas a cópia LOCAL (mensagens ainda na fila não são tocadas). */
static void cmd_del_msgs(void)
{
    g_n_msgs_locais = 0;
    printf("Suas mensagens locais foram apagadas.\n");
}

/* users: lista os usuários logados (seção crítica para ler a lista). */
static void cmd_users(void)
{
    sem_lock(g_semid);
    printf("Usuarios logados:\n");
    int total = 0;
    for (int i = 0; i < PT_MAX_USUARIOS; i++) {
        if (g_lista->usuarios[i].ativo) {
            printf("  %s (PID %d)\n",
                   g_lista->usuarios[i].nome, (int) g_lista->usuarios[i].pid);
            total++;
        }
    }
    sem_unlock(g_semid);
    if (total == 0) {
        printf("  (nenhum)\n");
    }
}

static void cmd_myid(void)
{
    printf("Voce e \"%s\" (PID %d).\n", g_nome, (int) g_pid);
}

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

static int validar_nome(const char *nome)
{
    size_t n = strlen(nome);
    if (n == 0 || n > MAX_NOME - 1) return 0;
    for (size_t i = 0; i < n; i++) {
        if (isspace((unsigned char) nome[i])) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- *
 * Parsing de comandos
 * ------------------------------------------------------------------------- */
static void processar_comando(const char *linha, int *sair)
{
    char buf[MAX_TEXTO * 2];
    strncpy(buf, linha, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    char *cmd = strtok(buf, " ");
    if (cmd == NULL) return;

    if (strcmp(cmd, "send") == 0) {
        char *nome  = strtok(NULL, " ");
        char *texto = strtok(NULL, "");
        if (nome == NULL || texto == NULL) {
            printf("Uso: send <nome-usuario> <texto>\n");
            return;
        }
        while (*texto == ' ') texto++;
        cmd_send(nome, texto);

    } else if (strcmp(cmd, "recv") == 0) {
        cmd_recv();
    } else if (strcmp(cmd, "msgs") == 0) {
        cmd_msgs();
    } else if (strcmp(cmd, "users") == 0) {
        cmd_users();
    } else if (strcmp(cmd, "myid") == 0) {
        cmd_myid();
    } else if (strcmp(cmd, "del") == 0) {
        char *sub = strtok(NULL, " ");
        if (sub != NULL && strcmp(sub, "msgs") == 0) {
            cmd_del_msgs();
        } else {
            printf("Uso: del msgs\n");
        }
    } else if (strcmp(cmd, "exit") == 0) {
        *sair = 1;
    } else if (strcmp(cmd, "help") == 0) {
        printf("Comandos: send <nome> <texto> | recv | msgs | del msgs | "
               "users | myid | exit\n");
    } else {
        printf("Comando desconhecido: \"%s\". Digite 'help'.\n", cmd);
    }
}

/* ------------------------------------------------------------------------- *
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    g_pid = getpid();

    /* Instala SIGINT para sair limpo (descadastrar + desanexar). */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tratar_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }

    /* Anexa os recursos IPC JÁ CRIADOS pelo peertalk_init (sem IPC_CREAT). Se
     * não existirem, os wrappers falham com mensagem clara. */
    key_t k_shm = gerar_chave(IPC_KEY_PATH, PROJ_ID_PEERTALK_SHM);
    key_t k_sem = gerar_chave(IPC_KEY_PATH, PROJ_ID_PEERTALK_SEM);
    key_t k_msg = gerar_chave(IPC_KEY_PATH, PROJ_ID_PEERTALK_MSG);

    g_shmid = criar_shm(k_shm, sizeof(struct pt_lista_compartilhada), 0);
    g_lista = anexar_shm(g_shmid);
    g_semid = criar_sem(k_sem, 0);
    g_msqid = abrir_fila(k_msg, 0);

    printf("=== PeerTalk — peertalker (PID %d) ===\n", (int) g_pid);
    printf("(Os recursos IPC devem ter sido criados por ./bin/peertalk_init)\n");

    /* Login: pede nome até conseguir (ou EOF). */
    for (;;) {
        char nome[MAX_NOME * 2];
        printf("Nome de usuario (sem espacos, ate %d chars): ", MAX_NOME - 1);
        fflush(stdout);
        if (!ler_linha(nome, sizeof(nome))) {
            printf("\nLogin cancelado. Saindo.\n");
            desanexar_shm(g_lista);
            return EXIT_SUCCESS;
        }
        if (!validar_nome(nome)) {
            printf("Nome invalido (vazio, com espacos ou longo demais).\n");
            continue;
        }
        if (fazer_login(nome)) {
            strncpy(g_nome, nome, MAX_NOME);
            g_nome[MAX_NOME - 1] = '\0';
            printf("Login efetuado como \"%s\".\n", g_nome);
            break;
        }
        printf("Nome em uso ou lista cheia. Tente outro nome.\n");
    }

    printf("\nDigite comandos ('help' para a lista, 'exit' para sair).\n");

    int sair = 0;
    while (!sair && !g_encerrar) {
        printf("\n%s> ", g_nome);
        fflush(stdout);

        char linha[MAX_TEXTO * 2];
        if (!ler_linha(linha, sizeof(linha))) {
            break;   /* EOF: encerra (logout no fim) */
        }
        processar_comando(linha, &sair);
    }

    if (g_encerrar) {
        printf("\n[peertalker] SIGINT recebido: fazendo logout.\n");
    }

    /* Logout: descadastra da lista compartilhada e desanexa a shm. NÃO remove
     * os recursos IPC — isso cabe ao peertalk_init clean / limpa_ipc.sh. */
    descadastrar();
    desanexar_shm(g_lista);
    printf("[peertalker] \"%s\" deslogado. Ate logo!\n", g_nome);
    return EXIT_SUCCESS;
}
