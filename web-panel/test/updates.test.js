// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { config } from '../src/config.js';
import { createSession, sessionCookie } from '../src/session.js';
import { checkAddonCatalog, checkRepositories, parseLockContent, UpdateCheckError } from '../src/updates.js';

const PINNED = '0123456789abcdef0123456789abcdef01234567';
const REMOTE = '89abcdef0123456789abcdef0123456789abcdef';
const LOCK_ROW = `mod-example\tmaster\t${PINNED}\t2026-09-01\thttps://github.com/example/mod-example.git`;

test('parseLockContent ignora comentarios y lee las cinco columnas del lock', () => {
  const rows = parseLockContent(`# comentario\n\n${LOCK_ROW}\n`, 'versions.lock');
  assert.deepEqual(rows, [{
    name: 'mod-example', branch: 'master', pinnedCommit: PINNED,
    pinnedDate: '2026-09-01', url: 'https://github.com/example/mod-example.git',
  }]);
});

test('parseLockContent rechaza URLs y ramas que git no debe recibir', () => {
  assert.throws(
    () => parseLockContent(LOCK_ROW.replace('https://github.com/', 'file:///'), 'versions.lock'),
    UpdateCheckError,
  );
  assert.throws(
    () => parseLockContent(LOCK_ROW.replace('\tmaster\t', '\t--upload-pack=bad\t'), 'versions.lock'),
    UpdateCheckError,
  );
});

test('checkRepositories distingue al día, actualización y remoto inaccesible', async () => {
  const base = parseLockContent(LOCK_ROW);
  const repositories = [
    ...base,
    { ...base[0], name: 'nuevo' },
    { ...base[0], name: 'caido' },
  ];
  const result = await checkRepositories(repositories, async ({ name }) => {
    if (name === 'caido') throw new Error('sin red');
    return name === 'nuevo' ? REMOTE : PINNED;
  });
  assert.deepEqual(result.map(({ status }) => status), ['current', 'update', 'unreachable']);
  assert.match(result[1].compareUrl, new RegExp(`${PINNED}\\.\\.\\.${REMOTE}$`));
  assert.equal(result[2].remoteCommit, null);
});

function fakeCatalog(overrides = {}) {
  return {
    sourceUrl: 'https://github.com/NoM0Re/WoW-3.3.5a-Addons',
    sourceCommit: PINNED,
    mirroredAt: '2026-09-07T12:01:32.961Z',
    addons: [
      { id: 'multibot', name: 'MultiBot', sourceUrl: null },
      { id: 'playerbotmanager', name: 'PlayerBotManager', sourceUrl: 'https://github.com/Lichborne-AC/PlayerbotManager' },
      { id: 'questradar', name: 'QuestRadar', sourceUrl: 'https://github.com/warblups/mod-quest-radar' },
      { id: 'guildlevels', name: 'GuildLevels', sourceUrl: 'https://github.com/Old-Man-Warcraft/mod-guild-levels' },
      { id: 'ensidiafails', name: 'EnsidiaFails', sourceUrl: 'https://github.com/NoM0Re/WoW-3.3.5a-Addons' },
      { id: 'auctionator-completo', name: 'Auctionator completo', sourceUrl: 'https://github.com/NoM0Re/WoW-3.3.5a-Addons' },
      { id: 'atlas', name: 'Atlas', sourceUrl: 'https://addons.rising-gods.de/addons/atlas' },
    ],
    ...overrides,
  };
}

test('checkAddonCatalog excluye MultiBot/ServerHelp (propios) y PlayerBotManager/QuestRadar/GuildLevels (seguidos aparte)', async () => {
  const rows = await checkAddonCatalog(fakeCatalog(), { resolveRemote: async () => PINNED });
  assert.deepEqual(rows.map((row) => row.name), ['EnsidiaFails', 'Auctionator completo', 'Atlas']);
  assert.ok(rows.every((row) => row.status === 'current'));
});

test('checkAddonCatalog rechaza un catálogo con sourceUrl o sourceCommit inválidos', async () => {
  await assert.rejects(
    checkAddonCatalog(fakeCatalog({ sourceUrl: 'file:///etc/passwd' })),
    UpdateCheckError,
  );
  await assert.rejects(
    checkAddonCatalog(fakeCatalog({ sourceCommit: 'no-es-un-sha' })),
    UpdateCheckError,
  );
});

