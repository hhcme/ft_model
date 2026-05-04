#!/usr/bin/env python3
"""Quick batch comparison for files that already have both HOOPS and FT exports."""
import subprocess, json, os, glob

PYTHON_EXE = 'C:/Users/24159/AppData/Local/Python/pythoncore-3.14-64/python.exe'
COMPARE_SCRIPT = 'D:/findtop/code/ft_model/scripts/compare_structural.py'
TEMP_DIR = 'C:/Users/24159/AppData/Local/Temp'

def run_comparison(hoops_path, ft_path):
    report_json = os.path.join(TEMP_DIR, 'quick_report.json')
    cmd = [PYTHON_EXE, COMPARE_SCRIPT, hoops_path, ft_path, '--json', report_json]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if os.path.exists(report_json):
        try:
            with open(report_json, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception:
            pass
    return None

# Find all files with both exports
pairs = []
for ft_path in glob.glob(os.path.join(TEMP_DIR, 'ft_*_compare.json')):
    basename = os.path.basename(ft_path)
    # Extract name between ft_ and _compare.json
    name = basename[3:-13]
    # Find matching hoops file
    hoops_path = os.path.join(TEMP_DIR, f'hoops_{name}.json')
    if not os.path.exists(hoops_path):
        # Try with .dwg suffix if not found
        hoops_path2 = os.path.join(TEMP_DIR, f'hoops_{name}')
        if os.path.exists(hoops_path2):
            hoops_path = hoops_path2
        else:
            continue
    pairs.append((name, hoops_path, ft_path))

pairs.sort()
print(f'Found {len(pairs)} file pairs to compare')
print('=' * 100)
print(f'  {"File":<45} {"Status":<8} {"Entity%":>8} {"Geo%":>8} {"Layers":>6} {"Space":>6} {"Note":<20}')
print(f'  {"-"*45} {"-"*8} {"-"*8} {"-"*8} {"-"*6} {"-"*6} {"-"*20}')

results = []
for name, h_path, f_path in pairs:
    report = run_comparison(h_path, f_path)
    if report:
        overall = report.get('overall', 'UNKNOWN')
        entity_ratio = report.get('entity_counts', {}).get('ratio_pct')
        geo_match = report.get('geometric', {}).get('match_pct')
        layer_match = report.get('layers', {}).get('match', False)
        space_ok = report.get('space_split', {}).get('split_ok', False)
        note = report.get('notes', [''])[0] if report.get('notes') else ''
        er = f"{entity_ratio:.1f}" if entity_ratio is not None else 'N/A'
        gm = f"{geo_match:.1f}" if geo_match is not None else 'N/A'
        lm = 'OK' if layer_match else 'FAIL'
        sp = 'OK' if space_ok else 'FAIL'
        print(f'  {name:<45} {overall:<8} {er:>8} {gm:>8} {lm:>6} {sp:>6} {note:<20}')
        results.append((name, overall, entity_ratio, geo_match))
    else:
        print(f'  {name:<45} ERROR')
        results.append((name, 'ERROR', None, None))

print('=' * 100)
pass_warn = sum(1 for _, s, _, _ in results if s in ('PASS', 'WARN'))
fail = sum(1 for _, s, _, _ in results if s == 'FAIL')
error = sum(1 for _, s, _, _ in results if s == 'ERROR')
total = len(results)
print(f'Total: {total}, PASS/WARN: {pass_warn}, FAIL: {fail}, ERROR: {error}')
if total > 0:
    print(f'Pass rate: {pass_warn/total*100:.1f}%')
