#!/usr/bin/env python3
"""Generate a campus master plan DXF (similar to big.dwg) for testing.

Creates a complex drawing with ~30K-60K entities including:
- Building footprints (rectangles, L-shapes, U-shapes)
- Roads (parallel lines)
- Vegetation (circles, arcs)
- Boundary lines
- Text labels
- Block references for repeated elements
"""

import math
import random
import os
import ezdxf
from ezdxf.enums import TextEntityAlignment

random.seed(42)

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), '..', 'test_data')


def add_building(msp, x, y, w, h, layer='BUILDING', rotation=0):
    """Add a building footprint as a closed LWPOLYLINE."""
    points = [(0, 0), (w, 0), (w, h), (0, h)]
    if rotation != 0:
        rad = math.radians(rotation)
        cos_r, sin_r = math.cos(rad), math.sin(rad)
        points = [(px * cos_r - py * sin_r + x, px * sin_r + py * cos_r + y)
                   for px, py in points]
    else:
        points = [(px + x, py + y) for px, py in points]
    msp.add_lwpolyline(points, format='xy', close=True,
                        dxfattribs={'layer': layer, 'color': 7})


def add_l_shape_building(msp, x, y, w, h, wing_w, wing_h, layer='BUILDING'):
    """Add an L-shaped building."""
    points = [
        (0, 0), (w, 0), (w, wing_h), (wing_w, wing_h),
        (wing_w, h), (0, h)
    ]
    points = [(px + x, py + y) for px, py in points]
    msp.add_lwpolyline(points, format='xy', close=True,
                        dxfattribs={'layer': layer, 'color': 7})


def add_road(msp, x1, y1, x2, y2, width, layer='ROAD'):
    """Add a road as two parallel lines."""
    dx = x2 - x1
    dy = y2 - y1
    length = math.sqrt(dx * dx + dy * dy)
    if length < 1e-6:
        return
    nx = -dy / length * width / 2
    ny = dx / length * width / 2
    msp.add_line((x1 + nx, y1 + ny), (x2 + nx, y2 + ny),
                  dxfattribs={'layer': layer, 'color': 9})
    msp.add_line((x1 - nx, y1 - ny), (x2 - nx, y2 - ny),
                  dxfattribs={'layer': layer, 'color': 9})


def add_trees(msp, cx, cy, count, radius_range=(2, 5), layer='VEGETATION'):
    """Add tree symbols (circles)."""
    for _ in range(count):
        tx = cx + random.uniform(-40, 40)
        ty = cy + random.uniform(-40, 40)
        r = random.uniform(*radius_range)
        msp.add_circle((tx, ty), radius=r,
                        dxfattribs={'layer': layer, 'color': 92})


def add_parking(msp, x, y, rows, cols, layer='PARKING'):
    """Add parking lot lines."""
    spot_w = 3.0
    spot_h = 6.0
    for row in range(rows):
        for col in range(cols):
            lx = x + col * spot_w
            ly = y + row * (spot_h + 1)
            msp.add_line((lx, ly), (lx, ly + spot_h),
                          dxfattribs={'layer': layer, 'color': 8})
            msp.add_line((lx, ly + spot_h), (lx + spot_w, ly + spot_h),
                          dxfattribs={'layer': layer, 'color': 8})


