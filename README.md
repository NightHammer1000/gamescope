# Telescope

*A gamescope fork focused on a gamemode that works across all vendors — not afraid of driver workarounds.*

Telescope came out of a bug: high resolution and HDR corrupt on NVIDIA GPUs, because gamescope's scanout path assumes Vulkan-allocated memory is something the display engine can scan out. Nothing in the Vulkan spec ever promised that. On Mesa it happens to hold. On NVIDIA it does not.

The fix is to allocate through GBM instead — or to wait for NVIDIA. They have been sitting on this since 2024, so GBM it is.

But it seems this whole topic is a bit of a philosophical minefield, left over from a vendor war that is not long past. Long story short: upstream gamescope does not want to use GBM.

So here is Telescope. A DRM-backend-focused gamescope fork whose only goal is to give you a working gamemode, no matter what hardware, no matter what philosophical stance is out there.

It coexists with gamescope fine — nothing it installs shares a path with upstream. Binaries are `telescope`, `telescopectl`, `telescopereaper`, `telescopestream`, `telescope-type`; data lives in `/usr/share/telescope`, config in `/etc/telescope` and `~/.config/telescope`, and the WSI layer is `VK_LAYER_FROG_telescope_wsi` with its own `ENABLE_TELESCOPE_WSI` gate so it never hooks gamescope's clients. Use gamescope for desktop or VR, and Telescope for gamemode.

I only care about one thing. Does it work? Is it clean code? Then it is in. And once it is fixed in the right place — the driver — it comes back out again.

So shoot your workarounds my way. Open a PR. Let us make the Linux gaming ecosystem less of a philosophical battlefield and something that actually works.

## Status

**Early. Not yet run on real hardware.**

The fork builds clean and its unit tests pass, but nothing here has been exercised on a real display yet. Do not put this on a machine you need working. [`HARDWARE-CHECKLIST.md`](HARDWARE-CHECKLIST.md) tracks exactly what has and has not been verified.

## Currently included workarounds

* **NVIDIA: scanout buffers are allocated through GBM, and composition is forced.**

  The display engine will not scan out client-allocated buffers either, so every frame has to be composited into a buffer we allocated.

  This is NVIDIA-only. AMD and Intel keep Vulkan-allocated scanout and, more importantly, keep direct scanout of client buffers — the copy this compositor exists to avoid. Where GBM is required there is deliberately no fallback: falling back to Vulkan allocation on such a driver does not fail, it renders corruption, and refusing to start beats that.

  Measured on real hardware: framecount is identical patched and unpatched, so the forced composition costs nothing in steady state.

* **NVIDIA: modesets take the link fully down, settle, then bring it back up** as a separate commit, instead of zeroing and refilling `CRTC_ID` / `ACTIVE` / `MODE_ID` in one atomic request.

  We have no guarantee how a driver sequences a disable-and-refill in a single request, and on NVIDIA the link appears never to actually drop — so it never retrains, and that is the corruption. This is also useful well beyond NVIDIA; see below.

## Making badly-behaved displays work

Plenty of sinks negotiate HDMI badly. AV receivers are the worst offenders, cheap TVs are close behind, and none of it is the GPU's fault — the display simply will not retrain its link properly unless you make it.

Telescope can force a real link drop on any driver, not just where a driver quirk applies:

| symptom | try |
|---|---|
| VRR never engages, or flickers, through an AV receiver | `drm_modeset_link_down=1` |
| Corruption or no signal after changing resolution or toggling HDR | `drm_modeset_link_down=1` |
| It helps, but not every time | raise `drm_modeset_link_down_settle_ms` |
| It works and you want the black screen shorter | lower `drm_modeset_link_down_settle_ms` |

Set them persistently in a Lua file — any `.lua` under `~/.config/telescope/scripts`:

```lua
-- ~/.config/telescope/scripts/my_receiver.lua
gamescope.convars.drm_modeset_link_down.value = 1
gamescope.convars.drm_modeset_link_down_settle_ms.value = 1000
```

Or against a running session, to try a value without restarting:

```sh
telescopectl drm_modeset_link_down 1
```

`drm_modeset_link_down` is `-1` by default, meaning "on where the driver is known to need it". `0` disables it everywhere, `1` forces it everywhere. The settle defaults to 1000ms; that number came from the experiment that found the workaround, not from measuring how long a link actually needs, so it is worth tuning down.

> The Lua namespace really is `gamescope.*`, not `telescope.*`. It is kept as-is so existing gamescope display-quirk scripts work unchanged.

## Currently included additional features

*Nothing yet.*

## Planned additional features

* Additional upscalers (SGSR, BCAS, xBR, Anime4K)
* Framegen?

