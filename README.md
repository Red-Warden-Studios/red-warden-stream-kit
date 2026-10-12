# Red Warden Stream Kit

**Free OBS Studio tools for people who stream, in one plugin and one installer.**

By [Red Warden Studios](https://redwardenstudios.com/division/bastion/stream-kit/). The Kit is a single OBS plugin (`rws-stream-kit.dll`) that adds one dock, **RW Stream Kit**, with three tools in it:

- **Multistream** - stream to several platforms at once from OBS's own Start Streaming button.
- **Pre-Flight** - checks your setup before you go live.
- **In-Flight** - a row of live checks that keeps an eye on the stream while you are on air.

Each tool can be switched on or off in the Kit's settings (the gear in the dock). A tool that is off does no work and shows nothing.

Needs OBS Studio 31.1 or newer on Windows 64-bit.

## Install

Run `RedWardenStreamKit-<version>-Setup.exe` and close OBS when asked. Start OBS and open the **Docks** menu to find **RW Stream Kit** (the first start docks it for you).

The installer replaces what earlier Kit versions and the standalone Multistream installer put on the PC (the separate `rws-multistream`, `rws-preflight`, `rws-inflight` and `rws-beacon` plugins) and keeps your destinations, stream keys and settings. To update, run a newer installer over the old one.

## Uninstall

Windows Settings > Apps > **Red Warden Stream Kit (OBS plugins)**. Destinations, settings and stream keys live in your OBS profile and Windows Credential Manager and are not touched.

## Manual install

The zip holds one folder, `rws-stream-kit`. Close OBS and copy it into `C:\ProgramData\obs-studio\plugins\` (or into a portable OBS's `plugins` folder). Do not leave an older `rws-multistream`, `rws-preflight` or `rws-inflight` folder next to it.

## Updates

The Kit checks `https://redwardenstudios.com/updates/stream-kit.json` once per OBS start and shows one line in the dock when a newer Kit exists. Nothing is downloaded or run automatically, and the check can be turned off in the Kit's settings.

## Repository layout

- `plugin/` - the Kit plugin: the dock, the Kit settings, the build files, the Kit's tests and the test scripts.
- `multistream/`, `preflight/`, `inflight/` - the three tools' sources and unit tests. `plugin/` builds them into the one DLL.
- `installer/` - the installer script (Inno Setup 6) and the update-notice file.
- `tools/` - the installer packaging script, the release test gate and the installer test.

Releases are tagged `kit-v<version>`. The source for every binary in a release is the tagged commit.

## Building from source

Windows only: Visual Studio 2022 and CMake 3.28 or newer. The first configure downloads the OBS sources and dependencies into `plugin\.deps`. From the repository root:

```
cd plugin
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

The result is `plugin\build_x64\RelWithDebInfo\rws-stream-kit.dll`, plus one test program per unit test.

## Tests

Two kinds, both run by `plugin\tools\run-all.ps1`:

- **Unit tests** (24 programs, built with the plugin) run without OBS.
- **Harnesses** start a portable copy of OBS with only `rws-stream-kit.dll` installed and drive it end to end: loading, the dock, each tool's behaviour, a stream through local relays, networks that fail or slow down, and twenty start/close cycles. They need a normal OBS install to copy from (or set `KIT_OBS_SOURCE` to an OBS folder), and they never touch your real OBS or its settings.

```
cd plugin
# quick check (partial: does not satisfy the release gate)
powershell -ExecutionPolicy Bypass -File tools\run-all.ps1 -Steps configure,build,unit

# full release run: one invocation with unit and every harness step
powershell -ExecutionPolicy Bypass -File tools\run-all.ps1 -Steps configure,build,unit,kit-dock,pf-load,pf-state,pf-state-nows,pf-integration,pf-ui,if-load,if-load-nows,if-audio,if-ui,ms-local,ms-resilience,ms-import,ms-bandwidth,net-light,exit-stress
```

Each run deletes the previous run's results, so the release gate only accepts one complete run.

Without `-Steps` it runs configure, build and the unit tests. The step names are listed at the top of the script. Each step writes `plugin\.testbed\run-<step>.txt` (its output) and `plugin\.testbed\status-<step>.txt` (exit code, result and the SHA-256 of the DLL it ran against). Close OBS first and run the harnesses one at a time; they start OBS windows that are hidden.

## Building the installer

`tools\package.ps1` builds nothing: it packages the existing RelWithDebInfo build and refuses unless every test step passed against exactly that DLL (`tools\check-test-results.ps1` compares the SHA-256 recorded by `run-all.ps1` with the DLL it is about to package, and again just before signing). Inno Setup 6 is required. `-NoSign` makes an unsigned dry run that writes to `dist-unsigned\`. `tools\test-installer.ps1` installs, upgrades and removes the Kit in a scratch folder with a silent installer. `tools\test-gate.ps1` tests the release gate itself.

## License

The Kit is GPL-2.0-or-later (see `LICENSE`). The GPL covers the code, not the name: "Red Warden" and the Red Warden logos are trademarks of Red Warden Studios LLC and are not licensed (see `NOTICE.txt`). If you distribute a modified version, give it a different name and remove the logos.

## Support

support@redwardenstudios.com
