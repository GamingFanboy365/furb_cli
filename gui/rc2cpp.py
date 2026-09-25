#!/usr/bin/env python3
"""rc2cpp.py -- compile a Visual Studio .rc script into C++ tables for the
GTK Win32 layer (gui/): menus, accelerator tables, dialog templates and the
string table, exactly as rc.exe would lay them out (dialog units, styles,
default control styles).  Icons, bitmaps, version info and the designer's
blocks are skipped.

    python3 gui/rc2cpp.py SCRIPT.rc RESOURCE.h OUT.cpp

Symbols come from the module's resource.h plus the standard Win32 constants
below.  The output defines 'furb_module_resources' (compat/furb_rc.h); every
module (the program and each mapper pack) links its own.
"""
import os, re, sys

WIN32 = {
	# window styles
	'WS_OVERLAPPED': 0, 'WS_POPUP': 0x80000000, 'WS_CHILD': 0x40000000, 'WS_MINIMIZE': 0x20000000,
	'WS_VISIBLE': 0x10000000, 'WS_DISABLED': 0x08000000, 'WS_CLIPSIBLINGS': 0x04000000,
	'WS_CLIPCHILDREN': 0x02000000, 'WS_MAXIMIZE': 0x01000000, 'WS_CAPTION': 0x00C00000,
	'WS_BORDER': 0x00800000, 'WS_DLGFRAME': 0x00400000, 'WS_VSCROLL': 0x00200000, 'WS_HSCROLL': 0x00100000,
	'WS_SYSMENU': 0x00080000, 'WS_THICKFRAME': 0x00040000, 'WS_GROUP': 0x00020000, 'WS_TABSTOP': 0x00010000,
	'WS_MINIMIZEBOX': 0x00020000, 'WS_MAXIMIZEBOX': 0x00010000, 'WS_OVERLAPPEDWINDOW': 0x00CF0000,
	'WS_POPUPWINDOW': 0x80880000, 'WS_EX_TOOLWINDOW': 0x80, 'WS_EX_CLIENTEDGE': 0x200, 'WS_EX_STATICEDGE': 0x20000,
	'WS_EX_TOPMOST': 8, 'WS_EX_ACCEPTFILES': 0x10, 'WS_EX_TRANSPARENT': 0x20, 'WS_EX_APPWINDOW': 0x40000,
	'WS_EX_WINDOWEDGE': 0x100, 'WS_EX_DLGMODALFRAME': 1, 'WS_EX_RIGHT': 0x1000, 'WS_EX_CONTROLPARENT': 0x10000,
	'WS_EX_NOPARENTNOTIFY': 4, 'WS_EX_LEFT': 0, 'WS_EX_LTRREADING': 0, 'WS_EX_RIGHTSCROLLBAR': 0,
	# dialog styles
	'DS_ABSALIGN': 1, 'DS_SYSMODAL': 2, 'DS_LOCALEDIT': 0x20, 'DS_SETFONT': 0x40, 'DS_MODALFRAME': 0x80,
	'DS_NOIDLEMSG': 0x100, 'DS_SETFOREGROUND': 0x200, 'DS_3DLOOK': 4, 'DS_FIXEDSYS': 8, 'DS_NOFAILCREATE': 0x10,
	'DS_CONTROL': 0x400, 'DS_CENTER': 0x800, 'DS_CENTERMOUSE': 0x1000, 'DS_CONTEXTHELP': 0x2000, 'DS_SHELLFONT': 0x48,
	# buttons
	'BS_PUSHBUTTON': 0, 'BS_DEFPUSHBUTTON': 1, 'BS_CHECKBOX': 2, 'BS_AUTOCHECKBOX': 3, 'BS_RADIOBUTTON': 4,
	'BS_3STATE': 5, 'BS_AUTO3STATE': 6, 'BS_GROUPBOX': 7, 'BS_USERBUTTON': 8, 'BS_AUTORADIOBUTTON': 9,
	'BS_OWNERDRAW': 0xB, 'BS_LEFTTEXT': 0x20, 'BS_TEXT': 0, 'BS_ICON': 0x40, 'BS_BITMAP': 0x80, 'BS_LEFT': 0x100,
	'BS_RIGHT': 0x200, 'BS_CENTER': 0x300, 'BS_TOP': 0x400, 'BS_BOTTOM': 0x800, 'BS_VCENTER': 0xC00,
	'BS_PUSHLIKE': 0x1000, 'BS_MULTILINE': 0x2000, 'BS_NOTIFY': 0x4000, 'BS_FLAT': 0x8000,
	# statics
	'SS_LEFT': 0, 'SS_CENTER': 1, 'SS_RIGHT': 2, 'SS_ICON': 3, 'SS_BLACKRECT': 4, 'SS_GRAYRECT': 5,
	'SS_WHITERECT': 6, 'SS_BLACKFRAME': 7, 'SS_GRAYFRAME': 8, 'SS_WHITEFRAME': 9, 'SS_SIMPLE': 0xB,
	'SS_LEFTNOWORDWRAP': 0xC, 'SS_OWNERDRAW': 0xD, 'SS_BITMAP': 0xE, 'SS_ETCHEDHORZ': 0x10, 'SS_ETCHEDVERT': 0x11,
	'SS_ETCHEDFRAME': 0x12, 'SS_NOPREFIX': 0x80, 'SS_NOTIFY': 0x100, 'SS_CENTERIMAGE': 0x200,
	'SS_RIGHTJUST': 0x400, 'SS_REALSIZEIMAGE': 0x800, 'SS_SUNKEN': 0x1000, 'SS_ENDELLIPSIS': 0x4000,
	'SS_PATHELLIPSIS': 0x8000, 'SS_WORDELLIPSIS': 0xC000,
	# edits
	'ES_LEFT': 0, 'ES_CENTER': 1, 'ES_RIGHT': 2, 'ES_MULTILINE': 4, 'ES_UPPERCASE': 8, 'ES_LOWERCASE': 0x10,
	'ES_PASSWORD': 0x20, 'ES_AUTOVSCROLL': 0x40, 'ES_AUTOHSCROLL': 0x80, 'ES_NOHIDESEL': 0x100,
	'ES_OEMCONVERT': 0x400, 'ES_READONLY': 0x800, 'ES_WANTRETURN': 0x1000, 'ES_NUMBER': 0x2000,
	# combo / list boxes
	'CBS_SIMPLE': 1, 'CBS_DROPDOWN': 2, 'CBS_DROPDOWNLIST': 3, 'CBS_OWNERDRAWFIXED': 0x10,
	'CBS_OWNERDRAWVARIABLE': 0x20, 'CBS_AUTOHSCROLL': 0x40, 'CBS_OEMCONVERT': 0x80, 'CBS_SORT': 0x100,
	'CBS_HASSTRINGS': 0x200, 'CBS_NOINTEGRALHEIGHT': 0x400, 'CBS_DISABLENOSCROLL': 0x800,
	'LBS_NOTIFY': 1, 'LBS_SORT': 2, 'LBS_NOREDRAW': 4, 'LBS_MULTIPLESEL': 8, 'LBS_OWNERDRAWFIXED': 0x10,
	'LBS_OWNERDRAWVARIABLE': 0x20, 'LBS_HASSTRINGS': 0x40, 'LBS_USETABSTOPS': 0x80,
	'LBS_NOINTEGRALHEIGHT': 0x100, 'LBS_MULTICOLUMN': 0x200, 'LBS_WANTKEYBOARDINPUT': 0x400,
	'LBS_EXTENDEDSEL': 0x800, 'LBS_DISABLENOSCROLL': 0x1000, 'LBS_NODATA': 0x2000, 'LBS_NOSEL': 0x4000,
	'LBS_STANDARD': 0xA00003,
	# scroll bars, trackbars, up-downs, list views
	'SBS_HORZ': 0, 'SBS_VERT': 1, 'TBS_AUTOTICKS': 1, 'TBS_VERT': 2, 'TBS_HORZ': 0, 'TBS_TOP': 4,
	'TBS_BOTTOM': 0, 'TBS_LEFT': 4, 'TBS_RIGHT': 0, 'TBS_BOTH': 8, 'TBS_NOTICKS': 0x10,
	'TBS_ENABLESELRANGE': 0x20, 'TBS_FIXEDLENGTH': 0x40, 'TBS_NOTHUMB': 0x80, 'TBS_TOOLTIPS': 0x100,
	'UDS_WRAP': 1, 'UDS_SETBUDDYINT': 2, 'UDS_ALIGNRIGHT': 4, 'UDS_ALIGNLEFT': 8, 'UDS_AUTOBUDDY': 0x10,
	'UDS_ARROWKEYS': 0x20, 'UDS_HORZ': 0x40, 'UDS_NOTHOUSANDS': 0x80,
	'LVS_ICON': 0, 'LVS_REPORT': 1, 'LVS_SMALLICON': 2, 'LVS_LIST': 3, 'LVS_SINGLESEL': 4,
	'LVS_SHOWSELALWAYS': 8, 'LVS_SORTASCENDING': 0x10, 'LVS_SORTDESCENDING': 0x20, 'LVS_SHAREIMAGELISTS': 0x40,
	'LVS_NOLABELWRAP': 0x80, 'LVS_AUTOARRANGE': 0x100, 'LVS_EDITLABELS': 0x200, 'LVS_NOSCROLL': 0x2000,
	'LVS_ALIGNLEFT': 0x800, 'LVS_NOCOLUMNHEADER': 0x4000, 'LVS_NOSORTHEADER': 0x8000,
	'IDOK': 1, 'IDCANCEL': 2, 'IDABORT': 3, 'IDRETRY': 4, 'IDIGNORE': 5, 'IDYES': 6, 'IDNO': 7, 'IDC_STATIC': -1,
	'VK_BACK': 8, 'VK_TAB': 9, 'VK_RETURN': 0xD, 'VK_SHIFT': 0x10, 'VK_CONTROL': 0x11, 'VK_MENU': 0x12,
	'VK_PAUSE': 0x13, 'VK_ESCAPE': 0x1B, 'VK_SPACE': 0x20, 'VK_PRIOR': 0x21, 'VK_NEXT': 0x22, 'VK_END': 0x23,
	'VK_HOME': 0x24, 'VK_LEFT': 0x25, 'VK_UP': 0x26, 'VK_RIGHT': 0x27, 'VK_DOWN': 0x28, 'VK_INSERT': 0x2D,
	'VK_DELETE': 0x2E, 'VK_ADD': 0x6B, 'VK_SUBTRACT': 0x6D, 'VK_OEM_PLUS': 0xBB, 'VK_OEM_MINUS': 0xBD,
	**{'VK_F%d' % i: 0x6F + i for i in range(1, 25)},
	**{'VK_NUMPAD%d' % i: 0x60 + i for i in range(10)},
}
CLASSES = {'BUTTON': 'Button', 'STATIC': 'Static', 'EDIT': 'Edit', 'COMBOBOX': 'ComboBox', 'LISTBOX': 'ListBox',
           'SCROLLBAR': 'ScrollBar'}

