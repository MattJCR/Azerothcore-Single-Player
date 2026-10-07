# Cómo contribuir

Gracias por querer mejorar el proyecto. Este documento explica qué se acepta, cómo
preparar una contribución y cómo se revisa. Hay una traducción al inglés en
`CONTRIBUTING_EN.md`: **los dos se actualizan juntos**.

## Antes de empezar

- **Una sola línea de desarrollo.** El proyecto tiene una única rama, `main`. Las
  contribuciones llegan como *pull request* contra `main`; no hay ramas de versión.
- **Abre antes una incidencia** si el cambio es grande (un módulo nuevo, tocar el
  instalador, cambiar el comportamiento de los bots). Así se acuerda el enfoque antes
  de que escribas código que luego no encaje.
- **Licencia.** Tu contribución se publica bajo la licencia del proyecto, la **GNU
  Affero General Public License v3.0 o posterior** (`LICENSE`). Los parches que
  modifican un proyecto ajeno conservan la licencia de ese proyecto (`NOTICE`,
  `REUSE.toml`). Al enviarla confirmas que tienes derecho a hacerlo.
- **Qué no entra.** Nada de World of Warcraft de Blizzard (datos, mapas, iconos,
  ficheros del cliente): se genera en la instalación a partir del cliente de cada
  jugador. Tampoco binarios, instantáneas de repositorios de terceros ni datos
  locales (`config.local.sh`, bases de datos, registros).

## Reglas del proyecto

Son las mismas que sigue quien lo mantiene; el detalle está en `REFERENCES.md`
(«La regla de oro», «Escribir un módulo propio: reglas», «Escribir un parche sobre
código de terceros»).

1. **Orden de preferencia.** Una opción de `config.sh` si basta; un `.sql` en
   `patches/` si son filas de base de datos; un `.patch` sólo si hay que cambiar
   código ajeno; un módulo propio en `modules/` para una funcionalidad nueva.
2. **SQL idempotente.** Cada `.sql` borra sus propias filas antes de insertarlas.
3. **Parches con cabecera.** Cada `.patch` empieza con una línea de licencia, el
   problema, la solución y el commit del proyecto ajeno contra el que se escribió.
   Se comprueba que aplica con `bash tools/verificar-parches.sh`.
4. **Identificadores propios** en los rangos de `REFERENCES.md` (por ejemplo,
   `entry` ≥ 600000 para objetos y NPC propios), para no chocar con el core ni con
   los módulos.
5. **Módulos.** Estructura estándar de AzerothCore (`src/*_loader.cpp` y
   `conf/*.conf.dist`); toda opción nueva va al `.conf.dist` y se fija desde
   `config.sh`. El trabajo que toca a bots de otros mapas va en
   `WorldScript::OnUpdate`, nunca en un hook de `PlayerScript`.
6. **Cabecera de licencia en el código propio.** Cada `.sh`, `.py`, `.js`, `.mjs`,
   `.cpp`, `.h` y `.ps1` nuevo lleva las dos líneas SPDX que ya llevan los demás
   (copia la cabecera de cualquier fichero del mismo tipo: copyright del proyecto y licencia `AGPL-3.0-or-later`).
7. **Fin de línea Unix** en scripts, `.tsv`, `.lock` y `.dist` (`.gitattributes`).
8. **Documentación.** Hay cuatro documentos principales: README, INSTALL, REFERENCES y
   CHANGELOG, más este. **README, INSTALL y CONTRIBUTING existen en español e
   inglés y se cambian a la vez**: si tocas uno, toca también su traducción. Todo
   cambio que se vea desde fuera lleva su entrada en `CHANGELOG.md`, con fecha.
9. **Sin datos personales.** Ni contraseñas, ni claves, ni direcciones IP reales, ni
   rutas de tu equipo, ni nombres propios, ni correos en el código, los documentos o los
   mensajes de commit. Usa ejemplos neutros (`192.168.1.100`, `<ruta>`). La
   integración continua lo comprueba con un escáner de secretos.
10. **Lo nuevo se prueba.** Una función que se nota en el juego se comprueba con el
    cliente sintético (`tools/cliente-sintetico/`, ver más abajo) antes de pedir la
    revisión; en el pull request explica cómo lo probaste y qué salió.

## El cliente sintético no se modifica

`tools/cliente-sintetico/` es la vara de medir con la que se comprueba que lo que se
cambia funciona de verdad. Si un pull request pudiera modificar esa herramienta,
podría hacer pasar en falso las comprobaciones. Por eso:

- **No se aceptan cambios sobre lo que ya existe** ahí: ni borrar ni editar líneas,
  ni renombrar ni eliminar ficheros.
- **Sí se aceptan pull requests que sólo añaden comprobaciones nuevas** (un caso nuevo,
  un fichero nuevo, líneas añadidas sin tocar las demás). Van **en un pull request
  aparte**, sin mezclarlas con otros cambios, y se verifican antes de darlas por
  buenas: se ejecuta la herramienta completa con y sin tu aportación, y los casos que
  ya existían tienen que dar exactamente el mismo resultado.
- La integración continua rechaza automáticamente un pull request que borre o
  modifique líneas ahí (`tests/proteger-cliente-sintetico.sh`). Esa comprobación es una
  ayuda, no la garantía: la garantía es la revisión manual descrita abajo.

## Cómo preparar el pull request

1. Haz un *fork* y una rama a partir de `main`.
2. Cambia sólo lo necesario para una cosa. Varios cambios sin relación son varios
   pull requests.
3. Ejecuta lo que corresponda a tu cambio antes de enviarlo (la integración
   continua repite estas comprobaciones):
   - `bash tests/estructura-publica.sh` (estructura, licencias, documentos),
   - `bash -n` sobre los scripts que toques y `python3 -m py_compile` sobre los `.py`,
   - los scripts de `tests/` relacionados con la zona que cambias,
   - `npm test` en `web-panel/` si tocas el panel.
4. Describe en el pull request qué problema resuelve, cómo lo probaste y qué
   documentación actualizaste.

## Cómo se revisa

- **Cada pull request se revisa manualmente, uno a uno**, por quien mantiene el
  proyecto, con ayuda de un asistente de IA como segunda lectura. Ninguno se integra
  de forma automática, aunque la integración continua esté en verde.
- Los cambios en `.github/`, `tests/`, `tools/cliente-sintetico/`, `install.sh`,
  `lib/` y `scripts/` los revisa siempre quien mantiene el proyecto (`CODEOWNERS`).
- **La integración no es un *merge* directo en GitHub.** El desarrollo ocurre en el
  repositorio de trabajo del mantenedor y esta copia pública se genera a partir de
  él. Un pull request aceptado se aplica allí conservando tu autoría y aparece aquí
  en la siguiente publicación; el pull request se cierra enlazando esa publicación.
  Puede tardar un poco; no significa que se haya rechazado.
- Si un cambio no encaja con las reglas de arriba, se explica el motivo y se propone
  cómo ajustarlo. No hay prisa: el proyecto es de uso personal y se mantiene en ratos.
