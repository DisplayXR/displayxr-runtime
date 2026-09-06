# Rear depth budget, in plain language

*How a transparent 3D app on a DisplayXR display decides whether it may draw
behind the screen. Companion to [ADR-040](../adr/ADR-040-rear-depth-budget.md)
and the [`XR_DXR_depth_budget` spec](../specs/extensions/XR_DXR_depth_budget.md).
Written for people who don't need the code.*

## The problem

A transparent app (the model viewer, the gaussian-splat viewer, the avatar) floats
its content over the normal Windows desktop. On a 3D display, that content has
real depth: parts pop out in front of the screen, parts sit behind it. The desktop
behind the app, however, is flat and lives exactly *at* the screen.

Depth behind the screen and a flat desktop don't mix. If the back of a model is
drawn over a desktop icon, your eyes get two contradictory messages: the model
*covers* the icon, so it must be in front, but its stereo depth says it is
*behind* the screen, and therefore behind the icon. That contradiction is
unpleasant, so the apps used to solve it bluntly: clip everything behind the
screen plane. You only ever saw the front half of the model.

## The insight

The contradiction needs two things: rear content **and** a background with
visible depth cues right there. A 3D display makes depth out of small
horizontal differences between what each eye sees. A plain wall of colour, or a
vertical gradient, or horizontal stripes, all look identical shifted sideways: they
carry no horizontal cue at all, so your eyes can't tell where they are in depth.
Over such a background the back of a model looks perfectly fine.

So the question is not "is there a desktop behind me" but "does the desktop right
behind my content have horizontal structure" (text, icons, window edges, photos).
When it doesn't, the clip can open.

## Who does what

Three parties, one job each, nobody stepping on anybody:

- **The display's plug-in owns the pixels.** It already captures the desktop behind
  the window for transparency (the first integration is Leia SR). It now hands the
  runtime a small, blurry thumbnail of that region, at most 15 times a second and
  only when the desktop actually changed. It has no opinion about it.
- **The runtime owns the decision.** It measures the thumbnail: how many pixels
  have a sharp horizontal step, and whether any column of pixels forms a vertical
  line (a window edge). Vertical structure is ignored on purpose. From that it
  publishes a single number to the app every frame: the *rear depth budget*, in
  screen heights. Zero means "clip at the screen, as before"; a thousand means
  "draw everything".
- **The app owns its geometry.** It reads the number and moves its far clipping
  plane accordingly. It also tells the runtime where on screen its content is, so
  only the desktop *behind the content* is measured, not the whole window.

The runtime never touches vendor code, the plug-in never analyses anything, and
the app never captures the desktop. Any 3D display vendor can join by supplying
the thumbnail.

## What happens, step by step

1. The app draws a frame and tells the runtime the screen rectangle its content
   occupies (the "content bounds").
2. Every 66 ms the runtime asks the plug-in for the latest thumbnail of the desktop
   behind the window. If nothing on the desktop changed, there is no new thumbnail
   and no work.
3. The runtime looks only at the part of the thumbnail under the content bounds,
   padded slightly outward, because the conflict shows up along the model's
   outline.
4. It scores the horizontal structure there. Below the threshold means "neutral".
5. Neutral must hold for about 0.4 s before the budget opens; that avoids
   flickering while a window is being dragged behind. When structure appears the
   budget closes within 0.1 s, because a conflict is worse than a missing rear.
6. The budget doesn't jump. It slides over about 0.3 s when opening and 0.15 s
   when closing, so the back of the model grows in and shrinks away smoothly.
   The app applies the value as is; no app has to animate anything.
7. Whenever the app isn't in transparent mode, or runs inside the workspace
   controller, or no thumbnail source exists (for example the simulated display,
   or a display vendor without the plug-in slot), the budget is fixed at the old
   behaviour. Nothing changes for anyone who didn't opt in.

## Why it costs almost nothing

- No extra desktop capture: the plug-in already had one for transparency.
- The thumbnail is tiny (about 320×180) and produced only on desktop change.
- The scoring is a few tens of thousands of pixel comparisons, roughly a tenth of
  a millisecond, at most 15 times a second, and never in the rendering path that
  drives the display.
- Apps that don't enable the extension pay nothing at all.

## Knobs, for the curious

- `DXR_REAR_BUDGET=open` or `=clip` forces the outcome for side-by-side tests.
- `DXR_REAR_BUDGET_DUMP=1` saves the thumbnail the runtime judged, on every state
  change, so you can see what it saw.
- `DXR_REAR_BUDGET_ROI=0` ignores the content bounds and measures the whole window.
- The dwell, close and slide times can be tuned with `DXR_REAR_BUDGET_*_MS`.

## What it doesn't do yet

- The threshold that separates "neutral" from "busy" is a fixed number, tuned by
  eye on one display. A graded budget (open *a little* over a slightly textured
  background) is a natural next step.
- Vendors without a background capture get no thumbnail and keep the old
  behaviour; a runtime-owned capture fallback is planned.
- Under the workspace controller the budget is fully open today; the controller
  could drive it in future, since it knows what sits behind each app.
