# Changelog

All notable changes to dvv-media. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same major and at least the minimum version, also for 0.x; breaking
changes only in a new major, after a deprecation. This file is the release notes (there are no
GitHub Releases).

## [0.1.0] - 2026-10-28

First versioned release.

### Added
- 37 built-in pixel formats (FFmpeg `pix_fmt` names) and custom formats described in JSON, with
  their plane geometry and the clip description JSON.
- Host and GPU frames and the frame store: host, staging and device tiers with budgets, LRU eviction
  and conversion plans.
- The source factory with the built-in providers in the component library `dvv::media_sources`:
  raw files, Y4M and synthetic patterns.
- GPU unpack and display shaders (Slang, embedded): the display transform applies the colour matrix,
  range and transfer, with PQ and HLG tone mapped to SDR.
- The CMake package `dvv_media` (`find_package(dvv_media 0.1)`, `SameMajorVersion`); it requires
  dvv-base, dvv-registry, dvv-platform and dvv-gpu 0.1.0 or later in the 0.x series.