def create_campus():
    """Generate a campus master plan DXF."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    # Layers
    doc.layers.add('BUILDING', color=7)
    doc.layers.add('ROAD', color=9)
    doc.layers.add('ROAD_MAJOR', color=30)
    doc.layers.add('VEGETATION', color=92)
    doc.layers.add('BOUNDARY', color=1)
    doc.layers.add('PARKING', color=8)
    doc.layers.add('WATER', color=4)
    doc.layers.add('SIDEWALK', color=8)
    doc.layers.add('DIMENSION', color=3)
    doc.layers.add('TEXT', color=2)
    doc.layers.add('GRID', color=252)

    # Block for tree symbol
    tree_block = doc.blocks.new('TREE')
    tree_block.add_circle((0, 0), radius=3, dxfattribs={'color': 92})
    tree_block.add_line((-3, 0), (3, 0), dxfattribs={'color': 92})
    tree_block.add_line((0, -3), (0, 3), dxfattribs={'color': 92})

    # Block for bench
    bench_block = doc.blocks.new('BENCH')
    bench_block.add_line((0, 0), (2, 0), dxfattribs={'color': 8})
    bench_block.add_line((0, 0.3), (2, 0.3), dxfattribs={'color': 8})

    # Block for lamp
    lamp_block = doc.blocks.new('LAMP')
    lamp_block.add_circle((0, 0), radius=0.5, dxfattribs={'color': 7})
    lamp_block.add_line((-0.5, 0), (0.5, 0), dxfattribs={'color': 7})
    lamp_block.add_line((0, -0.5), (0, 0.5), dxfattribs={'color': 7})

    # --- Boundary ---
    boundary_size = 2000
    msp.add_lwpolyline(
        [(0, 0), (boundary_size, 0), (boundary_size, boundary_size),
         (0, boundary_size)],
        format='xy', close=True,
        dxfattribs={'layer': 'BOUNDARY', 'color': 1})

    # --- Grid lines ---
    grid_spacing = 100
    for i in range(0, boundary_size + 1, grid_spacing):
        msp.add_line((i, 0), (i, boundary_size),
                      dxfattribs={'layer': 'GRID', 'color': 252})
        msp.add_line((0, i), (boundary_size, i),
                      dxfattribs={'layer': 'GRID', 'color': 252})

    # --- Main roads ---
    # Horizontal main road
    add_road(msp, 0, 500, boundary_size, 500, 30, 'ROAD_MAJOR')
    add_road(msp, 0, 1200, boundary_size, 1200, 30, 'ROAD_MAJOR')
    # Vertical main roads
    add_road(msp, 500, 0, 500, boundary_size, 25, 'ROAD_MAJOR')
    add_road(msp, 1200, 0, 1200, boundary_size, 25, 'ROAD_MAJOR')

    # --- Secondary roads ---
    for y in range(100, boundary_size, 200):
        add_road(msp, 0, y, boundary_size, y, 8, 'ROAD')
    for x in range(100, boundary_size, 200):
        add_road(msp, x, 0, x, boundary_size, 8, 'ROAD')

    # --- Buildings (random placement) ---
    building_count = 0
    for _ in range(500):
        bx = random.uniform(50, boundary_size - 100)
        by = random.uniform(50, boundary_size - 100)
        bw = random.uniform(15, 60)
        bh = random.uniform(15, 60)
        rot = random.choice([0, 0, 0, 0, 90, 45])

        if random.random() < 0.3:
            wing_w = bw * random.uniform(0.3, 0.6)
            wing_h = bh * random.uniform(0.3, 0.6)
            add_l_shape_building(msp, bx, by, bw, bh, wing_w, wing_h)
        else:
            add_building(msp, bx, by, bw, bh, rotation=rot)
        building_count += 1

    # --- Large landmark buildings ---
    landmarks = [
        (800, 800, 150, 100, 'Building_A'),
        (300, 1400, 200, 80, 'Building_B'),
        (1500, 600, 120, 180, 'Building_C'),
        (1000, 1600, 180, 120, 'Building_D'),
    ]
    for lx, ly, lw, lh, name in landmarks:
        add_building(msp, lx, ly, lw, lh)
        msp.add_text(name, dxfattribs={
            'layer': 'TEXT', 'height': 8.0,
            'insert': (lx + lw / 2, ly + lh + 10)
        })

    # --- Vegetation ---
    for _ in range(800):
        tx = random.uniform(10, boundary_size - 10)
        ty = random.uniform(10, boundary_size - 10)
        r = random.uniform(2, 8)
        msp.add_circle((tx, ty), radius=r,
                        dxfattribs={'layer': 'VEGETATION', 'color': 92})

    # Insert tree blocks
    for _ in range(400):
        tx = random.uniform(10, boundary_size - 10)
        ty = random.uniform(10, boundary_size - 10)
        scale = random.uniform(0.5, 2.0)
        msp.add_blockref('TREE', insert=(tx, ty), dxfattribs={
            'xscale': scale, 'yscale': scale, 'layer': 'VEGETATION'
        })

    # --- Parking lots ---
    for _ in range(20):
        px = random.uniform(100, boundary_size - 200)
        py = random.uniform(100, boundary_size - 200)
        add_parking(msp, px, py, random.randint(2, 5), random.randint(5, 15))

    # --- Water features ---
    # Lake
    msp.add_circle((1700, 1700), radius=80,
                    dxfattribs={'layer': 'WATER', 'color': 4})
    # Pond
    for _ in range(5):
        px = random.uniform(100, boundary_size - 100)
        py = random.uniform(100, boundary_size - 100)
        pr = random.uniform(10, 30)
        msp.add_circle((px, py), radius=pr,
                        dxfattribs={'layer': 'WATER', 'color': 4})

    # --- Arcs (curved paths, roundabouts) ---
    for _ in range(30):
        cx = random.uniform(100, boundary_size - 100)
        cy = random.uniform(100, boundary_size - 100)
        r = random.uniform(15, 50)
        start = random.uniform(0, 360)
        end = start + random.uniform(90, 300)
        msp.add_arc((cx, cy), radius=r, start_angle=start, end_angle=end,
                     dxfattribs={'layer': 'ROAD', 'color': 9})

    # --- Sidewalks (arcs and lines near roads) ---
    for y in [500, 1200]:
        msp.add_line((0, y + 18), (boundary_size, y + 18),
                      dxfattribs={'layer': 'SIDEWALK', 'color': 8})
        msp.add_line((0, y - 18), (boundary_size, y - 18),
                      dxfattribs={'layer': 'SIDEWALK', 'color': 8})

    # --- Bench and lamp block references ---
    for _ in range(100):
        bx = random.uniform(50, boundary_size - 50)
        by = random.uniform(50, boundary_size - 50)
        msp.add_blockref('BENCH', insert=(bx, by),
                          dxfattribs={'layer': 'SIDEWALK', 'rotation': random.uniform(0, 180)})

    for _ in range(200):
        lx = random.uniform(50, boundary_size - 50)
        ly = random.uniform(50, boundary_size - 50)
        msp.add_blockref('LAMP', insert=(lx, ly),
                          dxfattribs={'layer': 'SIDEWALK'})

    # --- Text labels ---
    labels = [
        'Main Gate', 'Library', 'Science Park', 'Dormitory Area',
        'Sports Center', 'Administration', 'Cafeteria', 'Lab Building',
        'Auditorium', 'Student Center', 'Parking A', 'Parking B',
        'Garden', 'Playground', 'Conference Hall', 'Gate North', 'Gate East'
    ]
    for i, label in enumerate(labels):
        lx = random.uniform(100, boundary_size - 100)
        ly = random.uniform(100, boundary_size - 100)
        msp.add_text(label, dxfattribs={
            'layer': 'TEXT', 'height': random.uniform(5, 15),
            'insert': (lx, ly)
        })

    # Save
    path = os.path.join(OUTPUT_DIR, 'campus_masterplan.dxf')
    doc.saveas(path)

    # Count entities
    count = len(list(msp))
    print(f'Created: {path} ({count} entities)')
    return path


if __name__ == '__main__':
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    create_campus()
