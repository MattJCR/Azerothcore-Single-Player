#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copia de seguridad completa del servidor: parada segura + bases + confs + logs.
# Uso: bash tools/backup-servidor.sh   (deja ~/backup-<fecha>/; traerlo a otra maquina)
#
# antes usaba credenciales/rutas fijas (acore/acore,
# ~/azerothcore, ~/azerothcore-installer), no comprobaba que "mysqldump | gzip"
# hubiera funcionado de verdad (un mysqldump fallido podía dejar un .sql.gz
# "válido" pero vacío bajo el mismo RUTA=... de éxito) y no incluía la base del
# propio panel (invitaciones, auditoría, configuración pendiente). Ahora
# reutiliza config.sh, escribe primero en una carpeta provisional y sólo la
# publica con un manifiesto si todo terminó bien; los servicios que para
# quedan parados a propósito (uso previsto: mover la copia a otra máquina) y
# se documentan al final, no se reinician solos.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

BK_FINAL="$HOME/backup-$(date +%Y%m%d-%H%M)"
BK="${BK_FINAL}.partial"
rm -rf "$BK"
mkdir -p "$BK"
trap 'rm -rf "$BK"' EXIT

header "Copia de seguridad completa"

# ── Parada segura ────────────────────────────────────────────────────────────
# Best effort por unidad: en modo en espera el worldserver puede llevar ya
# rato dormido, y stop sobre una unidad inactiva no es un fallo.
STOPPED_SERVICES=()
stop_if_active() {
    local UNIT="$1"
    if systemctl is-active --quiet "$UNIT" 2>/dev/null; then
        info "Parando $UNIT…"
        sudo -n systemctl stop "$UNIT" 2>/dev/null || sudo systemctl stop "$UNIT"
        STOPPED_SERVICES+=("$UNIT")
    fi
}

stop_if_active ac-worldserver
stop_if_active ac-authserver
# Modo en espera: ac-worldserver.socket sigue vivo aunque el .service ya esté
# parado y posee el puerto 8085 — una conexión entrante lo despertaría a mitad
# del volcado. Se para también durante la copia y queda documentado para reactivarlo a mano abajo.
if [ "${WORLDSERVER_STANDBY:-false}" = true ]; then
    stop_if_active ac-worldserver.socket
fi

# Comprobar la parada efectiva: seguir con el worldserver a medio cerrar (aún
# escribiendo en las bases) invalidaría el volcado igual que si no se hubiera
# intentado pararlo.
for UNIT in ac-worldserver ac-authserver; do
    if systemctl is-active --quiet "$UNIT" 2>/dev/null; then
        error "No se pudo parar $UNIT (sigue activo). Abortando sin tocar copias anteriores."
        exit 1
    fi
done
info "Parada confirmada: $(systemctl is-active ac-worldserver 2>/dev/null || echo inactive) / $(systemctl is-active ac-authserver 2>/dev/null || echo inactive)"

# ── Volcado de bases ─────────────────────────────────────────────────────────
# Credenciales de config.sh (antes fijas a "acore"/"acore"); MYSQL_PWD evita
# además pasar la contraseña como argumento visible en la lista de procesos.
DUMP_DBS=(acore_auth acore_characters acore_world)
if [ "${INSTALL_MOD_PLAYERBOTS:-false}" = true ]; then
    DUMP_DBS+=(acore_playerbots)
fi
if [ "${INSTALL_WEB_PANEL:-true}" = true ]; then
    DUMP_DBS+=("${PANEL_PANEL_DATABASE:-acore_panel}")
fi

export MYSQL_PWD="$AC_DB_PASS"
FAILED_DBS=()
for db in "${DUMP_DBS[@]}"; do
    OUT="$BK/$db.sql.gz"
    ERR="$BK/$db.mysqldump.err"
    set +e
    mysqldump -u "$AC_DB_USER" --single-transaction --routines --triggers "$db" 2>"$ERR" | gzip -1 > "$OUT"
    DUMP_RC=("${PIPESTATUS[@]}")
    set -e
    # Detectar el fallo del pipeline explícitamente (antes "mysqldump | gzip"
    # con stderr a /dev/null podía dejar un .sql.gz válido pero vacío si
    # mysqldump fallaba y sólo gzip tenía éxito).
    if [ "${DUMP_RC[0]}" -ne 0 ] || [ "${DUMP_RC[1]}" -ne 0 ] || [ ! -s "$OUT" ]; then
        FAILED_DBS+=("$db")
        error "Volcado de $db falló (mysqldump=${DUMP_RC[0]} gzip=${DUMP_RC[1]}; ver $ERR)."
        continue
    fi
    rm -f "$ERR"
    info "  $db -> $(du -h "$OUT" | cut -f1)"
done
unset MYSQL_PWD

if [ "${#FAILED_DBS[@]}" -gt 0 ]; then
    error "Fallaron ${#FAILED_DBS[@]} volcado(s): ${FAILED_DBS[*]}. No se publica la copia parcial de $BK."
    error "Servicios parados que siguen así tras este fallo: ${STOPPED_SERVICES[*]:-ninguno}."
    exit 1
fi

# ── Configuración y estado ──────────────────────────────────────────────────
cp -a "$AC_DIR/env/dist/etc" "$BK/etc" 2>/dev/null || warn "No se pudo copiar $AC_DIR/env/dist/etc"
cp "$SCRIPT_DIR/versions.lock" "$BK/" 2>/dev/null || true
cp "$AC_DIR/env/dist/bin/Server.log" "$BK/" 2>/dev/null || true
crontab -l > "$BK/crontab.txt" 2>/dev/null || true

# ── Manifiesto de copia completa ────────────────────────────────────────────
# Se escribe el último, después de que todo lo de arriba haya terminado sin
# abortar: su presencia es la señal de que la copia está completa. Nada debe
# darla por buena mientras sólo existe "$BK" (todavía ".partial").
{
    echo "fecha=$(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "host=$(hostname)"
    echo "bases_volcadas=${DUMP_DBS[*]}"
    echo "servicios_parados=${STOPPED_SERVICES[*]:-ninguno}"
    echo "--- sha256sum ---"
    (cd "$BK" && find . -type f ! -name MANIFEST.txt -exec sha256sum {} \; | sort)
} > "$BK/MANIFEST.txt"

# ── Publicar de forma atómica ───────────────────────────────────────────────
# El trap de arriba habría borrado "$BK" si algo hubiera fallado antes de este
# punto; el rename deja "$BK_FINAL" listo de un solo golpe, nunca a medias.
mv "$BK" "$BK_FINAL"
trap - EXIT

du -sh "$BK_FINAL"
ls -la "$BK_FINAL"
echo "RUTA=$BK_FINAL"

if [ "${#STOPPED_SERVICES[@]}" -gt 0 ]; then
    warn "Quedan parados a propósito: ${STOPPED_SERVICES[*]}."
    # Una unidad por orden: el NOPASSWD de sudoers (fase 6) sólo está
    # concedido así, dos unidades en la misma línea piden contraseña.
    for UNIT in "${STOPPED_SERVICES[@]}"; do
        warn "  sudo systemctl start $UNIT"
    done
fi
