# Upstream sources

Pristine copies. Never edit files here; the port lives in `core/`.

| Path | Origin | Ref |
|---|---|---|
| `linux-v7.1/fs/ntfs/` | https://github.com/torvalds/linux `fs/ntfs` (the "ntfsplus" driver by Namjae Jeon, merged for 7.1) | tag `v7.1`, commit `8cd9520d35a6c38db6567e97dd93b1f11f185dc6` |

Refresh:

    git clone --depth=1 --branch <tag> --filter=blob:none --sparse https://github.com/torvalds/linux.git
    cd linux && git sparse-checkout set fs/ntfs
    rm -rf ../upstream/linux-vX.Y/fs/ntfs && cp -R fs/ntfs ../upstream/linux-vX.Y/fs/ntfs

Diff `core/` against this directory to see exactly what the port changed.
