# Apple look: clock capsule, tvOS 26 grid, clear dock glass

Approved by the owner in conversation on 2026-10-20. It follows on from
`2026-10-20-liquid-glass-design.md`.

## Reference

The numbers come from Apple's tvOS 26 press image (Apple Newsroom, WWDC25), scaled to 1920×1080,
and from real screenshots of Home+ 0.9.0 on the C3.

- **Clock:** one Liquid Glass capsule at the top right, holding the time and, inside its right end,
  the profile picture. It is about 182×57, 52 px from the top, with its right edge in line with the
  dock's. The glass is tinted by the picture behind it. (Coverage of tvOS 17 says the corner can't
  be selected; the owner reports that it reacts when clicked.)
- **Grid:** 6 columns of 248×150 tiles, about 50 px apart.
  - The dock spans x 48–1872 and y 785–1015: 44 px of padding at the sides, 40 at the top and bottom.
  - The focused tile grows about 1.23× and gets a light rim.
- **Chevron:** a shallow white caret (about 50×10, a 4 px stroke, about 70% white), centred 30 px
  above the dock. It shows that Up leads into the Top Shelf.
- **Dock glass:**
  - Clear: the picture is sharp through it, just a little darker.
  - The rim is faint and the same all round, brighter only where a corner faces the light.
  - The picture bends near the rounded ends; 9to5Mac: "the dock on the home screen sparkles at the
    edges".
  - There is no white band along the top. On the C3, Home+ draws one: a crisp white line plus a
    haze below it.
- **No buttons beside the clock:** the App Switcher is a double press of the TV button, and Control
  Center a long press.
- **On the C3:** Settings rows over white tiles ("TV Settings", "Done" over YouTube and Netflix) wash
  out.

## Design

1. **Top right:** one capsule with the time and a gear in the circle where Apple shows the profile
   picture.
   - The whole capsule is one button. Up from the top row, or from Continue Watching, selects it.
     It lifts and turns white like other focused buttons, with "Settings" under it.
   - OK, or a pointer click, opens Settings, which grows out of the capsule.
   - The Recents button is removed. Recents stays on Back from the first app, and on Red. The first
     tile's hint becomes "Press OK to open · Hold for options · Back for Recents".
2. **Grid:**
   - 6 columns of 248×150 tiles, in a dock at x 48–1872 and y 785–1015.
   - Focus grows about 1.2×.
   - Logos get bigger inside the tiles: marks fill about 60% of the height, and wordmarks about 70%
     of the width. LG only ships 130 px icons, so logos never grow past about 1.4×.
3. **Chevron:** centred above the dock while Up from the focused app goes into Continue Watching.
4. **Glass shader:**
   - The band along the top is gone.
   - The rim is faint and even, with sharp highlights only where it faces the light and a small
     reflection on the opposite corner.
   - The dock is clear rather than frosted, with Apple's dimming over bright content.
   - Edges bend the picture more, and split its colours a little in the bevel. The split stays
     faint, since strong rainbow edges look cheap.
5. **Menus:** a dark backing behind each row, so text stays readable over white tiles. This is
   checked with Reduce Transparency too.

## Later changes (owner's review on the TV)

- **No "Settings" label:** the pill under the focused clock is gone. The white platter is enough.
- **Corner glint:** the bright glint where the dock's corner faces the light is a little dimmer
  (0.55 → 0.40).
- **Top Shelf carousel layout:**
  - The heading pill ("Continue Watching · Stremio", "Popular on Netflix") mirrors the clock.
    It has the same height and top line, and sits on the dock's left edge as the clock sits on
    its right.
  - The title (its logo when there is one), details, progress, hint and dots sit low on the left,
    just above the chevron, like the Apple TV app, so the picture's top stays clear.
- **Scrolling down:** the dock's glass grows to the screen's edges and frosts over as the grid
  scrolls. It becomes the blurred background the apps sit on, crossfading into it at the end. The
  owner compared this with a version where the dock simply leaves, and chose this one.
- **Blur fix:** the backdrop used to hold its own blur, which smeared wider every frame, and a
  blurred white stand-in for the dock. Together they made a white band that jumped away. The
  backdrop now keeps the picture at full strength, without either.

## Unchanged

OLED protection (dimming, black screen, pixel orbit), the pointer/keys handover, Reduce Motion and
Reduce Transparency, and the idle cost: nothing is drawn while nothing moves. The glass stays one
pass at half resolution, so the GPU budget shared with PicCap and HyperHDR is unchanged.

## Checks

- Desktop previews while iterating.
- Before/after screenshots on the C3, taken from the TV itself. The app that was in front is
  restored afterwards.
- The preview is not evidence for the TV. Glass on the C3's Mali GPU is judged from the TV's own
  screenshots.

## Steps

1. Grid: 6 columns, the new dock, focus scale and row spacing.
2. The clock capsule with the gear: remove the Recents button, then update navigation, pointer
   hit-testing and hints.
3. The chevron.
4. The glass shader: rim and band, clear dock, lensing and colour split.
5. Dark backing for menu rows.
6. README screenshots (`run.sh`), version 0.10.0, then install on the TV with real screenshots.
