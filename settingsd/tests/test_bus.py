# SPDX-License-Identifier: GPL-3.0-only
"""D-Bus tests for linht-settingsd. Run under dbus-run-session (see CMakeLists.txt):

    test_bus.py <path to linht-settingsd> <schemas dir> <interfaces dir>
"""
import asyncio
import getpass
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from dbus_next import Message, MessageType, Variant
from dbus_next.aio import MessageBus

DAEMON, SCHEMAS, INTERFACES = sys.argv[1:4]
NAME, PATH, IFACE = "org.m17project.LinHT.Settings", "/org/m17project/LinHT/Settings", "org.m17project.LinHT.Settings1"
ERR = IFACE + ".Error."


async def wait_for(condition, what, timeout=5.0):
    """Polls until condition() is true; fails with a clear message instead of a guessed sleep."""
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout
    while not condition():
        if loop.time() > end:
            raise AssertionError(f"timed out waiting for {what}")
        await asyncio.sleep(0.02)


CALLER, NOBODY = getpass.getuser(), "no-such-user-linht-test"


class SettingsBus(unittest.IsolatedAsyncioTestCase):
    schemas = SCHEMAS
    device_user = CALLER  # the test caller counts as the on-device UI
    web_user = NOBODY

    async def asyncSetUp(self):
        self.store = tempfile.TemporaryDirectory()
        self.bus = await asyncio.wait_for(MessageBus().connect(), 5)
        self.signals = []
        self.bus.add_message_handler(
            lambda m: self.signals.append(m.body) if m.message_type == MessageType.SIGNAL and m.member == "Changed" else None
        )
        await self.dbus("AddMatch", "s", [f"type='signal',interface='{IFACE}',member='Changed'"])
        await self.start()

    async def asyncTearDown(self):
        self.stop()
        self.bus.disconnect()
        self.store.cleanup()

    async def dbus(self, member, sig, body):
        return await self.bus.call(Message(destination="org.freedesktop.DBus", path="/org/freedesktop/DBus",
                                           interface="org.freedesktop.DBus", member=member, signature=sig, body=body))

    async def start(self):
        self.proc = subprocess.Popen([DAEMON, "--session", "--schemas", self.schemas, "--store", self.store.name,
                                      "--device-user", self.device_user, "--web-user", self.web_user])
        loop = asyncio.get_running_loop()
        end = loop.time() + 5
        while not (await self.dbus("NameHasOwner", "s", [NAME])).body[0]:
            self.assertIsNone(self.proc.poll(), "daemon exited during startup")
            self.assertLess(loop.time(), end, "daemon did not take its bus name")
            await asyncio.sleep(0.02)

    def stop(self):
        self.proc.terminate()
        self.assertEqual(self.proc.wait(5), 0, "daemon did not exit cleanly on SIGTERM")

    async def call(self, member, sig="", body=(), iface=IFACE):
        return await self.bus.call(Message(destination=NAME, path=PATH, interface=iface, member=member,
                                           signature=sig, body=list(body)))

    async def ok(self, member, sig="", *body, iface=IFACE):
        r = await self.call(member, sig, body, iface)
        self.assertEqual(r.message_type, MessageType.METHOD_RETURN, f"{member} failed: {r.error_name} {r.body}")
        return r.body

    async def error(self, member, sig, *body):
        r = await self.call(member, sig, body)
        self.assertEqual(r.message_type, MessageType.ERROR, f"{member} unexpectedly succeeded")
        return r.error_name, r.body[0] if r.body else ""

    async def get(self, ns, key):
        return (await self.ok("Get", "ss", ns, key))[0].value


class Contract(SettingsBus):
    async def test_introspection_matches_contract_xml(self):
        def members(iface):
            out = {}
            for m in iface:
                if m.tag in ("method", "signal"):
                    out[(m.tag, m.get("name"))] = [(a.get("name"), a.get("type"), a.get("direction", "out" if m.tag == "signal" else "in"))
                                                   for a in m.findall("arg")]
            return out

        contract = ET.parse(pathlib.Path(INTERFACES) / f"{IFACE}.xml").getroot().find(f"interface[@name='{IFACE}']")
        live_xml = (await self.ok("Introspect", iface="org.freedesktop.DBus.Introspectable"))[0]
        live = ET.fromstring(live_xml).find(f"interface[@name='{IFACE}']")
        self.assertIsNotNone(live, "interface not exported")
        self.assertEqual(members(live), members(contract))


