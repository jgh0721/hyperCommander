"""Verify Gitiles source content; its tar member timestamps vary per request."""
import hashlib
from pathlib import PurePosixPath
import sys
import tarfile


def verify(archive, expected):
    files = {}
    with tarfile.open(archive) as source:
        for member in source:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or "\\" in member.name:
                raise ValueError("Unsafe source archive path")
            if member.isdir():
                continue
            if not member.isfile() or member.name in files:
                raise ValueError("Unexpected source archive member")
            files[member.name] = hashlib.sha256(source.extractfile(member).read()).hexdigest()
    canonical = "".join(name + "\0" + files[name] + "\n" for name in sorted(files))
    digest = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    if digest != expected.lower():
        raise ValueError("Codec file-tree SHA256 mismatch: " + digest)
    print("Verified source file-tree SHA256:", digest)


if __name__ == "__main__":
    verify(*sys.argv[1:])
