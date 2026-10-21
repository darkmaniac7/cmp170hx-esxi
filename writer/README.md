# CMP 170HX BAR1-to-64 GiB UEFI writer

A UEFI application (gnu-efi, x86_64) that resizes the BAR1 of NVIDIA CMP 170HX cards
to 64 GiB before the ESXi loader runs, then chain-loads that loader.

## Layout

- `four-card/` - the four-card variant (fail-open). See `four-card/README.md`.
- `two-card/` - the two-card variant (no fallback). See `two-card/README.md`.
- `Makefile` - builds either variant. `make four-card` (default) or `make two-card`,
  with `GNUEFI=/path/to/gnu-efi/usr`. Outputs land in `build/`.
- `window/` - the orchestration that drives a run (preparation, media, host and
  recovery steps) around the EFI application.
- `four-card/docs/` - placement arithmetic (`FOUR_CARD_PLACEMENT.md`) and the
  design note on running the writer on every host boot (`BOOT_PERSISTENCE.md`).

## Build

```sh
make GNUEFI=/path/to/gnu-efi/usr            # four-card (default)
make GNUEFI=/path/to/gnu-efi/usr two-card
make GNUEFI=/path/to/gnu-efi/usr all
```

Each variant's addresses and host pins are specific to one host and must be
regenerated for another; the writer refuses on any mismatch by design. There is no
rollback, retry or reset inside the app.
