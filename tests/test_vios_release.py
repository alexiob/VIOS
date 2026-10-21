"""VERSION, include/vios/version.h and CHANGELOG.md agree (docs/RELEASING.md)."""
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import vios_release  # noqa: E402


class ReleaseTests(unittest.TestCase):
    def test_version_is_semver(self):
        self.assertRegex(vios_release.version(), r'^\d+\.\d+\.\d+$')

    def test_header_matches_version(self):
        header = (ROOT / 'include/vios/version.h').read_text()
        self.assertEqual(header, vios_release.version_header(vios_release.version()),
                         'run tools/vios_release.py sync (or make) after editing VERSION')

    def test_banner_is_major_minor(self):
        header = vios_release.version_header('1.23.4')
        self.assertIn('#define VIOS_BANNER "VIOS v1.23"', header)
        self.assertIn('#define VIOS_COPYRIGHT "(c) 2026 Alessandro Iob"', header)

    def test_changelog_has_this_version(self):
        text = (ROOT / 'CHANGELOG.md').read_text()
        self.assertIn('## [Unreleased]', text)
        notes = vios_release.notes(vios_release.version())
        self.assertIn('### ', notes)                 # the version's sections, not an empty heading
        self.assertNotIn('## [', notes)              # one section only
        with self.assertRaises(SystemExit):
            vios_release.notes('99.99.99')


if __name__ == '__main__':
    unittest.main()
