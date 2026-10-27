# Fills in the plugin API surface that the embedded host module does not define itself.
# install(m) runs at the end of the API module, install_plugin(m) at the end of the plugin module.
# Everything here works on the host-side mirror of a view/window; operations that need the
# editor's renderer or index (popups, phantoms, folds, symbols) keep their state and return
# correctly typed results.
import enum, html, importlib, itertools, json, os, re, sys, threading, time


def _consts(ns, cls, names, prefix=''):
    for n in names:
        ns[prefix + n] = getattr(cls, n)


def _install_enums(m):
    ns = m.__dict__
    I, F = enum.IntEnum, enum.IntFlag
    table = [
        ('HoverZone', I, [('TEXT', 1), ('GUTTER', 2), ('MARGIN', 3)], 'HOVER_'),
        ('NewFileFlags', F, [('NONE', 0), ('ENCODED_POSITION', 1), ('TRANSIENT', 4), ('FORCE_GROUP', 8), ('SEMI_TRANSIENT', 16),
                             ('ADD_TO_SELECTION', 32), ('REPLACE_MRU', 64), ('CLEAR_TO_RIGHT', 128), ('FORCE_CLONE', 256)], ''),
        ('FindFlags', F, [('NONE', 0), ('LITERAL', 1), ('IGNORECASE', 2), ('WHOLEWORD', 4), ('REVERSE', 8), ('WRAP', 16)], ''),
        ('QuickPanelFlags', F, [('NONE', 0), ('MONOSPACE_FONT', 1), ('KEEP_OPEN_ON_FOCUS_LOST', 2), ('WANT_EVENT', 4)], ''),
        ('PopupFlags', F, [('NONE', 0), ('COOPERATE_WITH_AUTO_COMPLETE', 2), ('HIDE_ON_MOUSE_MOVE', 4), ('HIDE_ON_MOUSE_MOVE_AWAY', 8),
                           ('KEEP_ON_SELECTION_MODIFIED', 16), ('HIDE_ON_CHARACTER_EVENT', 32)], ''),
        ('RegionFlags', F, [('NONE', 0), ('DRAW_EMPTY', 1), ('HIDE_ON_MINIMAP', 2), ('DRAW_EMPTY_AS_OVERWRITE', 4), ('PERSISTENT', 16),
                            ('DRAW_NO_FILL', 32), ('HIDDEN', 128), ('DRAW_NO_OUTLINE', 256), ('DRAW_SOLID_UNDERLINE', 512),
                            ('DRAW_STIPPLED_UNDERLINE', 1024), ('DRAW_SQUIGGLY_UNDERLINE', 2048), ('NO_UNDO', 8192)], ''),
        ('QueryOperator', I, [('EQUAL', 0), ('NOT_EQUAL', 1), ('REGEX_MATCH', 2), ('NOT_REGEX_MATCH', 3), ('REGEX_CONTAINS', 4),
                              ('NOT_REGEX_CONTAINS', 5)], 'OP_'),
        ('PointClassification', F, [('NONE', 0), ('WORD_START', 1), ('WORD_END', 2), ('PUNCTUATION_START', 4), ('PUNCTUATION_END', 8),
                                    ('SUB_WORD_START', 16), ('SUB_WORD_END', 32), ('LINE_START', 64), ('LINE_END', 128),
                                    ('EMPTY_LINE', 256)], 'CLASS_'),
        ('AutoCompleteFlags', F, [('NONE', 0), ('INHIBIT_WORD_COMPLETIONS', 8), ('INHIBIT_EXPLICIT_COMPLETIONS', 16),
                                  ('DYNAMIC_COMPLETIONS', 32), ('INHIBIT_REORDER', 128)], ''),
        ('CompletionItemFlags', F, [('NONE', 0), ('KEEP_PREFIX', 1)], 'COMPLETION_FLAG_'),
        ('DialogResult', I, [('CANCEL', 0), ('YES', 1), ('NO', 2)], 'DIALOG_'),
        ('UIElement', I, [('SIDE_BAR', 1), ('MINIMAP', 2), ('TABS', 4), ('STATUS_BAR', 8), ('MENU', 16), ('OPEN_FILES', 32)], None),
        ('PhantomLayout', I, [('INLINE', 0), ('BELOW', 1), ('BLOCK', 2)], 'LAYOUT_'),
        ('KindId', I, [('AMBIGUOUS', 0), ('KEYWORD', 1), ('TYPE', 2), ('FUNCTION', 3), ('NAMESPACE', 4), ('NAVIGATION', 5), ('MARKUP', 6),
                       ('VARIABLE', 7), ('SNIPPET', 8), ('COLOR_REDISH', 9), ('COLOR_ORANGISH', 10), ('COLOR_YELLOWISH', 11),
                       ('COLOR_GREENISH', 12), ('COLOR_CYANISH', 13), ('COLOR_BLUISH', 14), ('COLOR_PURPLISH', 15),
                       ('COLOR_PINKISH', 16), ('COLOR_DARK', 17), ('COLOR_LIGHT', 18)], 'KIND_ID_'),
        ('SymbolSource', I, [('ANY', 0), ('INDEX', 1), ('OPEN_FILES', 2)], 'SYMBOL_SOURCE_'),
        ('SymbolType', I, [('ANY', 0), ('DEFINITION', 1), ('REFERENCE', 2)], 'SYMBOL_TYPE_'),
        ('CompletionFormat', I, [('TEXT', 0), ('SNIPPET', 1), ('COMMAND', 2)], 'COMPLETION_FORMAT_'),
    ]
    for name, base, members, prefix in table:
        cls = base(name, members)
        ns[name] = cls
        if prefix is None:
            continue
        for member in cls:
            if member.name == 'NONE':
                continue
            ns[prefix + member.name] = member
    for cls_name in ('NewFileFlags', 'QuickPanelFlags', 'PopupFlags', 'RegionFlags', 'AutoCompleteFlags'):
        for member in ns[cls_name]:
            if member.name != 'NONE' and member.name not in ns:
                ns[member.name] = member
    ns['TRANSIENT'] = ns['NewFileFlags'].TRANSIENT
    ns['PERSISTENT'] = ns['RegionFlags'].PERSISTENT
    ns['HIDDEN'] = ns['RegionFlags'].HIDDEN
    ns['KEEP_OPEN_ON_FOCUS_LOST'] = ns['QuickPanelFlags'].KEEP_OPEN_ON_FOCUS_LOST
    ns['DRAW_OUTLINED'] = ns['RegionFlags'].DRAW_NO_FILL
    ns['HTML'] = 1
    for name in ('AMBIGUOUS', 'KEYWORD', 'TYPE', 'FUNCTION', 'NAMESPACE', 'NAVIGATION', 'MARKUP', 'VARIABLE'):
        ns['KIND_' + name] = (ns['KindId'][name], '', '')
    ns['KIND_SNIPPET'] = (ns['KindId'].SNIPPET, 's', 'Snippet')


class Edit:
    def __init__(self, token): self.edit_token = token
    def __repr__(self): return 'Edit(%r)' % (self.edit_token,)


