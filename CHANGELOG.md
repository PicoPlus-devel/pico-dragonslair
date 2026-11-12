# pico-dragonslair

An emulator of the laserdisc arcade game Dragon's Lair (Cinematronics, 1983) for RP2350 boards with DVI/HDMI output.

# General Info

[Binaries for each configuration and PCB design are at the end of this page](#downloads___).

[See the readme for the supported boards, the game data and the controls](https://github.com/PicoPlus-devel/pico-dragonslair#readme)

> [!IMPORTANT]
> An **RP2350** board is required. The original RP2040 (Pico 1) is not supported.

> [!IMPORTANT]
> The game data is not included. Besides MAME's `dlair.zip`, the laserdisc is needed: it is converted once on a computer from MAME's `dlair.chd` into the file `lair.pld`, with [ld2pico.exe](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/ld2pico.exe). See the readme.

# Changelog

## v0.2

- The game now runs on the [Pico Computer 3](https://github.com/UKTailwind/Pico-Computer-3): HDMI, sound through HDMI or the audio jack, USB controllers, and NES or SNES controllers and a Wii Classic controller on the I/O header. The pin assignment may still change. See [Pico Computer 3](https://github.com/PicoPlus-devel/pico-infonesPlus#pico-computer-3) in the pico-infonesPlus readme. Thanks to @javavi.
- The game now runs on a PCB for the Waveshare RP2350-PiZero, designed by @szuping: HDMI, sound through HDMI or the audio jack, a USB controller on the PIO-USB port, NES and SNES controller ports, and a socket for an ESP32 module that connects Bluetooth controllers through [BlueRetro](https://github.com/darthcloud/blueretro). The PCB design is not yet available. See [PCB with Waveshare RP2350-PiZero](https://github.com/PicoPlus-devel/pico-infonesPlus#pcb-with-waveshare-rp2350-pizero) in the pico-infonesPlus readme.
- NES and SNES controllers on the controller ports are now read with the same timing as a real console, on all boards. Thanks to @javavi.
- Under pico-bootLoader, the settings menu entry that goes back to the bootloader is now called **Return to boot loader**.
- Fixed: on boards with HSTX video, with *Display Mode* set to DVI, the picture could randomly drop out, and now and then a single line was shown shifted to the right for one frame.

## v0.1.2

- With a USB keyboard, Enter selects and Esc goes back in the menus.
- Fixed: on the Adafruit Fruit Jam and other boards with PIO-USB, USB controllers were often not found at start-up.

## v0.1.1

- The release now includes updateAll.exe, which installs the ROM set `dlair.zip` on the SD card from a Windows computer. The readme explains how to use it. The laserdisc is still converted with ld2pico.exe.

## v0.1

Initial release.

- Dragon's Lair (US revision F2) with the laserdisc pictures and sound, on every RP2350 board of this family. PSRAM is not needed.
- The laserdisc is read from the SD card while the game runs. It is converted once from MAME's `dlair.chd` into `lair.pld`, about 260 MB, with **ld2pico.exe**, a small Windows program released with this version (on Linux, build the same converter from the source).
- The score, lives and credits of the cabinet's LED scoreboard are shown along the top of the picture. SELECT + B switches them off and on.
- Can be started from pico-bootLoader, where it appears in the Arcade category.

<a name="downloads___"></a>
## Downloads by configuration

The laserdisc converter for Windows: [ld2pico.exe](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/ld2pico.exe)

The ROM set installer for Windows: [updateAll.exe](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/updateAll.exe)

Binaries for each configuration are listed below. Only RP2350-based boards are supported; no RP2040 / Pico 1 binaries are provided.

Wiring and assembly are the same as for the NES emulator; see the [pico-infonesPlus documentation](https://github.com/PicoPlus-devel/pico-infonesPlus#setup) for the RP2350 boards listed below.

### Standalone boards

| Board | Binary |
|:--|:--|
| Adafruit Metro RP2350 | [picoDragonsLair_AdafruitMetroRP2350_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_AdafruitMetroRP2350_arm.uf2) |
| Adafruit Fruit Jam | [picoDragonsLair_AdafruitFruitJam_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_AdafruitFruitJam_arm_piousb.uf2) |
| Adafruit Feather RP2350 with TLV320DAC3100 | [picoDragonsLair_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2) |
| Waveshare RP2350-PiZero | [picoDragonsLair_WaveShareRP2350PiZero_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_WaveShareRP2350PiZero_arm_piousb.uf2) |

### Breadboard

| Board | Binary |
|:--|:--|
| Pico 2 | [picoDragonsLair_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_AdafruitDVISD_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [picoDragonsLair_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_AdafruitDVISD_pico2_arm.uf2) |

### PicoNES PCB (PCB required)

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picoDragonsLair_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_AdafruitDVISD_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [picoDragonsLair_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_AdafruitDVISD_pico2_arm.uf2) |

PCB: [pico_nesPCB_v2.6.zip](https://github.com/PicoPlus-devel/pico-infonesPlus/releases/latest/download/pico_nesPCB_v2.6.zip). [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2)

3D-printed case: [thingiverse.com/thing:6689537](https://www.thingiverse.com/thing:6689537). When the board is fitted on male headers, use the latest top cover; the older covers assume a Pico soldered flat and leave no room for the USB cable.

### PicoNES Mini PCB, Waveshare RP2350-Zero (PCB required)

| Board | Binary |
|:--|:--|
| Waveshare RP2350-Zero | [picoDragonsLair_WaveShareRP2350ZeroWithPCB_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_WaveShareRP2350ZeroWithPCB_arm.uf2) |

PCB: [Gerber_PicoNES_Mini_PCB_v2.0.zip](https://github.com/PicoPlus-devel/pico-infonesPlus/releases/latest/download/Gerber_PicoNES_Mini_PCB_v2.0.zip)

3D-printed case: [thingiverse.com/thing:7041536](https://www.thingiverse.com/thing:7041536)

### PicoNES Micro PCB, Waveshare RP2350-USBA (PCB required)

[Binary](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_WaveShare2350USBA_arm_piousb.uf2)

PCB: [Gerber_PicoNES_Micro_v1.2.zip](https://github.com/PicoPlus-devel/pico-infonesPlus/releases/latest/download/Gerber_PicoNES_Micro_v1.2.zip)

[Build guide](https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/)

### Pimoroni Pico DV

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picoDragonsLair_PimoroniDVI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_PimoroniDVI_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [picoDragonsLair_PimoroniDVI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_PimoroniDVI_pico2_arm.uf2) |

### SpotPear HDMI

For more info about the SpotPear HDMI see https://spotpear.com/index/product/detail/id/1207.html and https://spotpear.com/index/study/detail/id/971.html.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picoDragonsLair_SpotpearHDMI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_SpotpearHDMI_pico2_arm.uf2) |

### Murmulator M1

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [picoDragonsLair_MurmulatorM1_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_MurmulatorM1_pico2_arm.uf2) |

### Murmulator M2

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Murmulator M2 | [picoDragonsLair_MurmulatorM2_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_MurmulatorM2_arm.uf2) |

### Olimex RP2040-PICO-PC

| Board | Binary |
|:--|:--|
| Olimex RP2040-PICO-PC with a Pico 2 | [picoDragonsLair_OlimexPicoPC_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_OlimexPicoPC_arm.uf2) |

### Pico Computer 3

For more info about the Pico Computer 3 see https://github.com/UKTailwind/Pico-Computer-3

| Board | Binary |
|:--|:--|
| Pico Computer 3 (PC3) | [picoDragonsLair_PicoComputer3_arm.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_PicoComputer3_arm.uf2) |

### PCB Waveshare RP2350-PiZero

For more info about this PCB, designed by Liu Szuping, see [PCB with Waveshare RP2350-PiZero](https://github.com/PicoPlus-devel/pico-infonesPlus#pcb-with-waveshare-rp2350-pizero). The PCB design is not yet available.

| Board | Binary |
|:--|:--|
| Waveshare RP2350-PiZero (*) | [picoDragonsLair_WaveShareRP2350PiZeroLiuPCB_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-dragonslair/releases/latest/download/picoDragonsLair_WaveShareRP2350PiZeroLiuPCB_arm_piousb.uf2) |

This binary is only for the RP2350-PiZero on this PCB.

(*) If you fitted this board with PSRAM and it has a non-Winbond flash chip, apply the [flash_config fix](https://github.com/PicoPlus-devel/pico-infonesPlus#psram-with-a-non-winbond-flash-chip) before flashing the emulator.