## What was removed

Telescope targets the DRM/KMS session case only. Removed from upstream:

| removed | use instead |
|---|---|
| Wayland and SDL backends (nested) | upstream gamescope |
| OpenVR backend, SteamVR overlay forwarding | upstream gamescope |
| ReShade effect support | — |

`drm` and `headless` are the only backends left. `headless` is for CI and testing, not for use.

---

## How it works

In an embedded session usecase, gamescope does the same thing as steamcompmgr, but with less extra copies and latency:

 - It's getting game frames through Wayland by way of Xwayland, so there's no copy within X itself before it gets the frame.
 - It can use DRM/KMS to directly flip game frames to the screen, even when stretching or when notifications are up, removing another copy.
 - When it does need to composite with the GPU, it does so with async Vulkan compute, meaning you get to see your frame quick even if the game already has the GPU busy with the next frame.
 - Because the game is running in its own personal Xwayland sandbox desktop, it can't interfere with your desktop and your desktop can't interfere with it.
 - You can spoof a virtual screen with a desired resolution and refresh rate as the only thing the game sees, and control/resize the output as needed. This can be useful in exotic display configurations like ultrawide or multi-monitor setups that involve rotation.

AMD requires Mesa 20.3+, Intel requires Mesa 21.2+. For NVIDIA's proprietary driver, version 515.43.04+ is required (make sure the `nvidia-drm.modeset=1` kernel parameter is set).

If running RadeonSI clients with older cards (GFX8 and below), currently have to set `R600_DEBUG=nodcc`, or corruption will be observed until the stack picks up DRM modifiers support.

## Building

**Debian/Ubuntu:**

```sh
apt install meson ninja-build pkg-config cmake libpipewire-0.3-dev hwdata libx11-dev \
  libwayland-dev libvulkan-dev wayland-protocols libx11-xcb-dev libxdamage-dev \
  libxcomposite-dev libxcursor-dev libxxf86vm-dev libxtst-dev libxres-dev libxmu-dev \
  libxkbcommon-dev libcap-dev libavif-dev libpixman-1-dev liblcms2-dev libseat-dev \
  libinput-dev xwayland libxcb-composite0-dev libxcb-ewmh-dev libxcb-icccm4-dev \
  libxcb-res0-dev libdrm-dev libgbm-dev libudev-dev libdecor-0-dev \
  glslang-tools libluajit-5.1-dev catch2
```

> **Your distro packages are probably too old.** Telescope needs wlroots 0.20, which needs
> libwayland ≥ 1.24, wayland-protocols ≥ 1.47, libdrm ≥ 2.4.129, pixman ≥ 0.46 and
> libxkbcommon ≥ 1.8. Ubuntu 24.04 ships none of those. Build them into `/usr/local` and
> export `PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig` before configuring.

```sh
git submodule update --init
meson setup build/
ninja -C build/
build/src/telescope -- <game>
```

Install:

```sh
meson install -C build/ --skip-subprojects
```

Run the tests:

```sh
meson test -C build/ --suite gamescope
```

## Options

See `--help` for the full list.

* `-W`, `-H`: set the output resolution. Defaults to 1280×720.
* `-w`, `-h`: set the resolution used by the game. If `-h` is specified but `-w` isn't, a 16:9 aspect ratio is assumed. Defaults to the values specified in `-W` and `-H`.
* `-r`: frame-rate limit for the game, in FPS. Defaults to unlimited.
* `-o`: frame-rate limit for the game when unfocused. Defaults to unlimited.
* `-F fsr`: use AMD FidelityFX™ Super Resolution 1.0 for upscaling
* `-F nis`: use NVIDIA Image Scaling v1.0.3 for upscaling
* `-S integer`: use integer scaling.
* `-S stretch`: use stretch scaling, the game will fill the window. (e.g. 4:3 to 16:9)
* `--backend`: `drm` (default) or `headless`.
* `-O`, `--prefer-output`: connectors in order of preference (ex: `DP-1,DP-2,HDMI-A-1`)
* `--generate-drm-mode`: mode generation algorithm (`cvt`, `fixed`)
* `--immediate-flips`: enable immediate flips, may result in tearing

## Contributing

Workarounds are welcome. The bar is: it works, it is clean, and it is scoped to the driver
that needs it — never applied globally where it would cost another vendor something.

Telescope keeps merging upstream gamescope, so keeping the diff against upstream small is a
design constraint rather than an afterthought. Read [`UPSTREAM.md`](UPSTREAM.md) before you
write anything, and [`CLAUDE.md`](CLAUDE.md) for the architecture and conventions.
