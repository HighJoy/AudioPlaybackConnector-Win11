#!/usr/bin/env python3
"""Compile a .po file into the YMO resource used by I18n.hpp.

Rewritten with the standard library only: the original needed the
translate-toolkit fork from GitHub, which made CI builds fragile. The binary
layout and FNV-1a hashing stay byte-for-byte identical to FnvHash.hpp.
"""
import sys

FNV1_32_INIT = 0x811C9DC5
FNV_32_PRIME = 0x01000193
ENCODING = 'utf-16le'


def fnv1a_32(data, hval=FNV1_32_INIT):
    for byte in data:
        hval ^= byte
        hval = (hval * FNV_32_PRIME) & 0xFFFFFFFF
    return hval


def unescape(value):
    return (value.replace('\\n', '\n')
                 .replace('\\t', '\t')
                 .replace('\\r', '\r')
                 .replace('\\"', '"')
                 .replace('\\\\', '\\'))


def parse_po(text):
    """Return a list of (context, msgid, msgstr, fuzzy) tuples."""
    entries = []
    context = None
    msgid = None
    msgstr = None
    field = None
    fuzzy = False

    def flush():
        nonlocal context, msgid, msgstr, field, fuzzy
        if msgid is not None:
            entries.append((context, msgid, msgstr or '', fuzzy))
        context = None
        msgid = None
        msgstr = None
        field = None
        fuzzy = False

    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            flush()
            continue
        if line.startswith('#'):
            if 'fuzzy' in line:
                fuzzy = True
            continue
        if line.startswith('msgctxt '):
            context = unescape(line[len('msgctxt '):].strip().strip('"'))
            field = None
        elif line.startswith('msgid '):
            if msgid is not None:
                flush()
            msgid = unescape(line[len('msgid '):].strip().strip('"'))
            field = 'id'
        elif line.startswith('msgstr '):
            msgstr = unescape(line[len('msgstr '):].strip().strip('"'))
            field = 'str'
        elif line.startswith('"'):
            piece = unescape(line.strip().strip('"'))
            if field == 'id':
                msgid += piece
            elif field == 'str':
                msgstr = (msgstr or '') + piece
    flush()
    return entries


def po2ymo(infile, outfile, includefuzzy=False):
    text = infile.read().decode('utf-8')
    units = {}
    for context, source, target, fuzzy in parse_po(text):
        if not source or not target:
            continue  # header entry / untranslated strings
        if fuzzy and not includefuzzy:
            continue
        key = (context + '\004' + source) if context else source
        units[fnv1a_32(key.encode(ENCODING))] = target.encode(ENCODING) + bytes(2)

    outfile.write(len(units).to_bytes(2, 'little'))
    offset = 2 + len(units) * 6
    for hash_value, data in units.items():
        outfile.write(hash_value.to_bytes(4, 'little'))
        outfile.write(offset.to_bytes(2, 'little'))
        offset += len(data)
    for data in units.values():
        outfile.write(data)
    return len(units)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print('usage: po2ymo.py <infile.po> <outfile.ymo>')
        sys.exit(1)
    with open(sys.argv[1], 'rb') as source, open(sys.argv[2], 'wb') as target:
        count = po2ymo(source, target)
    print(f'{sys.argv[2]}: {count} translated strings')
