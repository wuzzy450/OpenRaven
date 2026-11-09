# Neural v9 on Linux: START HERE (written 2026-11-08, on Windows)

**Goal:** train a v9 network that keeps v8's gains on armor, faces and metal but leaves smooth areas
alone (space backdrops, nebula, planet, haze, sky, fades). Fix it with better training data, not by
turning the strength down (user decision 2026-11-08).

## Why v8 fails on smooth areas

Comparisons on Windows (`openraven/nn_compare/`, same frame with neural off and on, 1:1 crops at 4K):
- Armor: finer grit and scratches, about 50-58% more edge detail. Good.
- Space backdrop (nebula, Halo ring): "wormy" texture and blotches on gradients that should be smooth. Bad.

Likely cause: `neural/run_v5.sh` (its tiles were reused for v8) cuts **the most detailed 1024x1024 tile** of each
4K frame, plus one more random one. Smooth content is barely in the 260 v8 pairs, so the network never learned to leave it alone.

## What was brought over from Windows

All inputs are the network's exact input: the frame right before the neural pass, without the HUD, 3840x2160 PNG.
Captured at max quality: `RENDER_SCALE 2.0` (7680x4320 supersampled), FSR off, TAA on, HDR off.

| Folder | Frames | What |
|---|---|---|
| `neural/capture_space/input/` | 75 | Mission 1 space gun section (planet, ring, nebula, ships, explosions, turret deck). The user played it with F9 capture. |
| `neural/capture_v9/smooth/` | 20 | Attract: desert haze, sky, calm cutscene shots |
| `neural/capture_v9/detail/` | 24 | Attract: armor, corridors, gameplay |
| `*/v8_output/` | same | What v8 made of each frame (reference only, do not train on it) |

`neural/capture_v9/frames.txt` lists each frame's edge score and how much v8 changed it.

**Caveat:** these are cleaner than real play frames (real play = FSR 2 native at 100%, no supersampling).
Mix in some frames captured with play settings (or the existing v8 tiles) so v9 does not only learn supersampled input.

## Steps

1. **Copy** `capture_space/` and `capture_v9/` (inputs only, ~0.9 GB) to the Linux box. Tiles go under
   ComfyUI input, e.g. `$CI/halo9t/` (`CI=/run/media/sean/RAID0-HDD-SERVER/MiniMaxH3/ComfyUI/input`).

2. **Cut tiles, this time including smooth ones.** Same 1024x1024 game-pixel tiles as `run_v5.sh`, but per frame:
   the most detailed tile, the **least** detailed non-black tile, and one random one. For the space frames, take
   2-3 smooth tiles (nebula, planet surface, star field). Skip tiles that are almost all black (mean < 8).

3. **Check the teacher first.** Run SeedVR2 on 5-10 smooth space tiles:
   `SV_DOWN=1.0 SV_TAG=_d1.0 RM_INPUT=$CI $PY -I neural/remaster_seedvr.py halo9t/<tile>.png ...`
   (same settings as v8: 7B Sharp, `SV_COLOR=lab`). Compare with the input:
   - If SeedVR2 keeps the gradients clean: good, go to step 4.
   - If SeedVR2 itself adds texture or blotches: v8 copied the teacher. Fix the targets, not the data amount: try
     `SV_DOWN` 0.75 or the non-Sharp 7B model for smooth tiles, or blend the target toward the input where the input
     is flat (low local contrast) before training.

4. **Run SeedVR2 on all new tiles** and build `neural/pairs_v9.txt` = the 260 lines of `pairs_v8.txt` + the new
   pairs (same format: `input.png target.png` per line). Aim for at least 30% smooth tiles in total.
   Unload ComfyUI's model before training (the `curl .../free` line in `run_v8.sh`).

5. **Train v9 from the v8 settings** (`run_v8.sh`), output `neural/raven_nn9`:
   `NN_A=24 NN_EXTRA=2 NN_B=32 NN_SCALES=1.5,1.7,2,2,3 NN_VGG=0.05 NN_ADV=0.1 NN_PROT=0 $PY -I neural/train5.py neural/raven_nn9 40000 neural/pairs_v9.txt`
   Keep the same network size, so the Windows Vulkan path and its speed (8.7 ms at 4K on the 3080) stay the same.
   Start from v8's weights if `train5.py` supports it (fine-tune: faster, keeps the armor gains); otherwise train from scratch.

6. **Quick check on Linux:** `cp neural/raven_nn9_weights.h neural/raven_nn_weights.h`, `./build.sh`, then
   `RAVENNN_DUMP=1 ./gfxshot.sh ...` as in `run_v8.sh`, on an attract frame with sky or haze.

7. **Bring back to Windows:** only `neural/raven_nn9_weights.h` (+ `.pt`). Repo rule: ship one network,
   `neural/raven_nn_weights.h` = the chosen one (update the sha256 in `NEXT_STEPS_WINDOWS.md`).

## Back on Windows: how to judge v9

- Rebuild (`openraven/win`, `make`), then compare v8 vs v9 on the **same captured inputs**: the `v8_output/` folders
  are the v8 side. Run v9 on the same inputs and make the same 1:1 crops as `openraven/nn_compare/`. The user wants
  side-by-side screenshots to decide whether it helps.
- In game: `capture_space.bat` (game folder) launches max quality + neural + F9 capture; `RAVENNN_PAIR_EVERY=N`
  saves before/after pairs every N frames without the key.
- v9 is better if the space backdrop crops show no added texture, and the armor crops keep the v8 detail.

## Also still open from the Windows side (not for the Linux session)

- fp16 vs fp32 accumulate quality check (PSNR) of the Vulkan kernels.
- Neural cost: ~49 -> ~35 FPS in an attract scene at 4K with the user's max settings.
- The `capture_space.bat` PNG conversion did not run after the game quit (frames were converted by hand). Check why.

## Done on Linux (2026-11-08)

- Teacher check: SeedVR2 itself adds texture on flat space tiles (HF 0.2 -> 0.6), so v8 copied the teacher.
  Fix: `neural/flat_blend.py` pulls every target toward the game tile where the game tile is flat (weight 0.1 flat .. 1 detailed).
- 390 new tiles (`neural/cut_v9.py`, $CI/halo9t: 199 smooth, 124 detail, 67 random) + v8's 260 = 650 pairs (`pairs_v9.txt`, blended targets).
- v9 = fine-tune of v8 (`NN_INIT`, new in train5.py), 20k steps, LR 2e-4, same size. Script/log: `neural/run_v9.sh`, `run_v9.log`.
- Tile check (v8 vs v9, texture score, game in brackets): space smooth 0.38 -> 0.28 (0.24), attract smooth 0.64 -> 0.32 (0.28),
  armor/detail 10.61 -> 10.71 (8.32). Smooth areas almost untouched, detail gains kept.
- Installed: `raven_nn_weights.h` = `raven_nn9_weights.h` (sha256 3288eed6...), built, gfxshot `nn9_test` OK, ~43 fps 4K.
- ComfyUI on Linux: `~/ai/ComfyUI/venv_p40` (uses p40-venv's torch via .pth), port 8190, input/output on MiniMaxH3.
- Next (Windows): nothing to copy, the RAID already has v9 in `neural/raven_nn_weights.h`. Rebuild (`openraven/win`, `make`), then follow "Back on Windows: how to judge v9" above. v8 is kept as `raven_nn8_weights.h` to switch back.
