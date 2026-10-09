// crosscheck.mjs - bidirectional compatibility check against the official
// @electron/asar tool:
//   1. official `asar pack`  -> our `asar x`  -> byte-compare trees
//   2. our `asar p`          -> official `asar extract` -> byte-compare
//   3. our `asar p`          -> official `asar list`    -> compare listings
//   4. our `asar t` verifies integrity of an official archive
//
// Usage: node scripts/crosscheck.mjs <path-to-asar.exe>
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync, mkdirSync, writeFileSync, readdirSync, statSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, relative, sep } from 'node:path';

const asarExe = process.argv[2];
if (!asarExe) {
  console.error('usage: node scripts/crosscheck.mjs <asar.exe>');
  process.exit(2);
}

const root = mkdtempSync(join(tmpdir(), 'asar-xcheck-'));
const tree = join(root, 'tree');
mkdirSync(join(tree, 'data', 'nested'), { recursive: true });
mkdirSync(join(tree, 'emptydir'), { recursive: true });

// Deterministic sample content, including a >4 MiB file to cross integrity
// block boundaries and a unicode file name.
writeFileSync(join(tree, 'hello.txt'), 'Hello, ASAR!\n');
writeFileSync(join(tree, 'empty.txt'), '');
{
  const big = Buffer.alloc(5 * 1024 * 1024 + 123);
  for (let i = 0; i < big.length; i++) big[i] = i & 0xff;
  writeFileSync(join(tree, 'data', 'big.bin'), big);
}
writeFileSync(join(tree, 'data', 'nested', 'deep.txt'), 'deep\n');
writeFileSync(join(tree, 'unicodé 日本語.txt'), 'unicode content\n');

function run(cmd, args) {
  const r = spawnSync(cmd, args, { encoding: 'buffer' });
  if (r.status !== 0) {
    console.error(`command failed: ${cmd} ${args.join(' ')}\n${r.stdout}\n${r.stderr}`);
    process.exit(1);
  }
  return r.stdout;
}

function npxAsar(args) {
  // Resolve @electron/asar through npx (downloads once, then cached).
  // shell:true is required on Windows because npx is a .cmd shim.
  const quoted = args.map((a) => (/\s/.test(a) ? `"${a}"` : a));
  const r = spawnSync('npx.cmd', ['--yes', '@electron/asar', ...quoted], {
    encoding: 'buffer',
    shell: true,
  });
  if (r.status !== 0) {
    console.error(`command failed: npx @electron/asar ${args.join(' ')}\n${r.stdout}\n${r.stderr}`);
    process.exit(1);
  }
  return r.stdout;
}

function walk(dir, base = dir, out = []) {
  for (const name of readdirSync(dir).sort()) {
    const full = join(dir, name);
    const st = statSync(full);
    if (st.isDirectory()) {
      out.push(relative(base, full) + '/');
      walk(full, base, out);
    } else {
      out.push(relative(base, full));
    }
  }
  return out;
}

function compareTrees(a, b, label) {
  const la = walk(a), lb = walk(b);
  const norm = (arr) => arr.map((p) => p.split(sep).join('/')).sort();
  if (JSON.stringify(norm(la)) !== JSON.stringify(norm(lb))) {
    console.error(`FAIL (${label}): file lists differ\n ours: ${norm(la).join('\n ')}\n theirs: ${norm(lb).join('\n ')}`);
    process.exit(1);
  }
  for (const rel of norm(la)) {
    if (rel.endsWith('/')) continue;
    const ba = readFileSync(join(a, rel));
    const bb = readFileSync(join(b, rel));
    if (!ba.equals(bb)) {
      console.error(`FAIL (${label}): content differs: ${rel}`);
      process.exit(1);
    }
  }
  console.log(`ok: ${label} (${la.length} entries identical)`);
}

let failures = 0;
try {
  // 1. Official packs, we extract.
  const theirs = join(root, 'theirs.asar');
  npxAsar(['pack', tree, theirs]);
  const outTheirs = join(root, 'out_theirs');
  run(asarExe, ['x', theirs, outTheirs]);
  compareTrees(tree, outTheirs, 'official pack -> our extract');
  // Integrity verification of an official archive with our tool.
  run(asarExe, ['t', theirs]);
  console.log('ok: our asar t verifies the official archive');

  // 2. We pack, official extracts.
  const ours = join(root, 'ours.asar');
  run(asarExe, ['p', tree, ours]);
  const outOurs = join(root, 'out_ours');
  npxAsar(['extract', ours, outOurs]);
  compareTrees(tree, outOurs, 'our pack -> official extract');

  // 3. Listings agree. The official list prints paths with '\' separators
  // and a leading '\' on Windows; normalize before comparing.
  const listing = npxAsar(['list', ours]).toString().replace(/\r/g, '');
  const listed = listing.split('\n').map((s) => s.replace(/\\/g, '/').replace(/^\/+/, '').replace(/\/$/, ''))
    .filter((s) => s.length > 0).sort();
  const expected = walk(tree).map((p) => p.split(sep).join('/').replace(/\/$/, '')).sort();
  if (JSON.stringify(listed) !== JSON.stringify(expected)) {
    console.error(`FAIL: listings differ\n official list: ${listed.join('\n ')}\n expected: ${expected.join('\n ')}`);
    failures++;
  } else {
    console.log('ok: official list matches our pack');
  }

  // 4. We can add to an official archive; official can still read it.
  writeFileSync(join(root, 'added.txt'), 'added by asar-windows\n');
  run(asarExe, ['a', theirs, join(root, 'added.txt'), '--to', 'extras']);
  const outAdded = join(root, 'out_added');
  npxAsar(['extract', theirs, outAdded]);
  // The original tree must still be intact (as a subset; extras/ was added).
  const subset = join(root, 'out_added_subset_check');
  const originalEntries = walk(tree).map((p) => p.split(sep).join('/'));
  for (const rel of originalEntries) {
    const src = join(tree, rel);
    const dst = join(outAdded, rel);
    const st = statSync(src);
    if (st.isDirectory()) {
      mkdirSync(dst, { recursive: true });
      continue;
    }
    const ba = readFileSync(src);
    const bb = readFileSync(dst);
    if (!ba.equals(bb)) {
      console.error(`FAIL: original file changed after add: ${rel}`);
      failures++;
    }
  }
  console.log('ok: official archive after our add (originals intact)');
  const addedContent = readFileSync(join(outAdded, 'extras', 'added.txt')).toString();
  if (addedContent !== 'added by asar-windows\n') {
    console.error('FAIL: added file missing after our add');
    failures++;
  } else {
    console.log('ok: our add is visible to the official tool');
  }
} finally {
  rmSync(root, { recursive: true, force: true });
}

if (failures) {
  console.error(`crosscheck: ${failures} FAILURE(S)`);
  process.exit(1);
}
console.log('crosscheck: PASS');
