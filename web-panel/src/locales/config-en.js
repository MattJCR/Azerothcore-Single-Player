// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Descripciones en inglés de los parámetros de configuración, por clave del
// .conf (la clave es única en todo el catálogo). Las trozea en tres ficheros
// el tamaño del catálogo; este las une.
import core from './config-core-en.js';
import modulesA from './config-modules-a-en.js';
import modulesB from './config-modules-b-en.js';

export default { ...core, ...modulesA, ...modulesB };
