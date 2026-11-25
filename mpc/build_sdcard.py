#!/usr/bin/env python3
"""Builds the FM1 Sampler MPC plugin and assembles an SD card folder ready to copy to the MPC.

    python3 mpc/build_sdcard.py [--sdk PATH] [--looperx ZIP_OR_FOLDER] [--no-test]

Steps:
  1. Finds the official Akai/inMusic MPC Plugin SDK (--sdk, $MPC_SDK, or mpc/.sdk/mpc-plugin-builder-sdk, cloned there
     from https://github.com/inmusic-software/mpc-plugin-builder-sdk if missing).
  2. Copies mpc/FM1Sampler into the SDK's plugins/ folder and fills its Content/Samples with the FM1 factory samples
     of this repository (samples/<group>/*.wav).
  3. Runs the SDK's tools/build.py (native test build, ARM32 for the MPC One, ARM64, skins, validation) and the SDK's
     host simulator test.
  4. Writes mpc/dist/SD/ (VST2 Plugins/FM1 - VST - FM1 Sampler, an "FM1 Samples" folder, LEEME.txt), adds the LooperX
     plugins if --looperx points at their release zip or at a folder holding "VST2 Plugins", and zips it all into
     mpc/dist/FM1_LooperX_MPC_SD.zip.
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PROJECT = os.path.join(HERE, "FM1Sampler")
SDK_URL = "https://github.com/inmusic-software/mpc-plugin-builder-sdk.git"
FOLDER = "FM1 - VST - FM1 Sampler"


def run(argv, cwd=None):
    print("+ " + " ".join(argv), flush=True)
    return subprocess.run(argv, cwd=cwd).returncode


def find_sdk(arg):
    for cand in (arg, os.environ.get("MPC_SDK"), os.path.join(HERE, ".sdk", "mpc-plugin-builder-sdk")):
        if cand and os.path.isfile(os.path.join(cand, "tools", "build.py")):
            return os.path.abspath(cand)
    dest = os.path.join(HERE, ".sdk", "mpc-plugin-builder-sdk")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if run(["git", "clone", "--depth", "1", SDK_URL, dest]) != 0:
        sys.exit("Could not clone the MPC Plugin SDK; pass --sdk PATH")
    return dest


def stage_project(sdk):
    dst = os.path.join(sdk, "plugins", "FM1Sampler")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(PROJECT, dst, ignore=shutil.ignore_patterns("build", "dist", "Content"))
    samples = os.path.join(REPO, "samples")
    shutil.copytree(samples, os.path.join(dst, "Content", "Samples"),
                    ignore=lambda d, names: [n for n in names if not (os.path.isdir(os.path.join(d, n)) or n.lower().endswith(".wav"))])
    return dst


def add_looperx(src, vst_dir, sd):
    """Copies the LooperX plugin folders (and its LooperX/ data folder) from a release zip or a folder."""
    tmp = None
    if os.path.isfile(src):
        tmp = os.path.join(HERE, "dist", "_looperx")
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(src) as z:
            z.extractall(tmp)
        src = tmp
    root = None
    for base, dirs, _ in os.walk(src):
        if "VST2 Plugins" in dirs:
            root = base
            break
    if root is None:
        sys.exit("No 'VST2 Plugins' folder found in %s" % src)
    n = 0
    for name in sorted(os.listdir(os.path.join(root, "VST2 Plugins"))):
        if name.startswith("LooperX - VST - "):
            shutil.copytree(os.path.join(root, "VST2 Plugins", name), os.path.join(vst_dir, name))
            n += 1
    if os.path.isdir(os.path.join(root, "LooperX")):
        shutil.copytree(os.path.join(root, "LooperX"), os.path.join(sd, "LooperX"))
    for extra in ("LEEME_LooperX.txt",):
        if os.path.isfile(os.path.join(root, extra)):
            shutil.copy(os.path.join(root, extra), os.path.join(sd, extra))
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    print("LooperX: %d plugins added" % n)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk", help="path to mpc-plugin-builder-sdk")
    ap.add_argument("--looperx", help="LooperX release zip (or a folder containing 'VST2 Plugins') to put on the card too")
    ap.add_argument("--no-test", action="store_true", help="skip the host simulator test")
    args = ap.parse_args()

    sdk = find_sdk(args.sdk)
    proj = stage_project(sdk)
    if run([sys.executable, os.path.join(sdk, "tools", "build.py"), proj], cwd=sdk) != 0:
        sys.exit("build.py failed")
    out = os.path.join(proj, "build", FOLDER)
    if not args.no_test:
        host_sim = os.path.join(sdk, ".sdk-tools", "host_sim")
        native = [f for f in os.listdir(os.path.join(proj, "build", "native")) if f.endswith((".so", ".dylib", ".dll"))]
        if os.path.isfile(host_sim) and native:
            if run([host_sim, os.path.join(proj, "build", "native", native[0]), "test", "--seconds", "3", "--notes", "48,60,64,67"]) != 0:
                sys.exit("host_sim reported failures")

    dist = os.path.join(HERE, "dist")
    sd = os.path.join(dist, "SD")
    shutil.rmtree(sd, ignore_errors=True)
    vst = os.path.join(sd, "VST2 Plugins")
    os.makedirs(vst)
    shutil.copytree(out, os.path.join(vst, FOLDER))
    os.makedirs(os.path.join(sd, "FM1 Samples"))
    shutil.copy(os.path.join(HERE, "sdcard", "FM1 Samples", "LEEME.txt"), os.path.join(sd, "FM1 Samples", "LEEME.txt"))
    shutil.copy(os.path.join(HERE, "sdcard", "LEEME.txt"), os.path.join(sd, "LEEME.txt"))
    name = "FM1_MPC_SD.zip"
    if args.looperx:
        add_looperx(os.path.abspath(args.looperx), vst, sd)
        name = "FM1_LooperX_MPC_SD.zip"
    zpath = os.path.join(dist, name)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(sd):
            for f in sorted(files):
                full = os.path.join(base, f)
                z.write(full, os.path.relpath(full, sd))
    print("\nSD card folder: %s\nZip: %s (%.1f MB)" % (sd, zpath, os.path.getsize(zpath) / 1e6))


if __name__ == "__main__":
    main()
