#!/usr/bin/env python3
"""Render_Ps2eBackendOnly: the typed-draw surface (engine/platform/ps2e/ps2e_draw.cpp and its header) speaks
engine/render/backend.h ONLY -- no backend folder header (engine/render/backends/*), no GL header
(PS2R_GL_HEADER, <GL/...>, <OpenGL/...>), no GL entry point or token in the text. The arm files
(ps2e_draw_backend_*.cpp) are where an API is named; a new graphics API is a new backend plus a new arm, never a
change to the surface."""
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
files = [root / 'engine/platform/ps2e/ps2e_draw.cpp', root / 'engine/platform/ps2e/ps2e_draw.h',
         root / 'engine/platform/ps2e/ps2e_draw_backend.h']
forbidden = [
    (re.compile(r'^\s*#\s*include\s+"engine/render/backends/'), 'a backend folder header'),
    (re.compile(r'^\s*#\s*include\s+(PS2R_GL_HEADER|<GL/|<OpenGL/|<GLES)'), 'a GL header'),
    (re.compile(r'\b(gl[A-Z]\w*)\s*\('), 'a GL entry point'),
    (re.compile(r'\bGL_[A-Z0-9_]+\b'), 'a GL token'),
    (re.compile(r'\bGl(Device|StateCache|Surface|ShaderSources|ShaderLibrary|DrawExecutor)\b'), 'a GL backend type'),
]
def strip_block_comments(text):
    # comments may NAME the arms and the GL device; code may not reach them -- block comments are blanked
    # (their newlines kept so line numbers hold), line comments are cut per line below
    return re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)


bad = 0
for f in files:
    for i, line in enumerate(strip_block_comments(f.read_text()).splitlines(), 1):
        code = line.split('//', 1)[0]
        for rx, what in forbidden:
            if rx.search(code):
                # SDL's GL-flavoured window calls (drawable size, context handover) are the SDL window half, not the backend
                if what == 'a GL entry point' and re.search(r'\bSDL_GL_\w+\s*\(', code) and not re.search(r'\bgl[A-Z]\w*\s*\(', code.replace('SDL_GL_', '')):
                    continue
                print('%s:%d: %s: %s' % (f.relative_to(root), i, what, line.strip()))
                bad += 1
print('Render_Ps2eBackendOnly:', 'FAIL' if bad else 'PASS')
sys.exit(1 if bad else 0)
