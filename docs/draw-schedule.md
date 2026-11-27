# Ticks that do not draw

```sh
WOWEE_DRAW_EVERY=4 WOWEE_FRAME_PROFILE=1 ./build/bin/wowee
```

One iteration of `Application::run` is a tick: input, update, interface,
picture. `core::DrawSchedule` (`include/core/draw_schedule.hpp`) says for each
tick whether it draws the picture. A normal client draws every tick. With
`WOWEE_DRAW_EVERY=N` one tick in N is drawn while in the world. A host that
steps the client (an agent that reads one picture every few ticks) decides
per tick:

```cpp
// any time before the tick's update, for example right after the event pump
core::Application::getInstance().drawSchedule().request(drawThisTick);
```

A request holds for one tick and wins over the count. Outside the world
(login, realm list, character list) every tick is drawn unless a host says
otherwise; the first tick in the world is drawn and the count starts there.

## What a tick does, drawn or not

| part of the tick | drawn tick | undrawn tick |
|---|---|---|
| events, key bindings, `Input` | yes | yes |
| `Application::update`: network, game state, movement, addons' OnUpdate, `Renderer::update` (camera, collision, animation clocks, particles, audio) | yes | yes, except the two rows below |
| unit shadow blobs (`collectUnitShadows`) | yes | no: the world pass is their only reader and they are gathered afresh each time |
| sun occlusion sample (`Renderer::sampleSunOcclusion`) | yes | no: eased over the summed time on the next drawn tick, see "Differences" |
| `Renderer::beginFrame`: swapchain image, command buffer, per-frame data, pre-passes | yes | `Renderer::passFrameUndrawn` |
| `Renderer::renderWorld` | yes | `Renderer::passWorldUndrawn` |
| the interface: portraits and model frames, widget layout, key routes, OnShow/OnHide, mouse dispatch, `UIManager::render` (target input, nameplates, minimap), `WidgetRenderer::draw`, rect dumps, `ImGui::Render`, the map window's frame | yes | yes, the same calls in the same order |
| `Renderer::endFrame`: post-process, ImGui draw data into the command buffer, submit, present | yes | no |

The interface runs whole on an undrawn tick because none of it is only a
picture. `WidgetRenderer::draw` sizes and places tooltips, records the
rectangles of chat links for the next click, measures edit boxes for their
scroll range and hands frames back to this client's own panels;
`UIManager::render` resolves clicks on the world and on nameplates; ImGui's
hover state for the next tick comes from the windows submitted in this one.
Its draw lists are built and dropped.

`passFrameUndrawn` and `passWorldUndrawn` do what the two calls they stand in
for do besides drawing, at the same point of the tick:

- every submitted frame is waited for, then the destruction queued against
  the frame slots runs and finished upload batches are retired
  (`VkContext::settleSubmittedFrames`). `beginFrame` does this for the one
  slot it reuses; code that retires GPU objects relies on "two drawn frames
  later nothing reads this", and ImGui retires textures by counting its own
  frames, which are ticks. With every frame finished before an undrawn tick
  goes on, both stay true;
- frames copied for a recording are collected (the picture of drawn tick T
  is available during tick T+1 when T+1 is undrawn, instead of at the second
  drawn frame after T);
- the player's motion for grass bending and water ripples is tracked
  (`Renderer::trackPlayerMotion`): a blend per tick;
- the grass population advances one slice (`updateGrassPopulation`);
- sealed interiors and the rooms each WMO shows are decided
  (`Renderer::updateWorldVisibility`). `WMORenderer::roomHides` reads them,
  and through it `Application` hides units the player cannot see from mouse
  targeting, nameplates and unit shadows: game-visible state that the world
  pass happened to compute;
- the doodad draw distance moves one step towards its target
  (`M2Renderer::passUndrawn`): a fixed fraction per tick.

The frame budget report (`WOWEE_FRAME_PROFILE=1`, `docs/perf_baseline.md`)
shows them as `passFrameUndrawn` and `passWorldUndrawn`, and counts
`ticks drawn`. Stage averages are per tick, so `renderWorld` reads a
quarter of its cost when one tick in four is drawn.

