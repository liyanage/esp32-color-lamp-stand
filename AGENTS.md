# Repository guidance for agents

## PCB sources

The authoritative hardware sources are the KiCad files in `pcb/`, including
the project-local symbol library and `sym-lib-table`. Gerbers, BOMs, CPLs, and
preview images are generated artifacts; do not edit them manually.

Preserve these intentional design choices unless the user explicitly requests
a redesign:

- The ESP32-S3 module antenna projects beyond the PCB edge.
- The board uses two copper layers with GND zones on both sides.
- Test points are present on the PCB but excluded from assembly.

Do not recreate or restore the retired redesign/migration scripts. They used
hard-coded placements and could overwrite the finished schematic or layout.

## PCB workflow

Run these commands from the repository root:

```sh
make -C pcb check
make -C pcb manufacturing
```

`check` runs error-level ERC and DRC, checks for unconnected items, and checks
schematic/PCB parity. `manufacturing` repeats those checks and regenerates the
JLCPCB Gerber ZIP, BOM, CPL, and preview images.

Run the checks after changing the schematic or PCB. Regenerate manufacturing
files only after the design is saved and routing and zones are final. Review
KiCad warnings and the generated previews before release.

The exporter requires an LCSC field on every assembled schematic component and
requires BOM and CPL designators to match. Keep JLCPCB part selections in the
schematic fields so the generated BOM remains reproducible.

## Git and external actions

- Use `--no-gpg-sign` when creating commits.
- Do not place, submit, or pay for a manufacturing order without explicit user
  confirmation.
- Uploading files for analysis is not authorization to place an order.

