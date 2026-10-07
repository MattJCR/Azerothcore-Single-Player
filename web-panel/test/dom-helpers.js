// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// test/dom-helpers.js — arranca el frontend REAL (public/index.html +
// public/app.js, exactamente como lo carga el navegador) dentro de jsdom.
//
// Antes, sólo se probaban los módulos de public/ que no tocan el DOM
// (map-projection.js): shared.js/players.js/map.js/serverConfig.js hacen
// `$('#algo')` a nivel de módulo y nunca tuvieron una red de pruebas propia
// — un cambio podía desconectar un botón de su manejador, o un id de su
// selector, sin que ninguna prueba lo notara. mountApp() es la "nueva forma
// de probar" para
// la parte de M53/M54/M56/M57 que no depende de datos reales ni de GM en
// vivo: lógica de sondeo, accesibilidad del menú/cabeceras y búsqueda.
//
// No sustituye la comprobación en el panel desplegado que
// hay que hacer para las vistas con datos reales/permisos GM — jsdom no ejecuta un motor de
// layout real (getBoundingClientRect da siempre 0), así que nada de esto
// verifica disposición visual a un ancho concreto.
//
// IMPORTANTE: llamar a mountApp() como mucho UNA VEZ por fichero de test
// (node:test ya aísla cada fichero en su propio proceso, así que eso basta
// para partir de cero). public/app.js importa './shared.js' sin query
// string, así que el módulo de shared.js es un singleton de verdad para todo
// el proceso: un segundo mountApp() en el mismo fichero no crea un segundo
// "app" aislado, vuelve a ejecutar el bootstrap de app.js (login automático,
// listeners) sobre el document viejo y deja el document nuevo sin cablear.
// Dentro de un mismo fichero, cambia el comportamiento de "red" reasignando
// `fetchHandler` (ver setFetchHandler) y usa las funciones exportadas de
// `shared` para llevar la app de un estado a otro.
import { JSDOM } from 'jsdom';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(fileURLToPath(import.meta.url));
const indexHtml = fs.readFileSync(path.join(root, '..', 'public', 'index.html'), 'utf8');

let mounted = false;

// fetchHandler(url, options) sustituye a fetch(): cada test decide qué
// "responde la red" en vez de necesitar un servidor Express real. Devuelve
// { status, body } (status 200 por defecto). setFetchHandler() permite
// cambiar esa respuesta a mitad de fichero sin volver a montar la app.
// lang: idioma del panel ('es' por defecto, como lo esperan las pruebas existentes; jsdom
// anunciaría en-US y el panel arrancaría en inglés).
export async function mountApp({ fetchHandler, lang = 'es' } = {}) {
  if (mounted) throw new Error('mountApp() ya se llamó en este proceso — un segundo mount deja el document nuevo sin los listeners de app.js. Usa setFetchHandler() para cambiar de escenario.');
  mounted = true;

  const dom = new JSDOM(indexHtml, { url: 'http://localhost/', pretendToBeVisual: true });
  const { window } = dom;
  global.window = window;
  window.localStorage.setItem('panel-lang', lang);
  global.document = window.document;
  window.scrollTo = () => {};
  window.requestAnimationFrame = (cb) => setTimeout(cb, 0);
  global.requestAnimationFrame = window.requestAnimationFrame;
  // jsdom no implementa el objeto global CSS (con CSS.escape); todos los
  // navegadores objetivo sí lo hacen, así que esto es un hueco del propio
  // jsdom, no algo que haya que evitar en public/*.js.
  if (!window.CSS) window.CSS = { escape: (value) => String(value).replace(/[^a-zA-Z0-9_ -￿-]/g, (ch) => `\\${ch}`) };
  global.CSS = window.CSS;
  // Otro hueco de jsdom (sin motor de layout real, no implementa scroll):
  // todos los navegadores objetivo sí tienen Element.prototype.scrollIntoView.
  if (!window.Element.prototype.scrollIntoView) window.Element.prototype.scrollIntoView = () => {};

  let currentHandler = fetchHandler;
  global.fetch = async (url, options) => {
    const result = currentHandler ? await currentHandler(String(url), options) : { status: 404, body: { error: 'no configurado en el test' } };
    const status = result.status ?? 200;
    return {
      status,
      ok: status >= 200 && status < 300,
      json: async () => result.body ?? {},
    };
  };

  const shared = await import('../public/shared.js');
  await import('../public/app.js');
  // app.js dispara api('/api/me').then(showApp).catch(showLogin) en cuanto se
  // importa; hay que dejar que esa cadena se resuelva (normalmente a
  // showLogin(), porque el fetchHandler del test no tiene sesión) antes de
  // que el test llame a shared.showApp() a mano — si no, showLogin() llegaría
  // después y borraría el estado que el test acaba de poner.
  await flush();

  return {
    dom,
    window,
    document: window.document,
    shared,
    setFetchHandler: (next) => { currentHandler = next; },
  };
}

// Deja correr la cola de microtareas/timers (fetch simulado, .then/.catch,
// el setTimeout(cb, 0) que hace de requestAnimationFrame) antes de comprobar
// el DOM. `times` en vueltas porque una sola pasada no basta para una cadena
// de varios `await` encadenados (api() → refresh() → render()).
export async function flush(times = 4) {
  for (let i = 0; i < times; i += 1) await new Promise((resolve) => setTimeout(resolve, 0));
}