# rc.exe's implied class and style for each control statement
# (keyword: class, default style, whether the statement has a text argument)
STATEMENTS = {
	'LTEXT': ('Static', 0x00020000 | 0, True), 'RTEXT': ('Static', 0x00020000 | 2, True),
	'CTEXT': ('Static', 0x00020000 | 1, True), 'ICON': ('Static', 3, True),
	'PUSHBUTTON': ('Button', 0x00010000 | 0, True), 'DEFPUSHBUTTON': ('Button', 0x00010000 | 1, True),
	'CHECKBOX': ('Button', 0x00010000 | 2, True), 'AUTOCHECKBOX': ('Button', 0x00010000 | 3, True),
	'RADIOBUTTON': ('Button', 4, True), 'AUTORADIOBUTTON': ('Button', 9, True),
	'STATE3': ('Button', 0x00010000 | 5, True), 'AUTO3STATE': ('Button', 0x00010000 | 6, True),
	'GROUPBOX': ('Button', 7, True), 'PUSHBOX': ('Button', 0x00010000 | 0xA, True),
	'EDITTEXT': ('Edit', 0x00010000 | 0x00800000, False), 'COMBOBOX': ('ComboBox', 0x00010000 | 1, False),
	'LISTBOX': ('ListBox', 0x00800000 | 1, False), 'SCROLLBAR': ('ScrollBar', 0, False),
}


