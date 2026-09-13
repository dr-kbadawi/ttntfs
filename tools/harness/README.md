# `iotrace` — what device I/O does the core actually do?

Wraps a file-backed `struct ntfs_bdev` with a counting shim and histograms every
transfer the core makes, so a file-level operation can be traced to its real
device I/O. It calls the core directly (`ntfs_write`), bypassing FSKit and the
kernel, which is exactly what makes it useful: anything the kernel adds shows up
as a difference between this and the extension's own `io window` log lines.

```
cc -O1 -I core/include -I platform/include -o /tmp/iotrace tools/harness/iotrace.c \
    build/libntfscore.a build/libntfsplatform.a
dd if=/dev/zero of=/tmp/t.img bs=1m count=512
tools/.local/sbin/mkntfs -Q -F -L TRACE /tmp/t.img
/tmp/iotrace /tmp/t.img 500
```

It settled the 16 KiB question on 2026-09-14: through the core, a 4 KiB write is
one 4 KiB device write with no reads, and a 16 KiB write is one 16 KiB device
write. The core neither amplifies nor splits. See docs/progress/vfs.md.
