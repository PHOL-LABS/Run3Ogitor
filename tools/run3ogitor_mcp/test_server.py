import base64
import io
import json
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

import server


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        owner = self

        class Fake:
            def call(self, name, args):
                owner.calls.append((name, args))
                if name == "viewport_capture":
                    return {"image_base64": base64.b64encode(b"png").decode(), "width": 1}
                return {"scene_loaded": True}

        self.server = server.Server(Fake())
        self.request("initialize", {"protocolVersion": "2025-03-26"})

    def request(self, method, params=None):
        return self.server.handle({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}})

    def test_negotiation_discovery_and_notifications(self):
        result = self.request("initialize", {"protocolVersion": "2025-03-26"})["result"]
        self.assertEqual(result["protocolVersion"], "2025-03-26")
        self.assertEqual(result["serverInfo"]["name"], "run3ogitor")
        self.assertIsNone(self.server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}))
        tools = self.request("tools/list")["result"]["tools"]
        self.assertGreaterEqual(len(tools), 18)
        self.assertEqual(len(tools), len({t["name"] for t in tools}))

    def test_malformed_arguments_never_reach_editor(self):
        for args in ({"id": True}, {"id": 1, "position": [1, 2]}, {"id": 1, "position": [1, float("nan"), 3]}, {"id": 1, "unknown": 2}):
            result = self.request("tools/call", {"name": "object_transform", "arguments": args})["result"]
            self.assertTrue(result["isError"])
        self.assertFalse(self.calls)

    def test_errors_and_image_content(self):
        self.assertEqual(self.request("nonexistent")["error"]["code"], -32601)
        self.assertEqual(self.request("tools/call", {"name": "nonexistent"})["error"]["code"], -32602)
        result = self.request("tools/call", {"name": "viewport_capture"})["result"]
        self.assertEqual(result["content"][1]["type"], "image")
        self.assertNotIn("image_base64", result["content"][0]["text"])

    def test_missing_connection_is_actionable(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "Click Connect"):
                server.Bridge(descriptor=Path(directory) / "absent.json").call("editor_status", {})

    def test_bridge_authenticated_newline_framing(self):
        with tempfile.TemporaryDirectory() as directory, socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            port = listener.getsockname()[1]
            token = "a" * 64
            descriptor = Path(directory) / "connection.json"
            descriptor.write_text(json.dumps({"host": "127.0.0.1", "port": port, "token": token}))
            requests = []

            def reply():
                with listener.accept()[0] as client, client.makefile("rb") as stream:
                    requests.append(json.loads(stream.readline()))
                    client.sendall(b'{"ok":true,"result":{"modified":false}}\n')

            thread = threading.Thread(target=reply)
            thread.start()
            self.assertEqual(server.Bridge(port, descriptor).call("editor_status", {}), {"modified": False})
            thread.join(3)
            self.assertEqual(requests[0]["token"], token)
            self.assertEqual(requests[0]["tool"], "editor_status")

    def test_real_stdio_process_has_only_protocol_on_stdout(self):
        requests = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "editor_status"}},
        ]
        with tempfile.TemporaryDirectory() as directory:
            process = subprocess.run([sys.executable, str(Path(server.__file__)), "--connection-file", str(Path(directory) / "absent.json")],
                                     input="\n".join(json.dumps(r) for r in requests) + "\n", capture_output=True, text=True, timeout=10)
        self.assertEqual(process.returncode, 0, process.stderr)
        messages = [json.loads(line) for line in process.stdout.splitlines()]
        self.assertEqual([m["id"] for m in messages], [1, 2, 3])
        self.assertTrue(messages[-1]["result"]["isError"])


if __name__ == "__main__":
    unittest.main()
