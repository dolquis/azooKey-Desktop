"""Portable release SBOM regression tests; no compiler or downloaded models."""

import copy
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("release_sbom", ROOT / "scripts/complete-release-sbom.py")
SBOM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SBOM)


class ReleaseSbomTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("CMakeLists.txt", "THIRD_PARTY_LICENSES", "settings-app/packages.lock.json"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        self.build = self.root / "build"
        self.build.mkdir()
        self.pins = dict(re.findall(r'set\((AZOOKEY_\w+_GIT_TAG)\s+"([0-9a-f]{40})"',
                                    (self.root / "CMakeLists.txt").read_text(encoding="utf-8")))
        self.cache = self.build / "CMakeCache.txt"
        self.cache.write_text("".join(f"{k}:STRING={v}\n" for k, v in self.pins.items()), encoding="utf-8")
        self.runtime = []
        for name in sorted(SBOM.RUNTIME_FILES):
            path = self.root / name
            path.write_bytes(name.encode())
            self.runtime.append({"name": name, "version": "14.44.35211.0", "sha256": SBOM.sha256(path)})
        self.runtime_info = self.root / "runtime.json"
        self.runtime_info.write_text(json.dumps(self.runtime), encoding="utf-8")
        self.msi = self.root / "azooKey test.msi"
        self.msi.write_bytes(b"fixture, not a real MSI")
        self.document = {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
                         "name": "syft-fixture", "documentNamespace": "https://example.test/sbom/1",
                         "creationInfo": {"created": "2026-09-05T00:00:00Z", "creators": ["Tool: syft"]},
                         "packages": [{"SPDXID": "SPDXRef-original", "name": "existing", "downloadLocation": "NOASSERTION"}],
                         "relationships": [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-original"}]}

    def git(self, args, **_):
        if "status" in args:
            return ""
        key = "AZOOKEY_LLAMA_CPP_GIT_TAG" if "llama_cpp-src" in args[2] else "AZOOKEY_WIL_GIT_TAG"
        return self.pins[key] + "\n"

    def generate(self):
        with patch.object(SBOM.subprocess, "check_output", side_effect=self.git):
            return SBOM.complete(self.document, self.root, self.build, self.runtime_info, self.root, self.msi)

    def test_release_inventory_preserves_syft_and_links_dependencies(self):
        before = copy.deepcopy(self.document)
        result = self.generate()
        self.assertEqual(before, self.document)
        self.assertEqual(result["packages"][0], before["packages"][0])
        self.assertEqual(result["relationships"][0], before["relationships"][0])
        packages = {x["name"]: x for x in result["packages"]}
        self.assertEqual(packages["llama.cpp"]["versionInfo"], self.pins["AZOOKEY_LLAMA_CPP_GIT_TAG"])
        self.assertEqual(packages["WIL"]["versionInfo"], self.pins["AZOOKEY_WIL_GIT_TAG"])
        for entry in self.runtime:
            self.assertEqual(packages[entry["name"]]["versionInfo"], entry["version"])
            self.assertEqual(packages[entry["name"]]["checksums"][0]["checksumValue"], entry["sha256"])
        for name, (version, _) in SBOM.nuget_packages(self.root).items():
            if name in SBOM.BUILD_ONLY:
                self.assertNotIn(name, packages)
            else:
                self.assertEqual(packages[name]["versionInfo"], version)
        self.assertNotIn("GoogleTest", packages)
        self.assertIn("GoogleTest", result["annotations"][0]["comment"])
        self.assertEqual({x["licenseId"] for x in result["hasExtractedLicensingInfos"]},
                         {"LicenseRef-MSVC-runtime", "LicenseRef-Windows-App-SDK"})
        root_id = packages[self.msi.name]["SPDXID"]
        dependencies = {x["relatedSpdxElement"] for x in result["relationships"] if x["spdxElementId"] == root_id}
        self.assertEqual(dependencies, {x["SPDXID"] for x in result["packages"][2:]})

    def test_cache_pin_mismatch_is_rejected(self):
        self.cache.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "cache differs"):
            self.generate()

    def test_checkout_mismatch_and_dirty_source_are_rejected(self):
        for response in ("0" * 40, " M include/test.h"):
            with self.subTest(response=response), patch.object(SBOM.subprocess, "check_output", return_value=response):
                with self.assertRaisesRegex(ValueError, "source differs"):
                    SBOM.cmake_pin(self.root, self.build, "AZOOKEY_WIL_GIT_TAG")

    def test_local_source_override_is_rejected(self):
        with self.cache.open("a", encoding="utf-8") as stream:
            stream.write(f"AZOOKEY_WIL_SOURCE_DIR:PATH={self.root.as_posix()}\n")
        with self.assertRaisesRegex(ValueError, "local source override"):
            self.generate()

    def test_missing_changed_and_duplicate_runtime_are_rejected(self):
        for entries in (self.runtime[:2], [self.runtime[0]] * 3):
            self.runtime_info.write_text(json.dumps(entries), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "three app-local"):
                self.generate()
        self.runtime_info.write_text(json.dumps(self.runtime), encoding="utf-8")
        (self.root / self.runtime[0]["name"]).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "CRT changed"):
            self.generate()

    def test_missing_version_is_rejected(self):
        self.runtime[0]["version"] = ""
        self.runtime_info.write_text(json.dumps(self.runtime), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "numeric CRT"):
            self.generate()

    def test_unknown_nuget_dependency_is_rejected(self):
        path = self.root / "settings-app/packages.lock.json"
        data = SBOM.read_json(path)
        next(iter(data["dependencies"].values()))["Unreviewed.Package"] = {"resolved": "1.0", "contentHash": ""}
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "coverage differ"):
            self.generate()

    def test_missing_attribution_is_rejected(self):
        path = self.root / "THIRD_PARTY_LICENSES"
        text = path.read_text(encoding="utf-8")
        text = re.sub(r'<!-- sbom: .*?"name":"WIL".*? -->', "", text)
        path.write_text(text, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "one SBOM selector"):
            self.generate()

    def test_removed_section_and_license_drift_are_rejected(self):
        path = self.root / "THIRD_PARTY_LICENSES"
        original = path.read_text(encoding="utf-8")
        path.write_text(re.sub(r"## 配布物へ同梱する WIL.*?(?=\n## )", "", original, flags=re.S), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "coverage differs"):
            self.generate()
        path.write_text(original.replace("- License: MIT License", "- License: Apache-2.0", 1), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "attribution license"):
            self.generate()

    def test_unknown_spdx_version_and_repeated_completion_are_rejected(self):
        self.document = self.generate()
        with self.assertRaisesRegex(ValueError, "Duplicate SBOM"):
            self.generate()
        self.document["spdxVersion"] = "SPDX-3.0"
        with self.assertRaisesRegex(ValueError, "SPDX-2.3"):
            self.generate()

    def test_workflow_completes_before_attestation_and_upload(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertLess(workflow.index("Complete SBOM with release dependencies"), workflow.index("- name: Attest SBOM"))
        self.assertIn("--input azooKey.syft.spdx.json --output azooKey.spdx.json", workflow)
        self.assertIn("git diff --exit-code -- settings-app/packages.lock.json", workflow)
        self.assertIn("sbom-path: ${{ github.workspace }}\\azooKey.spdx.json", workflow)

    def test_cli_with_real_git_checkouts_and_rejected_write(self):
        # Exercise Git, argument quoting, UTF-8 JSON, and the file-output boundary.
        cmake = self.root / "CMakeLists.txt"
        text = cmake.read_text(encoding="utf-8")
        for key, dependency in (("AZOOKEY_LLAMA_CPP_GIT_TAG", "llama_cpp"), ("AZOOKEY_WIL_GIT_TAG", "wil")):
            directory = self.build / "_deps" / (dependency + "-src")
            directory.mkdir(parents=True)
            def git(*args):
                return subprocess.check_output(["git", "-C", str(directory), *args], text=True, stderr=subprocess.STDOUT).strip()
            git("init")
            (directory / "CMakeLists.txt").write_text("# Test source\n", encoding="utf-8")
            git("add", "CMakeLists.txt")
            git("-c", "user.name=SBOM Test", "-c", "user.email=sbom@example.test", "-c", "commit.gpgsign=false", "commit", "-m", "fixture")
            pin = git("rev-parse", "HEAD")
            text = text.replace(self.pins[key], pin)
            self.pins[key] = pin
        cmake.write_text(text, encoding="utf-8")
        self.cache.write_text("".join(f"{k}:STRING={v}\n" for k, v in self.pins.items()), encoding="utf-8")
        source = self.root / "syft.json"
        source.write_text(json.dumps(self.document), encoding="utf-8")
        output = self.root / "output.json"
        args = [sys.executable, str(ROOT / "scripts/complete-release-sbom.py"), "--root", str(self.root),
                "--build-dir", str(self.build), "--runtime-info", str(self.runtime_info),
                "--runtime-dir", str(self.root), "--msi", str(self.msi), "--input", str(source), "--output", str(output)]
        good = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(good.returncode, 0, good.stderr)
        before = output.read_bytes()
        self.assertIn("WIL", {p["name"] for p in SBOM.read_json(output)["packages"]})
        (self.root / self.runtime[0]["name"]).write_bytes(b"changed")
        bad = subprocess.run(args, capture_output=True, text=True)
        self.assertNotEqual(bad.returncode, 0)
        self.assertEqual(output.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
