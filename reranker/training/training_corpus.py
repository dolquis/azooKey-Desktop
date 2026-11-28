"""Pinned corpus inputs and Aozora reading extraction."""

from __future__ import annotations

from collections.abc import Callable, Iterator
from dataclasses import dataclass
import hashlib
from http.client import HTTPMessage
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile
from typing import IO, BinaryIO, Protocol, cast
import urllib.error
import urllib.parse
import urllib.request
import zipfile


class TrainingError(ValueError):
    """Invalid training input or failed verification."""


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, allow_nan=False) + "\n").encode()


@dataclass(frozen=True)
class Asset:
    id: str
    url: str
    revision: str
    sha256: str | None
    license: str
    notice: str
    member: str

    def cache_path(self, cache: Path) -> Path:
        return cache / f"asset-{self.id}.zip"


def load_manifest(path: Path) -> tuple[list[Asset], Asset]:
    manifest = json.loads(path.read_bytes())
    if not isinstance(manifest, dict) or manifest.get("schema") != 1:
        raise TrainingError("invalid source manifest schema")
    corpora = manifest.get("corpora")
    if not isinstance(corpora, list):
        raise TrainingError("source manifest requires a corpora list")

    def parse(row: object, licenses: set[str], *, pending_hash: bool = False) -> Asset:
        required = {"id", "url", "revision", "sha256", "license", "notice", "member"}
        if not isinstance(row, dict) or set(row) != required:
            raise TrainingError("invalid source manifest asset fields")
        for key in required - {"sha256"}:
            if not isinstance(row[key], str) or not row[key].strip():
                raise TrainingError(f"invalid source manifest field: {key}")
        digest = row["sha256"]
        if not (digest is None and pending_hash) and (
            not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)
        ):
            raise TrainingError("source asset requires a lowercase SHA256")
        url = urllib.parse.urlsplit(row["url"])
        if not url.hostname or url.scheme != "https" or url.username or url.password or url.fragment:
            raise TrainingError("source asset requires a plain HTTPS URL")
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,79}", row["id"]):
            raise TrainingError("invalid source asset id")
        member = PurePosixPath(row["member"])
        if member.is_absolute() or ".." in member.parts or "\\" in row["member"]:
            raise TrainingError("invalid source archive member")
        if row["license"] not in licenses:
            raise TrainingError("source asset license is outside the allowlist")
        return Asset(**row)

    works = [parse(row, {"PD"}) for row in corpora]
    dictionary = parse(manifest.get("reading_dictionary"), {"Apache-2.0"}, pending_hash=True)
    if len({asset.id for asset in [*works, dictionary]}) != len(works) + 1:
        raise TrainingError("duplicate source asset id")
    return works, dictionary


class _HttpsOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(
        self, req: urllib.request.Request, fp: IO[bytes], code: int, msg: str,
        headers: HTTPMessage, newurl: str,
    ) -> urllib.request.Request | None:
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise TrainingError("source download redirected outside HTTPS")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _open_url(url: str) -> BinaryIO:
    return cast(BinaryIO, urllib.request.build_opener(_HttpsOnly()).open(url, timeout=60))


def verify_cached(asset: Asset, cache: Path) -> Path:
    path = asset.cache_path(cache)
    if asset.sha256 is None:
        raise TrainingError(f"source hash awaits approval: {asset.id}")
    if not path.is_file() or sha256_file(path) != asset.sha256:
        raise TrainingError(f"missing cache or SHA256 mismatch: {asset.id}")
    return path


def fetch_asset(
    asset: Asset, cache: Path, *, offline: bool, opener: Callable[[str], BinaryIO] = _open_url,
) -> Path:
    path = asset.cache_path(cache)
    if asset.sha256 is None or offline or path.exists():
        return verify_cached(asset, cache)
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cache) as scratch:
        staged = Path(scratch) / "source.zip"
        with opener(asset.url) as stream, staged.open("wb") as output:
            shutil.copyfileobj(stream, output)
        if sha256_file(staged) != asset.sha256:
            raise TrainingError(f"source SHA256 mismatch: {asset.id}")
        staged.replace(path)
    return path


