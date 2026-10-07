// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/auth.js — sesión y cuenta: login, logout, /api/me, registro con
// invitación y cambio de contraseña.
// El límite de intentos (rateLimited/recordFailure/clearFailures) y
// currentGmLevel se quedan inyectados desde app.js porque los usan también
// otros dominios (help, characters/search, moderación…), no sólo éste.
import { verifyPassword } from '../srp6.js';
import { clearSessionCookie, createSession, sessionCookie } from '../session.js';
import { AccountError, buildCredentials } from '../accounts.js';
import { hashInviteCode } from '../invites.js';

export function registerAuthRoutes(app, {
  authDb, config, panelTable, requireAuth, requireCsrfHeader, noStore,
  rateLimited, recordFailure, clearFailures, currentGmLevel, audit, clientKey,
}) {
  app.post('/api/login', async (request, response, next) => {
    noStore(response);
    if (rateLimited(request)) return response.status(429).json({ error: 'Demasiados intentos. Espera 15 minutos.' });
    const username = typeof request.body?.username === 'string' ? request.body.username.trim() : '';
    const password = typeof request.body?.password === 'string' ? request.body.password : '';
    if (!username || username.length > 32 || !password || password.length > 128) {
      recordFailure(request);
      return response.status(400).json({ error: 'Usuario o contraseña no válidos' });
    }

    try {
      const [rows] = await authDb.execute(
        'SELECT id, username, salt, verifier, locked, last_ip FROM account WHERE username = ? LIMIT 1',
        [username],
      );
      const account = rows[0];
      const ipLocked = account && Number(account.locked) !== 0 && account.last_ip !== request.ip;
      if (!account || ipLocked || !verifyPassword(account.username, password, account.salt, account.verifier)) {
        recordFailure(request);
        return response.status(401).json({ error: 'Usuario o contraseña incorrectos' });
      }
      clearFailures(request);
      const token = createSession(account, config.sessionSecret, config.sessionTtl);
      response.setHeader('Set-Cookie', sessionCookie(token, config.sessionTtl, config.cookieSecure));
      const gmlevel = await currentGmLevel(account.id);
      response.json({ user: { username: account.username, gmlevel, isGm: gmlevel > 0 } });
    } catch (error) {
      next(error);
    }
  });

  app.post('/api/logout', (_request, response) => {
    noStore(response);
    response.setHeader('Set-Cookie', clearSessionCookie(config.cookieSecure));
    response.status(204).end();
  });

  app.get('/api/me', requireAuth, async (request, response, next) => {
    noStore(response);
    try {
      const gmlevel = await currentGmLevel(request.session.sub);
      response.json({ user: { username: request.session.username, gmlevel, isGm: gmlevel > 0 } });
    } catch (error) {
      next(error);
    }
  });

  // ── Registro con invitación de un solo uso ─────────────────────────────
  app.post('/api/register', async (request, response, next) => {
    noStore(response);
    if (rateLimited(request)) return response.status(429).json({ error: 'Demasiados intentos. Espera 15 minutos.' });

    const inviteCode = typeof request.body?.inviteCode === 'string' ? request.body.inviteCode.trim() : '';
    if (!inviteCode || inviteCode.length > 64) {
      recordFailure(request);
      return response.status(400).json({ error: 'Clave de invitación no válida' });
    }

    let credentials;
    try {
      credentials = buildCredentials(request.body?.username, request.body?.password);
    } catch (error) {
      if (error instanceof AccountError) return response.status(400).json({ error: error.message });
      return next(error);
    }

    const codeHash = hashInviteCode(inviteCode);
    let connection;
    try {
      connection = await authDb.getConnection();
      await connection.beginTransaction();

      const [inviteRows] = await connection.execute(
        `SELECT code_hash FROM ${panelTable('panel_invite')}
         WHERE code_hash = ? AND used_at IS NULL AND (expires_at IS NULL OR expires_at > NOW())
         FOR UPDATE`,
        [codeHash],
      );
      if (!inviteRows.length) {
        await connection.rollback();
        recordFailure(request);
        return response.status(400).json({ error: 'La clave de invitación no existe, ya se usó o ha caducado' });
      }

      const [existingRows] = await connection.execute('SELECT id FROM account WHERE username = ? LIMIT 1', [credentials.username]);
      if (existingRows.length) {
        await connection.rollback();
        return response.status(409).json({ error: 'Ya existe una cuenta con ese nombre' });
      }

      const [insertResult] = await connection.execute(
        'INSERT INTO account (username, salt, verifier, expansion) VALUES (?, ?, ?, 2)',
        [credentials.username, credentials.salt, credentials.verifier],
      );
      const newAccountId = insertResult.insertId;
      await connection.execute(
        'INSERT IGNORE INTO realmcharacters (realmid, acctid, numchars) SELECT id, ?, 0 FROM realmlist',
        [newAccountId],
      );
      await connection.execute(
        `UPDATE ${panelTable('panel_invite')} SET used_by_account = ?, used_at = NOW() WHERE code_hash = ? AND used_at IS NULL`,
        [newAccountId, codeHash],
      );
      await connection.commit();
      clearFailures(request);

      await audit({
        actorAccount: newAccountId, actorName: credentials.username, action: 'register',
        target: credentials.username, detail: 'cuenta creada con invitación', result: 'ok', ip: clientKey(request),
      });

      const token = createSession({ id: newAccountId, username: credentials.username }, config.sessionSecret, config.sessionTtl);
      response.setHeader('Set-Cookie', sessionCookie(token, config.sessionTtl, config.cookieSecure));
      response.status(201).json({ user: { username: credentials.username, gmlevel: 0, isGm: false } });
    } catch (error) {
      if (connection) await connection.rollback().catch(() => {});
      if (error?.code === 'ER_DUP_ENTRY') return response.status(409).json({ error: 'Ya existe una cuenta con ese nombre' });
      next(error);
    } finally {
      if (connection) connection.release();
    }
  });

  // ── Mi cuenta: cambio de contraseña ─────────────────────────────────────
  app.post('/api/account/password', requireAuth, requireCsrfHeader, async (request, response, next) => {
    noStore(response);
    const currentPassword = typeof request.body?.currentPassword === 'string' ? request.body.currentPassword : '';
    try {
      const [rows] = await authDb.execute('SELECT username, salt, verifier FROM account WHERE id = ? LIMIT 1', [request.session.sub]);
      const account = rows[0];
      if (!account || !verifyPassword(account.username, currentPassword, account.salt, account.verifier)) {
        return response.status(401).json({ error: 'La contraseña actual no es correcta' });
      }
      const credentials = buildCredentials(account.username, request.body?.newPassword);
      await authDb.execute('UPDATE account SET salt = ?, verifier = ? WHERE id = ?', [credentials.salt, credentials.verifier, request.session.sub]);
      await audit({
        actorAccount: request.session.sub, actorName: request.session.username, action: 'password-change',
        target: account.username, detail: null, result: 'ok', ip: clientKey(request),
      });
      response.status(204).end();
    } catch (error) {
      if (error instanceof AccountError) return response.status(400).json({ error: error.message });
      next(error);
    }
  });
}