class Reading(SettingsBus):
    async def test_list_namespaces(self):
        (namespaces,) = await self.ok("ListNamespaces")
        self.assertEqual(sorted(namespaces), ["system.developer", "system.display", "system.keypad", "system.shell"])

    async def test_describe_in_schema_order_with_typed_values(self):
        title, keys = await self.ok("Describe", "s", "system.display")
        self.assertEqual(title, "Display")
        self.assertEqual([k[0] for k in keys], ["brightness", "timeout"])
        b = dict(keys)["brightness"]
        self.assertEqual(b["type"].value, "int")
        self.assertEqual((b["default"].signature, b["default"].value), ("x", 255))
        self.assertEqual((b["min"].signature, b["min"].value), ("x", 32))
        self.assertEqual(b["apply"].value, "live")
        self.assertEqual(b["access"].value, "user")
        self.assertEqual((b["order"].signature, b["order"].value), ("x", 1))
        t = dict(keys)["timeout"]
        self.assertEqual(t["choices"].signature, "av")
        self.assertEqual([v.value for v in t["choices"].value], [0, 5, 10, 15, 30, 60])
        self.assertEqual(t["unit"].value, "s")
        self.assertNotIn("min", t)  # absent fields are left out, not sent as zero

    async def test_get_and_get_all_return_defaults(self):
        self.assertEqual(await self.get("system.display", "brightness"), 255)
        (values,) = await self.ok("GetAll", "s", "system.keypad")
        self.assertEqual({k: v.value for k, v in values.items()}, {"backlight_brightness": 255, "backlight_timeout": 10})

    async def test_unknown_namespace_and_key(self):
        self.assertEqual((await self.error("Get", "ss", "nope", "x"))[0], ERR + "UnknownNamespace")
        self.assertEqual((await self.error("Get", "ss", "system.display", "nope"))[0], ERR + "UnknownKey")
        self.assertEqual((await self.error("Describe", "s", "nope"))[0], ERR + "UnknownNamespace")


class Writing(SettingsBus):
    async def test_set_stores_value_emits_changed_and_persists(self):
        await self.ok("Set", "ssv", "system.display", "brightness", Variant("x", 96))
        self.assertEqual(await self.get("system.display", "brightness"), 96)
        await wait_for(lambda: self.signals, "Changed")
        ns, values = self.signals[0]
        self.assertEqual((ns, {k: v.value for k, v in values.items()}), ("system.display", {"brightness": 96}))
        self.assertTrue((pathlib.Path(self.store.name) / "system.display.yaml").exists())
        self.stop()
        await self.start()
        self.assertEqual(await self.get("system.display", "brightness"), 96)

    async def test_invalid_values_are_rejected(self):
        name, msg = await self.error("Set", "ssv", "system.display", "brightness", Variant("x", 5))
        self.assertEqual(name, ERR + "InvalidValue")
        self.assertIn("min", msg)
        name, msg = await self.error("Set", "ssv", "system.display", "brightness", Variant("u", 100))
        self.assertEqual(name, ERR + "InvalidValue")  # exact signature required, no conversion
        self.assertIn("type", msg)
        self.assertEqual(await self.get("system.display", "brightness"), 255)

    async def test_set_many_is_all_or_nothing(self):
        name, _ = await self.error("SetMany", "sa{sv}", "system.display",
                                   {"timeout": Variant("x", 30), "brightness": Variant("x", 1000)})
        self.assertEqual(name, ERR + "InvalidValue")
        self.assertEqual(await self.get("system.display", "timeout"), 15)
        name, _ = await self.error("SetMany", "sa{sv}", "system.display", {"timeout": Variant("x", 30), "nope": Variant("x", 1)})
        self.assertEqual(name, ERR + "UnknownKey")
        self.assertEqual(await self.get("system.display", "timeout"), 15)

        await self.ok("SetMany", "sa{sv}", "system.display", {"timeout": Variant("x", 30), "brightness": Variant("x", 64)})
        await wait_for(lambda: self.signals, "Changed")
        self.assertEqual(len(self.signals), 1)  # one signal per call; the failed calls sent none
        self.assertEqual({k: v.value for k, v in self.signals[0][1].items()}, {"timeout": 30, "brightness": 64})

    async def test_unchanged_value_emits_nothing(self):
        await self.ok("Set", "ssv", "system.display", "timeout", Variant("x", 15))  # already the default
        await self.ok("Set", "ssv", "system.display", "timeout", Variant("x", 60))
        await wait_for(lambda: self.signals, "Changed")
        # signals from one sender arrive in order: if the no-op had signalled, it would be first
        self.assertEqual({k: v.value for k, v in self.signals[0][1].items()}, {"timeout": 60})

    async def test_reset_returns_default_and_signals_it(self):
        await self.ok("Set", "ssv", "system.keypad", "backlight_timeout", Variant("x", 60))
        await self.ok("Reset", "ss", "system.keypad", "backlight_timeout")
        self.assertEqual(await self.get("system.keypad", "backlight_timeout"), 10)
        await wait_for(lambda: len(self.signals) == 2, "two Changed signals")
        self.assertEqual({k: v.value for k, v in self.signals[1][1].items()}, {"backlight_timeout": 10})

    async def test_device_only_key_writable_by_device_user(self):
        await self.ok("Set", "ssv", "system.developer", "enabled", Variant("b", True))
        self.assertTrue(await self.get("system.developer", "enabled"))


