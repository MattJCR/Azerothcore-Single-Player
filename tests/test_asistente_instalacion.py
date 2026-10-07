# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("asistente", ROOT / "tools/asistente-instalacion.py")
wizard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wizard)


class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        # Windows busca System32 antes que PATH al crear procesos. Resolver el
        # ejecutable permite probar con Git Bash sin invocar el lanzador WSL.
        if os.name == "nt":
            run = subprocess.run
            bash = shutil.which("bash")
            def resolved_run(command, *args, **kwargs):
                if command[0] == "bash":
                    command = [bash, *command[1:]]
                return run(command, *args, **kwargs)
            patcher = patch.object(subprocess, "run", side_effect=resolved_run)
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_preserves_manual_settings(self):
        original = '# Mi nota\nCUSTOM=99\nREALM_IP=old\n'
        changed = wizard.render_local(original, {"REALM_IP": "192.168.1.77", "REALM_NAME": "Mi reino"})
        self.assertEqual(
            changed.splitlines(),
            ["# Mi nota", "CUSTOM=99", "REALM_IP=192.168.1.77", "REALM_NAME='Mi reino'"],
        )

    def test_new_local_file_gets_header_and_only_changed_keys(self):
        created = wizard.render_local("", {"REALM_IP": "192.168.1.77"})
        self.assertTrue(created.startswith(wizard.LOCAL_HEADER))
        self.assertEqual(created[len(wizard.LOCAL_HEADER):], "REALM_IP=192.168.1.77\n")

    def test_shared_config_is_neutral(self):
        content = (ROOT / "config.sh").read_text(encoding="utf-8")
        self.assertIn('REALM_IP="127.0.0.1"', content)
        self.assertIn('source "$AC_CONFIG_LOCAL"', content)

    def test_reads_effective_bash_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            content = '\n'.join(f'{field}=example' for field in wizard.FIELDS)
            content += '\nAC_HOME=/tmp\nAC_DIR="$AC_HOME/custom-server"\n'
            (root / "config.sh").write_text(content, encoding="utf-8")
            config = wizard.load_config(root)
            self.assertEqual(config["AC_DIR"], "/tmp/custom-server")
            self.assertEqual(config["REALM_NAME"], "example")

    def test_local_file_overrides_shared_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copy(ROOT / "config.sh", root / "config.sh")
            self.assertEqual(wizard.load_config(root)["REALM_IP"], "127.0.0.1")
            (root / "config.local.sh").write_text('REALM_IP="192.168.1.77"\nAC_DIR="/srv/propio"\n', encoding="utf-8")
            config = wizard.load_config(root)
            self.assertEqual(config["REALM_IP"], "192.168.1.77")
            self.assertEqual(config["AC_DIR"], "/srv/propio")

    def test_rejects_ambiguous_definitions(self):
        with self.assertRaises(ValueError):
            wizard.render_local('REALM_IP=one\nREALM_IP=two\n', {"REALM_IP": "192.168.1.77"})

    def test_commented_definition_is_not_a_definition(self):
        changed = wizard.render_local('# REALM_IP=one\n', {"REALM_IP": "192.168.1.77"})
        self.assertEqual(changed.splitlines(), ["# REALM_IP=one", "REALM_IP=192.168.1.77"])

    def test_does_not_accept_arbitrary_setting(self):
        with self.assertRaises(ValueError):
            wizard.render_local("", {"INSTALL_MOD_PLAYERBOTS": "false"})

    def test_input_validation(self):
        for value in ("192.168.1.1", "10.0.0.5"):
            self.assertTrue(wizard.validate("REALM_IP", value))
        for value in ("127.0.0.1", "0.0.0.0", "224.0.0.1", "999.1.1.1", "x'; DROP TABLE x;--"):
            self.assertFalse(wizard.validate("REALM_IP", value))
        self.assertTrue(wizard.validate("REALM_NAME", "Mi reino español"))
        self.assertFalse(wizard.validate("REALM_NAME", "O'Brien"))
        self.assertTrue(wizard.validate("ADMIN_ACCOUNT_PASS", "Prueba12@"))
        for value in ("short", "x" * 17, 'abc"123456', "clave con espacios"):
            self.assertFalse(wizard.validate("ADMIN_ACCOUNT_PASS", value))

    def test_first_save_creates_private_local_file_and_keeps_shared_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "config.sh").write_text('REALM_NAME="Original"\n', encoding="utf-8")
            backup = wizard.save_config(root, "", {"REALM_NAME": "Nuevo reino"})
            self.assertIsNone(backup)
            self.assertIn("REALM_NAME='Nuevo reino'", (root / "config.local.sh").read_text(encoding="utf-8"))
            self.assertEqual((root / "config.sh").read_text(encoding="utf-8"), 'REALM_NAME="Original"\n')
            self.assertFalse((root / ".instalacion").exists())
            if os.name != "nt":
                self.assertEqual((root / "config.local.sh").stat().st_mode & 0o777, 0o600)

    def test_backup_and_atomic_save(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = 'REALM_NAME="Original"\n# Manual\nCUSTOM=99\n'
            (root / "config.local.sh").write_text(original, encoding="utf-8")
            backup = wizard.save_config(root, original, {"REALM_NAME": "Nuevo reino"})
            self.assertEqual(backup.read_text(encoding="utf-8"), original)
            updated = (root / "config.local.sh").read_text(encoding="utf-8")
            self.assertIn("CUSTOM=99", updated)
            self.assertIn("REALM_NAME='Nuevo reino'", updated)
            if os.name != "nt":
                self.assertEqual((root / "config.local.sh").stat().st_mode & 0o777, 0o600)
                self.assertEqual(backup.stat().st_mode & 0o777, 0o600)

    def test_concurrent_edit_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "config.local.sh").write_text("REALM_NAME=manual\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                wizard.save_config(root, "REALM_NAME=original\n", {"REALM_NAME": "nuevo"})
            self.assertEqual((root / "config.local.sh").read_text(), "REALM_NAME=manual\n")
            self.assertFalse((root / ".instalacion").exists())

    def test_cancel_does_not_write_or_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = 'REALM_IP="127.0.0.1"\n'
            (root / "config.sh").write_text(original, encoding="utf-8")
            config = dict.fromkeys(wizard.FIELDS, "unchanged")
            config.update(AC_DIR=str(root / "server"), INSTALL_WEB_PANEL="true")
            with patch.object(wizard, "ROOT", root), patch.object(wizard.sys, "platform", "linux"), \
                    patch.object(wizard, "load_config", return_value=config), \
                    patch.object(wizard, "preflight", return_value=[]), \
                    patch.object(wizard.sys.stdin, "isatty", return_value=True), \
                    patch.object(wizard, "ask", side_effect=["192.168.1.20", "Mi reino", "jugador"]), \
                    patch.object(wizard, "ask_password", return_value="Prueba12@"), \
                    patch("builtins.input", return_value="n"), \
                    patch.object(wizard.subprocess, "call") as launch:
                self.assertEqual(wizard.main([]), 0)
                launch.assert_not_called()
            self.assertEqual((root / "config.sh").read_text(), original)
            self.assertFalse((root / "config.local.sh").exists())
            self.assertFalse((root / ".instalacion").exists())

    def test_saved_metacharacters_are_literal_in_bash(self):
        value = "text$(echo BAD)`echo BAD`'end"
        script = wizard.render_local("", {"REALM_NAME": value})
        output = subprocess.check_output(["bash", "-c", script + '\nprintf "%s" "$REALM_NAME"'])
        self.assertEqual(output.decode(), value)


if __name__ == "__main__":
    unittest.main()
