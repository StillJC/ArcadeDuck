# Konami GV Compatibility

This table applies to the ArcadeDuck POC Preview using the default CPU Interpreter.

| MAME set | Game | Status | Controls / hardware | Notes |
|---|---|---|---|---|
| `powyak96` | Jikkyou Powerful Pro Yakyuu '96 | Playable | Standard controls | Boots through attract mode and gameplay |
| `hyperath` | Hyper Athlete | Playable | Standard controls | Boots through attract mode and gameplay |
| `lacrazyc` | Let's Attack Crazy Cross | Playable | Standard controls | Use the default CPU Interpreter |
| `susume` | Susume! Taisen Puzzle-Dama | Playable | Standard controls | Use the default CPU Interpreter |
| `btchamp` | Beat the Champ | Playable | Dual trackballs | Trackball controls and test-menu exit supported |
| `kdeadeye` | Dead Eye | Playable | Lightguns | Gun aiming, trigger, and reload supported |
| `weddingr` | Wedding Rhapsody | Playable | Standard controls | Boots through attract mode and gameplay |
| `tmosh` | Tokimeki Memorial Oshiete Your Heart | Playable | Buttons and sensor inputs | Daughterboard and printer behavior is preliminary |
| `tmoshs` | Tokimeki Memorial Oshiete Your Heart Seal Version | Playable | Buttons and sensor inputs | Daughterboard and printer behavior is preliminary |
| `tmoshsp` | Tokimeki Memorial Oshiete Your Heart Seal Version Plus | Playable | Buttons and sensor inputs | Daughterboard and printer behavior is preliminary |
| `tmoshspa` | Tokimeki Memorial Oshiete Your Heart Seal Version Plus | Playable | Buttons and sensor inputs | Daughterboard and printer behavior is preliminary |
| `nagano98` | Winter Olympics in Nagano '98 | Playable | Standard controls | Boots through attract mode and gameplay |
| `naganoj` | Hyper Olympic in Nagano | Playable | Standard controls | Boots through attract mode and gameplay |
| `simpbowl` | The Simpsons Bowling | Playable | Trackball | Daughterboard flash and trackball supported |

## Test conditions

- CPU mode: Interpreter
- BIOS: `bios/konamigv.zip`
- Expected BIOS member: `999a01.7e`
- ROM layout: non-merged MAME sets
- Disk media: matching CHD where required
- Default display aspect ratio: 4:3
- Optional widescreen stretching remains available

Compatibility describes the POC Preview and does not guarantee compatibility with future migrated ArcadeDuck builds.