class RemoteCaller(SettingsBus):
    device_user = NOBODY  # the test caller is now a remote client

    async def test_device_only_key_is_denied(self):
        name, _ = await self.error("Set", "ssv", "system.developer", "enabled", Variant("b", True))
        self.assertEqual(name, ERR + "AccessDenied")
        name, _ = await self.error("SetMany", "sa{sv}", "system.developer", {"enabled": Variant("b", True)})
        self.assertEqual(name, ERR + "AccessDenied")
        name, _ = await self.error("Reset", "ss", "system.developer", "enabled")
        self.assertEqual(name, ERR + "AccessDenied")
        self.assertFalse(await self.get("system.developer", "enabled"))

    async def test_user_key_is_allowed(self):
        await self.ok("Set", "ssv", "system.display", "brightness", Variant("x", 128))


class AccessLevels(SettingsBus):
    """One key per access level (docs/settings-schema.md). The test caller is never root."""

    async def asyncSetUp(self):
        self.schema_dir = tempfile.TemporaryDirectory()
        self.schemas = self.schema_dir.name
        levels = "".join(f"  {a.replace('-', '_')}: {{type: bool, default: false, label: {a}, access: {a}, order: {i}}}\n"
                         for i, a in enumerate(("user", "admin", "device-only", "factory"), 1))
        (pathlib.Path(self.schemas) / "test.access.yaml").write_text(
            f"namespace: test.access\nversion: 1\ntitle: Access\nkeys:\n{levels}")
        await super().asyncSetUp()

    async def asyncTearDown(self):
        await super().asyncTearDown()
        self.schema_dir.cleanup()

    async def check_levels(self, **allowed):
        for key, ok in allowed.items():
            r = await self.call("Set", "ssv", ("test.access", key, Variant("b", True)))
            if ok:
                self.assertEqual(r.message_type, MessageType.METHOD_RETURN, f"{key}: {r.error_name} {r.body}")
            else:
                self.assertEqual(r.error_name, ERR + "AccessDenied", f"{key} was writable")


class AccessAsDeviceUser(AccessLevels):
    async def test_levels(self):
        await self.check_levels(user=True, admin=True, device_only=True, factory=False)


class AccessAsWebUser(AccessLevels):
    device_user, web_user = NOBODY, CALLER

    async def test_levels(self):
        await self.check_levels(user=True, admin=True, device_only=False, factory=False)


class AccessAsOtherUser(AccessLevels):
    device_user = NOBODY

    async def test_levels(self):
        await self.check_levels(user=True, admin=False, device_only=False, factory=False)


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1], verbosity=2)
