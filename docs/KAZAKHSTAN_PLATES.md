# Kazakhstan Plate Formats

## Sources Checked

- Official legal text: Adilet, Ministry of Internal Affairs Order of 19 December 2015 No. 1040, "On approval of forms and samples of state registration number plates": https://adilet.zan.kz/rus/docs/V1500012892
- Official 2026 amendment: Adilet, Acting Minister of Internal Affairs Order of 9 July 2026 No. 484, registered 10 July 2026 No. 39273: https://adilet.zan.kz/rus/docs/V2600039273
- English mirror of the digital codes/forms page used for cross-checking labels: https://24pdd.kz/numeric-codes/
- Public reporting on the 2018 ST RK 986-2012 alphabet expansion adding G, I, J, Q: https://otyrar.kz/2018/02/novye-bukvy-poyavilis-na-gosnomernyx-znakax/

## Prioritized Ordinary Formats

Layouts are configuration, not code. `validation.formats` in `config/jetson-nano.yaml` and
`config/default.yaml` lists them as slot patterns:

```text
D  digit
L  letter drawn from validation.letters
R  region-code digit, checked against validation.regions
```

Both shipped profiles accept the two current ordinary layouts a parking entrance sees:

| Name | Profile | Pattern | Normalized form |
| --- | --- | --- | --- |
| `current_individual` | Individual / physical person, Type 1A and 2A | `DDDLLLRR` | `123ABC02` |
| `current_legal_entity` | Legal entity, Type 1 and 2 | `DDDLLRR` | `123AB02` |

The built-in rules, used when a config lists no formats, also accept the 1993 layout `LDDDLLL`
(`A123BCD`, name `legacy_1993`, weight 0.9). The shipped profiles leave it out because no 1993
plate has been checked against the OCR; `config/default.yaml` shows the entry to uncomment.

Adding a layout is a config edit and needs no rebuild. When several layouts match a reading, the
one needing the fewest character repairs wins; ties break on the layout's configured `weight`.

The Adilet order describes Type 1A as ordinary plates for passenger cars of individuals and Type 1 as ordinary plates for passenger cars of legal entities. It also describes the ordinary Type 1/1A and 2/2A backgrounds as white, with black symbols, flag, and `KZ`.

## Region Codes

The 2026 Adilet amendment republishes the region-code appendix. The runtime accepts:

| Code | Region |
| --- | --- |
| 01 | Astana |
| 02 | Almaty |
| 03 | Akmola region |
| 04 | Aktobe region |
| 05 | Almaty region |
| 06 | Atyrau region |
| 07 | West Kazakhstan region |
| 08 | Zhambyl region |
| 09 | Karaganda region |
| 10 | Kostanay region |
| 11 | Kyzylorda region |
| 12 | Mangystau region |
| 13 | Turkistan region |
| 14 | Pavlodar region |
| 15 | North Kazakhstan region |
| 16 | East Kazakhstan region |
| 17 | Shymkent |
| 18 | Abay region |
| 19 | Zhetysu region |
| 20 | Ulytau region |

## Alphabet Policy

The parser treats current ordinary letters as Latin `A-Z`. This follows the 2018 expansion reports that G, I, J, and Q were added to the ST RK 986-2012 letter set, removing the older exclusion commonly cited before 2018.

Obscene, official, or otherwise reserved letter combinations should be handled as an issuance-policy layer only if the deployment owner supplies an authoritative current list. The ANPR parser should not reject a visually valid plate solely because a string looks unusual.

## Ambiguity Policy

Corrections are position-aware and come from `validation.digit_confusions` and
`validation.letter_confusions`. A digit slot only accepts a digit-shaped substitution, a letter
slot only a letter-shaped one, and a character with no configured partner is never rewritten. The
validator therefore cannot invent a plausible-looking plate out of an unreadable one.

Shipped defaults:

- digit slots: `O/Q/D -> 0`, `I/L -> 1`, `Z -> 2`, `S -> 5`, `G -> 6`, `B -> 8`;
- letter slots: `0 -> O`, `1 -> I`, `2 -> Z`, `5 -> S`, `6 -> G`, `8 -> B`.

`1 -> I` in letter slots is a deliberate change from the earlier policy, which excluded it on the
grounds that `1` could plausibly mean more than one Latin letter. Two things argued for adding
it: it is the single most common confusion in practice, and on a development clip the OCR
alternated between `I` and `1` in the same slot across consecutive frames, with `I` being
correct. The risk is real but narrow, and it is bounded by two other mechanisms: a repaired
reading carries a confidence penalty, and multi-frame consensus has to agree on the repaired
string before anything is accepted.

A deployment that disagrees lists `validation.letter_confusions` in its config without that
entry. Nothing in the code depends on it.

If more than `validation.max_corrections` repairs are needed, the reading is reported `AMBIGUOUS`
and discarded. Rejecting an uncertain result is always preferred to forcing it into a
valid-looking plate.

## Not Yet Production-Supported

The parser does not yet fully validate:

- diplomatic red plates;
- foreign citizen/company yellow plates;
- police/government/service plates;
- motorcycle and trailer layouts;
- DataMatrix/security-marker presence;
- lists of prohibited or reserved combinations.

Those formats are documented as known scope, not silently accepted as ordinary parking
registrations. Each is a `validation.formats` entry away once its layout is confirmed against the
legal text, but none should be added without a labelled sample to test against.

## OCR Model Caveat

The OCR is Nomeroff Net's dedicated `kz` model. A country-specific model is a good basis, but
this repository has no labelled KZ evaluation set. Do not turn the upstream model's dataset
accuracy into a parking-camera accuracy claim; evaluate the actual distance, angle, shutter, glare,
weather and plate mix. See [OCR](OCR.md).
