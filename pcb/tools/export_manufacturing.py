#!/usr/bin/env python3
"""Validate the KiCad project and create JLCPCB upload files."""

from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


PCB_DIR = Path(__file__).resolve().parents[1]
PROJECT_NAME = "esp32-color-lamp-stand"
SCHEMATIC = PCB_DIR / f"{PROJECT_NAME}.kicad_sch"
BOARD = PCB_DIR / f"{PROJECT_NAME}.kicad_pcb"
DEFAULT_OUTPUT = PCB_DIR / "manufacturing" / "jlcpcb"
GERBER_SUFFIXES = (
    "F_Cu.gtl",
    "B_Cu.gbl",
    "F_Mask.gts",
    "B_Mask.gbs",
    "F_Silkscreen.gto",
    "B_Silkscreen.gbo",
    "Edge_Cuts.gm1",
    "PTH.drl",
    "NPTH.drl",
)


def find_kicad_cli() -> str:
    configured = os.environ.get("KICAD_CLI")
    if configured:
        return configured

    found = shutil.which("kicad-cli")
    if found:
        return found

    macos_path = Path("/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    if macos_path.is_file():
        return str(macos_path)

    raise RuntimeError("kicad-cli not found; install KiCad or set KICAD_CLI")


def run(command: list[str]) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, check=True, shell=False)


def check_project(kicad_cli: str, work_dir: Path) -> None:
    erc_report = work_dir / "erc.rpt"
    drc_report = work_dir / "drc.rpt"

    run(
        [
            kicad_cli,
            "sch",
            "erc",
            "--output",
            str(erc_report),
            "--severity-error",
            "--exit-code-violations",
            str(SCHEMATIC),
        ]
    )
    run(
        [
            kicad_cli,
            "pcb",
            "drc",
            "--output",
            str(drc_report),
            "--severity-error",
            "--schematic-parity",
            "--exit-code-violations",
            str(BOARD),
        ]
    )


def export_gerbers(kicad_cli: str, work_dir: Path) -> Path:
    gerber_dir = work_dir / "gerbers"
    gerber_dir.mkdir()
    run(
        [
            kicad_cli,
            "pcb",
            "export",
            "gerbers",
            "--output",
            str(gerber_dir),
            "--layers",
            "F.Cu,B.Cu,F.Mask,B.Mask,F.Silkscreen,B.Silkscreen,Edge.Cuts",
            "--subtract-soldermask",
            "--check-zones",
            str(BOARD),
        ]
    )
    run(
        [
            kicad_cli,
            "pcb",
            "export",
            "drill",
            "--output",
            str(gerber_dir),
            "--format",
            "excellon",
            "--drill-origin",
            "absolute",
            "--excellon-units",
            "mm",
            "--excellon-separate-th",
            str(BOARD),
        ]
    )
    return gerber_dir


def export_bom(kicad_cli: str, work_dir: Path) -> Path:
    path = work_dir / "jlcpcb_bom.csv"
    run(
        [
            kicad_cli,
            "sch",
            "export",
            "bom",
            "--output",
            str(path),
            "--fields",
            "Value,Reference,Footprint,LCSC,QUANTITY",
            "--labels",
            "Comment,Designator,Footprint,LCSC Part #,Quantity",
            "--group-by",
            "Value,Footprint,LCSC",
            "--sort-field",
            "Reference",
            "--exclude-dnp",
            "--ref-range-delimiter",
            "",
            str(SCHEMATIC),
        ]
    )

    with path.open(newline="") as source:
        rows = [
            row
            for row in csv.DictReader(source)
            if not row["Designator"].startswith("TP")
        ]

    missing = [row["Designator"] for row in rows if not row["LCSC Part #"]]
    if missing:
        raise RuntimeError("Missing LCSC fields: " + ", ".join(missing))

    fieldnames = ["Comment", "Designator", "Footprint", "LCSC Part #", "Quantity"]
    with path.open("w", newline="") as destination:
        writer = csv.DictWriter(
            destination,
            fieldnames=fieldnames,
            quoting=csv.QUOTE_ALL,
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)
    return path