test('checkAddonCatalog marca sólo los addons cuyo archivo cambió en src/Addons/', async () => {
  const rows = await checkAddonCatalog(fakeCatalog(), {
    resolveRemote: async () => REMOTE,
    fetchChangedIds: async (url, from, to) => {
      assert.equal(url, 'https://github.com/NoM0Re/WoW-3.3.5a-Addons');
      assert.equal(from, PINNED);
      assert.equal(to, REMOTE);
      return new Set(['ensidiafails']);
    },
  });
  const byName = Object.fromEntries(rows.map((row) => [row.name, row]));
  assert.equal(byName.EnsidiaFails.status, 'update');
  assert.match(byName.EnsidiaFails.compareUrl, new RegExp(`${PINNED}\\.\\.\\.${REMOTE}$`));
  assert.equal(byName.Atlas.status, 'current');
  assert.equal(byName.Atlas.compareUrl, null);
});

test('checkAddonCatalog marca todos como pendientes si no se puede afinar qué archivo cambió', async () => {
  const rows = await checkAddonCatalog(fakeCatalog(), {
    resolveRemote: async () => REMOTE,
    fetchChangedIds: async () => { throw new Error('rate limited'); },
  });
  assert.ok(rows.every((row) => row.status === 'update'));
});

test('checkAddonCatalog marca todos como inaccesibles si no se puede resolver el commit remoto', async () => {
  const rows = await checkAddonCatalog(fakeCatalog(), { resolveRemote: async () => { throw new Error('sin red'); } });
  assert.ok(rows.every((row) => row.status === 'unreachable' && row.remoteCommit === null));
});

function fakePool() {
  return { execute: async () => [[]], query: async () => [[]] };
}

function fakeAuthDb(gmlevel) {
  return { ...fakePool(), execute: async (sql) => (sql.includes('FROM account_access') ? [[{ gmlevel }]] : [[]]) };
}

function cookie() {
  const token = createSession({ id: 1, username: 'ADMIN' }, config.sessionSecret, config.sessionTtl);
  return sessionCookie(token, config.sessionTtl, config.cookieSecure).split(';')[0];
}

async function startApp(gmlevel, checkUpdates = async () => ({ checkedAt: new Date().toISOString(), summary: {}, groups: [] })) {
  const app = createApp({ authDb: fakeAuthDb(gmlevel), panelDb: fakePool(), checkUpdates });
  const server = app.listen(0);
  await new Promise((resolve) => server.once('listening', resolve));
  return { server, base: `http://127.0.0.1:${server.address().port}` };
}

test('la comprobación web exige sesión, GM3 y cabecera CSRF', async (t) => {
  const low = await startApp(2);
  t.after(() => low.server.close());
  const noSession = await fetch(`${low.base}/api/updates/check`, { method: 'POST' });
  assert.equal(noSession.status, 401);
  const noAdmin = await fetch(`${low.base}/api/updates/check`, { method: 'POST', headers: { Cookie: cookie(), 'X-Panel-Request': '1' } });
  assert.equal(noAdmin.status, 403);

  const admin = await startApp(3);
  t.after(() => admin.server.close());
  const noCsrf = await fetch(`${admin.base}/api/updates/check`, { method: 'POST', headers: { Cookie: cookie() } });
  assert.equal(noCsrf.status, 403);
});

test('GM3 obtiene el resultado estructurado de la comprobación', async (t) => {
  const expected = { checkedAt: '2026-09-09T10:00:00.000Z', summary: { total: 1, updates: 1, current: 0, unreachable: 0 }, groups: [] };
  let calls = 0;
  const { server, base } = await startApp(3, async () => { calls += 1; return expected; });
  t.after(() => server.close());
  const response = await fetch(`${base}/api/updates/check`, {
    method: 'POST', headers: { Cookie: cookie(), 'X-Panel-Request': '1', 'Content-Type': 'application/json' }, body: '{}',
  });
  assert.equal(response.status, 200);
  assert.deepEqual(await response.json(), expected);
  assert.equal(calls, 1);
});