class Region:
    __slots__ = ['a', 'b', 'xpos']

    def __init__(self, a=0, b=None, xpos=-1):
        if hasattr(a, 'a') and hasattr(a, 'b') and b is None:
            a, b, xpos = a.a, a.b, getattr(a, 'xpos', -1)
        self.a = int(a); self.b = self.a if b is None else int(b); self.xpos = xpos

    def __iter__(self): return iter((self.a, self.b))
    def __str__(self): return '(%d, %d)' % (self.a, self.b)
    def __repr__(self): return 'Region(%d, %d)' % (self.a, self.b)
    def __len__(self): return self.size()
    def __eq__(self, other): return isinstance(other, Region) and self.a == other.a and self.b == other.b
    def __ne__(self, other): return not self.__eq__(other)
    def __lt__(self, other): return self.to_tuple() < other.to_tuple()
    def __le__(self, other): return self.to_tuple() <= other.to_tuple()
    def __gt__(self, other): return self.to_tuple() > other.to_tuple()
    def __ge__(self, other): return self.to_tuple() >= other.to_tuple()
    def __hash__(self): return hash((self.a, self.b))
    def __contains__(self, item): return self.contains(item)
    def to_tuple(self): return (self.begin(), self.end())
    def empty(self): return self.a == self.b
    def begin(self): return self.a if self.a < self.b else self.b
    def end(self): return self.a if self.a > self.b else self.b
    def size(self): return abs(self.a - self.b)
    def contains(self, x):
        if isinstance(x, Region) or (hasattr(x, 'begin') and hasattr(x, 'end')):
            return self.begin() <= x.begin() and self.end() >= x.end()
        return self.begin() <= int(x) <= self.end()
    def cover(self, region): return Region(min(self.begin(), region.begin()), max(self.end(), region.end()))
    def intersection(self, region): return Region(max(self.begin(), region.begin()), min(self.end(), region.end()))
    def intersects(self, region):
        return self == region or max(self.begin(), region.begin()) < min(self.end(), region.end())
    def cut(self, region):
        if self.intersects(region):
            out = []
            if self.begin() < region.begin(): out.append(Region(self.begin(), region.begin()))
            if self.end() > region.end(): out.append(Region(region.end(), self.end()))
            return out
        return [Region(self.a, self.b)]


class Selection:
    def __init__(self, view=None): self.view = view; self.regions = []
    def is_valid(self): return True
    def clear(self): self.regions.clear()
    def add(self, region):
        r = region if isinstance(region, Region) else Region(region)
        if r not in self.regions: self.regions.append(r)
        self.regions.sort(key=lambda x: x.begin())
    def add_all(self, regions):
        for r in regions: self.add(r)
    def subtract(self, region):
        out = []
        for r in self.regions:
            out.extend(r.cut(region) if r.intersects(region) else [r])
        self.regions = out
    def contains(self, region):
        pt = region.begin() if isinstance(region, Region) else int(region)
        return any(r.contains(pt) for r in self.regions)
    def has_empty_region(self): return any(r.empty() for r in self.regions)
    def has_non_empty_region(self): return any(not r.empty() for r in self.regions)
    def has_multiple_non_empty_regions(self): return sum(1 for r in self.regions if not r.empty()) > 1
    def __len__(self): return len(self.regions)
    def __getitem__(self, index): return self.regions[index]
    def __iter__(self): return iter(list(self.regions))
    def __repr__(self): return 'Selection(%r)' % (self.regions,)


class _Id(int):
    """The mirror stores ids as plain ints; plugins call view.id() / window.id()."""
    def __call__(self): return int(self)


class Syntax:
    def __init__(self, path='', name='', hidden=False, scope=''):
        self.path = path; self.name = name; self.hidden = hidden; self.scope = scope
    def __eq__(self, other): return isinstance(other, Syntax) and self.path == other.path
    def __hash__(self): return hash(self.path)
    def __repr__(self): return 'Syntax(%r, %r)' % (self.path, self.name)


class Html:
    def __init__(self, data): self.data = data
    def __str__(self): return str(self.data)


class HistoricPosition:
    def __init__(self, pt, row, col, col_utf16, col_utf8):
        self.pt = pt; self.row = row; self.col = col; self.col_utf16 = col_utf16; self.col_utf8 = col_utf8


class TextChange:
    def __init__(self, pa, pb, len_utf16, len_utf8, s):
        self.a = pa; self.b = pb; self.len_utf16 = len_utf16; self.len_utf8 = len_utf8; self.str = s


class ContextStackFrame:
    def __init__(self, context_name, source_file, source_location):
        self.context_name = context_name; self.source_file = source_file; self.source_location = source_location


class Phantom:
    def __init__(self, region, content, layout, on_navigate=None):
        self.region = region; self.content = content; self.layout = layout; self.on_navigate = on_navigate; self.id = None
    def __eq__(self, other):
        return isinstance(other, Phantom) and self.region == other.region and self.content == other.content and self.layout == other.layout
    def __hash__(self): return hash((self.region.begin(), self.region.end(), self.content, int(self.layout)))
    def to_tuple(self): return (self.region.to_tuple(), self.content, self.layout, self.on_navigate)


class PhantomSet:
    def __init__(self, view, key=''): self.view = view; self.key = key; self.phantoms = []
    def __del__(self):
        try: self.view.erase_phantoms(self.key)
        except Exception: pass
    def update(self, phantoms):
        self.view.erase_phantoms(self.key)
        self.phantoms = list(phantoms)
        for p in self.phantoms:
            p.id = self.view.add_phantom(self.key, p.region, p.content, p.layout, p.on_navigate)


class ListInputItem:
    def __init__(self, text, value, details='', annotation='', kind=(0, '', '')):
        self.text = text; self.value = value; self.details = details; self.annotation = annotation; self.kind = kind


class SymbolRegion:
    def __init__(self, name, region, syntax, type, kind):
        self.name = name; self.region = region; self.syntax = syntax; self.type = type; self.kind = kind


class SymbolLocation:
    def __init__(self, path, display_name, row, col, syntax, type, kind):
        self.path = path; self.display_name = display_name; self.row = row; self.col = col
        self.syntax = syntax; self.type = type; self.kind = kind
    def path_encoded_position(self): return '%s:%d:%d' % (self.path, self.row, self.col)


class Buffer:
    def __init__(self, view): self._view = view
    def id(self): return int(self._view.id)
    def file_name(self): return self._view.file_name()
    def views(self): return [self._view]
    def primary_view(self): return self._view


class Sheet:
    def __init__(self, view): self._view = view
    def id(self): return int(self._view.id)
    def window(self): return self._view.window_obj
    def view(self): return self._view
    def file_name(self): return self._view.file_name()
    def is_semi_transient(self): return False
    def is_transient(self): return False
    def is_selected(self): return self in (self._view.window_obj.selected_sheets() if self._view.window_obj else [])
    def group(self): return getattr(self._view, '_group', 0)
    def close(self, on_close=lambda did_close: None):
        ok = self._view.close(); on_close(ok); return ok
    def __eq__(self, other): return isinstance(other, Sheet) and other._view is self._view
    def __hash__(self): return hash(id(self._view))


class TextSheet(Sheet):
    def set_name(self, name): self._view.set_name(name)


class ImageSheet(Sheet):
    pass


class HtmlSheet(Sheet):
    def set_name(self, name): self._view.set_name(name)
    def set_contents(self, contents): self._view.text = str(contents); self._view._emit(False)


def _sheet_for(view):
    sheet = getattr(view, '_sheet', None)
    if sheet is None:
        sheet = view._sheet = TextSheet(view)
    return sheet


_IS_WORD = lambda c: c.isalnum() or c == '_'
_IS_PUNCT = lambda c: not c.isspace() and not _IS_WORD(c)


def _pt(x):
    return x.begin() if hasattr(x, 'begin') else int(x)


def _utf16_len(s): return len(s.encode('utf-16-le')) // 2


