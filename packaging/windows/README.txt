Alien Resurrection - native port for Windows
=============================================

A native version of the PlayStation game Alien Resurrection (USA), made by
reverse engineering the original executables. You need your own copy of
the game: a disc image of Alien Resurrection (USA, SLUS-00633) as a .cue
sheet with its .bin track files. This download contains no part of the
game - neither data nor code. The game's code is read from your disc image
when you play.

Requirements
------------
- Windows 10 or 11, 64-bit
- A graphics card with Vulkan or Direct3D 12 support

Getting started
---------------
1. Unzip this folder anywhere (keep SDL3.dll next to AlienResurrection.exe).
2. Run AlienResurrection.exe.
3. Choose "Select disc image" and pick your Alien Resurrection (USA).cue.
4. Adjust Settings and Controls if you like, then "Start game".

In game: F2 (or Select+Start on a controller) opens the pause menu with
settings and controls, F11 toggles fullscreen, F12 saves a screenshot.
F5 quick saves anywhere (one slot, quicksave.state next to the memory
cards) and F9 returns to that moment.

Files
-----
Settings (config.ini), memory card saves (memcard0.mcr) and the log file
(alien.log) are stored in:
    %APPDATA%\AlienResurrectionPort\AlienResurrection\

Troubleshooting
---------------
- The game stutters on an older PC: lower Settings > Frame rate boost.
- The game does not start or looks wrong: in Settings, change "Graphics
  API" to Direct3D 12 (or Vulkan), then restart.
- alien.log in the folder above records what happened; include it when
  reporting a problem.
- Running from a command prompt shows the log there instead, and accepts
  the command-line options listed by: AlienResurrection.exe --help

Licenses
--------
LICENSE.txt covers the port's own code. SDL 3 (zlib license) and Dear ImGui
(MIT license) are included under licenses\. Alien Resurrection is a
trademark of its owners; this project is not affiliated with them.
