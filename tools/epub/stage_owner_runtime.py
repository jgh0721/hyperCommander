"""Stage a fresh diagnostic runtime or explicit product candidate. Never install or approve."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--kit', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--product-candidate', action='store_true',
                        help='Stage a product-profile candidate; does not install or approve it')
    parser.add_argument('--sign-product', action='store_true', help='Sign all product PE files before sealing the inventory')
    parser.add_argument('--signing-cache', type=Path, help='Existing local signing cache; never contains approval for EPUB input files')
    args = parser.parse_args()
    if args.sign_product and not args.product_candidate:
        raise ValueError('Signing requires explicit product candidate mode')
    if args.product_candidate and args.executable.name != 'HyperEpubWeb.exe':
        raise ValueError('Product candidate requires the separately built HyperEpubWeb.exe')
    args.output = args.output.resolve()
    for source in [args.kit.resolve(), args.bundle.resolve(), args.executable.resolve().parent]:
        if args.output == source or source in args.output.parents:
            raise ValueError('Output must be separate from runtime and bundle inputs')
    args.output.mkdir(parents=True, exist_ok=False)
    exe = args.output/('HyperEpubWeb.exe' if args.product_candidate else 'hc_epub_render_poc.exe')
    shutil.copyfile(args.executable, exe)
    (args.output/'bundle').mkdir()
    for name in ['reader.js', 'bundle.json', 'THIRD-PARTY-NOTICES.txt']:
        shutil.copyfile(args.bundle/name, args.output/'bundle'/name)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    # Match cmake/DeployQt.cmake's Hybrid CRT plugin workaround and embedded qt.conf layout.
    plugins = [args.kit/'plugins/platforms/qwindows.dll', args.kit/'plugins/imageformats/qjpeg.dll', args.kit/'plugins/imageformats/qsvg.dll']
    command = [str(args.kit/'bin/windeployqt.exe'), '--release', '--no-gui', '--no-plugins',
               '--no-compiler-runtime', '--no-translations', str(exe.resolve()), *map(str, plugins)]
    env = os.environ.copy()
    env['PATH'] = str(args.kit/'bin')+os.pathsep+env['PATH']
    run = subprocess.run(command, env=env, capture_output=True, startupinfo=startup, timeout=180)
    (args.output/'deployment.log').write_bytes(run.stdout+run.stderr)
    if run.returncode:
        raise RuntimeError('windeployqt failed; see deployment.log')
    shutil.copyfile(args.kit/'bin/Qt6Gui.dll', args.output/'Qt6Gui.dll')
    for plugin in plugins:
        destination = args.output/'Libs'/plugin.parent.name/plugin.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(plugin, destination)
    shutil.copytree(args.kit/'resources', args.output/'Libs/resources')
    locale = args.output/'Libs/translations/qtwebengine_locales'
    locale.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.kit/'translations/qtwebengine_locales/en-US.pak', locale/'en-US.pak')
    if args.sign_product:
        signer = Path(__file__).resolve().parents[1]/'update/Sign-ProductFile.ps1'
        for path in sorted(args.output.rglob('*')):
            if path.is_file() and path.suffix.lower() in ('.exe', '.dll'):
                command = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                    '-File', str(signer), '-Path', str(path), '-RelativePath', path.relative_to(args.output).as_posix()]
                if args.signing_cache:
                    command += ['-CacheRoot', str(args.signing_cache.resolve())]
                signed = subprocess.run(command,
                    capture_output=True, startupinfo=startup, timeout=180)
                with (args.output/'deployment.log').open('ab') as log:
                    log.write(signed.stdout+signed.stderr)
                if signed.returncode:
                    raise RuntimeError('Product signing failed; see deployment.log')
    files = []
    for path in sorted(args.output.rglob('*')):
        if path.is_file() and path.name != 'deployment.log':
            files.append(dict(path=path.relative_to(args.output).as_posix(), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    if len(files) > 500:
        raise RuntimeError('Runtime file budget exceeded')
    metadata = dict(schemaVersion=2, diagnosticOnly=True, releaseEligible=False,
        profile='epub-passive-read-only-poc', protocolVersion=1, helper=exe.name,
        bundleManifestSha256=hashlib.sha256((args.output/'bundle/bundle.json').read_bytes()).hexdigest(), files=files)
    if args.product_candidate:
        metadata.update(schemaVersion=3, diagnosticOnly=False, releaseEligible=True,
                        profile='epub-passive-read-only', readOnly=True, readerEngine='epubjs', readerVersion='0.3.93')
    manifest = args.output/('epub-runtime-inventory.json' if args.product_candidate else 'poc-runtime.json')
    manifest.write_text(json.dumps(metadata, indent=2), encoding='utf-8', newline='\r\n')
    print(json.dumps(dict(runtime=str(args.output.resolve()), manifestSha256=hashlib.sha256(manifest.read_bytes()).hexdigest(), files=len(files))))


if __name__ == '__main__':
    main()
