// The supported game versions, by the main program's build ID (exefs/main, 0x40..0x53). Each one has its own
// address tables: game_addresses/patches_<key>.h and runtime_<key>.h, with the same names. An unknown build ID is
// refused: there is no fallback table. tools/game_addresses.py reads this same file.
// An X-macro list (no include guard): GA_VERSION(key, label, main build ID) and
// GA_RELEASE(key, label, SHA-256 of exefs/main.npdm).
//
// 1.0.1 and 1.1.0 share one main program (and the same romfs), so they share one table and one build of the game
// code. Only their main.npdm differs: GA_RELEASE tells them apart for the start screen and the log.

GA_VERSION(v1_0_0, "1.0.0",         "AE16F71E002AF8CB059A9A74C4D90F34BA984892")
GA_VERSION(v1_0_1, "1.0.1 / 1.1.0", "909E904AF78AC1B8DEEFE97AB2CCDB51968F0EC7")
// Kirby Star Allies 1.0.0 and 4.0.0a: no Zelda patches or runtime addresses (empty tables, GA() reads 0; game_is_kirby())
GA_VERSION(kirby_1_0_0, "Kirby 1.0.0",  "059DD87BEDE7C09CCF9BCB9FE528FA9000000000")
GA_VERSION(kirby_4_0_0, "Kirby 4.0.0a", "D55608916FA56C184F2A8FE80DA0427C00000000")

GA_RELEASE(v1_0_1, "1.0.1", "f0fbe16569637adc67a35ed3d9d341ef012bca79cd4011e75f47ab6477b28c0a")
GA_RELEASE(v1_0_1, "1.1.0", "97eb5b50aab8a01952c3ac390ab57929138b4b030ce806e62d071e1fa6113257")
