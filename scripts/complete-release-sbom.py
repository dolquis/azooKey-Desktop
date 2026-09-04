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
    declarations = re.findall(r"FetchContent_Declare\(\s*([\w-]+)\s+([^)]*)\)", cmake, re.I)
    dependencies = {name.lower() for name, body in declarations
                    if re.search(r"GIT_TAG\s+\$\{" + re.escape(key) + r"\}", body)}
    if len(dependencies) != 1:
        raise ValueError(f"Expected one FetchContent declaration using {key}")
    dependency = dependencies.pop()
    source = build_dir / "_deps" / (dependency + "-src")
    # Reject local-source overrides: they take precedence over FetchContent.
    for variable in (key.replace("GIT_TAG", "SOURCE_DIR"), "FETCHCONTENT_SOURCE_DIR_" + dependency.upper()):
        value = re.search(r"(?m)^" + variable + r":[^=]+=(.*)$", cache)
        if value and value[1] and (Path(value[1]) / "CMakeLists.txt").exists():
            if Path(value[1]).resolve() != source.resolve():
                raise ValueError(f"Unsupported local source override: {variable}")
    # Verify the fetched checkout too: a cache variable alone is not build evidence.
    if not (source / ".git").exists():
        raise ValueError(f"Missing FetchContent checkout: {source}; canonical release build required")
    try:
        actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True, stderr=subprocess.PIPE).strip()
        # Use the checkout's Git attributes / CRLF normalization, and compare both
        # staged and unstaged tracked changes. Do not override clone-time settings.
        dirty = subprocess.check_output(["git", "-C", str(source), "diff", "--name-only", "HEAD", "--"], text=True, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as error:
        raise ValueError(f"Cannot inspect FetchContent checkout: {source}") from error
    if actual != pin or dirty:
        raise ValueError(f"Fetched source differs from pin: {dependency}")
    return pin


def msi_root(document, msi, version):
    """Reuse the single MSI package described by Syft; reject ambiguous input."""
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", version):
        raise ValueError("Expected release version MAJOR.MINOR.PATCH")
    if any(int(part) > limit for part, limit in zip(version.split("."), (255, 255, 65535))):
        raise ValueError("Release version exceeds MSI limits")
    described = set(document.get("documentDescribes", []))
    described.update(r["relatedSpdxElement"] for r in document.get("relationships", [])
                     if r["spdxElementId"] == document["SPDXID"] and r["relationshipType"] == "DESCRIBES")
    roots = [p for p in document.get("packages", []) if p["SPDXID"] in described]
    if len(described) != 1 or len(roots) != 1:
        raise ValueError("Expected one described MSI package in Syft output")
    package = roots[0]
    # Accept portable filenames and Windows/POSIX source paths from Syft.
    if package["name"].replace("\\", "/").rsplit("/", 1)[-1] != msi.name:
        raise ValueError("Syft root does not describe the supplied MSI")
    digest = sha256(msi)
    checksums = package.setdefault("checksums", [])
    for checksum in checksums:
        if checksum["algorithm"] == "SHA256" and checksum["checksumValue"].lower() != digest:
            raise ValueError("Syft MSI checksum differs from supplied artifact")
    if not any(c["algorithm"] == "SHA256" for c in checksums):
        checksums.append({"algorithm": "SHA256", "checksumValue": digest})
    package["versionInfo"] = version
    return package["SPDXID"]


def complete(document, root, build_dir, runtime_info, runtime_dir, msi, version):
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

    root_id = msi_root(result, msi, version)
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
            raise ValueError(f"Attribution selector matches no dependency: {item['source']}")
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
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        result = complete(read_json(args.input), args.root, args.build_dir, args.runtime_info, args.runtime_dir, args.msi, args.version)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Release SBOM error: {error}\n")
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Completed release SBOM: {len(result['packages'])} packages")


if __name__ == "__main__":
    main()
