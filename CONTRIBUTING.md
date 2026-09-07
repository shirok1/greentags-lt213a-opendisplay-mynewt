# Contributing

This is an experimental nRF51822 firmware port. Hardware validation is still pending.

1. Follow the toolchain and dependency setup in README.md.
2. Run `./tools/build.sh` and `./tools/test.sh` before opening a pull request.
3. Describe the behavior changed, validation performed, and any Flash/RAM change.
4. For hardware results include chip marking/revision, panel model, wiring, toolchain version, and whether SWD was attached. Never include device encryption keys or private configuration dumps.

Keep the 128 KiB Flash / 16 KiB RAM limits and the 2 KiB initial heap check. Do not relax resource checks to make a build pass. Generated `config_data.h` is tracked; edit `tools/generate_config.py`, regenerate, and include both changes.

Dependencies in `repos/` and build outputs in `bin/` are not committed. Vendored uzlib has a pinned revision and its own license; preserve its notices. Changes to the Mynewt LFRC restriction belong in the reproducible patch script, not only in the local dependency checkout.

Use `clang-format` with the repository configuration for first-party C code. Avoid formatting vendored sources. Host tests cannot establish radio timing, sleep current, flash endurance, or panel waveform correctness; distinguish simulation from hardware evidence.
