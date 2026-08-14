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
| **Target mode** | **Required.** Which game mode the module applies to: the ordinary game, an existing mode of the linked game, or a new mode this module adds. Without it the build refuses — a module that applies to everything is how mods break each other. |

If you declare a **new mode**, the export also generates the new-game-screen
checkbox for it (the xml patch, the text entry and `mode_register.script`).
Those files are machinery, not content — the content browser hides them so you
can't delete one by accident, and the export regenerates them every time.

## 2. Building

**Mod → Build Mod into Game** · toolbar **Build Mod** · `Ctrl+B`

Writes straight into `<linked game>\modules\<id>`. No packing step, no copying
by hand — the game reads modules from that folder as-is.

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

The game picks the module up on the next start. In the console:

```
xms_enable test
```

```
xms_disable test
```

Disabled modules are listed in `<game>\modules\disabled.ltx`; enabling and
disabling never touches the module's own files.

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

## 6. When the build refuses

| Message | Fix |
|---|---|
| `no target game mode` | Mod → Edit Manifest, pick a mode. |
| `Module id is missing or invalid` | Mod → Edit Manifest, `id` must be `a-z 0-9 _ . -`. |
| `No linked game` | Link a game — the build target is that install. |
| `target already holds a module: N file(s) there are not in this project` | Something else owns that folder. Confirm only if you're sure it's yours. |

## Not the same thing

The **Compile** menu (Build, Make Game, Make Details, Make AI Map…) is the
X-Ray *level* compiler — it bakes geometry, lighting and AI maps for a level
you authored from scratch. It has nothing to do with shipping a mod. If you're
adding things to existing levels, you never need it.
