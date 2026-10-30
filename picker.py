#!/usr/bin/env python3
"""
Source picker, launched by spyglass.py when an app asks to share the screen.
Prints the choice and exits:
  monitor:<name>   e.g. "monitor:DP-1"
  window:<title>   e.g. "window:Firefox"
Prints nothing if cancelled.
"""
import gi
import sys
import os
import subprocess
import json

gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gtk, GtkLayerShell, Gdk, GLib

PICKER_WIDTH  = 480
ANIM_TICK_MS  = 12
ANIM_OPEN_MS  = 520
ANIM_CLOSE_MS = 280
START_SCALE   = 0.82
SCRIM_ALPHA   = 0.30
ROW_HEIGHT    = 38
FADE_MS       = 110
MORPH_MS      = 280

SCREENCOPY_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'screencopy')


def ease_out(t):
    return 1.0 - (1.0 - t) ** 4


def ease_in(t):
    return t ** 3


CSS = b"""
* {
    font-family: 'Plus Jakarta Sans', sans-serif;
}

window { background: transparent; }
stack, list { background: transparent; }

.scrim { background: black; }

.picker-frame {
    background: linear-gradient(to bottom, rgb(70, 120, 123), rgb(212, 185, 168));
    padding: 3px;
    border-radius: 11px;
    box-shadow:
        0 0 12px 1px rgba(140, 155, 145, 0.35),
        0 8px 32px rgba(0, 0, 0, 0.55);
}

.picker-inner {
    background: rgba(18, 18, 24, 0.78);
    border-radius: 10px;
    padding: 0 0 8px 0;
}

.picker-title {
    color: rgba(232, 228, 223, 0.60);
    font-size: 12px;
    padding: 16px 0 12px 0;
}

.type-btn {
    background: rgba(232, 228, 223, 0.07);
    color: #E8E4DF;
    border: none;
    border-radius: 8px;
    font-size: 14px;
    padding: 10px 0;
    margin: 18px 10px 18px 10px;
    min-width: 150px;
    box-shadow: none;
}

.type-btn:hover {
    background: rgba(232, 228, 223, 0.14);
    box-shadow: none;
}

.type-btn:active {
    background: rgba(232, 228, 223, 0.10);
}

.divider {
    background: rgba(190, 190, 205, 0.12);
    min-height: 1px;
    margin: 0;
}

.back-btn {
    background: transparent;
    border: none;
    box-shadow: none;
    color: rgba(232, 228, 223, 0.45);
    font-size: 16px;
    padding: 0 16px;
    min-height: 44px;
    min-width: 0;
}

.back-btn:hover {
    background: transparent;
    color: #E8E4DF;
    box-shadow: none;
}

.back-btn:active {
    background: transparent;
}

.panel-title {
    color: rgba(232, 228, 223, 0.70);
    font-size: 13px;
    padding-right: 16px;
}

.item-label {
    color: rgba(232, 228, 223, 0.55);
    font-size: 13px;
    padding: 8px 18px;
}

.item-label.active {
    color: #E8E4DF;
}

row {
    background: transparent;
    border: none;
    outline: none;
}

row.active-row {
    background: rgba(232, 228, 223, 0.07);
}

row:focus, row:selected, row:hover {
    background: transparent;
    box-shadow: none;
}

row.active-row:hover {
    background: rgba(232, 228, 223, 0.07);
}
"""


def get_monitors():
    try:
        out = subprocess.check_output(['wlr-randr'], stderr=subprocess.DEVNULL, text=True)
        monitors = []
        for line in out.splitlines():
            stripped = line.strip()
            if stripped and not line.startswith(' ') and not line.startswith('\t'):
                name = stripped.split()[0]
                monitors.append(name)
        return monitors if monitors else ['DP-1', 'DP-2']
    except Exception:
        return ['DP-1', 'DP-2']


