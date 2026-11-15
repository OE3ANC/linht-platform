# SPDX-License-Identifier: GPL-3.0-only
"""Stand-in for linht-shelld in the simulator (interfaces/org.m17project.LinHT.Shell1.xml).

Runs one app process at a time, like shelld runs one unit. Apps wrapping a systemd unit (the
Radio app, gui_test) do not exist on a PC: launching them fails, and "home" is the launcher.

    mock-shell.py <apps dir with manifests> <dir with the built app binaries>
"""
import asyncio
import pathlib
import signal
import sys

import yaml
from dbus_next import BusType, DBusError, Variant
from dbus_next.aio import MessageBus
from dbus_next.service import PropertyAccess, ServiceInterface, dbus_property, method

NAME, PATH, IFACE = "org.m17project.LinHT.Shell", "/org/m17project/LinHT/Shell", "org.m17project.LinHT.Shell1"
LAUNCHER = "org.m17project.linht.launcher"


def load_apps(apps_dir, bin_dir):
    apps = {}
    for manifest in sorted(pathlib.Path(apps_dir).glob("*/manifest.yaml")):
        m = yaml.safe_load(manifest.read_text())
        ui = next(c for c in m["components"] if c.get("type") == "ui")
        exe = pathlib.Path(bin_dir) / pathlib.Path(ui["exec"]).name if "exec" in ui else None
        apps[m["id"]] = {"name": m["name"], "category": m.get("category", ""), "exe": exe}
    return apps


class Shell(ServiceInterface):
    def __init__(self, apps, done):
        super().__init__(IFACE)
        self.apps, self.done, self.proc, self.fg = apps, done, None, ""

    @method()
    def ListApps(self) -> "aa{sv}":  # only apps that can run on a PC
        return [{"id": Variant("s", i), "name": Variant("s", a["name"]), "category": Variant("s", a["category"]),
                 "icon_small": Variant("s", ""), "icon_large": Variant("s", "")}
                for i, a in self.apps.items() if a["exe"] and a["exe"].exists()]

    # Sync methods: dbus-next mishandles DBusError raised from async ones. The switch runs as a task.
    @method()
    def Launch(self, app_id: "s"):
        app = self.apps.get(app_id)
        if not app:
            raise DBusError(IFACE + ".Error.UnknownApp", "no such app")
        if not app["exe"] or not app["exe"].exists():
            raise DBusError(IFACE + ".Error.LaunchFailed", f"{app_id} is not available in the simulator")
        asyncio.get_running_loop().create_task(self.switch(app_id))

    @method()
    def Home(self):
        asyncio.get_running_loop().create_task(self.switch(LAUNCHER))

    @dbus_property(access=PropertyAccess.READ)
    def Foreground(self) -> "s":
        return self.fg

    async def switch(self, app_id):
        if self.proc and self.proc.returncode is None:
            old, self.proc = self.proc, None
            old.terminate()
            await old.wait()
        self.set_fg("")
        self.proc = await asyncio.create_subprocess_exec(str(self.apps[app_id]["exe"]))
        self.set_fg(app_id)
        asyncio.get_running_loop().create_task(self.watch(self.proc, app_id))

    async def watch(self, proc, app_id):
        await proc.wait()
        if proc is not self.proc:
            return  # we stopped it ourselves
        if app_id == LAUNCHER:
            self.done.set()  # closing the launcher window ends the simulator
        else:
            await self.switch(LAUNCHER)  # an app that quit: back to the menu, like shelld

    def set_fg(self, app_id):
        self.fg = app_id
        self.emit_properties_changed({"Foreground": app_id})


async def main(apps_dir, bin_dir):
    done = asyncio.Event()
    bus = await MessageBus(bus_type=BusType.SESSION).connect()
    shell = Shell(load_apps(apps_dir, bin_dir), done)
    bus.export(PATH, shell)
    await bus.request_name(NAME)
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, done.set)
    await shell.switch(LAUNCHER)
    await done.wait()
    if shell.proc and shell.proc.returncode is None:
        shell.proc.terminate()
        await shell.proc.wait()


if __name__ == "__main__":
    asyncio.run(main(*sys.argv[1:3]))
