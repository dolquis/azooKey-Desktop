"""Enrich the MSI's Syft SPDX 2.3 document with declared build inputs.

Attribution comes from THIRD_PARTY_LICENSES; versions are never copied here.
Only the canonical release build (FetchContent, locked NuGet, app-local CRT)
is supported. Missing or inconsistent evidence fails before writing output.
"""

import argparse
import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess


RUNTIME_FILES = {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}
BUILD_ONLY = {"Microsoft.Windows.SDK.BuildTools", "Microsoft.Windows.SDK.BuildTools.MSIX"}


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def inventory(root):
    """Read version selectors and licenses from their attribution sections."""
    text = (root / "THIRD_PARTY_LICENSES").read_text(encoding="utf-8")
    result = []
    for section in re.split(r"(?m)^## ", text)[1:]:
        markers = re.findall(r"<!-- sbom: (.+?) -->", section)
        if section.startswith("配布物へ同梱する") and len(markers) != 1:
            raise ValueError("Redistributed attribution needs one SBOM selector")
        for marker in markers:
            item = json.loads(marker)
            license_line = re.search(r"(?m)^- License: (.+)", section)
            if not license_line:
                raise ValueError("SBOM selector has no attribution license")
            if not item["license"].startswith("LicenseRef-") and not license_line[1].startswith(item["license"]):
                raise ValueError("SPDX identifier differs from attribution license")
            item["attribution"] = re.sub(r"<!-- sbom: .+? -->\n?", "", section).strip()
            result.append(item)
    sources = [item["source"] for item in result]
    if len(sources) != len(set(sources)) or not result:
        raise ValueError("Missing or duplicate SBOM selectors")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    required = {"cmake:" + key for key in re.findall(r"set\((AZOOKEY_\w+_GIT_TAG)\s", cmake)} | {"runtime"}
    if {source for source in sources if not source.startswith("nuget:")} != required:
        raise ValueError("CMake/runtime attribution coverage differs")
    return result


def nuget_packages(root):
    packages = {}
    for target in read_json(root / "settings-app/packages.lock.json")["dependencies"].values():
        for name, entry in target.items():
            value = (entry["resolved"], entry["contentHash"])
            if name in packages and packages[name] != value:
                raise ValueError(f"Conflicting NuGet resolutions: {name}")
            packages[name] = value
    return packages


def cmake_pin(root, build_dir, key):
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"set\(" + re.escape(key) + r'\s+"([0-9a-f]{40})"', cmake)
    if not match:
        raise ValueError(f"Missing full SHA pin: {key}")
    pin = match[1]
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    if not re.search(r"(?m)^" + key + ":STRING=" + pin + r"$", cache):
        raise ValueError(f"CMake cache differs from canonical pin: {key}")
    dependency = {"AZOOKEY_LLAMA_CPP_GIT_TAG": "llama_cpp", "AZOOKEY_WIL_GIT_TAG": "wil"}[key]
    source = build_dir / "_deps" / (dependency + "-src")
    # Reject local-source overrides: they take precedence over FetchContent.
    for variable in (key.replace("GIT_TAG", "SOURCE_DIR"), "FETCHCONTENT_SOURCE_DIR_" + dependency.upper()):
        value = re.search(r"(?m)^" + variable + r":[^=]+=(.*)$", cache)
        if value and value[1] and (Path(value[1]) / "CMakeLists.txt").exists():
            if Path(value[1]).resolve() != source.resolve():
                raise ValueError(f"Unsupported local source override: {variable}")
    # Verify the fetched checkout too: a cache variable alone is not build evidence.
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"], text=True)
    if actual != pin or dirty:
        raise ValueError(f"Fetched source differs from pin: {dependency}")
    return pin


