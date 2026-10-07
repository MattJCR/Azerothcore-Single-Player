// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// routes/botOperations.js — rutas HTTP de "Operaciones de bots"
// (mod-bot-operations, 16/09/2026): extraídas de app.js.
// registerBotOperationsRoutes() recibe sólo lo que necesita — middlewares ya
// construidos, `worldDb` y las utilidades comunes de auditoría/no-cache —,
// nunca la `app` como excusa para tocar cualquier cosa. `createApp()`
// (app.js) sigue siendo el único punto de composición e inyección.
//
// Sólo lee/escribe bot_operations_snapshot y bot_operations_action en
// acore_world (worldDb): nunca interpreta texto de SOAP ni ejecuta comandos
// arbitrarios (ver REFERENCES.md). Como nunca toca SOAP, abrir esta pestaña
// no despierta un worldserver en espera; si mod-bot-operations lleva dormido
// lo mismo que el worldserver, `stale` avisa de que no hay una instantánea
// reciente en vez de mostrar datos viejos como si fueran de ahora mismo.
import { BotOperationsError, validateAction, validateReason } from '../botOperations.js';

const BOT_OPERATIONS_STALE_AFTER_MS = 30_000;
const BOT_OPERATIONS_ACTION_COOLDOWN_MS = 15_000;
const BOT_OPERATIONS_HISTORY_LIMIT = 50;

export function registerBotOperationsRoutes(app, { requireAuth, requireGm, requireGmLevel, requireCsrfHeader, noStore, audit, clientKey, worldDb }) {
  app.get('/api/bot-operations/snapshot', requireAuth, requireGm, async (_request, response, next) => {
    noStore(response);
    try {
      const [rows] = await worldDb.query(
        'SELECT reservations, world_stage, world_pvp, queues, `groups`, quest_mates, guilds, updated_at '
        + 'FROM bot_operations_snapshot WHERE id = 1',
      );
      if (!rows[0]) return response.json({ available: false, stale: true, updatedAt: null });

      const row = rows[0];
      const updatedAt = new Date(row.updated_at);
      response.json({
        available: true,
        stale: Date.now() - updatedAt.getTime() > BOT_OPERATIONS_STALE_AFTER_MS,
        updatedAt: updatedAt.toISOString(),
        reservations: row.reservations,
        worldStage: row.world_stage,
        worldPvp: row.world_pvp,
        queues: row.queues,
        groups: row.groups,
        questMates: row.quest_mates,
        guilds: row.guilds,
      });
    } catch (error) {
      next(error);
    }
  });

  app.get('/api/bot-operations/actions', requireAuth, requireGm, async (_request, response, next) => {
    noStore(response);
    try {
      const [rows] = await worldDb.query(
        'SELECT id, action, param, reason, actor_name, status, result, requested_at, completed_at '
        + 'FROM bot_operations_action ORDER BY id DESC LIMIT ?',
        [BOT_OPERATIONS_HISTORY_LIMIT],
      );
      response.json({
        actions: rows.map((row) => ({
          id: Number(row.id),
          action: row.action,
          param: row.param || null,
          reason: row.reason || null,
          actorName: row.actor_name,
          status: row.status,
          result: row.result,
          requestedAt: row.requested_at,
          completedAt: row.completed_at,
        })),
      });
    } catch (error) {
      next(error);
    }
  });

  app.post(
    '/api/bot-operations/actions',
    requireAuth,
    requireGmLevel(3, 'Se requiere una cuenta de administrador'),
    requireCsrfHeader,
    async (request, response, next) => {
      noStore(response);
      let action; let param; let reason;
      try {
        ({ action, param } = validateAction(request.body?.action, request.body?.param));
        reason = validateReason(request.body?.reason);
      } catch (error) {
        if (error instanceof BotOperationsError) return response.status(400).json({ error: error.message });
        return next(error);
      }
      try {
        // Enfriamiento: una acción igual RESUELTA hace poco se rechaza en vez
        // de repetirla sin más. "status != 'pending'" es a propósito: una
        // duplicada que sigue pendiente no es un enfriamiento, es el mismo
        // trabajo en curso, y la idempotencia de abajo debe ganarle (mismo id,
        // 202) en vez de un 429.
        const [recentRows] = await worldDb.query(
          'SELECT id FROM bot_operations_action WHERE action = ? AND param = ? AND status != \'pending\' '
          + 'AND requested_at > (NOW() - INTERVAL ? SECOND) ORDER BY id DESC LIMIT 1',
          [action, param, Math.ceil(BOT_OPERATIONS_ACTION_COOLDOWN_MS / 1000)],
        );
        if (recentRows[0]) return response.status(429).json({ error: 'Espera unos segundos antes de repetir esta acción.' });

        // Idempotencia real: la clave única generada `pending_key` (ver
        // bot_operations.sql) impide que exista más de una fila 'pending' con
        // la misma acción+param, así que el propio INSERT es la comprobación
        // atómica. Dos peticiones simultáneas ya no pueden colar dos filas
        // (un SELECT previo dejaba una ventana entre comprobar e insertar):
        // la segunda choca contra la restricción y comparte la fila de la
        // primera en vez de duplicar el trabajo.
        try {
          const [insertResult] = await worldDb.execute(
            "INSERT INTO bot_operations_action (action, param, reason, actor_account, actor_name, status, requested_at) "
            + "VALUES (?, ?, ?, ?, ?, 'pending', NOW())",
            [action, param, reason, request.session.sub, request.session.username],
          );
          await audit({
            actorAccount: request.session.sub, actorName: request.session.username, action: 'bot-operations-action',
            target: action, detail: param || reason || null, result: 'ok', ip: clientKey(request),
          });
          return response.status(202).json({ id: insertResult.insertId });
        } catch (error) {
          if (error?.code !== 'ER_DUP_ENTRY') throw error;
          const [pendingRows] = await worldDb.query(
            "SELECT id FROM bot_operations_action WHERE action = ? AND param = ? AND status = 'pending' ORDER BY id DESC LIMIT 1",
            [action, param],
          );
          if (pendingRows[0]) return response.status(202).json({ id: Number(pendingRows[0].id) });
          throw error;
        }
      } catch (error) {
        next(error);
      }
    },
  );
}
