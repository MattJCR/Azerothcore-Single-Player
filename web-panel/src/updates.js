// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { execFile } from 'node:child_process';
import { readFile } from 'node:fs/promises';

const COMMIT_PATTERN = /^[0-9a-f]{40}$/i;
const DATE_PATTERN = /^\d{4}-\d{2}-\d{2}$/;
const BRANCH_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._/-]*$/;
const NAME_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;

export class UpdateCheckError extends Error {}

function validBranch(branch) {
  return BRANCH_PATTERN.test(branch)
    && !branch.includes('..')
    && !branch.includes('//')
    && !branch.endsWith('.')
    && !branch.endsWith('/');
}

function validRepositoryUrl(value) {
  try {
    const url = new URL(value);
    return url.protocol === 'https:'
      && url.hostname.toLowerCase() === 'github.com'
      && !url.username
      && !url.password;
  } catch {
    return false;
  }
}

export function parseLockContent(content, source = 'lock') {
  const repositories = [];
  for (const [index, rawLine] of String(content).split(/\r?\n/).entries()) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    const columns = rawLine.split('\t');
    if (columns.length !== 5) {
      throw new UpdateCheckError(`${source}: formato no válido en la línea ${index + 1}`);
    }
    const [name, branch, commit, date, url] = columns.map((value) => value.trim());
    if (!NAME_PATTERN.test(name) || !validBranch(branch) || !COMMIT_PATTERN.test(commit)
      || !DATE_PATTERN.test(date) || !validRepositoryUrl(url)) {
      throw new UpdateCheckError(`${source}: datos no válidos en la línea ${index + 1}`);
    }
    repositories.push({ name, branch, pinnedCommit: commit.toLowerCase(), pinnedDate: date, url });
  }
  if (!repositories.length) throw new UpdateCheckError(`${source}: no contiene repositorios`);
  return repositories;
}

export function remoteHead(repository, { gitBinary = 'git', timeoutMs = 7_000 } = {}) {
  return new Promise((resolve, reject) => {
    execFile(
      gitBinary,
      ['ls-remote', '--refs', repository.url, `refs/heads/${repository.branch}`],
      { encoding: 'utf8', timeout: timeoutMs, windowsHide: true, maxBuffer: 64 * 1024 },
      (error, stdout) => {
        if (error) return reject(error);
        const commit = String(stdout).trim().split(/\s+/, 1)[0]?.toLowerCase();
        if (!COMMIT_PATTERN.test(commit || '')) return reject(new Error('La rama remota no devolvió un commit'));
        resolve(commit);
      },
    );
  });
}

async function mapWithConcurrency(values, concurrency, mapper) {
  const results = new Array(values.length);
  let nextIndex = 0;
  async function worker() {
    while (nextIndex < values.length) {
      const index = nextIndex;
      nextIndex += 1;
      results[index] = await mapper(values[index], index);
    }
  }
  await Promise.all(Array.from({ length: Math.min(concurrency, values.length) }, worker));
  return results;
}

export async function checkRepositories(repositories, resolveRemote = remoteHead, concurrency = 8) {
  return mapWithConcurrency(repositories, concurrency, async (repository) => {
    const sourceUrl = repository.url.replace(/\.git$/i, '');
    try {
      const remoteCommit = await resolveRemote(repository);
      return {
        name: repository.name,
        branch: repository.branch,
        pinnedCommit: repository.pinnedCommit,
        pinnedDate: repository.pinnedDate,
        remoteCommit,
        status: remoteCommit === repository.pinnedCommit ? 'current' : 'update',
        sourceUrl,
        compareUrl: remoteCommit === repository.pinnedCommit
          ? null
          : `${sourceUrl}/compare/${repository.pinnedCommit}...${remoteCommit}`,
      };
    } catch (error) {
      console.warn(`[${new Date().toISOString()}] no se pudo consultar ${repository.name}: ${error.message}`);
      return {
        name: repository.name,
        branch: repository.branch,
        pinnedCommit: repository.pinnedCommit,
        pinnedDate: repository.pinnedDate,
        remoteCommit: null,
        status: 'unreachable',
        sourceUrl,
        compareUrl: null,
      };
    }
  });
}

function summarize(repositories) {
  const updates = repositories.filter((repository) => repository.status === 'update').length;
  const current = repositories.filter((repository) => repository.status === 'current').length;
  const unreachable = repositories.filter((repository) => repository.status === 'unreachable').length;
  return { total: repositories.length, updates, current, unreachable };
}

// El catálogo grande (~150 addons de terceros) no viene de un repositorio por
// addon como addons.lock, sino de un único mirror en bloque de
// NoM0Re/WoW-3.3.5a-Addons (ver web-panel/addons/README.md): un solo commit
// fijado para todos. Para poder listar cada addon por separado -y no sólo
// "el mirror entero está desactualizado"- se resuelve el commit remoto del
// mirror y, sólo si difiere, se pide a la API compare de GitHub qué archivos
// de src/Addons/ cambiaron; el nombre de archivo se reduce al mismo `id` que
// usa web-panel/tools/sync-addons.mjs (slug del nombre sin extensión, con las
// mismas dos excepciones de Auctionator) para saber qué addon del catálogo es.
// PlayerBotManager, QuestRadar y GuildLevels no entran aquí: cada uno tiene
// su propio repositorio y ya se sigue con precisión total vía addons.lock.
// MultiBot y ServerHelp son propios (sourceUrl null en el catálogo) y
// tampoco forman parte de este repositorio.
const CATALOG_BRANCH = 'main';
const CATALOG_OWN_REPOSITORY_IDS = new Set(['playerbotmanager', 'questradar', 'guildlevels']);
const CATALOG_ID_OVERRIDES = {
  'auctionator+.rar': 'auctionator-completo',
  'auctionator_watchlist.rar': 'auctionator-watchlist',
};

