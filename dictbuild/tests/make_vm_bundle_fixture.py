"""Make tiny synthetic VM-package test bundles using the fixture checkout's builder."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
from unittest import mock


def generate(repository: Path, output: Path) -> None:
    # The caller copies production modules/inputs into TestDrive. Import those
    # copies so their __file__ and receipt fingerprints describe that checkout.
    sys.path.insert(0, str(repository / "dictbuild"))
    import build_bundled
    import dictbuild

    temporary_directory = tempfile.TemporaryDirectory
    with temporary_directory(dir=repository) as temporary:
        def tiny(layer: str) -> tuple[bytes, str]:
            metadata = json.loads((repository / "dictbuild" / "sources" /
                                   f"{layer}.metadata.json").read_text(encoding="utf-8"))
            tsv = Path(temporary) / f"{layer}.lex.tsv"
            tsv.write_text("# SPDX-License-Identifier: Apache-2.0; THIRD_PARTY_LICENSES; synthetic fixture\n"
                           "surface\treading\tpos\tsource_id\n"
                           f"東京\tとうきょう\t名詞\t{metadata['sources'][0]['source_id']}\n",
                           encoding="utf-8", newline="\n")
            return dictbuild.build([tsv], metadata, dictbuild.LAYERS.index(layer),
                                  repository / "dictbuild" / "notices")

        with mock.patch.object(build_bundled, "build_sudachi", side_effect=lambda *_: tiny("sudachi_lexicon")), \
                mock.patch.object(build_bundled, "build_technical_terms", side_effect=lambda: tiny("technical_terms_lexicon")), \
                mock.patch.object(build_bundled.tempfile, "TemporaryDirectory",
                                  side_effect=lambda: temporary_directory(dir=repository)):
            build_bundled.build_bundle(output, Path(temporary) / "unused-cache", True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.repository.resolve(), args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
