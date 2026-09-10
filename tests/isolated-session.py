"""Run integration tests with private settings, files, D-Bus and optionally X11."""
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile


def main():
    gui = sys.argv[1] == "--gui"
    command = sys.argv[2:]
    with tempfile.TemporaryDirectory(prefix="otpclient-session-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        env.update(
            GSETTINGS_BACKEND="memory", GTK_A11Y="none", GTK_USE_PORTAL="0",
            GIO_USE_VFS="local", XDG_CURRENT_DESKTOP="", GSK_RENDERER="cairo",
            XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data"),
            XDG_CACHE_HOME=str(root / "cache"), XDG_RUNTIME_DIR=str(root / "runtime"),
            DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"),
            LC_ALL="C", TMPDIR=directory,
        )
        (root / "runtime").mkdir(mode=0o700)
        # No service directories: tests must never activate the host keyring,
        # portals, clipboard managers or notification service.
        config = root / "bus.conf"
        config.write_text(
            '<busconfig><type>session</type><listen>unix:tmpdir=' + directory + '</listen>'
            '<auth>EXTERNAL</auth><policy context="default"><allow own="*"/>'
            '<allow send_destination="*"/><allow receive_sender="*"/>'
            '</policy></busconfig>'
        )
        processes = []
        try:
            bus = subprocess.Popen(
                ["dbus-daemon", "--nofork", "--print-address=1", "--config-file=" + str(config)],
                stdout=subprocess.PIPE, text=True,
            )
            processes.append(bus)
            if not select.select([bus.stdout], [], [], 5)[0]:
                raise RuntimeError("Private D-Bus failed to start")
            env["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().strip()
            if not env["DBUS_SESSION_BUS_ADDRESS"]:
                raise RuntimeError("Private D-Bus returned no address")
            if gui:
                read_fd, write_fd = os.pipe()
                try:
                    with (root / "xvfb.log").open("w") as log:
                        display = subprocess.Popen(
                            ["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                             "1280x1024x24", "-nolisten", "tcp", "-ac"],
                            pass_fds=(write_fd,), stdout=log, stderr=log,
                        )
                    processes.append(display)
                    os.close(write_fd)
                    write_fd = None
                    if not select.select([read_fd], [], [], 8)[0]:
                        raise RuntimeError("Xvfb failed: " + (root / "xvfb.log").read_text()[-2000:])
                    number = os.read(read_fd, 100).decode().strip()
                    if not number:
                        raise RuntimeError("Xvfb returned no display")
                    env.update(DISPLAY=":" + number, GDK_BACKEND="x11")
                finally:
                    os.close(read_fd)
                    if write_fd is not None:
                        os.close(write_fd)
            return subprocess.run(command, env=env, timeout=90).returncode
        finally:
            for process in reversed(processes):
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    sys.exit(main())
