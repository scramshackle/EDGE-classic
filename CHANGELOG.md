# CHANGELOG for EDGE-Classic X.XX (since EDGE-Classic 1.53)

## New Features

- Additional DDFVERB presets: CHAMBER, HALL, CAVERN, SEWER and OUTDOORS

## General Improvements/Changes

- Migrated from SDL2 to SDL3
  - Miniaudio replaced with SDL3 AudioStream mechanisms
- Fluidlite and OpalMIDI replaced with EDGETracker's playback engine
  - Handles MIDI and IMF
  - Uses much smaller EGTB instrument bank files
  - Support for true OPL emulation and soundfonts removed

## General Bugfixes

- Fixed "Read This!" menu causing a CTD when no HELP* related images existed (custom standalone games)
