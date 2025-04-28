# pacer

This is my project for analysing telemetry data from races.
Atm it's in the early stages so there're lots of questionable things laying around (such as filepaths baked in binary, lol).
It's still pretty much work-in-progress so proceed with caution.

## getting started

I believe, main dependencies are already 

Bunch of views to take a look at:
- gps map;
- timeline to take a deeper look at data;
- table with entire payload;
- table with sectors (drag sector-line segments to change it);
- single lap telemetry.

## components

Some components:
- `apps/timeline.cpp`: main app: consists of bunch of different views on top of parsed data;
- `examples/`: bunch of examples of usage of 3rd party dependencies (e.g. implot, imgui, gpmf-parser);
- `libs/`: parser for telemetry data, laps mangling, some geometry utilities.

## future ideas

- actual video-feed inside `timeline` app;
- lap segmentation, comparision between laps with delta;
- more on-line approach, e.g. to use live in session for comments like:
    - too much wheelspin on exit;
    - too little braking on entry;
    - shorter line is better;
    - keep minimum speed higher;
    - etc.
- integration with 3rd party gps data, e.g. from sampled file, consider building ios app for capturing;
- nanobind-based python bindings to rapidly experiment in python (I've been putting it off due to shitty code);
- clean up the code (lmao).

## licence

It's ain't much, but it's honest work.

pacer © 2025 by Denys Smirnov is licensed under CC BY-NC-SA 4.0.
To view a copy of this license, visit https://creativecommons.org/licenses/by-nc-sa/4.0/
