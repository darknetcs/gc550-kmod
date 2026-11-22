# gc550 — Linux driver for the AVerMedia Live Gamer Extreme (GC550)

Work in progress. See [DESIGN.md](DESIGN.md) for the design, the safety rules and the
bring-up plan. **Nothing in this repository is ready to load yet.**

## Layout

| Path | What |
|---|---|
| `src/gc550-guard.[ch]` | the command allow-list; nothing reaches the card without passing it |
| `src/gc550-bootstrap.[ch]` | walking and checking the bootstrap table |
| `src/gc550-bootstrap-data.c` | the bootstrap table, **generated** by `tools/gen-bootstrap.py` |
| `src/gc550-parser.[ch]` | capture stream parser (video frames and audio) |
| `tools/recordings/` | the recorded Windows bootstrap the table is generated from |
| `tests/` | userspace unit tests; they never touch a device |

## Tests

```
make -C tests                 # guard, bootstrap table, parser, and a differential
                              # test against lgx2userspace's FrameBuilder
make -C tests LGX_SRC=        # without the differential test
```

Regenerate the table (the output is committed, so building never needs Python):

```
tools/gen-bootstrap.py tools/recordings/gc550-f150158.txt > src/gc550-bootstrap-data.c
```

## Credits

The protocol knowledge, the bootstrap recording and the stream format come from
[lgx2userspace](https://github.com/ChrisAJS/lgx2userspace) (MIT, Copyright 2021 Chris
Sawczuk and contributors). This driver is `GPL-2.0-only OR MIT`.
