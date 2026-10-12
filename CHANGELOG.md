# Changelog: Red Warden Stream Kit

Free OBS tools from Red Warden Studios. Newest first. Each entry is also the GitHub release text.

---

## 1.1.0 - 12 October 2026

**One plugin, one dock, and a new tool: In-Flight watches your stream while you are live.**

- **One dock for everything.** Multistream, Pre-Flight and In-Flight now live in a single "RW Stream Kit" dock. Multistream stays on top; below it the dock moves from Pre-Flight to In-Flight when you go live. Your destinations, stream keys and settings carry over from 1.0.x.
- **New: In-Flight.** Seven lights (Mic, Desktop, Scene, Rec, Net, PC, Stream) show at a glance whether your stream is healthy, with one plain sentence when something needs you.
- **A network light for every stream.** It judges OBS's own stream and each Multistream destination on its own, names the one that is dropping frames, and can turn just that one off while the rest keep going.
- **Levels.** Live meters for your mic and desktop audio, in dB or LUFS, with true peak and an optional reference line.
- **Mic test numbers.** Pre-Flight's mic test now shows how loud you were: Loudest and Typical.
- **Less setup.** If Pre-Flight already knows your mic, In-Flight uses the same audio choices automatically.
- **Tools on or off in Kit settings,** instead of checkboxes in the installer.
- **A user manual,** installed with the Kit and opened from the dock's ? button.

**Install:** close OBS and run `RedWardenStreamKit-1.1.0-Setup.exe`. It replaces Kit 1.0.x and the standalone Multistream and keeps your settings.

Windows 10/11 64-bit, designed for OBS Studio 31.1 or newer. Signed by Red Warden Studios LLC. Source is in this repository (GPL-2.0-or-later); this release was built from tag `kit-v1.1.0`.

Support: support@redwardenstudios.com

---

## 1.0.1 - 7 October 2026

**Red Warden Stream Kit 1.0.1**: one installer, pick the tools you want.

- **Red Warden Multistream 1.0.2:** stream to several platforms at once from OBS's own Start button. Your stream key is now refused anywhere in a server URL's path, however short, because OBS writes server URLs to its log.
- **Red Warden Pre-Flight 1.0.1 (new):** a dock that checks your mic, starting scene, stream setup, Multistream destinations, recording space and OBS performance before you go live, plus your own checklist.