class Parser:
	def __init__(self, text, symbols):
		self.sym = symbols
		self.toks = self.tokenize(self.preprocess(text))
		self.i = 0

	@staticmethod
	def preprocess(text):
		"""#if/#ifdef/#ifndef/#else/#endif with every macro undefined (as when
		rc.exe runs outside the resource editor); other directives dropped."""
		out, stack = [], [True]
		for line in text.split('\n'):
			s = line.strip()
			if s.startswith('#'):
				d = s[1:].strip()
				if d.startswith('ifdef'):
					stack.append(False)
				elif d.startswith('ifndef'):
					stack.append(stack[-1])
				elif d.startswith('if'):
					e = re.sub(r'defined\s*\(\s*\w+\s*\)', '0', d[2:])
					e = e.replace('||', ' or ').replace('&&', ' and ').replace('!', ' not ')
					try:
						v = bool(eval(e, {}))
					except Exception:
						v = False
					stack.append(stack[-1] and v)
				elif d.startswith('else'):
					stack[-1] = not stack[-1] and all(stack[:-1])
				elif d.startswith('endif'):
					stack.pop()
				out.append('')
				continue
			out.append(line if all(stack) else '')
		return '\n'.join(out)

	@staticmethod
	def tokenize(text):
		toks = []
		i, n = 0, len(text)
		while i < n:
			c = text[i]
			if c in ' \t\r\n':
				i += 1
			elif text.startswith('//', i):
				i = text.find('\n', i)
				i = n if i < 0 else i
			elif text.startswith('/*', i):
				i = text.find('*/', i) + 2
			elif c == '"':
				j, s = i + 1, ''
				while True:
					if text[j] == '"':
						if j + 1 < n and text[j + 1] == '"':
							s += '"'; j += 2; continue
						break
					if text[j] == '\\' and j + 1 < n:
						s += {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '0': '\0', 'a': '\a'}.get(text[j + 1], '\\' + text[j + 1])
						j += 2; continue
					s += text[j]; j += 1
				toks.append(('str', s))
				i = j + 1
			elif c.isdigit():
				m = re.match(r'0[xX][0-9a-fA-F]+|\d+', text[i:])
				toks.append(('num', int(m.group(0), 0)))
				i += len(m.group(0))
				while i < n and text[i] in 'lLuU':
					i += 1
			elif c.isalpha() or c == '_':
				m = re.match(r'\w+', text[i:])
				toks.append(('id', m.group(0)))
				i += len(m.group(0))
			else:
				toks.append(('op', c))
				i += 1
		return toks

	def peek(self, k=0):
		return self.toks[self.i + k] if self.i + k < len(self.toks) else ('eof', None)

	def next(self):
		t = self.peek()
		self.i += 1
		return t

	def accept(self, kind, val=None):
		t = self.peek()
		if t[0] == kind and (val is None or t[1] == val):
			self.i += 1
			return True
		return False

	def value(self, name):
		if name in self.sym:
			return self.sym[name]
		if name in WIN32:
			return WIN32[name]
		raise SyntaxError('unknown symbol %s' % name)

	def expr(self):
		"""number/symbol terms joined by | + - (and NOT x, which clears bits)"""
		v, clear = 0, 0
		op = '|'
		while True:
			neg = False
			if self.accept('id', 'NOT'):
				neg = True
			paren = self.accept('op', '(')
			t = self.next()
			if t[0] == 'op' and t[1] == '-':
				t = self.next(); x = -(t[1] if t[0] == 'num' else self.value(t[1]))
			else:
				x = t[1] if t[0] == 'num' else self.value(t[1])
			if paren:
				self.accept('op', ')')
			if neg:
				clear |= x
			elif op == '|':
				v |= x
			elif op == '+':
				v += x
			elif op == '-':
				v -= x
			if self.peek()[0] == 'op' and self.peek()[1] in '|+-':
				op = self.next()[1]
				continue
			return v & ~clear if clear else v, clear

	def num(self):
		return self.expr()[0]

	def comma(self):
		return self.accept('op', ',')

	def block_begin(self):
		if not (self.accept('id', 'BEGIN') or self.accept('op', '{')):
			raise SyntaxError('expected BEGIN near %s' % (self.toks[self.i:self.i + 5],))

	def at_end(self):
		return self.accept('id', 'END') or self.accept('op', '}')

	def skip_block(self):
		self.block_begin()
		depth = 1
		while depth:
			t = self.next()
			if t in (('id', 'BEGIN'), ('op', '{')):
				depth += 1
			elif t in (('id', 'END'), ('op', '}')):
				depth -= 1

	def parse(self):
		menus, accels, dialogs, strings = [], [], [], []
		while self.peek()[0] != 'eof':
			t = self.next()
			if t == ('id', 'LANGUAGE'):
				self.next(); self.comma(); self.next(); continue
			if t == ('id', 'STRINGTABLE'):
				while self.peek()[0] == 'id' and self.peek()[1] not in ('BEGIN',):
					self.next()
				self.block_begin()
				while not self.at_end():
					sid = self.num(); self.comma()
					strings.append((sid, self.next()[1]))
				continue
			name = t[1]
			kind = self.next()
			if kind[0] != 'id':
				raise SyntaxError('bad resource header near %s %s' % (t, kind))
			k = kind[1]
			if k in ('MENU', 'MENUEX'):
				while self.peek()[0] == 'id' and self.peek()[1] not in ('BEGIN',):
					self.next()
				menus.append((self.rid(name), self.menu_items()))
			elif k == 'ACCELERATORS':
				while self.peek()[0] == 'id' and self.peek()[1] not in ('BEGIN',):
					self.next()
				accels.append((self.rid(name), self.accel_items()))
			elif k in ('DIALOG', 'DIALOGEX'):
				dialogs.append(self.dialog(self.rid(name), k == 'DIALOGEX'))
			elif k in ('ICON', 'BITMAP', 'CURSOR', 'RCDATA', 'HTML', 'PNG') and self.peek()[0] == 'str':
				self.next()
			elif k in ('TEXTINCLUDE', 'DESIGNINFO', 'GUIDELINES', 'AFX_DIALOG_LAYOUT', 'DLGINIT', 'RCDATA',
			           'VERSIONINFO', 'TOOLBAR'):
				while self.peek() not in (('id', 'BEGIN'), ('op', '{')):
					self.next()
				self.skip_block()
			else:
				raise SyntaxError('unsupported resource %s %s' % (name, k))
		return menus, accels, dialogs, strings

	def rid(self, name):
		return self.value(name) if not isinstance(name, int) else name

	def menu_items(self):
		self.block_begin()
		items = []
		while not self.at_end():
			t = self.next()
			if t == ('id', 'POPUP'):
				text = self.next()[1]
				flags = self.menu_flags()
				items.append((text, 0, flags | 0x10, self.menu_items()))
			elif t == ('id', 'MENUITEM'):
				if self.accept('id', 'SEPARATOR'):
					items.append(('', 0, 0x800, []))
					continue
				text = self.next()[1]
				self.comma()
				mid = self.num()
				items.append((text, mid, self.menu_flags(), []))
			else:
				raise SyntaxError('bad menu item %s' % (t,))
		return items

	def menu_flags(self):
		f = 0
		while self.comma() or self.peek()[0] == 'id' and self.peek()[1] in ('CHECKED', 'GRAYED', 'INACTIVE',
		                                                                   'MENUBARBREAK', 'MENUBREAK', 'HELP'):
			t = self.next()
			f |= {'CHECKED': 8, 'GRAYED': 1, 'INACTIVE': 2, 'MENUBARBREAK': 0x20, 'MENUBREAK': 0x40, 'HELP': 0x4000}.get(t[1], 0)
		return f

	def accel_items(self):
		"""(key, flags, command): flags are FVIRTKEY 1, FSHIFT 4, FCONTROL 8, FALT 0x10;
		a "^X" string is the ASCII control character."""
		self.block_begin()
		out = []
		while not self.at_end():
			t = self.next()
			if t[0] == 'str':
				key = ord(t[1][1].upper()) - 64 if t[1].startswith('^') else ord(t[1][0])
			else:
				key = t[1] if t[0] == 'num' else self.value(t[1])
			self.comma()
			cmd = self.num()
			flags = 0
			while self.comma():
				flags |= {'VIRTKEY': 1, 'ASCII': 0, 'NOINVERT': 2, 'SHIFT': 4, 'CONTROL': 8, 'ALT': 0x10}[self.next()[1]]
			out.append((key, flags, cmd))
		return out

	def dialog(self, did, ex):
		x = self.num(); self.comma(); y = self.num(); self.comma()
		cx = self.num(); self.comma(); cy = self.num()
		if self.comma():
			self.num()	# help id
		d = {'id': did, 'x': x, 'y': y, 'cx': cx, 'cy': cy, 'style': 0x80000000 | 0x00C00000 | 0x00080000,
		     'exstyle': 0, 'caption': '', 'font': 'MS Shell Dlg', 'size': 8, 'menu': 0, 'controls': []}
		explicit_style = False
		while not (self.peek() in (('id', 'BEGIN'), ('op', '{'))):
			t = self.next()[1]
			if t == 'STYLE':
				d['style'] = self.num(); explicit_style = True
			elif t == 'EXSTYLE':
				d['exstyle'] = self.num()
			elif t == 'CAPTION':
				d['caption'] = self.next()[1]
			elif t == 'FONT':
				d['size'] = self.num(); self.comma(); d['font'] = self.next()[1]
				while self.comma():
					self.num()
			elif t == 'MENU':
				m = self.next(); d['menu'] = m[1] if m[0] == 'num' else self.value(m[1])
			elif t == 'CLASS':
				self.next()
			elif t in ('LANGUAGE', 'CHARACTERISTICS', 'VERSION'):
				self.num()
				if self.comma():
					self.num()
			else:
				raise SyntaxError('dialog %s: unknown statement %s' % (did, t))
		self.block_begin()
		while not self.at_end():
			d['controls'].append(self.control(ex))
		return d

	def control(self, ex):
		kw = self.next()[1]
		text = ''
		if kw == 'CONTROL':
			t = self.next()
			text = t[1] if t[0] == 'str' else str(t[1] if t[0] == 'num' else self.value(t[1]))
			self.comma()
			cid = self.num(); self.comma()
			cls = self.next()[1]
			cls = CLASSES.get(cls.upper(), cls)
			self.comma()
			style, clear = self.expr(); self.comma()
			style |= 0x50000000	# WS_CHILD | WS_VISIBLE
			style &= ~clear
			x = self.num(); self.comma(); y = self.num(); self.comma()
			cx = self.num(); self.comma(); cy = self.num()
		else:
			cls, style, has_text = STATEMENTS[kw]
			if has_text:
				t = self.next()
				text = t[1] if t[0] == 'str' else str(t[1] if t[0] == 'num' else self.value(t[1]))
				self.comma()
			cid = self.num(); self.comma()
			x = self.num(); self.comma(); y = self.num()
			cx = cy = 0
			if self.comma():
				cx = self.num(); self.comma(); cy = self.num()
			style |= 0x50000000
			if self.comma():
				s, clear = self.expr()
				style = (style | s) & ~clear
		exstyle = 0
		if self.comma():
			exstyle = self.num()
			if self.comma():
				self.num()	# help id
		return {'cls': cls, 'text': text, 'id': cid, 'x': x, 'y': y, 'cx': cx, 'cy': cy, 'style': style & 0xFFFFFFFF,
		        'exstyle': exstyle}


