#!/usr/bin/env python3
"""gui_smoke.py -- start the GUI (furb) on a private X server and check that
it runs a ROM, draws frames, takes keyboard input, opens dialogs and exits
cleanly.  Needs Xvfb, xdotool and a window manager (openbox); SDL's dummy
audio driver stands in for a sound card.

    python3 tools/gui_smoke.py [--furb build/furb] [--screens DIR]

--screens keeps a screenshot of each step (needs ImageMagick's import).
"""
import argparse, os, shutil, signal, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('--furb', default=os.path.join(HERE, '..', 'build', 'furb'))
ap.add_argument('--screens')
a = ap.parse_args()
for tool in ('Xvfb', 'xdotool', 'openbox'):
	if not shutil.which(tool):
		sys.exit('gui_smoke: %s not found (apt install xvfb xdotool openbox)' % tool)

T = tempfile.mkdtemp(prefix='furb_gui_')
fails = []
def check(name, ok, detail=''):
	print('%-50s %s %s' % (name, 'ok  ' if ok else 'FAIL', detail))
	if not ok:
		fails.append(name)

# a ROM that shows the controller: $4016 bit 0 of the first read goes to the
# background colour every frame (A pressed = a different colour)
code = bytes([
	0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x80, 0x8D, 0x00, 0x20,	# SEI CLD LDX #$FF TXS; NMI on
	0xA9, 0x08, 0x8D, 0x01, 0x20,					# show background
	0x4C, 0x0F, 0xC0,						# loop
	# NMI ($C012): strobe, read A, palette 0 = $0F or $21
	0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x00, 0x8D, 0x16, 0x40,
	0xAD, 0x16, 0x40, 0x29, 0x01, 0xAA,				# LDA $4016 AND #1 TAX
	0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,
	0xBD, 0x40, 0xC0, 0x8D, 0x07, 0x20,				# LDA colors,X STA $2007
	0xA9, 0x00, 0x8D, 0x06, 0x20, 0x8D, 0x06, 0x20, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20,
	0x40])
prg = bytearray(16384)
prg[:len(code)] = code
prg[0x40:0x42] = bytes([0x0F, 0x21])					# black, light blue
prg[0x3FFA:0x4000] = struct.pack('<HHH', 0xC012, 0xC000, 0xC012)
rom = os.path.join(T, 'pad.nes')
open(rom, 'wb').write(b'NES\x1a\x01\x01' + bytes(10) + bytes(prg) + bytes(8192))

env = dict(os.environ, DISPLAY=':77', SDL_AUDIODRIVER='dummy')
xvfb = subprocess.Popen(['Xvfb', ':77', '-screen', '0', '1024x768x24'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
wm = subprocess.Popen(['openbox'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
cfg = os.path.join(T, 'settings.reg')
log = open(os.path.join(T, 'log.txt'), 'w')
furb = subprocess.Popen([a.furb, rom, '--data-dir', os.path.join(T, 'data'), '--config', cfg], env=env, stdout=log, stderr=log)

def xdo(*args):
	return subprocess.run(['xdotool'] + list(args), env=env, capture_output=True, text=True).stdout.strip()
def window(name, timeout=10):
	end = time.time() + timeout
	while time.time() < end:
		w = xdo('search', '--onlyvisible', '--name', name)
		if w:
			return w.split()[0]
		time.sleep(0.2)
	return None
def pixel(x, y):
	"""RGB of the screen at x, y"""
	r = subprocess.run(['import', '-window', 'root', '-crop', '1x1+%d+%d' % (x, y), 'txt:-'], env=env, capture_output=True, text=True)
	for line in r.stdout.splitlines():
		if '(' in line and not line.startswith('#'):
			return tuple(int(float(v)) for v in line.split('(')[1].split(')')[0].split(',')[:3])
	return None
def shot(name):
	if a.screens and shutil.which('import'):
		os.makedirs(a.screens, exist_ok=True)
		subprocess.run(['import', '-window', 'root', os.path.join(a.screens, name + '.png')], env=env)

try:
	main = window('FPS')
	check('main window shows, title reports FPS', main is not None, xdo('getwindowname', main) if main else '')
	if main:
		time.sleep(1)
		fps = xdo('getwindowname', main)
		n = int(fps.split(' - ')[1].split()[0]) if ' FPS' in fps else 0
		check('runs at about 60 fps', 50 <= n <= 70, fps)
		geo = xdo('getwindowgeometry', main)
		xdo('windowactivate', '--sync', main)
		pos = [int(v) for v in geo.split('Position: ')[1].split()[0].split(',')]
		x, y = pos[0] + 100, pos[1] + 200
		before = pixel(x, y)
		shot('1-running')
		xdo('keydown', 'x'); time.sleep(0.5)
		held = pixel(x, y)
		shot('2-A-held')
		xdo('keyup', 'x'); time.sleep(0.5)
		after = pixel(x, y)
		check('keyboard reaches the pad (A = X key changes the screen)', before == after and held != before, '%s %s %s' % (before, held, after))
		xdo('key', 'F10'); time.sleep(1.5)
		hdr = window('iNES Header Editor', 5)
		shot('3-header-editor')
		check('F10 opens the header editor dialog', hdr is not None)
		if hdr:
			xdo('windowactivate', '--sync', hdr)
			xdo('key', 'Escape'); time.sleep(0.8)	# (GTK ignores xdotool's synthetic --window events)
			check('Escape closes it', window('iNES Header Editor', 1) is None)
		xdo('windowactivate', '--sync', main)
		xdo('key', 'ctrl+F2'); time.sleep(1.5)
		check('Ctrl+F2 opens the PPU debugger', window('Debugger - PPU', 5) is not None)
		shot('4-ppu-debugger')
	furb.send_signal(signal.SIGTERM)
	try:
		code = furb.wait(10)
	except subprocess.TimeoutExpired:
		code = None
	check('SIGTERM closes it cleanly', code == 0, str(code))
	check('settings saved on exit', os.path.exists(cfg) and 'ButtonsStdController1' in open(cfg).read())
finally:
	if furb.poll() is None:
		furb.kill()
	wm.kill()
	xvfb.kill()
print('\n%d failure(s); temp dir %s' % (len(fails), T))
sys.exit(1 if fails else 0)
