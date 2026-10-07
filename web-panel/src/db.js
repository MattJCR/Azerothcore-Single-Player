// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import mysql from 'mysql2/promise';
import { config } from './config.js';

const common = {
  host: config.db.host,
  port: config.db.port,
  user: config.db.user,
  password: config.db.password,
  waitForConnections: true,
  connectionLimit: 5,
  maxIdle: 5,
  idleTimeout: 60000,
  enableKeepAlive: true,
  charset: 'utf8mb4',
};

export const authDb = mysql.createPool({ ...common, database: config.db.authDatabase });
export const charactersDb = mysql.createPool({ ...common, database: config.db.charactersDatabase });
export const worldDb = mysql.createPool({ ...common, database: config.db.worldDatabase });
export const panelDb = mysql.createPool({ ...common, database: config.db.panelDatabase });

// Tablas de acore_world referenciadas desde consultas sobre acore_characters
// (equipo, bolsas, banco): mismo servidor MySQL, join entre esquemas.
export const worldTable = (name) => `\`${config.db.worldDatabase}\`.\`${name}\``;

// Tabla de acore_panel referenciada desde una transacción sobre acore_auth
// (registro: consumir la invitación y crear la cuenta son atómicos porque
// ambas tablas viven en el mismo servidor MySQL y se tocan desde la misma
// conexión).
export const panelTable = (name) => `\`${config.db.panelDatabase}\`.\`${name}\``;

export async function closeDatabases() {
  await Promise.all([authDb.end(), charactersDb.end(), worldDb.end(), panelDb.end()]);
}
