# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Compila el dispatcher y sus macros reales; comprueba vetos y observadores."""
import pathlib
import subprocess
import sys
import tempfile

core = pathlib.Path(sys.argv[1]) / "src/server/game/Scripting"
text = (core / "ScriptDefines/PlayerScript.cpp").read_text()
start = text.index("bool ScriptMgr::OnPlayerUpdateFishingSkill(")
body = text[start:text.index("\n}", start) + 2]
macros = (core / "ScriptMgrMacros.h").read_text()
macros = macros[macros.index("#define CALL_ENABLED_HOOKS"):macros.index("#define CALL_ENABLED_BOOLEAN_HOOKS_WITH_DEFAULT_FALSE")]
source = r'''
#include <cassert>
#include <cstdint>
#include <vector>
using int32 = int32_t;
struct Player {};
struct PlayerScript {
    bool allowed;
    unsigned calls = 0;
    bool OnPlayerUpdateFishingSkill(Player*, int32, int32, int32, int32) {
        ++calls;
        return allowed;
    }
};
constexpr unsigned PLAYERHOOK_ON_UPDATE_FISHING_SKILL = 0;
template<class T> struct ScriptRegistry {
    inline static std::vector<T*> EnabledHooks[1];
};
struct ScriptMgr { bool OnPlayerUpdateFishingSkill(Player*, int32, int32, int32, int32); };
''' + macros + '\n' + body + r'''
int main() {
    ScriptMgr manager;
    auto& hooks = ScriptRegistry<PlayerScript>::EnabledHooks[0];
    assert(manager.OnPlayerUpdateFishingSkill(nullptr, 1, 1, 1, 100));
    for (bool first : {false, true}) {
        for (bool second : {false, true}) {
            PlayerScript a{first}, b{second};
            hooks = {&a, &b};
            assert(manager.OnPlayerUpdateFishingSkill(nullptr, 1, 1, 1, 100) == (first && second));
            assert(a.calls == 1 && b.calls == 1);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix="sp01-dispatch-") as tmp:
    cpp = pathlib.Path(tmp) / "test.cpp"
    binary = pathlib.Path(tmp) / "test"
    cpp.write_text(source)
    subprocess.run(["c++", "-std=c++17", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Dispatcher de pesca: vacío y cuatro combinaciones de veto OK")
