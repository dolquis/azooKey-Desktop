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
    header = "# SPDX-License-Identifier: Apache-2.0; THIRD_PARTY_LICENSES; synthetic fixture\n"
    with temporary_directory(dir=repository) as temporary:
        work = Path(temporary)
        notices = repository / "dictbuild" / "notices"

        def metadata(layer: str) -> dict:
            return json.loads((repository / "dictbuild" / "sources" /
                               f"{layer}.metadata.json").read_text(encoding="utf-8"))

        def tiny(layer: str, reading: str = "とうきょう", surface: str = "東京",
                 name: str | None = None) -> Path:
            tsv = work / (name or f"{layer}.lex.tsv")
            tsv.write_text(header + "surface\treading\tpos\tsource_id\n"
                           f"{surface}\t{reading}\t名詞\t{metadata(layer)['sources'][0]['source_id']}\n",
                           encoding="utf-8", newline="\n")
            return tsv

        # One word in sudachi_lexicon and one kana word in lattice_core, in one tiny connection space.
        connections = work / "sudachi_lexicon.conn.tsv"
        connections.write_text(
            header + "\t".join(dictbuild.CONNECTION_COLUMNS) + "\n"
            "とうきょう\t東京\t1\t1\t3000\t名詞-普通名詞-一般\t\t東京\t名詞,普通名詞,一般,*,*,*\n"
            "は\tは\t2\t2\t3846\t助詞-係助詞\t\tは\t助詞,係助詞,*,*,*,*\n", encoding="utf-8", newline="\n")
        matrix = work / "matrix.def"
        matrix.write_text("3 3\n" + "".join(f"{r} {l} {r - l}\n" for r in range(3) for l in range(3)),
                          encoding="utf-8", newline="\n")
        sudachi = dictbuild.LAYERS.index("sudachi_lexicon")

        def sudachi_layer(*_):
            return dictbuild.build([tiny("sudachi_lexicon")], metadata("sudachi_lexicon"), sudachi, notices,
                                   connections, dictbuild.connection_space(matrix))

        def lattice_core(*_):
            kana = tiny("sudachi_lexicon", "は", "は", "lattice_core.lex.tsv")
            return dictbuild.build_lattice_core([kana], connections, matrix, metadata("sudachi_lexicon"),
                                                sudachi, notices)

        def technical_terms():
            layer = "technical_terms_lexicon"
            return dictbuild.build([tiny(layer)], metadata(layer), dictbuild.LAYERS.index(layer), notices)

        with mock.patch.object(build_bundled, "build_sudachi", side_effect=sudachi_layer), \
                mock.patch.object(build_bundled, "build_lattice_core", side_effect=lattice_core), \
                mock.patch.object(build_bundled, "build_technical_terms", side_effect=technical_terms), \
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
