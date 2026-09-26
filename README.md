# lens

A descriptor-driven live inspector for arbitrary game processes.

Point it at a YAML file, attach to a running process, and it resolves that
target's modules, entity lists, and field schema at runtime, then enumerates and
draws live entities. There is no engine-specific C++ in the core: a target is
data, and adding one is a file, not a fork.

```
lens run     adapters/sim.yaml          # attach + draw
lens resolve adapters/sim.yaml          # what resolved, what didn't, and why
lens list    adapters/sim.yaml          # dump every entity field as text
lens diff    before.json after.json     # semantic field diff
```

## The idea

Every tool in this space hardcodes one engine. When the game patches, the tool
is dead until its author updates it. lens moves that knowledge into a descriptor
and gives you a diff tool, so a patch produces a report instead of a crash.

Three resolution modes, all in the descriptor:

- **signature** — scan the module for a byte pattern with wildcards
- **pointer chain** — module base + offsets, or a RIP-relative pointer
- **schema table** — ask the *target* where its own fields live (Source 2
  netvars, Unreal's reflected properties). The target names its own fields;
  the descriptor references them by name and never bakes an address.

## Build

Needs a C++20 compiler. No third-party dependencies.

```sh
make -j4                 # release, both binaries
./build.sh               # same via cmake
make DEBUG=1             # -O0 -g3, LDEBUG traces on
make selftest            # build, then run the 48-check acceptance suite
```

Windows uses `src/core/mem_win.cpp` (ReadProcessMemory + module enumeration)
and `src/overlay/backend_win32.cpp` (DX11 swap-chain vtable patch from outside
the process, layered click-through window). Linux uses the POSIX shared-memory
transport and the offline renderer.

## Running it here

The repo ships a synthetic target so the full path runs on a bare checkout with
no game and no Windows:

```sh
lens selftest                            # 48 checks, spawns the sim itself
lens run   adapters/sim.yaml             # auto-spawns ./lens_sim
lens dump  adapters/sim.yaml out/frame.ppm --svg out/frame.svg
```

`lens_sim` publishes a POSIX shared-memory segment containing a synthetic
module image, real signature bytes at known offsets, an entity array, a moving
camera, and its own schema table — including a decoy table so the resolver's
`match:` selector is actually exercised.

## What the acceptance path proves

`lens selftest` runs 48 checks covering: signature scanning (exact, nibble, and
`??` wildcards), YAML descriptor parsing, attach, schema-table discovery,
entity enumeration, typed field reads, camera read, world-to-screen projection,
a real write, snapshot capture, and semantic diff. It spawns the synthetic
target itself and tears it down after.

```
$ lens selftest
lens selftest: 48 passed, 0 failed
```

## Descriptor reference

Top-level keys, in the order the resolver uses them:

| key | what it does |
|---|---|
| `target` | name, engine label, transport (`sim` or `win`), process identifier |
| `modules` | list of `{name, sig}` — scanned at attach, base = first match |
| `schema_table` | where the target publishes its own field table |
| `lists` | entity arrays and their fields |
| `camera` | view matrix, eye position, fov |
| `local_field` | which field marks the local player |

### `modules`

```yaml
modules:
  - name: client
    sig: 48 8B 05 ?? ?? ?? ?? 48 85 C0    # ?? = any byte
```

Wildcards: `??` any byte, `4?` low nibble any, `?4` high nibble any. Alignment
and max-hit count are on `Signature`.

### `schema_table`

```yaml
schema_table:
  module: client
  ptr_sig: 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 10
  match: 1                 # which hit to use when the pattern matches more than once
  rip_offset: 3            # disp32 at this offset from the match → pointer to the table
  entry_stride: 64
  array_off: 0             # entry + N  = u64 array address
  count_off: 8             #            u32 element count
  stride_off: 12           #            u32 element stride
  name_str_off: 32         #            char[32] field name
  max_entries: 64
  publish_as: cs2          # entries become "cs2.players", "cs2.camera", ...
```

`match` matters: real binaries have decoy patterns. The synthetic target
publishes two schema stubs and the descriptor picks the second.

### `lists`

```yaml
lists:
  - name: players
    module: client
    count_schema: players        # resolve by name from the schema table
    max: 64
    fields:
      - name: health
        kind: pointer
        module: client
        offset: 0x120            # into the element struct
        type: f32
        writable: true
```

`kind` is `pointer` (base + offset) or `rip` (RIP-relative). `type` is one of
`i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool str cstr vec3 f32x3 mat4`. `count` is
the element count for `str`, `cstr`, and array types. `deref` follows a pointer
before reading.

`writable: true` is what makes a field editable in the table view, and it only
takes effect when the transport was opened with `--write`.

### `camera`

Same field syntax, but addressed by `schema:` name instead of a module offset:

```yaml
camera:
  view: { schema: camera, offset: 0,  type: mat4 }
  pos:  { schema: camera, offset: 64, type: f32x3 }
  fov:  { schema: camera, offset: 80, type: f32 }
```

## When the game patches

This is the workflow the tool is built around.

```sh
lens resolve adapters/source2-cs2.yaml      # what still resolves
lens snapshot v1.json && ... && lens snapshot v2.json
lens diff v1.json v2.json                   # which fields changed
```

Every resolution carries a health state:

- **ok** — resolved and current
- **stale** — the anchor signature no longer matches; the address is from cache
- **dead** — nothing resolved; the field is skipped, not guessed

A patch that breaks one signature reports one stale line. The rest of the
descriptor keeps working, which is the whole point: a broken offset is a
one-line report, not a failed tool.

The synthetic target lets you see this without patching a real game — break a
signature in a copy of `adapters/sim.yaml` and `lens resolve` reports exactly
which field went stale while the other 10 stay ok.

## Layout

```
adapters/          one YAML per target; sim.yaml is the tested reference
config/lens.yaml   render, hotkeys, ESP behaviour, write permissions
src/core/          memory transports, pattern scanner, matrix math, YAML
src/descriptor/    descriptor model, loading, validation, resolution + caching
src/runtime/       attach, enumerate, typed read/write, camera, snapshot, diff
src/overlay/       draw list, font, offline PPM/SVG, Win32 DX11 hook
src/features/      ESP, corner panel, editable field table
src/script/        expression language over live frame values
src/sim/           the synthetic target
tools/gen_font.py  regenerates the glyph table
```

## Settings

`config/lens.yaml` holds the non-descriptor half — what's drawn, which fields,
hotkeys, and whether writes are permitted. Load it with `--config`. The
descriptor says where things *are*; the settings say what you want to *see*.

```sh
lens run adapters/sim.yaml --config config/lens.yaml --write
```

## Notes

- The overlay is external: a DX11 swap-chain vtable patch from outside the
  process, no DLL injection, no writes to the target's code.
- `mem_->add_bound()` installs a declared extent per module, schema, and list,
  and the read path refuses to leave it. A field offset that has drifted past
  its array reads as nothing rather than as whatever followed it in memory.
- Reads are the default. Writes require both `writable: true` on the field and
  `--write` on the command line.
- The Makefile uses `-MMD -MP`. Without it, editing a struct in a header leaves
  stale objects with the old layout, and the link succeeds straight into a
  segfault.
- `adapters/source2-cs2.yaml` and `adapters/ue5.yaml` are the schema-driven
  shape — structurally complete, but written against dumps rather than a live
  session, so they're the starting point for a real adapter, not a proven one.
