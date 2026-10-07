# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Cliente sintético de WoW 3.3.5a (build 12340) para verificar el servidor.

Habla el mismo protocolo que Wow.exe contra authserver y worldserver, sin
interfaz: ver tools/cliente-sintetico/README.md. Todo formato de paquete está
contrastado con el core fijado (mirrors/core@7f12e89ee5f4.tar.gz); la
referencia exacta va en el comentario de cada función.
"""
