// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { execFile } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import { config } from './config.js';

const execFileAsync = promisify(execFile);

// Nombre fijo de la unidad systemd del worldserver (06_systemd.sh la crea con
// este nombre siempre; no es configurable en ningún otro sitio del panel
// tampoco, ver serverStatus.js). Rutas absolutas a los binarios: el sandbox
// del panel (ProtectSystem=strict) los deja ejecutar, pero sin depender del
// PATH que systemd le dé al proceso.
const WORLDSERVER_UNIT = 'ac-worldserver';
const SYSTEMCTL = '/usr/bin/systemctl';
const PS = '/usr/bin/ps';

// PID actual, reinicios acumulados por systemd (systemd ya lo cuenta: nada
// que llevar aparte) y desde cuándo lleva activa la unidad. Sin systemctl
// (entorno de pruebas, otra distro) o con la unidad inexistente, todo queda
// "no observado" en vez de fallar la muestra entera.
export async function getWorldserverUnitInfo() {
  try {
    const { stdout } = await execFileAsync(
      SYSTEMCTL,
      ['show', WORLDSERVER_UNIT, '-p', 'MainPID', '-p', 'NRestarts', '-p', 'ActiveEnterTimestamp', '-p', 'ActiveState'],
    );
    const props = Object.fromEntries(
      stdout.split('\n').filter(Boolean).map((line) => {
        const index = line.indexOf('=');
        return [line.slice(0, index), line.slice(index + 1)];
      }),
    );
    const pid = Number.parseInt(props.MainPID ?? '0', 10);
    const restarts = Number.parseInt(props.NRestarts ?? '', 10);
    return {
      observed: true,
      pid: pid > 0 ? pid : null,
      active: props.ActiveState === 'active',
      restarts: Number.isFinite(restarts) ? restarts : null,
      activeSince: props.ActiveEnterTimestamp && props.ActiveEnterTimestamp !== 'n/a' ? props.ActiveEnterTimestamp : null,
    };
  } catch {
    return { observed: false, pid: null, active: false, restarts: null, activeSince: null };
  }
}

// CPU%, memoria actual (RSS) y de pico, y segundos de vida del proceso. La
// memoria de pico sale de VmHWM (/proc/<pid>/status): el kernel ya la lleva
// por proceso desde que arrancó, así que "el máximo" no necesita un
// recolector propio muestreando de fondo — es justo lo que esta tarea pide
// evitar. null si el proceso no existe (dormido en modo espera, o
// desapareció entre leer el PID y muestrear): "no observado", no un cero.
export async function sampleProcess(pid) {
  if (!pid) return null;
  try {
    const [{ stdout: psOut }, statusRaw] = await Promise.all([
      execFileAsync(PS, ['-o', '%cpu=,rss=,etimes=', '-p', String(pid)]),
      readFile(`/proc/${pid}/status`, 'utf8'),
    ]);
    const parts = psOut.trim().split(/\s+/).map(Number);
    const [cpuPct, rssKb, uptimeSecs] = parts;
    const peakMatch = statusRaw.match(/^VmHWM:\s+(\d+)\s+kB/m);
    return {
      cpuPct: Number.isFinite(cpuPct) ? cpuPct : null,
      memRssKb: Number.isFinite(rssKb) ? rssKb : null,
      memPeakKb: peakMatch ? Number.parseInt(peakMatch[1], 10) : null,
      uptimeSecs: Number.isFinite(uptimeSecs) ? uptimeSecs : null,
    };
  } catch {
    return null;
  }
}

// Avisos de "Tick lento" (modules/shared/SlowTick.h) en el Server.log ACTUAL.
// El core renombra el log anterior al arrancar en vez de truncarlo (ver el
// comentario de fase 5 sobre el appender de registro), así que contar
// líneas del fichero de ahora mismo ya equivale a "desde el último
// arranque", sin necesitar una fecha de corte aparte.
export async function countSlowTicks() {
  if (!config.worldserverLogPath) return { observed: false, count: null };
  try {
    const content = await readFile(config.worldserverLogPath, 'utf8');
    let count = 0;
    for (const line of content.split('\n')) {
      if (line.includes('Tick lento')) count += 1;
    }
    return { observed: true, count };
  } catch {
    return { observed: false, count: null };
  }
}

// Una muestra completa del sistema, tal cual la guarda metricsCache.js. Sin
// PID (worldserver dormido o caído) los campos de proceso quedan "no
// observado" en vez de inventar un cero.
export async function sampleSystem() {
  const unit = await getWorldserverUnitInfo();
  const [process, slowTicks] = await Promise.all([
    sampleProcess(unit.pid),
    countSlowTicks(),
  ]);
  return {
    unit,
    process, // null si no se pudo muestrear
    slowTicks,
  };
}
