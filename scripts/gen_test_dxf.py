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


def create_advanced_primitives():
    """ELLIPSE, SPLINE, POINT, SOLID — entity types with zero test coverage."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add('ELLIPSES', color=4)
    doc.layers.add('SPLINES', color=5)
    doc.layers.add('POINTS', color=3)
    doc.layers.add('SOLIDS', color=6)

    # --- ELLIPSE ---
    # Full ellipses with varying ratios
    for i, ratio in enumerate([1.0, 0.5, 0.1]):
        cx = i * 100
        msp.add_ellipse(
            (cx, 0, 0), major_axis=(20, 0, 0), ratio=ratio,
            dxfattribs={'layer': 'ELLIPSES'}
        )

    # Partial ellipses with start/end parametric angles (radians)
    msp.add_ellipse(
        (300, 0, 0), major_axis=(20, 0, 0), ratio=0.6,
        start_param=math.radians(30), end_param=math.radians(150),
        dxfattribs={'layer': 'ELLIPSES'}
    )
    msp.add_ellipse(
        (400, 0, 0), major_axis=(20, 0, 0), ratio=0.7,
        start_param=math.radians(0), end_param=math.radians(350),
        dxfattribs={'layer': 'ELLIPSES'}
    )

    # Rotated ellipse (45 degrees)
    angle = math.radians(45)
    msp.add_ellipse(
        (500, 0, 0), major_axis=(20 * math.cos(angle), 20 * math.sin(angle), 0),
        ratio=0.5,
        dxfattribs={'layer': 'ELLIPSES'}
    )

    # Degenerate: near-zero major axis (should be handled gracefully)
    msp.add_ellipse(
        (600, 0, 0), major_axis=(0.001, 0, 0), ratio=1.0,
        dxfattribs={'layer': 'ELLIPSES'}
    )

    # --- SPLINE ---
    # Control-points-only spline (degree 3)
    msp.add_spline(
        [(0, 200), (20, 260), (60, 280), (100, 240), (140, 220)],
        degree=3,
        dxfattribs={'layer': 'SPLINES'}
    )

    # Fit-points spline (triggers Catmull-Rom)
    spline_fit = msp.add_spline(
        dxfattribs={'layer': 'SPLINES'}
    )
    spline_fit.degree = 3
    for pt in [(200, 200, 0), (230, 260, 0), (270, 230, 0), (300, 270, 0)]:
        spline_fit.fit_points.append(pt)

    # Closed spline
    spline_closed = msp.add_spline(dxfattribs={'layer': 'SPLINES'})
    spline_closed.closed = True
    spline_closed.degree = 3
    for pt in [(350, 200, 0), (380, 250, 0), (350, 280, 0), (320, 250, 0)]:
        spline_closed.control_points.append(pt)
    spline_closed.knots = [0, 0, 0, 1, 1, 1]

    # Degenerate: 2-point spline
    msp.add_spline(
        [(420, 200), (420, 260)],
        degree=1,
        dxfattribs={'layer': 'SPLINES'}
    )

    # --- POINT ---
    for x, y in [(0, 350), (50, 370), (100, 350)]:
        msp.add_point((x, y, 0), dxfattribs={'layer': 'POINTS'})

    # --- SOLID ---
    # Triangle (3 corners)
    msp.add_solid(
        [(0, 420), (40, 420), (20, 460)],
        dxfattribs={'layer': 'SOLIDS'}
    )

    # Quad (4 corners) — DXF order: 1,2,4,3 → triangles v0-v1-v3 and v1-v2-v3
    msp.add_solid(
        [(60, 420), (100, 420), (100, 460), (60, 460)],
        dxfattribs={'layer': 'SOLIDS'}
    )

    # Degenerate: collinear corners
    msp.add_solid(
        [(120, 440), (140, 440), (160, 440), (130, 440)],
        dxfattribs={'layer': 'SOLIDS'}
    )

    path = os.path.join(OUTPUT_DIR, 'advanced_primitives.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_hatch_dimensions():
    """HATCH and DIMENSION entities — complex boundary and annotation types."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add('HATCHES', color=1)
    doc.layers.add('DIMS', color=3)

    # --- HATCH ---
    # Solid fill with rectangular polyline boundary
    hatch1 = msp.add_hatch(
        color=1, dxfattribs={'layer': 'HATCHES'}
    )
    hatch1.set_pattern_fill('SOLID', color=1)
    hatch1.paths.add_polyline_path(
        [(0, 0), (100, 0), (100, 50), (0, 50)], is_closed=True
    )

    # Solid fill with 6-sided polygon boundary
    hatch2 = msp.add_hatch(
        color=3, dxfattribs={'layer': 'HATCHES'}
    )
    hatch2.set_pattern_fill('SOLID', color=3)
    pts = []
    for i in range(6):
        angle = math.radians(i * 60)
        pts.append((150 + 30 * math.cos(angle), 25 + 30 * math.sin(angle)))
    hatch2.paths.add_polyline_path(pts, is_closed=True)

    # HATCH with edge-defined boundary (line edges + arc edge)
    hatch3 = msp.add_hatch(
        color=5, dxfattribs={'layer': 'HATCHES'}
    )
    hatch3.set_pattern_fill('SOLID', color=5)
    edge_path = hatch3.paths.add_edge_path()
    edge_path.add_line((250, 0), (350, 0))
    edge_path.add_line((350, 0), (350, 40))
    edge_path.add_arc(
        center=(300, 40), radius=50,
        start_angle=0, end_angle=180,
        ccw=True
    )
    edge_path.add_line((250, 40), (250, 0))

    # HATCH with multiple boundary loops (donut: outer circle + inner circle)
    hatch4 = msp.add_hatch(
        color=4, dxfattribs={'layer': 'HATCHES'}
    )
    hatch4.set_pattern_fill('SOLID', color=4)
    # Outer circle approximated by polyline
    outer_pts = []
    for i in range(36):
        angle = math.radians(i * 10)
        outer_pts.append((500 + 40 * math.cos(angle), 25 + 40 * math.sin(angle)))
    hatch4.paths.add_polyline_path(outer_pts, is_closed=True)
    # Inner circle (hole)
    inner_pts = []
    for i in range(36):
        angle = math.radians(i * 10)
        inner_pts.append((500 + 20 * math.cos(angle), 25 + 20 * math.sin(angle)))
    hatch4.paths.add_polyline_path(inner_pts, is_closed=True)

    # Empty HATCH (0 boundary loops — should render nothing)
    msp.add_hatch(
        color=6, dxfattribs={'layer': 'HATCHES'}
    )

    # --- DIMENSION ---
    # Linear dimensions
    msp.add_linear_dim(
        base=(0, -20), p1=(0, 0), p2=(100, 0),
        dxfattribs={'layer': 'DIMS', 'color': 3}
    )
    msp.add_linear_dim(
        base=(0, -40), p1=(0, 0), p2=(0, 50),
        angle=90,
        dxfattribs={'layer': 'DIMS', 'color': 3}
    )
    msp.add_linear_dim(
        base=(0, -60), p1=(0, 0), p2=(70, 70),
        dxfattribs={'layer': 'DIMS', 'color': 3}
    )

    # Aligned dimension
    msp.add_aligned_dim(
        p1=(250, 0), p2=(350, 0), distance=-20,
        dxfattribs={'layer': 'DIMS', 'color': 5}
    )

    # Dimension with explicit text override
    dim_text = msp.add_linear_dim(
        base=(400, -20), p1=(400, 0), p2=(480, 0),
        dxfattribs={'layer': 'DIMS', 'color': 3, 'text': 'Custom'}
    )

    path = os.path.join(OUTPUT_DIR, 'hatch_dimensions.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_insert_advanced():
    """MINSERT arrays, negative scale, 3-level nesting, circular reference."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add('BLOCKS', color=7)

    # Basic block: a cross shape
    cross = doc.blocks.new('CROSS')
    cross.add_line((-5, 0), (5, 0))
    cross.add_line((0, -5), (0, 5))
    cross.add_circle((0, 0), radius=3)

    # --- MINSERT 4x3 array ---
    msp.add_blockref('CROSS', insert=(0, 0), dxfattribs={
        'layer': 'BLOCKS',
        'column_count': 4, 'row_count': 3,
        'column_spacing': 20, 'row_spacing': 15,
    })

    # MINSERT with rotation (30 degrees)
    msp.add_blockref('CROSS', insert=(120, 0), dxfattribs={
        'layer': 'BLOCKS',
        'column_count': 3, 'row_count': 2,
        'column_spacing': 20, 'row_spacing': 15,
        'rotation': 30,
    })

    # MINSERT with non-uniform scale
    msp.add_blockref('CROSS', insert=(250, 0), dxfattribs={
        'layer': 'BLOCKS',
        'xscale': 2.0, 'yscale': 0.5,
        'column_count': 2, 'row_count': 2,
        'column_spacing': 30, 'row_spacing': 20,
    })

    # --- Negative scale (mirror) ---
    msp.add_blockref('CROSS', insert=(350, 50), dxfattribs={
        'layer': 'BLOCKS',
        'xscale': -1.0,
    })

    # --- 3-level nested blocks ---
    inner = doc.blocks.new('INNER_DOT')
    inner.add_circle((0, 0), radius=2)

    mid = doc.blocks.new('MID_RING')
    mid.add_blockref('INNER_DOT', insert=(0, 0))
    mid.add_circle((0, 0), radius=5)

    outer = doc.blocks.new('OUTER_GROUP')
    outer.add_blockref('MID_RING', insert=(0, 0))
    outer.add_blockref('MID_RING', insert=(15, 0))

    msp.add_blockref('OUTER_GROUP', insert=(0, 120))

    # --- Circular reference (tests depth limit) ---
    cycle_a = doc.blocks.new('CYCLE_A')
    cycle_b = doc.blocks.new('CYCLE_B')
    cycle_a.add_line((0, 0), (10, 10))
    cycle_a.add_blockref('CYCLE_B', insert=(0, 0))
    cycle_b.add_line((0, 0), (10, -10))
    cycle_b.add_blockref('CYCLE_A', insert=(0, 0))

    msp.add_blockref('CYCLE_A', insert=(150, 120))

    path = os.path.join(OUTPUT_DIR, 'insert_advanced.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_text_mtext_rich():
    """TEXT/MTEXT with formatting, special characters, and edge cases."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add('RICH_TEXT', color=7)

    # --- TEXT variations ---
    # Basic text with rotation
    msp.add_text('Rotated 30deg', dxfattribs={
        'layer': 'RICH_TEXT', 'height': 5.0,
        'insert': (0, 0), 'rotation': 30
    })

    # Text with width factor
    msp.add_text('Wide Text', dxfattribs={
        'layer': 'RICH_TEXT', 'height': 5.0,
        'insert': (0, 30), 'width': 2.0
    })

    # Text with various alignments
    msp.add_text('Center Aligned', dxfattribs={
        'layer': 'RICH_TEXT', 'height': 5.0,
        'insert': (0, 60)
    }).set_placement((200, 60), align=TextEntityAlignment.MIDDLE_CENTER)

    # Empty text (degenerate)
    msp.add_text('', dxfattribs={
        'layer': 'RICH_TEXT', 'height': 5.0, 'insert': (0, 90)
    })

    # Zero height text (degenerate)
    msp.add_text('Zero Height', dxfattribs={
        'layer': 'RICH_TEXT', 'height': 0.0, 'insert': (0, 110)
    })

    # --- MTEXT formatting ---
    # Paragraph breaks
    msp.add_mtext('Line 1\\PLine 2\\PLine 3', dxfattribs={
        'layer': 'RICH_TEXT', 'char_height': 4.0,
        'insert': (0, 150)
    })

    # Unicode characters
    msp.add_mtext('Angle: \\U+2220\\PDiameter: \\U+2300\\PDegree: \\U+00B0', dxfattribs={
        'layer': 'RICH_TEXT', 'char_height': 4.0,
        'insert': (0, 200)
    })

    # Inline formatting (height and color overrides)
    msp.add_mtext('{\\H2x;Big} {\\H0.5x;Small} {\\C1;Red} {\\C3;Green}', dxfattribs={
        'layer': 'RICH_TEXT', 'char_height': 4.0,
        'insert': (0, 250)
    })

    # Very long single line
    msp.add_mtext('A' * 200, dxfattribs={
        'layer': 'RICH_TEXT', 'char_height': 3.0,
        'insert': (0, 300), 'rect_width': 200
    })

    path = os.path.join(OUTPUT_DIR, 'text_mtext_rich.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_edge_cases_v2():
    """Extended edge cases: angle wrapping, degenerate curves, layer states."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    doc.layers.add('EDGE_V2', color=7)

    # ARC with start > end (wrapping past 360)
    msp.add_arc((0, 0), radius=20, start_angle=300, end_angle=60,
                 dxfattribs={'layer': 'EDGE_V2'})

    # ARC with negative angles
    msp.add_arc((80, 0), radius=15, start_angle=-45, end_angle=45,
                 dxfattribs={'layer': 'EDGE_V2'})

    # LWPOLYLINE with all bulge arcs (semicircles between vertices)
    msp.add_lwpolyline(
        [(0, 100, 1.0), (30, 100, 1.0), (60, 100, 1.0), (90, 100, 1.0)],
        format='xyb', close=True,
        dxfattribs={'layer': 'EDGE_V2'}
    )

    # Closed LWPOLYLINE where last segment has bulge
    msp.add_lwpolyline(
        [(0, 150, 0.0), (40, 150, 0.0), (40, 180, 0.5), (0, 180, -0.5)],
        format='xyb', close=True,
        dxfattribs={'layer': 'EDGE_V2'}
    )

    # ELLIPSE with tiny parametric span
    msp.add_ellipse(
        (200, 0, 0), major_axis=(20, 0, 0), ratio=0.5,
        start_param=math.radians(44.9), end_param=math.radians(45.1),
        dxfattribs={'layer': 'EDGE_V2'}
    )

    # SPLINE degree 2 (quadratic)
    msp.add_spline(
        [(0, 250), (25, 300), (50, 270), (75, 310), (100, 260)],
        degree=2,
        dxfattribs={'layer': 'EDGE_V2'}
    )

    # Entities on OFF layer (visible=False)
    doc.layers.add('OFF_LAYER', color=1)
    doc.layers.get('OFF_LAYER').off()
    msp.add_line((0, 350), (100, 350), dxfattribs={'layer': 'OFF_LAYER'})
    msp.add_circle((50, 380), radius=10, dxfattribs={'layer': 'OFF_LAYER'})

    # Entity with color 0 (ByBlock) outside a block
    msp.add_line((0, 400), (100, 400), dxfattribs={'layer': 'EDGE_V2', 'color': 0})

    # Entity with lineweight
    msp.add_line((0, 420), (100, 420), dxfattribs={
        'layer': 'EDGE_V2', 'lineweight': 50  # 0.5mm
    })

    # Full circle as arc 0-360 (duplicate test but in v2 context)
    msp.add_arc((250, 0), radius=10, start_angle=0, end_angle=360,
                 dxfattribs={'layer': 'EDGE_V2'})

    # Circle with extremely large radius
    msp.add_circle((0, 0), radius=1e6, dxfattribs={'layer': 'EDGE_V2'})

    path = os.path.join(OUTPUT_DIR, 'edge_cases_v2.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_simple_r2000():
    """R2000 format DXF with LINE/CIRCLE/ARC/TEXT/INSERT/BLOCK — version coverage."""
    doc = ezdxf.new('R2000')
    msp = doc.modelspace()

    msp.add_line((0, 0), (100, 50))
    msp.add_circle((50, 50), radius=25)
    msp.add_arc((150, 50), radius=30, start_angle=0, end_angle=180)

    text = msp.add_text('Hello R2000', height=5)
    text.set_placement((10, 80))

    block = doc.blocks.new('TEST_BLOCK')
    block.add_line((0, 0), (20, 20))
    block.add_circle((10, 10), radius=5)
    msp.add_blockref('TEST_BLOCK', insert=(200, 50))

    path = os.path.join(OUTPUT_DIR, 'simple_r2000.dxf')
    doc.saveas(path)
    print(f'  Created: {path}')
    return path


def create_stress_mixed():
    """50K entities including all types for stress testing."""
    doc = ezdxf.new('R2010')
    msp = doc.modelspace()

    import random
    random.seed(43)

    layers = ['STRESS_GEOM', 'STRESS_CURVE', 'STRESS_ANNOT']
    for name, color in zip(layers, [7, 4, 2]):
        doc.layers.add(name, color=color)

    count = 0

    def rand_layer():
        return random.choice(layers)

    def rand_pt(scale=5000):
        return random.uniform(-scale, scale), random.uniform(-scale, scale)

    # 15K lines
    for _ in range(15000):
        x0, y0 = rand_pt()
        dx, dy = random.uniform(-100, 100), random.uniform(-100, 100)
        msp.add_line((x0, y0), (x0 + dx, y0 + dy), dxfattribs={'layer': rand_layer()})
        count += 1

    # 10K circles
    for _ in range(10000):
        cx, cy = rand_pt()
        r = random.uniform(1, 50)
        msp.add_circle((cx, cy), radius=r, dxfattribs={'layer': rand_layer()})
        count += 1

    # 5K arcs
    for _ in range(5000):
        cx, cy = rand_pt()
        r = random.uniform(5, 30)
        start = random.uniform(0, 360)
        end = start + random.uniform(10, 350)
        msp.add_arc((cx, cy), radius=r, start_angle=start, end_angle=end,
                     dxfattribs={'layer': rand_layer()})
        count += 1

    # 5K lwpolylines
    for _ in range(5000):
        x0, y0 = rand_pt()
        pts = []
        for j in range(4):
            angle = math.radians(j * 90 + random.uniform(-20, 20))
            dist = random.uniform(5, 30)
            pts.append((x0 + dist * math.cos(angle), y0 + dist * math.sin(angle)))
        msp.add_lwpolyline(pts, format='xy', close=random.choice([True, False]),
                           dxfattribs={'layer': rand_layer()})
        count += 1

    # 5K ellipses
    for _ in range(5000):
        cx, cy = rand_pt()
        major = random.uniform(5, 30)
        ratio = random.uniform(0.1, 1.0)
        rot = random.uniform(0, 180)
        angle_rad = math.radians(rot)
        msp.add_ellipse(
            (cx, cy, 0),
            major_axis=(major * math.cos(angle_rad), major * math.sin(angle_rad), 0),
            ratio=ratio,
            dxfattribs={'layer': rand_layer()}
        )
        count += 1

    # 3K splines
    for _ in range(3000):
        x0, y0 = rand_pt()
        pts = []
        for j in range(random.randint(3, 6)):
            pts.append((x0 + random.uniform(-30, 30), y0 + random.uniform(-30, 30)))
        msp.add_spline(pts, degree=3, dxfattribs={'layer': rand_layer()})
        count += 1

    # 3K points
    for _ in range(3000):
        x, y = rand_pt()
        msp.add_point((x, y, 0), dxfattribs={'layer': rand_layer()})
        count += 1

    # 2K solids
    for _ in range(2000):
        x0, y0 = rand_pt()
        s = random.uniform(5, 20)
        msp.add_solid(
            [(x0, y0), (x0 + s, y0), (x0 + s / 2, y0 + s)],
            dxfattribs={'layer': rand_layer()}
        )
        count += 1

    # 1K hatches (solid fill, rectangular boundary)
    for _ in range(1000):
        x0, y0 = rand_pt(2000)
        s = random.uniform(5, 20)
        hatch = msp.add_hatch(dxfattribs={'layer': rand_layer()})
        hatch.set_pattern_fill('SOLID', color=7)
        hatch.paths.add_polyline_path(
            [(x0, y0), (x0 + s, y0), (x0 + s, y0 + s), (x0, y0 + s)],
            is_closed=True
        )
        count += 1

    # 1K dimensions
    for _ in range(1000):
        x0, y0 = rand_pt(2000)
        length = random.uniform(10, 50)
        try:
            msp.add_linear_dim(
                base=(x0, y0 - 10), p1=(x0, y0), p2=(x0 + length, y0),
                dxfattribs={'layer': rand_layer()}
            )
        except Exception:
            pass
        count += 1

    path = os.path.join(OUTPUT_DIR, 'stress_mixed.dxf')
    doc.saveas(path)
    print(f'  Created: {path} ({count} entities)')
    return path


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print('Generating test DXF files...')

    # Phase 1: original tests
    create_minimal()
    create_basic_primitives()
    create_lwpolyline_test()
    create_insert_test()
    create_text_test()
    create_edge_cases()

    # Version coverage
    create_simple_r2000()

    # Phase 2: new entity type coverage
    create_advanced_primitives()
    create_hatch_dimensions()
    create_insert_advanced()
    create_text_mtext_rich()
    create_edge_cases_v2()

    # Stress tests (large files, skip if exists)
    for name, gen_fn in [
        ('stress_50k.dxf', create_stress_test),
        ('stress_mixed.dxf', create_stress_mixed),
    ]:
        stress_path = os.path.join(OUTPUT_DIR, name)
        if not os.path.exists(stress_path):
            gen_fn()
        else:
            print(f'  Skipped (exists): {stress_path}')

    print('Done.')


if __name__ == '__main__':
    main()
