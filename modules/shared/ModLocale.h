// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * ModLocale.h — los mensajes de los módulos propios, en el idioma del cliente.
 *
 * EL PROBLEMA
 * Los módulos propios escriben al jugador (chat del sistema, avisos de
 * progresión, estado de colas...) siempre en español, aunque el cliente sea
 * enUS. El core ya sabe el idioma de cada sesión (`GetSessionDbcLocale`), pero
 * ningún módulo lo miraba.
 *
 * LA SOLUCIÓN
 * Igual que el panel web: el español es el idioma de origen y sigue
 * escribiéndose en el código; sólo hay que envolverlo:
 *
 *   handler.SendSysMessage(ModLocale::L(handler, "Se revisara tu cola."));
 *   handler.PSendSysMessage(ModLocale::L(handler, "Cola de {}: {}/{} bots."), a, b, c);
 *
 * `L()` mira el idioma real del cliente (no el de los DBC del servidor) y
 * devuelve el propio texto español si es esES/esMX (o si el
 * texto no tiene traducción: nunca queda un mensaje vacío) y la traducción
 * inglesa en cualquier otro caso. La clave es el literal español TAL CUAL
 * (con sus `{}` si es una cadena de formato); la traducción puede reordenar los
 * argumentos con índices (`{1} ... {0}`). Se funciona igual con textos que se
 * eligen en tiempo de ejecución (`L(player, RoleName(bot))`), porque la
 * búsqueda es por contenido.
 *
 * Cada módulo registra su tabla una vez, al cargarse:
 *
 *   static constexpr ModLocale::Entry kEntries[] = { {"es", "en"}, ... };
 *   ModLocale::Register(kEntries);
 *
 * y tests/test_modlocale.py comprueba que cada `L(..., "literal")` del código
 * tiene entrada, que no sobra ninguna y que los `{}` casan entre idiomas.
 *
 * COSTE
 * El camino español es una comparación de enteros. El inglés, una búsqueda en
 * una tabla que sólo se escribe al arrancar (lectura concurrente segura).
 *
 * Misma regla ODR que el resto de shared/: copia byte a byte en cada módulo
 * (la pone el instalador desde modules/shared/, ver install_own_modules en
 * lib/utils.sh). Sube SharedAbi::kSharedAbiVersion al tocar esta cabecera.
 */

#ifndef WOTLK_SP_MOD_LOCALE_H
#define WOTLK_SP_MOD_LOCALE_H

#include "Chat.h"
#include "Common.h"
#include "Player.h"
#include "WorldSession.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ModLocale
{
    struct Entry
    {
        char const* es;
        char const* en;
    };

    // Clave: el literal español (vive en el binario, así que el string_view no
    // se invalida nunca). Se rellena en el arranque, antes de que haya sesiones.
    inline std::unordered_map<std::string_view, char const*>& Table()
    {
        static std::unordered_map<std::string_view, char const*> table;
        return table;
    }

    inline void Register(Entry const* entries, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
            Table().emplace(entries[i].es, entries[i].en);
    }

    template <std::size_t N>
    inline void Register(Entry const (&entries)[N])
    {
        Register(entries, N);
    }

    inline bool IsSpanish(LocaleConstant locale)
    {
        return locale == LOCALE_esES || locale == LOCALE_esMX;
    }

    inline LocaleConstant LocaleOf(LocaleConstant locale) { return locale; }
    // OJO: GetSessionDbcLocale() devuelve el idioma de los DBC que tiene el servidor
    // (cae al esES del servidor si no hay DBC del idioma del cliente), no el del
    // cliente. El idioma real del cliente es GetSessionDbLocaleIndex().
    inline LocaleConstant LocaleOf(WorldSession const* session)
    {
        // Sin sesión (consola, bot sin cliente): el español del proyecto.
        return session ? static_cast<LocaleConstant>(session->GetSessionDbLocaleIndex()) : LOCALE_esES;
    }
    inline LocaleConstant LocaleOf(Player const* player) { return LocaleOf(player ? player->GetSession() : nullptr); }
    inline LocaleConstant LocaleOf(ChatHandler const& handler) { return static_cast<LocaleConstant>(handler.GetSessionDbLocaleIndex()); }
    inline LocaleConstant LocaleOf(ChatHandler const* handler) { return handler ? LocaleOf(*handler) : LOCALE_esES; }

    // Traducción de `es` para `locale`; el propio `es` si el cliente es español o
    // si no hay traducción.
    inline char const* Pick(LocaleConstant locale, char const* es)
    {
        if (IsSpanish(locale))
            return es;
        auto const& table = Table();
        auto it = table.find(std::string_view(es));
        return it == table.end() ? es : it->second;
    }

    template <typename Ctx>
    inline char const* L(Ctx const& ctx, char const* es)
    {
        return Pick(LocaleOf(ctx), es);
    }

    // Para textos ya montados en un std::string (un nombre de rol, un motivo):
    // devuelve una copia, porque el original puede ser temporal.
    template <typename Ctx>
    inline std::string L(Ctx const& ctx, std::string const& es)
    {
        return Pick(LocaleOf(ctx), es.c_str());
    }
}

#endif // WOTLK_SP_MOD_LOCALE_H
