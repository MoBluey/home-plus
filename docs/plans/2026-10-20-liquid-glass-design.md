# Home+ Native: Liquid Glass redesign

Approved 2026-10-20. Goal: make the native launcher a faithful replica of Apple's tvOS 26
Liquid Glass home screen, covering Home, Control Center, menus, motion and sound, while keeping the TV
smooth.

## Decisions
- **Scope:** everything, including motion and sound.
- **Reference:** Apple's public material (WWDC25 "Meet Liquid Glass", the Liquid Glass technology
  overview, the HIG, the tvOS 26 newsroom article). Timings are measured from Apple's videos.
- **Priority:** a smooth TV comes first. Hard budget:
  - 60 fps with no missed vsyncs while navigating
  - GPU load no higher than 0.4.0
  - RSS below 0.4.0's 242 MB
  - 0% CPU while hidden
  The TV has a Mali-G52 MP3, about 400 MB free and almost no swap left, and shares it with PicCap and HyperHDR.
- **Approach:** our own GLES2 renderer (approach A). SDL keeps only the window, input, the GL
  context and vsync.
- **Not copied:** SF Pro (Apple licenses it for Apple platforms only; Inter stays), Apple's icons and
  Apple's sounds.

## Architecture
- `native/src/render.c/.h`, new: GL function table loaded via `SDL_GL_GetProcAddress`, so there's no
  link dependency and the desktop preview works with Mesa. It batches quads and has four programs:
  1. **tex:** textured quad with tint and alpha (images, text, icons).
  2. **shape:** rounded rect or capsule computed from a signed distance field (SDF), with fill,
     stroke and soft shadow, antialiased at 1 px.
  3. **glass:** the Liquid Glass material (below).
  4. **blur:** dual-Kawase down and up passes.
- **Render targets:** a backdrop at ½ resolution (960×540) holding everything behind the glass, plus blur
  levels at ¼, ⅛ and 1/16. About 3 MB in total, against about 30 MB of full-screen SDL targets in 0.4.0.
  The blur is cached while the backdrop is still.
- **Frame loop:** render only when something changes or animates, and never while hidden. The
  `cut-home-native-memory` session's hidden-window work is merged first.
- `glass.c` is replaced by the glass program. `gfx.c` keeps stb image and text work but returns GL
  textures. `main.c` keeps its layout and logic; only the drawing calls move to the render API.

## Material (glass program)
- **Lensing:** the backdrop is sampled with an offset along the shape's SDF normal. The offset is
  strongest at the rim and falls to zero about ¼ of the shorter side inwards. A small R/G/B split
  sits at the rim.
- **Regular variant:** heavy blur, a brightness lift and a tint that adapts to the backdrop's
  luminance, so text stays legible. Used for menus, panels, the clock and the top buttons.
- **Clear variant:** light blur, plus a dimming layer when the backdrop is bright. Used over
  artwork, such as the dock over the Top Shelf.
- **Highlights:** a thin rim highlight, brightest where the normal faces the light (top left by
  default), with a fainter one opposite. The light direction follows focus tilt and the Magic
  Remote pointer.
- **Depth:** a soft analytic shadow under the shape and slight darkening inside the edge.
- **Focus:** brightness lift plus a soft inner glow.
- **Shapes:** capsules and concentric rounded rects. Glass is never drawn on glass.

## Screens
- **Home:** full-bleed Top Shelf; 5 columns at tvOS proportions.
  - **App icons:** each sits in a glass frame with a rim highlight. On focus the artwork shifts
    slightly inside the frame for depth, since LG apps have flat icons.
- **Top right:** a Regular glass clock capsule and round Recents and Settings buttons.
- **Control Center:** a Regular glass panel with tiles for Recents, Settings, Trakt and Stremio. It
  replaces the separate pop-up sheets. Its position and the gesture that opens it follow tvOS 26,
  pending research.
- **Menus and app options:** centred Regular glass sheets with capsule buttons; the focused button
  lights up.

## Motion and sound
- **Springs** replace `approach()`. Focus lift is about 1.1×, with a response of about 0.35 s and a
  little bounce. Tilt follows the pointer. Scrolling uses springs too. All values are tuned against
  Apple's videos.
- **Morph:** sheets and Control Center grow out of the button that opened them and shrink back into
  it.
- **Sound:** a soft tick when focus moves and a click on select. Both are synthesised at start-up,
  played through SDL audio, quiet, and can be switched off in Settings.

## Testing
- **Desktop:** preview screenshots (Linux, Mesa GLES2) compared side by side with Apple's reference
  images.
- **TV:** screenshots through `com.webos.service.capture/executeOneShot`, a frame-time log with a
  missed-vsync count while navigating, VmRSS from `/proc`, and CPU while hidden.
- **Release:** native 0.5.0. Keep the 0.4.0 package for rollback.

## Coordination
- This work lives in the `liquid-glass` branch, in the worktree `.claude/worktrees/liquid-glass`.
- The `cut-home-native-memory` session edits `native/src` in the main checkout. Its changes are merged
  in before `main.c`'s drawing is moved over.
- The `fix-tools-install-sh` session only touches `tools/`.
- Three sessions share the TV. Each one announces before installing a build.

## Implementation steps
1. `render.c/.h`: context, GL loader, batcher, tex and shape programs, FBO helpers. Builds for the
   TV and the desktop.
2. Blur and glass programs, with an API for drawing the backdrop and glass panels.
3. `gfx.c` produces GL textures (images, text, icons).
4. Move `main.c`'s drawing onto the render API (after the memory-session merge). Remove `glass.c`
   and the mask and nine-slice textures.
5. A spring system and morph transitions.
6. The Control Center panel and the restyled sheets.
7. Synthesised sounds and a Settings toggle for them.
8. TV verification against the budget, screenshots, then release 0.5.0.
