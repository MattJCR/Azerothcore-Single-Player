#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
# tools/medir-etapas.sh — mide la población por etapa de mod-world-bots.
# Para usar mientras el usuario entra con Etapasesenta (60), Etapasetenta (70)
# o Darkcore (80). Una línea cada 30 s con lo que importa: hasta qué nivel
# llegan los bots, en qué mapas están, y qué está haciendo la recolocación.
#
#   bash medir-etapas.sh            # 10 minutos
#   bash medir-etapas.sh 20         # 20 minutos
#
# Se ejecuta en la VM (lee Server.log y las dos bases). No toca nada.
# =============================================================================
MIN="${1:-10}"
L=/home/acore/azerothcore/env/dist/bin/Server.log
Q() { mysql -uacore -pacore acore_characters -N -s -e "$1" 2>/dev/null; }

echo
echo "  ETAPAS: medida cada 30 s durante $MIN min (hora del servidor, UTC)"
echo "  Mapas: 0 Reinos del Este, 1 Kalimdor, 530 Terrallende, 571 Rasganorte"
echo
printf "  %-8s | %-46s | %-28s | %s\n" "hora" "jugadores conectados (nivel)" "bots por encima del tope" "de nivel 80: total / en Dalaran"
printf "  %s\n" "---------|------------------------------------------------|------------------------------|-------------------------------"

BASE=$(grep -ac "world-bots\] Etapa " "$L")
for i in $(seq 1 $(( MIN * 2 )) ); do
  T=$(date -u +%H:%M:%S)
  JUG=$(Q "SELECT GROUP_CONCAT(CONCAT(name,' ',level) SEPARATOR ', ') FROM characters WHERE online = 1 AND guid >= 9000200 AND guid <= 9000299")
  [ -z "$JUG" ] && JUG="(ninguno: sin restriccion de etapa)"
  # El tope lo dice el log; si no hay etapa aplicada, 80
  TOPE=$(grep -a "world-bots\] Etapa activa" "$L" | tail -1 | grep -o "hasta el nivel [0-9]*" | grep -o "[0-9]*")
  [ -z "$TOPE" ] && TOPE=80
  SOBRAN=$(Q "SELECT CONCAT(IFNULL(SUM(level > $TOPE AND level < 80),0),' entre $((TOPE+1)) y 79')  FROM characters WHERE online = 1")
  L80=$(Q "SELECT CONCAT(SUM(level=80),' / ',SUM(level=80 AND zone=4395)) FROM characters WHERE online = 1")
  MAPAS=$(Q "SELECT GROUP_CONCAT(CONCAT(map,':',n) ORDER BY n DESC SEPARATOR ' ') FROM (SELECT map, COUNT(*) n FROM characters WHERE online=1 GROUP BY map) t")
  printf "  %-8s | %-46s | %-28s | %s\n" "$T" "${JUG:0:46}" "$SOBRAN" "$L80"
  printf "  %-8s   mapas: %s\n" "" "$MAPAS"
  [ $i -lt $(( MIN * 2 )) ] && sleep 30
done

echo
echo "  LO QUE HA HECHO LA RECOLOCACION (lineas nuevas desde que empezo la medida):"
grep -a "world-bots\] Etapa" "$L" | tail -n +$(( BASE + 1 )) | tail -20 | cut -c1-150
echo
echo "  RESUMEN DEL LOG:"
grep -a "world-bots\] Etapa activa" "$L" | tail -3 | cut -c1-160
for t in "re-aleatorizado" "zona de espera" "bloqueado"; do
  printf "  %-18s %s veces\n" "$t:" "$(grep -ac "world-bots\] Etapa .*$t" "$L")"
done
