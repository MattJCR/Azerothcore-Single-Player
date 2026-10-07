// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import express from 'express';
import { ResourceError } from '../resources.js';

// Preparación de recursos desde el cliente del jugador (PUB04-I). Sólo
// administradores; las que escriben exigen además la cabecera anti-CSRF.
export function registerResourceRoutes(app, { requireAuth, requireAdmin, requireCsrfHeader, noStore, audit, clientKey, resources }) {
  const base = '/api/recursos';
  const record = (request, action, target, detail, result = 'ok') => audit({
    actorAccount: request.session.sub, actorName: request.session.username, action, target, detail, result, ip: clientKey(request),
  });

  function handle(action) {
    return async (request, response, next) => {
      try {
        await action(request, response);
      } catch (error) {
        if (error instanceof ResourceError) return response.status(error.status).json({ error: error.message });
        next(error);
      }
    };
  }

  app.get(base, requireAuth, requireAdmin, handle(async (_request, response) => {
    noStore(response);
    response.json({ recursos: resources.status(), trabajo: resources.currentJob() || resources.lastJob() });
  }));

  app.post(`${base}/trabajos`, requireAuth, requireAdmin, requireCsrfHeader, handle(async (request, response) => {
    noStore(response);
    const { idiomas, entradas, regenerar } = request.body || {};
    const result = resources.createJob({
      languages: idiomas, declared: entradas && typeof entradas === 'object' ? entradas : {}, force: Array.isArray(regenerar) ? regenerar : [],
    });
    await record(request, 'recursos-crear', result.job?.id || null, JSON.stringify(Object.keys(result.plan).filter((id) => result.plan[id].necesario)));
    response.status(result.job ? 201 : 200).json({ trabajo: result.job, plan: result.plan });
  }));

  app.get(`${base}/trabajos/:id`, requireAuth, requireAdmin, handle(async (request, response) => {
    noStore(response);
    const job = resources.getJob(request.params.id);
    if (!job) return response.status(404).json({ error: 'Trabajo no encontrado' });
    response.json({ trabajo: job });
  }));

  app.put(`${base}/trabajos/:id/lote`, requireAuth, requireAdmin, requireCsrfHeader,
    express.raw({ type: 'application/zip', limit: '64mb' }),
    handle(async (request, response) => {
      noStore(response);
      if (!Buffer.isBuffer(request.body) || !request.body.length) throw new ResourceError('Falta el lote (application/zip)');
      response.json({ trabajo: await resources.receive(request.params.id, request.body) });
    }));

  app.post(`${base}/trabajos/:id/iniciar`, requireAuth, requireAdmin, requireCsrfHeader, handle(async (request, response) => {
    noStore(response);
    const { job } = resources.start(request.params.id);
    await record(request, 'recursos-iniciar', request.params.id, null);
    response.status(202).json({ trabajo: job });
  }));

  app.delete(`${base}/trabajos/:id`, requireAuth, requireAdmin, requireCsrfHeader, handle(async (request, response) => {
    noStore(response);
    const job = resources.cancel(request.params.id);
    await record(request, 'recursos-cancelar', request.params.id, null);
    response.json({ trabajo: job });
  }));
}
