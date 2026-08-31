"""Real process contract: no GUI, SDK, shell wrapper or mocked session."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

EXE = str(Path(sys.argv.pop(1)).resolve())
RECOVER = str(Path(sys.argv.pop(1)).resolve())


class Agent:
    def __init__(self, root, *args):
        env = os.environ.copy()
        env.update(LOCALAPPDATA=str(root), XDG_STATE_HOME=str(root))
        self.stderr = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [EXE, "agent", *args], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.stderr, env=env, cwd=root)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read_lines, daemon=True)
        self.reader.start()
        self.counter = 0

    def read_lines(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def read(self):
        line = self.lines.get(timeout=30)
        if line is None:
            self.stderr.seek(0)
            raise AssertionError("agent closed without a reply: " +
                                 self.stderr.read().decode("utf-8", "replace"))
        return json.loads(line)

    def call(self, command, params=None, generation=None):
        self.counter += 1
        request = dict(schema=1, id=f"q{self.counter}", command=command,
                       params={} if params is None else params)
        if generation is not None:
            request["host_generation"] = generation
        self.raw(json.dumps(request, ensure_ascii=False).encode() + b"\n")
        response = self.read()
        assert response["id"] == request["id"], response
        return response

    def raw(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def finish(self):
        if not self.process.stdin.closed:
            self.process.stdin.close()
        return self.process.wait(timeout=30)

    def dispose(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.reader.join(timeout=5)
        self.stderr.close()


class AgentContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ntpacker-agent-")
        self.root = Path(self.temp.name)
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            client.dispose()
        self.temp.cleanup()

    def start(self, *args):
        client = Agent(self.root, *args)
        self.clients.append(client)
        return client

    def test_new_status_snapshot_and_preserve_close(self):
        client = self.start("--new")
        ready = client.read()
        self.assertEqual(ready["type"], "ready", ready)
        self.assertEqual(ready["state"], "bound")
        initial = ready["session"]
        self.assertEqual(initial["project"]["version"], 5)
        status = initial["status"]
        self.assertEqual(status["revision"], 0)
        self.assertEqual(status["host_kind"], "headless")
        self.assertTrue(status["recovery"]["available"])
        response = client.call("project.snapshot", generation=status["host_generation"])
        self.assertTrue(response["ok"], response)
        self.assertEqual(response["result"], initial)
        closed = client.call("session.close", {"decision": "preserve"})
        self.assertTrue(closed["ok"], closed)
        self.assertEqual(client.finish(), 0)
        self.assertEqual(list(self.root.rglob("*.ntpacker_project")), [])

    def test_optional_generation_is_checked_before_preserve_close(self):
        client = self.start("--new")
        initial = client.read()["session"]
        generation = initial["status"]["host_generation"]
        stale = ("1" if generation[0] == "0" else "0") + generation[1:]
        for command, params in [("help", {}), ("capabilities", {}),
                                ("operations.list", {}), ("session.status", {}),
                                ("session.close", {"decision": "preserve"})]:
            with self.subTest(command=command):
                response = client.call(command, params, stale)
                self.assertFalse(response["ok"], response)
                self.assertEqual(response["error"]["code"], "host_changed")
        snapshot = client.call("project.snapshot", generation=generation)
        self.assertEqual(snapshot["result"], initial)
        closed = client.call("session.close", {"decision": "preserve"}, generation)
        self.assertTrue(closed["ok"], closed)
        self.assertEqual(closed["result"], {"closed": True, "preserved": True})
        self.assertEqual(client.finish(), 0)

    def test_transaction_preview_history_undo_redo_and_retries(self):
        client = self.start("--new")
        initial = client.read()["session"]
        generation = initial["status"]["host_generation"]
        atlas = initial["project"]["atlases"][0]["id"]
        transaction = {"schema": 1, "transaction": {
            "id": "2" * 32, "expected_revision": 0, "label": "Agent batch",
            "operations": [
                {"op": "atlas.rename", "atlas_id": atlas, "name": "герои"},
                {"op": "atlas.settings.set", "atlas_id": atlas, "padding": 3}]}}
        preview = client.call("project.apply", {"transaction": transaction, "dry_run": True}, generation)
        self.assertTrue(preview["ok"], preview)
        self.assertEqual(preview["result"]["mode"], "dry_run")
        self.assertEqual(client.call("project.snapshot", generation=generation)["result"], initial)
        applied = client.call("project.apply", {"transaction": transaction}, generation)
        self.assertTrue(applied["ok"], applied)
        self.assertEqual(applied["result"]["transaction_result"]["result"]["revision"], 1)
        duplicate = client.call("project.apply", {"transaction": transaction}, generation)
        self.assertFalse(duplicate["ok"])
        self.assertEqual(duplicate["error"]["code"], "duplicate_id")
        history = client.call("history.list", generation=generation)["result"]
        self.assertEqual(len(history["entries"]), 1)
        self.assertEqual(history["entries"][0]["author"],
                         "agent(" + initial["status"]["controller_id"] + ")")
        undo = client.call("history.undo", {"expected_revision": 1}, generation)
        self.assertTrue(undo["ok"], undo)
        self.assertEqual(undo["result"]["revision"], 2)
        self.assertEqual(client.call("project.snapshot", generation=generation)["result"]["project"], initial["project"])
        redo = client.call("history.redo", {"expected_revision": 2}, generation)
        self.assertTrue(redo["ok"], redo)
        self.assertEqual(redo["result"]["revision"], 3)
        transaction["transaction"].update(id="3" * 32, expected_revision=3)
        noop = client.call("project.apply", {"transaction": transaction}, generation)
        self.assertEqual(noop["result"]["transaction_result"]["result"]["status"], "no_change")
        final_project = client.call("project.snapshot", generation=generation)["result"]["project"]
        self.assertEqual(client.finish(), 0)
        self.assertTrue(list((self.root / "ntpacker" / "recovery").rglob("*.ntpjournal")))
        self.assertEqual(list(self.root.rglob("*.ntpacker_project")), [])
        recovered = self.root / "recovered.ntpacker_project"
        result = subprocess.run([RECOVER, str(self.root / "ntpacker" / "recovery"), str(recovered)],
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(recovered.read_text(encoding="utf-8")), final_project)

    def test_unbound_discovery_and_one_binding(self):
        client = self.start()
        self.assertEqual(client.read()["state"], "unbound")
        expected = {"help", "capabilities", "operations.list", "session.bind", "session.status",
                    "session.close", "project.snapshot", "project.apply", "history.list", "history.undo", "history.redo"}
        caps = client.call("capabilities")["result"]
        self.assertEqual(set(caps["commands"]), expected)
        self.assertEqual(len(caps["operations"]), 19)
        help_reply = client.call("help")["result"]
        self.assertEqual({row["command"] for row in help_reply["commands"]}, expected)
        operations = client.call("operations.list")["result"]
        self.assertEqual({row["op"] for row in operations["operations"]}, set(caps["operations"]))
        missing = client.call("project.snapshot")
        self.assertEqual(missing["error"]["code"], "not_bound")
        bound = client.call("session.bind", {"new": True})
        self.assertTrue(bound["ok"], bound)
        self.assertIn("project", bound["result"])
        second = client.call("session.bind", {"new": True})
        self.assertEqual(second["error"]["code"], "already_bound")
        self.assertEqual(client.finish(), 0)

    def test_disabled_policy_rechecked_without_changing_project(self):
        client = self.start("--new")
        initial = client.read()["session"]
        generation = initial["status"]["host_generation"]
        folder = self.root / "ntpacker" / "automation"
        folder.mkdir(parents=True)
        policy = folder / "permissions.json"
        policy.write_text(json.dumps(dict(schema=1, mode="disabled", projects=[])))
        response = client.call("project.snapshot", generation=generation)
        self.assertEqual(response["error"]["code"], "authorization_disabled")
        self.assertEqual(client.call("session.status")["result"]["authorization"], "disabled")
        policy.write_text(json.dumps(dict(schema=1, mode="ask", projects=[])))
        self.assertEqual(client.call("project.snapshot", generation=generation)["result"], initial)
        self.assertEqual(client.finish(), 0)

    def test_rejected_requests_leave_snapshot_and_history_unchanged(self):
        client = self.start("--new")
        initial = client.read()["session"]
        generation = initial["status"]["host_generation"]
        atlas = initial["project"]["atlases"][0]["id"]
        transaction = {"schema": 1, "transaction": {
            "id": "4" * 32, "expected_revision": 0, "operations": [
                {"op": "atlas.rename", "atlas_id": atlas, "name": "should-roll-back"},
                {"op": "atlas.settings.set", "atlas_id": atlas, "padding": -2}]}}
        invalid_calls = [
            ("project.apply", {"transaction": transaction}, generation),
            ("project.snapshot", {}, "0" * 32),
            ("project.snapshot", {"surprise": True}, generation),
            ("history.undo", {"expected_revision": 9007199254740992}, generation),
            ("history.undo", {"expected_revision": 0.5}, generation),
            ("session.close", {"decision": "discard"}, generation)]
        for command, params, host in invalid_calls:
            response = client.call(command, params, host)
            self.assertFalse(response["ok"], response)
        transaction["transaction"]["author"] = "human"
        response = client.call("project.apply", {"transaction": transaction}, generation)
        self.assertEqual(response["error"]["details"]["phase"], "decode")
        for data in [b'{"schema":01,"id":"x","command":"session.status","params":{}}\n',
                     b'{"schema":1,"id":"x","command":"session.status","params":{},"host_generation":17}\n',
                     b'{"schema":1,"id":"x","command":"session.status","params":{},"params":{}}\n',
                     b'{"schema":1,"id":"x","command":"session.status","params":{"a":"\\u0000"}}\n',
                     b'\xff\n', b'{"schema":1}\0\n', b'[]\n', b'{broken}\n']:
            client.raw(data)
            self.assertFalse(client.read()["ok"], data)
        self.assertEqual(client.call("project.snapshot", generation=generation)["result"], initial)
        self.assertEqual(client.call("history.list", generation=generation)["result"]["entries"], [])
        self.assertEqual(client.finish(), 0)

    def test_split_requests_blank_lines_and_limit_with_crlf(self):
        client = self.start()
        client.read()
        client.raw(b"\r\n \t\n")
        request = b'{"schema":1,"id":"split","command":"session.status","params":{}}'
        client.raw(request[:13])
        client.raw(request[13:] + b"\r\n")
        self.assertEqual(client.read()["id"], "split")
        limit = 2 * 1024 * 1024
        client.raw(request + b" " * (limit - len(request)) + b"\r\n")
        self.assertTrue(client.read()["ok"])
        self.assertEqual(client.finish(), 0)

    def test_oversized_line_terminates_and_partial_eof_does_not_apply(self):
        client = self.start()
        client.read()
        client.raw(b" " * (2 * 1024 * 1024 + 1))
        self.assertEqual(client.read()["error"]["code"], "request_too_large")
        self.assertEqual(client.finish(), 1)
        partial = self.start()
        partial.read()
        partial.raw(b'{"schema":1,"id":"p","command":"session.bind","params":{"new":true}}')
        self.assertEqual(partial.finish(), 1)
        self.assertEqual(list(self.root.rglob("*.ntpjournal")), [])

    def test_recovery_failure_is_visible_and_exit_is_not_success(self):
        folder = self.root / "ntpacker"
        folder.mkdir()
        (folder / "recovery").write_text("not a directory")
        client = self.start("--new")
        initial = client.read()["session"]
        self.assertTrue(initial["status"]["recovery"]["degraded"])
        generation = initial["status"]["host_generation"]
        atlas = initial["project"]["atlases"][0]["id"]
        transaction = {"schema": 1, "transaction": {
            "id": "5" * 32, "expected_revision": 0,
            "operations": [{"op": "atlas.rename", "atlas_id": atlas, "name": "unsaved"}]}}
        response = client.call("project.apply", {"transaction": transaction}, generation)
        self.assertTrue(response["ok"], response)
        self.assertIn("recovery_degraded", [n["code"] for n in response["notices"]])
        self.assertEqual(client.finish(), 1)

    def test_reserved_operations_are_rejected_before_preview_or_apply(self):
        client = self.start("--new")
        initial = client.read()["session"]
        generation = initial["status"]["host_generation"]
        atlas = initial["project"]["atlases"][0]["id"]
        source = "source_" + "7" * 32
        animation = "anim_" + "8" * 32
        scenarios = [
            [{"op": "source.add", "atlas_id": atlas, "source_id": source,
              "kind": "file", "key": str(self.root / "first.png")},
             {"op": "source.replace", "atlas_id": atlas, "source_id": source,
              "key": str(self.root / "second.png")}],
            [{"op": "animation.create", "atlas_id": atlas, "anim_id": animation,
              "name": "walk"},
             {"op": "animation.frames.set", "atlas_id": atlas,
              "anim_id": animation, "frames": []}]]
        for index, operations in enumerate(scenarios, 1):
            transaction = {"schema": 1, "transaction": {
                "id": f"{index:032x}", "expected_revision": 0,
                "operations": operations}}
            for preview in (True, False):
                response = client.call("project.apply", {
                    "transaction": transaction, "dry_run": preview}, generation)
                self.assertFalse(response["ok"], response)
                self.assertEqual(response["error"]["code"], "unknown_op")
                self.assertEqual(client.call("project.snapshot", generation=generation)["result"], initial)
                self.assertEqual(client.call("history.list", generation=generation)["result"]["entries"], [])
        self.assertEqual(client.finish(), 0)

    def test_nested_transaction_limit_counts_original_whitespace(self):
        client = self.start("--new")
        generation = client.read()["session"]["status"]["host_generation"]
        raw = ('{"schema":1,"id":"limit","host_generation":"' + generation +
               '","command":"project.apply","params":{"transaction":{').encode()
        raw += b" " * (1024 * 1024)
        raw += b'"schema":1,"transaction":{"id":"66666666666666666666666666666666","expected_revision":0,"operations":[]}}}}\n'
        client.raw(raw)
        response = client.read()
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "out_of_bounds")
        self.assertEqual(client.finish(), 0)

    def test_raw_revision_never_rounds_into_an_accepted_write(self):
        client = self.start("--new")
        initial = client.read()["session"]
        generation = initial["status"]["host_generation"]
        atlas = initial["project"]["atlases"][0]["id"]
        transaction = {"schema": 1, "transaction": {"id": "7" * 32, "expected_revision": 0,
                       "operations": [{"op": "atlas.rename", "atlas_id": atlas, "name": "before"}]}}
        self.assertTrue(client.call("project.apply", {"transaction": transaction}, generation)["ok"])
        current = client.call("project.snapshot", generation=generation)["result"]
        for raw_number in ["1.0000000000000001", "0.99999999999999999", "1.00000000000000001e0"]:
            request = {"schema": 1, "id": "rounded", "command": "history.undo",
                       "host_generation": generation, "params": {"expected_revision": "RAW"}}
            client.raw(json.dumps(request).replace('"RAW"', raw_number).encode() + b"\n")
            self.assertFalse(client.read()["ok"], raw_number)
            transaction["transaction"].update(id="8" * 32, expected_revision="RAW")
            transaction["transaction"]["operations"][0]["name"] = "should-not-change"
            request.update(command="project.apply", params={"transaction": transaction})
            client.raw(json.dumps(request).replace('"RAW"', raw_number).encode() + b"\n")
            response = client.read()
            self.assertFalse(response["ok"], raw_number)
            self.assertEqual(response["error"]["details"]["phase"], "decode")
        transaction.update(schema="RAW")
        transaction["transaction"]["expected_revision"] = 1
        client.raw(json.dumps(request).replace('"RAW"', "1.0000000000000001").encode() + b"\n")
        self.assertFalse(client.read()["ok"])
        self.assertEqual(client.call("project.snapshot", generation=generation)["result"], current)
        client.raw(('{"schema":1,"id":"exact","command":"history.undo","host_generation":"' +
                    generation + '","params":{"expected_revision":100e-2}}\n').encode())
        self.assertTrue(client.read()["ok"])
        self.assertEqual(client.finish(), 0)

    def test_startup_help_policy_and_explicit_discard(self):
        helper = self.start("--help", "--json")
        self.assertEqual(len(helper.read()["commands"]), 11)
        self.assertEqual(helper.finish(), 0)
        unavailable = self.start("--project", str(self.root / "missing.ntpacker_project"))
        self.assertFalse(unavailable.read()["ok"])
        self.assertEqual(unavailable.finish(), 2)
        folder = self.root / "ntpacker" / "automation"
        folder.mkdir(parents=True)
        policy = folder / "permissions.json"
        policy.write_text(json.dumps(dict(schema=1, mode="disabled", projects=[])))
        denied = self.start("--new")
        self.assertEqual(denied.read()["error"]["code"], "authorization_disabled")
        self.assertEqual(denied.finish(), 8)
        policy.unlink()
        client = self.start("--new")
        initial = client.read()["session"]
        generation = initial["status"]["host_generation"]
        result = client.call("session.close", {"decision": "discard", "expected_revision": 0}, generation)
        self.assertTrue(result["ok"], result)
        self.assertFalse(result["result"]["preserved"])
        self.assertEqual(client.finish(), 0)
        self.assertEqual(list(self.root.rglob("*.ntpjournal")), [])

    def test_large_snapshot_is_one_complete_line(self):
        client = self.start("--new")
        initial = client.read()["session"]
        generation = initial["status"]["host_generation"]
        for batch in range(2):
            operations = [{"op": "atlas.create", "atlas_id": "atlas_" + format(i, "032x"),
                           "name": "atlas-" + str(i) + "-" + "x" * 120}
                          for i in range(batch * 3000 + 1, (batch + 1) * 3000 + 1)]
            transaction = {"schema": 1, "transaction": {"id": format(batch + 100, "032x"),
                           "expected_revision": batch, "operations": operations}}
            response = client.call("project.apply", {"transaction": transaction}, generation)
            self.assertTrue(response["ok"], response)
        snapshot = client.call("project.snapshot", generation=generation)["result"]
        self.assertEqual(len(snapshot["project"]["atlases"]), 6001)
        self.assertGreater(len(json.dumps(snapshot)), 1024 * 1024)
        self.assertEqual(snapshot["status"]["revision"], 2)
        self.assertEqual(client.finish(), 0)

    def test_broken_stdout_preserves_commit_for_core_recovery(self):
        env = dict(os.environ, LOCALAPPDATA=str(self.root), XDG_STATE_HOME=str(self.root))
        with subprocess.Popen([EXE, "agent", "--new"], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env) as process:
            initial = json.loads(process.stdout.readline())["session"]
            process.stdout.close()
            atlas = initial["project"]["atlases"][0]["id"]
            request = {"schema": 1, "id": "lost", "command": "project.apply",
                       "host_generation": initial["status"]["host_generation"], "params": {
                "transaction": {"schema": 1, "transaction": {"id": "b" * 32, "expected_revision": 0,
                    "operations": [{"op": "atlas.rename", "atlas_id": atlas, "name": "lost-reply"}]}}}}
            process.stdin.write(json.dumps(request).encode() + b"\n")
            process.stdin.close()
            self.assertEqual(process.wait(timeout=30), 1, process.stderr.read())
        self.assertEqual(list(self.root.rglob("*.ntpacker_project")), [])
        recovered = self.root / "recovered.ntpacker_project"
        result = subprocess.run([RECOVER, str(self.root / "ntpacker" / "recovery"), str(recovered)],
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = initial["project"]
        expected["atlases"][0]["name"] = "lost-reply"
        self.assertEqual(json.loads(recovered.read_text(encoding="utf-8")), expected)


if __name__ == "__main__":
    unittest.main()
