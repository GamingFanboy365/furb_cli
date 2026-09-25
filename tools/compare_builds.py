#!/usr/bin/env python3
"""compare_builds.py -- run two furb_cli builds over a set of ROMs and report
every ROM whose output differs (per-frame video hashes, WAV, last-frame CPU
RAM and register dump).

    python3 tools/compare_builds.py BUILD_A/furb_cli BUILD_B/furb_cli ROM_OR_DIR... [--frames N]

Directories are searched for .nes/.nsf/.fds/.unf files.  tools/fuzzrom.py
makes OneBus/VT test ROMs for this; public test-ROM collections (blargg etc.)
cover the rest.  Each ROM gets a little input (Start, A, Right) so menus move.
Used to check that the 32-bit (--m32) and native 64-bit builds agree.
"""
import argparse, hashlib, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ap = argparse.ArgumentParser()
ap.add_argument('a')
ap.add_argument('b')
ap.add_argument('roms', nargs='+')
ap.add_argument('--frames', type=int, default=400)
args = ap.parse_args()

roms = []
for r in args.roms:
	if os.path.isdir(r):
		for d, _, fs in os.walk(r):
			roms += [os.path.join(d, f) for f in fs if f.lower().endswith(('.nes', '.nsf', '.fds', '.unf'))]
	else:
		roms.append(r)
roms.sort()
T = tempfile.mkdtemp(prefix='furb_compare_')
N = args.frames


def run(exe, tag, i, rom):
	p = os.path.join(T, '%s%d' % (tag, i))
	cmd = [exe, rom, '--frames', str(N), '--hashes', '--quiet', '--no-save', '--data-dir', p + 'd',
	       '--wav', p + '.wav', '--dump', str(N - 1), '--out', p,
	       '--input', '%d-%d:Start;%d-%d:A;%d-%d:Right' % (N // 4, N // 4 + 5, N // 2, N // 2 + 3, N // 2, N - 1)]
	r = subprocess.run(cmd, capture_output=True, timeout=900)
	out = {'exit': r.returncode, 'video': hashlib.sha1(r.stdout).hexdigest()}
	for k, ext in (('wav', '.wav'), ('ram', '_f%04d.ram' % (N - 1)), ('regs', '_f%04d.txt' % (N - 1))):
		f = p + ext
		out[k] = hashlib.sha1(open(f, 'rb').read()).hexdigest() if os.path.exists(f) else None
	return out


def both(i_rom):
	i, rom = i_rom
	return rom, run(args.a, 'a', i, rom), run(args.b, 'b', i, rom)


bad = 0
with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
	for rom, x, y in ex.map(both, enumerate(roms)):
		diff = [k for k in x if x[k] != y[k]]
		if diff:
			bad += 1
			print('DIFF %-10s %s' % (','.join(diff), rom))
print('%d ROM(s), %d differ' % (len(roms), bad))
sys.exit(1 if bad else 0)
