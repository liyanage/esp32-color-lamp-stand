#!/usr/bin/env python3
"""Replace the legacy PCB population with the ESP32-S3 redesign."""

from pathlib import Path
import sys
import xml.etree.ElementTree as ET

import pcbnew


ROOT = Path(__file__).resolve().parents[2]
SOURCE_BOARD = ROOT / "pcb" / "esp32-color-lamp-stand.kicad_pcb"
NETLIST = Path("/tmp/esp32-color-lamp-stand-redesign.xml")
OUTPUT = SOURCE_BOARD
FOOTPRINT_ROOT = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")
FOOTPRINT_IO = pcbnew.PCB_IO_KICAD_SEXPR()


def mm(value):
    return pcbnew.FromMM(value)


def point(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


def parse_netlist():
    root = ET.parse(NETLIST).getroot()
    components = {}
    for component in root.find("components"):
        reference = component.attrib["ref"]
        footprint = component.findtext("footprint")
        if not footprint:
            continue
        fields = {field.attrib["name"]: field.text or "" for field in component.findall("./fields/field")}
        components[reference] = {
            "value": component.findtext("value") or "",
            "footprint": footprint,
            "lcsc": fields.get("LCSC", ""),
        }

    connections = {}
    for net in root.find("nets"):
        name = net.attrib["name"]
        for node in net.findall("node"):
            connections[(node.attrib["ref"], node.attrib["pin"])] = name
    return components, connections


def load_footprint(identifier):
    library, name = identifier.split(":", 1)
    footprint = FOOTPRINT_IO.FootprintLoad(str(FOOTPRINT_ROOT / f"{library}.pretty"), name)
    if footprint is None:
        raise RuntimeError(f"Unable to load footprint {identifier}")
    footprint.SetFPID(pcbnew.LIB_ID(library, name))
    return footprint


def central_placement():
    return {
        "J1": (81.788, 44.0, -90),
        "U1": (129.5, 44.0, -90),
        "U2": (91.0, 44.0, 0),
        "U3": (105.0, 27.0, -90),
        "R1": (88.0, 37.5, 90),
        "R2": (88.0, 50.5, 90),
        "R3": (98.0, 40.5, 0),
        "R4": (98.0, 47.5, 0),
        "R5": (87.0, 54.0, 90),
        "R6": (95.0, 54.0, 90),
        "R7": (121.0, 62.0, 0),
        "R8": (128.0, 62.0, 0),
        "C1": (100.0, 24.0, 90),
        "C2": (110.0, 24.0, 90),
        "C3": (113.0, 28.0, 90),
        "C4": (113.0, 32.0, 90),
        "C5": (103.0, 54.0, 90),
        "C6": (106.0, 63.0, 0),
        "SW1": (87.0, 62.0, 0),
        "SW2": (95.0, 62.0, 0),
        "TP1": (121.0, 18.0, 0),
        "TP2": (129.0, 18.0, 0),
        "TP3": (125.0, 18.0, 0),
        "TP4": (121.0, 69.0, 0),
    }


def inward_cap_position(led_position):
    x, y, orientation = led_position
    if y < 10:
        return x, y + 7.0, orientation
    if y > 78:
        return x, y - 7.0, orientation
    if x < 10:
        return x + 7.0, y, orientation
    return x - 7.0, y, orientation


def main():
    components, connections = parse_netlist()
    source_board = pcbnew.LoadBoard(str(SOURCE_BOARD))

    old_led_positions = {}
    for footprint in source_board.GetFootprints():
        reference = footprint.GetReference()
        if reference.startswith("D") and reference[1:].isdigit():
            position = footprint.GetPosition()
            old_led_positions[reference] = (
                pcbnew.ToMM(position.x),
                pcbnew.ToMM(position.y),
                footprint.GetOrientationDegrees(),
            )

    if len(old_led_positions) != 32:
        raise RuntimeError(f"Expected 32 existing LED positions, found {len(old_led_positions)}")

    board = pcbnew.BOARD()
    for drawing in source_board.GetDrawings():
        board.Add(drawing.Duplicate())

    net_items = {}
    for name in sorted(set(connections.values())):
        item = pcbnew.NETINFO_ITEM(board, name)
        board.Add(item)
        net_items[name] = item

    placements = central_placement()
    placements.update(old_led_positions)
    for index in range(32):
        placements[f"C{index + 7}"] = inward_cap_position(old_led_positions[f"D{index + 1}"])

    missing = sorted(set(components) - set(placements))
    if missing:
        raise RuntimeError(f"No placement defined for: {', '.join(missing)}")

    for reference, component in sorted(components.items()):
        footprint = load_footprint(component["footprint"])
        footprint.SetReference(reference)
        footprint.SetValue(component["value"])
        x, y, orientation = placements[reference]
        footprint.SetPosition(point(x, y))
        footprint.SetOrientationDegrees(orientation)
        for pad in footprint.Pads():
            net_name = connections.get((reference, pad.GetNumber()))
            if net_name:
                pad.SetNet(net_items[net_name])
        board.Add(footprint)

    pcbnew.SaveBoard(str(OUTPUT), board)
    print(f"Saved {OUTPUT}")
    print(f"Footprints: {len(list(board.GetFootprints()))}")
    print(f"Tracks: {len(list(board.GetTracks()))}")
    print(f"Zones: {len(list(board.Zones()))}")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(error, file=sys.stderr)
        raise
