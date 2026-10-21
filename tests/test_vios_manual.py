"""The manual stays in step with the sources: every word documented, the
generated reference current, every ```forth example running on the host."""
import contextlib
import io
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import vios_manual  # noqa: E402


class ManualTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        vios_manual.build()

    def quietly(self, fn):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            ok = fn()
        return ok, out.getvalue()

    def test_every_word_is_documented(self):
        ok, report = self.quietly(vios_manual.check)
        self.assertTrue(ok, report)

    def test_word_reference_is_current(self):
        text = vios_manual.MANUAL.read_text()
        current = vios_manual.BEGIN + text.split(vios_manual.BEGIN, 1)[1].split(vios_manual.END, 1)[0] + vios_manual.END
        self.assertEqual(current, vios_manual.render_reference(), 'run: python3 tools/vios_manual.py words')

    def test_examples_run(self):
        ok, report = self.quietly(vios_manual.examples)
        self.assertTrue(ok, report)


if __name__ == '__main__':
    unittest.main()
