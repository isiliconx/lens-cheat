# tools

`gen_font.py` — regenerates `src/overlay/font_data.{h,cpp}`. The glyph table is
generated rather than hand-typed so a typo cannot ship as a scrambled character.

    python3 tools/gen_font.py

The three `*_probe.cpp` scratch harnesses that were used while bringing the
resolver up have been removed. They were diagnostic scaffolding, not part of the
product. To rebuild an equivalent probe, compile it against the same source set
the Makefile uses:

    g++ -std=c++20 -O0 -g -Isrc probe.cpp \
        src/core/*.cpp src/descriptor/*.cpp src/runtime/*.cpp \
        src/overlay/*.cpp src/features/*.cpp src/config/*.cpp src/script/*.cpp \
        -lrt -o probe

On Windows, drop `backend_win32.cpp` and `mem_win.cpp` from the list and add
`-luser32 -lpsapi -ladvapi32`.
