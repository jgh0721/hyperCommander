"""Stage an independently pinned EPUB runtime into the shared application root."""
import argparse
import json
from pathlib import Path
import shutil
from qualify_runtime import audit, digest


def deploy(runtime, pin, source, application):
    runtime = runtime.resolve(strict=True)
    destination = application.resolve()
    if destination == runtime or runtime in destination.parents or destination in runtime.parents:
        raise ValueError('Runtime and destination must be separate')
    report = audit(runtime, pin, source, product_candidate=True, shared_root=False)
    manifest = 'Libs/epub-runtime-inventory.json'
    if not (runtime / manifest).is_file():
        raise ValueError('Shared deployment requires EPUB schema 4')
    def owned(name):
        return name in ('HyperEpubWeb.exe', manifest) or name.startswith('Libs/epub-bundle/')
    names = {row['path'] for row in report['files']} | {manifest}
    # Validate every destination before writing. Shared Qt files must agree with
    # both independent inventories; an EPUB update must never replace another kit.
    for name in names:
        output = destination / name
        for path in (output, *output.parents):
            if path.is_symlink() or path.is_junction():
                raise ValueError(f'Linked destination: {name}')
        if output.exists() and not output.is_file():
            raise ValueError(f'Non-file destination: {name}')
    for row in report['files']:
        output = destination / row['path']
        if output.exists() and not owned(row['path']) and digest(output) != row['sha256']:
            raise ValueError(f'Shared runtime integrity mismatch: {row["path"]}')
    bundle = destination / 'Libs/epub-bundle'
    if bundle.exists():
        for path in bundle.rglob('*'):
            if path.is_symlink() or path.is_junction():
                raise ValueError('Linked EPUB bundle')
            if path.is_file() and path.relative_to(destination).as_posix() not in names:
                raise ValueError('Unlisted EPUB bundle input')
    for row in report['files']:
        output = destination / row['path']
        if output.exists() and digest(output) == row['sha256']:
            continue
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(runtime / row['path'], output)
    shutil.copyfile(runtime / manifest, destination / manifest)
    return audit(destination, pin, source, product_candidate=True, shared_root=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--source', type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--verify-only', action='store_true')
    mode.add_argument('--application', type=Path)
    args = parser.parse_args()
    result = (audit(args.runtime, args.manifest_sha256, args.source, product_candidate=True)
              if args.verify_only else deploy(args.runtime, args.manifest_sha256, args.source, args.application))
    print(json.dumps({k: result[k] for k in ('manifestSha256', 'fileCount', 'productProfile', 'releaseEligible')}))


if __name__ == '__main__':
    main()
