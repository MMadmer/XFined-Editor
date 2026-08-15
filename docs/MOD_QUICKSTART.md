# Building and shipping a mod

Short version: press **Build Mod** (or `Ctrl+B`). Everything below is why that works
and what to do when it refuses.

---

## 1. Once per project

**Mod → Edit Manifest…** — this is the only mandatory setup.

| Field | What it is |
|---|---|
| `id` | Folder name in the game (`a-z 0-9 _ . -`). The module lands in `<game>\modules\<id>`. |
| `name`, `version` | Shown to the player. |
| **Target mode** | **Required.** Who gets the module's level work: **the ordinary game** = only when no campaign mode is active (Revolution II won't see it); **a mode already in the game** = only that campaign; **a NEW mode** = the campaign this module itself adds; **every game and every mode** = no gate at all — use only for things that belong everywhere. Without a choice the build refuses. |

If you declare a **new mode**, the export also generates the new-game-screen
checkbox for it (the xml patch, the text entry and `mode_register.script`).
Those files are machinery, not content — the content browser hides them so you
can't delete one by accident, and the export regenerates them every time.

## 2. Building

**Mod → Build Mod into Game** · toolbar **Build Mod** · `Ctrl+B`

Writes straight into `<linked game>\modules\<id>`. No packing step, no copying
by hand — the game reads modules from that folder as-is.

