// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-update-notice (ver ModLocale.h).
 * La clave es el literal español tal como está en mod_update_notice.cpp, con
 * sus códigos de color. Las líneas del informe las escribe
 * tools/revisar-actualizaciones.sh y se muestran tal cual.
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_UPDATE_NOTICE_LOCALE_H
#define WOTLK_SP_UPDATE_NOTICE_LOCALE_H

#include "ModLocale.h"

namespace UpdateNoticeLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "|cffff6060[Actualizaciones]|r No se pudo leer el informe (revisa permisos del fichero).",
          "|cffff6060[Updates]|r The report could not be read (check the file permissions)." },
        { "|cff00ff00[Actualizaciones]|r Todo al dia: ningun repositorio tiene commits nuevos sobre versions.lock.",
          "|cff00ff00[Updates]|r All up to date: no repository has new commits over versions.lock." },
        { "|cffffcc00[Actualizaciones]|r Hay versiones nuevas rio arriba (tools/revisar-actualizaciones.sh):",
          "|cffffcc00[Updates]|r There are new upstream versions (tools/revisar-actualizaciones.sh):" },
        { "|cffffcc00  ... y {} lineas mas (sube UpdateNotice.MaxLines para verlas).|r",
          "|cffffcc00  ... and {} more lines (raise UpdateNotice.MaxLines to see them).|r" },
        { "|cffff6060  Informe del {} ({} dias): la revision semanal puede haber fallado.|r",
          "|cffff6060  Report of {} ({} days old): the weekly check may have failed.|r" },
        { "|cffffcc00  Informe del {}.|r", "|cffffcc00  Report of {}.|r" },
        { "|cffff6060  versions.lock es posterior al informe ({}): puede estar desfasado, "
          "vuelve a ejecutar tools/revisar-actualizaciones.sh.|r",
          "|cffff6060  versions.lock is newer than the report ({}): it may be out of date, "
          "run tools/revisar-actualizaciones.sh again.|r" },
        { "|cffffcc00  El servidor NO se ha tocado. Actualizar: ./install.sh --only 3, --from 4, --freeze.|r",
          "|cffffcc00  The server has NOT been touched. To update: ./install.sh --only 3, --from 4, --freeze.|r" },
        { "mod-update-notice esta desactivado.", "mod-update-notice is disabled." },
        { "No tienes permiso para ver los avisos de actualizacion.", "You do not have permission to see the update notices." },
    };
}

#endif // WOTLK_SP_UPDATE_NOTICE_LOCALE_H
