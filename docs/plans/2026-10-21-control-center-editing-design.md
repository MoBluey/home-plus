# Control Center: add, remove, move and resize controls

Approved by the owner in conversation on 2026-10-21:
- the controls (1) and editing with the remote (2);
- the service owning the list;
- ambient light kept out of the way of the rest of the code, since only a few people run HyperHDR.

Sections 3 to 5 follow from those choices and are a proposal.

## Goal

People shape the Control Center the way iOS 18 allows: add controls from a gallery, remove them, move them and change their size, all with the remote. The new controls are more TV settings, plus ambient light for people who run HyperHDR and PicCap on the TV.

**Not doing**, as the owner chose:
- renaming controls;
- choosing which values a control steps through;
- binding controls to remote buttons;
- shortcuts to apps, inputs or web pages;
- webhooks and scripts.

Nothing here needs typing, so there's no setup page for phones.

## 1. The controls

The gallery groups them as below. ★ marks the new ones. Sizes: Small is one round unit, Wide is 2×1, and Large is 2×2.

| Group | Control | Kind | Sizes | OK |
|---|---|---|---|---|
| TV | Power Off | action | Small, Large | turns the TV off |
| TV | Screen Off | action | Small, Wide | picture off, any key brings it back |
| TV | Wi-Fi | opens | Wide | LG's network settings; shows the network |
| TV | Sleep Timer | stepper | Small, Wide | next time |
| TV | Inputs | list | Small | the inputs sheet |
| Picture & Sound | Picture Mode | list | Small, Wide | the picture-mode sheet |
| Picture & Sound | Eye Comfort | switch | Small, Wide | on / off |
| Picture & Sound | Sound Output | stepper | Small, Wide | next output |
| Picture & Sound | ★ Sound Mode | stepper | Small, Wide | next mode |
| Picture & Sound | ★ OLED Brightness | stepper | Small, Wide | next level |
| Picture & Sound | ★ Energy Saving | stepper | Small, Wide | next level |
| Picture & Sound | ★ Game Optimizer | switch | Small, Wide | on / off |
| Picture & Sound | ★ Dynamic Tone Mapping | stepper | Small, Wide | On, Off, HGIG |
| Ambient Light | ★ Ambient Light | switch | Small, Wide | HyperHDR's LEDs on / off |
| Ambient Light | ★ Screen Capture | switch | Small, Wide | PicCap's capture on / off |
| Home+ | Recents | opens | Small | Recents |
| Home+ | Accessibility | opens | Small | Settings → Accessibility |
| Home+ | Settings | opens | Small | Settings |

**What each size shows:**
- A Wide stepper shows its name and current value.
- A Small stepper shows only its icon; after OK, the new value appears under it for a moment.
- Switches light up when they're on, as iOS's do.

**Values:**
- Steppers go through the values the TV offers, and wrap around.
- OLED Brightness is a number from 0 to 100. It steps 20 → 40 → 60 → 80 → 100 → 20, and a value in between goes to the next level up.
- Energy Saving leaves out LG's "screen off" level, because Screen Off already does that.

**The default and what's offered:**
- The default layout is today's, from `9031ac7`. New controls are only in the gallery, so nothing changes for anyone who never edits.
- A control appears in the gallery only if it works on this TV:
  - Ambient Light only if HyperHDR is installed (`org.webosbrew.hyperhdr.loader.service`);
  - Screen Capture only if PicCap is installed (`org.webosbrew.piccap.service`);
  - each TV setting only if the TV describes it (`getSystemSettingDesc`).

**Settings is pinned:** it can be moved but not removed, because it's the only way into Settings.

## 2. Editing with the remote

- **Start:** hold OK on any control and choose **Edit Controls**.
- **While editing:**
  - the controls wobble gently, or get an outline with Reduce Motion on;
  - the focused control shows "OK to move · Hold for options";
  - a dashed **+** slot at the end of the grid adds a control.
- **Move:** OK picks the control up, and the others reflow around it as it moves.
  - Left and Right move it one place earlier or later in the order.
  - Up and Down jump to the row above or below.
  - OK or Back drops it.
- **Options:** hold OK on a control while editing.
  - **Size** offers only the sizes that control supports.
  - **Remove** isn't offered for Settings, and asks for no confirmation, since the control can be added back.
- **Add:** the + slot opens the gallery, grouped as in section 1, with only the controls that aren't placed yet. The chosen control goes at the end in its first size, and gets the focus.
- **Done:** Back leaves editing, and every change is saved as it's made.
  - A **Reset** button at the bottom restores the default layout, after asking.
  - The grid holds as many rows as fit under the clock. When it's full, the + slot says "No Room".

## 3. How it works

**The service owns the list and the catalogue**, as `controlCenter: [{ "id": "power", "size": "large" }, …]` in `config.json`. The order is the order on screen. With no list saved, the service uses the default.

**Catalogue, in `service/control.js`.** Each entry has:
- `id` and `title`;
- `kind`: `action`, `opens`, `list`, `stepper` or `switch`;
- `icon`, a name;
- `group` and `sizes`;
- `available()`, `read()`, and for steppers and switches, `run()`.

