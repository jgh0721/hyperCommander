"""Development-only cross-reader ISO validation; no Python product dependency.

Requires the pinned PyCdlib source from IsoImage/packaging/source-lock.json.
The supplied image is read-only. Outputs are created exclusively under --work.
"""
import argparse
import collections
import ctypes as c
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import time


class Source(c.Structure):
    _fields_ = [('disk', c.c_char_p), ('path', c.c_char_p), ('directory', c.c_uint32)]


class Options(c.Structure):
    _fields_ = [('size', c.c_uint32), ('profile', c.c_uint32), ('label', c.c_char_p),
                ('limit', c.c_uint64), ('timestamp', c.c_int64),
                ('progress', c.c_void_p), ('context', c.c_void_p)]


class Info(c.Structure):
    _fields_ = [('size', c.c_uint32), ('namespaces', c.c_uint32), ('selected', c.c_uint32),
                ('revision', c.c_uint32), ('bytes', c.c_uint64), ('count', c.c_uint64),
                ('label', c.c_char * 129), ('features', c.c_uint32)]


class Entry(c.Structure):
    _fields_ = [('size', c.c_uint32), ('directory', c.c_uint32),
                ('bytes', c.c_uint64), ('path', c.c_char_p)]


class HashSink:
    def __init__(self):
        self.hash = hashlib.sha256()
        self.size = 0

    def write(self, data):
        self.hash.update(data)
        self.size += len(data)
        return len(data)


