"""End-to-end MCP test against a real editor. Creates a fresh overlay only.

Example: python smoke_editor.py --editor .../qtOgitor.exe --content-root .../TheLongWay --output-root .../mcp-test
"""
import argparse
import base64
import ctypes
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import uuid

import server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--content-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    args = parser.parse_args()
    args.output_root.mkdir(parents=True, exist_ok=True)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    env = dict(os.environ, RUN3_MCP_AUTOCONNECT=str(port))
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    log = (args.output_root / "editor.log").open("wb")
    editor = subprocess.Popen([str(args.editor.resolve())], cwd=args.editor.resolve().parent, env=env, startupinfo=startup, stdout=log, stderr=subprocess.STDOUT)
    mcp = None
    count = 0
    try:
        mcp = subprocess.Popen([sys.executable, str(Path(server.__file__)), "--port", str(port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")

        def request(method, params=None):
            nonlocal count
            count += 1
            mcp.stdin.write(json.dumps({"jsonrpc": "2.0", "id": count, "method": method, "params": params or {}}) + "\n")
            mcp.stdin.flush()
            response = json.loads(mcp.stdout.readline())
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["result"]

        def call(tool_name, **kw):
            result = request("tools/call", {"name": tool_name, "arguments": kw})
            if result.get("isError"):
                raise RuntimeError(f"{tool_name}: {result['content']}")
            return json.loads(result["content"][0]["text"])

        request("initialize", {"protocolVersion": server.PROTOCOL, "capabilities": {}, "clientInfo": {"name": "run3-integration-test", "version": "1"}})
        assert len(request("tools/list")["tools"]) == len(server.TOOLS)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if editor.poll() is not None:
                raise RuntimeError(f"Editor exited early ({editor.returncode}); inspect editor.log")
            if server.connection_path(port).exists():
                try:
                    call("editor_status")
                    break
                except RuntimeError:
                    pass
            time.sleep(0.25)
        else:
            raise RuntimeError("Editor MCP bridge did not become ready")

        # Authentication is enforced even for editor_status.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as bad:
            bad.sendall(b'{"tool":"editor_status","arguments":{},"token":"wrong"}\n')
            assert not json.loads(bad.makefile("rb").readline())["ok"]

        name = "mcp_demo_" + uuid.uuid4().hex[:8]
        level = call("level_create", name=name, content_root=str(args.content_root.resolve()), output_root=str(args.output_root.resolve()))
        call("environment_set", ambient=[0.35, 0.38, 0.42], player_position=[0, 180, 350])
        material = call("material_create", name="stone", color=[0.65, 0.7, 0.78])["material"]
        textures = call("asset_list", kind="texture", query=".png", limit=3)["assets"]
        assert textures, "Expected at least one discoverable texture"
        call("material_create", name="textured", color=[1, 1, 1], texture=textures[0])
        accent = call("material_create", name="accent", color=[0.8, 0.25, 0.12])["material"]
        floor = call("object_create", kind="box", name="floor", position=[0, -20, 0], scale=[1000, 40, 1000], material=material, collision=True)
        assert "mcp_unit_box.mesh" in call("asset_list", kind="mesh", query="mcp_unit_box")["assets"]
        call("object_create", kind="mesh", name="textured_crate", mesh="mcp_unit_box.mesh", position=[-180, 40, 80], scale=[80, 80, 80], material="mcp_textured")
        call("object_create", kind="box", name="back_wall", position=[0, 180, -500], scale=[1000, 400, 40], material=material)
        call("object_create", kind="box", name="left_wall", position=[-500, 180, 0], scale=[40, 400, 1000], material=material)
        prop = call("object_create", kind="box", name="pedestal", position=[0, 60, 0], scale=[150, 120, 150], material=accent)
        light = call("object_create", kind="light", name="key_light", position=[100, 350, 250], color=[1, 0.85, 0.65], range=4000)
        call("object_transform", id=prop["id"], position=[100, 60, 0], rotation_degrees=[0, 30, 0])
        call("history", action="undo")
        assert call("object_get", id=prop["id"])["position"] == [0, 60, 0]
        call("history", action="redo")
        assert call("object_get", id=prop["id"])["position"] == [100, 60, 0]
        call("object_material", id=prop["id"], material=material)
        call("history", action="undo")
        # The runtime consumes materialFile on mesh/nocollide records.
        mesh_records = [o for o in call("scene_list", query="pedestal", limit=20)["objects"] if o["tag"] == "nocollide"]
        assert len(mesh_records) == 1
        assert call("object_get", id=mesh_records[0]["id"])["fields"]["@materialFile"] == accent
        call("object_material", id=mesh_records[0]["id"], material="mcp_textured")
        call("object_material", id=mesh_records[0]["id"], material=accent)
        native_light = [o for o in call("scene_list", query="key_light", limit=20)["objects"] if o["tag"] == "light"][0]
        call("object_fields", id=native_light["id"], fields={"@power": "1.5"})
        assert call("object_get", id=native_light["id"])["fields"]["@power"] == "1.5"
        nodes = call("scene_list", tag="nodes")["objects"][0]["id"]
        trigger = call("record_create", parent_id=nodes, xml='<node name="trigger_transform"><position x="250" y="50" z="0"/><trigger name="demo_trigger"><scale x="100" y="100" z="100"/></trigger></node>')
        call("object_delete", id=trigger["id"])
        assert not call("scene_list", query="trigger_transform")["objects"]
        call("history", action="undo")
        assert call("scene_list", query="trigger_transform")["objects"]
        # Refuses replacing a dirty scene.
        refused = request("tools/call", {"name": "level_open", "arguments": {"scene_cfg": level["scene_cfg"], "content_root": str(args.content_root.resolve())}})
        assert refused["isError"]
        call("camera_set", position=[750, 650, 950], look_at=[0, 80, 0])
        capture = request("tools/call", {"name": "viewport_capture", "arguments": {"max_size": 1280}})
        assert not capture["isError"], capture
        image = next(c for c in capture["content"] if c["type"] == "image")
        png = base64.b64decode(image["data"])
        assert png.startswith(b"\x89PNG\r\n\x1a\n")
        (args.output_root / "overview.png").write_bytes(png)
        validation = call("scene_validate")
        assert not validation["render_failed"] and validation["player_spawns"] == 1, validation
        call("level_save")
        assert not call("editor_status")["modified"]
        cfg = Path(level["scene_cfg"])
        xml = (cfg.parent / "scene.xml").read_text()
        assert "mcp_unit_box.mesh" in xml and "key_light" in xml and "demo_trigger" in xml
        assert (cfg.parent / "mcp_stone.material").exists()
        assert (cfg.parent / "mcp_unit_box.mesh").exists()
        assert (cfg.parent / "mcp_unit_box.mesh").read_bytes()[2:80].startswith(b"[MeshSerializer_v1.8]")
        call("level_open", scene_cfg=str(cfg), content_root=str(args.content_root.resolve()), overlays=[str(args.output_root.resolve())])
        assert call("scene_list", query="pedestal")["total"] >= 1
        call("camera_set", position=[750, 650, 950], look_at=[0, 80, 0])
        reopened = request("tools/call", {"name": "viewport_capture"})
        assert not reopened["isError"]
        reopened_image = next(c for c in reopened["content"] if c["type"] == "image")
        (args.output_root / "reopened.png").write_bytes(base64.b64decode(reopened_image["data"]))
        print(json.dumps({"PASS": True, "tools": len(server.TOOLS), "scene_cfg": str(cfg), "screenshot": str(args.output_root / "overview.png"), "validation": validation}, indent=2), flush=True)
    finally:
        if mcp:
            mcp.stdin.close()
            try:
                mcp.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mcp.terminate()
                mcp.wait(timeout=5)
        if editor.poll() is None and os.name == "nt":
            # Close only windows belonging to the process started by this test.
            callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
            def close_window(hwnd, _):
                pid = ctypes.c_ulong()
                ctypes.windll.user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(pid))
                if pid.value == editor.pid:
                    ctypes.windll.user32.PostMessageW(ctypes.c_void_p(hwnd), 0x0010, 0, 0)
                return True
            ctypes.windll.user32.EnumWindows(callback_type(close_window), 0)
        try:
            editor.wait(timeout=5)
        except subprocess.TimeoutExpired:
            editor.terminate()
            editor.wait(timeout=5)
        log.close()


if __name__ == "__main__":
    main()
