#!/usr/bin/env python3
import sys
import os

if os.environ.get('XDG_CURRENT_DESKTOP') != 'Resonance':
    sys.exit(0)
import json
import struct
import threading
import subprocess
import traceback
import dbus
import dbus.service
import dbus.mainloop.glib
import gi
gi.require_version('Gst', '1.0')
from gi.repository import GLib, Gst, Gio

PICKER_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'picker.py')

sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

Gst.init(None)

BUS_NAME         = 'org.freedesktop.impl.portal.desktop.resonance'
SCREENCAST_IFACE = 'org.freedesktop.impl.portal.ScreenCast'
SESSION_IFACE    = 'org.freedesktop.impl.portal.Session'
WINDOWS_PATH     = '/tmp/resonance-windows.json'

SOURCE_TYPE_MONITOR  = 1
SOURCE_TYPE_WINDOW   = 2
CURSOR_MODE_HIDDEN   = 1
CURSOR_MODE_EMBEDDED = 2

SCREENCOPY_BIN     = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'screencopy')
DMABUF_CAPTURE_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'capture_dmabuf')

# how long to stay up after D-Bus activation with no session
IDLE_EXIT_SECONDS = 30

FRAME_MAGIC = 0x52534350  # 'RSCP'
FRAME_HDR   = 16          # 4 x uint32


def get_window_geometry(win_id):
    try:
        with open(WINDOWS_PATH) as f:
            windows = json.load(f)
        return windows.get(win_id)
    except Exception:
        return None


def find_pipewire_node_id(node_name):
    try:
        out = subprocess.check_output(['pw-dump'], stderr=subprocess.DEVNULL)
        nodes = json.loads(out)
        for obj in nodes:
            if obj.get('type') != 'PipeWire:Interface:Node':
                continue
            props = obj.get('info', {}).get('props', {})
            if props.get('node.name') == node_name:
                return obj['id']
    except Exception as e:
        print(f'pw-dump error: {e}', flush=True)
    return None


def _screencopy_reader(proc, appsrc):
    """Background thread: read frames from screencopy and push into appsrc."""
    caps_set = False
    pts = 0
    frame_duration = Gst.util_uint64_scale_int(1, Gst.SECOND, 60)

    while True:
        hdr = proc.stdout.read(FRAME_HDR)
        if len(hdr) < FRAME_HDR:
            break
        magic, w, h, fmt = struct.unpack('<IIII', hdr)
        if magic != FRAME_MAGIC:
            print('screencopy reader: bad magic, desynced', flush=True)
            break

        frame_size = w * h * 4
        data = proc.stdout.read(frame_size)
        if len(data) < frame_size:
            break

        if not caps_set:
            # wl_shm format codes -> GStreamer formats (memory byte order)
            _fmt_map = {0: 'BGRA', 1: 'BGRx', 0x34324258: 'RGBx', 0x34324142: 'RGBA'}
            gst_fmt = _fmt_map.get(fmt, 'BGRx')
            caps = Gst.Caps.from_string(
                f'video/x-raw,format={gst_fmt},width={w},height={h},framerate=60/1'
            )
            appsrc.set_property('caps', caps)
            caps_set = True
            print(f'screencopy reader: first frame {w}x{h} fmt={fmt} ({gst_fmt})', flush=True)

        buf = Gst.Buffer.new_wrapped(data)
        buf.pts = pts
        buf.duration = frame_duration
        pts += frame_duration

        ret = appsrc.emit('push-buffer', buf)
        if ret != Gst.FlowReturn.OK:
            print(f'screencopy reader: appsrc push returned {ret}, stopping', flush=True)
            break

    print('screencopy reader: exiting', flush=True)



