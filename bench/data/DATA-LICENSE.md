# Evaluation test data license

Except for the NLL dictbuild fixture described below, the authored evaluation cases in this directory are dedicated to the public domain under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/).

They were written specifically for azooKey Desktop and do not reproduce or derive from AJIMEE,
Sudachi, NEologd, UniDic, JMdict, or any other third-party corpus or dictionary.

`mixed_english_eval.jsonl` names common English words (product names, technical terms)
inside authored sentences. Its `expected_english` surfaces follow the casing rule of the
mixed-input specification; the bundled English dictionary (`data/english-words.tsv`) is
read by path at evaluation time and no part of it is copied into this directory.

`nll_fixture.lex.tsv` and `nll_fixture.metadata.json` are synthetic, repository-authored
dictbuild fixtures licensed under the repository's [MIT license](../../LICENSE).
They use MIT to exercise dictbuild's license-notice catalog with the repository license.
The `THIRD_PARTY_LICENSES` header marker is required by dictbuild's input contract;
it does not indicate third-party material in this fixture.
