/*
 * servidor.c — Processo SERVIDOR do sistema DynaThreadMaker.
 *
 * Papel: ler continuamente a fila de mensagens esperando requisições dos
 * clientes. Para cada requisição de criação de thread, cria uma thread
 * secundária (pthread) que simula a execução pedida pelo usuário (imprime
 * mensagens, dorme T segundos, aloca e soma um vetor). Ao receber uma
 * requisição de encerramento, remove a fila e finaliza.
 *
 * ---------------------------------------------------------------------------
 * O PONTO CENTRAL DE CONCORRÊNCIA (enunciado, Fig. 2):
 *
 * Os argumentos da nova thread são preparados pela thread PRINCIPAL em uma
 * área de memória compartilhada por todas as threads do processo. Se a
 * principal seguir para a próxima requisição e sobrescrever essa área ANTES
 * de a thread secundária recém-criada copiar os argumentos para suas
 * variáveis locais, a secundária leria dados corrompidos (condição de corrida).
 *
 * Solução: a área de argumentos é uma SEÇÃO CRÍTICA protegida por um mutex +
 * uma variável de condição (que funcionam como um semáforo binário de
 * sinalização). O fluxo é:
 *
 *   PRINCIPAL                                 SECUNDÁRIA
 *   ---------                                 ----------
 *   preenche `args` compartilhado
 *   args_copiados = 0
 *   pthread_create(secundária) ----------->   (recebe ponteiro para `args`)
 *   espera (cond) até args_copiados == 1      copia `args` -> variáveis locais
 *                                             args_copiados = 1
 *                          <---------------   sinaliza a principal (cond)
 *   (desbloqueia) responde ao cliente         (segue sozinha: imprime, dorme,
 *   e volta a ler a fila                       soma o vetor, imprime, encerra)
 *
 * Assim a principal só reutiliza a área de argumentos depois que a secundária
 * garantidamente já a copiou — eliminando a corrida.
 * ---------------------------------------------------------------------------
 */
#include "protocol.h"
#include "ipc_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

/* ------------------------------------------------------------------------- *
 * Estado de sincronização da cópia de argumentos
 * ------------------------------------------------------------------------- */

/* Área compartilhada onde a principal coloca os argumentos da próxima thread.
 * É protegida por `mutex`; a secundária a copia para variáveis locais. */
static struct dyn_msg_requisicao args_compartilhados;

/* Mutex que protege `args_compartilhados` e a flag `args_copiados`. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* Condição usada pela secundária para avisar a principal que já copiou os
 * argumentos. A principal espera nessa condição; a flag evita "perder" o
 * sinal e protege contra despertares espúrios (wait sempre em laço). */
static pthread_cond_t  cond  = PTHREAD_COND_INITIALIZER;
static int             args_copiados = 0;

/* id da fila — global para ser acessível pelo handler de sinal. */
static int g_msqid = -1;

/* ------------------------------------------------------------------------- *
 * Tratamento de SIGINT (Ctrl+C)
 * ------------------------------------------------------------------------- *
 * Se o servidor for interrompido com Ctrl+C, precisamos remover a fila para
 * não deixá-la órfã. Mantemos o handler mínimo (apenas chamadas async-signal
 * -safe: write() e _exit()), evitando funções não reentrantes como printf().
 */
