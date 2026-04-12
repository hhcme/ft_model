#!/usr/bin/env python3
"""Generate test DXF files for the CAD engine parser.

Creates multiple DXF files covering different entity types,
layer configurations, and edge cases. These files are used
for parser validation — not for visual regression testing.

Usage:
    python scripts/gen_test_dxf.py
"""

import os
import math
import ezdxf
from ezdxf.enums import TextEntityAlignment


OUTPUT_DIR = os.path.join(os.path.dirname(__file__), '..', 'test_data')


def create_basic_primitives():
    """Simple file with LINE, CIRCLE, ARC entities across layers."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    # Layer setup
    doc.layers.add('GEOMETRY', color=7)  # white
    doc.layers.add('RED_LAYER', color=1)
    doc.layers.add('GREEN_LAYER', color=3)
    doc.layers.add('BLUE_LAYER', color=5)

    # Lines on GEOMETRY layer
    for i in range(10):
        msp.add_line((0, i * 10), (100, i * 10 + 5), dxfattribs={'layer': 'GEOMETRY'})

    # Diagonal lines on RED_LAYER
    for i in range(5):
        msp.add_line((i * 20, 0), (i * 20 + 10, 100), dxfattribs={'layer': 'RED_LAYER'})

    # Circles on GREEN_LAYER
    for i in range(5):
        cx = 200 + i * 30
        msp.add_circle((cx, 50), radius=10 + i * 2, dxfattribs={'layer': 'GREEN_LAYER'})

    # A large circle to test high segment count
    msp.add_circle((200, -100), radius=500, dxfattribs={'layer': 'GREEN_LAYER'})

    # Arcs on BLUE_LAYER
    for i in range(8):
        cx = 400 + i * 25
        start = i * 45
        end = start + 180 + i * 10
        msp.add_arc((cx, 50), radius=10, start_angle=start, end_angle=end,
                     dxfattribs={'layer': 'BLUE_LAYER'})

    # Full circle as arc (0-360)
    msp.add_arc((500, 50), radius=15, start_angle=0, end_angle=360,
                 dxfattribs={'layer': 'BLUE_LAYER'})

    path = os.path.join(OUTPUT_DIR, 'basic_primitives.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_lwpolyline_test():
    """LWPOLYLINE with various vertex configurations and bulge arcs."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add('POLYLINES', color=4)

    # Simple rectangle (closed)
    msp.add_lwpolyline(
        [(0, 0), (100, 0), (100, 50), (0, 50)],
        format='xy', close=True,
        dxfattribs={'layer': 'POLYLINES'}
    )

    # Open polyline with varying bulge
    msp.add_lwpolyline(
        [(0, 100, 0.5), (50, 100, -0.5), (100, 100, 0.0), (150, 100)],
        format='xyb',
        dxfattribs={'layer': 'POLYLINES'}
    )

    # Star shape (closed with sharp vertices)
    points = []
    for i in range(10):
        angle = math.radians(i * 36)
        r = 50 if i % 2 == 0 else 25
        points.append((r * math.cos(angle) + 200, r * math.sin(angle) + 150))
    msp.add_lwpolyline(points, format='xy', close=True,
                        dxfattribs={'layer': 'POLYLINES'})

    # Polyline with all bulge = 1 (semicircles between vertices)
    msp.add_lwpolyline(
        [(0, 250, 1.0), (40, 250, 1.0), (80, 250, 1.0), (120, 250, 1.0)],
        format='xyb', close=True,
        dxfattribs={'layer': 'POLYLINES'}
    )

    # Single segment polyline (2 vertices)
    msp.add_lwpolyline(
        [(0, 300), (200, 300)],
        format='xy',
        dxfattribs={'layer': 'POLYLINES'}
    )

    # Degenerate: single vertex polyline (should be handled gracefully)
    msp.add_lwpolyline(
        [(300, 300)],
        format='xy',
        dxfattribs={'layer': 'POLYLINES'}
    )

    path = os.path.join(OUTPUT_DIR, 'lwpolyline.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_insert_test():
    """INSERT (block reference) with nested blocks and scaling."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    # Define a simple block: a square with a circle inside
    block = doc.blocks.new('UNIT_CELL')
    block.add_line((0, 0), (10, 0))
    block.add_line((10, 0), (10, 10))
    block.add_line((10, 10), (0, 10))
    block.add_line((0, 10), (0, 0))
    block.add_circle((5, 5), radius=3)

    # Insert the block multiple times (grid layout)
    for row in range(5):
        for col in range(5):
            x = col * 20
            y = row * 20
            msp.add_blockref('UNIT_CELL', insert=(x, y))

    # Scaled insert
    msp.add_blockref('UNIT_CELL', insert=(150, 0), dxfattribs={'xscale': 2.0, 'yscale': 3.0})

    # Rotated insert
    msp.add_blockref('UNIT_CELL', insert=(150, 50), dxfattribs={'rotation': 45.0})

    # Nested block: block that references another block
    outer_block = doc.blocks.new('GRID_2X2')
    outer_block.add_blockref('UNIT_CELL', insert=(0, 0))
    outer_block.add_blockref('UNIT_CELL', insert=(15, 0))
    outer_block.add_blockref('UNIT_CELL', insert=(0, 15))
    outer_block.add_blockref('UNIT_CELL', insert=(15, 15))
    msp.add_blockref('GRID_2X2', insert=(0, 120))

    path = os.path.join(OUTPUT_DIR, 'insert_blocks.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_text_test():
    """TEXT and MTEXT entities (Phase 2, but we generate now for forward compat)."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add('TEXT_LAYER', color=2)

    # Simple text
    msp.add_text('Hello CAD Engine', dxfattribs={
        'layer': 'TEXT_LAYER',
        'height': 5.0,
        'insert': (0, 0, 0)
    })

    # Multiple text with different heights
    for i in range(5):
        msp.add_text(f'Text height {i+2}', dxfattribs={
            'layer': 'TEXT_LAYER',
            'height': float(i + 2),
            'insert': (0, 20 + i * 15)
        })

    # Multiline text
    msp.add_mtext('Line 1\nLine 2\nLine 3', dxfattribs={
        'layer': 'TEXT_LAYER',
        'char_height': 3.0,
        'insert': (200, 0)
    })

    path = os.path.join(OUTPUT_DIR, 'text_entities.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_edge_cases():
    """Edge cases: empty file, zero-radius circle, overlapping entities, etc."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add('EDGE', color=6)

    # Zero-radius circle (degenerate — should be a point)
    msp.add_circle((0, 0), radius=0, dxfattribs={'layer': 'EDGE'})

    # Very small circle (should get minimum segments)
    msp.add_circle((50, 0), radius=0.001, dxfattribs={'layer': 'EDGE'})

    # Very large circle
    msp.add_circle((0, 0), radius=1e6, dxfattribs={'layer': 'EDGE'})

    # Arc with very small angle span
    msp.add_arc((100, 0), radius=10, start_angle=0, end_angle=0.01,
                 dxfattribs={'layer': 'EDGE'})

    # Arc with full 360 degrees (same as circle)
    msp.add_arc((100, 50), radius=5, start_angle=0, end_angle=360,
                 dxfattribs={'layer': 'EDGE'})

    # Line with zero length (point)
    msp.add_line((200, 0), (200, 0), dxfattribs={'layer': 'EDGE'})

    # Line with very large coordinates
    msp.add_line((-1e8, -1e8), (1e8, 1e8), dxfattribs={'layer': 'EDGE'})

    # Entities on a frozen layer (should parse but not render)
    doc.layers.add('FROZEN', color=1)
    doc.layers.get('FROZEN').freeze()
    msp.add_line((0, 200), (100, 200), dxfattribs={'layer': 'FROZEN'})
    msp.add_circle((50, 200), radius=20, dxfattribs={'layer': 'FROZEN'})

    # Entity on a locked layer (should parse and render, but not select)
    doc.layers.add('LOCKED', color=3)
    doc.layers.get('LOCKED').lock()
    msp.add_line((0, 250), (100, 250), dxfattribs={'layer': 'LOCKED'})

    # Entities with explicit color overrides (not by-layer)
    msp.add_line((0, 300), (100, 300), dxfattribs={'layer': 'EDGE', 'color': 1})  # red
    msp.add_line((0, 310), (100, 310), dxfattribs={'layer': 'EDGE', 'color': 3})  # green
    msp.add_line((0, 320), (100, 320), dxfattribs={'layer': 'EDGE', 'color': 256})  # by layer

    path = os.path.join(OUTPUT_DIR, 'edge_cases.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_stress_test():
    """Large file for performance testing — 50K entities."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    import random
    random.seed(42)  # Deterministic for reproducibility

    layers = ['STRESS_A', 'STRESS_B', 'STRESS_C']
    for name in layers:
        doc.layers.add(name, color={'STRESS_A': 1, 'STRESS_B': 3, 'STRESS_C': 5}[name])

    count = 0

    # 20K lines
    for _ in range(20000):
        x0, y0 = random.uniform(-5000, 5000), random.uniform(-5000, 5000)
        dx, dy = random.uniform(-100, 100), random.uniform(-100, 100)
        layer = random.choice(layers)
        msp.add_line((x0, y0), (x0 + dx, y0 + dy), dxfattribs={'layer': layer})
        count += 1

    # 15K circles
    for _ in range(15000):
        cx, cy = random.uniform(-5000, 5000), random.uniform(-5000, 5000)
        r = random.uniform(1, 50)
        layer = random.choice(layers)
        msp.add_circle((cx, cy), radius=r, dxfattribs={'layer': layer})
        count += 1

    # 10K arcs
    for _ in range(10000):
        cx, cy = random.uniform(-5000, 5000), random.uniform(-5000, 5000)
        r = random.uniform(5, 30)
        start = random.uniform(0, 360)
        end = start + random.uniform(10, 350)
        layer = random.choice(layers)
        msp.add_arc((cx, cy), radius=r, start_angle=start, end_angle=end,
                     dxfattribs={'layer': layer})
        count += 1

    # 5K lwpolylines (4 vertices each)
    for _ in range(5000):
        x0, y0 = random.uniform(-5000, 5000), random.uniform(-5000, 5000)
        pts = []
        for j in range(4):
            angle = math.radians(j * 90 + random.uniform(-20, 20))
            dist = random.uniform(5, 30)
            pts.append((x0 + dist * math.cos(angle), y0 + dist * math.sin(angle)))
        close = random.choice([True, False])
        layer = random.choice(layers)
        msp.add_lwpolyline(pts, format='xy', close=close, dxfattribs={'layer': layer})
        count += 1

    path = os.path.join(OUTPUT_DIR, 'stress_50k.dxf')
    doc.saveas(path)
    print(f'  Created: {path} ({count} entities)')
    return path


def create_minimal():
    """Smallest possible file with one of each Phase 1 entity."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    msp.add_line((0, 0), (10, 10))
    msp.add_circle((50, 50), radius=5)
    msp.add_arc((100, 100), radius=20, start_angle=30, end_angle=150)
    msp.add_lwpolyline([(0, 200), (50, 200), (50, 250)], format='xy')

    path = os.path.join(OUTPUT_DIR, 'minimal.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print('Generating test DXF files...')
    create_minimal()
    create_basic_primitives()
    create_lwpolyline_test()
    create_insert_test()
    create_text_test()
    create_edge_cases()

    # Stress test takes a while, generate separately
    stress_path = os.path.join(OUTPUT_DIR, 'stress_50k.dxf')
    if not os.path.exists(stress_path):
        create_stress_test()
    else:
        print(f'  Skipped (exists): {stress_path}')

    print('Done.')


if __name__ == '__main__':
    main()
