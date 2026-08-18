#!/usr/bin/env python3
"""MCP stdio bridge for the XFined Editor.

The editor hosts a tiny line-based JSON endpoint on 127.0.0.1:28016 (alive from
the first frame, project browser included). This script exposes it as a
standard MCP server over stdio — no third-party dependencies, pure stdlib.

Register (Claude Code):  claude mcp add -s user xfined-editor -- python <this file>
"""
import json
import os
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 28016
CONNECT_TIMEOUT_SECONDS = 20.0


def _request_timeout_seconds() -> float:
    raw = os.environ.get("XFINED_MCP_CLIENT_TIMEOUT_MS", os.environ.get("XFINED_MCP_REQUEST_TIMEOUT_MS", "180000"))
    try:
        return max(5.0, float(raw) / 1000.0 + 5.0)
    except ValueError:
        return 185.0


REQUEST_TIMEOUT_SECONDS = _request_timeout_seconds()

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
                       "Synchronous — big scenes take a while. Unsaved scene changes are preserved unless discard=true is explicit.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "scene": {"type": "string", "description": "scene file (.level)"},
                "discard": {
                    "type": "boolean",
                    "description": "explicitly discard unsaved scene changes after the replacement scene validates; default false",
                },
            },
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
                       "provides_mode_id/provides_mode_title declare a new game mode (empty id drops it) - "
                       "export then generates the checkbox that puts it on the new-game screen. "
                       "target states which of the three the module is built for and is REQUIRED before "
                       "an export succeeds; it is inferred from mode/provides_mode when omitted, but "
                       "'the ordinary game' has to be said out loud (target=default). "
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
                "provides_mode_title": {"type": "string", "description": "the mode name the player will see"},
                "target": {"type": "string", "enum": ["default", "existing", "new"],
                           "description": "what this module is built for: the ordinary game, a mode already "
                                          "in the linked game, or a mode it brings itself"},
                "requires": {"type": "string"},
                "after": {"type": "string"},
                "before": {"type": "string"},
                "conflicts": {"type": "string"},
            },
        },
    },
    {
        "name": "xfined_mod_export",
        "description": "Export the active project as an XMS module into <target>/modules/<id>/ (default), "
                       "or with flat=true as a plain gamedata_<id> overlay plus a vanilla-compatibility report. "
                       "Without 'target' the module deploys to the LINKED GAME (game_link): <game>/modules/<id>. "
                       "NOT mods/ - that one is JSGME's folder, and a module in it is a module "
                       "JSGME offers to copy over the game. "
                       "The module export is a CLEAN build: the target module folder is emptied first, so a file "
                       "the project no longer has cannot survive there; "
                       "a target inside the game's gamedata is refused - mods never merge into the game tree.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {"type": "string", "description": "target root folder (created if missing); omit = linked game root"},
                "flat": {"type": "boolean", "description": "flat gamedata export (default false)"},
            },
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
        "name": "xfined_theme",
        "description": "Read or change the editor theme. action='get' reports the current and available presets; "
                       "action='set' persists preset='xfined-purple' or 'graphite'; action='reset' restores the "
                       "XFined Purple default. Omitting action reads the current theme.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["get", "set", "reset"]},
                "preset": {"type": "string", "enum": ["xfined-purple", "graphite"]},
            },
        },
    },
    {
        "name": "xfined_frame_pacing",
        "description": "Read live editor frame-pacing statistics or persist the active idle cap. The idle cap only "
                       "applies outside Play In Editor and realtime rendering; window messages, redraw work, and MCP "
                       "requests wake it immediately. action='set' accepts active_idle_fps=30..240, reset restores 120.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["get", "stats", "set", "reset"]},
                "active_idle_fps": {"type": "integer", "minimum": 30, "maximum": 240},
            },
        },
    },
    {
        "name": "xfined_command_palette",
        "description": "Search, open, close, inspect, or execute the editor command palette. action='query' returns "
                       "the same ranked results shown by Ctrl+Shift+P. action='execute' accepts the returned numeric "
                       "command/subcommand pair (preferred) or an exact id. Omitting action uses query when query is "
                       "present, otherwise reports palette state.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["state", "query", "open", "close", "execute"]},
                "query": {"type": "string"},
                "limit": {"type": "integer", "minimum": 1, "maximum": 250},
                "id": {"type": "string", "description": "exact id returned by query"},
                "command": {"type": "integer", "minimum": 0},
                "subcommand": {"type": "integer", "minimum": 0},
            },
        },
    },
    {
        "name": "xfined_viewport_navigation",
        "description": "Read or control the viewport camera widget. Set one of the six canonical axis views or "
                       "return to the free perspective camera; action='frame' focuses all objects or the selection.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["get", "set", "frame"]},
                "view": {"type": "string", "enum": ["perspective", "front", "back", "left", "right", "top", "bottom"]},
                "scope": {"type": "string", "enum": ["all", "selection"]},
            },
        },
    },
    {
        "name": "xfined_progress",
        "description": "Read the editor's non-modal task center, including nested task progress and elapsed time, "
                       "or request cooperative cancellation when cancelable=true. Scene loading stays live but "
                       "non-cancelable because it has no rollback transaction. Omitting action reads state.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["get", "cancel"]},
            },
        },
    },
    {
        "name": "xfined_property_inspector",
        "description": "Drive the Properties or World Properties panel: read counts/filter/open state, set a search "
                       "filter, expand or collapse all matching branches, or open/close the panel.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["get", "filter", "expand", "collapse", "open", "close"]},
                "target": {"type": "string", "enum": ["selection", "world"]},
                "filter": {"type": "string"},
            },
        },
    },
    {
        "name": "xfined_content_browser_navigation",
        "description": "Inspect or drive Content Browser history, breadcrumbs, Back/Forward/Up/Home, and session "
                       "favorite folders. Location state includes source, category, and source-relative folder.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["get", "navigate", "back", "forward", "up", "home", "favorite_add", "favorite_remove", "favorite_open"]},
                "source": {
                    "description": "project/editor/darf or 0/1/2",
                    "oneOf": [
                        {"type": "string", "enum": ["project", "editor", "darf"]},
                        {"type": "integer", "enum": [0, 1, 2]},
                    ],
                },
                "category": {
                    "description": "editor asset category name or index",
                    "oneOf": [
                        {
                            "type": "string",
                            "enum": [
                                "Objects", "Levels", "Groups", "Visuals", "Textures", "Textures (raw)",
                                "Particles", "Sounds", "Entities", "Light Anims",
                            ],
                        },
                        {"type": "integer", "enum": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]},
                    ],
                },
                "folder": {"type": "string", "description": "source-relative folder; empty means root"},
                "index": {"type": "integer", "minimum": 0, "description": "favorite index returned by get"},
            },
        },
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
        "name": "xfined_content_copy",
        "description": "Copy files or folders INTO the project. 'src' is a file/folder path or a ';'-separated "
                       "list of them; relative paths are taken against the project root, absolute paths are "
                       "allowed and a src that is not on disk is looked up in the linked game's archives "
                       "(same paths darf_list returns). 'dst' is the destination FOLDER and must be inside the "
                       "project — writing anywhere else is refused, including via '..'. recursive defaults to "
                       "true, overwrite to false. Returns real counts plus per-item 'errors' and 'skips'.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "src": {"type": "string", "description": "file/folder, or several separated by ';'"},
                "dst": {"type": "string", "description": "destination folder inside the project"},
                "recursive": {"type": "boolean"},
                "overwrite": {"type": "boolean"},
            },
            "required": ["src", "dst"],
        },
    },
    {
        "name": "xfined_content_move",
        "description": "Move files or folders inside the project. Same arguments as content_copy, but BOTH src "
                       "and dst must be inside the project: the game install and the shared Editor Content are "
                       "read-only sources, so moving out of them (which would delete from them) is refused with "
                       "an explanation. recursive defaults to true, overwrite to false.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "src": {"type": "string", "description": "file/folder, or several separated by ';'"},
                "dst": {"type": "string", "description": "destination folder inside the project"},
                "recursive": {"type": "boolean"},
                "overwrite": {"type": "boolean"},
            },
            "required": ["src", "dst"],
        },
    },
    {
        "name": "xfined_content_delete",
        "description": "Delete files or folders inside the project. 'path' is one entry or a ';'-separated list. "
                       "recursive defaults to false, so a non-empty folder is reported instead of being wiped. "
                       "Anything outside the project root — and the project root itself — is refused.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "file/folder, or several separated by ';'"},
                "recursive": {"type": "boolean"},
            },
            "required": ["path"],
        },
    },
    {
        "name": "xfined_list_commands",
        "description": "Introspect the editor's own command registry: every action the menus and shortcuts can "
                       "perform, as {id, name, menu, presets}. 'name' is the COMMAND_* identifier, 'menu' the "
                       "menu path when it has one, 'presets' the documented (p1,p2) combinations of multi-variant "
                       "commands. Feed these to xfined_exec_command. This is the full control surface of the "
                       "editor - commands added in future builds show up here automatically.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_exec_command",
        "description": "Execute any command from the editor's registry (see xfined_list_commands) - the same "
                       "dispatch the menus and keyboard shortcuts use, so this can do anything a human at the "
                       "keyboard can. Address it by 'id', by COMMAND_* 'name', or by menu path. Parameters are "
                       "explicit: p1s/p2s pass strings, p1i/p2i integers. Prefer the dedicated xfined_* tools "
                       "when one exists (they validate and report better); this is the escape hatch for "
                       "everything else. Note some commands open file pickers or other windows - take a "
                       "screenshot afterwards to see what happened.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {"type": "integer", "description": "command id from list_commands"},
                "name": {"type": "string", "description": "COMMAND_* identifier or menu path (alternative to id)"},
                "p1s": {"type": "string", "description": "first parameter as a string"},
                "p1i": {"type": "integer", "description": "first parameter as an integer"},
                "p2s": {"type": "string", "description": "second parameter as a string"},
                "p2i": {"type": "integer", "description": "second parameter as an integer"},
            },
        },
    },
    {
        "name": "xfined_content_mkdir",
        "description": "Create a folder inside the project, with any missing parent along the way. 'path' is "
                       "taken against the project root when relative (e.g. 'gamedata/meshes/mymod'). Anything "
                       "that resolves outside the project root is refused. Creating a folder that already "
                       "exists succeeds and changes nothing.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "folder to create, relative to the project root"},
            },
            "required": ["path"],
        },
    },
    {
        "name": "xfined_content_browser_open",
        "description": "Reveal an asset OR a folder in the editor's Content Browser: opens the panel, switches "
                       "source when asked, navigates there and selects it. 'name' is an asset as returned by "
                       "list_assets (or darf_list for the game source), or a folder path like 'levels' or "
                       "'levels\\\\l07_military' - the only way to walk the grid into a folder without a mouse. "
                       "'source' is project / editor / darf (omit to stay on the current tab). open=true also "
                       "opens the asset's viewer, the same thing a double click does (ignored for folders).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "asset name from list_assets / darf_list, or a folder path"},
                "source": {"type": "string", "enum": ["project", "editor", "darf"]},
                "open": {"type": "boolean", "description": "also open the viewer (default false)"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "xfined_content_browser_selection",
        "description": "What the Content Browser is showing right now: whether it is open, the active source, "
                       "the current folder and the ordered selection. With action=select/toggle/range/clear it "
                       "also applies normal file-manager selection semantics against the complete filtered view, "
                       "including virtualized offscreen rows. select establishes the anchor; range extends from it. "
                       "Names must match an entry in the current view exactly. Omit action (or use get) to only read.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["get", "select", "toggle", "range", "clear"]},
                "name": {"type": "string", "description": "exact asset or folder path for select/toggle/range"},
            },
        },
    },
    {
        "name": "xfined_content_browser_copy",
        "description": "Copy assets out of a READ-ONLY source (Editor Content = the shared SDK library, or DARF "
                       "Content = the linked game install) into the open project. Pass 'names' (';'-separated, as "
                       "returned by list_assets / darf_list) for individual items, or 'folder' to take everything "
                       "under it including subfolders. Each asset lands where the engine expects it: an Editor "
                       "Content object goes to <project>/rawdata/objects/..., a mesh to <project>/gamedata/meshes/..., "
                       "and a texture brings its .thm along. Never prompts - 'overwrite' decides (default true).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "names": {"type": "string", "description": "';'-separated asset names from list_assets / darf_list"},
                "folder": {"type": "string", "description": "copy this folder recursively instead of individual names"},
                "dst": {"type": "string", "description": "project-relative target folder (e.g. 'gamedata/meshes/mymod'). Omit to mirror each asset into the layout fs.ltx implies instead."},
                "source": {"type": "string", "enum": ["editor", "darf"], "description": "omit to use the current tab"},
                "category": {"type": "string", "description": "Editor Content category caption, e.g. 'Objects' (omit to use the current one)"},
                "overwrite": {"type": "boolean", "description": "replace files already in the project (default true)"},
            },
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
        "name": "xfined_mark_authored",
        "description": "Flip the mod-content flag on the SELECTED objects. The mod build ships only "
                       "authored objects; everything an imported base level brought stays base. "
                       "authored=false relabels back to base level.",
        "inputSchema": {
            "type": "object",
            "properties": {"authored": {"type": "boolean", "description": "default true"}},
        },
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
        "name": "xfined_list_spawn_sections",
        "description": "The gameplay-object roster: every config section of the LINKED game that declares $spawn - "
                       "actors, NPC, items, restrictors/triggers, anomalies, physics. This is what the old SDK's "
                       "'Spawn Element' panel listed. Filter is a substring over section and group; names feed "
                       "xfined_place_spawn.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "filter": {"type": "string"},
                "limit": {"type": "integer"},
            },
        },
    },
    {
        "name": "xfined_place_spawn",
        "description": "Place a GAMEPLAY object (spawn element) by its config section (see list_spawn_sections): "
                       "the server entity is built from the section, shapes the section declares are attached, and "
                       "its properties (logic/custom_data included) are editable in the Properties panel. Optional "
                       "x/y/z (default: in front of the camera), snap_to_ground, name.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "section": {"type": "string", "description": "config section, e.g. stalker or space_restrictor"},
                "x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
                "snap_to_ground": {"type": "boolean"},
            },
            "required": ["section"],
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
        "name": "xfined_recovery",
        "description": "Inspect or operate project-scoped crash recovery. action=get reports ownership, dirty state, "
                       "availability, pending operation id, source/snapshot paths and errors. action=snapshot creates a "
                       "synchronous non-mutating snapshot without changing the scene file, name, recents or dirty flag. "
                       "restore/discard only act on the validated startup offer and never overwrite the source scene; "
                       "they are deferred to the editor draw loop and may return pending.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["get", "snapshot", "restore", "discard"],
                    "description": "default: get",
                },
            },
            "additionalProperties": False,
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
        "name": "xfined_game_modes",
        "description": "Game modes a module can be built for, scanned from the LINKED game install. "
                       "Dead Air has no config listing them: they are checkboxes in "
                       "scripts/ui_mm_faction_select.script laid out in configs/ui/ui_mm_faction_select_16.xml, "
                       "where the left column holds campaigns and the right one holds rule options "
                       "(one life, richer stashes, ...). Only campaigns are returned, plus modes declared "
                       "by XMS modules installed in that game. Each entry has id, caption (what the player "
                       "sees), key ([character_creation] key) and source.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_outliner_filter",
        "description": "Drive the World Outliner's search box and type funnel, and read back what survives. "
                       "'text' uses the Unreal search grammar: every space-separated word has to match, "
                       "-word excludes, \"two words\" match together. 'types' is a ';'-separated list of the "
                       "object classes to SHOW (empty string shows all; see scene_tree for the names). "
                       "'selected_only' limits the tree to the current selection; 'visibility' is all, visible, "
                       "or hidden. Omitted arguments keep their current value. Invalid types or visibility leave "
                       "the previous filter intact. Returns text, selected_only, hidden_types, visibility, shown "
                       "and total.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "search terms; empty string clears the search"},
                "types": {"type": "string", "description": "';'-separated classes to show, e.g. 'way;spawn'"},
                "selected_only": {"type": "boolean", "description": "show only selected objects"},
                "visibility": {
                    "type": "string",
                    "enum": ["all", "visible", "hidden"],
                    "description": "visibility filter; omitted keeps the current mode",
                },
            },
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
    # ---- quest graph (NQ) ------------------------------------------------------
    # One .nqasset = one quest = declarative Lua (`return { ... }`), see
    # docs/NQ_ARCHITECTURE.md. Structured arguments travel as STRINGS holding
    # Lua table text; every response carries {"ok":...}, problems as
    # [{code,severity,node,slot,message}] with the codes of §15 (E = blocks the
    # build, W = warning) and an outline (text description of the graph).
    {
        "name": "xfined_quest_catalog",
        "description": "The quest kind catalog: every trigger / main action / extra action / condition kind with "
                       "group, title, desc, use, params (name, type, required, default, min/max, enum), pins, "
                       "wait/once (main), event (cond), plus catalog version and source ('game' = read from the "
                       "linked game install, 'bundled' = the editor's fallback copy). Read it before writing a quest.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_quest_list",
        "description": "Every *.nqasset of the project: path, id, title, node count, error/warning counts, open/dirty "
                       "state (files that do not parse come back with readable=false and the parse error).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "xfined_quest_new",
        "description": "Create a quest file from the minimal template (trigger.start -> flow.end; id = file name, "
                       "title 'Новый квест') at 'path' (project-relative, '.nqasset' appended when missing; refuses to "
                       "overwrite) and open it. Returns the canonical text, outline and problems.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string", "description": "e.g. quests/wolf_debt.nqasset"}},
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_open",
        "description": "Open a quest document (loads the file, lays out nodes without pos, validates). Returns id, "
                       "problems, outline. Opening an already open document just returns its state.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_close",
        "description": "Close an open quest document. A dirty document answers error 'unsaved' unless discard=true.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}, "discard": {"type": "boolean"}},
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_get",
        "description": "Read a quest as text: 'lua' (canonical file text), 'outline' (one line per node in flow order "
                       "plus the problems), 'problems', counts. Works for files that are not open (read from disk).",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_write",
        "description": "Replace a quest with the whole file text 'lua' (return { nq = 1, id = ..., nodes = { ... } }). "
                       "Open document: replaced in memory with an undo snapshot (saved=false, call xfined_quest_save). "
                       "Closed or new file: the canonical text is written to disk when it parses (saved=true). "
                       "Validation problems are returned, they do not block the write - only the build refuses errors.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "lua": {"type": "string", "description": "complete .nqasset text"},
            },
            "required": ["path", "lua"],
        },
    },
    {
        "name": "xfined_quest_undo",
        "description": "Undo the last change of an open quest document (its own undo ring, separate from the scene).",
        "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    },
    {
        "name": "xfined_quest_redo",
        "description": "Redo the last undone change of an open quest document.",
        "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    },
    {
        "name": "xfined_quest_apply",
        "description": "Apply operations to a quest (opens the document when needed; all-or-nothing, one undo step). "
                       "'ops' is Lua text of a list: { { op = \"add_node\", node = { id = ..., kind = ..., params = { ... }, "
                       "out = { next = \"x\" } } }, { op = \"set_node\", id = ..., patch = { kind/once/params/cond/on_enter/"
                       "on_exit/out/pos/comment } }, { op = \"rename_node\", id, new_id } (updates references), "
                       "{ op = \"remove_node\", id } (drops edges), { op = \"connect\", from, pin, to }, "
                       "{ op = \"disconnect\", from, pin[, to] }, { op = \"add_action\", id, slot = \"enter\"|\"exit\"[, index], "
                       "action = { kind, params } }, { op = \"set_action\", id, slot, index, patch = { kind/params } }, "
                       "{ op = \"move_action\", id, slot, index[, to_id][, to_slot][, to_index] }, "
                       "{ op = \"remove_action\", id, slot, index }, { op = \"set_quest\", patch = { id/title/activation/"
                       "vars/tasks } }, { op = \"set_pos\", id, x, y } }. Indexes are 1-based. Returns problems and outline; "
                       "the document stays open and dirty (xfined_quest_save writes it).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "ops": {"type": "string", "description": "Lua table text with the operation list"},
            },
            "required": ["path", "ops"],
        },
    },
    {
        "name": "xfined_quest_validate",
        "description": "Validate a quest (open document or file on disk): problems with codes E001-E050 (block the "
                       "build) and W011-W070 (warnings), plus outline.",
        "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    },
    {
        "name": "xfined_quest_save",
        "description": "Write an open quest document to its file (canonical text). Refuses with 'modified externally' "
                       "when the file changed on disk since it was loaded, unless force=true.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}, "force": {"type": "boolean"}},
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_reload",
        "description": "Re-read an open quest document from disk (drops unsaved changes and the undo history).",
        "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    },
    {
        "name": "xfined_quest_layout",
        "description": "Deterministic pin-aware top-down graph layout. Children follow the visible canvas pin order; "
                       "complete nested branch bounds stay apart, exclusive trigger trees use a larger gap, and "
                       "shared descendants become merge blocks between their incoming branches. "
                       "all=false places only nodes without pos. Returns the positions; the document stays dirty.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}, "all": {"type": "boolean"}},
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_view",
        "description": "Open (or focus) the graph tab of a quest and set its view: frame='all' or a node id, "
                       "zoom_level 0..9 (6 = 100%), cx/cy = world centre (cx/cy override 'frame'), "
                       "slot='enter:0'/'exit:1' opens that action of the framed node in the inspector ('none' clears it). "
                       "The bridge waits for deferred frame-all layout before returning when the editor is drawing. "
                       "Then use xfined_screenshot_editor to look at it.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}, "frame": {"type": "string"}, "zoom_level": {"type": "integer"},
                           "cx": {"type": "number"}, "cy": {"type": "number"}, "slot": {"type": "string"}},
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_find",
        "description": "Graph-wide node search over ids, kind ids/titles, parameters, conditions, actions, links and "
                       "comments. Terms are ANDed; quoted phrases stay together; prefix a term with '-' to exclude it. "
                       "action=get|set|next|previous|clear; get with query atomically updates without navigating, "
                       "set can select the first result, next/previous frame a result. "
                       "Returns deterministic file-order results plus the active result and exact graph view state.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "action": {"type": "string", "enum": ["get", "set", "next", "previous", "clear"]},
                "query": {"type": "string", "description": "For get/set/next/previous, replace the current query before returning or navigating."},
                "select": {"type": "boolean", "description": "For action=set, frame the first result."},
                "limit": {"type": "integer", "description": "Maximum returned results, 1..500 (default 100)."},
            },
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_project_find",
        "description": "Read-only full-project node search across every sorted .nqasset without opening documents. "
                       "Uses the same quoted, negative-term and case-folding semantics as xfined_quest_find. Dirty "
                       "open documents overlay their disk file. Results report source=disk|memory; complete=false "
                       "and diagnostics identify files whose content could not be inspected.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string"},
                "limit": {"type": "integer", "description": "Maximum returned results, 1..1000 (default 200)."},
            },
            "required": ["query"],
        },
    },
    {
        "name": "xfined_quest_references",
        "description": "Find exact references whose parameter type is declared by the active NQ catalog. "
                       "Pass path to limit the scan to one quest (required in practice for document-scoped task_id). "
                       "Nested cond_list/cases_cond are inspected. Unknown kinds/params, malformed values, Lua and "
                       "unreadable files make complete=false with exact diagnostics; no identifier is guessed from "
                       "a parameter name.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "type": {"type": "string", "description": "Catalog parameter type, for example task_id."},
                "value": {"type": "string", "description": "Exact value to find."},
                "path": {"type": "string", "description": "Optional project-relative quest path scope."},
                "limit": {"type": "integer", "description": "Maximum returned references, 1..2000 (default 500)."},
            },
            "required": ["type", "value"],
        },
    },
    {
        "name": "xfined_quest_rename_task",
        "description": "Rename one task declaration and every proven catalog-typed task_id reference in the same "
                       "quest as one undo step. Refuses before creating an undo snapshot when coverage is incomplete "
                       "(unknown kind/parameter, malformed nested data, custom Lua, or partial source). A task id is "
                       "persisted in runtime/save state, so ack_runtime_identity must be explicitly true.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "from": {"type": "string"},
                "to": {"type": "string"},
                "ack_runtime_identity": {
                    "type": "boolean",
                    "description": "Must be true: existing saves/running state under the old task id are not migrated.",
                },
            },
            "required": ["path", "from", "to", "ack_runtime_identity"],
        },
    },
    {
        "name": "xfined_quest_bookmarks",
        "description": "Read or control transient per-document node bookmarks. action=get|add|remove|toggle|next|"
                       "previous|clear. add/remove/toggle use 'node', or the sole selected node when omitted. Bookmarks "
                       "are editor-only and never change the .nqasset or its undo/dirty state.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "action": {"type": "string", "enum": ["get", "add", "remove", "toggle", "next", "previous", "clear"]},
                "node": {"type": "string"},
            },
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_history",
        "description": "Read or navigate the transient graph view history. action=get|back|forward|clear. Each state "
                       "contains world center, zoom level, selected node ids and inspector slot; no quest data changes.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "action": {"type": "string", "enum": ["get", "back", "forward", "clear"]},
            },
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_minimap",
        "description": "Read or control the transient interactive graph minimap with action=get|show|hide|toggle. "
                       "Returns visibility, graph bounds, current viewport bounds, center, zoom and selection.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "action": {"type": "string", "enum": ["get", "show", "hide", "toggle"]},
            },
            "required": ["path"],
        },
    },
    {
        "name": "xfined_quest_lookup",
        "description": "Search the game data behind the parameter pickers: type = item_section | squad_section | "
                       "level | smart | story_id | profile | community | info | spot_type (from the linked game, cached), "
                       "or the document-scoped task_id | var_name | ref_name | node_id (needs 'path') | quest_id "
                       "(quests of the project). 'query' is a case-insensitive substring of id or name; 'limit' default 50.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "type": {"type": "string"},
                "query": {"type": "string"},
                "limit": {"type": "integer"},
                "path": {"type": "string", "description": "quest path for document-scoped types"},
            },
            "required": ["type"],
        },
    },
    {
        "name": "xfined_quest_check_all",
        "description": "The build gate: validate every quest of the project like xfined_mod_export does. Returns "
                       "passed (no errors), error/warning counts and the log lines ('<path>: <code> <node>: <message>').",
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
    "xfined_theme": "theme",
    "xfined_frame_pacing": "frame_pacing",
    "xfined_command_palette": "command_palette",
    "xfined_viewport_navigation": "viewport_navigation",
    "xfined_progress": "progress",
    "xfined_property_inspector": "property_inspector",
    "xfined_content_browser_navigation": "content_browser_navigation",
    "xfined_asset_preview_model": "asset_preview",
    "xfined_mod_export_spawn": "mod_export_spawn",
    "xfined_mod_export_cut": "mod_export_cut",
    "xfined_mod_export_xcform": "mod_export_xcform",
    "xfined_mod_export_ogf_probe": "mod_export_ogf_probe",
    "xfined_game_link": "game_link",
    "xfined_darf_list": "darf_list",
    "xfined_darf_copy": "darf_copy",
    "xfined_content_copy": "content_copy",
    "xfined_content_move": "content_move",
    "xfined_content_delete": "content_delete",
    "xfined_content_mkdir": "content_mkdir",
    "xfined_list_commands": "list_commands",
    "xfined_exec_command": "exec_command",
    "xfined_content_browser_open": "content_browser_open",
    "xfined_content_browser_selection": "content_browser_selection",
    "xfined_content_browser_copy": "content_browser_copy",
    "xfined_scene_stats": "scene_stats",
    "xfined_camera_get": "camera_get",
    "xfined_camera_set": "camera_set",
    "xfined_select_objects": "select_objects",
    "xfined_deselect_all": "deselect_all",
    "xfined_mark_authored": "mark_authored",
    "xfined_object_set_position": "object_set_position",
    "xfined_object_set_rotation": "object_set_rotation",
    "xfined_object_set_scale": "object_set_scale",
    "xfined_place_object": "place_object",
    "xfined_drop_objects": "drop_objects",
    "xfined_list_spawn_sections": "list_spawn_sections",
    "xfined_place_spawn": "place_spawn",
    "xfined_view_mode": "view_mode",
    "xfined_list_assets": "list_assets",
    "xfined_asset_preview": "asset_preview",
    "xfined_rename_object": "rename_object",
    "xfined_delete_selected": "delete_selected",
    "xfined_save_scene": "save_scene",
    "xfined_recovery": "recovery",
    "xfined_outliner_show": "outliner_show",
    "xfined_outliner_filter": "outliner_filter",
    "xfined_game_modes": "game_modes",
    "xfined_scene_tree": "scene_tree",
    "xfined_undo": "undo",
    "xfined_redo": "redo",
    "xfined_quest_catalog": "quest_catalog",
    "xfined_quest_list": "quest_list",
    "xfined_quest_new": "quest_new",
    "xfined_quest_open": "quest_open",
    "xfined_quest_close": "quest_close",
    "xfined_quest_get": "quest_get",
    "xfined_quest_write": "quest_write",
    "xfined_quest_undo": "quest_undo",
    "xfined_quest_redo": "quest_redo",
    "xfined_quest_apply": "quest_apply",
    "xfined_quest_validate": "quest_validate",
    "xfined_quest_save": "quest_save",
    "xfined_quest_reload": "quest_reload",
    "xfined_quest_layout": "quest_layout",
    "xfined_quest_view": "quest_view",
    "xfined_quest_find": "quest_find",
    "xfined_quest_project_find": "quest_project_find",
    "xfined_quest_references": "quest_references",
    "xfined_quest_rename_task": "quest_rename_task",
    "xfined_quest_bookmarks": "quest_bookmarks",
    "xfined_quest_history": "quest_history",
    "xfined_quest_minimap": "quest_minimap",
    "xfined_quest_lookup": "quest_lookup",
    "xfined_quest_check_all": "quest_check_all",
}


