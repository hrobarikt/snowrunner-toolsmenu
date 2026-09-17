# Vendored dependencies

Copied in rather than fetched at build time, so a clone builds offline and the
exact source of the shipped binary is in this repository.

## imgui

Dear ImGui v1.91.5 (commit `f401021d5a5d56fe2304056c391e78f81c8d4b8f`), from
https://github.com/ocornut/imgui, under the MIT licence in `imgui/LICENSE.txt`.

Only what the app builds against: the core, and the Win32 and Direct3D 11
backends. The demo, the examples, the other backends and the docs are not here.
To update, copy the same files from a newer tag and note the version above.