The build **bakes the open scene by itself**: every placed object goes into
`spawn\<level>.xspawn` for the scene's level, and a model that lives in the
project is staged into `gamedata\meshes\` — the one module subtree the game
mounts into its file system. Save the scene, press Build, done. (The scene is
the source of truth: objects you deleted from it disappear from the layer too,
and an object whose model exists nowhere is refused with a warning instead of
shipping a crash.)

**What counts as "yours":** only objects you placed — tool add, drag&drop from
the content browser, paste. Everything an imported base level brought (all the
stock props, NPCs and logic of, say, Escape) is the game's, not the mod's, and
never ships; baking it would double the whole level in game. The label is
visible per object and can be flipped: **Mod → Mark Selection as Mod Content /
as Base Level**. Objects placed in scenes saved before this flag existed count
as base — re-mark them once.

The build is a **mirror, not a merge**. Files the target holds and the project
no longer has are deleted, so a stale file from an old build can never haunt
you. If the target already holds files this project didn't put there (someone
else's module sharing your id), the build stops and asks before deleting
anything.

Other outputs, when you need them:

- **Mod → Build to Another Folder…** — same module layout, arbitrary target.
  Use it to build into a second install, or to a staging folder before zipping.
- **Mod → Export Flat Gamedata…** — a plain `gamedata_<id>` folder for people
  running a JSGME-style setup instead of XMS.

## 3. Testing it

The game picks the module up on the next start — **and added objects reach
existing saves too**: loading a save composes the module layers the same way
a new game does, and anything this playthrough has never seen is spawned in
once (the game remembers what it already applied, so a prop the player
destroyed stays destroyed, and nothing ever doubles). Mind the mode gate:
a module built for one campaign contributes nothing in another.

What still needs a NEW game: *changing or removing* things that already exist
in the playthrough — a save carries its own copy of every object, so edits to
base objects and deletions apply to fresh games only. Level overlays
(collision, visuals, AI map) rebuild on every level load and always reflect
the current module.

Console helpers:

```
xms_enable test
```

```
xms_disable test
```

```
xms_list
```

`xms_why <file>` answers "which module gave me this file"; the log line
`* XMS: spawn layers composed: +N added ...` confirms your ops applied on the
new game. Disabled modules are listed in `<game>\modules\disabled.ltx`;
enabling and disabling never touches the module's own files.

## 4. Shipping it to other people

Yes — zip the folder, that's the whole thing:

```
<game>\modules\<id>\        ->  <id>-1.0.0.zip
```

The recipient unpacks it into their own `<game>\modules\`, so they end up with
`<game>\modules\<id>\mod.ltx` and the rest beside it. Nothing is written into
the game's own files, on your machine or theirs, which is why uninstalling is
just deleting the folder.

Tell them the target mode you picked — a module gated to `survival` does
nothing in a `metro` playthrough, and that looks like a broken mod if nobody
said so.

## 5. Baking scene work into the mod

Scene edits don't ship by themselves; they get baked into module files first.
**Mod → Bake Scene Layer**:

| Layer | Output | What it carries |
|---|---|---|
| **Spawn Layer** | `spawn\<level>.xspawn` | Placed objects — both raw `.ogf` props and gameplay spawn elements, the latter with their config section and their logic (`custom_data`). |
| **Collision Overlay** | `levels\<level>\overlay.xcform` | Extra collision geometry. |
| **Visual Overlay** | `levels\<level>\visuals\*.ogf` + `overlay_visuals.ltx` | Extra visible geometry. |
| **Level Cut** | cut boxes in `overlay.xcform` | The selection used as volumes that *remove* base collision and visuals — replace a chunk of a stock level without editing a single base file. |

These write into the project. The next build ships them.

## 6. Quests (NQ)

A quest is a node graph in its own file. Make one in the Content Browser —
**RMB → Create → Quest Graph** — or over MCP with `xfined_quest_new`. Either way
you get a working stub (`trigger.start → flow.end`) and a document tab.

Edit it whichever way suits you. The tab is an infinite canvas: nodes flow top
to bottom, RMB pans, the wheel zooms through ten fixed levels, the strips above
and below a node are its `on_enter` / `on_exit` actions, and the inspector on
the right builds its widgets from the kind catalog the linked game ships. The
file itself is plain declarative Lua (`return { … }`), so writing or patching it
as text is a first-class path — `xfined_quest_get` hands back the canonical text
plus a readable outline of the graph, `xfined_quest_write` replaces it whole,
`xfined_quest_apply` does surgical edits. Node coordinates are optional; leave
`pos` out and **Layout** (or `xfined_quest_layout`) places everything.

Validation runs as you edit and shows up in the problems bar under the canvas —
click a problem to jump to the node. Errors (`E…`) block, warnings (`W…`) do
not. `xfined_quest_check_all` runs exactly the gate the build runs.

Then press **Build** (`Ctrl+B`). If any quest in the project has an error the
build **refuses** with `quest graph errors: N (see log)` and the log lists them
one per line — same codes the game prints. A clean build copies the `.nqasset`
files straight into `<game>\modules\<id>\`, keeping the folder they sit in
inside your project. Nothing is generated: no Lua, no XML, no string tables.

Test it in the game's console:

| Command | What you get |
|---|---|
| `nq list` | every quest of every module: uid, status, token count, problems |
| `nq state <uid>` | tokens, variables, refs, tasks, errors and the full outline |
| `nq jump <uid> <node>` / `nq fire <uid> <node>` | force the quest onto a node / finish a waiting one |
| `nq reload` | re-read the assets without restarting; state survives |
| `nq debug 1` | mirror every transition into PDA news |

Format reference, kind catalog and the meaning of every problem code:
[docs/nq/NQ_FORMAT.md](nq/NQ_FORMAT.md). The game-side contract lives in the
engine repo as `docs/dead-air/NQ_RUNTIME.md`.

## 7. When the build refuses

| Message | Fix |
|---|---|
| `no target game mode` | Mod → Edit Manifest, pick a mode. |
| `Module id is missing or invalid` | Mod → Edit Manifest, `id` must be `a-z 0-9 _ . -`. |
| `No linked game` | Link a game — the build target is that install. |
| `target already holds a module: N file(s) there are not in this project` | Something else owns that folder. Confirm only if you're sure it's yours. |
| `quest graph errors: N (see log)` | Open the log, fix the listed `E…` codes — see [docs/nq/NQ_FORMAT.md](nq/NQ_FORMAT.md). |

## Not the same thing

The **Compile** menu (Build, Make Game, Make Details, Make AI Map…) is the
X-Ray *level* compiler — it bakes geometry, lighting and AI maps for a level
you authored from scratch. It has nothing to do with shipping a mod. If you're
adding things to existing levels, you never need it.
