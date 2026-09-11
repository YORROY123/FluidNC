"""Bound Arduino 3.3.9's stop-event wait, retaining resources on timeout."""
from pathlib import Path

OLD = '''    //wait for stop
    while (getStatusBits() & ESP_NETIF_STARTED_BIT) {
      delay(10);
    }'''
NEW = '''    // FluidNC: bound stop-event wait; retain handle/resources on timeout.
    const uint32_t stop_wait_started = millis();
    while (getStatusBits() & ESP_NETIF_STARTED_BIT) {
      if (uint32_t(millis() - stop_wait_started) >= 2000) {
        log_e("Ethernet stop event timeout; driver retained for recovery");
        return;
      }
      delay(10);
    }'''

def patch_text(text):
    if NEW in text:
        return text
    if text.count(OLD) != 1:
        raise RuntimeError('Unsupported Arduino ETH.cpp: expected stop-wait code not found exactly once')
    return text.replace(OLD, NEW)

if __name__ == '__main__':
    import unittest
    class PatchTests(unittest.TestCase):
        def test_only_expected_wait_changes(self):
            self.assertEqual(patch_text('before\n' + OLD + '\nafter'), 'before\n' + NEW + '\nafter')
        def test_idempotent(self):
            self.assertEqual(patch_text(patch_text(OLD)), NEW)
        def test_unknown_or_ambiguous_version_fails(self):
            for value in ('unknown', OLD + OLD):
                with self.assertRaises(RuntimeError): patch_text(value)
        def test_scons_hook_applies_patch(self):
            import tempfile
            import runpy
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / 'libraries/Ethernet/src/ETH.cpp'
                source.parent.mkdir(parents=True)
                source.write_text(OLD)
                class FakeEnv(dict):
                    def PioPlatform(self): return self
                    def get_package_dir(self, name): return directory
                runpy.run_path(__file__, init_globals={'Import': lambda name: None, 'env': FakeEnv(PIOENV='wifi_eth')})
                self.assertEqual(source.read_text(), NEW)
    unittest.main()
else:
    try:
        Import('env')
    except NameError:
        pass
    else:
        if env['PIOENV'] == 'wifi_eth':
            package = env.PioPlatform().get_package_dir('framework-arduinoespressif32')
            source = Path(package) / 'libraries/Ethernet/src/ETH.cpp'
            original = source.read_text(encoding='utf-8')
            patched = patch_text(original)
            if patched != original:
                source.write_text(patched, encoding='utf-8', newline='\n')
            print('FluidNC: Arduino Ethernet stop-event wait bounded to 2000 ms')
