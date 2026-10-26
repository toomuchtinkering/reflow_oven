"""PlatformIO pre-build hook: regenerate src/web/tuning_html_gz.c from the web
sources before every build, so editing tuning.html / fit.js / app.js can never
ship a stale embedded page. Runs at SConscript-load time (before the source
glob), and build_web_asset.py only rewrites the .c when its bytes change, so an
unchanged page costs nothing. Wired via `extra_scripts = pre:...` in platformio.ini.
"""
Import("env")  # noqa: F821  (SCons injects `env` and `Import` into this scope)
import subprocess, os

project_dir = env["PROJECT_DIR"]  # noqa: F821
script = os.path.join(project_dir, "tools", "build_web_asset.py")
print("build_web_asset: regenerating embedded tuning SPA")
subprocess.run(["python3", script], check=True)
