

https://github.com/user-attachments/assets/f9485d78-d3a1-40c2-894f-7cd2c7d4c727

You can learn Japanese from your PS Vita :)

In a visual novel, press L + R: Rubi reads the dialogue off the screen, and you pick a word to see what it means and save it as an Anki card with a screenshot. Fully offline, on the Vita itself.

## Installation instructions

You need a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore, VitaShell, and
**Settings > HENkaku Settings > Enable Unsafe Homebrew** turned on.

Download the [Rubi .zip](https://github.com/bee-san/Rubi_for_vita/releases/download/v0.10.0/Rubi-0.10.0.zip)
and extract it.

### 1. Transfer modules

Using VitaShell FTP, transfer the files from `Rubi-0.10.0/ur0/tai/` to `ur0:tai/` on the Vita:

- `VitaJPOverlay_Kernel.skprx`
- `VitaJPOverlay_OCR.suprx`
- `VitaJPOverlay_Shell.suprx`

### 2. Edit the taiHEN config

Back up `ur0:tai/config.txt`, then add `ur0:tai/VitaJPOverlay_Kernel.skprx` to its `*KERNEL`
section and `ur0:tai/VitaJPOverlay_Shell.suprx` to its `*main` section. The two sections can
appear in either order; each plugin must be beneath its matching section. Keep your other entries.

Example (`ur0:tai/config.txt`):

```text
*KERNEL
ur0:tai/VitaJPOverlay_Kernel.skprx

*main
ur0:tai/VitaJPOverlay_Shell.suprx
```

If `ux0:tai/config.txt` exists, taiHEN reads it first: back it up and add the entries there instead.
`VitaJPOverlay_OCR.suprx` needs no config entry; the kernel plugin loads it into enabled games.

### 3. Transfer data files for the plugin

Transfer `Rubi-0.10.0/ur0/data/VitaJPOverlay` to `ur0:data/` on the Vita, so the folder is present
as `ur0:data/VitaJPOverlay`.

### 4. Transfer data files for OCR and the dictionary

**This is a separate folder, not a duplicate of step 3.** Transfer
`Rubi-0.10.0/ux0/data/VitaJPOverlay` to `ux0:data/` on the Vita, so the folder is present
as `ux0:data/VitaJPOverlay`.

If updating an existing installation, back up your `ux0:data/VitaJPOverlay/config.ini` first
and keep your existing settings file when copying the data.

### 5. Transfer and install the VPK / companion app

Close Rubi first if it is already open, including when suspended behind VitaShell: pull the
top-right corner of its LiveArea page. Transfer `Rubi-0.10.0/Rubi.vpk` to the Vita and install
(promote) it with VitaShell. Accept the installation and extended-permissions prompts.

### 6. Reboot the Vita

In VitaShell, press **START** and choose **Reboot**.

Start a game and press **L + R** to open Rubi. Every game is ON by default.

The [guide](docs/guide.md#install) also covers the standalone VPK and computer-side installers.
