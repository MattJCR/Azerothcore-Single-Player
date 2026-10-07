// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/moderation.js — expulsar/silenciar/banear, anuncios, correo de
// objetos y oro, y el listado de sanciones vigentes. Extraído de app.js con el mismo patrón de inyección que
// routes/serverConfig.js/botOperations.js. Cada acción va contra el
// worldserver por SOAP, nunca por SQL directo: es la única vía que afecta a
// una sesión ya conectada y que confirma que la acción se aplicó. Todas
// quedan en panel_audit.
import { SoapError } from '../soap.js';
import {
  CommandError, kickCommand, muteCommand, unmuteCommand, banCommand, unbanCommand,
  announceCommand, sendItemsCommand, sendMoneyCommand,
} from '../commands.js';

export function registerModerationRoutes(app, {
  authDb, charactersDb, soap, requireAuth, requireGmLevel, requireCsrfHeader, noStore, audit, clientKey, currentGmLevel,
}) {
  async function characterAccountId(name) {
    if (typeof name !== 'string' || !name.trim()) return null;
    const [rows] = await charactersDb.execute(
      'SELECT account FROM characters WHERE LOWER(name) = LOWER(?) AND deleteDate IS NULL LIMIT 1',
      [name.trim()],
    );
    return rows[0] ? Number(rows[0].account) : null;
  }

  async function accountIdByUsername(name) {
    if (typeof name !== 'string' || !name.trim()) return null;
    const [rows] = await authDb.execute('SELECT id FROM account WHERE username = ? LIMIT 1', [name.trim()]);
    return rows[0] ? Number(rows[0].id) : null;
  }

  async function banTargetAccountId(body) {
    return body?.type === 'account' ? accountIdByUsername(body?.name) : characterAccountId(body?.name);
  }

  // Registra cada endpoint de moderación: valida jerarquía de rango, arma el
  // comando con la lista blanca de commands.js, lo manda por SOAP y audita.
  function moderationRoute({ path: routePath, action, minGmLevel, resolveTarget, targetLabel, buildCommand }) {
    app.post(
      routePath,
      requireAuth,
      requireGmLevel(minGmLevel, minGmLevel >= 3 ? 'Se requiere una cuenta de administrador' : 'Se requiere una cuenta con rango de moderación'),
      requireCsrfHeader,
      async (request, response, next) => {
        noStore(response);
        const target = targetLabel ? targetLabel(request.body) : null;

        // La validación de la lista blanca corta antes que nada: ni consulta
        // a la base ni llamada SOAP para un parámetro mal formado.
        let command;
        try {
          command = buildCommand(request.body);
        } catch (error) {
          if (error instanceof CommandError) return response.status(400).json({ error: error.message });
          return next(error);
        }

        try {
          if (resolveTarget) {
            const targetAccountId = await resolveTarget(request.body);
            if (targetAccountId === null) return response.status(404).json({ error: 'Objetivo no encontrado' });
            const targetGmLevel = await currentGmLevel(targetAccountId);
            if (targetGmLevel >= request.gmlevel) {
              return response.status(403).json({ error: 'No puedes actuar sobre una cuenta de tu mismo rango o superior' });
            }
          }
          const result = await soap.executeCommand(command);
          await audit({
            actorAccount: request.session.sub, actorName: request.session.username, action,
            target, detail: command, result: 'ok', ip: clientKey(request),
          });
          response.json({ result });
        } catch (error) {
          if (error instanceof SoapError) {
            await audit({
              actorAccount: request.session.sub, actorName: request.session.username, action,
              target, detail: error.message, result: 'error', ip: clientKey(request),
            });
            return response.status(503).json({ error: error.message });
          }
          next(error);
        }
      },
    );
  }

  moderationRoute({
    path: '/api/moderation/kick', action: 'kick', minGmLevel: 1,
    resolveTarget: (body) => characterAccountId(body?.characterName),
    targetLabel: (body) => body?.characterName ?? null,
    buildCommand: (body) => kickCommand({ characterName: body?.characterName, reason: body?.reason }),
  });
  moderationRoute({
    path: '/api/moderation/mute', action: 'mute', minGmLevel: 1,
    resolveTarget: (body) => characterAccountId(body?.characterName),
    targetLabel: (body) => body?.characterName ?? null,
    buildCommand: (body) => muteCommand({ characterName: body?.characterName, duration: body?.duration, reason: body?.reason }),
  });
  moderationRoute({
    path: '/api/moderation/unmute', action: 'unmute', minGmLevel: 1,
    resolveTarget: (body) => characterAccountId(body?.characterName),
    targetLabel: (body) => body?.characterName ?? null,
    buildCommand: (body) => unmuteCommand({ characterName: body?.characterName }),
  });
  moderationRoute({
    path: '/api/moderation/ban', action: 'ban', minGmLevel: 2,
    resolveTarget: banTargetAccountId,
    targetLabel: (body) => body?.name ?? null,
    buildCommand: (body) => banCommand({ type: body?.type, name: body?.name, duration: body?.duration, reason: body?.reason }),
  });
  moderationRoute({
    path: '/api/moderation/unban', action: 'unban', minGmLevel: 2,
    resolveTarget: banTargetAccountId,
    targetLabel: (body) => body?.name ?? null,
    buildCommand: (body) => unbanCommand({ type: body?.type, name: body?.name }),
  });
  moderationRoute({
    path: '/api/moderation/announce', action: 'announce', minGmLevel: 2,
    resolveTarget: null,
    targetLabel: null,
    buildCommand: (body) => announceCommand({ text: body?.text }),
  });
  moderationRoute({
    path: '/api/moderation/send-items', action: 'send-items', minGmLevel: 3,
    resolveTarget: (body) => characterAccountId(body?.characterName),
    targetLabel: (body) => body?.characterName ?? null,
    buildCommand: (body) => sendItemsCommand({
      characterName: body?.characterName, subject: body?.subject, text: body?.text, itemEntry: body?.itemEntry, count: body?.count,
    }),
  });
  moderationRoute({
    path: '/api/moderation/send-money', action: 'send-money', minGmLevel: 3,
    resolveTarget: (body) => characterAccountId(body?.characterName),
    targetLabel: (body) => body?.characterName ?? null,
    buildCommand: (body) => sendMoneyCommand({
      characterName: body?.characterName, subject: body?.subject, text: body?.text, copper: body?.copper,
    }),
  });

  app.get('/api/moderation/sanctions', requireAuth, requireGmLevel(1, 'Se requiere una cuenta con rango de moderación'), async (_request, response, next) => {
    noStore(response);
    try {
      const [accountBans] = await authDb.query(
        `SELECT ab.id, a.username, ab.bandate, ab.unbandate, ab.bannedby, ab.banreason
         FROM account_banned ab JOIN account a ON a.id = ab.id
         WHERE ab.active = 1 ORDER BY ab.bandate DESC LIMIT 100`,
      );
      const [characterBans] = await charactersDb.query(
        `SELECT cb.guid, c.name, cb.bandate, cb.unbandate, cb.bannedby, cb.banreason
         FROM character_banned cb JOIN characters c ON c.guid = cb.guid
         WHERE cb.active = 1 ORDER BY cb.bandate DESC LIMIT 100`,
      );
      const [muted] = await authDb.query(
        'SELECT id, username, mutetime, mutereason, muteby FROM account WHERE mutetime <> 0 ORDER BY mutetime DESC LIMIT 100',
      );
      response.json({
        accountBans: accountBans.map((row) => ({
          id: Number(row.id), username: row.username, banDate: Number(row.bandate), unbanDate: Number(row.unbandate),
          bannedBy: row.bannedby, reason: row.banreason, permanent: Number(row.bandate) === Number(row.unbandate),
        })),
        characterBans: characterBans.map((row) => ({
          guid: Number(row.guid), name: row.name, banDate: Number(row.bandate), unbanDate: Number(row.unbandate),
          bannedBy: row.bannedby, reason: row.banreason, permanent: Number(row.bandate) === Number(row.unbandate),
        })),
        muted: muted.map((row) => ({
          id: Number(row.id), username: row.username, muteUntil: Number(row.mutetime),
          reason: row.mutereason, mutedBy: row.muteby, pending: Number(row.mutetime) < 0,
        })),
      });
    } catch (error) {
      next(error);
    }
  });
}