def get_friendly_name(app_id):
    # Standard overrides for known applications
    overrides = {
        'com.obsproject.studio': 'OBS',
        'com/mitchellh.ghostty': 'Ghostty',
        'com.mitchellh.ghostty': 'Ghostty',
        'com.visualstudio.code': 'VS Code',
    }
    
    app_id_lower = app_id.lower()
    if app_id_lower in overrides:
        return overrides[app_id_lower]
    
    # Substring checks for common patterns
    if 'obsproject' in app_id_lower or 'obs-studio' in app_id_lower:
        return 'OBS'
    if 'ghostty' in app_id_lower:
        return 'Ghostty'
    if 'nautilus' in app_id_lower:
        return 'Nautilus'
    if 'kdenlive' in app_id_lower:
        return 'Kdenlive'
    if 'thunar' in app_id_lower:
        return 'Thunar'
    if 'firefox' in app_id_lower:
        return 'Firefox'
    if 'chrome' in app_id_lower:
        return 'Google Chrome'
    if 'chromium' in app_id_lower:
        return 'Chromium'
    if 'code' in app_id_lower and 'visualstudio' in app_id_lower:
        return 'VS Code'
    
    # General fallback rule:
    # 1. Split by '/' and take the last segment
    parts = app_id.split('/')
    last_part = parts[-1]
    
    # 2. Split by '.' and take the last segment
    dot_parts = last_part.split('.')
    if len(dot_parts) > 1:
        last_segment = dot_parts[-1]
        prev_segment = dot_parts[-2]
        # If the last segment is generic like "Studio", "Client", etc.
        if last_segment.lower() in ('studio', 'client', 'app', 'desktop', 'helper') and len(prev_segment) > 2:
            p_cap = prev_segment[0].upper() + prev_segment[1:] if prev_segment else ""
            l_cap = last_segment[0].upper() + last_segment[1:] if last_segment else ""
            name = f"{p_cap} {l_cap}"
            if 'obsproject' in name.lower():
                name = name.replace('Obsproject', 'OBS').replace('obsproject', 'OBS')
            return name
        else:
            last_part = last_segment
            
    # 3. Clean up dashes or underscores and title-case
    friendly = last_part.replace('-', ' ').replace('_', ' ')
    words = []
    uppercase_words = {'ide', 'cli', 'gui', 'tui', 'obs', 'sdk', 'api', 'db', 'url', 'uri', 'ui', 'ux', 'gimp', 'cpu', 'gpu', 'ram', 'vm'}
    for w in friendly.split():
        if w.lower() in uppercase_words:
            words.append(w.upper())
        elif w.isupper() and len(w) > 1:
            words.append(w)
        else:
            words.append(w[0].upper() + w[1:] if w else "")
    return ' '.join(words)


def get_windows():
    try:
        with open('/tmp/resonance-windows.json') as f:
            wins = json.load(f)
        return [(entry.get('title') or get_friendly_name(entry.get('app_id', '')), win_id)
                for win_id, entry in wins.items()]
    except Exception:
        return []



class MonitorScrim(Gtk.Window):
    def __init__(self, monitor):
        super().__init__(type=Gtk.WindowType.TOPLEVEL)
        self.set_decorated(False)
        self.set_app_paintable(True)

        visual = self.get_screen().get_rgba_visual()
        if visual:
            self.set_visual(visual)

        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.OVERLAY)
        GtkLayerShell.set_keyboard_mode(self, GtkLayerShell.KeyboardMode.NONE)
        for edge in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.BOTTOM,
                     GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self, edge, True)
        GtkLayerShell.set_exclusive_zone(self, -1)
        GtkLayerShell.set_monitor(self, monitor)

        box = Gtk.Box()
        box.get_style_context().add_class('scrim')
        box.set_hexpand(True)
        box.set_vexpand(True)
        box.set_app_paintable(True)
        self.add(box)

        self.props.opacity = 0
        self.show_all()


