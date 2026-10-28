#!/usr/bin/env python3
"""Run3Ogitor MCP stdio server. Python 3.10+, standard library only.

The authenticated loopback bridge performs all edits on Ogitor's UI thread.
Nothing in this process opens a game asset or executes user-supplied code.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import socket
import sys

PROTOCOL = "2025-11-25"
SUPPORTED = {"2024-11-05", "2025-03-26", "2025-06-18", PROTOCOL}
MAX_REQUEST = 1024 * 1024
MAX_RESPONSE = 32 * 1024 * 1024
INSTRUCTIONS = """Create real Run3 levels in the connected Ogitor editor. Start with editor_status,
then level_open or level_create. Discover IDs with scene_list and assets with asset_list.
World coordinates are Y-up; rotation_degrees is [pitch,yaw,roll], applied Y*X*Z.
Use object_create boxes (unit cube scaled to world dimensions) for floors and walls,
or meshes for props. Parent-relative transforms are returned alongside world positions.
Create materials from existing textures, assign them to meshes, place lights and set a
player spawn. After editing, move the camera, call viewport_capture, inspect its IMAGE,
and correct scale, placement, textures and illumination. Capture more than one angle.
Use scene_validate as a structural check, not as a substitute for visual inspection.
Save explicitly with level_save. history supports undo/redo for scene edits; newly written
asset files and newly created level directories survive undo. Gameplay/scripts/physics
are not simulated in Ogitor. Never assume a successful preview proves gameplay works.
"""

S = {"type": "string"}
N = {"type": "number"}
I = {"type": "integer", "minimum": 1}
B = {"type": "boolean"}
V = {"type": "array", "items": N, "minItems": 3, "maxItems": 3}
PAGE = {"query": S, "offset": {"type": "integer", "minimum": 0}, "limit": {"type": "integer", "minimum": 1, "maximum": 200}}
TRANSFORM = {"position": V, "rotation_degrees": V, "scale": V}


def tool(name, description, properties=None, required=(), *, read=False, destructive=False):
    return {
        "name": name,
        "description": description,
        "inputSchema": {"type": "object", "properties": properties or {}, "required": list(required), "additionalProperties": False},
        "annotations": {"readOnlyHint": read, "destructiveHint": destructive, "openWorldHint": False},
    }


TOOLS = [
    tool("editor_status", "Read editor connection, open native map, unsaved status and render failure state.", read=True),
    tool("level_create", "Create and open a NEW native Run3 level in output_root/run3/maps/high/name. Refuses existing levels and unsaved open scenes. Writes initial scene.cfg, scene.xml, sequence.xml immediately. output_root is an existing directory used as a writable overlay; content_root supplies the game assets.",
         {"name": S, "content_root": S, "output_root": S, "overlays": {"type": "array", "items": S}}, ("name", "content_root", "output_root")),
    tool("level_open", "Open a native scene.cfg using a game content root and optional existing overlays in low-to-high priority order. Refuses unsaved edits. The last overlay is the save destination, or the base if none.",
         {"scene_cfg": S, "content_root": S, "overlays": {"type": "array", "items": S}}, ("scene_cfg", "content_root")),
    tool("level_save", "Save native map documents with existing backup and external-edit protection. Writes to last overlay or base content. Asset files created by other tools are already written.", destructive=True),
    tool("asset_list", "Search/paginate available mesh filenames, texture filenames, or material names. Use returned names verbatim.", {"kind": {"type": "string", "enum": ["mesh", "texture", "material"]}, **PAGE}, read=True),
    tool("scene_list", "Search/paginate native records with stable IDs for this open session, parent IDs, tags and local/world transforms. Use tag=nodes to find mesh parents, tag=adents for sequence entities. Refresh IDs after undo/reopen.", {"tag": S, **PAGE}, read=True),
    tool("object_get", "Read an object's complete native XML, editable property paths, transform and local bounds. Property paths are returned in fields.", {"id": I}, ("id",), read=True),
    tool("object_create", "Place a native mesh, unit box, or light under parent_id (defaults to first nodes section). Scale boxes to desired [width,height,depth] in Run3 world units. Mesh/box is wrapped in a transform node. collision=true emits entity, false emits nocollide. Light rotation aims local -Z. New box mesh asset persists immediately; scene edits require save.",
         {"kind": {"type": "string", "enum": ["mesh", "box", "light"]}, "name": S, "parent_id": I, "mesh": S, "material": S, "collision": B, "light_type": {"type": "string", "enum": ["point", "spot", "directional"]}, "color": V, "range": {"type": "number", "exclusiveMinimum": 0}, **TRANSFORM}, ("kind", "name")),
    tool("record_create", "Advanced: insert one native XML record/subtree under an explicit native parent. Use object_create for mesh/light placement. Use this for triggers, NPCs, sounds, gameplay entities and other existing Run3 schemas; inspect authored examples first. Editor previews never execute Lua.", {"parent_id": I, "xml": S}, ("parent_id", "xml")),
    tool("object_transform", "Change parent-relative position, scale and/or rotation in degrees. Undoable as one edit.", {"id": I, **TRANSFORM}, ("id",)),
    tool("object_fields", "Patch native XML fields using string values. Read object_get.fields for exact existing paths. New attributes may use @name. Do not use this to add child records; use record_create. Undoable.", {"id": I, "fields": {"type": "object", "additionalProperties": S, "minProperties": 1}}, ("id", "fields")),
    tool("object_material", "Assign an existing material to a mesh record or to a node with one direct mesh child. Updates native material references for saving and refreshes the preview. Undoable.", {"id": I, "material": S}, ("id", "material")),
    tool("material_create", "Write a NEW map-local material asset using RGB color and optional existing texture filename. unlit is useful for emissive/signage previews; default is lit. Returns mcp_name to assign with object_material. File creation survives undo.", {"name": S, "color": V, "texture": S, "unlit": B}, ("name",)),
    tool("environment_set", "Set native ambient RGB lighting and/or world player spawn under the scene environment. Updates live lighting. Undoable.", {"ambient": V, "player_position": V}),
    tool("object_delete", "Delete a native record and its subtree. Refuses document roots. Undoable.", {"id": I}, ("id",), destructive=True),
    tool("history", "Undo or redo one editor operation. Refresh scene IDs afterwards. Does not remove newly written assets or level directories.", {"action": {"type": "string", "enum": ["undo", "redo"]}}, ("action",), destructive=True),
    tool("camera_set", "Move the editor camera in WORLD coordinates and aim at look_at, or set rotation_degrees. Capture afterwards to evaluate placement and lighting. Does not alter the player spawn.", {"position": V, "look_at": V, "rotation_degrees": V}),
    tool("viewport_capture", "Render the current editor viewport and return a PNG IMAGE for visual evaluation. Look at this image, identify problems, edit and capture again. Ogitor must be visible, unminimized, on the Render tab. Grid is omitted unless include_grid=true; its UI setting is restored afterwards. This is the editor preview, not gameplay rendering.", {"max_size": {"type": "integer", "minimum": 128, "maximum": 2048}, "include_grid": B}, read=True),
    tool("scene_validate", "Evaluate structure: missing meshes, duplicate game names, player spawn and renderer health. Returns actionable issues. Use viewport_capture for visual judgement and the Run3 runtime for gameplay verification.", read=True),
    tool("render_restore", "Retry the editor renderer after a recoverable frame failure without closing the map or losing edits. Inspect editor_status and capture to confirm recovery."),
]
BY_NAME = {t["name"]: t for t in TOOLS}


def validate(value, schema, path="arguments"):
    kind = schema.get("type")
    types = {"object": dict, "array": list, "string": str, "boolean": bool, "integer": int, "number": (int, float)}
    if kind and (not isinstance(value, types[kind]) or kind in ("integer", "number") and isinstance(value, bool)):
        raise ValueError(f"{path} must be {kind}")
    if isinstance(value, (float, int)) and not isinstance(value, bool):
        if not math.isfinite(value):
            raise ValueError(f"{path} must be finite")
        if "minimum" in schema and value < schema["minimum"] or "maximum" in schema and value > schema["maximum"]:
            raise ValueError(f"{path} outside allowed range")
        if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
            raise ValueError(f"{path} must exceed {schema['exclusiveMinimum']}")
    if "enum" in schema and value not in schema["enum"]:
        raise ValueError(f"{path} must be one of {schema['enum']}")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                raise ValueError(f"Missing {path}.{key}")
        if len(value) < schema.get("minProperties", 0):
            raise ValueError(f"{path} is empty")
        for key, item in value.items():
            sub = schema.get("properties", {}).get(key, schema.get("additionalProperties", {}))
            if sub is False:
                raise ValueError(f"Unknown {path}.{key}")
            if isinstance(sub, dict):
                validate(item, sub, f"{path}.{key}")
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0) or len(value) > schema.get("maxItems", 10000):
            raise ValueError(f"Invalid length for {path}")
        for index, item in enumerate(value):
            validate(item, schema.get("items", {}), f"{path}[{index}]")


def connection_path(port):
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "Run3Ogitor" / f"mcp-{port}.json"


class Bridge:
    def __init__(self, port=9877, descriptor=None, timeout=60):
        self.port = port
        self.descriptor = Path(descriptor) if descriptor else connection_path(port)
        self.timeout = timeout

    def call(self, name, arguments):
        try:
            info = json.loads(self.descriptor.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise RuntimeError(f"Ogitor is disconnected. Click Connect on the Run3 MCP toolbar with port {self.port}. Connection file: {self.descriptor}") from exc
        if info.get("host") != "127.0.0.1" or info.get("port") != self.port or not isinstance(info.get("token"), str) or len(info["token"]) < 32:
            raise RuntimeError("Invalid local Ogitor connection descriptor")
        payload = json.dumps({"tool": name, "arguments": arguments, "token": info["token"]}, allow_nan=False).encode("utf-8") + b"\n"
        if len(payload) > MAX_REQUEST:
            raise ValueError("Editor request exceeds 1 MiB")
        sent = False
        try:
            with socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout) as sock:
                sock.sendall(payload)
                sent = True
                with sock.makefile("rb") as stream:
                    line = stream.readline(MAX_RESPONSE + 1)
                if not line or len(line) > MAX_RESPONSE or not line.endswith(b"\n"):
                    raise RuntimeError("Editor disconnected or returned an oversized response. The request may have executed; inspect the scene before retrying a mutation.")
                response = json.loads(line)
                if not response.get("ok"):
                    raise RuntimeError(response.get("error", "Editor operation failed"))
                result = response.get("result")
                if not isinstance(result, dict):
                    raise RuntimeError("Invalid editor response")
                return result
        except (OSError, ValueError) as exc:
            suffix = " The request may have executed; inspect the scene before retrying a mutation." if sent else " Click Connect in Ogitor and check the port."
            raise RuntimeError(f"Ogitor bridge: {exc}.{suffix}") from exc


class Server:
    def __init__(self, bridge):
        self.bridge = bridge
        self.initialized = False

    def handle(self, request):
        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
            return {"jsonrpc": "2.0", "id": request.get("id") if isinstance(request, dict) else None, "error": {"code": -32600, "message": "Invalid request"}}
        if "id" not in request:
            return None  # Notifications, including initialized/cancelled, have no response.
        response = {"jsonrpc": "2.0", "id": request["id"]}
        method, params = request["method"], request.get("params", {})
        try:
            if not isinstance(params, dict):
                raise ValueError("params must be an object")
            if method == "initialize":
                self.initialized = True
                version = params.get("protocolVersion")
                result = {"protocolVersion": version if version in SUPPORTED else PROTOCOL, "capabilities": {"tools": {"listChanged": False}}, "serverInfo": {"name": "run3ogitor", "version": "1.0.0"}, "instructions": INSTRUCTIONS}
            elif method == "ping":
                result = {}
            elif not self.initialized:
                raise ValueError("Initialize the MCP session first")
            elif method == "tools/list":
                if params.get("cursor"):
                    raise ValueError("Invalid pagination cursor")
                result = {"tools": TOOLS}
            elif method == "tools/call":
                name = params.get("name")
                if not isinstance(name, str) or name not in BY_NAME:
                    raise ValueError("Unknown tool")
                arguments = params.get("arguments", {})
                try:
                    validate(arguments, BY_NAME[name]["inputSchema"])
                    data = self.bridge.call(name, arguments)
                    encoded = data.pop("image_base64", None)
                    result = {"content": [{"type": "text", "text": json.dumps(data, ensure_ascii=True, allow_nan=False)}], "isError": False}
                    if encoded:
                        result["content"].append({"type": "image", "data": encoded, "mimeType": "image/png"})
                except (ValueError, RuntimeError) as exc:
                    result = {"content": [{"type": "text", "text": str(exc)}], "isError": True}
            else:
                response["error"] = {"code": -32601, "message": "Method not found"}
                return response
            response["result"] = result
        except ValueError as exc:
            response["error"] = {"code": -32602, "message": str(exc)}
        return response


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=int(os.environ.get("RUN3_MCP_PORT", "9877")))
    parser.add_argument("--connection-file")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or not 0 < args.timeout <= 300:
        parser.error("port must be 1024..65535 and timeout must be 0..300 seconds")
    server = Server(Bridge(args.port, args.connection_file, args.timeout))
    while True:
        line = sys.stdin.buffer.readline(MAX_REQUEST + 1)
        if not line:
            break
        try:
            if len(line) > MAX_REQUEST or not line.endswith(b"\n"):
                raise ValueError("Message exceeds 1 MiB or is not newline-delimited")
            request = json.loads(line, parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"Invalid JSON number {value}")))
            response = server.handle(request)
        except (ValueError, UnicodeError) as exc:
            response = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": str(exc)}}
        except Exception as exc:
            print(f"Run3Ogitor MCP error: {type(exc).__name__}: {exc}", file=sys.stderr, flush=True)
            response = {"jsonrpc": "2.0", "id": request.get("id") if isinstance(request, dict) else None, "error": {"code": -32603, "message": "Internal MCP server error"}}
        if response is not None:
            sys.stdout.buffer.write(json.dumps(response, ensure_ascii=True, allow_nan=False).encode("utf-8") + b"\n")
            sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