static void tratar_sigint(int sig)
{
    (void) sig;
    const char aviso[] = "\n[servidor] SIGINT recebido: removendo a fila e encerrando.\n";
    write(STDOUT_FILENO, aviso, sizeof(aviso) - 1);

    if (g_msqid != -1) {
        /* msgctl é async-signal-safe na prática; remove a fila. */
        msgctl(g_msqid, IPC_RMID, NULL);
    }
    _exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------------- *
 * Rotina da thread secundária
 * ------------------------------------------------------------------------- */
static void *rotina_thread(void *arg)
{
    (void) arg; /* os argumentos vêm da área compartilhada, não por ponteiro */

    /* --- SEÇÃO CRÍTICA: copiar argumentos para variáveis locais --- */
    pthread_mutex_lock(&mutex);

    /* Cópia local: a partir daqui esta thread não depende mais da área
     * compartilhada, que a principal poderá reutilizar com segurança. */
    char nome[MAX_NOME];
    char msg_inicial[MAX_TEXTO];
    char msg_final[MAX_TEXTO];
    int  tempo_seg = args_compartilhados.tempo_seg;
    int  tam_vetor = args_compartilhados.tam_vetor;
    pid_t dono     = args_compartilhados.pid_cliente;

    /* strncpy + terminador explícito: protege contra strings não terminadas. */
    strncpy(nome,        args_compartilhados.nome,        MAX_NOME);
    nome[MAX_NOME - 1] = '\0';
    strncpy(msg_inicial, args_compartilhados.msg_inicial, MAX_TEXTO);
    msg_inicial[MAX_TEXTO - 1] = '\0';
    strncpy(msg_final,   args_compartilhados.msg_final,   MAX_TEXTO);
    msg_final[MAX_TEXTO - 1] = '\0';

    /* Sinaliza à principal que a cópia terminou e libera o mutex. */
    args_copiados = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
    /* --- FIM DA SEÇÃO CRÍTICA --- */

    /* Daqui em diante a thread opera só com suas variáveis locais. */
    printf("[thread \"%s\" | cliente %d] INICIANDO. %s (vou dormir %d s)\n",
           nome, (int) dono, msg_inicial, tempo_seg);
    fflush(stdout);

    if (tempo_seg > 0) {
        sleep((unsigned int) tempo_seg);
    }

    /* Aloca dinamicamente o vetor de N inteiros, inicializa 1..N e soma. */
    if (tam_vetor > 0) {
        int *vetor = malloc((size_t) tam_vetor * sizeof(int));
        if (vetor == NULL) {
            fprintf(stderr, "[thread \"%s\"] falha ao alocar vetor de %d ints\n",
                    nome, tam_vetor);
        } else {
            long long soma = 0;  /* long long evita overflow em N grande */
            for (int i = 0; i < tam_vetor; i++) {
                vetor[i] = i + 1;        /* valores de 1 até N */
                soma += vetor[i];
            }
            printf("[thread \"%s\"] vetor de %d ints (1..%d) -> somatorio = %lld\n",
                   nome, tam_vetor, tam_vetor, soma);
            fflush(stdout);
            free(vetor);
        }
    } else {
        printf("[thread \"%s\"] tamanho de vetor <= 0: nada a alocar.\n", nome);
        fflush(stdout);
    }

    printf("[thread \"%s\" | cliente %d] ENCERRANDO. %s\n",
           nome, (int) dono, msg_final);
    fflush(stdout);

    return NULL;
}

/* ------------------------------------------------------------------------- *
 * Envia a resposta ao cliente (mtype = PID do cliente)
 * ------------------------------------------------------------------------- */
static void responder_cliente(pid_t pid_cliente, int sucesso, const char *texto)
{
    struct dyn_msg_resposta resp;
    memset(&resp, 0, sizeof(resp));
    resp.mtype   = (long) pid_cliente;   /* roteia a resposta para o cliente */
    resp.sucesso = sucesso;
    strncpy(resp.texto, texto, MAX_TEXTO);
    resp.texto[MAX_TEXTO - 1] = '\0';

    enviar_msg(g_msqid, &resp, DYN_RESP_BODY_SIZE);
}

/* ------------------------------------------------------------------------- *
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    /* Instala o handler de SIGINT para limpeza da fila no Ctrl+C. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tratar_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }

    /* Cria/abre a fila de mensagens (servidor é o dono: IPC_CREAT). */
    key_t chave = gerar_chave(IPC_KEY_PATH, PROJ_ID_DYNATHREAD);
    g_msqid = abrir_fila(chave, IPC_CREAT | 0666);

    printf("[servidor] DynaThreadMaker no ar. Fila msqid=%d (chave 0x%x).\n",
           g_msqid, (unsigned int) chave);
    printf("[servidor] Aguardando requisicoes... (Ctrl+C encerra e limpa a fila)\n");
    fflush(stdout);

    /* Loop principal: recebe requisições destinadas ao servidor (mtype 1). */
    for (;;) {
        struct dyn_msg_requisicao req;
        receber_msg(g_msqid, &req, DYN_REQ_BODY_SIZE, MSGTYP_SERVIDOR);

        if (req.op == DYN_OP_ENCERRAR) {
            printf("[servidor] Pedido de encerramento recebido (cliente %d). "
                   "Removendo a fila e saindo.\n", (int) req.pid_cliente);
            fflush(stdout);
            remover_fila(g_msqid);
            break;
        }

        if (req.op != DYN_OP_CRIAR_THREAD) {
            /* Operação desconhecida: responde erro e continua. */
            responder_cliente(req.pid_cliente, 0,
                              "Operacao desconhecida — requisicao ignorada.");
            continue;
        }

        /* --- Preparar a área de argumentos compartilhada (seção crítica) --- */
        pthread_mutex_lock(&mutex);
        args_compartilhados = req;   /* copia a requisição inteira para a área */
        args_copiados = 0;           /* ainda não foi copiada pela secundária  */

        /* Cria a thread secundária. Como ela vai ler a área compartilhada,
         * só liberamos a área (e tratamos a próxima requisição) depois que
         * ela sinalizar que copiou. */
        pthread_t tid;
        int rc = pthread_create(&tid, NULL, rotina_thread, NULL);
        if (rc != 0) {
            /* Falhou em criar a thread: libera o mutex, avisa o cliente e
             * volta ao loop sem esperar sinal (não há thread para sinalizar). */
            pthread_mutex_unlock(&mutex);
            fprintf(stderr, "[servidor] pthread_create falhou: %s\n",
                    strerror(rc));
            responder_cliente(req.pid_cliente, 0,
                              "Servidor nao conseguiu criar a thread.");
            continue;
        }

        /* Não vamos dar join: a thread roda de forma independente. Soltamos
         * seus recursos com detach para não vazar o TCB ao terminar. */
        pthread_detach(tid);

        /* Espera, em laço (protege contra despertar espúrio), até a
         * secundária confirmar a cópia dos argumentos. */
        while (!args_copiados) {
            pthread_cond_wait(&cond, &mutex);
        }
        pthread_mutex_unlock(&mutex);
        /* --- Argumentos já copiados: área livre para a próxima requisição --- */

        /* Responde ao cliente confirmando a criação da thread. */
        responder_cliente(req.pid_cliente, 1,
                          "Thread criada com sucesso pelo servidor.");
    }

    printf("[servidor] Encerrado.\n");
    return EXIT_SUCCESS;
}