class Picker(Gtk.Window):
    def __init__(self):
        super().__init__(type=Gtk.WindowType.TOPLEVEL)
        self._anim_id     = None
        self._closing     = False
        self._anim_elapsed = 0
        self._selection   = None

        self._fade_id      = None
        self._fade_elapsed = 0
        self._fade_out     = False
        self._fade_target  = None

        self._selected     = 0
        self._other_scrims = []

        self.set_decorated(False)
        self.set_app_paintable(True)

        visual = self.get_screen().get_rgba_visual()
        if visual:
            self.set_visual(visual)

        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.OVERLAY)
        GtkLayerShell.set_keyboard_mode(self, GtkLayerShell.KeyboardMode.EXCLUSIVE)
        for edge in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.BOTTOM,
                     GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self, edge, True)
        GtkLayerShell.set_exclusive_zone(self, -1)

        active_monitor = None
        try:
            with open('/tmp/resonance-active-output') as f:
                parts = f.read().strip().split()
            cx, cy = int(parts[0]), int(parts[1])
            display = Gdk.Display.get_default()
            active_monitor = display.get_monitor_at_point(cx, cy)
            if active_monitor:
                GtkLayerShell.set_monitor(self, active_monitor)
        except Exception:
            display = Gdk.Display.get_default()

        display = Gdk.Display.get_default()
        for i in range(display.get_n_monitors()):
            m = display.get_monitor(i)
            if m != active_monitor:
                self._other_scrims.append(MonitorScrim(m))

        self._build_ui()
        self._apply_css()

        self.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
        self.connect('button-press-event', self._on_window_click)
        self.connect('key-press-event', self._on_key_press)

        self.show_all()
        self._padding_y   = self.inner.get_preferred_height()[1] - self.stack.get_preferred_height()[1]
        self.stack.set_visible_child_name('home')

        self.scrim.props.opacity = 0
        self._panel.props.opacity = 0
        self.frame.set_size_request(int(PICKER_WIDTH * START_SCALE), -1)
        self._start_anim(closing=False)

    # ---- UI construction ----

    def _build_ui(self):
        overlay = Gtk.Overlay()

        self.scrim = Gtk.Box()
        self.scrim.get_style_context().add_class('scrim')
        self.scrim.set_hexpand(True)
        self.scrim.set_vexpand(True)
        self.scrim.set_app_paintable(True)

        self._panel = Gtk.Box()
        self._panel.set_halign(Gtk.Align.CENTER)
        self._panel.set_valign(Gtk.Align.CENTER)
        self._panel.set_app_paintable(True)

        self.frame = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.frame.get_style_context().add_class('picker-frame')
        self.frame.set_size_request(PICKER_WIDTH, -1)
        self.frame.set_app_paintable(True)

        self.inner = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.inner.get_style_context().add_class('picker-inner')
        self.inner.set_app_paintable(True)

        self.stack = Gtk.Stack()
        self.stack.set_homogeneous(False)
        self.stack.set_transition_type(Gtk.StackTransitionType.NONE)
        self.stack.set_app_paintable(True)

        self.home_box = self._build_home()
        self.stack.add_named(self.home_box, 'home')

        self.list_panel, self.list_title_lbl, self.list_box, self._select_cb = \
            self._build_list_panel()
        self.stack.add_named(self.list_panel, 'list')

        self.inner.pack_start(self.stack, True, True, 0)
        self.frame.pack_start(self.inner, True, True, 0)
        self._panel.pack_start(self.frame, False, False, 0)

        overlay.add(self.scrim)
        overlay.add_overlay(self._panel)
        self.add(overlay)

    def _build_home(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.set_app_paintable(True)

        title = Gtk.Label(label='Share your screen')
        title.get_style_context().add_class('picker-title')
        title.set_app_paintable(True)
        box.pack_start(title, False, False, 0)

        divider = Gtk.Box()
        divider.get_style_context().add_class('divider')
        divider.set_app_paintable(True)
        box.pack_start(divider, False, False, 0)

        btn_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        btn_row.set_halign(Gtk.Align.CENTER)
        btn_row.set_app_paintable(True)

        display_btn = Gtk.Button(label='Display')
        display_btn.set_relief(Gtk.ReliefStyle.NONE)
        display_btn.get_style_context().add_class('type-btn')
        display_btn.connect('clicked', self._on_display_clicked)

        window_btn = Gtk.Button(label='Window')
        window_btn.set_relief(Gtk.ReliefStyle.NONE)
        window_btn.get_style_context().add_class('type-btn')
        window_btn.connect('clicked', self._on_window_clicked)

        btn_row.pack_start(display_btn, False, False, 0)
        btn_row.pack_start(window_btn, False, False, 0)
        box.pack_start(btn_row, False, False, 0)

        return box

    def _build_list_panel(self):
        panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        panel.set_app_paintable(True)

        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        header.set_app_paintable(True)

        back_btn = Gtk.Button(label='←')
        back_btn.set_relief(Gtk.ReliefStyle.NONE)
        back_btn.get_style_context().add_class('back-btn')
        back_btn.connect('clicked', self._on_back_clicked)
        header.pack_start(back_btn, False, False, 0)

        title_lbl = Gtk.Label(label='')
        title_lbl.get_style_context().add_class('panel-title')
        title_lbl.set_app_paintable(True)
        header.pack_start(title_lbl, True, True, 0)

        panel.pack_start(header, False, False, 0)

        divider = Gtk.Box()
        divider.get_style_context().add_class('divider')
        divider.set_app_paintable(True)
        panel.pack_start(divider, False, False, 0)

        list_box = Gtk.ListBox()
        list_box.set_selection_mode(Gtk.SelectionMode.NONE)
        list_box.set_app_paintable(True)
        list_box.add_events(Gdk.EventMask.POINTER_MOTION_MASK)
        list_box.connect('motion-notify-event', self._on_list_motion)

        cb_holder = [None]
        list_box.connect('row-activated',
                         lambda lb, row: cb_holder[0] and cb_holder[0](lb, row))

        panel.pack_start(list_box, False, False, 0)

        return panel, title_lbl, list_box, cb_holder

    def _on_list_motion(self, widget, event):
        row = widget.get_row_at_y(int(event.y))
        if row:
            self._set_selection(row.get_index())
        return False

    def _set_selection(self, index):
        rows = self.list_box.get_children()
        if not rows or index < 0 or index >= len(rows):
            return
        if 0 <= self._selected < len(rows):
            rows[self._selected].get_child().get_style_context().remove_class('active')
            rows[self._selected].get_style_context().remove_class('active-row')
        self._selected = index
        rows[self._selected].get_child().get_style_context().add_class('active')
        rows[self._selected].get_style_context().add_class('active-row')

    def _move_selection(self, delta):
        rows = self.list_box.get_children()
        if not rows:
            return
        self._set_selection((self._selected + delta) % len(rows))

    def _populate_list(self, title, items, on_select):
        self.list_title_lbl.set_text(title)
        for child in self.list_box.get_children():
            self.list_box.remove(child)
        self._select_cb[0] = on_select
        self._selected = 0
        for i, item in enumerate(items):
            lbl = Gtk.Label(label=item)
            lbl.set_halign(Gtk.Align.START)
            lbl.get_style_context().add_class('item-label')
            lbl.set_app_paintable(True)
            
            row = Gtk.ListBoxRow()
            row.set_app_paintable(True)
            row.add(lbl)
            
            if i == 0:
                lbl.get_style_context().add_class('active')
                row.get_style_context().add_class('active-row')
                
            self.list_box.add(row)
        self.list_box.show_all()

    def _apply_css(self):
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(), provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    # ---- open / close animation (identical to Tide) ----

    def _start_anim(self, closing):
        self._closing = closing
        self._anim_elapsed = 0
        if self._anim_id:
            GLib.source_remove(self._anim_id)
        self._anim_id = GLib.timeout_add(ANIM_TICK_MS, self._anim_tick)

    def _anim_tick(self):
        self._anim_elapsed += ANIM_TICK_MS
        duration = ANIM_CLOSE_MS if self._closing else ANIM_OPEN_MS
        t = min(self._anim_elapsed / duration, 1.0)

        if self._closing:
            progress = ease_in(t)
            self.scrim.props.opacity  = SCRIM_ALPHA * (1.0 - progress)
            self._panel.props.opacity = 1.0 - progress
            scale = 1.0 - (1.0 - START_SCALE) * progress
        else:
            progress = ease_out(t)
            self.scrim.props.opacity  = SCRIM_ALPHA * progress
            self._panel.props.opacity = progress
            scale = START_SCALE + (1.0 - START_SCALE) * progress

        for s in self._other_scrims:
            s.props.opacity = SCRIM_ALPHA * (1.0 - progress if self._closing else progress)

        self.frame.set_size_request(int(PICKER_WIDTH * scale), -1)

        if t >= 1.0:
            self._anim_id = None
            if self._closing:
                if self._selection:
                    print(self._selection, flush=True)
                Gtk.main_quit()
            return False
        return True

    def _close(self, selection=None):
        if self._closing:
            return
        self._selection = selection
        self._start_anim(closing=True)

    # ---- panel fade transition ----

    def _fade_to(self, panel_name):
        self._fade_target  = panel_name
        self._fade_elapsed = 0
        self._fade_out     = True
        if self._fade_id:
            GLib.source_remove(self._fade_id)
        self._fade_id = GLib.timeout_add(ANIM_TICK_MS, self._fade_tick)

    def _fade_tick(self):
        self._fade_elapsed += ANIM_TICK_MS
        t = min(self._fade_elapsed / FADE_MS, 1.0)
 
        if self._fade_out:
            self.stack.props.opacity = 1.0 - ease_in(t)
            if t >= 1.0:
                # Capture current height before switching visible panel
                self._height_start = self.inner.get_allocated_height()
                # Lock height to prevent sudden jump during layout switch
                self.inner.set_size_request(-1, self._height_start)
                
                self.stack.set_visible_child_name(self._fade_target)
                
                # Query new target height for the new panel layout dynamically
                self._height_target = self.stack.get_preferred_height()[1] + self._padding_y
                
                self._fade_elapsed = 0
                self._fade_out = False
        else:
            t_fade = min(self._fade_elapsed / FADE_MS, 1.0)
            t_morph = min(self._fade_elapsed / MORPH_MS, 1.0)
            
            self.stack.props.opacity = ease_out(t_fade)
            
            # Smoothly interpolate height request
            current_h = int(self._height_start + (self._height_target - self._height_start) * ease_out(t_morph))
            self.inner.set_size_request(-1, current_h)
            
            if t_morph >= 1.0:
                self.stack.props.opacity = 1.0
                # Restore automatic sizing
                self.inner.set_size_request(-1, -1)
                self.queue_draw()
                self._fade_id = None
                return False

        self.queue_draw()
        return True

    # ---- button handlers ----

    def _on_display_clicked(self, btn):
        monitors = get_monitors()
        def on_select(lb, row):
            idx = row.get_index()
            if idx < len(monitors):
                self._close(f'monitor:{monitors[idx]}')
        self._populate_list('Select a Display', monitors, on_select)
        self._fade_to('list')

    def _on_window_clicked(self, btn):
        windows = get_windows()
        if not windows:
            windows = [('No windows found', None)]
        labels = [w[0] for w in windows]
        def on_select(lb, row):
            idx = row.get_index()
            if idx < len(windows) and windows[idx][1]:
                self._close(f'window:{windows[idx][1]}')
        self._populate_list('Select a Window', labels, on_select)
        self._fade_to('list')

    def _on_back_clicked(self, btn):
        self._fade_to('home')

    def _on_key_press(self, widget, event):
        key = event.keyval
        if key == Gdk.KEY_Escape:
            self._close()
            return True
        elif self.stack.get_visible_child_name() == 'list':
            if key == Gdk.KEY_Down:
                self._move_selection(1)
                return True
            elif key == Gdk.KEY_Up:
                self._move_selection(-1)
                return True
            elif key in (Gdk.KEY_Return, Gdk.KEY_KP_Enter):
                rows = self.list_box.get_children()
                if rows and 0 <= self._selected < len(rows):
                    self.list_box.row_activated(rows[self._selected])
                return True
        return False

    def _on_window_click(self, widget, event):
        alloc = self.frame.get_allocation()
        inside = (alloc.x <= event.x <= alloc.x + alloc.width and
                  alloc.y <= event.y <= alloc.y + alloc.height)
        if not inside:
            self._close()
        return False


if __name__ == '__main__':
    win = Picker()
    Gtk.main()
