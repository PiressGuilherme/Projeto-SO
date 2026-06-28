#!/usr/bin/env bash
#
# limpa_ipc.sh — Remove recursos IPC System V órfãos do usuário atual.
#
# Recursos IPC System V (filas de mensagens, memória compartilhada, semáforos)
# sobrevivem ao processo que os criou. Se um servidor é morto com SIGKILL ou
# trava antes de remover seus recursos, eles ficam "órfãos" e aparecem em
# `ipcs`. Reusar a mesma chave depois pode pegar um recurso em estado sujo.
#
# Este script remove TODOS os recursos IPC pertencentes ao usuário corrente —
# útil para limpar entre testes. 
#
# Uso:
#   ./scripts/limpa_ipc.sh          # remove os recursos e mostra o antes/depois
#
set -u

usuario="$(id -un)"

echo "==> Recursos IPC do usuário '$usuario' ANTES da limpeza:"
ipcs

echo
echo "==> Removendo filas de mensagens, memória compartilhada e semáforos de '$usuario'..."

# ipcs -q/-m/-s lista filas/shm/sem. Extraímos a 2ª coluna (id) das linhas
# cujo dono (coluna 3) é o usuário atual, e removemos cada id com ipcrm.
# Cabeçalhos e linhas em branco são descartados pelo teste de coluna 3.

remover() {
    # $1 = flag do ipcs (-q | -m | -s); $2 = flag do ipcrm (-q | -m | -s)
    ipcs "$1" | awk -v u="$usuario" 'NR>3 && $3==u {print $2}' | while read -r id; do
        [ -n "$id" ] && ipcrm "$2" "$id" 2>/dev/null \
            && echo "    removido $2 $id"
    done
}

remover -q -q   # filas de mensagens
remover -m -m   # memória compartilhada
remover -s -s   # semáforos

echo
echo "==> Recursos IPC do usuário '$usuario' DEPOIS da limpeza:"
ipcs