def export_cpl(kicad_cli: str, work_dir: Path) -> Path:
    raw_path = work_dir / "positions.csv"
    output_path = work_dir / "jlcpcb_cpl.csv"
    run(
        [
            kicad_cli,
            "pcb",
            "export",
            "pos",
            "--output",
            str(raw_path),
            "--side",
            "front",
            "--format",
            "csv",
            "--units",
            "mm",
            "--exclude-dnp",
            str(BOARD),
        ]
    )

    with raw_path.open(newline="") as source:
        rows = [row for row in csv.DictReader(source) if not row["Ref"].startswith("TP")]

    with output_path.open("w", newline="") as destination:
        writer = csv.writer(
            destination,
            quoting=csv.QUOTE_ALL,
            lineterminator="\n",
        )
        writer.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        writer.writerows(
            [row["Ref"], row["PosX"], row["PosY"], row["Side"], row["Rot"]]
            for row in rows
        )
    return output_path


def designators_from_bom(path: Path) -> set[str]:
    with path.open(newline="") as source:
        return {
            designator
            for row in csv.DictReader(source)
            for designator in row["Designator"].split(",")
        }


def designators_from_cpl(path: Path) -> set[str]:
    with path.open(newline="") as source:
        return {row["Designator"] for row in csv.DictReader(source)}


def validate_assembly_files(bom: Path, cpl: Path) -> None:
    bom_designators = designators_from_bom(bom)
    cpl_designators = designators_from_cpl(cpl)
    if bom_designators != cpl_designators:
        missing_from_cpl = sorted(bom_designators - cpl_designators)
        missing_from_bom = sorted(cpl_designators - bom_designators)
        raise RuntimeError(
            f"BOM/CPL mismatch; missing from CPL: {missing_from_cpl}; "
            f"missing from BOM: {missing_from_bom}"
        )
    print(f"Validated {len(bom_designators)} assembled components")


def create_gerber_zip(gerber_dir: Path, work_dir: Path) -> Path:
    archive = work_dir / f"{PROJECT_NAME}-gerbers.zip"
    files = [gerber_dir / f"{PROJECT_NAME}-{suffix}" for suffix in GERBER_SUFFIXES]
    missing = [str(path) for path in files if not path.is_file()]
    if missing:
        raise RuntimeError("Missing Gerber/drill files: " + ", ".join(missing))

    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
        for path in files:
            output.write(path, path.name)
    return archive


def render_previews(kicad_cli: str, work_dir: Path) -> tuple[Path, Path]:
    outputs = []
    for side in ("top", "bottom"):
        path = work_dir / f"board-{side}.png"
        run(
            [
                kicad_cli,
                "pcb",
                "render",
                "--output",
                str(path),
                "--side",
                side,
                "--width",
                "2400",
                "--height",
                "1600",
                "--quality",
                "high",
                "--background",
                "opaque",
                str(BOARD),
            ]
        )
        outputs.append(path)
    return outputs[0], outputs[1]


def publish(files: list[Path], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    expected_names = {path.name for path in files}
    obsolete_names = {"jlcpcb_cpl_formatted.csv"}

    for path in files:
        temporary = output_dir / f".{path.name}.tmp"
        shutil.copy2(path, temporary)
        temporary.replace(output_dir / path.name)

    for name in obsolete_names - expected_names:
        obsolete = output_dir / name
        if obsolete.exists():
            obsolete.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check", "export"), nargs="?", default="export")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    arguments = parser.parse_args()

    kicad_cli = find_kicad_cli()
    with tempfile.TemporaryDirectory(prefix="esp32-lamp-manufacturing-") as temporary:
        work_dir = Path(temporary)
        check_project(kicad_cli, work_dir)
        if arguments.command == "check":
            print("ERC and DRC passed")
            return 0

        gerber_dir = export_gerbers(kicad_cli, work_dir)
        bom = export_bom(kicad_cli, work_dir)
        cpl = export_cpl(kicad_cli, work_dir)
        validate_assembly_files(bom, cpl)
        archive = create_gerber_zip(gerber_dir, work_dir)
        top_preview, bottom_preview = render_previews(kicad_cli, work_dir)
        publish([archive, bom, cpl, top_preview, bottom_preview], arguments.output)

    print(f"Manufacturing files written to {arguments.output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