def read_symbols(path):
	sym = {}
	for line in open(path, encoding='cp1252', errors='replace'):
		m = re.match(r'\s*#\s*define\s+(\w+)\s+(.+?)\s*(//.*)?$', line)
		if not m:
			continue
		v = m.group(2).strip().rstrip('L').strip('()')
		try:
			sym[m.group(1)] = int(v, 0)
		except ValueError:
			if v in sym:
				sym[m.group(1)] = sym[v]
	return sym


def cstr(s):
	out = 'L"'
	for ch in s:
		o = ord(ch)
		if ch == '"':
			out += '\\"'
		elif ch == '\\':
			out += '\\\\'
		elif ch == '\n':
			out += '\\n'
		elif ch == '\t':
			out += '\\t'
		elif ch == '\r':
			out += '\\r'
		elif 32 <= o < 127:
			out += ch
		else:
			out += '\\x%04X""' % o	# (the "" stops the next character being taken as a hex digit)
	return out + '"'


def main():
	rc, hdr, out = sys.argv[1:4]
	sym = read_symbols(hdr)
	text = open(rc, encoding='cp1252', errors='replace').read()
	menus, accels, dialogs, strings = Parser(text, sym).parse()
	o = ['// generated by gui/rc2cpp.py from %s -- do not edit' % os.path.basename(rc), '#include "furb_rc.h"', '']
	n = [0]

	def menu_arr(items):
		n[0] += 1
		name = 'menu_%d' % n[0]
		rows = []
		for text, mid, flags, sub in items:
			child = menu_arr(sub) if sub else 'NULL'
			rows.append('\t{%s, %d, 0x%X, %d, %s},' % (cstr(text), mid, flags, len(sub), child))
		o.append('static const FurbRcMenuItem %s[] = {\n%s\n};' % (name, '\n'.join(rows)))
		return name

	menu_rows = ['\t{%d, %d, %s},' % (mid, len(items), menu_arr(items)) for mid, items in menus]
	acc_rows = []
	for i, (aid, items) in enumerate(accels):
		o.append('static const FurbRcAccel acc_%d[] = {\n%s\n};' % (i, '\n'.join(
			'\t{0x%X, 0x%X, %d},' % (k, f, c) for k, f, c in items)))
		acc_rows.append('\t{%d, %d, acc_%d},' % (aid, len(items), i))
	dlg_rows = []
	for i, d in enumerate(dialogs):
		o.append('static const FurbRcControl dlg_%d[] = {\n%s\n};' % (i, '\n'.join(
			'\t{L"%s", %s, %d, %d, %d, %d, %d, 0x%X, 0x%X},' % (c['cls'], cstr(c['text']), c['id'], c['x'], c['y'],
			                                                   c['cx'], c['cy'], c['style'], c['exstyle'])
			for c in d['controls']) or '\t{NULL, NULL, 0, 0, 0, 0, 0, 0, 0},'))
		dlg_rows.append('\t{%d, %s, %d, %d, %d, %d, 0x%X, 0x%X, %s, %d, %d, %d, dlg_%d},' % (
			d['id'], cstr(d['caption']), d['x'], d['y'], d['cx'], d['cy'], d['style'] & 0xFFFFFFFF, d['exstyle'],
			cstr(d['font']), d['size'], d['menu'], len(d['controls']), i))
	o.append('static const FurbRcMenu menus[] = {\n%s\n\t{0, 0, NULL}\n};' % '\n'.join(menu_rows))
	o.append('static const FurbRcAccelTable accels[] = {\n%s\n\t{0, 0, NULL}\n};' % '\n'.join(acc_rows))
	o.append('static const FurbRcDialog dialogs[] = {\n%s\n\t{0}\n};' % '\n'.join(dlg_rows))
	o.append('static const FurbRcString strings[] = {\n%s\n\t{0, NULL}\n};' % '\n'.join(
		'\t{%d, %s},' % (sid, cstr(s)) for sid, s in strings))
	o.append('extern "C" const FurbResources furb_module_resources = {menus, %d, accels, %d, dialogs, %d, strings, %d};' % (
		len(menus), len(accels), len(dialogs), len(strings)))
	open(out, 'w').write('\n'.join(o) + '\n')


if __name__ == '__main__':
	main()
