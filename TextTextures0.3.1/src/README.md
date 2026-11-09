# Text Textures

A Radiant plugin for the BO3 Mod Tools (MTPlugins / RadiantPlugins host) that turns text into a material from inside Radiant.
Type in the pane, pick a font, colours and size, and click **Create texture**. You get a real BO3 image and material that you
can put on a brush face or drop in as a sign.

## Install

1. Copy `TextTextures.dll` and `TextTextures.ini` into `<Black Ops III>\bin\plugins\TextTextures\`.
2. Start Radiant through the Mod Tools as usual. With `autoload_new=1` in `RadiantPlugins.ini` the plugin loads on its own;
   otherwise tick it in the Plugins window.
3. The **Text Textures** pane opens as a tab beside Entity Info. The **Text Textures** menu toggles it.

## Use

1. Type the text. Enter starts a new line, and long lines wrap.
2. Choose the typeface, auto-fit or a fixed size, bold/italic/underline, alignment, padding and an optional outline.
3. Pick the text, outline and background colours. Tick **Transparent** for no background.
4. Pick the texture size (powers of two, 64-2048) and the material kind:
   - **Solid sign** (`lit`): an opaque panel.
   - **Decal on a wall** (`lit_decal`): only the letters show. Put it on a thin brush just in front of a wall.
   - **Cut-out letters** (`lit_alphatest`): only the letters show, with hard edges, for free-standing lettering.
5. **Glow** (optional): *Letters glow*, *Letters and outline glow*, or *Whole sign glows* (a backlit lightbox). The
   glowing parts light up in their own colour, even in the dark. **Brightness** is the material's `scaleRGB`. Treyarch's
   glowing signs use 4 to 32, mostly 8. This makes the material emissive: `lit_emissive` for a solid sign,
   `decal_emissive` for a decal, `lit_emissive_transparent` for cut-out letters, with **Use gameplay intensity** (`emissiveIncompetence`) on. It also writes a second image,
   `i_<name>_glow` (`<name>_glow.png`), that is black wherever nothing glows. When a 3D model is built with Glow on, the
   front of its letters glows too. The sides don't.
6. Name it (the material gets the name and the image becomes `i_<name>`) and click **Create texture**.
   The preview updates as you type and shows transparency as a checkerboard. It shows the texture, not the glow.

Create texture:

- writes `<BO3>\texture_assets\text_textures\<name>.png`
- adds or replaces `i_<name>` (image.gdf) and `<name>` (material.gdf) in `source_data\text_textures.gdt`, or in the GDT you choose
  (a `.bak` copy of the previous GDT is kept)
- runs `gdtdb\gdtdb.exe /update`, so Radiant and the linker can find the asset

Running Create again with the same name overwrites that texture. If the name already belongs to an asset in a different GDT,
the plugin refuses instead of making a duplicate.

### Putting it in the map

- **Apply to selection**: select brushes, look at the face you want, and click it. The material goes on that face,
  stretched once across it, upright and readable from the front (walls read with +Z up, floors and ceilings with your
  heading up). **Every face** puts it on all faces instead. One Ctrl+Z undoes it.
- **then apply to selection** (next to Create texture) does both in one click.
- **Place sign** adds a thin brush in front of the camera, facing you, with the material on the front and caulk elsewhere.
  Set **Sign width** in units; the height follows the texture's shape.

Patches, models and other point entities in the selection are skipped. Brushes inside func_groups and other brush entities
are handled.

### 3D text model

For lettering that sticks out, like a big sign on a building, the **3D text model** section turns the same text (font, bold,
italic, alignment and lines) into an extruded XModel.

- **Line height**: one line of text, in world units (capitals are about 70% of it). **Depth**: how far the letters stick out.
- **Front** uses the Text colour and **Sides** has its own colour. Each becomes a small solid-colour material
  (`<name>_front` / `<name>_side`). You can type an existing material name instead, e.g. a metal, and it is used as is.
- **Back** closes the rear of the letters. Untick it for letters flush against a wall. **Collision** sets
  BulletCollisionLOD High.
- **Origin**: *Bottom centre of the back* sits the letters on their baseline against a wall; *Centre* is the middle of
  the block.
- **Build 3D model** writes `model_export\text_models\<name>.xmodel_export`, compiles it with `bin\export2bin.exe`, adds
  the xmodel (and its colour materials) to the GDT, runs `gdtdb /update`, and, with *then place it* ticked, adds a
  `misc_model` 160 units ahead of the camera, facing you (Snap 90° keeps it on a world axis). Move or rotate it like any
  model, and Ctrl+Z removes it.

The model faces +X (that is its front) and the text runs along +Y, so a misc_model's yaw is simply the direction the letters
face. Holes in letters (O, A, B, 8...) are cut properly, curved sides are smooth-shaded and flat sides stay sharp. Fonts
without a real bold or italic face get one faked by Windows, which can make overlapping outlines. If that happens the
status line says so; pick a font that has the style built in.

## What the GDT entries are

The image and material entries reuse the full key sets of Treyarch's own stock entries (a `lit_decal` material and a diffuse
image from the stock GDTs, in `GdtTemplates.h`). Only the keys that differ between kinds are changed: type, category, usage,
colorMap, surface type and compression; with Glow also `colorMap00` (the glow image) and `scaleRGB`, as Treyarch's
`mtl_p7_sin_sign_neon_*` and `t7_decal_signage_*` emissive materials do. The rest is unchanged: compression (`compressed high color`, which keeps letter edges sharp). So every value is one
BO3's GDFs accept, and the entries open normally in APE if you want to tweak them there.

## Notes and limits

- Built against the MTPlugins SDK (`sdk/modtools_plugin.h`, API version 1) and tested here with a mock host under Wine.
  It has not yet run inside a real Radiant. If something misbehaves, `RadiantPlugins.log` has a `[TextTextures]` line for
  every step.
- After Create, the plugin asks the 3D camera to reload its assets. This only happens on the Steam Radiant build it knows
  (timestamp `0x5DF45023`, the same hook Brush Modeler uses). If a new or re-created texture still shows as missing or stale,
  restart Radiant once.
- If APE has the target GDT open with unsaved edits, saving in APE afterwards can overwrite the new entries. Let APE reload
  the file, or use a GDT that only this plugin writes to (the default).
- Floor and ceiling faces sometimes need a quarter turn to read upright. That uses Radiant's texture rotation, which follows
  the standard Q3 rule; walls never need it.
- Text is drawn with Windows GDI, so any installed TrueType/OpenType font works. Map compiles pick the material up
  automatically, because the map uses it.

## Settings

`TextTextures.ini` keeps the pane's last values, plus a few that only live there: the image folder, the texture-browser
`usage` category, sign distance/thickness/back material, and the camera reload switch. See the comments in the file.

## Building

See the README at the top of the repository: one solution (`MTPlugins-Stattic.sln`) or CMake builds both plugins, which
share the `common/` folder (files, PNG, GDT entries, XModel export, Radiant helpers).