def complete(document, root, build_dir, runtime_info, runtime_dir, msi):
    if document.get("spdxVersion") != "SPDX-2.3":
        raise ValueError("Expected a Syft SPDX-2.3 document")
    result = copy.deepcopy(document)
    result["creationInfo"]["creators"].append("Tool: azookey-complete-release-sbom")
    packages = result.setdefault("packages", [])
    relationships = result.setdefault("relationships", [])
    licenses = result.setdefault("hasExtractedLicensingInfos", [])
    ids = {document["SPDXID"]} | {x["SPDXID"] for field in ("packages", "files") for x in result.get(field, [])}

    def add(name, version, license_id, comment, download="NOASSERTION", checksums=None):
        identifier = "SPDXRef-azookey-" + hashlib.sha256(name.encode()).hexdigest()[:20]
        if identifier in ids:
            raise ValueError(f"Duplicate SBOM package: {name}")
        ids.add(identifier)
        package = {"SPDXID": identifier, "name": name, "versionInfo": version,
                   "downloadLocation": download, "filesAnalyzed": False,
                   "licenseConcluded": "NOASSERTION", "licenseDeclared": license_id,
                   "copyrightText": "NOASSERTION", "comment": comment}
        if checksums:
            package["checksums"] = checksums
        packages.append(package)
        return identifier

    msi_digest = sha256(msi)
    root_id = add(msi.name, msi_digest, "NOASSERTION", "Unsigned MSI; versionInfo is its SHA256.",
                  checksums=[{"algorithm": "SHA256", "checksumValue": msi_digest}])
    relationships.append({"spdxElementId": result["SPDXID"], "relationshipType": "DESCRIBES", "relatedSpdxElement": root_id})
    locked = nuget_packages(root)
    covered = set()
    runtime = read_json(runtime_info)
    if len(runtime) != len(RUNTIME_FILES) or {x["name"] for x in runtime} != RUNTIME_FILES:
        raise ValueError("Expected exactly the three app-local CRT DLLs")
    for item in inventory(root):
        source, _, selector = item["source"].partition(":")
        license_id = item["license"]
        if license_id.startswith("LicenseRef-"):
            if any(x["licenseId"] == license_id for x in licenses):
                raise ValueError(f"Duplicate extracted license: {license_id}")
            licenses.append({"licenseId": license_id, "name": item["name"], "extractedText": item["attribution"]})
        entries = []
        if source == "cmake":
            version = cmake_pin(root, build_dir, selector)
            url = re.search(r"https://github.com/[^`\s）]+\.git", item["attribution"])
            if not url:
                raise ValueError("CMake attribution needs its upstream Git URL")
            entries.append((item["name"], version, "git+" + url[0] + "@" + version, None))
        elif source == "nuget":
            for name, (version, content_hash) in sorted(locked.items()):
                if name == selector or (selector.endswith(".") and name.startswith(selector)):
                    if name in covered:
                        raise ValueError(f"Duplicate NuGet attribution: {name}")
                    covered.add(name)
                    digest = base64.b64decode(content_hash, validate=True)
                    if len(digest) != 64:
                        raise ValueError("Invalid NuGet SHA512")
                    entries.append((name, version, f"https://www.nuget.org/api/v2/package/{name}/{version}",
                                    [{"algorithm": "SHA512", "checksumValue": digest.hex()}]))
        elif source == "runtime":
            for entry in runtime:
                if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", entry["version"]) or entry["version"] == "0.0.0.0":
                    raise ValueError("Missing numeric CRT file version")
                digest = sha256(runtime_dir / entry["name"])
                if digest != entry["sha256"]:
                    raise ValueError("CRT changed after version capture")
                entries.append((entry["name"], entry["version"], "NOASSERTION",
                                [{"algorithm": "SHA256", "checksumValue": digest}]))
        else:
            raise ValueError(f"Unknown SBOM source: {source}")
        if not entries:
            raise ValueError(f"Attribution selector matches no dependency: {selector}")
        for name, version, download, checksums in entries:
            identifier = add(name, version, license_id, item["attribution"], download, checksums)
            relationships.append({"spdxElementId": root_id, "relationshipType": "DEPENDS_ON", "relatedSpdxElement": identifier})
    if set(locked) - covered != BUILD_ONLY:
        raise ValueError("NuGet lock and THIRD_PARTY_LICENSES coverage differ")
    result.setdefault("annotations", []).append({
        "annotationDate": result["creationInfo"]["created"], "annotationType": "OTHER",
        "annotator": "Tool: azookey-complete-release-sbom",
        "comment": "Declared canonical release build inputs, not an exhaustive binary scan. "
                   "GoogleTest and Windows SDK BuildTools are test/build-only and not shipped. "
                   "GGUF models and CUDA runtime are not in the base MSI. "
                   "LicenseRef text is attribution from THIRD_PARTY_LICENSES, not a license grant."})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("input", "output", "build-dir", "runtime-info", "runtime-dir", "msi"):
        parser.add_argument("--" + flag, required=True, type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    result = complete(read_json(args.input), args.root, args.build_dir, args.runtime_info, args.runtime_dir, args.msi)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Completed release SBOM: {len(result['packages'])} packages")


if __name__ == "__main__":
    main()
