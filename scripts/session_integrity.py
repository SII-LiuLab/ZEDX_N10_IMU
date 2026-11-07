#!/usr/bin/env python3
"""Create/verify a whole-session SHA256 manifest, including metadata and derived images."""
import argparse
import hashlib
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(8*1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def seal(root):
    root = Path(root)
    manifest = root / 'SESSION-SHA256SUMS'
    lines = []
    for path in sorted(root.rglob('*')):
        if path.is_file() and path != manifest:
            if path.is_symlink():
                raise ValueError(f'Symlink in session: {path}')
            lines.append(f'{digest(path)}  {path.relative_to(root).as_posix()}\n')
    manifest.write_text(''.join(lines))
    return len(lines)


def verify(root):
    root = Path(root).resolve()
    count, seen = 0, set()
    for line in (root / 'SESSION-SHA256SUMS').read_text().splitlines():
        expected, relative = line.split('  ', 1)
        path = root / relative
        if path.is_symlink() or not path.resolve().is_relative_to(root) or relative in seen:
            raise ValueError(f'Invalid manifest path: {relative}')
        if digest(path) != expected:
            raise ValueError(f'Checksum mismatch: {relative}')
        seen.add(relative)
        count += 1
    if count == 0:
        raise ValueError('Empty manifest')
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() and p != root/'SESSION-SHA256SUMS'}
    if actual != seen:
        raise ValueError(f'Unlisted or missing session files: {actual ^ seen}')
    return count


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['seal', 'verify'])
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    count = (seal if args.action == 'seal' else verify)(args.directory)
    print(f'{args.action}: {count} files passed')
