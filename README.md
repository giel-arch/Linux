# postmarketOS untuk OPPO A38 (CPH2579)

Port postmarketOS downstream untuk **OPPO A38 (CPH2579)** — MediaTek MT6768
(Helio G85), kernel 4.19.191 downstream OPPO (`oppo/mt6769_u_14.0_a38_a18`).

Repo ini berisi:
- Source kernel downstream OPPO MT6768 (dengan `vendor/` dari
  `android_kernel_modules_oppo_mt6769` yang digabung, symlink direkonstruksi
  agar bisa dibuild berdiri sendiri)
- `pmaports/` — device package (`device-oppo-a38`) + kernel package
  (`linux-oppo-a38`) untuk postmarketOS
- `.github/workflows/build.yml` — build image via pmbootstrap di GitHub Actions
- `images/vbmeta_a.img` — vbmeta stock (untuk flashing dengan verity disabled)

## Format boot image

Device ini memakai **boot image header v2 + DTB** (ekstensi MTK): DTB
(`mediatek/mt6768`) ditempatkan di section dtb boot image dalam format tabel
MediaTek (magic `0xd7b7ab1e`, 32-byte header). `boot-deploy` +
`mkbootimg-osm0sis` mendukung format ini (`deviceinfo_header_version="2"` +
`deviceinfo_dtb`). DTB yang di-build dari source ini **byte-identical** dengan
DTB di boot.img stock.

## Cara build

Push ke branch `main` → GitHub Actions otomatis menjalankan pmbootstrap:
`pmbootstrap install --no-interactive` → artifact `boot.img` + `rootfs.img`.

Build manual lokal (butuh pmbootstrap):

```
# patch APKBUILD kernel dengan sha archive commit kamu, lalu:
pmbootstrap config aports $PWD/pmaports
pmbootstrap config device oppo-a38
pmbootstrap install --no-interactive --password 147147
pmbootstrap export output/
```

## Cara flash (butuh unlock bootloader)

Pastikan slot yang aktif `a` (`fastboot getvar current-slot`). Semua file dari
artifact CI.

```
# 1. boot.img (kernel + initramfs pmOS)
fastboot flash boot boot.img

# 2. matikan verifikasi AVB
fastboot flash vbmeta images/vbmeta_a.img --disable-verity --disable-verification

# 3. rootfs ke userdata (HAPUS data!)
fastboot erase userdata
fastboot flash userdata rootfs.img
```

Password user default: `147147` (ubah di workflow / perintah install).
UI default: `console`. Setelah sistem jalan, ganti UI via
`pmbootstrap config ui phosh` (misal) dan rebuild.

## Status

- [x] Kernel build (vmlinuz + dtb byte-identical dengan stock)
- [x] boot.img v2+dtb sesuai format stock
- [ ] Layar sentuh / display (MTK_FB legacy fbdev, butuh pengujian di device)
- [ ] WiFi/BT (firmware consys belum di-package)
- [ ] Modem, sensor, kamera

Catatan: `joan` pada nama file backup hanyalah watermark, bukan codename.