**The TV settings** share one helper: `setting(category, key)`, generalised from `pictureModes()`. It reads the allowed values with `getSystemSettingDesc` (`values.arrayExt`, visible and active), the current one with `getSystemSettings`, and sets the next with `setSystemSettings`. Each new setting is one catalogue line.

**Ambient light** lives in its own module, `service/ambient.js`, of about 60 lines. It adds its two entries to the catalogue, and nothing else in the service knows about it, so removing it means deleting the file and one line.
- **Ambient Light:**
  - its state is HyperHDR's JSON-RPC `serverinfo`, at `http://127.0.0.1:8090/json-rpc`: the `LEDDEVICE` component is enabled or not;
  - OK sends `componentstate` for `LEDDEVICE`;
  - if HyperHDR isn't running, it counts as off, and OK starts it through its loader service (`start`);
  - tests point it elsewhere with `HOMEPLUS_HYPERHDR`, like `HOMEPLUS_TMDB`.
- **Screen Capture:** PicCap's luna service `status`, `start` and `stop`, which PicCap's own app uses.

**API:**
- `GET /api/control` keeps today's fields and adds:
  - `controls`: the saved list, cleaned up as described in section 4;
  - `catalog`: every entry, with `available`, and the current `value` or `on` only for placed controls.
- `POST /api/control` gets three new actions:
  - `{ action: "run", id }` steps or toggles a control and returns its new state;
  - `{ action: "layout", controls: [...] }` checks the list and saves it;
  - `{ action: "reset" }` goes back to the default.
- Today's actions (`powerOff`, `sound`, `sleepTimer`, `pictureMode`, `eyeComfort`, `input`) stay as they are, for the built-in controls and for the luna `control` method.

**Launcher (C):**
- **Layout:** the fixed positions in `CC_TILE` give way to the list from the API, placed by a first-fit packer in reading order on the four-unit grid. The default list packs exactly into today's layout.
- **Icons:** looked up by name. An unknown name gets a plain glyph. The new icons are sound mode, brightness, energy, game, HDR, ambient light and capture.
- **OK:** the built-in controls keep their own handling (Power Off, Wi-Fi, Screen Off, the Picture and Inputs sheets, Recents, Accessibility, Settings). Everything else sends `run` and shows the reply. **There's no HyperHDR or PicCap code in C.**
- **Editing:** the edit mode, the options sheet, the gallery sheet and the Reset confirmation, as in section 2. Each change posts `layout`.

## 4. When things go wrong, and limits

- **A placed control that stops working** (HyperHDR removed, a setting gone after a TV update) shows dimmed, with "Not Available". OK does nothing, and it can be removed while editing.
- **A failed `run`** (a luna error, or HyperHDR not answering within 2 s): the control shakes, and its state is read again. The service logs why.
- **A saved list the service doesn't accept** is cleaned up:
  - unknown or repeated controls are dropped;
  - a size the control doesn't support becomes its first size;
  - Settings is added back if it's missing.
  - If nothing usable is left, it's the default. The launcher decides what fits on screen; the service only caps the list at 28 units as a sanity check.
- **Performance:**
  - controls are drawn as today, with no extra blur passes;
  - the wobble runs only while editing, and never with Reduce Motion;
  - state is read only for placed controls, and only while the Control Center is open. That's one HyperHDR request and one PicCap call per opening, and only if they're placed.

## 5. Testing

- **`test/control.test.js`:**
  - cleaning up lists: unknown, repeated, wrong size, Settings missing, too long;
  - availability from the TV's setting descriptions;
  - steppers wrap;
  - switches toggle;
  - each new control calls the right luna method.
- **`test/ambient.test.js`:** a mock HyperHDR JSON-RPC server (`serverinfo`, `componentstate`, and not running), a fake PicCap service (`status`, `start`, `stop`), and availability from injected service folders.
- **`test/api.test.js`:** `GET /api/control` has `controls` and `catalog`; `layout` round-trips; `reset`; `run`.
- **Previews (`run.sh`):** new scenes for editing, the gallery, the options sheet, and a customised layout with ambient switches. The harness gets a stand-in HyperHDR and PicCap. `check-screens.py` covers the new scenes automatically.
- **On the TV:**
  - GPU memory with a full grid (Mali `mem_allocs`);
  - the wobble's frame time;
  - the checks in "To verify".

## To verify on the C3 first

1. **The settings' keys and values,** with `getSystemSettingDesc`:
   - `sound`/`soundMode`;
   - OLED Pixel Brightness (`picture`/`backlight`?);
   - `picture`/`energySaving`;
   - Game Optimizer's category and key;
   - `picture`/`hdrDynamicToneMapping`.
2. **HyperHDR:** the JSON-RPC port (8090 by default), whether requests from 127.0.0.1 need a token, and `LEDDEVICE` in `serverinfo`'s components. It wasn't running during the brainstorm.
3. **PicCap:** the fields in `status`'s reply, and what `start` and `stop` reply. Its `config.json` has `autostart: true`, so capture comes back after a reboot; that's fine for an on/off switch.