function slugArchivePathToAddonId(archivePath) {
  const filename = archivePath.split('/').pop() || '';
  const override = CATALOG_ID_OVERRIDES[filename.toLowerCase()];
  if (override) return override;
  return filename.replace(/\.(zip|rar)$/i, '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
}

export async function defaultFetchChangedAddonIds(sourceUrl, fromCommit, toCommit) {
  const repositoryPath = sourceUrl.replace(/^https:\/\/github\.com\//i, '');
  const response = await fetch(`https://api.github.com/repos/${repositoryPath}/compare/${fromCommit}...${toCommit}`);
  if (!response.ok) throw new Error(`la API compare de GitHub respondió ${response.status}`);
  const data = await response.json();
  const ids = new Set();
  for (const file of data.files || []) {
    if (file.filename.startsWith('src/Addons/')) ids.add(slugArchivePathToAddonId(file.filename));
  }
  return ids;
}

export async function checkAddonCatalog(catalog, { resolveRemote = remoteHead, fetchChangedIds = defaultFetchChangedAddonIds } = {}) {
  if (!validRepositoryUrl(catalog.sourceUrl) || !COMMIT_PATTERN.test(catalog.sourceCommit || '')) {
    throw new UpdateCheckError('addons/catalog.json: sourceUrl o sourceCommit no válidos');
  }
  const pinnedCommit = catalog.sourceCommit.toLowerCase();
  const pinnedDate = /^\d{4}-\d{2}-\d{2}/.test(catalog.mirroredAt || '') ? catalog.mirroredAt.slice(0, 10) : 'desconocida';
  const url = catalog.sourceUrl;
  const addons = (catalog.addons || []).filter((addon) => addon.sourceUrl !== null && !CATALOG_OWN_REPOSITORY_IDS.has(addon.id));

  let remoteCommit = null;
  let bundleStatus = 'unreachable';
  try {
    remoteCommit = await resolveRemote({ url, branch: CATALOG_BRANCH });
    bundleStatus = remoteCommit === pinnedCommit ? 'current' : 'update';
  } catch (error) {
    console.warn(`[${new Date().toISOString()}] no se pudo consultar el catálogo de addons: ${error.message}`);
  }

  // Si el mirror entero está desactualizado, por defecto se marcan todos como
  // pendientes (conservador: sabemos que algo cambió). Si se puede afinar por
  // archivo, sólo quedan marcados los que de verdad cambiaron.
  let changedIds = null;
  if (bundleStatus === 'update') {
    try {
      changedIds = await fetchChangedIds(url, pinnedCommit, remoteCommit);
    } catch (error) {
      console.warn(`[${new Date().toISOString()}] no se pudo afinar qué addons del catálogo cambiaron: ${error.message}`);
    }
  }
  const compareUrl = bundleStatus === 'update' ? `${url}/compare/${pinnedCommit}...${remoteCommit}` : null;

  return addons.map((addon) => {
    const status = bundleStatus === 'update' && changedIds ? (changedIds.has(addon.id) ? 'update' : 'current') : bundleStatus;
    return {
      name: addon.name,
      branch: CATALOG_BRANCH,
      pinnedCommit,
      pinnedDate,
      remoteCommit,
      status,
      sourceUrl: addon.sourceUrl || url,
      compareUrl: status === 'update' ? compareUrl : null,
    };
  });
}

export function createUpdateChecker({ versionsLockPath, addonsLockPath, addonsCatalogPath, resolveRemote = remoteHead, fetchChangedIds } = {}) {
  return async function checkUpdates() {
    let serverContent;
    let addonContent;
    let catalogContent;
    try {
      [serverContent, addonContent, catalogContent] = await Promise.all([
        readFile(versionsLockPath, 'utf8'),
        readFile(addonsLockPath, 'utf8'),
        readFile(addonsCatalogPath, 'utf8'),
      ]);
    } catch (error) {
      throw new UpdateCheckError(`No se pudieron leer los ficheros de versiones: ${error.message}`);
    }

    let catalog;
    try {
      catalog = JSON.parse(catalogContent);
    } catch (error) {
      throw new UpdateCheckError(`addons/catalog.json: JSON no válido (${error.message})`);
    }

    const [serverRepositories, addonLockRepositories, catalogRepositories] = await Promise.all([
      checkRepositories(parseLockContent(serverContent, 'versions.lock'), resolveRemote),
      checkRepositories(parseLockContent(addonContent, 'addons.lock'), resolveRemote),
      checkAddonCatalog(catalog, { resolveRemote, ...(fetchChangedIds ? { fetchChangedIds } : {}) }),
    ]);
    const addonRepositories = [...addonLockRepositories, ...catalogRepositories]
      .sort((a, b) => a.name.localeCompare(b.name, 'es'));

    const groups = [
      { id: 'server', label: 'Core y módulos del servidor', source: 'versions.lock', repositories: serverRepositories, summary: summarize(serverRepositories) },
      { id: 'addons', label: 'Addons', source: 'addons.lock + addons/catalog.json', repositories: addonRepositories, summary: summarize(addonRepositories) },
    ];
    const all = groups.flatMap((group) => group.repositories);
    return { checkedAt: new Date().toISOString(), summary: summarize(all), groups };
  };
}
