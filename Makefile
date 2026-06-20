# Makefile — Trabalho Prático de Sistemas Operacionais (UFCSPA)
#
# Compila os sistemas concorrentes do trabalho. Cada sistema é formado por
# binários separados (servidor/cliente) que linkam o código comum de IPC.
#
# Alvos:
#   make            (= make all) compila tudo que já existe
#   make dynathreadmaker        compila apenas o DynaThreadMaker
#   make clean                  remove binários e objetos
#
# Regra do projeto: tratar todo warning como bug. Por isso -Wall -Wextra.
# -std=c11 fixa o padrão da linguagem; -pthread habilita a libpthread.

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -pthread
# _DEFAULT_SOURCE expõe ftok/sleep e demais funções POSIX/SysV sob -std=c11.
CPPFLAGS := -D_DEFAULT_SOURCE -Icommon
LDFLAGS := -pthread

BIN     := bin

# Objeto comum, linkado por todos os sistemas.
COMMON_SRC := common/ipc_utils.c
COMMON_OBJ := common/ipc_utils.o

# ------------------------------------------------------------------------- #
.PHONY: all clean dirs dynathreadmaker

all: dynathreadmaker

# Garante a existência do diretório de saída antes de linkar.
dirs:
	@mkdir -p $(BIN)

# ------------------------------------------------------------------------- #
# DynaThreadMaker
# ------------------------------------------------------------------------- #
dynathreadmaker: dirs $(BIN)/dyn_servidor $(BIN)/dyn_cliente

$(BIN)/dyn_servidor: dynathreadmaker/servidor.c $(COMMON_OBJ) | dirs
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ dynathreadmaker/servidor.c $(COMMON_OBJ) $(LDFLAGS)

$(BIN)/dyn_cliente: dynathreadmaker/cliente.c $(COMMON_OBJ) | dirs
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ dynathreadmaker/cliente.c $(COMMON_OBJ) $(LDFLAGS)

# ------------------------------------------------------------------------- #
# Objeto comum
# ------------------------------------------------------------------------- #
$(COMMON_OBJ): $(COMMON_SRC) common/ipc_utils.h common/protocol.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $(COMMON_SRC)

# ------------------------------------------------------------------------- #
clean:
	rm -f $(COMMON_OBJ)
	rm -rf $(BIN)
