# ASAR container format

An `.asar` file is Electron's archive format: a JSON header describing a file
tree, followed by the concatenated payload of every file.

Everything is little-endian.

## Layout

```
offset  size  field
0       4     uint32  pickle prefix, always 4
4       4     uint32  headerLen - length of the header pickle buffer
8       4     uint32  payloadLen - pickle payload length (jsonLen aligned up to 4)
12      4     uint32  jsonLen   - byte length of the UTF-8 JSON header
16      jsonLen       UTF-8 JSON: {"files": {...}}
...     pad to headerLen (zero bytes)
8+headerLen   ...     file payloads, concatenated
```

The JSON header is a Chromium "Pickle": `[uint32 payloadLen][uint32 jsonLen]
[json bytes][zero padding to a 4-byte boundary]`.

## Header JSON

```json
{
  "files": {
    "main.js": {
      "size": 1234,
      "offset": "0",
      "integrity": {
        "algorithm": "SHA256",
        "hash": "<hex of whole file>",
        "blockSize": 4194304,
        "blocks": ["<hex of 4 MiB block 0>", "..."]
      }
    },
    "resources": { "files": { "icon.png": { "...": "..." } } },
    "link.txt": { "link": "main.js" },
    "unpacked.bin": { "size": 5, "offset": "1234", "unpacked": true }
  }
}
```

* `offset` is a **decimal string**, relative to the start of the data section
  (`8 + headerLen`). Absolute payload offset = `8 + headerLen + Number(offset)`.
* Directory nodes contain a `files` object. Empty directories are valid.
* `link` nodes are symlinks; the target is archive-relative and `/`-separated.
* `unpacked: true` means the payload lives outside the archive in
  `<archive path>.unpacked/<relative path>` (Electron's "unpack binaries"
  feature).
* `executable: true` mirrors the Unix executable bit (informational on
  Windows).
* `integrity` is written by the official `asar` tool; `blockSize` is 4194304
  and there is always **at least one** block hash, even for empty files (the
  digest of the empty string).

## Notes for implementers

* The header is parsed leniently by Electron; writers should match the
  official layout above (compact JSON, sorted or readdir order - both work).
* Adding a file to an existing archive does not require rewriting the
  payloads: keep existing offsets, append new payloads after the old data
  section, and write the updated header at the front. This is what
  `asar a` and the 7-Zip plugin do.
* There is no per-file checksum in the container itself; `integrity` is
  advisory metadata (verified by Electron only when the corresponding fuse is
  enabled).
