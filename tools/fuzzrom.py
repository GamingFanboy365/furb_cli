#!/usr/bin/env python3
"""fuzzrom.py -- generate OneBus (mapper 256, NES 2.0) register-fuzzing ROMs
for every famiclone console type, for regression-testing furb builds against
each other (e.g. the 32-bit build against the 64-bit one).

    python3 tools/fuzzrom.py OUTDIR

Every frame the program fills OAM with pseudo-random bytes and DMAs it, makes
random writes to $2010-$201F (VT PPU), $4100-$413F (VT/OneBus CPU side,
including banking) and the APU, streams random bytes into VRAM and palette
RAM, and picks random scroll/control values with rendering on.  Every 8 KiB
PRG bank holds the same program, so random PRG bank switches keep running it.
The output is meaningless as a picture; what matters is that it touches the
OneBus/VT PPU paths (4bpp, extended palettes, 16x16 sprites, hi-res) and is
deterministic, so two builds must produce the same frames and samples.
"""
import os, random, sys

CONSOLES = {'vt02': 6, 'vt03': 7, 'vt09': 8, 'vt32': 9, 'vt369': 10, 'um6578': 11}


class Asm:
	"""Just enough of a 6502 assembler for this program."""
	OPS = {  # mnemonic -> {mode: opcode}
		'LDA': {'imm': 0xA9, 'zp': 0xA5, 'abs': 0xAD}, 'STA': {'zp': 0x85, 'abs': 0x8D, 'absx': 0x9D},
		'LDX': {'imm': 0xA2}, 'LDY': {'imm': 0xA0}, 'CMP': {'zp': 0xC5}, 'AND': {'imm': 0x29},
		'ORA': {'imm': 0x09}, 'EOR': {'imm': 0x49, 'zp': 0x45}, 'INC': {'zp': 0xE6}, 'ROL': {'zp': 0x26},
		'JMP': {'abs': 0x4C}, 'JSR': {'abs': 0x20},
		'SEI': 0x78, 'CLD': 0xD8, 'TXS': 0x9A, 'TAX': 0xAA, 'INX': 0xE8, 'DEY': 0x88, 'RTS': 0x60,
		'RTI': 0x40, 'ASL': 0x0A,
		'BEQ': 0xF0, 'BNE': 0xD0, 'BCC': 0x90,
	}

	def __init__(self, org):
		self.org, self.code, self.labels, self.fix = org, bytearray(), {}, []

	def pc(self):
		return self.org + len(self.code)

	def label(self, name):
		self.labels[name] = self.pc()

	def __call__(self, op, arg=None, mode=None):
		o = self.OPS[op]
		if isinstance(o, int) and op in ('BEQ', 'BNE', 'BCC'):
			self.code += bytes([o, 0])
			self.fix.append(('rel', len(self.code) - 1, arg))
		elif isinstance(o, int):
			self.code.append(o)
		elif isinstance(arg, str):
			self.code += bytes([o['abs'], 0, 0])
			self.fix.append(('abs', len(self.code) - 2, arg))
		else:
			mode = mode or ('zp' if arg < 0x100 and 'zp' in o else 'abs')
			self.code.append(o[mode])
			self.code += bytes([arg & 0xFF]) if mode in ('imm', 'zp') else bytes([arg & 0xFF, arg >> 8])

	def link(self):
		for kind, at, name in self.fix:
			t = self.labels[name]
			if kind == 'abs':
				self.code[at:at + 2] = bytes([t & 0xFF, t >> 8])
			else:
				d = t - (self.org + at + 1)
				assert -128 <= d < 128, name
				self.code[at] = d & 0xFF
		return bytes(self.code)