def editor_call(cmd: str, extra: dict | None = None) -> dict:
    payload = {"cmd": cmd}
    if extra:
        payload.update(extra)
    try:
        with socket.create_connection((HOST, PORT), timeout=CONNECT_TIMEOUT_SECONDS) as s:
            s.settimeout(REQUEST_TIMEOUT_SECONDS)
            s.sendall((json.dumps(payload) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(1 << 20)
                if not chunk:
                    break
                buf += chunk
        try:
            text = buf.decode("utf-8")
        except UnicodeDecodeError as e:
            return {
                "ok": False,
                "error": f"editor returned malformed UTF-8 at response byte {e.start}",
            }
        return json.loads(text)
    except OSError as e:
        return {"ok": False, "error": f"editor not reachable on {HOST}:{PORT} ({e})"}
    except json.JSONDecodeError as e:
        return {"ok": False, "error": f"editor returned malformed JSON ({e})"}


def tool_result(name: str, args: dict) -> dict:
    data = editor_call(CMD_MAP[name], args)
    if name == "xfined_quest_view" and data.get("ok") and data.get("pending"):
        deadline = time.monotonic() + 1.0
        while data.get("pending") and time.monotonic() < deadline:
            time.sleep(0.02)
            data = editor_call("quest_view", {"path": args["path"]})
            if not data.get("ok"):
                break
    if name == "xfined_recovery" and args.get("action", "get") in ("restore", "discard") \
            and data.get("ok") and data.get("pending"):
        operation_id = data.get("operation_id")
        deadline = time.monotonic() + 3.0
        while data.get("pending") and time.monotonic() < deadline:
            time.sleep(0.02)
            current = editor_call("recovery", {"action": "get"})
            if not current.get("ok"):
                data = current
                break
            if current.get("operation_id") != operation_id:
                data = current
                data["ok"] = False
                data["error"] = "recovery operation was superseded before completion"
                break
            data = current
        if data.get("state") == "error":
            data["ok"] = False
            data["error"] = data.get("last_error") or "recovery operation failed"
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
    # MCP speaks UTF-8; a Windows console hands Python cp1251 unless told
    # otherwise, which turned every non-latin argument (a game mode's name, a
    # search term) into mojibake on the way in and back out again.
    for stream in (sys.stdin, sys.stdout):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

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
