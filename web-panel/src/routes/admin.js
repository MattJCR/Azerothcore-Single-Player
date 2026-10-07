// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/admin.js — administración (GM3): invitaciones de un solo uso,
// alta directa de cuentas, y la comprobación de actualizaciones. extraído de app.js con el mismo patrón de inyección que
// routes/serverConfig.js/botOperations.js. requireAdmin llega ya construido
// desde app.js porque también lo usa routes/serverConfig.js.
import { AccountError, buildCredentials } from '../accounts.js';
import { generateInviteCode, hashInviteCode } from '../invites.js';
import { UpdateCheckError } from '../updates.js';

export function registerAdminRoutes(app, {
  authDb, panelDb, requireAuth, requireAdmin, requireCsrfHeader, noStore, audit, clientKey, checkUpdates,
}) {
  app.get('/api/admin/invites', requireAuth, requireAdmin, async (_request, response, next) => {
    noStore(response);
    try {
      const [rows] = await panelDb.query(
        `SELECT code_hash AS codeHash, created_by AS createdBy, created_at AS createdAt, expires_at AS expiresAt,
                used_by_account AS usedByAccount, used_at AS usedAt, note
         FROM panel_invite ORDER BY created_at DESC LIMIT 100`,
      );
      response.json({
        invites: rows.map((row) => ({
          id: row.codeHash.slice(0, 12), createdBy: row.createdBy, createdAt: row.createdAt, expiresAt: row.expiresAt,
          usedByAccount: row.usedByAccount ? Number(row.usedByAccount) : null, usedAt: row.usedAt, note: row.note,
        })),
      });
    } catch (error) {
      next(error);
    }
  });

  app.post('/api/admin/invites', requireAuth, requireAdmin, requireCsrfHeader, async (request, response, next) => {
    noStore(response);
    const note = typeof request.body?.note === 'string' ? request.body.note.trim().slice(0, 255) : '';
    const expiresInHours = Number.isInteger(request.body?.expiresInHours) && request.body.expiresInHours > 0 ? request.body.expiresInHours : null;
    try {
      const code = generateInviteCode();
      const codeHash = hashInviteCode(code);
      await panelDb.execute(
        'INSERT INTO panel_invite (code_hash, created_by, created_at, expires_at, note) VALUES (?, ?, NOW(), ?, ?)',
        [codeHash, request.session.username, expiresInHours ? new Date(Date.now() + expiresInHours * 3600_000) : null, note || null],
      );
      await audit({
        actorAccount: request.session.sub, actorName: request.session.username, action: 'invite-create',
        target: null, detail: note || null, result: 'ok', ip: clientKey(request),
      });
      response.status(201).json({ code });
    } catch (error) {
      next(error);
    }
  });

  app.post('/api/admin/accounts', requireAuth, requireAdmin, requireCsrfHeader, async (request, response, next) => {
    noStore(response);
    try {
      const credentials = buildCredentials(request.body?.username, request.body?.password);
      const [existingRows] = await authDb.execute('SELECT id FROM account WHERE username = ? LIMIT 1', [credentials.username]);
      if (existingRows.length) return response.status(409).json({ error: 'Ya existe una cuenta con ese nombre' });
      const [insertResult] = await authDb.execute(
        'INSERT INTO account (username, salt, verifier, expansion) VALUES (?, ?, ?, 2)',
        [credentials.username, credentials.salt, credentials.verifier],
      );
      await authDb.execute('INSERT IGNORE INTO realmcharacters (realmid, acctid, numchars) SELECT id, ?, 0 FROM realmlist', [insertResult.insertId]);
      await audit({
        actorAccount: request.session.sub, actorName: request.session.username, action: 'account-create',
        target: credentials.username, detail: 'creada por administrador', result: 'ok', ip: clientKey(request),
      });
      response.status(201).json({ username: credentials.username });
    } catch (error) {
      if (error instanceof AccountError) return response.status(400).json({ error: error.message });
      if (error?.code === 'ER_DUP_ENTRY') return response.status(409).json({ error: 'Ya existe una cuenta con ese nombre' });
      next(error);
    }
  });

  // Misma comparación de commits fijados/remotos que los revisores de tools,
  // pero sin dar al proceso web acceso de escritura a los clones instalados.
  // Una comprobación simultánea se comparte entre pestañas para no
  // multiplicar las conexiones a GitHub por dobles clics o por varios
  // administradores.
  let updateCheckInFlight = null;
  app.post('/api/updates/check', requireAuth, requireAdmin, requireCsrfHeader, async (_request, response, next) => {
    noStore(response);
    try {
      if (!updateCheckInFlight) {
        updateCheckInFlight = Promise.resolve().then(() => checkUpdates()).finally(() => { updateCheckInFlight = null; });
      }
      response.json(await updateCheckInFlight);
    } catch (error) {
      if (error instanceof UpdateCheckError) return response.status(503).json({ error: error.message });
      next(error);
    }
  });
}
