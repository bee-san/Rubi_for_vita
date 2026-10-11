# Settings PNG runtime compatibility

The installed v0.7.7 Settings app crashed during icon loading. Its completed
native dump points to `_free_r` through `png_free_jmpbuf` and
`_vita2d_load_PNG_generic`, not to PVF text measurement. The pinned SDK libpng
and libvita2d packages were compiled with a 92-byte jump buffer, while the
app's linked ARM `setjmp`/`longjmp` transfer 104 bytes. The save overwrites
libpng's adjacent jump metadata, explaining the invalid free in the dump.

The fix builds unmodified libpng 1.6.58 from a SHA-256-pinned primary-source
archive with the app's current VitaSDK. Its local jump buffer is 160 bytes;
its constructor and destructor stack buffers use the same headers. The
Settings target links this project-private static library explicitly instead
of the SDK libpng archive. Global SDK files, icon loading, the readable layout,
settings storage and OCR plugins are unchanged.

The existing libvita2d loader requests 92 bytes once per new PNG reader.
Libpng's `png_set_longjmp_fn` accepts a first request no larger than its local
buffer and returns that full local storage, so the actual 104-byte save fits
the new 160-byte buffer. The constructor clears its temporary jump pointer
before returning the reader. Repeating the API call with a different size is
not supported; the existing loader does not do so. These contracts are in
[libpng 1.6.58's error implementation](https://github.com/pnggroup/libpng/blob/v1.6.58/pngerror.c)
and [constructor](https://github.com/pnggroup/libpng/blob/v1.6.58/png.c).

Every native Settings link runs `tools/check_settings_png_abi.py`. It reads
the actual linked ARM save/restore instructions and compares their byte spans
with libpng's compiled private-structure geometry and linked buffer limit.
The geometry object is build-only and is not included in the app. Unknown
instruction layouts and a different linked PNG buffer limit fail the build.
This gate establishes native binary compatibility, not icon rendering or
physical readability; those still require a launch on the Vita.