def _view_methods(m):
    R = m.Region
    FindFlags = m.FindFlags
    PC = m.PointClassification

    def _lines(self):
        return self.text.split('\n')

    def _line_start(self, row):
        pos = 0
        for i, line in enumerate(_lines(self)):
            if i == row: return pos
            pos += len(line) + 1
        return len(self.text)

    def buffer_id(self): return int(self.__dict__['id'])
    def buffer(self): return Buffer(self)
    def sheet_id(self): return int(self.__dict__['id'])
    def sheet(self): return _sheet_for(self)
    def element(self): return None
    def is_primary(self): return True
    def window(self): return self.window_obj
    def clones(self): return [self]
    def is_loading(self): return False
    def is_read_only(self): return bool(self._read_only)
    def is_scratch(self): return bool(self._settings.get('scratch', False))
    def encoding(self): return self.__dict__.get('_encoding', 'UTF-8')
    def set_encoding(self, name): self.__dict__['_encoding'] = str(name)
    def line_endings(self):
        return self.__dict__.get('_line_endings') or ('Windows' if '\r\n' in self.text else 'Unix')
    def set_line_endings(self, name): self.__dict__['_line_endings'] = str(name)
    def close(self, on_close=lambda did_close: None):
        ok = m._invalidate_view_id(int(self.__dict__['id']))
        on_close(ok); return ok
    def reset_reference_document(self): self.__dict__['_reference'] = None
    def set_reference_document(self, reference): self.__dict__['_reference'] = reference
    def begin_edit(self, edit_token=0, cmd='', args=None): self.__dict__['_in_edit'] = True; return Edit(edit_token)
    def end_edit(self, edit): self.__dict__['_in_edit'] = False
    def is_in_edit(self): return bool(self.__dict__.get('_in_edit', False))
    def change_count(self): return self.__dict__.get('_changes', 0)
    def change_id(self): return (int(self.__dict__['id']), self.change_count(), 0)
    def transform_region_from(self, region, change_id): return region

    def erase(self, edit, region):
        region = region if isinstance(region, R) else R(region, region + 1)
        self.text = self.text[:region.begin()] + self.text[region.end():]
        self._dirty = True; self.__dict__['_changes'] = self.change_count() + 1
        self._set_cursor(region.begin()); self._emit(False)

    def _compile(pattern, flags):
        f = re.MULTILINE
        if flags & FindFlags.LITERAL: pattern = re.escape(pattern)
        if flags & FindFlags.WHOLEWORD: pattern = r'\b(?:' + pattern + r')\b'
        if flags & FindFlags.IGNORECASE: f |= re.IGNORECASE
        return re.compile(pattern, f)

    def find(self, pattern, start_pt, flags=0):
        try: rx = _compile(pattern, int(flags))
        except re.error: return R(-1, -1)
        start_pt = max(0, int(start_pt))
        if int(flags) & FindFlags.REVERSE:
            found = [x for x in rx.finditer(self.text) if x.end() <= start_pt]
            if not found and int(flags) & FindFlags.WRAP: found = list(rx.finditer(self.text))
            return R(found[-1].start(), found[-1].end()) if found else R(-1, -1)
        hit = rx.search(self.text, start_pt)
        if not hit and int(flags) & FindFlags.WRAP: hit = rx.search(self.text)
        return R(hit.start(), hit.end()) if hit else R(-1, -1)

    def find_all(self, pattern, flags=0, fmt=None, extractions=None, within=None):
        try: rx = _compile(pattern, int(flags))
        except re.error: return []
        bounds = None if within is None else ([within] if isinstance(within, R) else list(within))
        out = []
        for hit in rx.finditer(self.text):
            if bounds is not None and not any(b.begin() <= hit.start() and hit.end() <= b.end() for b in bounds): continue
            out.append(R(hit.start(), hit.end()))
            if fmt is not None and extractions is not None:
                extractions.append(hit.expand(re.sub(r'\$(\d+)', r'\\\1', fmt)))
        return out

    def meta_info(self, key, pt): return None
    def extract_tokens_with_scopes(self, region): return [(region, scope_name(self, region.begin()))]
    def scope_name(self, pt):
        syntax = self._settings.get('syntax') or ''
        base = os.path.splitext(os.path.basename(str(syntax)))[0].lower()
        return ('source.' + base if base and base != 'plain text' else 'text.plain') + ' '
    def extract_scope(self, pt):
        pt = _pt(pt); w = word(self, pt)
        return w if not w.empty() else R(pt, min(pt + 1, len(self.text)))
    def expand_to_scope(self, pt, selector): return None
    def context_backtrace(self, pt): return []
    def match_selector(self, pt, selector): return m.score_selector(scope_name(self, pt), selector) > 0
    def score_selector(self, pt, selector): return m.score_selector(scope_name(self, pt), selector)
    def find_by_selector(self, selector): return []
    def style(self): return {}
    def style_for_scope(self, scope): return {}

    def _indent_of(self, row):
        text = _lines(self)[row] if row < len(_lines(self)) else ''
        width = len(text) - len(text.lstrip(' \t'))
        return text[:width].count('\t') + text[:width].count(' ') // max(1, int(self._settings.get('tab_size', 4)))

    def indentation_level(self, pt): return _indent_of(self, rowcol(self, _pt(pt))[0])

    def indented_region(self, pt):
        rows = _lines(self); row = rowcol(self, _pt(pt))[0]; level = _indent_of(self, row)
        lo = hi = row
        while lo > 0 and (not rows[lo - 1].strip() or _indent_of(self, lo - 1) >= level): lo -= 1
        while hi + 1 < len(rows) and (not rows[hi + 1].strip() or _indent_of(self, hi + 1) >= level): hi += 1
        return R(_line_start(self, lo), _line_start(self, hi) + len(rows[hi]))

    def has_non_empty_selection_region(self): return self._sel.has_non_empty_region()

    def lines(self, region):
        out = []; pos = line(self, region.begin()).begin()
        while True:
            ln = line(self, pos); out.append(ln)
            if ln.end() >= region.end() or ln.end() >= len(self.text): break
            pos = ln.end() + 1
        return out

    def split_by_newlines(self, region):
        out = []; pos = region.begin()
        for part in self.text[region.begin():region.end()].split('\n'):
            out.append(R(pos, pos + len(part))); pos += len(part) + 1
        return out

    def line(self, x):
        if isinstance(x, R):
            return R(line(self, x.begin()).begin(), line(self, x.end()).end())
        p = max(0, min(int(x), len(self.text))); s = self.text.rfind('\n', 0, p) + 1; e = self.text.find('\n', p)
        return R(s, len(self.text) if e < 0 else e)

    def full_line(self, x):
        ln = line(self, x)
        return R(ln.begin(), min(ln.end() + 1, len(self.text)))

    def word(self, x):
        if isinstance(x, R):
            return R(word(self, x.begin()).begin(), word(self, x.end()).end())
        p = max(0, min(int(x), len(self.text))); s = p; e = p
        while s > 0 and _IS_WORD(self.text[s - 1]): s -= 1
        while e < len(self.text) and _IS_WORD(self.text[e]): e += 1
        if s == e and p < len(self.text) and not self.text[p].isspace():
            return R(p, p + 1)
        return R(s, e)

    def classify(self, pt):
        t = self.text; n = len(t); p = max(0, min(int(pt), n))
        prev = t[p - 1] if p > 0 else '\n'; cur = t[p] if p < n else '\n'
        f = 0
        if prev == '\n': f |= PC.LINE_START
        if cur == '\n': f |= PC.LINE_END
        if prev == '\n' and cur == '\n': f |= PC.EMPTY_LINE
        if _IS_WORD(cur) and not _IS_WORD(prev): f |= PC.WORD_START
        if _IS_WORD(prev) and not _IS_WORD(cur): f |= PC.WORD_END
        if _IS_PUNCT(cur) and not _IS_PUNCT(prev): f |= PC.PUNCTUATION_START
        if _IS_PUNCT(prev) and not _IS_PUNCT(cur): f |= PC.PUNCTUATION_END
        if _IS_WORD(cur) and cur != '_' and (prev == '_' or not _IS_WORD(prev) or (prev.islower() and cur.isupper()) or prev.isdigit() != cur.isdigit()):
            f |= PC.SUB_WORD_START
        if _IS_WORD(prev) and prev != '_' and (cur == '_' or not _IS_WORD(cur) or (prev.islower() and cur.isupper()) or prev.isdigit() != cur.isdigit()):
            f |= PC.SUB_WORD_END
        return PC(f)

    def find_by_class(self, pt, forward, classes, separators='', sub_word_separators=''):
        n = len(self.text); p = int(pt); step = 1 if forward else -1
        p += step
        while 0 <= p <= n:
            if classify(self, p) & classes: return p
            if separators and 0 <= p < n and self.text[p] in separators: return p
            p += step
        return -1

    def expand_by_class(self, x, classes, separators='', sub_word_separators=''):
        r = x if isinstance(x, R) else R(int(x))
        s = r.begin(); e = r.end(); n = len(self.text)
        while s > 0 and not (classify(self, s) & classes) and not (separators and self.text[s - 1] in separators): s -= 1
        while e < n and not (classify(self, e) & classes) and not (separators and self.text[e] in separators): e += 1
        return R(s, e)

    def rowcol(self, tp):
        p = max(0, min(int(tp), len(self.text)))
        return (self.text.count('\n', 0, p), p - (self.text.rfind('\n', 0, p) + 1))

    def rowcol_utf8(self, tp):
        row, col = rowcol(self, tp); ln = _lines(self)[row]
        return (row, len(ln[:col].encode('utf-8')))

    def rowcol_utf16(self, tp):
        row, col = rowcol(self, tp); ln = _lines(self)[row]
        return (row, _utf16_len(ln[:col]))

    def text_point(self, row, col, *, clamp_column=False):
        rows = _lines(self)
        if row < 0: return 0
        if row >= len(rows): return len(self.text)
        return _line_start(self, row) + min(col, len(rows[row]))

    def _text_point_units(self, row, col, measure):
        rows = _lines(self)
        if row < 0: return 0
        if row >= len(rows): return len(self.text)
        used = 0; idx = 0
        for ch in rows[row]:
            if used >= col: break
            used += measure(ch); idx += 1
        return _line_start(self, row) + idx

    def text_point_utf8(self, row, col, *, clamp_column=False):
        return _text_point_units(self, row, col, lambda c: len(c.encode('utf-8')))

    def text_point_utf16(self, row, col, *, clamp_column=False):
        return _text_point_units(self, row, col, _utf16_len)

    def utf8_code_units(self, tp=None):
        return len((self.text if tp is None else self.text[:int(tp)]).encode('utf-8'))

    def utf16_code_units(self, tp=None):
        return _utf16_len(self.text if tp is None else self.text[:int(tp)])

    def visible_region(self): return R(0, len(self.text))
    def show(self, location, show_surrounds=True, keep_to_left=False, animate=True): return None
    def show_at_center(self, location, animate=True): return None
    def viewport_extent(self): return (1000.0, 800.0)
    def layout_extent(self): return (1000.0, len(_lines(self)) * line_height(self))
    def line_height(self): return 16.0
    def em_width(self): return 8.0
    def text_to_layout(self, tp):
        row, col = rowcol(self, tp); return (col * em_width(self), row * line_height(self))
    def layout_to_text(self, xy):
        return text_point(self, int(xy[1] // line_height(self)), int(xy[0] // em_width(self)))
    def text_to_window(self, tp): return text_to_layout(self, tp)
    def layout_to_window(self, xy): return (xy[0], xy[1])
    def window_to_layout(self, xy): return (xy[0], xy[1])
    def window_to_text(self, xy): return layout_to_text(self, xy)

    def _fold_list(self): return self.__dict__.setdefault('_folds', [])
    def is_folded(self, region): return any(f.contains(region) for f in _fold_list(self))
    def folded_regions(self): return list(_fold_list(self))
    def fold(self, x):
        items = [x] if isinstance(x, R) else list(x); folds = _fold_list(self); added = False
        for r in items:
            if r not in folds: folds.append(r); added = True
        return added
    def unfold(self, x):
        items = [x] if isinstance(x, R) else list(x); folds = _fold_list(self); gone = []
        for r in items:
            for f in list(folds):
                if f.intersects(r) or f == r: folds.remove(f); gone.append(f)
        return gone

    def add_regions(self, key, regions, scope='', icon='', flags=0, annotations=[], annotation_color='', on_navigate=None, on_close=None):
        self.__dict__.setdefault('_region_store', {})[str(key)] = [R(r) for r in (regions or [])]
        self._settings.set('regions_' + str(key), [repr(r) for r in (regions or [])])
    def get_regions(self, key): return list(self.__dict__.setdefault('_region_store', {}).get(str(key), []))
    def erase_regions(self, key):
        self.__dict__.setdefault('_region_store', {}).pop(str(key), None)
        self._settings.erase('regions_' + str(key))

    _phantom_ids = itertools.count(1)
    def add_phantom(self, key, region, content, layout, on_navigate=None):
        pid = next(_phantom_ids)
        self.__dict__.setdefault('_phantoms', {})[pid] = (str(key), R(region), content, layout, on_navigate)
        return pid
    def erase_phantoms(self, key):
        store = self.__dict__.setdefault('_phantoms', {})
        for pid in [p for p, v in store.items() if v[0] == str(key)]: del store[pid]
    def erase_phantom_by_id(self, pid): self.__dict__.setdefault('_phantoms', {}).pop(pid, None)
    def query_phantom(self, pid):
        v = self.__dict__.setdefault('_phantoms', {}).get(pid)
        return [v[1]] if v else []
    def query_phantoms(self, pids): return [r for p in pids for r in query_phantom(self, p)]

    def assign_syntax(self, syntax):
        self._settings.set('syntax', syntax.path if isinstance(syntax, Syntax) else syntax)
    def set_syntax_file(self, syntax_file): assign_syntax(self, syntax_file)
    def syntax(self):
        path = self._settings.get('syntax')
        return m.syntax_from_path(path) if path else None
    def symbols(self): return []
    def get_symbols(self): return []
    def indexed_symbols(self): return []
    def indexed_references(self): return []
    def symbol_regions(self): return []
    def indexed_symbol_regions(self, type=0): return []
    def get_status(self, key): return self._statuses.get(str(key), '')

    def extract_completions(self, prefix, tp=-1):
        words = sorted(set(re.findall(r'[A-Za-z_]\w*', self.text)))
        return [w for w in words if w.startswith(prefix) and w != prefix]
    def find_all_results(self): return []
    def find_all_results_with_text(self): return []
    def command_history(self, index, modifying_only=False): return ('', {}, 0)
    def overwrite_status(self): return bool(self.__dict__.get('_overwrite', False))
    def set_overwrite_status(self, value): self.__dict__['_overwrite'] = bool(value)

    def show_popup_menu(self, items, on_done, flags=0):
        on_done(-1)
    def show_popup(self, content, flags=0, location=-1, max_width=320, max_height=240, on_navigate=None, on_hide=None):
        self.__dict__['_popup'] = {'content': content, 'on_hide': on_hide, 'on_navigate': on_navigate}
    def update_popup(self, content):
        if self.__dict__.get('_popup'): self.__dict__['_popup']['content'] = content
    def is_popup_visible(self): return bool(self.__dict__.get('_popup'))
    def hide_popup(self):
        popup = self.__dict__.pop('_popup', None)
        if popup and popup.get('on_hide'): popup['on_hide']()
    def is_auto_complete_visible(self): return False
    def preserve_auto_complete_on_focus_lost(self): return None
    def export_to_html(self, regions=None, minihtml=False, enclosing_tags=False, font_size=True, font_family=True):
        return '<pre>' + html.escape(self.text) + '</pre>'
    def clear_undo_stack(self): return None

    table = dict(locals())
    for junk in ('m', 'R', 'FindFlags', 'PC', 'self'):
        table.pop(junk, None)
    return {k: v for k, v in table.items() if callable(v) and not k.startswith('_')}


def _install_view(m):
    methods = _view_methods(m)
    for name, fn in methods.items():
        setattr(m.View, name, fn)
    # View ids are stored as plain ints by the host mirror; make view.id() callable too.
    original_init = m.View.__init__

    def init(self, *args, **kwargs):
        original_init(self, *args, **kwargs)
        self.id = _Id(self.id)
    m.View.__init__ = init
    m.View.id = property(lambda self: self.__dict__['id'], lambda self, v: self.__dict__.__setitem__('id', _Id(v)))

    for name in ('insert', 'replace'):
        original = getattr(m.View, name)

        def bumped(self, edit, a, b, _o=original):
            _o(self, edit, a, b)
            self.__dict__['_changes'] = self.__dict__.get('_changes', 0) + 1
        setattr(m.View, name, bumped)


def _install_settings(m):
    S = m.Settings
    plain_set, plain_erase = S.set, S.erase

    def fire(self):
        for tag, cb in list(self.__dict__.get('_listeners', {}).items()):
            try: cb()
            except Exception as e: print('[settings-listener-error]', tag, e)

    def set_(self, key, value):
        changed = self.data.get(key, object()) != value
        plain_set(self, key, value)
        if changed: fire(self)

    def erase(self, key):
        had = key in self.data
        plain_erase(self, key)
        if had: fire(self)

    def setdefault(self, key, value):
        if not self.has(key): self.set(key, value)
        return self.get(key)

    def update(self, other=(), **kwargs):
        items = dict(other, **kwargs)
        for k, v in items.items(): plain_set(self, k, v)
        if items: fire(self)

    def add_on_change(self, tag, callback): self.__dict__.setdefault('_listeners', {})[tag] = callback
    def clear_on_change(self, tag): self.__dict__.setdefault('_listeners', {}).pop(tag, None)
    S.set = set_; S.erase = erase; S.setdefault = setdefault; S.update = update
    S.add_on_change = add_on_change; S.clear_on_change = clear_on_change
    S.__contains__ = lambda self, key: self.has(key)
    S.__getitem__ = lambda self, key: self.data[key]
    S.__setitem__ = lambda self, key, value: self.set(key, value)


def _install_completions(m):
    CompletionItem, CompletionList = m.CompletionItem, m.CompletionList

    def item_init(self, trigger, annotation='', completion='', completion_format=0, kind=(0, '', ''), details='', flags=0):
        self.trigger = trigger; self.annotation = annotation; self.completion = completion
        self.completion_format = completion_format; self.kind = kind; self.details = details; self.flags = flags

    @classmethod
    def snippet_completion(cls, trigger, snippet, annotation='', kind=None, details=''):
        return cls(trigger, annotation, snippet, m.CompletionFormat.SNIPPET, kind or m.KIND_SNIPPET, details)

    @classmethod
    def command_completion(cls, trigger, command, args=None, annotation='', kind=(0, '', ''), details=''):
        return cls(trigger, annotation, m.format_command(command, args), m.CompletionFormat.COMMAND, kind, details)

    def set_completions(self, completions, flags=0): self.completions = completions; self.flags = flags
    CompletionItem.__init__ = item_init
    CompletionItem.snippet_completion = snippet_completion
    CompletionItem.command_completion = command_completion
    CompletionItem.__repr__ = lambda self: 'CompletionItem(%r)' % (self.trigger,)
    CompletionList.set_completions = set_completions


def _install_functions(m):
    clip = {'text': ''}
    flags = {}
    syntaxes_cache = []

    def _scan_syntaxes():
        if syntaxes_cache: return syntaxes_cache
        for root in m._package_roots():
            if not os.path.isdir(root): continue
            for base, _, files in os.walk(root):
                for f in files:
                    if f.endswith('.sublime-syntax') or f.endswith('.tmLanguage'):
                        full = os.path.join(base, f)
                        rel = os.path.relpath(full, root).replace(os.sep, '/')
                        name = os.path.splitext(f)[0]; scope = ''; hidden = False; exts = []
                        try:
                            head = open(full, 'r', encoding='utf-8', errors='replace').read(4096)
                            hit = re.search(r'^name:\s*(.+)$', head, re.M)
                            if hit: name = hit.group(1).strip().strip('"\'')
                            hit = re.search(r'^scope:\s*(\S+)', head, re.M)
                            if hit: scope = hit.group(1)
                            hidden = bool(re.search(r'^hidden:\s*true', head, re.M))
                            exts = re.findall(r'^\s*-\s*([\w.+-]+)\s*$', head.split('file_extensions:', 1)[1].split('\n\n', 1)[0], re.M) if 'file_extensions:' in head else []
                        except Exception: pass
                        syn = Syntax('Packages/' + rel, name, hidden, scope)
                        syn._exts = exts
                        syntaxes_cache.append(syn)
        return syntaxes_cache

    def channel(): return 'stable'
    def executable_hash(): return ('moreno', 'moreno', 'moreno')
    def yes_no_cancel_dialog(msg, yes_title='', no_title='', title=''):
        print('[yes-no-cancel]', msg); return m.DialogResult.CANCEL
    def open_dialog(callback, file_types=[], directory=None, multi_select=False, allow_folders=False): callback(None)
    def save_dialog(callback, file_types=[], directory=None, name=None, extension=None): callback(None)
    def choose_font_dialog(callback, default=None): callback(None)
    def format_command(cmd, args=None):
        return cmd if not args else cmd + ' ' + json.dumps(args)
    def command_url(cmd, args=None):
        from urllib.parse import quote
        return 'subl:' + cmd + ('' if not args else ' ' + quote(json.dumps(args)))
    def get_clipboard(size_limit=16777216): return clip['text'][:size_limit]
    def get_clipboard_async(callback, size_limit=16777216): callback(get_clipboard(size_limit))
    def set_clipboard(text): clip['text'] = str(text)

    def _flag(name):
        def setter(flag=None):
            if flag is not None: flags[name] = bool(flag)
            return None
        def getter(): return flags.get(name, False)
        return setter, getter
    for n in ('commands', 'input', 'fps', 'result_regex', 'indexing', 'build_systems', 'control_tree'):
        setter, getter = _flag(n)
        setattr(m, 'log_' + n, setter)
        setattr(m, 'get_log_' + n, getter)

    def ui_info(): return {'system': {'ui_scale': 1.0}, 'window': {}, 'view': {}}
    def load_binary_resource(name):
        p = m._resource_path(name)
        if not p: raise FileNotFoundError(name)
        return open(p, 'rb').read()

    def encode_value(value, pretty=False, update_text=None):
        return json.dumps(value, indent=4 if pretty else None, ensure_ascii=False)

    def expand_variables(value, variables):
        if isinstance(value, str):
            return re.sub(r'\$\{(\w+)(?::([^}]*))?\}|\$(\w+)', lambda h: _expand_one(h, variables), value)
        if isinstance(value, dict): return {k: expand_variables(v, variables) for k, v in value.items()}
        if isinstance(value, (list, tuple)): return [expand_variables(v, variables) for v in value]
        return value

    def _expand_one(h, variables):
        key = h.group(1) or h.group(3)
        if key in variables: return str(variables[key])
        default = h.group(2)
        return h.group(0) if default is None else default

    def get_macro(): return []
    def project_history(): return []
    def folder_history(): return []

    def score_selector(scope_name, selector):
        atoms = scope_name.split()
        best = 0
        for alt in re.split(r'\s*[,|]\s*', selector.strip()):
            tokens = re.sub(r'(^|\s)-\s+', r'-', alt).split()
            if not tokens: continue
            positive = [t for t in tokens if not t.startswith('-')]
            negative = [t[1:] for t in tokens if t.startswith('-')]
            if any(a == n or a.startswith(n + '.') for n in negative for a in atoms): continue
            idx = 0; score = 0
            for tok in positive:
                while idx < len(atoms) and not (atoms[idx] == tok or atoms[idx].startswith(tok + '.')): idx += 1
                if idx >= len(atoms): score = 0; break
                score += 1 + len(tok.split('.')); idx += 1
            else:
                best = max(best, score or 1)
        return best

    def list_syntaxes(): return list(_scan_syntaxes())
    def syntax_from_path(path):
        path = str(path)
        for s in _scan_syntaxes():
            if s.path == path or s.path.endswith('/' + path.lstrip('/')): return s
        return None
    def find_syntax_by_name(name): return [s for s in _scan_syntaxes() if s.name == name]
    def find_syntax_by_scope(scope): return [s for s in _scan_syntaxes() if s.scope == scope]
    def find_syntax_for_file(path, first_line=''):
        ext = os.path.splitext(str(path))[1].lstrip('.').lower(); base = os.path.basename(str(path))
        for s in _scan_syntaxes():
            if ext and ext in [e.lower() for e in getattr(s, '_exts', [])]: return s
            if base in getattr(s, '_exts', []): return s
        return None
    def make_sheet(sheet_id):
        v = m._view_registry.get(int(sheet_id))
        return _sheet_for(v) if v else None
    def windows(): return [m.active_window()]

    for fn in (channel, executable_hash, yes_no_cancel_dialog, open_dialog, save_dialog, choose_font_dialog, format_command,
               command_url, get_clipboard, get_clipboard_async, set_clipboard, ui_info, load_binary_resource, encode_value,
               expand_variables, get_macro, project_history, folder_history, score_selector, list_syntaxes, syntax_from_path,
               find_syntax_by_name, find_syntax_by_scope, find_syntax_for_file, make_sheet, windows):
        setattr(m, fn.__name__, fn)

    real_ok_cancel = m.ok_cancel_dialog
    m.ok_cancel_dialog = lambda msg, ok_title='OK', title='': real_ok_cancel(msg, ok_title)
    m.select_folder_dialog = lambda callback, directory=None, multi_select=False: callback(None)


def _install_window(m):
    W = m.Window
    R = m.Region

    def state(self): return self.__dict__.setdefault('_state', {
        'layout': {'cols': [0.0, 1.0], 'rows': [0.0, 1.0], 'cells': [[0, 0, 1, 1]]}, 'group': 0, 'selected': [],
        'sidebar': True, 'minimap': True, 'status': True, 'tabs': True, 'menu': True, 'folders': [], 'project': None,
        'panel': '', 'io_panels': {}, 'history': []})

    def views_in_group(self, group): return [v for v in self.views() if getattr(v, '_group', 0) == group]
    def sheet_list(views): return [m._sheet_for(v) for v in views]

    def id_(self): return 1
    def hwnd(self): return 0
    def bring_to_front(self): return None
    def active_sheet(self):
        v = self.active_view(); return m._sheet_for(v) if v else None
    def new_html_sheet(self, name, contents, flags=0, group=-1):
        v = self.new_file(); v.set_name(name); v.text = str(contents)
        if group >= 0: v._group = group
        v._emit(False); return m._sheet_for(v)
    def find_open_file(self, fname, group=-1):
        target = os.path.normcase(os.path.abspath(str(fname)))
        for v in self.views():
            fn = v.file_name()
            if fn and os.path.normcase(os.path.abspath(fn)) == target and (group < 0 or getattr(v, '_group', 0) == group): return v
        return None
    def file_history(self): return list(state(self)['history'])
    def num_groups(self): return len(state(self)['layout']['cells'])
    def active_group(self):
        v = self.active_view(); return getattr(v, '_group', state(self)['group']) if v else state(self)['group']
    def focus_group(self, idx):
        state(self)['group'] = int(idx)
        v = active_view_in_group(self, idx)
        if v: self.focus_view(v)
    def focus_sheet(self, sheet): self.focus_view(sheet.view())
    def select_sheets(self, sheets): state(self)['selected'] = list(sheets)
    def selected_sheets(self):
        sel = [s for s in state(self)['selected'] if s.view().is_valid()]
        if sel: return sel
        s = active_sheet(self); return [s] if s else []
    def selected_sheets_in_group(self, group): return [s for s in selected_sheets(self) if s.group() == group]
    def active_view_in_group(self, group):
        vs = views_in_group(self, group)
        active = self.active_view() if self.views() else None
        if active in vs: return active
        return vs[-1] if vs else None
    def active_sheet_in_group(self, group):
        v = active_view_in_group(self, group); return m._sheet_for(v) if v else None
    def sheets(self): return sheet_list(self.views())
    def sheets_in_group(self, group): return sheet_list(views_in_group(self, group))
    def num_sheets_in_group(self, group): return len(views_in_group(self, group))
    def num_views_in_group(self, group): return len(views_in_group(self, group))
    def transient_sheet_in_group(self, group): return None
    def transient_view_in_group(self, group): return None
    def promote_sheet(self, sheet): return None
    def get_view_index(self, view):
        if view not in self.views(): return (-1, -1)
        g = getattr(view, '_group', 0); return (g, views_in_group(self, g).index(view))
    def get_sheet_index(self, sheet): return get_view_index(self, sheet.view())
    def set_view_index(self, view, group, index):
        self.views()
        if view not in self._views: return
        self._views.remove(view); view._group = group
        peers = views_in_group(self, group)
        if index < 0 or index >= len(peers): self._views.append(view)
        else: self._views.insert(self._views.index(peers[index]), view)
    def set_sheet_index(self, sheet, group, index): set_view_index(self, sheet.view(), group, index)
    def move_sheets_to_group(self, sheets, group, insertion_idx=-1, select=True):
        for offset, s in enumerate(sheets):
            set_view_index(self, s.view(), group, -1 if insertion_idx < 0 else insertion_idx + offset)
        if select and sheets: state(self)['selected'] = list(sheets)
    def get_layout(self): return json.loads(json.dumps(state(self)['layout']))
    def set_layout(self, layout): state(self)['layout'] = dict(layout)
    def layout(self): return get_layout(self)

    def create_io_panel(self, name, on_input, unlisted=False):
        v = self.create_output_panel(name, unlisted); state(self)['io_panels'][str(name)] = on_input; return v
    def find_io_panel(self, name):
        return self.find_output_panel(name) if str(name) in state(self)['io_panels'] else None
    def active_panel(self): return state(self)['panel'] or None
    def panels(self):
        return ['output.' + n for n in self._output_panels] + ['console', 'find', 'replace', 'incremental_find', 'find_in_files']
    def get_output_panel(self, name): return self.find_output_panel(name) or self.create_output_panel(name)

    def vis(key):
        def getter(self): return state(self)[key]
        def setter(self, flag, animate=True): state(self)[key] = bool(flag)
        return getter, setter
    for getter_name, setter_name, key in (('is_sidebar_visible', 'set_sidebar_visible', 'sidebar'),
                                           ('is_minimap_visible', 'set_minimap_visible', 'minimap'),
                                           ('is_status_bar_visible', 'set_status_bar_visible', 'status'),
                                           ('get_tabs_visible', 'set_tabs_visible', 'tabs'),
                                           ('is_menu_visible', 'set_menu_visible', 'menu')):
        getter, setter = vis(key)
        setattr(W, getter_name, getter); setattr(W, setter_name, setter)

    def folders(self):
        data = state(self)['project'] or {}
        out = []
        for f in data.get('folders', []):
            p = f.get('path', '') if isinstance(f, dict) else str(f)
            if p: out.append(os.path.abspath(p))
        return out or list(state(self)['folders'])
    def project_file_name(self): return None
    def workspace_file_name(self): return None
    def project_data(self): return state(self)['project']
    def set_project_data(self, data): state(self)['project'] = data
    def template_settings(self): return m.load_settings('<window-template>')
    def symbol_locations(self, sym, source=0, type=0, kind_id=0, kind_letter=''): return []
    def lookup_symbol_in_index(self, symbol): return []
    def lookup_symbol_in_open_files(self, symbol): return []
    def lookup_references_in_index(self, symbol): return []
    def lookup_references_in_open_files(self, symbol): return []
    def extract_variables(self):
        v = self.active_view(); fn = v.file_name() if v else None
        out = {'packages': m.packages_path(), 'platform': {'windows': 'Windows', 'osx': 'OSX', 'linux': 'Linux'}[m.platform()]}
        folders_ = folders(self)
        if folders_: out['folder'] = folders_[0]
        if fn:
            out['file'] = fn; out['file_path'] = os.path.dirname(fn); out['file_name'] = os.path.basename(fn)
            base, ext = os.path.splitext(os.path.basename(fn)); out['file_base_name'] = base; out['file_extension'] = ext.lstrip('.')
        return out
    def status_message(self, msg): m.status_message(msg)
    def settings(self): return m.load_settings('<window>')
    def is_valid(self): return True

    for fn in (id_, hwnd, bring_to_front, active_sheet, new_html_sheet, find_open_file, file_history, num_groups, active_group,
               focus_group, focus_sheet, select_sheets, selected_sheets, selected_sheets_in_group, active_view_in_group,
               active_sheet_in_group, sheets, sheets_in_group, num_sheets_in_group, num_views_in_group, views_in_group,
               transient_sheet_in_group, transient_view_in_group, promote_sheet, get_view_index, get_sheet_index, set_view_index,
               set_sheet_index, move_sheets_to_group, get_layout, set_layout, layout, create_io_panel, find_io_panel, active_panel,
               panels, get_output_panel, folders, project_file_name, workspace_file_name, project_data, set_project_data, template_settings,
               symbol_locations, lookup_symbol_in_index, lookup_symbol_in_open_files, lookup_references_in_index,
               lookup_references_in_open_files, extract_variables, status_message, settings, is_valid):
        setattr(W, 'id' if fn is id_ else fn.__name__, fn)

    original_open = W.open_file
    def open_file(self, path, flags=0, group=-1):
        existing = find_open_file(self, path)
        if existing is not None and not (int(flags) & m.NewFileFlags.FORCE_CLONE):
            self.focus_view(existing); return existing
        view = original_open(self, path)
        if group >= 0: view._group = group
        hist = state(self)['history']
        if str(path) not in hist: hist.insert(0, str(path))
        return view
    W.open_file = open_file

    original_new = W.new_file
    def new_file(self, flags=0, syntax=''):
        view = original_new(self)
        if syntax: view.assign_syntax(syntax)
        return view
    W.new_file = new_file

    original_views = W.views
    def views(self, *, include_transient=False): return original_views(self)
    W.views = views

    original_quick = W.show_quick_panel
    def show_quick_panel(self, items, on_select, flags=0, selected_index=-1, on_highlight=None, placeholder=None):
        wrapped = on_select
        if int(flags) & m.QuickPanelFlags.WANT_EVENT:
            wrapped = lambda index: on_select(index, {'modifier_keys': {}})
        return original_quick(self, items, wrapped, flags, selected_index)
    W.show_quick_panel = show_quick_panel


def install(m):
    m.Edit = Edit; m.Region = Region; m.Selection = Selection; m.Syntax = Syntax; m.Html = Html
    m.HistoricPosition = HistoricPosition; m.TextChange = TextChange; m.ContextStackFrame = ContextStackFrame
    m.Phantom = Phantom; m.PhantomSet = PhantomSet; m.ListInputItem = ListInputItem
    m.SymbolRegion = SymbolRegion; m.SymbolLocation = SymbolLocation
    m.Buffer = Buffer; m.Sheet = Sheet; m.TextSheet = TextSheet; m.ImageSheet = ImageSheet; m.HtmlSheet = HtmlSheet
    m._sheet_for = _sheet_for
    _install_enums(m)
    _install_settings(m)
    _install_functions(m)
    _install_view(m)
    _install_window(m)
    _install_completions(m)
    m.windows = lambda: [m.active_window()]
    m.run_command = lambda cmd, args=None: m.active_window().run_command(cmd, args)


# ---------------------------------------------------------------------------------------------
# Plugin-side classes
# ---------------------------------------------------------------------------------------------

def _snake(name):
    raw = name[:-7] if name.endswith('Command') else name
    out = ''
    for i, ch in enumerate(raw):
        if i and ch.isupper() and (raw[i - 1].islower() or (i + 1 < len(raw) and raw[i + 1].islower())): out += '_'
        out += ch.lower()
    return out


def _all_subclasses(cls):
    seen = []
    stack = list(cls.__subclasses__())
    while stack:
        c = stack.pop()
        if c not in seen:
            seen.append(c); stack.extend(c.__subclasses__())
    return seen


def install_plugin(p):
    api = importlib.import_module('sub' + 'lime')
    ns = p.__dict__

    class Command:
        @classmethod
        def name(cls): return _snake(cls.__name__)
        def is_enabled(self, *args, **kwargs): return True
        def is_visible(self, *args, **kwargs): return True
        def is_checked(self, *args, **kwargs): return False
        def description(self, *args, **kwargs): return None
        def want_event(self): return False
        def input(self, args): return None
        def input_description(self): return ''
        def run(self, *args, **kwargs): pass
        def is_enabled_(self, args): return self._call_with_args(self.is_enabled, args)
        def is_visible_(self, args): return self._call_with_args(self.is_visible, args)
        def is_checked_(self, args): return self._call_with_args(self.is_checked, args)
        def description_(self, args): return self._call_with_args(self.description, args)
        def _call_with_args(self, fn, args):
            try: return fn(**self.filter_args(args)) if args else fn()
            except TypeError: return fn()
        def create_input_handler_(self, args):
            handler = self.input(args)
            if handler is not None and hasattr(handler, 'setup_'): handler.setup_(args)
            return handler
        def run_(self, edit_token, args):
            args = self.filter_args(args)
            try:
                if args: return self.run(**args)
                return self.run()
            except TypeError:
                return self.run(**args) if args else self.run()
        def filter_args(self, args):
            if self.want_event(): return args
            return {k: v for k, v in (args or {}).items() if k != 'event'}

    class ApplicationCommand(Command):
        def __init__(self, *args): pass
        def run(self): pass

    class WindowCommand(Command):
        def __init__(self, window=None): self.window = window
        def run(self): pass

    class TextCommand(Command):
        def __init__(self, view=None): self.view = view
        def run(self, edit): pass
        def run_(self, edit_token, args):
            args = self.filter_args(args)
            return self.run(api.Edit(edit_token), **args)

    class EventListener: pass

    class ViewEventListener:
        @classmethod
        def is_applicable(cls, settings): return True
        @classmethod
        def applies_to_primary_view_only(cls): return True
        def __init__(self, view=None): self.view = view

    class TextChangeListener:
        @classmethod
        def is_applicable(cls, buffer): return True
        def __init__(self): self.buffer = None
        def is_attached(self): return self.buffer is not None
        def attach(self, buffer): self.buffer = buffer
        def detach(self): self.buffer = None

    class CommandInputHandler:
        def name(self): return _snake(type(self).__name__).replace('_input_handler', '').replace('_handler', '')
        def placeholder(self): return ''
        def initial_text(self): return ''
        def initial_selection(self): return []
        def preview(self, arg): return None
        def validate(self, arg): return True
        def cancel(self): pass
        def confirm(self, arg): pass
        def next_input(self, args): return None
        def setup(self, args): pass
        def description(self, text): return text if isinstance(text, str) else str(text)
        def want_event(self): return False
        def create_input_handler_(self, args): return self.next_input(args)
        def preview_(self, v): return self.preview(v)
        def validate_(self, v): return self.validate(v)
        def cancel_(self): return self.cancel()
        def confirm_(self, v): return self.confirm(v)
        def setup_(self, args): return self.setup(args)
        def description_(self, v, text): return self.description(text)

    class BackInputHandler(CommandInputHandler):
        def name(self): return 'back'
        def placeholder(self): return 'Back'
        def next_input(self, args): return None

    class TextInputHandler(CommandInputHandler):
        def list_items(self): return None

    class ListInputHandler(CommandInputHandler):
        def list_items(self): return []

    ns.update(Command=Command, ApplicationCommand=ApplicationCommand, WindowCommand=WindowCommand, TextCommand=TextCommand,
              EventListener=EventListener, ViewEventListener=ViewEventListener, TextChangeListener=TextChangeListener,
              CommandInputHandler=CommandInputHandler, BackInputHandler=BackInputHandler,
              TextInputHandler=TextInputHandler, ListInputHandler=ListInputHandler)

    ns['api_ready'] = True
    ns['application_command_classes'] = []
    ns['window_command_classes'] = []
    ns['text_command_classes'] = []
    ns['view_event_listener_classes'] = []
    ns['text_change_listener_classes'] = []
    ns['view_event_listeners'] = {}
    ns['text_change_listeners'] = {}
    ns['all_command_classes'] = [ns['application_command_classes'], ns['window_command_classes'], ns['text_command_classes']]
    ns['all_callbacks'] = {}
    ns['deferred_plugin_loadeds'] = []
    ns['override_path'] = None
    ns['multi_importer'] = None

    _instances = {}

    def _listener_instances():
        out = []
        for cls in _all_subclasses(EventListener):
            if cls not in _instances:
                try: _instances[cls] = cls()
                except Exception as e: print('[listener-init-error]', cls.__name__, e); continue
            out.append(_instances[cls])
        return out

    def _view(view_id):
        return api._view_registry.get(int(view_id))

    def _window(window_id): return api.active_window()

    def _call(name, *args):
        for inst in _listener_instances():
            cb = getattr(inst, name, None)
            if cb is None: continue
            try: cb(*args)
            except Exception as e: print('[%s-error]' % name, e)

    def _first(name, *args):
        for inst in _listener_instances():
            cb = getattr(inst, name, None)
            if cb is None: continue
            try:
                result = cb(*args)
                if result is not None: return result
            except Exception as e: print('[%s-error]' % name, e)
        return None

    def _view_hook(name):
        def hook(view_id):
            v = _view(view_id)
            if v is not None: _call(name, v)
        hook.__name__ = name
        return hook

    view_hooks = ('on_new', 'on_new_async', 'on_clone', 'on_clone_async', 'on_load', 'on_load_async', 'on_revert', 'on_revert_async',
                  'on_reload', 'on_reload_async', 'on_pre_move', 'on_post_move', 'on_post_move_async', 'on_pre_close', 'on_close',
                  'on_pre_save', 'on_pre_save_async', 'on_post_save', 'on_post_save_async', 'on_modified', 'on_modified_async',
                  'on_selection_modified', 'on_selection_modified_async', 'on_activated', 'on_activated_async', 'on_deactivated',
                  'on_deactivated_async')
    for name in view_hooks:
        ns[name] = _view_hook(name)
    for name in ('on_new_buffer', 'on_new_buffer_async', 'on_associate_buffer', 'on_associate_buffer_async', 'on_close_buffer',
                 'on_close_buffer_async'):
        ns[name] = _view_hook(name.replace('buffer', 'buffer'))
    for name in ('on_new_project', 'on_new_project_async', 'on_load_project', 'on_load_project_async', 'on_pre_save_project',
                 'on_post_save_project', 'on_post_save_project_async', 'on_pre_close_project', 'on_new_window',
                 'on_new_window_async', 'on_pre_close_window'):
        ns[name] = (lambda n: lambda window_id: _call(n, _window(window_id)))(name)
    ns['on_init'] = lambda module_names=None: None
    ns['on_exit'] = lambda: _call('on_exit')

    def on_query_context(view_id, key, operator, operand, match_all):
        v = _view(view_id)
        return _first('on_query_context', v, key, operator, operand, match_all) if v is not None else None

    def on_query_completions(view_id, prefix, locations):
        v = _view(view_id)
        if v is None: return None
        flags = 0; items = []
        for inst in _listener_instances():
            cb = getattr(inst, 'on_query_completions', None)
            if cb is None: continue
            try: result = cb(v, prefix, locations)
            except Exception as e: print('[on_query_completions-error]', e); continue
            if result is None: continue
            if isinstance(result, tuple) and len(result) == 2 and isinstance(result[1], int): items.extend(result[0]); flags |= result[1]
            elif hasattr(result, 'completions'): items.extend(result.completions); flags |= int(result.flags)
            else: items.extend(result)
        return (items, flags)

    def on_hover(view_id, point, hover_zone):
        v = _view(view_id)
        if v is not None: _call('on_hover', v, point, hover_zone)

    def _command_hook(name, window):
        def hook(*args):
            target = _window(args[0]) if window else _view(args[0])
            if target is None: return None
            return _first(name, target, *args[1:])
        hook.__name__ = name
        return hook

    ns['on_query_context'] = on_query_context
    ns['on_query_completions'] = on_query_completions
    ns['on_hover'] = on_hover
    ns['on_text_command'] = _command_hook('on_text_command', False)
    ns['on_window_command'] = _command_hook('on_window_command', True)
    ns['on_post_text_command'] = _command_hook('on_post_text_command', False)
    ns['on_post_window_command'] = _command_hook('on_post_window_command', True)

    def split_trigger(trigger):
        return trigger.split('\t', 1) if '\t' in trigger else [trigger, '']
    ns['split_trigger'] = split_trigger

    def plugin_module_for_obj(obj): return sys.modules.get(type(obj).__module__)
    ns['plugin_module_for_obj'] = plugin_module_for_obj

    def unload_module(module):
        name = getattr(module, '__name__', str(module))
        sys.modules.pop(name, None)
    def unload_plugin(modulename): sys.modules.pop(modulename, None)
    def load_module(module): return module
    def reload_plugin(modulename):
        mod = sys.modules.get(modulename)
        if mod is not None: importlib.reload(mod)
    def set_override_path(path): ns['override_path'] = path
    def update_compressed_packages(pkgs): return None
    def decorate_handler(cls, method_name): return None
    def trap_exceptions(fn): return fn
    def profile(fn): return fn
    def add_profiling(obj): return obj
    def get_profiling_data(): return []
    ns.update(unload_module=unload_module, unload_plugin=unload_plugin, load_module=load_module, reload_plugin=reload_plugin,
              set_override_path=set_override_path, update_compressed_packages=update_compressed_packages,
              decorate_handler=decorate_handler, trap_exceptions=trap_exceptions, profile=profile, add_profiling=add_profiling,
              get_profiling_data=get_profiling_data)

    ns['Summary'] = type('Summary', (), {'__init__': lambda self: setattr(self, 'total', 0),
                                         'record': lambda self, *a, **k: None})

    def create_application_commands(): return [c() for c in _all_subclasses(ApplicationCommand)]
    def create_window_commands(window_id): return [c(_window(window_id)) for c in _all_subclasses(WindowCommand)]
    def create_text_commands(view_id): return [c(_view(view_id)) for c in _all_subclasses(TextCommand)]
    def create_view_event_listeners(classes, view):
        out = []
        for cls in classes:
            try:
                if cls.is_applicable(view.settings()): out.append(cls(view))
            except Exception as e: print('[view-listener-error]', cls.__name__, e)
        return out
    def check_view_event_listeners(view):
        found = create_view_event_listeners(_all_subclasses(ViewEventListener), view)
        ns['view_event_listeners'][int(view.id)] = found
    def attach_view(view): check_view_event_listeners(view)
    def detach_view(view): ns['view_event_listeners'].pop(int(view.id), None)
    def find_view_event_listener(view, cls):
        for l in ns['view_event_listeners'].get(int(view.id), []):
            if type(l) is cls: return l
        return None
    def check_all_view_event_listeners():
        for v in api.active_window().views(): check_view_event_listeners(v)
    def attach_buffer(buf_id): return None
    def detach_buffer(buf_id): return None
    def check_text_change_listeners(buf): return None
    def is_view_event_listener_applicable(cls, view): return cls.is_applicable(view.settings())
    def run_view_callbacks(name, view_id, *args):
        v = _view(view_id)
        if v is None: return
        for l in ns['view_event_listeners'].get(int(view.id if False else view_id), []):
            cb = getattr(l, name, None)
            if cb:
                try: cb(*args)
                except Exception as e: print('[%s-error]' % name, e)
    def run_window_callbacks(name, window_id, *args): _call(name, _window(window_id), *args)
    def synthesize_on_activated_async(): return None
    def notify_application_commands(): return None
    def on_api_ready(): return None
    for fn in (create_application_commands, create_window_commands, create_text_commands, create_view_event_listeners,
               check_view_event_listeners, attach_view, detach_view, find_view_event_listener, check_all_view_event_listeners,
               attach_buffer, detach_buffer, check_text_change_listeners, is_view_event_listener_applicable, run_view_callbacks,
               run_window_callbacks, synthesize_on_activated_async, notify_application_commands, on_api_ready):
        ns[fn.__name__] = fn
    ns['check_all_view_event_listeners_scheduled'] = check_all_view_event_listeners
    ns['el_callbacks'] = {}
    ns['vel_callbacks'] = {}
    ns['view_event_listener_excluded_callbacks'] = set()
    ns['text_change_listener_callbacks'] = set()
    ns['pending_on_activated_async_callbacks'] = []
    ns['pending_on_activated_async_lock'] = threading.Lock()
    class ZipResourceReader:
        def __init__(self, loader, fullname): self.loader = loader; self.fullname = fullname
        def open_resource(self, resource): raise FileNotFoundError(resource)
        def resource_path(self, resource): raise FileNotFoundError(resource)
        def is_resource(self, name): return False
        def contents(self): return iter(())

    class ZipLoader:
        def __init__(self, zippath, *args): self.zippath = zippath
        def has(self, fullname): return False
        def get_resource_reader(self, fullname): return ZipResourceReader(self, fullname)
        def get_filename(self, fullname): return self.zippath
        def get_code(self, fullname): return None
        def get_source(self, fullname): return None
        def is_package(self, fullname): return False

    class MultizipImporter:
        def __init__(self): self.loaders = []
        def find_spec(self, fullname, path=None, target=None): return None

    ns.update(ZipResourceReader=ZipResourceReader, ZipLoader=ZipLoader, MultizipImporter=MultizipImporter)
