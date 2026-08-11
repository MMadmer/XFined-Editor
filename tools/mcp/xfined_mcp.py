#!/usr/bin/env python3
"""MCP stdio bridge for the XFined Editor.

The editor hosts a tiny line-based JSON endpoint on 127.0.0.1:28016 (alive from
the first frame, project browser included). This script exposes it as a
standard MCP server over stdio — no third-party dependencies, pure stdlib.

Register (Claude Code):  claude mcp add -s user xfined-editor -- python <this file>
"""
import json
import socket
import sys

HOST, PORT = "127.0.0.1", 28016

TOOLS = [
    {
        "name": "xfined_ping",
        "description": "Check that the XFined Editor is running and its MCP endpoint responds.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_state",
        "description": "Editor state: active project (name/path), opened scene, FPS.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_screenshot_viewport",
        "description": "Screenshot of the 3D viewport render target (PNG image).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_screenshot_editor",
        "description": "Screenshot of the whole editor window incl. all panels (PNG). "
                       "Captured via PrintWindow, so it works even when the editor is covered by other windows.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_scene_info",
        "description": "Scene summary: opened scene file, modified flag, object counts per tool class.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_selection",
        "description": "Objects currently selected in the viewport: name, class, position/rotation/scale.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_list_projects",
        "description": "List recent projects (name + path) known to the editor.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_open_project",
        "description": "Open a project by its folder path (closes the current one if different). "
                       "The project's last scene loads automatically on the next frame.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "project folder path"}},
            "required": ["path"],
        },
    },
    {
        "name": "xfined_open_scene",
        "description": "Load a scene in the active project (name relative to the project's rawdata/levels, or a full path). "
                       "Synchronous — big scenes take a while.",
        "inputSchema": {
            "type": "object",
            "properties": {"scene": {"type": "string", "description": "scene file (.level)"}},
            "required": ["scene"],
        },
    },
    {
        "name": "xfined_list_objects",
        "description": "List scene objects: optional filters 'class' (tool class name from xfined_scene_info), "
                       "'name_contains' (substring), 'limit' (default 100). Returns names for other commands.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "class": {"type": "string"},
                "name_contains": {"type": "string"},
                "limit": {"type": "integer"},
            },
        },
    },
    {
        "name": "xfined_focus_object",
        "description": "Teleport the user's camera to frame an object, exactly like selecting it and pressing F. "
                       "Set select=true to also select the object (clears previous selection, switches active target).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "exact object name"},
                "select": {"type": "boolean", "description": "also select the object (default false)"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "xfined_object_info",
        "description": "Info about a single scene object by its exact name (see xfined_scene_info/selection for names).",
        "inputSchema": {
            "type": "object",
            "properties": {"name": {"type": "string", "description": "exact object name"}},
            "required": ["name"],
        },
    },
    {
        "name": "xfined_mod_manifest",
        "description": "XMS mod manifest (mod.ltx) of the active project: id, name, version, "
                       "target game mode + declared modes, requires/after/before/conflicts lists "
                       "and which module dirs exist.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_mod_set_manifest",
        "description": "Update fields of the active project's mod.ltx. Strings: id (validated, [a-z0-9_.-]+), "
                       "name, version, mode (game mode the module is limited to; empty = all modes). "
                       "provides_mode_id/provides_mode_title declare a new game mode (empty id drops it). "
                       "Comma-joined lists: requires, after, before, conflicts "
                       "(a present-but-empty string clears the list). Omitted fields keep their value.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {"type": "string"},
                "name": {"type": "string"},
                "version": {"type": "string"},
                "mode": {"type": "string", "description": "target game mode id, empty = all modes"},
                "provides_mode_id": {"type": "string", "description": "declare a new game mode (empty = remove)"},
                "provides_mode_title": {"type": "string", "description": "string id for the declared mode title"},
                "requires": {"type": "string"},
                "after": {"type": "string"},
                "before": {"type": "string"},
                "conflicts": {"type": "string"},
            },
        },
    },
    {
        "name": "xfined_mod_export",
        "description": "Export the active project as an XMS module into <target>/mods/<id>/ (default), "
                       "or with flat=true as a plain gamedata_<id> overlay plus a vanilla-compatibility report. "
                       "Existing files in the target are overwritten.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {"type": "string", "description": "target root folder (created if missing)"},
                "flat": {"type": "boolean", "description": "flat gamedata export (default false)"},
            },
            "required": ["target"],
        },
    },
    {
        "name": "xfined_preview_model",
        "description": "Open the model preview window on a model (same as double-clicking it in the content browser). "
                       "source='visual' (editor FS, default) or 'darf' (linked game install). Without 'name' the "
                       "window just opens empty.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "model name or path"},
                "source": {"type": "string", "description": "'visual' (default) or 'darf'"},
            },
        },
    },
    {
        "name": "xfined_reset_layout",
        "description": "Rebuild the default panel arrangement (same as Windows > Reset Layout). Useful when the "
                       "docking layout ends up broken or level_imgui.ini was lost.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_asset_preview_model",
        "description": "Render a preview of a MODEL (.ogf) and return it as a base64 PNG. Game meshes ship no baked "
                       "thumbnail, so the editor renders them offscreen. Use source='visual' for a model the editor "
                       "FS knows by name (e.g. 'dynamics\\\\box\\\\box_1a'), or source='darf' for a path inside the "
                       "linked game install (see darf_list).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "model name or path"},
                "source": {"type": "string", "description": "'visual' (editor FS, default) or 'darf' (linked game install)"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "xfined_mod_export_spawn",
        "description": "Export placed scene objects as an additive spawn layer (spawn/<level>.xspawn in the project). "
                       "This is the composable way to put a model on a base level: the module ships spawn ops, the "
                       "engine merges them into the spawn registry at load, and the level itself is never rewritten - "
                       "so several mods can add things to the same map. Each object becomes op=add with the section, "
                       "world position/direction and a visual override. Re-exporting rewrites only its own entries.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "level": {"type": "string", "description": "level name override (default: opened scene file name)"},
                "selected_only": {"type": "boolean", "description": "false = export every scene object (default true)"},
                "section": {"type": "string", "description": "spawn section for the ops (default physic_object)"},
                "mode": {"type": "string", "description": "game mode gate written on every op (default: none, the module-level gate applies)"},
            },
        },
    },
    {
        "name": "xfined_mod_export_cut",
        "description": "Subtractive level delta: the selected objects are used as VOLUMES, not geometry. Their world "
                       "bounds are written as cut boxes into levels/<level>/overlay.xcform and as hide entries into "
                       "overlay_visuals.ltx, so at load the engine drops base collision triangles inside them and "
                       "detaches base visuals fully inside them. This is how you dig a pit or delete a fence without "
                       "rewriting the level. Add your own geometry with the other exports afterwards.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "level": {"type": "string", "description": "level name override (default: opened scene file name)"},
                "selected_only": {"type": "boolean", "description": "false = use every scene object's bounds (default true)"},
                "overlap": {"type": "boolean", "description": "also detach base visuals that merely touch the box - needed when replacing one big terrain visual (default false)"},
                "grow": {"type": "integer", "description": "grow every box by this many centimetres before cutting (default 0)"},
            },
        },
    },
    {
        "name": "xfined_mod_export_xcform",
        "description": "Bake scene objects into a collision overlay (levels/<level>/overlay.xcform in the project): "
                       "world-space triangles + game materials, merged into the base level's static collision by the engine. "
                       "Defaults: selected objects only, sector 0, level = opened scene name.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "level": {"type": "string", "description": "level name override (default: opened scene file name)"},
                "sector": {"type": "integer", "description": "sector id stamped on all triangles (default 0)"},
                "selected_only": {"type": "boolean", "description": "false = bake every scene object (default true)"},
            },
        },
    },
    {
        "name": "xfined_mod_export_ogf_probe",
        "description": "Export selected static scene objects as world-space .ogf visuals into "
                       "levels/<level>/visuals/ and register them in overlay_visuals.ltx. "
                       "Skeleton/dynamic references are skipped (not world-bakeable).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "level": {"type": "string", "description": "level name override (default: opened scene file name)"},
                "selected_only": {"type": "boolean", "description": "false = export every static scene object (default true)"},
            },
        },
    },
    {
        "name": "xfined_game_link",
        "description": "Report or set the game install the active project is linked to. Without args it "
                       "reports {linked, game_root, reason}; with 'path' it validates and links that folder "
                       "(must contain fsgame.ltx and a database\\ or gamedata\\ subfolder) and mounts its "
                       "content for the DARF Content browser. The game folder is only ever read.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "game folder to link; omit to just report"}},
        },
    },
    {
        "name": "xfined_darf_list",
        "description": "List files of the linked game install: everything inside database\\*.xdb* plus the "
                       "loose gamedata\\ tree, as one virtual gamedata-relative listing. Optional 'filter' "
                       "(case-insensitive substring), 'ext' (extension without the dot, e.g. ogf) and "
                       "'limit' (default 200). Paths returned here are what darf_copy expects.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "filter": {"type": "string"},
                "ext": {"type": "string"},
                "limit": {"type": "integer"},
            },
        },
    },
    {
        "name": "xfined_darf_copy",
        "description": "Copy one file of the linked game install into the project at "
                       "<project>/gamedata/<same relative path>. 'path' as returned by darf_list; "
                       "'overwrite' defaults to false, in which case an existing target is reported "
                       "instead of being replaced. Never writes into the game folder.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "file path as returned by darf_list"},
                "overwrite": {"type": "boolean"},
            },
            "required": ["path"],
        },
    },
    {
        "name": "xfined_scene_stats",
        "description": "scene_info plus selection count and camera position/rotation in one call.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_camera_get",
        "description": "Editor camera state: position, hpb rotation (radians) and view direction.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_camera_set",
        "description": "Move/rotate the editor camera. Position x,y,z and rotation h,p,b (radians); "
                       "omitted fields keep their current value.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
                "h": {"type": "number"}, "p": {"type": "number"}, "b": {"type": "number"},
            },
        },
    },
    {
        "name": "xfined_select_objects",
        "description": "Clear the selection, then select the listed objects by exact name "
                       "(comma-separated). Switches the active tool to the first found object's class. "
                       "Returns how many matched and which names are missing.",
        "inputSchema": {
            "type": "object",
            "properties": {"names": {"type": "string", "description": "comma-separated object names"}},
            "required": ["names"],
        },
    },
    {
        "name": "xfined_deselect_all",
        "description": "Clear the selection in every tool.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_object_set_position",
        "description": "Set an object's position. Omitted components keep their current value. Undo checkpoint included.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "xfined_object_set_rotation",
        "description": "Set an object's rotation in radians (x,y,z Euler). Omitted components keep their value.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "xfined_object_set_scale",
        "description": "Set an object's scale. Omitted components keep their current value.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "xfined_place_object",
        "description": "Place a library object into the scene with a full transform. "
                       "ref = library object path (e.g. 'equipments\\\\item_box'). Position x/y/z, rotation rx/ry/rz "
                       "in radians, scale sx/sy/sz (or 'scale' for uniform); omitted parts keep their default "
                       "(no position at all = a few metres in front of the camera). snap_to_ground settles it on "
                       "the surface below, align_normal also tilts it to that surface.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "ref": {"type": "string", "description": "library object, e.g. props\\\\box_wood_01"},
                "name": {"type": "string", "description": "custom object name (must be unique)"},
                "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
                "rx": {"type": "number"}, "ry": {"type": "number"}, "rz": {"type": "number"},
                "sx": {"type": "number"}, "sy": {"type": "number"}, "sz": {"type": "number"},
                "scale": {"type": "number", "description": "uniform scale shorthand"},
                "snap_to_ground": {"type": "boolean", "description": "drop onto the surface below after placing"},
                "align_normal": {"type": "boolean", "description": "with snap_to_ground: align to the surface normal"},
            },
            "required": ["ref"],
        },
    },
    {
        "name": "xfined_drop_objects",
        "description": "Drop objects onto the surface below them (the editor's End key, same semantics as Unreal's "
                       "Snap to Floor). mode='box' (default) sweeps the whole bounding box, so a wide object rests "
                       "on the rim of a narrow gap instead of falling through; mode='line' traces a single ray from "
                       "the pivot (UE's Alt+End). Without 'names' the current selection is dropped. Undoable.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "names": {"type": "string", "description": "comma-separated object names; empty = selection"},
                "mode": {"type": "string", "description": "'box' (swept bounds, default) or 'line' (pivot ray)"},
                "align_normal": {"type": "boolean", "description": "also rotate to the surface normal (default false)"},
            },
        },
    },
    {
        "name": "xfined_view_mode",
        "description": "Read or change how the viewport renders. Without arguments it returns the current state. "
                       "preset: unlit (the editor's normal textured view, this is the default), "
                       "lit (enables the fixed-function hardware lighting - only 8 light slots exist, so most "
                       "geometry turns black; useful for checking where lights actually reach), wireframe, point. "
                       "fill: solid|wireframe|point; shade: flat|gouraud. Booleans: lighting, textures, edged_faces, "
                       "filter_linear, fog, environment, grid, safe_rect (raw engine flags - 'textures' has no "
                       "visible effect in this render path). Explicit fields override the preset.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "preset": {"type": "string"},
                "fill": {"type": "string"},
                "shade": {"type": "string"},
                "lighting": {"type": "boolean"},
                "textures": {"type": "boolean"},
                "edged_faces": {"type": "boolean"},
                "filter_linear": {"type": "boolean"},
                "fog": {"type": "boolean"},
                "environment": {"type": "boolean"},
                "grid": {"type": "boolean"},
                "safe_rect": {"type": "boolean"},
            },
        },
    },
    {
        "name": "xfined_list_assets",
        "description": "List assets available to the editor. category = one of the content-browser captions "
                       "(Objects, Groups, Visuals, Textures, Textures (raw), Particles, Sounds, Entities, Light Anims); "
                       "default Objects, the placeable ones. Optional 'filter' substring and 'limit' (default 200). "
                       "Names returned here are exactly what place_object's 'ref' expects.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "category": {"type": "string"},
                "filter": {"type": "string"},
                "limit": {"type": "integer"},
            },
        },
    },
    {
        "name": "xfined_asset_preview",
        "description": "Preview thumbnail of a single asset as a PNG image, so its look can be judged before "
                       "placing it. Same category set as list_assets (default Objects).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "asset name from list_assets"},
                "category": {"type": "string"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "xfined_rename_object",
        "description": "Rename a scene object (names are lowercased; the new name must be unique).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "new_name": {"type": "string"},
            },
            "required": ["name", "new_name"],
        },
    },
    {
        "name": "xfined_delete_selected",
        "description": "Delete every selected object (all tools). Returns the removed count; undoable.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_save_scene",
        "description": "Save the opened scene to its current file, or to an explicit 'file' path.",
        "inputSchema": {
            "type": "object",
            "properties": {"file": {"type": "string", "description": "optional target .level path"}},
        },
    },
    {
        "name": "xfined_outliner_show",
        "description": "Open or close the World Outliner panel in the editor. open=false closes it (default true).",
        "inputSchema": {
            "type": "object",
            "properties": {"open": {"type": "boolean", "description": "open the panel (default true)"}},
        },
    },
    {
        "name": "xfined_scene_tree",
        "description": "The World Outliner's data: the scene grouped by object tool class, sorted by class name. "
                       "Each group returns 'class', 'count' (objects matching the filter), 'total' (objects in the "
                       "class) and 'objects' with name/selected/visible. Optional 'filter' (case-insensitive name "
                       "substring, applied across all classes), 'class' (single tool class) and 'limit' per class "
                       "(default 200).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "filter": {"type": "string"},
                "class": {"type": "string"},
                "limit": {"type": "integer"},
            },
        },
    },
    {
        "name": "xfined_undo",
        "description": "Undo the last scene operation.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_redo",
        "description": "Redo the last undone scene operation.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]

CMD_MAP = {
    "xfined_ping": "ping",
    "xfined_state": "state",
    "xfined_screenshot_viewport": "screenshot_viewport",
    "xfined_screenshot_editor": "screenshot_editor",
    "xfined_scene_info": "scene_info",
    "xfined_selection": "selection",
    "xfined_object_info": "object_info",
    "xfined_list_projects": "list_projects",
    "xfined_open_project": "open_project",
    "xfined_open_scene": "open_scene",
    "xfined_list_objects": "list_objects",
    "xfined_focus_object": "focus_object",
    "xfined_mod_manifest": "mod_manifest",
    "xfined_mod_set_manifest": "mod_set_manifest",
    "xfined_mod_export": "mod_export",
    "xfined_preview_model": "preview_model",
    "xfined_reset_layout": "reset_layout",
    "xfined_asset_preview_model": "asset_preview",
    "xfined_mod_export_spawn": "mod_export_spawn",
    "xfined_mod_export_cut": "mod_export_cut",
    "xfined_mod_export_xcform": "mod_export_xcform",
    "xfined_mod_export_ogf_probe": "mod_export_ogf_probe",
    "xfined_game_link": "game_link",
    "xfined_darf_list": "darf_list",
    "xfined_darf_copy": "darf_copy",
    "xfined_scene_stats": "scene_stats",
    "xfined_camera_get": "camera_get",
    "xfined_camera_set": "camera_set",
    "xfined_select_objects": "select_objects",
    "xfined_deselect_all": "deselect_all",
    "xfined_object_set_position": "object_set_position",
    "xfined_object_set_rotation": "object_set_rotation",
    "xfined_object_set_scale": "object_set_scale",
    "xfined_place_object": "place_object",
    "xfined_drop_objects": "drop_objects",
    "xfined_view_mode": "view_mode",
    "xfined_list_assets": "list_assets",
    "xfined_asset_preview": "asset_preview",
    "xfined_rename_object": "rename_object",
    "xfined_delete_selected": "delete_selected",
    "xfined_save_scene": "save_scene",
    "xfined_outliner_show": "outliner_show",
    "xfined_scene_tree": "scene_tree",
    "xfined_undo": "undo",
    "xfined_redo": "redo",
}


def editor_call(cmd: str, extra: dict | None = None) -> dict:
    payload = {"cmd": cmd}
    if extra:
        payload.update(extra)
    try:
        with socket.create_connection((HOST, PORT), timeout=20) as s:
            s.sendall((json.dumps(payload) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(1 << 20)
                if not chunk:
                    break
                buf += chunk
        return json.loads(buf.decode())
    except (OSError, json.JSONDecodeError) as e:
        return {"ok": False, "error": f"editor not reachable on {HOST}:{PORT} ({e})"}


def tool_result(name: str, args: dict) -> dict:
    data = editor_call(CMD_MAP[name], args)
    png = data.pop("png_base64", None)
    content = []
    if png and data.get("ok"):
        content.append({"type": "image", "data": png, "mimeType": "image/png"})
        content.append({"type": "text", "text": "screenshot captured"})
    else:
        content.append({"type": "text", "text": json.dumps(data, ensure_ascii=False)})
    return {"content": content, "isError": not data.get("ok", False)}


def reply(msg_id, result=None, error=None):
    out = {"jsonrpc": "2.0", "id": msg_id}
    if error is not None:
        out["error"] = error
    else:
        out["result"] = result
    sys.stdout.write(json.dumps(out) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        method = msg.get("method", "")
        msg_id = msg.get("id")

        if method == "initialize":
            reply(msg_id, {
                "protocolVersion": msg.get("params", {}).get("protocolVersion", "2024-11-05"),
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "xfined-editor", "version": "1.0.0"},
            })
        elif method == "notifications/initialized":
            pass
        elif method == "tools/list":
            reply(msg_id, {"tools": TOOLS})
        elif method == "tools/call":
            params = msg.get("params", {})
            name = params.get("name", "")
            if name in CMD_MAP:
                reply(msg_id, tool_result(name, params.get("arguments") or {}))
            else:
                reply(msg_id, error={"code": -32602, "message": f"unknown tool {name}"})
        elif msg_id is not None:
            reply(msg_id, error={"code": -32601, "message": f"unknown method {method}"})


if __name__ == "__main__":
    main()
