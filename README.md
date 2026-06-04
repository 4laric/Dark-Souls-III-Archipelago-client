# ER Archipelago runtime-client — additions bundle

Merge these into the spec-3 (Elden Ring runtime-client) repo, which mirrors the DS3 Archipelago
client layout (MSVC `.sln`/`.vcxproj`, all source under `archipelago-client/`).

```
archipelago-client/   Client source — the decode/version/singleton/hooks headers and the Windows
                      gamehook implementation. ADD er_gamehook_win.cpp to the .vcxproj and
                      .vcxproj.filters or it won't compile. These supersede the DS3-specific pieces:
                        er_gamehook.* + er_hooks.h   ->  replace GameHook.{h,cpp}
                        er_goods_row.h + er_item_decode.h  ->  replace the goods struct/decode in Params.h
                        er_singletons.h              ->  replace the fd4_singleton subproject
                      ItemRandomiser / Core / ArchipelagoInterface keep their structure; rewire their
                      item-handling to call er_ap::game:: and add the three touchpoints (Init(),
                      Archipelago_SendLocationCheck / Er_GrantReceivedGoods, versionSatisfies()).

tests/                Host (g++) unit tests + Makefile. NOT part of the .sln. Run anywhere:
                        cd tests && make test
                      Validates the pure logic (decode, row offsets, param walk, version gate) and
                      reconciles the recombine against spec-2's golden vectors. Good CI (Linux) check.

tools/                NOTES.md  — provenance & reference: build target/hash, resolved address table,
                                  the param-walk chain, the decode contract, cross-validation, and the
                                  two first-run verification flags. READ THIS FIRST.
                      fetch_def.py / compute_offsets.py — re-derive and re-validate the EquipParamGoods
                                  offsets from Paramdex (compute_offsets.py asserts the 176-byte stride).
                      EquipParamGoods.xml — the Paramdex def the offsets were computed from (vendored).
```

Everything that can be validated without the game already is. The only remaining steps are your
MSVC build and the two runtime confirmations described in `tools/NOTES.md`.
