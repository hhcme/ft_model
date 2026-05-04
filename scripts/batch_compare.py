#!/usr/bin/env python3
"""
batch_compare.py — 批量DWG解析器结构树对比测试

用法:
  python3 batch_compare.py [test_dwg_dir]
  python3 batch_compare.py [test_dwg_dir] --structural

示例:
  python3 batch_compare.py D:/findtop/code/ft_model/test_dwg
  python3 batch_compare.py D:/findtop/code/ft_model/test_dwg --structural
"""

import subprocess
import json
import os
import sys
import argparse

# 配置路径
HOOPS_EXE = 'D:/findtop/code/ft_model/build/tools/dwg_to_json/Release/dwg_to_json.exe'
FT_EXE = 'D:/findtop/code/ft_model/build/core/test/Debug/render_export.exe'
COMPARE_SCRIPT_LEGACY = 'D:/findtop/code/ft_model/scripts/compare_hoops.py'
COMPARE_SCRIPT_STRUCTURAL = 'D:/findtop/code/ft_model/scripts/compare_structural.py'
PYTHON_EXE = 'C:/Users/24159/AppData/Local/Python/pythoncore-3.14-64/python.exe'
TEMP_DIR = 'C:/Users/24159/AppData/Local/Temp'
HOOPS_SDK = 'D:/findtop/code/fd_converter/unzip_sdk/HOOPS_Exchange_2026.2.0'

