Alien Resurrection - native port for macOS
==========================================

A native version of the PlayStation game Alien Resurrection (USA). You need
your own copy of the game: a disc image of Alien Resurrection (USA,
SLUS-00633) as a .cue sheet with its .bin track files. This download
contains no part of the game - neither data nor code. The game's code is
read from your disc image when you play.

Requirements
------------
- macOS 11 (Big Sur) or later, Apple Silicon or Intel

Getting started
---------------
1. Move AlienResurrection.app to your Applications folder (optional).
2. Open it. The app is not notarized by Apple, so the first time macOS
   blocks it: open System Settings > Privacy & Security, scroll down and
   choose "Open Anyway" for AlienResurrection, then open it again.
3. Choose "Select disc image" and pick your Alien Resurrection (USA).cue.
4. Adjust Settings and Controls if you like, then "Start game".

In game: F2 (or Select+Start on a controller) opens the pause menu with
settings and controls, F11 or Cmd+Ctrl+F toggles fullscreen, F12 saves a
screenshot.
F5 quick saves anywhere (one slot, quicksave.state next to the memory
cards) and F9 returns to that moment.

Files
-----
Settings (config.ini) and memory card saves (memcard0.mcr) are stored in:
    ~/Library/Application Support/AlienResurrectionPort/AlienResurrection/

If the game stutters on an older Mac, lower Settings > Frame rate boost.

Licenses
--------
LICENSE.txt covers the port's own code. SDL 3 (zlib license) and Dear ImGui
(MIT license) are included under licenses/. Alien Resurrection is a
trademark of its owners; this project is not affiliated with them.
