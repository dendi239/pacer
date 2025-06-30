# pacer

This is my project for analysing telemetry data from races.
Atm it's in the early stages so there're lots of questionable things laying around (such as filepaths baked in binary, lol).
It's still pretty much work-in-progress so proceed with caution.

## getting started

### env

I use [pixi](https://pixi.sh) for external dependency management.
It is much better than `conda`/`mamba`/`micromamba` while leveraging the same infrastructure.
I use `cmake` for building binary stuff, `litgen` for code-generation of bindings and `scikit-build` to glue two together via `pyproject.toml`.
Is it the best setup? No. Does it work? Somewhat.

```bash
pixi install  # that's the only thing you need.
```

After you done installing pixi stuff, presumably you can do following:

```bash
pixi shell                            # now you're in the shell with new python and stuff
cmake -S . -B build/Release -G Ninja  # creates build tree
cmake --build build/Release           # builds everything
```

### what to do?

There're two good places to get started:

- `timeline` app (build it with CMake, tweak source code to read your files), somewhat good, has delta, laps, sectors, but not proper interpolation;
- `notebooks/interpolation.ipynb` --- notebook with bunch of convenient stuff, has some gradient descent based timestamp interpolation, somewhat decent for analysis I want to do.

## components

Some components:

- `pacer/`: bread and butter --- main library code, mixed C++ and Python stuff;
- `apps/timeline.cpp`: main app: consists of bunch of different views on top of parsed data;
- `examples/`: bunch of examples of usage of 3rd party dependencies (e.g. implot, imgui, gpmf-parser);
- `notebooks/`: me hacking stuff and never tyding it up;
- `libs/`: parser for telemetry data, laps mangling, some geometry utilities;
- `bindings/`: python bindings that semi-automatically generated from C++ source code, not too stable.

## future ideas

Still in progress:

- actual video-feed inside `timeline` app;
- more on-line approach, e.g. to use live in session for comments like:
  - too much wheelspin on exit;
  - too little braking on entry;
  - shorter line is better;
  - keep minimum speed higher;
  - etc.
- timestamp interpolation in C++;
- emscripten based web app;
- gradient descent (maybe something else) to properly get timestamp within C++;
- clean up the code (lmao).

Wow, something already done:

- lap segmentation, comparision between laps with delta;
- nanobind-based python bindings to rapidly experiment in python (I've been putting it off due to shitty code);
- integration with 3rd party gps data, e.g. from sampled file, consider building ios app for capturing;

## credits

It's ain't much, but it's honest work.

pacer © 2025 by Denys Smirnov is licensed under CC BY-NC-SA 4.0.
To view a copy of this license, visit <https://creativecommons.org/licenses/by-nc-sa/4.0/>
