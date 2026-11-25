import json
import sys
from pathlib import Path
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

from benchparse import parse_json_output, parse_markdown_output
from collector import Node
from dashboard import authorize_token, build_remote_command, run_action, validate_model_path


NODE = Node("thebe", "thebe", "head", 50052, 8080)


class ActionTests(unittest.TestCase):
    def test_unknown_node_and_action_are_rejected_before_subprocess(self):
        with mock.patch("dashboard.subprocess.run") as run:
            with self.assertRaisesRegex(ValueError, "unknown node"):
                run_action({"thebe": NODE}, "other", "stop-server")
            with self.assertRaisesRegex(ValueError, "unknown action"):
                run_action({"thebe": NODE}, "thebe", "reboot")
        run.assert_not_called()

    def test_known_action_uses_inventory_ssh_alias(self):
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch("dashboard.subprocess.run", return_value=completed) as run:
            result = run_action({"thebe": NODE}, "thebe", "stop-server")
        self.assertTrue(result["ok"])
        self.assertEqual(run.call_args.args[0][:5], ["ssh", "-o", "BatchMode=yes", "--", "thebe"])
        self.assertEqual(run.call_args.args[0][-1], "pkill -x llama-server || true")

    def test_model_path_is_shell_quoted(self):
        command = build_remote_command(NODE, "start-server", "~/models/a model's.gguf")
        self.assertIn("$HOME/'models/a model'\"'\"'s.gguf'", command)
        self.assertIn("--port 8080", command)

    def test_model_traversal_and_non_model_paths_are_rejected(self):
        for path in ("~/models/../secret", "/tmp/model.gguf", "~/models/", "~/models/x\ncommand"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                validate_model_path(path)


class TokenTests(unittest.TestCase):
    def test_token_must_be_configured_and_match(self):
        self.assertFalse(authorize_token(None, "secret"))
        self.assertFalse(authorize_token("secret", None))
        self.assertFalse(authorize_token("secret", "wrong"))
        self.assertTrue(authorize_token("secret", "secret"))


class BenchParserTests(unittest.TestCase):
    def test_json_parser_pairs_prompt_and_generation(self):
        fixture = json.dumps([
            {"model_filename": "tiny.gguf", "n_gpu_layers": 99, "n_prompt": 512, "n_gen": 0, "avg_ts": 21.25},
            {"model_filename": "tiny.gguf", "n_gpu_layers": 99, "n_prompt": 0, "n_gen": 128, "avg_ts": 8.5},
        ])
        self.assertEqual(parse_json_output(fixture), [{
            "model_reported": "tiny.gguf", "ngl_reported": 99,
            "pp": 21.25, "pp_tokens": 512, "tg": 8.5, "tg_tokens": 128,
        }])

    def test_markdown_parser_pairs_prompt_and_generation(self):
        fixture = """
model | size | params | backend | ngl | test | t/s
--- | ---: | ---: | --- | ---: | --- | ---:
tiny 1B Q4 | 700 MiB | 1.0 B | Vulkan | 99 | pp 512 | 20.50 ± 0.10
tiny 1B Q4 | 700 MiB | 1.0 B | Vulkan | 99 | tg 128 | 7.25 ± 0.03
"""
        self.assertEqual(parse_markdown_output(fixture), [{
            "model_reported": "tiny 1B Q4", "ngl_reported": 99,
            "pp": 20.5, "pp_tokens": 512, "tg": 7.25, "tg_tokens": 128,
        }])


if __name__ == "__main__":
    unittest.main()
