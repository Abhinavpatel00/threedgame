# Why the blocks looked dim (and the fix)

## Symptom
Voxel blocks appeared noticeably darker than their source textures.

## Root cause
The render path was mixing **linear** and **display (sRGB/nonlinear)** color spaces incorrectly:

1. `triangle.slang` samples block textures (stored as `VK_FORMAT_R8G8B8A8_SRGB`) into linear shader color.
2. That color is rendered into HDR (`R16G16B16A16_SFLOAT`) correctly in linear space.
3. `postprocess.slang` tonemapped the color, but wrote it directly to `ldr_color` (`VK_FORMAT_R8G8B8A8_UNORM`) **without sRGB encoding**.
4. The engine then `vkCmdBlitImage` copies `ldr_color` into the sRGB swapchain image.

Because blit/copy paths do not perform sRGB conversion for you, linear values ended up being treated like display-space values. That makes midtones look too dark ("dim").

## Fix applied
In `shaders/postprocess.slang`, after tonemapping, output is now explicitly encoded to sRGB:

- `color = tonemapACES(color);`
- `color = toSrgb(saturate(color));`

This matches what the display expects and restores perceived brightness.

## Why this fix is minimal and safe
- No descriptor/pipeline layout changes.
- No buffer layout changes.
- Keeps existing bindless + push-constant flow.
- Only adjusts final transfer color encoding in postprocess.

## Notes
A cleaner long-term path is to do final presentation through a fullscreen graphics pass into the sRGB swapchain (letting attachment conversion handle encoding), instead of blitting from an UNORM intermediate.

## sRGB conversion vs gamma correction (important)

These are related but not identical terms:

- **Linear color**: physically meaningful space where lighting math should happen.
- **sRGB conversion**: applying the sRGB transfer function (piecewise curve) when storing/displaying.
- **Gamma correction**: often used as shorthand, but usually means an approximate power curve like `pow(x, 1/2.2)`.

So in practical renderer terms, “gamma correction” for display usually means **linear → display-encoded** output. The accurate version is **linear → sRGB** (not just a simple gamma power).

## Does `vkCmdBlitImage` do gamma/sRGB correction?

For your path, treat the answer as **no**.

`vkCmdBlitImage` is a transfer op. It handles copying/filtering/format conversion rules, but it is **not** a color-management stage that automatically applies display transfer-function correction for your final image pipeline. In particular, it does not replace the explicit linear→sRGB encode you need before presenting when your intermediate is `VK_FORMAT_R8G8B8A8_UNORM`.

That is why this fix is needed:

1. Scene is in linear HDR (`R16G16B16A16_SFLOAT`).
2. Tonemapped result was still linear-ish.
3. It was written to UNORM LDR, then blitted to swapchain.
4. Without explicit encode, perceived brightness is too dark.

After adding `toSrgb(saturate(color))` in postprocess, the values in LDR are now display-encoded, so presentation brightness looks correct.

## Rule of thumb for this renderer

- Keep all lighting and blending in **linear**.
- Convert to **sRGB once at output** (either in postprocess compute, or by rendering directly to an sRGB attachment in a graphics pass).
