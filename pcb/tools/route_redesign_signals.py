#!/usr/bin/env python3
"""Route point-to-point signal nets for the ESP32-S3 redesign."""

from collections import defaultdict
from pathlib import Path
import math

import pcbnew


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "pcb" / "esp32-color-lamp-stand.kicad_pcb"
OUTPUT = Path("/tmp/esp32-color-lamp-stand-signals.kicad_pcb")
POWER_NETS = {"/+5V", "/GND", "/+3V3"}


def distance(first, second):
    return math.hypot(first.x - second.x, first.y - second.y)


def minimum_spanning_edges(points):
    connected = {0}
    edges = []
    while len(connected) < len(points):
        _, start, end = min(
            (distance(points[a], points[b]), a, b)
            for a in connected
            for b in range(len(points))
            if b not in connected
        )
        edges.append((points[start], points[end]))
        connected.add(end)
    return edges


def add_track(board, net, start, end, width, layer=pcbnew.F_Cu):
    track = pcbnew.PCB_TRACK(board)
    track.SetStart(start)
    track.SetEnd(end)
    track.SetWidth(pcbnew.FromMM(width))
    track.SetLayer(layer)
    track.SetNet(net)
    board.Add(track)


def main():
    board = pcbnew.LoadBoard(str(SOURCE))
    pads_by_net = defaultdict(list)
    net_items = {}

    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            name = pad.GetNetname()
            if not name or name.startswith("unconnected-"):
                continue
            net_items[name] = pad.GetNet()
            position = pad.GetPosition()
            if all(position != existing for existing in pads_by_net[name]):
                pads_by_net[name].append(position)

    routed_nets = 0
    for name, points in sorted(pads_by_net.items()):
        if name in POWER_NETS or len(points) < 2:
            continue
        width = 0.20 if name in {"/USB_D+", "/USB_D-", "/USB_CONN_D+", "/USB_CONN_D-", "/USB_PROT_D+", "/USB_PROT_D-"} else 0.25
        for start, end in minimum_spanning_edges(points):
            add_track(board, net_items[name], start, end, width)
        routed_nets += 1

    pcbnew.SaveBoard(str(OUTPUT), board)
    print(f"Saved {OUTPUT}")
    print(f"Signal nets routed: {routed_nets}")
    print(f"Track segments: {len(list(board.GetTracks()))}")


if __name__ == "__main__":
    main()