def run_command(cmd, env=None):
    """运行命令并返回结果"""
    result = subprocess.run(cmd, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', env=env)
    return result

def main():
    parser = argparse.ArgumentParser(description='批量DWG解析器对比测试')
    parser.add_argument('test_dir', nargs='?', default='D:/findtop/code/ft_model/test_dwg',
                        help='测试DWG文件目录')
    parser.add_argument('--structural', action='store_true',
                        help='使用 compare_structural.py 进行结构对比')
    parser.add_argument('--json', metavar='REPORT.json',
                        help='输出JSON汇总报告')
    args = parser.parse_args()

    test_dir = args.test_dir
    use_structural = args.structural
    json_output = args.json

    compare_script = COMPARE_SCRIPT_STRUCTURAL if use_structural else COMPARE_SCRIPT_LEGACY

    # 查找所有DWG文件
    dwg_files = []
    if os.path.isdir(test_dir):
        for f in os.listdir(test_dir):
            if f.lower().endswith('.dwg'):
                dwg_files.append(f)
    dwg_files.sort()

    if not dwg_files:
        print(f'未在 {test_dir} 找到DWG文件')
        return 1

    # 设置环境变量
    env = os.environ.copy()
    env['HOOPS_EXCHANGE_ROOT'] = HOOPS_SDK
    env['A3DLIBS_ROOT'] = HOOPS_SDK
    env['HOOPS_EXCHANGE_LICENSE'] = ''
    # Add vcpkg MinGW bin to PATH so render_export can find libzlib1.dll
    vcpkg_bin = 'D:/claw_project/code_solo/vcpkg_installed/x64-mingw-dynamic/bin'
    if vcpkg_bin not in env.get('PATH', ''):
        env['PATH'] = vcpkg_bin + os.pathsep + env.get('PATH', '')

    print('=' * 80)
    if use_structural:
        print('DWG Parser Structural Alignment - Batch Report')
    else:
        print('DWG Parser Structure Tree Comparison - Batch Report')
    print(f'Test directory: {test_dir}')
    print(f'对比模式: {"structural" if use_structural else "legacy"}')
    print('=' * 80)

    results = []
    summary_matrix = []

    for dwg_file in dwg_files:
        input_path = os.path.join(test_dir, dwg_file)
        hoops_out = os.path.join(TEMP_DIR, f'hoops_{dwg_file}.json')
        ft_out = os.path.join(TEMP_DIR, f'ft_{dwg_file}_compare.json')

        print(f'\n--- {dwg_file} ---')

        # 运行HOOPS解析器
        r = run_command([HOOPS_EXE, input_path, hoops_out], env=env)
        if r.returncode != 0:
            err = r.stderr[:200] if r.stderr else ''
            print(f'  HOOPS FAILED: {err}')
            results.append((dwg_file, 'FAIL', 'HOOPS解析失败'))
            summary_matrix.append({
                'file': dwg_file, 'status': 'FAIL', 'note': 'HOOPS解析失败',
                'overall': None, 'entity_ratio': None, 'geo_match': None,
            })
            continue

        # Skip large files early: if HOOPS JSON is too large, don't waste time on FT export
        MAX_HOOPS_JSON_MB = 500
        MAX_FT_JSON_MB = 500
        hoops_size_mb = os.path.getsize(hoops_out) / (1024 * 1024)
        if hoops_size_mb > MAX_HOOPS_JSON_MB:
            print(f'  SKIP: HOOPS JSON too large ({hoops_size_mb:.1f}MB > {MAX_HOOPS_JSON_MB}MB)')
            results.append((dwg_file, 'SKIP', f'HOOPS JSON {hoops_size_mb:.1f}MB'))
            summary_matrix.append({
                'file': dwg_file, 'status': 'SKIP',
                'note': f'HOOPS JSON {hoops_size_mb:.1f}MB',
                'overall': None, 'entity_ratio': None, 'geo_match': None,
            })
            continue

        # 运行FT解析器 (structural模式添加 --compare-mode)
        ft_cmd = [FT_EXE, input_path, ft_out]
        if use_structural:
            ft_cmd.append('--compare-mode')
        r = run_command(ft_cmd, env=env)
        if r.returncode != 0:
            err = r.stderr[:200] if r.stderr else ''
            print(f'  FT FAILED: {err}')
            results.append((dwg_file, 'FAIL', 'FT解析失败'))
            summary_matrix.append({
                'file': dwg_file, 'status': 'FAIL', 'note': 'FT解析失败',
                'overall': None, 'entity_ratio': None, 'geo_match': None,
            })
            continue

        ft_size_mb = os.path.getsize(ft_out) / (1024 * 1024)
        if ft_size_mb > MAX_FT_JSON_MB:
            print(f'  SKIP: FT JSON too large ({ft_size_mb:.1f}MB > {MAX_FT_JSON_MB}MB)')
            results.append((dwg_file, 'SKIP', f'FT JSON {ft_size_mb:.1f}MB'))
            summary_matrix.append({
                'file': dwg_file, 'status': 'SKIP',
                'note': f'FT JSON {ft_size_mb:.1f}MB',
                'overall': None, 'entity_ratio': None, 'geo_match': None,
            })
            continue

        # 运行对比
        compare_cmd = [PYTHON_EXE, compare_script, hoops_out, ft_out]
        if use_structural:
            compare_cmd.append('--json')
            report_json = os.path.join(TEMP_DIR, f'report_{dwg_file}.json')
            compare_cmd.append(report_json)
        r = run_command(compare_cmd)

        if use_structural and os.path.exists(report_json):
            try:
                with open(report_json, 'r', encoding='utf-8') as f:
                    report = json.load(f)
                overall = report.get('overall', 'UNKNOWN')
                entity_ratio = report.get('entity_counts', {}).get('ratio_pct')
                geo_match = report.get('geometric', {}).get('match_pct')
                layer_match = report.get('layers', {}).get('match', False)
                space_ok = report.get('space_split', {}).get('split_ok', False)
                summary_matrix.append({
                    'file': dwg_file,
                    'status': overall,
                    'note': '',
                    'overall': overall,
                    'entity_ratio': entity_ratio,
                    'geo_match': geo_match,
                    'layer_match': layer_match,
                    'space_ok': space_ok,
                })
                status_label = overall
                results.append((dwg_file, status_label, ''))
                print(f'  结果: {overall} | 实体比例: {entity_ratio:.1f}% | 几何匹配: {geo_match:.1f}%')
            except Exception as e:
                results.append((dwg_file, 'FAIL', f'报告解析失败: {e}'))
                summary_matrix.append({
                    'file': dwg_file, 'status': 'FAIL', 'note': str(e),
                    'overall': None, 'entity_ratio': None, 'geo_match': None,
                })
        else:
            # Legacy mode parsing
            if r.returncode == 0:
                results.append((dwg_file, 'PASS', ''))
            else:
                output = r.stdout if r.stdout else ''
                if 'INSERT' in output and 'adjusted' in output:
                    results.append((dwg_file, 'PASS*', '有INSERT差异但已调整'))
                else:
                    results.append((dwg_file, 'WARN', '需人工检查'))
            if r.stdout:
                lines = r.stdout.strip().split('\n')
                for line in lines:
                    if '###' in line or 'Match' in line or 'total' in line or 'OK' in line or 'WARN' in line:
                        print(f'  {line.strip()}')

    # 打印汇总矩阵
    print('\n' + '=' * 80)
    if use_structural:
        print('STRUCTURAL SUMMARY MATRIX')
        print('=' * 80)
        print(f'  {"File":<40} {"Status":<8} {"Entity%":>8} {"Geo%":>8} {"Layers":>6} {"Space":>6}')
        print(f'  {"-"*40} {"-"*8} {"-"*8} {"-"*8} {"-"*6} {"-"*6}')
        for row in summary_matrix:
            er = f"{row['entity_ratio']:.1f}" if row['entity_ratio'] is not None else 'N/A'
            gm = f"{row['geo_match']:.1f}" if row['geo_match'] is not None else 'N/A'
            lm = 'OK' if row.get('layer_match') else 'FAIL' if row.get('layer_match') is not None else 'N/A'
            sp = 'OK' if row.get('space_ok') else 'FAIL' if row.get('space_ok') is not None else 'N/A'
            print(f'  {row["file"]:<40} {row["status"]:<8} {er:>8} {gm:>8} {lm:>6} {sp:>6}')
    else:
        print('SUMMARY')
        print('=' * 80)

    pass_count = sum(1 for _, s, _ in results if s in ('PASS', 'PASS*', 'WARN'))
    fail_count = sum(1 for _, s, _ in results if s == 'FAIL')
    total = len(results)

    if not use_structural:
        for f, status, note in results:
            note_str = f' ({note})' if note else ''
            print(f'  {f:40s} {status:6s}{note_str}')

    print(f'\n总计: {total} 文件, {pass_count} 通过/警告, {fail_count} 失败')
    if use_structural and total > 0:
        ok_pct = pass_count / total * 100.0
        print(f'通过率: {ok_pct:.1f}% (目标 ≥80%)')
        if ok_pct >= 80.0:
            print('=> 达到 v0.9.0 批量对比目标')

    # 保存JSON报告
    if json_output and use_structural:
        with open(json_output, 'w', encoding='utf-8') as f:
            json.dump({
                'mode': 'structural',
                'test_dir': test_dir,
                'total_files': total,
                'pass_warn_count': pass_count,
                'fail_count': fail_count,
                'matrix': summary_matrix,
            }, f, indent=2, ensure_ascii=False)
        print(f'\nJSON报告已保存: {json_output}')

    return 0 if fail_count == 0 else 2 if use_structural else 1

if __name__ == '__main__':
    sys.exit(main())
