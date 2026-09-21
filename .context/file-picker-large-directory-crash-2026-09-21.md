# File picker large-directory crash

## Report

Opening a Linux directory with many entries in Sol's Causality file picker can crash the app.

## Confirmed ownership

`sol/src/ui/file_picker.c` builds one Causality button per directory entry. Its click-context pool grows with `realloc` while those addresses have already been supplied as `Ca_BtnDesc.click_data`. A growth relocation leaves earlier buttons with dangling callback pointers. This is reachable as soon as a listing exceeds the initial 64 contexts.

## Implemented fix

The picker reserves all callback contexts before each Causality rebuild, using checked capacity growth. Its render loop cannot relocate callback contexts. The file list is virtualized from Causality's scroll signal: it retains full scroll extent with spacers but creates at most 96 row widgets, plus a small overscan.

## Validation

`cmake --build build --target sol -j 8`, full `ctest --test-dir build --output-on-failure` (20/20), and `git diff --check` pass. Live Linux rendering remains pending because this workspace is running the macOS build.