def fileHash(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class Engine:
    def __init__(self, path):
        self.dll = c.CDLL(str(path.resolve()))
        self.dll.hcIsoLastError.restype = c.c_char_p
        self.dll.hcIsoOpen.argtypes = [c.c_char_p, c.c_uint32, c.POINTER(c.c_void_p)]
        self.dll.hcIsoClose.argtypes = [c.c_void_p]
        self.dll.hcIsoGetInfo.argtypes = [c.c_void_p, c.POINTER(Info)]
        self.dll.hcIsoGetEntry.argtypes = [c.c_void_p, c.c_uint64, c.POINTER(Entry)]
        self.dll.hcIsoRead.argtypes = [c.c_void_p, c.c_uint64, c.c_uint64,
                                      c.c_void_p, c.c_uint32, c.POINTER(c.c_uint32)]
        self.dll.hcIsoBuild.argtypes = [c.c_char_p, c.POINTER(Source), c.c_uint64,
                                       c.POINTER(Options)]

    def check(self, result):
        if result:
            raise RuntimeError(f'core status {result}: {self.dll.hcIsoLastError().decode()}')

    def scan(self, path, namespace, destination=None, hashFiles=True):
        handle = c.c_void_p()
        self.check(self.dll.hcIsoOpen(str(path.resolve()).encode(), namespace, c.byref(handle)))
        try:
            info = Info(size=c.sizeof(Info))
            self.check(self.dll.hcIsoGetInfo(handle, c.byref(info)))
            metadata = {'namespaces': info.namespaces, 'selected': info.selected,
                        'udfRevision': f'{info.revision >> 8}.{info.revision & 255:02x}',
                        'imageBytes': info.bytes, 'label': info.label.decode(),
                        'features': info.features}
            entries = {}
            buffer = c.create_string_buffer(1024 * 1024)
            for index in range(info.count):
                entry = Entry(size=c.sizeof(Entry))
                self.check(self.dll.hcIsoGetEntry(handle, index, c.byref(entry)))
                name = entry.path.decode()
                parts = PurePosixPath(name).parts
                if not parts or any(part in ('.', '..') or ':' in part or '\\' in part for part in parts) or name.startswith('/'):
                    raise ValueError(f'unsafe path: {name}')
                output = None
                target = destination.joinpath(*parts) if destination else None
                if target:
                    if not target.resolve().is_relative_to(destination.resolve()):
                        raise ValueError(f'extraction escaped: {name}')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if entry.directory:
                        target.mkdir(exist_ok=True)
                    else:
                        output = target.open('xb')
                digest = hashlib.sha256()
                offset = 0
                try:
                    if not entry.directory and hashFiles:
                        while offset < entry.bytes:
                            count = c.c_uint32()
                            self.check(self.dll.hcIsoRead(handle, index, offset, buffer, len(buffer), c.byref(count)))
                            if not count.value:
                                raise RuntimeError(f'premature EOF: {name}')
                            data = buffer.raw[:count.value]
                            digest.update(data)
                            if output:
                                output.write(data)
                            offset += count.value
                finally:
                    if output:
                        output.close()
                entries[name] = {'directory': bool(entry.directory), 'bytes': entry.bytes,
                                 'sha256': digest.hexdigest() if not entry.directory and hashFiles else None}
            return metadata, entries
        finally:
            self.dll.hcIsoClose(handle)

    def build(self, target, directory, entries, profile):
        sources = (Source * len(entries))(*[
            Source(None if item['directory'] else str(directory.joinpath(*PurePosixPath(name).parts).resolve()).encode(),
                   name.encode(), item['directory']) for name, item in sorted(entries.items())])
        options = Options(c.sizeof(Options), profile, b'HC_WINDOWS_TEST', 0, 0, None, None)
        self.check(self.dll.hcIsoBuild(str(target.resolve()).encode(), sources, len(sources), c.byref(options)))


def referenceScan(path, namespace, pycdlib):
    iso = pycdlib.PyCdlib()
    iso.open(str(path))
    try:
        entries = {}
        key = {1: 'iso_path', 2: 'joliet_path', 3: 'udf_path'}[namespace]
        for root, directories, files in iso.walk(**{key: '/'}):
            for name in directories:
                full = root.rstrip('/') + '/' + name
                entries[full.lstrip('/')] = {'directory': True, 'bytes': 0, 'sha256': None}
            for name in files:
                full = root.rstrip('/') + '/' + name
                sink = HashSink()
                iso.get_file_from_iso_fp(sink, **{key: full})
                normalized = full.lstrip('/')
                if namespace == 1:
                    normalized = normalized.rsplit(';', 1)[0].rstrip('.')
                entries[normalized] = {'directory': False, 'bytes': sink.size, 'sha256': sink.hash.hexdigest()}
        return entries
    finally:
        iso.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', required=True, type=Path)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--pycdlib-source', required=True, type=Path)
    parser.add_argument('--profile', type=int, choices=(2, 3), default=2,
                        help='2: ISO/Joliet/UDF; 3: ISO/UDF for long Windows names')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.pycdlib_source.resolve()))
    import pycdlib
    engine = Engine(args.core)
    report = {'image': args.image.name, 'coreSha256': fileHash(args.core), 'phases': []}
    started = time.monotonic()

    def record(phase, **values):
        report['phases'].append({'phase': phase, 'elapsedSeconds': round(time.monotonic() - started, 2), **values})
        (args.work / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\r\n')
        print(phase, json.dumps(values, ensure_ascii=True), flush=True)

    report['inputSha256'] = fileHash(args.image)
    original = {}
    for namespace in (1, 2, 3):
        try:
            metadata, entries = engine.scan(args.image, namespace, hashFiles=False)
        except RuntimeError as error:
            if str(error) == 'core status 3: Requested namespace absent':
                record(f'original.namespace.{namespace}.absent')
                continue
            raise
        original[namespace] = entries
        record(f'original.namespace.{namespace}.enumerate', metadata=metadata, entries=len(entries))
    destination = args.work / 'extracted'
    destination.mkdir()
    metadata, expected = engine.scan(args.image, 3, destination)
    (args.work / 'inventory.json').write_text(json.dumps(expected, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\r\n')
    record('original.udf.extract', files=sum(not item['directory'] for item in expected.values()),
           directories=sum(item['directory'] for item in expected.values()),
           bytes=sum(item['bytes'] for item in expected.values()),
           over4GiB=[name for name, item in expected.items() if item['bytes'] > 0xffffffff])
    reference = referenceScan(args.image, 3, pycdlib)
    if reference != expected:
        raise AssertionError('Original UDF differs from PyCdlib inventory/payload hashes')
    record('original.udf.pycdlib.compare', matched=len(expected))
    recreated = args.work / 'recreated-bridge.iso'
    engine.build(recreated, destination, expected, args.profile)
    record('recreated.build', bytes=recreated.stat().st_size, sha256=fileHash(recreated))
    for namespace in (1, 2, 3):
        if namespace == 2 and args.profile == 3:
            continue
        metadata, actual = engine.scan(recreated, namespace)
        reference = referenceScan(recreated, namespace, pycdlib)
        if actual != reference:
            raise AssertionError(f'Recreated namespace {namespace} differs from PyCdlib')
        if namespace == 1:
            def fingerprints(entries):
                return collections.Counter((item['directory'], item['bytes'], item['sha256']) for item in entries.values())
            if fingerprints(actual) != fingerprints(expected):
                raise AssertionError('Primary aliases differ in count, size, or payload')
        elif actual != expected:
            raise AssertionError(f'Recreated namespace {namespace} lost names or payloads')
        record(f'recreated.namespace.{namespace}.compare', metadata=metadata, matched=len(actual))
    if fileHash(args.image) != report['inputSha256']:
        raise AssertionError('Original ISO changed during validation')
    record('complete', originalPreserved=True, bootRecreated=False)


if __name__ == '__main__':
    main()
