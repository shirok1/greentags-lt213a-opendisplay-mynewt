# Third-party sources

The root LICENSE and NOTICE are retained from the Apache Mynewt project template. This repository is an independent LT213A port, not an Apache or OpenDisplay release. Third-party files retain their own licenses and notices; the root license does not replace them.

| Component | Version / source | Distribution |
|---|---|---|
| Apache Mynewt core | `mynewt_1_15_0_tag` | Downloaded to ignored `repos/`; retain upstream LICENSE/NOTICE when redistributing |
| Apache NimBLE | `nimble_1_10_0_tag` | Downloaded dependency, Apache-2.0 |
| Nordic nrfx | `v3.14.0` | Downloaded dependency, see upstream license and file notices |
| ARM CMSIS 5 | `5.9.0` | Downloaded dependency, see upstream license and file notices |
| Mbed TLS | `v3.6.6` | Downloaded dependency, see upstream licenses |
| OpenDisplay streaming uzlib | `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb` | Vendored subset; [source record](libs/od-uzlib/UPSTREAM.md), [license](libs/od-uzlib/LICENSE) |

The uzlib directory in OpenDisplay Firmware carries its own zlib license; preserve it rather than inferring a license from the parent firmware repository. Its unmodified streaming decoder is compiled with the adapter in `apps/opendisplay/src/inflate.c`.

The GDEW0213T5 panel command sequences and LUT values were derived from Good Display examples `GDEW0213T5_Arduino_20191016` and `GDEW0213T5_Arduino_P20201021`. Those example archives are not redistributed here; their redistribution license was not supplied. This provenance note does not grant rights to the original archives.

OpenDisplay protocol headers, Python client and official firmware were used as interoperability/behavior references; they are not bundled here except for the explicitly listed uzlib subset. Protocol and power references are linked in README.md and docs/.
