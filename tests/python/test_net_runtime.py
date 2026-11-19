"""scripts/prepare_net_runtime.py: the net capsule's derived iop/hw.cpp (LAN over the internet, docs/ONLINE-LAN.md)."""
import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'scripts/{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


net = load('prepare_net_runtime')
usb = load('prepare_usb_runtime')
DONOR = ROOT / net.DONOR_FILE


@unittest.skipUnless(DONOR.exists(), 'needs the pinned donor checkout under private/references')
class NetRuntimeTests(unittest.TestCase):
    def donor_text(self):
        return DONOR.read_bytes().decode('utf-8').replace('\r\n', '\n')

    def test_every_edit_point_is_found_once(self):
        derived = net.derive(self.donor_text())
        self.assertIn('#include "net_runtime.hpp"', derived)
        self.assertEqual(derived.count('if (net_address(a) && gt4::net::adapter_active()) return net_read(a, size);'), 1)
        self.assertEqual(derived.count('{ net_write(a, v, size); return; }'), 1)
        self.assertEqual(derived.count('net_adapter().dma(d.madr, words * 4u, (d.chcr & 1u) != 0);'), 1)
        self.assertEqual(derived.count('    net_reset();\n'), 1)

    def test_input_behaviour_is_kept_when_the_adapter_is_off(self):
        donor = self.donor_text()
        derived = net.derive(donor)
        # Every donor line survives: the capsule only adds lines, each behind adapter_active().
        remaining = iter(derived.splitlines())
        for line in donor.splitlines():
            self.assertTrue(any(line == candidate for candidate in remaining), line)

    def test_polls_on_the_iop_clock_and_uses_the_dev9_line(self):
        derived = net.derive(self.donor_text())
        self.assertIn('kernel::schedule(kernel::now() + 36864u, [generation] { net_tick(generation); });', derived)
        self.assertIn('raise_interrupt(gt4::net::kDev9InterruptLine)', derived)

    def test_builds_on_the_usb_capsule(self):
        both = net.derive(usb.derive(self.donor_text()))
        self.assertIn('usb_controller().read(', both)
        self.assertIn('net_adapter().poll();', both)

    def test_unknown_input_is_refused(self):
        with self.assertRaises(SystemExit):
            net.derive('// not iop/hw.cpp\n')
        with self.assertRaises(SystemExit):
            net.derive(net.derive(self.donor_text()))  # already derived: the edit points are no longer unique

    def test_prepare_writes_the_capsule_with_its_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            usb_out, out, alone = Path(folder) / 'usb', Path(folder) / 'net', Path(folder) / 'alone'
            usb.main([str(usb_out)])
            net.main([str(out), '--usb-capsule', str(usb_out)])
            receipt = json.loads((out / 'runtime.json').read_text(encoding='utf-8'))
            self.assertEqual(receipt['hw_sha256'], hashlib.sha256((out / 'hw.cpp').read_bytes()).hexdigest())
            self.assertEqual(receipt['usb_hw_sha256'], hashlib.sha256((usb_out / 'hw.cpp').read_bytes()).hexdigest())
            net.main([str(alone)])
            self.assertEqual(json.loads((alone / 'runtime.json').read_text(encoding='utf-8'))['input'], 'donor')
            with self.assertRaises(FileExistsError):
                net.main([str(out)])
            # A USB capsule that was edited after its pin is refused.
            (usb_out / 'hw.cpp').write_bytes((usb_out / 'hw.cpp').read_bytes() + b'\n')
            with self.assertRaises(SystemExit):
                net.main([str(Path(folder) / 'tampered'), '--usb-capsule', str(usb_out)])


if __name__ == '__main__':
    unittest.main()
