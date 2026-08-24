# Telescope

*A gamescope fork for a gamemode that works on every vendor, driver workarounds included.*

Telescope started with a bug. High resolution and HDR corrupt on NVIDIA GPUs, because gamescope's scanout path assumes Vulkan-allocated memory is something the display engine can scan out. The Vulkan spec never promised that. It happens to be true on Mesa and it is not true on NVIDIA.

You can fix it by allocating through GBM, or you can wait for NVIDIA. They have been sitting on this since 2024, so GBM it is.

Except this turns out to be a bit of a philosophical minefield, left over from a vendor war that is not long past. Long story short, upstream gamescope does not want to use GBM.

So here is Telescope. A DRM-backend-focused gamescope fork whose only goal is to give you a working gamemode, no matter what hardware, no matter what philosophical stance is out there.

The compositor coexists with gamescope. Nothing it installs shares a path with upstream: binaries are `telescope`, `telescopectl`, `telescopereaper`, `telescopestream` and `telescope-type`, data lives in `/usr/share/telescope`, config in `/etc/telescope` and `~/.config/telescope`, and the WSI layer is `VK_LAYER_FROG_telescope_wsi` behind its own `ENABLE_TELESCOPE_WSI` gate so it never hooks gamescope's clients. Run gamescope for desktop or VR and Telescope for gamemode.

The session package is another story. `telescope-session` conflicts with `gamescope-session-steam` and has to. Two gamemode sessions cannot both own the display, and both of them ship `steamos-session-select`, which is the name Steam's "Switch to Desktop" button calls. You want one or the other.

I only care about one thing. Does it work? Is it clean code? Then it is in. And once it is fixed in the right place, meaning the driver, it comes back out again.

So shoot your workarounds my way. Open a PR. Let us make the Linux gaming ecosystem less of a philosophical battlefield and something that actually works.

## Status

Early, and not yet run on real hardware. The fork builds clean and its unit tests pass, but nothing here has been exercised on a real display. Do not put it on a machine you need working.

## Currently included workarounds

**NVIDIA: scanout buffers come from GBM, and composition is forced.**

The display engine will not scan out client-allocated buffers either, so every frame gets composited into a buffer we allocated.

This applies to NVIDIA only. AMD and Intel keep Vulkan-allocated scanout, and they keep direct scanout of client buffers, which is the copy this compositor exists to avoid. Where GBM is required there is no fallback, on purpose. Falling back to Vulkan allocation on such a driver does not produce an error, it produces a corrupt image, so Telescope refuses to start instead.

Measured on real hardware: framecount is identical patched and unpatched, so forcing composition costs nothing in steady state.

**NVIDIA: modesets take the link fully down, let it settle, then bring it back up** as a separate commit, instead of zeroing and refilling `CRTC_ID` / `ACTIVE` / `MODE_ID` in one atomic request.

Nothing guarantees how a driver sequences a disable-and-refill inside a single request, and on NVIDIA the link never seems to actually drop, so it never retrains. That is the corruption. This one is useful well beyond NVIDIA, see below.

## Making badly-behaved displays work

Plenty of sinks negotiate HDMI badly, AV receivers worst of all. None of it is the GPU's fault. The display just will not retrain its link properly unless you make it.

Telescope can force a real link drop on any driver, not only where a driver quirk applies:

| symptom | try |
|---|---|
| VRR never engages, or flickers, through an AV receiver | `drm_modeset_link_down=1` |
| Corruption or no signal after changing resolution or toggling HDR | `drm_modeset_link_down=1` |
| It helps, but not every time | raise `drm_modeset_link_down_settle_ms` |
| It works and you want the black screen shorter | lower `drm_modeset_link_down_settle_ms` |

Set them persistently in a Lua file, any `.lua` under `~/.config/telescope/scripts`:

```lua
-- ~/.config/telescope/scripts/my_receiver.lua
gamescope.convars.drm_modeset_link_down.value = 1
gamescope.convars.drm_modeset_link_down_settle_ms.value = 1000
```

Or against a running session, if you want to try a value without restarting:

```sh
telescopectl drm_modeset_link_down 1
```

`drm_modeset_link_down` defaults to `-1`, meaning "on where the driver is known to need it". `0` disables it everywhere and `1` forces it everywhere. The settle time defaults to 1000ms. That number came out of the experiment that found the workaround rather than any measurement of how long a link actually needs, so try lowering it.

> The Lua namespace is `gamescope.*`, not `telescope.*`. It stays that way so existing gamescope display-quirk scripts keep working.

## Currently included additional features

Nothing yet.

## Planned additional features

* Additional upscalers (SGSR, BCAS, xBR, Anime4K)
* Framegen?

## What was removed

Telescope targets the DRM/KMS session case only. Removed from upstream:

| removed | use instead |
|---|---|
| Wayland and SDL backends (nested) | upstream gamescope |
| OpenVR backend, SteamVR overlay forwarding | upstream gamescope |
| ReShade effect support | nothing |

`drm` and `headless` are the only backends left, and `headless` is there for CI and testing rather than for use.

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
* `-F fsr`: use AMD FidelityFX™ Super Resolution 1.0 for upscaling
* `-F nis`: use NVIDIA Image Scaling v1.0.3 for upscaling
* `-S integer`: use integer scaling.
* `-S stretch`: use stretch scaling, the game will fill the window. (e.g. 4:3 to 16:9)
* `--backend`: `drm` (default) or `headless`.
* `-O`, `--prefer-output`: connectors in order of preference (ex: `DP-1,DP-2,HDMI-A-1`)
* `--generate-drm-mode`: mode generation algorithm (`cvt`, `fixed`)
* `--immediate-flips`: enable immediate flips, may result in tearing

## Contributing

Workarounds are welcome. The bar is that it works, that the code is clean, and that it only
applies to the driver that needs it. Nothing gets applied globally where it would cost
another vendor something.

Telescope keeps merging upstream gamescope, so the size of our diff against upstream is a
design constraint rather than a detail. In practice that means new behaviour goes in new
files, hooks into upstream files stay to a line or two, and nothing gets reformatted,
reordered or renamed just because we would have written it differently. A deleted file is
one merge conflict you resolve once. A file you reindented conflicts forever.

We merge upstream, never rebase onto it.