def program(seed):
	a = Asm(0xE000)
	SEED, FRAME = 0x00, 0x03
	a.label('reset')
	a('SEI'); a('CLD'); a('LDX', 0xFF, 'imm'); a('TXS')
	a('LDA', seed & 0xFF, 'imm'); a('STA', SEED)
	a('LDA', seed >> 8 | 1, 'imm'); a('STA', SEED + 1)
	a('LDA', 0x80, 'imm'); a('STA', 0x2000)
	a.label('loop')
	a('LDA', FRAME)
	a.label('wait'); a('CMP', FRAME); a('BEQ', 'wait')
	# random OAM, DMA'd
	a('LDX', 0, 'imm')
	a.label('oam'); a('JSR', 'rnd'); a('STA', 0x0200, 'absx'); a('INX'); a('BNE', 'oam')
	a('LDA', 0, 'imm'); a('STA', 0x2003); a('LDA', 2, 'imm'); a('STA', 0x4014)
	# VT PPU registers $2010-$201F
	a('LDY', 8, 'imm')
	a.label('r1'); a('JSR', 'rnd'); a('AND', 0x0F, 'imm'); a('ORA', 0x10, 'imm'); a('TAX')
	a('JSR', 'rnd'); a('STA', 0x2000, 'absx'); a('DEY'); a('BNE', 'r1')
	# OneBus / VT CPU-side registers $4100-$413F
	a('LDY', 8, 'imm')
	a.label('r2'); a('JSR', 'rnd'); a('AND', 0x3F, 'imm'); a('TAX')
	a('JSR', 'rnd'); a('STA', 0x4100, 'absx'); a('DEY'); a('BNE', 'r2')
	# APU
	a('LDY', 4, 'imm')
	a.label('r3'); a('JSR', 'rnd'); a('AND', 0x0F, 'imm'); a('TAX')
	a('JSR', 'rnd'); a('STA', 0x4000, 'absx'); a('DEY'); a('BNE', 'r3')
	a('LDA', 0x0F, 'imm'); a('STA', 0x4015)
	# 64 random bytes to a random VRAM/palette address
	a('JSR', 'rnd'); a('AND', 0x1F, 'imm'); a('ORA', 0x20, 'imm'); a('STA', 0x2006)
	a('JSR', 'rnd'); a('STA', 0x2006)
	a('LDY', 64, 'imm')
	a.label('r4'); a('JSR', 'rnd'); a('STA', 0x2007); a('DEY'); a('BNE', 'r4')
	# scroll, control (NMI stays on), mask (rendering stays on)
	a('LDA', 0, 'imm'); a('STA', 0x2006); a('STA', 0x2006)
	a('JSR', 'rnd'); a('STA', 0x2005); a('JSR', 'rnd'); a('STA', 0x2005)
	a('JSR', 'rnd'); a('AND', 0x3B, 'imm'); a('ORA', 0x80, 'imm'); a('STA', 0x2000)
	a('JSR', 'rnd'); a('AND', 0xE1, 'imm'); a('ORA', 0x1E, 'imm'); a('STA', 0x2001)
	a('JMP', 'loop')
	a.label('nmi'); a('INC', FRAME)
	a.label('irq'); a('RTI')
	# 16-bit Galois LFSR step; returns lo^hi in A.  Touches only A.
	a.label('rnd')
	a('LDA', SEED); a('ASL'); a('ROL', SEED + 1); a('BCC', 'n1'); a('EOR', 0x39, 'imm')
	a.label('n1'); a('STA', SEED); a('EOR', SEED + 1); a('RTS')
	code = a.link()
	return code, a.labels


def build(console, seed=0xACE1):
	rng = random.Random(console * 7919 + seed)
	code, labels = program(seed + console)
	bank = bytearray(rng.getrandbits(8) for _ in range(8192))
	bank[:len(code)] = code
	for i, v in enumerate(('nmi', 'reset', 'irq')):
		bank[0x1FFA + 2 * i:0x1FFC + 2 * i] = labels[v].to_bytes(2, 'little')
	prg = bytes(bank) * 64                       # 512 KiB, every bank the same program
	chr_ = bytes(rng.getrandbits(8) for _ in range(256 * 1024))
	hdr = bytearray(16)
	hdr[0:4] = b'NES\x1a'
	hdr[4], hdr[5] = 32, 32                      # 32 x 16 KiB PRG, 32 x 8 KiB CHR
	hdr[6] = 0x00                                # mapper 256: low nibble 0
	hdr[7] = 0x08 | 0x03                         # NES 2.0, extended console type
	hdr[8] = 0x01                                # mapper bits 8-11 = 1, submapper 0
	hdr[10] = 0x07                               # 8 KiB PRG RAM
	hdr[13] = console
	return bytes(hdr) + prg + chr_


if __name__ == '__main__':
	out = sys.argv[1] if len(sys.argv) > 1 else '.'
	os.makedirs(out, exist_ok=True)
	for name, ct in CONSOLES.items():
		p = os.path.join(out, 'fuzz_%s.nes' % name)
		open(p, 'wb').write(build(ct))
		print(p)
