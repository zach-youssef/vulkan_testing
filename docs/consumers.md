# Downstream consumers

Other repositories depend on the wrapper, ZGAL and their web output. Check them when changing what
they rely on.

## PPU (`~/code/ppu`)

PPU uses the wrapper and ZGAL as a git submodule (`external/vulkan_test`) and builds with
`zgal.mk`.

To try PPU against wrapper changes that aren't pushed yet, point its build at this checkout:
`make -C ~/code/ppu ZGAL_ROOT=<this checkout>` (or `make -C ~/code/ppu web ZGAL_ROOT=...`). Bump
PPU's submodule only after the wrapper change is pushed.

## personal-site (`~/code/personal-site`)

The site serves `/projects/vkPPU` and `/projects/zgal` (programs built for the web) and ZGAL's
reference at `/zgal/docs/`, which the README links to. It has its own submodules of both:
`external/zgal` (this repository) and `external/vk_ppu` (PPU, with this repository nested in it).
Its `./build-zgal [checkout]` builds from `external/zgal` by default; pass this checkout to try
unpushed changes. That script relies on:

- `make web docs`;
- the layout of `build/web/particles/`: `particles.zgal.html`, its `*.js` files and the `zgal/`
  runtime directory, which it copies;
- `examples/particles/particles.zgal`, which the page shows beside the program;
- the generated main's `--frames`, `--size` and `--capture` options, run under Node for the
  screenshot shown to browsers without WebGPU;
- all of `build/docs` (`zgal.html` with its stylesheets and scripts, and `zgal.txt`), copied as the
  served docs.

Its pages rely on:

- `<html data-zgal="running|stopped|failed">`, `#zgal-message` and `canvas#zgal` from
  `web/zgal/main-browser.ts`;
- asset URLs resolved relative to the generated main's `import.meta.url` (`dsl/private/ts-main.rkt`),
  so a built program can be served from any path;
- PPU's `tests/approved/<game>/60.png` screenshots;
- the `/zgal/docs/` URL.

When changing any of these, check `build-zgal` and the site's end-to-end tests:
`e2e/tests/ppu*.spec.ts` and `e2e/tests/zgal*.spec.ts`.