## Differences between a drawn frame and the same frame of an all-drawn run

Game state, interface state and input handling do not depend on the
schedule. A drawn frame after undrawn ticks differs from the frame an
all-drawn run would show in these places, all in the picture:

- **Sun occlusion** (sun glare and flare). The sample is taken on drawn
  ticks and eased over the time since the last one
  (`rendering/sun_occlusion_ease.hpp`). The ease is an exponential, so the
  value is the same unless the sample would have changed on an undrawn tick:
  while the line from the camera to the sun crosses the edge of a hill or a
  building. Then the eased value differs by at most
  exp(-t/0.25) - exp(-N·t/0.25) of its range on the first drawn frame (0.37
  for one tick in four of 50 ms) and the gap closes with the 0.25 s time
  constant.
- **What the draw path eases per picture.** Volumetric fog's layer height and
  density, the Nether World tint and FSR2's frame time step by the time since
  the last drawn frame (`Renderer::sinceDrawSeconds_`); the first two chase
  moving targets once per picture instead of once per tick.
- **History the GPU keeps per picture**: FSR2, volumetric fog, ray traced
  lighting, water refraction and sun shafts accumulate over drawn frames, so
  they see fewer, wider-spaced samples.
- **The clear colour behind a sealed interior** is the fog colour of the last
  drawn frame rather than of the tick before.
- **Things built in the draw path arrive on the next drawn frame**: bone
  buffers, particle and ribbon buffers, minimap tiles, ImGui's font atlas
  updates, a portrait or model frame's first picture (the interface shows the
  fallback portrait until then), a rebuilt swapchain after a resize, an
  anti-aliasing or fog quality change.
- Random numbers that only pictures consume (particle emission, weather) are
  drawn every tick as before; nothing reseeds.

A screenshot (`GameScreen` key, `WOWEE_SCREENSHOT`) taken on an undrawn tick
is the last drawn frame. `WOWEE_SCREENSHOT` and `WOWEE_RECORD` count drawn
frames. `GetFramerate()` reports ticks per second.

## Checked

`_experiments/20261127T160209Z.draw_skip_ticks/` in the workspace: a scripted
sequence of real input (keys, typed chat, clicks on a unit, on action
buttons and on the loot window, wheel, a right-button drag) at 640x360, run
with every tick drawn and with one in four.

- Game outcome and interface state: the same events and the same state at 13
  recorded points in 5 runs of each kind, apart from what the server rolls;
  among runs with equal rolls the one-in-four run differs from three
  all-drawn runs in 0 of about 21,000 widgets at each point.
- Movement: in 1 of 9 one-in-four runs the character ended 1.24 yd from where
  every other run ended, on top of an object the others stop against. 24
  repeats of the same key presses in that mode ended within 0.1 yd of each
  other, as 24 all-drawn repeats did. The cause is not established.
- Pictures: differ between the kinds by no more than two runs of one kind
  differ. The ticks of two runs do not line up without a stepping host, so
  frames were not compared tick for tick.
- Cost of a tick, uncapped: 7.7 ms with every tick drawn and 5.9 ms with one
  in four in Northshire, 6.6 and 5.2 ms in Shattrath; drawing was 1.75 and
  2.5 ms of the tick, the interface that runs on every tick 1.0 ms.

Unit tests: `tests/test_draw_schedule.cpp`.

## Not skipped, though only the picture reads it

Measured at 640x360 and left running every tick because leaving it out would
change later drawn frames or gain nothing:

- doodad bone matrices (`M2Renderer::update`): 0.004 ms a tick in Northshire
  (5 instances in view), 0.06 ms in Shattrath (40);
- particles, ribbons, weather, water wakes, footprints: their state depends
  on the step pattern (emission per step, random numbers);
- `WidgetRenderer::draw`'s draw lists (0.2 ms): built together with the
  interface state listed above.
