#!/usr/bin/env python3
"""Apply the existing GS control transforms to Magatama's source-owned runtime.

Unlike the retired RR5 diagnostic builder, inputs already contain product host
and DMA fixes. Exact source anchors still fail closed on incompatible inputs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import gs_control_memory_overlay as memory
import gs_control_gif_overlay as gif
import gs_control_runtime_overlay as runtime


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--runtime-source', type=Path, required=True)
    p.add_argument('--include-input', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    for key in ('runtime', 'memory', 'vif1', 'gif', 'interrupt', 'system', 'stubs_gs'):
        p.add_argument(f'--{key}-input', type=Path, required=True)
    args = p.parse_args()
    root = args.runtime_source.resolve()
    output = args.output_dir.resolve()
    if output == root or root in output.parents:
        raise RuntimeError('GS overlay must be outside source runtime')
    transforms = {
        'runtime': runtime.runtime_cpp,
        'memory': lambda s: runtime.memory_irq_cpp(memory.memory_cpp(s)),
        'vif1': lambda s: runtime.vif_irq_cpp(memory.vif1_cpp(s)),
        'gif': gif.gif_cpp,
        'interrupt': runtime.interrupt_cpp,
        'system': runtime.system_cpp,
        'stubs_gs': lambda s: s,
    }
    files = {}
    for original in args.include_input.rglob('*'):
        if original.is_file():
            rel = Path('include') / original.relative_to(args.include_input)
            s = original.read_text()
            if str(rel) == 'include/runtime/ps2_memory.h':
                s = runtime.memory_irq_header(memory.memory_header(s))
            elif str(rel) == 'include/runtime/ps2_gif_arbiter.h':
                s = gif.gif_header(s)
            files[rel] = s
    for key, transform in transforms.items():
        original = getattr(args, key + '_input').resolve()
        rel = original.relative_to(root)
        s = transform(original.read_text())
        if key == 'vif1':
            s = s.replace('m_gsDirectImageExtra = imageQw > m_gsDirectDeclaredQw - 1u ?',
                'm_gsDirectImageExtra = !g_magatamaStrictVifDirect && imageQw > m_gsDirectDeclaredQw - 1u ?')
        files[rel] = s
    rel = Path('src/lib/Kernel/Syscalls/Interrupt.h')
    files[rel] = runtime.interrupt_header((root / rel).read_text())
    # Stage only after all source anchors were validated.
    receipt = {'schema_version': 1, 'overlay': 'magatama-producer-gs-control', 'files': []}
    for rel, text in files.items():
        dest = output / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.read_text() != text:
            dest.write_text(text)
        receipt['files'].append({'path': str(rel), 'sha256': hashlib.sha256(text.encode()).hexdigest()})
    manifest = output / 'gs-control-overlay-manifest.json'
    encoded = json.dumps(receipt, indent=2) + '\n'
    if not manifest.exists() or manifest.read_text() != encoded:
        manifest.write_text(encoded)

if __name__ == '__main__':
    main()
