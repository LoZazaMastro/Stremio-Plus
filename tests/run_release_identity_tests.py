#!/usr/bin/env python3
"""Keep 0.6.00 title updates monotonic without changing historical receipts."""
import importlib.util
import json
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('receipt', root / 'tools/build-receipt.py')
receipt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receipt)
metadata = json.loads((root / 'app/sce_sys/param.json').read_text())
checks = 0
with tempfile.TemporaryDirectory() as temporary:
    tree = Path(temporary)
    (tree / 'app/sce_sys').mkdir(parents=True)
    for version, native in [('0.4.5', '00.045.000'), ('0.5.20', '00.070.000'),
                            ('0.6.00', '00.600.000'), ('0.6.01', '00.601.000'),
                            ('0.6.10', '00.610.000'), ('0.7.00', '00.700.000')]:
        (tree / 'CMakeLists.txt').write_text(
            f'project(stremio-ps5 VERSION {version})\n'
            'STREMIO_VERSION="${PROJECT_VERSION}" STREMIO_TITLE_ID="PPSA74126"\n')
        for content, valid in [(native, True), ('00.001.000', False)]:
            (tree / 'app/sce_sys/param.json').write_text(json.dumps(dict(metadata, contentVersion=content)))
            try:
                result = receipt.release_identity(tree, tree / 'app')
            except SystemExit:
                assert not valid, (version, content)
            else:
                assert valid and result == (version, 'PPSA74126', native)
            checks += 1
assert receipt.release_identity(root, root / 'app') == ('0.6.00', 'PPSA74126', '00.600.000')
print(f'Release identity tests passed: {checks + 1} checks.')
