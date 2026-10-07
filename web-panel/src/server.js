// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { closeDatabases } from './db.js';
import { config } from './config.js';
import { createApp } from './app.js';

const app = createApp();

const server = app.listen(config.port, config.host, () => {
  console.log(`Panel de AzerothCore escuchando en http://${config.host}:${config.port}`);
});

async function shutdown(signal) {
  console.log(`${signal}: cerrando el panel`);
  clearInterval(app.locals.rateLimitSweep);
  server.close(async () => {
    await closeDatabases();
    process.exit(0);
  });
  setTimeout(() => process.exit(1), 10_000).unref();
}

process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('SIGINT', () => shutdown('SIGINT'));

export { app };
