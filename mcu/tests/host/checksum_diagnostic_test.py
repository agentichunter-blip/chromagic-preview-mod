"""Run the ROM upload regression and require its nonfatal warning on stdout.

The host ESP_LOG stubs are disabled, matching the production log configuration.
"""
import re
import subprocess
import sys

result = subprocess.run([sys.argv[1], 'rom'], capture_output=True, text=True)
sys.stdout.write(result.stdout)
sys.stderr.write(result.stderr)
assert result.returncode == 0, f'Upload regression failed: {result.returncode}'
warnings = re.findall(r'VIRTUAL_CART_CHECKSUM_WARNING stored=([0-9a-f]{4}) actual=([0-9a-f]{4})', result.stdout)
assert len(warnings) == 1 and warnings[0][0] != warnings[0][1], warnings
