"""Copy an independently pinned product runtime into a fresh isolated application layout."""
import argparse
import json
from pathlib import Path
import shutil
from qualify_runtime import audit


def deploy(runtime, pin, source, application):
    runtime = runtime.resolve(strict=True)
    destination = (application / 'Libs' / 'Epub').resolve()
    if destination == runtime or runtime in destination.parents or destination in runtime.parents:
        raise ValueError('Runtime and destination must be separate')
    report = audit(runtime, pin, source, product_candidate=True)
    if destination.exists():
        # Never merge into or repair an existing tree. An identical snapshot is idempotent.
        audit(destination, pin, source, product_candidate=True)
        return report
    destination.mkdir(parents=True, exist_ok=False)
    for row in report['files']:
        output = destination / row['path']
        output.parent.mkdir(parents=True, exist_ok=True)
        with (runtime / row['path']).open('rb') as reader, output.open('xb') as writer:
            shutil.copyfileobj(reader, writer)
    manifest = 'epub-runtime-inventory.json'
    with (runtime / manifest).open('rb') as reader, (destination / manifest).open('xb') as writer:
        shutil.copyfileobj(reader, writer)
    # Source drift during copying cannot produce an accepted destination.
    return audit(destination, pin, source, product_candidate=True)


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
