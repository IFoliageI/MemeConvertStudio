"""Synthetic child/grandchild for the Windows Job Object check."""
import json
from pathlib import Path
import subprocess
import sys
import time
import os

config = json.load(sys.stdin)
grandchild = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(300)"],
                              creationflags=subprocess.CREATE_NO_WINDOW)
Path(config["marker"]).write_text(json.dumps({"child": os.getpid(), "grandchild": grandchild.pid}))
while True:
    time.sleep(1)