@dataclass(frozen=True)
class ReadingUnit:
    surface: str
    reading: str


class _Morpheme(Protocol):
    def surface(self) -> str: ...
    def reading_form(self) -> str: ...


def sudachi_reader(dictionary: Asset, cache: Path) -> Callable[[str], list[ReadingUnit]]:
    from sudachipy import dictionary as sudachi_dictionary  # type: ignore[import-untyped]

    archive_path = verify_cached(dictionary, cache)
    with zipfile.ZipFile(archive_path) as archive:
        with archive.open(dictionary.member) as source:
            hasher = hashlib.sha256()
            for chunk in iter(lambda: source.read(1 << 20), b""):
                hasher.update(chunk)
            digest = hasher.hexdigest()
        dic_path = cache / f"sudachi-{digest}.dic"
        if not dic_path.exists():
            with tempfile.TemporaryDirectory(dir=cache) as scratch:
                staged = Path(scratch) / dic_path.name
                with archive.open(dictionary.member) as source, staged.open("wb") as target:
                    shutil.copyfileobj(source, target)
                if sha256_file(staged) != digest:
                    raise TrainingError("Sudachi dictionary SHA256 mismatch during extraction")
                try:
                    staged.rename(dic_path)
                except FileExistsError:
                    # A concurrent prepare already published the dictionary.
                    pass
    if sha256_file(dic_path) != digest:
        raise TrainingError("cached Sudachi dictionary SHA256 mismatch")
    tokenizer = sudachi_dictionary.Dictionary(dict=str(dic_path.resolve())).create()

    def read(text: str) -> list[ReadingUnit]:
        tokens: list[_Morpheme] = tokenizer.tokenize(text)
        return [ReadingUnit(token.surface(), token.reading_form()) for token in tokens]

    return read


_ANNOTATION = re.compile(r"［＃[^］]*］")
_RUBY = re.compile(r"｜([^｜《》\n]+)《([^《》\n]+)》|([一-龯㐀-䶿々〆〇ヶ仝]+)《([^《》\n]+)》")


def to_hiragana(text: str) -> str:
    return "".join(chr(ord(char) - 0x60) if "ァ" <= char <= "ヶ" else char for char in text)


def aozora_units(line: str, read: Callable[[str], list[ReadingUnit]]) -> Iterator[ReadingUnit]:
    if "※［＃" in line:
        return
    line = _ANNOTATION.sub("", line)
    matches = list(_RUBY.finditer(line))
    if any(marker in _RUBY.sub("", line) for marker in "｜《》"):
        return
    offset = 0
    for match in matches:
        if match.start() > offset:
            yield from read(line[offset:match.start()])
        yield ReadingUnit(match[1] or match[3], match[2] or match[4])
        offset = match.end()
    if offset < len(line):
        yield from read(line[offset:])


def _body(text: str) -> list[str]:
    lines = text.partition("\n底本：")[0].splitlines()
    separators = [i for i, line in enumerate(lines[:40]) if re.fullmatch(r"-{10,}", line)]
    if len(separators) >= 2:
        return lines[separators[1] + 1:]
    for i, line in enumerate(lines):
        if not line.strip():
            return lines[i + 1:]
    raise TrainingError("Aozora text does not have a header boundary")


def read_work(
    asset: Asset, cache: Path, read: Callable[[str], list[ReadingUnit]],
) -> Iterator[dict[str, str]]:
    with zipfile.ZipFile(verify_cached(asset, cache)) as archive:
        text = archive.read(asset.member).decode("cp932").replace("\r\n", "\n")
    context = ""
    for line_no, line in enumerate(_body(text), 1):
        if not line.strip():
            context = ""
            continue
        units = list(aozora_units(line, read))
        if not units:
            context = ""
            continue
        for index, unit in enumerate(units, 1):
            reading = to_hiragana(unit.reading)
            if re.fullmatch(r"[ぁ-ゖー]+", reading) and unit.surface.strip():
                yield {
                    "id": f"{asset.id}:{line_no}:{index}", "work": asset.id,
                    "left_context": context[-200:], "input": reading, "chosen": unit.surface,
                }
            context = (context + unit.surface)[-200:]
        context = (context + "\n")[-200:]