class ResonanceSession(dbus.service.Object):
    def __init__(self, bus, session_handle, app_id, portal):
        super().__init__(bus, str(session_handle))
        self.app_id = app_id
        self.session_handle = session_handle
        self.portal = portal
        self.pipeline = None
        self.screencopy_proc = None
        self.selection = None  # set by SelectSources picker result
        self.stream_position = None
        self.stream_size = None
        print(f'Session created: {session_handle}', flush=True)

    @dbus.service.method(SESSION_IFACE, in_signature='', out_signature='')
    def Close(self):
        print(f'Session.Close: {self.app_id}', flush=True)
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None
        if self.screencopy_proc:
            self.screencopy_proc.terminate()
            self.screencopy_proc = None
        try:
            self.remove_from_connection()
        except Exception:
            pass
        self.portal._on_session_closed(self.session_handle)


class ResonancePortal(dbus.service.Object):
    def __init__(self, bus, mainloop):
        bus_name = dbus.service.BusName(BUS_NAME, bus)
        super().__init__(bus_name, '/org/freedesktop/portal/desktop')
        self._bus = bus
        self._mainloop = mainloop
        self.sessions = {}
        self._idle_timeout = None
        print('Spyglass registered on D-Bus', flush=True)
        # xdg-desktop-portal wakes backends just to read properties,
        # so being activated doesn't count as being used
        self._arm_idle_timeout()

    def _arm_idle_timeout(self):
        self._cancel_idle_timeout()
        self._idle_timeout = GLib.timeout_add_seconds(
            IDLE_EXIT_SECONDS, self._on_idle_timeout)

    def _cancel_idle_timeout(self):
        if self._idle_timeout is not None:
            GLib.source_remove(self._idle_timeout)
            self._idle_timeout = None

    def _on_idle_timeout(self):
        self._idle_timeout = None
        if self.sessions:
            return False
        print(f'Idle for {IDLE_EXIT_SECONDS}s with no sessions — '
              'shutting down spyglass', flush=True)
        self._mainloop.quit()
        return False

    def _on_session_closed(self, session_handle):
        self.sessions.pop(str(session_handle), None)
        if not self.sessions:
            print('No active sessions left — shutting down spyglass', flush=True)
            self._mainloop.quit()

    # --- Properties ---

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='ss', out_signature='v')
    def Get(self, interface, prop):
        if interface == SCREENCAST_IFACE:
            if prop == 'version':
                return dbus.UInt32(4)
            if prop == 'AvailableSourceTypes':
                return dbus.UInt32(SOURCE_TYPE_MONITOR | SOURCE_TYPE_WINDOW)
            if prop == 'AvailableCursorModes':
                return dbus.UInt32(CURSOR_MODE_HIDDEN | CURSOR_MODE_EMBEDDED)
        raise dbus.exceptions.DBusException(
            'org.freedesktop.DBus.Error.InvalidArgs',
            f'No property {prop} on {interface}')

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if interface == SCREENCAST_IFACE:
            return dbus.Dictionary({
                'version': dbus.UInt32(4),
                'AvailableSourceTypes': dbus.UInt32(SOURCE_TYPE_MONITOR | SOURCE_TYPE_WINDOW),
                'AvailableCursorModes': dbus.UInt32(CURSOR_MODE_HIDDEN | CURSOR_MODE_EMBEDDED),
            }, signature='sv')
        return dbus.Dictionary({}, signature='sv')

    # --- ScreenCast ---

    @dbus.service.method(SCREENCAST_IFACE, in_signature='oosa{sv}', out_signature='ua{sv}')
    def CreateSession(self, handle, session_handle, app_id, options):
        print(f'CreateSession: app={app_id}', flush=True)
        # real session now, stop the exit countdown
        self._cancel_idle_timeout()
        session = ResonanceSession(self._bus, session_handle, str(app_id), self)
        self.sessions[str(session_handle)] = session
        return dbus.UInt32(0), dbus.Dictionary({}, signature='sv')

    @dbus.service.method(SCREENCAST_IFACE, in_signature='oosa{sv}', out_signature='ua{sv}',
                         async_callbacks=('return_cb', 'error_cb'))
    def SelectSources(self, handle, session_handle, app_id, options,
                      return_cb, error_cb):
        print(f'SelectSources: app={app_id} types={options.get("types")}', flush=True)
        session = self.sessions.get(str(session_handle))
        if not session:
            return_cb(dbus.UInt32(1), dbus.Dictionary({}, signature='sv'))
            return

        def _run_picker():
            try:
                proc = subprocess.Popen(
                    [sys.executable, PICKER_PY],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                out, err = proc.communicate()
                if err:
                    print(f'SelectSources: picker stderr:\n{err.decode()}', flush=True)
                selection = out.decode().strip()
                print(f'SelectSources: picker returned "{selection}"', flush=True)
                GLib.idle_add(_on_picker_done, selection)
            except Exception as e:
                print(f'SelectSources: picker error: {e}', flush=True)
                GLib.idle_add(return_cb, dbus.UInt32(1),
                              dbus.Dictionary({}, signature='sv'))

        def _on_picker_done(selection):
            if selection:
                session.selection = selection
                return_cb(dbus.UInt32(0), dbus.Dictionary({}, signature='sv'))
            else:
                return_cb(dbus.UInt32(1), dbus.Dictionary({}, signature='sv'))
            return False

        threading.Thread(target=_run_picker, daemon=True).start()

    @dbus.service.method(SCREENCAST_IFACE, in_signature='oossa{sv}', out_signature='ua{sv}',
                         async_callbacks=('return_cb', 'error_cb'))
    def Start(self, handle, session_handle, app_id, parent_window, options,
              return_cb, error_cb):
        print(f'Start: app={app_id} full_options={dict(options)}', flush=True)
        try:
            session = self.sessions.get(str(session_handle))
            if not session:
                print('Start: session not found', flush=True)
                return_cb(dbus.UInt32(1), dbus.Dictionary({}, signature='sv'))
                return

            node_name = f'resonance-sc-{abs(hash(str(session_handle))) % 100000}'
            print(f'Start: node_name="{node_name}"', flush=True)
            print(f'Start: selection="{session.selection}"', flush=True)

            if not os.path.exists(SCREENCOPY_BIN):
                print(f'Start: screencopy binary not found at {SCREENCOPY_BIN}', flush=True)
                return_cb(dbus.UInt32(1), dbus.Dictionary({}, signature='sv'))
                return

            sel = session.selection or ''

            # capture_dmabuf: zero-copy GPU capture + PBO readback, same output
            # as screencopy.c. only monitors use it so far, windows use the old path
            if sel.startswith('monitor:'):
                monitor_name = sel[len('monitor:'):]
                print(f'Start: capturing monitor={monitor_name}', flush=True)
                if os.path.exists(DMABUF_CAPTURE_BIN):
                    capture_args = [DMABUF_CAPTURE_BIN, '--output-name', monitor_name]
                else:
                    capture_args = [SCREENCOPY_BIN, '--output-name', monitor_name]
            elif sel.startswith('window:'):
                capture_args = [SCREENCOPY_BIN]
                window_id = sel[len('window:'):]
                geom = get_window_geometry(window_id)
                if geom:
                    print(f'Start: capturing window id={window_id} geom={geom}', flush=True)
                    capture_args += ['--region',
                        f"{geom['x']},{geom['y']},{geom['w']},{geom['h']}"]
                    session.stream_position = (geom['x'], geom['y'])
                    session.stream_size = (geom['w'], geom['h'])
                else:
                    print(f'Start: geometry not found for window id={window_id}, falling back to output 0', flush=True)
            else:
                capture_args = [SCREENCOPY_BIN]

            proc = subprocess.Popen(capture_args, stdout=subprocess.PIPE)
            session.screencopy_proc = proc
            pipeline = Gst.parse_launch(
                'appsrc name=src format=time is-live=true block=true ! '
                'videoconvert ! '
                'pipewiresink name=pwsink sync=false'
            )
            print('Start: pipeline parsed OK', flush=True)

            pwsink = pipeline.get_by_name('pwsink')
            stream_props = Gst.Structure.new_empty('props')
            stream_props.set_value('node.name', node_name)
            stream_props.set_value('media.class', 'Video/Source')
            stream_props.set_value('media.role', 'Screen')
            pwsink.set_property('stream-properties', stream_props)
            session.pipeline = pipeline

            appsrc = pipeline.get_by_name('src')
            threading.Thread(
                target=_screencopy_reader,
                args=(proc, appsrc),
                daemon=True,
            ).start()

            gst_bus = pipeline.get_bus()
            gst_bus.add_watch(GLib.PRIORITY_DEFAULT,
                              lambda b, m: self._on_gst_message(b, m, session, node_name, return_cb))

            ret = pipeline.set_state(Gst.State.PLAYING)
            print(f'Start: set_state(PLAYING) returned {ret}', flush=True)
        except Exception:
            print(f'Start: exception:\n{traceback.format_exc()}', flush=True)
            return_cb(dbus.UInt32(1), dbus.Dictionary({}, signature='sv'))

    def _on_gst_message(self, bus, message, session, node_name, return_cb):
        try:
            if message.type == Gst.MessageType.STATE_CHANGED:
                if message.src == session.pipeline:
                    _, new, _ = message.parse_state_changed()
                    print(f'GST state_changed: {new.value_nick}', flush=True)
                    if new == Gst.State.PAUSED:
                        GLib.timeout_add(300, self._send_start_response,
                                         session, node_name, return_cb)
            elif message.type == Gst.MessageType.ERROR:
                err, dbg = message.parse_error()
                print(f'GStreamer error: {err} | {dbg}', flush=True)
                return_cb(dbus.UInt32(1), dbus.Dictionary({}, signature='sv'))
            elif message.type == Gst.MessageType.WARNING:
                warn, dbg = message.parse_warning()
                print(f'GStreamer warning: {warn} | {dbg}', flush=True)
        except Exception:
            print(f'_on_gst_message exception:\n{traceback.format_exc()}', flush=True)
        return True

    def _send_start_response(self, session, node_name, return_cb):
        node_id = find_pipewire_node_id(node_name)
        if node_id is None:
            print(f'Start: node "{node_name}" not found in PipeWire', flush=True)
            return_cb(dbus.UInt32(1), dbus.Dictionary({}, signature='sv'))
        else:
            print(f'Start: found PipeWire node id={node_id}', flush=True)
            stream_opts = {}
            if session.stream_position:
                stream_opts['position'] = dbus.Struct(
                    (dbus.Int32(session.stream_position[0]), dbus.Int32(session.stream_position[1])),
                    signature='ii')
            if session.stream_size:
                stream_opts['size'] = dbus.Struct(
                    (dbus.Int32(session.stream_size[0]), dbus.Int32(session.stream_size[1])),
                    signature='ii')
            streams = dbus.Array(
                [(dbus.UInt32(node_id), dbus.Dictionary(stream_opts, signature='sv'))],
                signature='(ua{sv})'
            )
            return_cb(dbus.UInt32(0), dbus.Dictionary(
                {'streams': streams}, signature='sv'))
        return False


if __name__ == '__main__':
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    mainloop = GLib.MainLoop()
    portal = ResonancePortal(bus, mainloop)

    display = os.environ.get('WAYLAND_DISPLAY', 'wayland-0')
    runtime_dir = os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')
    socket_path = os.path.join(runtime_dir, display)
    _socket_file = Gio.File.new_for_path(socket_path)
    _wayland_monitor = _socket_file.monitor_file(Gio.FileMonitorFlags.NONE, None)

    def _on_socket_changed(monitor, f, other_f, event):
        if event == Gio.FileMonitorEvent.DELETED:
            print('Wayland socket gone — shutting down spyglass', flush=True)
            mainloop.quit()

    _wayland_monitor.connect('changed', _on_socket_changed)

    mainloop.run()
