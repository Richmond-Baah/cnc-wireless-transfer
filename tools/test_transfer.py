import os
import sys
import hashlib
import json
import unittest
import tempfile
import shutil

class TestCNCWirelessTransferSimulation(unittest.TestCase):
    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.storage_path = os.path.join(self.test_dir, "storage")
        os.makedirs(self.storage_path, exist_ok=True)

    def tearDown(self):
        shutil.rmtree(self.test_dir)

    def simulate_sanitize_filename(self, input_name):
        basename = os.path.basename(input_name.replace('\\', '/'))
        while basename.startswith('.') or basename.startswith(' '):
            basename = basename[1:]
        if not basename:
            basename = "job_default.nc"
        return basename

    def test_path_traversal_sanitization(self):
        self.assertEqual(self.simulate_sanitize_filename("../evil.gcode"), "evil.gcode")
        self.assertEqual(self.simulate_sanitize_filename("..\\evil.gcode"), "evil.gcode")
        self.assertEqual(self.simulate_sanitize_filename("/absolute/path.gcode"), "path.gcode")
        self.assertEqual(self.simulate_sanitize_filename("C:\\Windows\\file.gcode"), "file.gcode")
        self.assertEqual(self.simulate_sanitize_filename("..hidden.nc"), "hidden.nc")
        self.assertEqual(self.simulate_sanitize_filename(""), "job_default.nc")

    def test_file_sizes_and_chunking(self):
        # Test empty file, 1 KB, 8 KB, 8 KB + 1 byte
        test_sizes = [0, 1024, 8192, 8193, 32768]
        chunk_size = 4096

        for size in test_sizes:
            content = b"A" * size
            expected_md5 = hashlib.md5(content).hexdigest()
            filename = f"test_{size}.gcode"
            sanitized = self.simulate_sanitize_filename(filename)
            final_path = os.path.join(self.storage_path, sanitized)
            tmp_path = os.path.join(self.storage_path, sanitized + ".tmp")

            # Simulate chunked transfer
            with open(tmp_path, "wb") as f:
                if size == 0:
                    pass
                else:
                    for offset in range(0, size, chunk_size):
                        chunk = content[offset:offset + chunk_size]
                        f.write(chunk)

            # Verify size and MD5
            self.assertEqual(os.path.getsize(tmp_path), size)

            with open(tmp_path, "rb") as f:
                staged_content = f.read()
            computed_md5 = hashlib.md5(staged_content).hexdigest()
            self.assertEqual(computed_md5, expected_md5)

            # Atomic commit
            os.rename(tmp_path, final_path)
            self.assertTrue(os.path.exists(final_path))
            self.assertEqual(os.path.getsize(final_path), size)

    def test_md5_mismatch_cleanup(self):
        filename = "corrupt.nc"
        sanitized = self.simulate_sanitize_filename(filename)
        final_path = os.path.join(self.storage_path, sanitized)
        tmp_path = os.path.join(self.storage_path, sanitized + ".tmp")

        content = b"BAD DATA"
        bad_md5 = "00000000000000000000000000000000"

        with open(tmp_path, "wb") as f:
            f.write(content)

        computed_md5 = hashlib.md5(content).hexdigest()
        self.assertNotEqual(computed_md5, bad_md5)

        # Mismatch -> delete tmp file
        if computed_md5 != bad_md5:
            os.remove(tmp_path)

        self.assertFalse(os.path.exists(tmp_path))
        self.assertFalse(os.path.exists(final_path))

    def test_orphan_temp_cleanup_simulation(self):
        valid_file = os.path.join(self.storage_path, "part1.gcode")
        orphan_tmp = os.path.join(self.storage_path, ".transfer1.tmp")
        orphan_tmp2 = os.path.join(self.storage_path, "job2.nc.tmp")

        with open(valid_file, "wb") as f:
            f.write(b"M30")
        with open(orphan_tmp, "wb") as f:
            f.write(b"partial")
        with open(orphan_tmp2, "wb") as f:
            f.write(b"partial2")

        for entry in os.listdir(self.storage_path):
            if entry.startswith(".") or ".tmp" in entry:
                path = os.path.join(self.storage_path, entry)
                os.remove(path)

        self.assertTrue(os.path.exists(valid_file))
        self.assertFalse(os.path.exists(orphan_tmp))
        self.assertFalse(os.path.exists(orphan_tmp2))

    def test_extended_file_extensions(self):
        allowed_exts = ['.nc', '.gcode', '.tap', '.cnc', '.gc', '.ngc', '.pcn', '.plt', '.hnc', '.txt', '.crv', '.sbp', '.dxf', '.svg', '.xcn', '.din', '.cut', '.iso']
        for ext in allowed_exts:
            fname = f"test_file{ext}"
            sanitized = self.simulate_sanitize_filename(fname)
            self.assertTrue(sanitized.endswith(ext))

    def test_retry_logic_simulation(self):
        # Simulate chunk upload retry mechanism (max 3 attempts)
        max_retries = 3
        
        def mock_upload_chunk(chunk_id, fail_count):
            attempts = 0
            while attempts < max_retries:
                attempts += 1
                if attempts <= fail_count:
                    continue # simulate failure
                return True # success
            return False # final failure

        # Success on 1st try
        self.assertTrue(mock_upload_chunk(1, fail_count=0))
        # Success on 2nd try (after 1 failure)
        self.assertTrue(mock_upload_chunk(2, fail_count=1))
        # Success on 3rd try (after 2 failures)
        self.assertTrue(mock_upload_chunk(3, fail_count=2))
        # Failure after 3 failures (exceeds max_retries)
        self.assertFalse(mock_upload_chunk(4, fail_count=3))

    def test_authentication_simulation(self):
        valid_token = "auth_token_secret_123"
        
        def check_auth(token):
            return token == valid_token

        self.assertTrue(check_auth("auth_token_secret_123"))
        self.assertFalse(check_auth("invalid_token"))
        self.assertFalse(check_auth(""))

if __name__ == '__main__':
    unittest.main()
