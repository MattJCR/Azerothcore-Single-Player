#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  lib/notify-gm.sh — Avisa a las cuentas GM con un correo dentro del juego
#
#  Uso:  notify-gm.sh "Asunto" "Cuerpo del mensaje"
#
#  ¿Por qué correo y no un announce? Porque un announce solo lo ve quien esté
#  conectado en ese momento, y estas tareas corren de madrugada. El correo se
#  queda esperando y el cliente avisa con "Tienes correo nuevo" en cuanto el GM
#  entra, que es justo lo que se pedía.
#
#  Se envía por la consola del worldserver a través de la sesión screen, con el
#  comando `send mail`, así que no hace falta tocar la BD a mano ni compilar
#  nada. Si el worldserver no está levantado, no se pierde el aviso: queda
#  anotado en el log y se reintentará en la siguiente pasada.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../config.sh"
source "$SCRIPT_DIR/utils.sh"

SUBJECT="${1:?Falta el asunto}"
BODY="${2:?Falta el cuerpo del mensaje}"

# Las comillas dobles romperían el comando de consola al inyectarlo por screen.
SUBJECT="${SUBJECT//\"/}"
BODY="${BODY//\"/}"

if ! screen -list 2>/dev/null | grep -q "\.${WS_SCREEN}[[:space:]]"; then
    warn "[aviso-gm] El worldserver no está levantado: no se puede enviar el correo."
    warn "[aviso-gm] Asunto pendiente: $SUBJECT"
    exit 1
fi

# Personajes de cuentas con nivel de GM suficiente. Se excluyen los personajes
# de servicio del bot de subastas, que nadie mira.
MAIL_TARGETS=$(mysql_q "
    SELECT c.name
      FROM acore_characters.characters c
      JOIN acore_auth.account_access a ON a.id = c.account
     WHERE a.gmlevel >= ${GM_NOTIFY_MIN_LEVEL:-3}
       AND c.guid < ${AH_BOT_GUID_BASE:-9000001};")

if [ -z "$MAIL_TARGETS" ]; then
    warn "[aviso-gm] Ninguna cuenta con gmlevel >= ${GM_NOTIFY_MIN_LEVEL:-3} tiene personajes."
    exit 1
fi

SENT=0
while IFS= read -r CHAR; do
    [ -n "$CHAR" ] || continue
    screen -S "$WS_SCREEN" -X stuff \
        "send mail ${CHAR} \"${SUBJECT}\" \"${BODY}\"$(printf '\r')" 2>/dev/null || true
    sleep 1
    log "[aviso-gm] Correo enviado a ${CHAR}: ${SUBJECT}"
    SENT=$((SENT + 1))
done <<< "$MAIL_TARGETS"

info "[aviso-gm] $SENT correo(s) enviado(s)."
exit 0
