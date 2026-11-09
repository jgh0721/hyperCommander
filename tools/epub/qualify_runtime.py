"""Read-only EPUB runtime audit. Evidence is not product release authorization."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique_object)


def audit(root, pin, source, product=None, product_candidate=False, shared_root=False):
    root = root.resolve(strict=True)
    shared_layout = product_candidate and (root / 'Libs/epub-runtime-inventory.json').is_file()
    if shared_root and not shared_layout:
        raise ValueError('Shared product runtime requires schema 4')
    manifest_name = ('Libs/epub-runtime-inventory.json' if shared_layout else 'epub-runtime-inventory.json') if product_candidate else 'poc-runtime.json'
    bundle_root = 'Libs/epub-bundle' if shared_layout else 'bundle'
    helper_name = 'HyperEpubWeb.exe' if product_candidate else 'hc_epub_render_poc.exe'
    manifest = root / manifest_name
    if len(pin) != 64 or any(c not in '0123456789abcdef' for c in pin) or digest(manifest) != pin:
        raise ValueError('Independent manifest pin mismatch')
    data = read_json(manifest)
    expected = dict(schemaVersion=2, diagnosticOnly=True, releaseEligible=False,
                    profile='epub-passive-read-only-poc', protocolVersion=1, helper='hc_epub_render_poc.exe')
    if product_candidate:
        expected.update(schemaVersion=4 if shared_layout else 3, diagnosticOnly=False, releaseEligible=True,
                        profile='epub-passive-read-only', helper=helper_name,
                        readOnly=True, readerEngine='epubjs', readerVersion='0.3.93')
    if any(data.get(k) != v for k, v in expected.items()):
        raise ValueError('Unrecognized runtime profile; no implicit product promotion')
    rows, seen = [], set()
    files = data.get('files')
    if not isinstance(files, list) or not 1 <= len(files) <= 511:
        raise ValueError('Invalid file inventory')
    for entry in files:
        name = entry['path']
        path = PurePosixPath(name)
        if (not name or path.is_absolute() or '\\' in name or ':' in name or
                any(p in ('', '.', '..') or p.endswith((' ', '.')) for p in name.split('/')) or
                name.casefold() in seen or name.casefold() == manifest_name.casefold()):
            raise ValueError(f'Invalid or duplicate path: {name}')
        seen.add(name.casefold())
        full = (root / name).resolve(strict=True)
        if not full.is_relative_to(root) or not full.is_file() or digest(full) != entry['sha256']:
            raise ValueError(f'File integrity mismatch: {name}')
        row = dict(path=name, bytes=full.stat().st_size, sha256=entry['sha256'])
        if product is not None and (product / name).is_file():
            row['sameProductBytes'] = digest(product / name) == entry['sha256']
        rows.append(row)
    required = {helper_name.casefold(), 'qtwebengineprocess.exe', f'{bundle_root}/bundle.json',
                f'{bundle_root}/reader.js', f'{bundle_root}/third-party-notices.txt'}
    if not {name.casefold() for name in required} <= seen:
        raise ValueError('Missing helper, engine or license inventory')
    sealed = root / bundle_root if shared_root else root
    actual = {p.relative_to(root).as_posix().casefold() for p in sealed.rglob('*') if p.is_file()}
    if actual - seen - {manifest_name.casefold(), 'deployment.log'}:
        raise ValueError('Unlisted runtime files')
    if digest(root / bundle_root / 'bundle.json') != data['bundleManifestSha256']:
        raise ValueError('Bundle pin mismatch')
    bundle = read_json(root / bundle_root / 'bundle.json')
    if digest(source / 'package-lock.json') != bundle['packageLockSha256']:
        raise ValueError('Source dependency lock differs from bundle')
    for name, sha in bundle['sourceHashes'].items():
        full = (source / name).resolve(strict=True)
        if not full.is_relative_to(source.resolve()) or digest(full) != sha:
            raise ValueError(f'Bundle source drift: {name}')
    reader = bundle['files']
    if len(reader) != 1 or reader[0]['path'] != 'reader.js' or digest(root / bundle_root / 'reader.js') != reader[0]['sha256']:
        raise ValueError('Reader bundle mismatch')
    return dict(schemaVersion=1, runtime=str(root), manifestSha256=pin,
                diagnosticOnly=not product_candidate, releaseEligible=False, productProfile=product_candidate, engine='epubjs',
                dependencies=bundle['dependencies'], fileCount=len(rows),
                totalBytes=sum(r['bytes'] for r in rows),
                identicalProductBytes=sum(r['bytes'] for r in rows if r.get('sameProductBytes') is True),
                conflictingProductFiles=[r['path'] for r in rows if r.get('sameProductBytes') is False],
                deploymentDecision='shared product root; existing shared files must match independent pins' if shared_layout else 'isolated EPUB runtime',
                pendingReleaseGates=['product inventory and independent native pin',
                                    'actual F3/F4 host and PDF/HWP coexistence',
                                    'IME and accessibility', 'large-book and aggregate resource scheduler',
                                    'Watercolor propagation', 'installed and portable verification'],
                files=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--source', type=Path, default=Path(__file__).parent / 'web')
    parser.add_argument('--product', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--product-candidate', action='store_true')
    args = parser.parse_args()
    result = audit(args.runtime, args.manifest_sha256, args.source, args.product, args.product_candidate)
    with args.output.open('x', encoding='utf-8', newline='\r\n') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    print(json.dumps({k: result[k] for k in ('fileCount', 'totalBytes', 'identicalProductBytes',
                                          'conflictingProductFiles', 'releaseEligible')}))


if __name__ == '__main__':
    main()
