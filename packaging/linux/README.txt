Alien Resurrection - native port for Linux
==========================================

A native version of the PlayStation game Alien Resurrection (USA). You need
your own copy of the game: a disc image of Alien Resurrection (USA,
SLUS-00633) as a .cue sheet with its .bin track files. This download
contains no part of the game - neither data nor code. The game's code is
read from your disc image when you play.

Requirements
------------
- 64-bit Linux with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36,
  SteamOS 3 and later), X11 or Wayland
- A Vulkan graphics driver: Mesa (mesa-vulkan-drivers on Debian/Ubuntu,
  vulkan-radeon / vulkan-intel on Arch, mesa-vulkan-drivers on Fedora) or
  the NVIDIA driver

Getting started
---------------
1. Extract the archive anywhere (keep the lib folder next to the program).
2. Run ./AlienResurrection (from a terminal, or double-click it if your
   file manager runs programs).
3. Choose "Select disc image" and pick your Alien Resurrection (USA).cue.
4. Adjust Settings and Controls if you like, then "Start game".

Optional: to get a menu entry, edit AlienResurrection.desktop so that Exec=
and Icon= point to this folder, and copy it to ~/.local/share/applications/.

In game: F2 (or Select+Start on a controller) opens the pause menu with
settings and controls, F11 toggles fullscreen, F12 saves a screenshot.
F5 quick saves anywhere (one slot, quicksave.state next to the memory
cards) and F9 returns to that moment.

Files
-----
Settings (config.ini) and memory card saves (memcard0.mcr) are stored in:
    ~/.local/share/AlienResurrectionPort/AlienResurrection/

If the game stutters on an older PC, lower Settings > Frame rate boost.
Run it from a terminal to see its log when reporting a problem.

Licenses
--------
LICENSE.txt covers the port's own code. SDL 3 (zlib license) and Dear ImGui
(MIT license) are included under licenses/. Alien Resurrection is a
trademark of its owners; this project is not affiliated with them.
