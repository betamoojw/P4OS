# Python modules inside python.so

These `.py` files are frozen into the app: `tools/gen_upy.sh` turns them
into `main/upy/frozen_content.c`, and `import` finds them under the
`.frozen` entry of `sys.path`, after `/python/lib` (a newer version
installed on the card wins). Run the script after changing one.

| File | From | License |
|------|------|---------|
| `requests/__init__.py` | micropython-lib `python-ecosys/requests` 1.1.1 | MIT (LICENSE-micropython-lib) |
| `mip/__init__.py` | micropython-lib `micropython/mip` 0.5.0 | MIT (LICENSE-micropython-lib) |
| `tls.py`, `ssl.py` | this app | P4OS's |

micropython-lib (<https://github.com/micropython/micropython-lib>) at commit
`90028be0829c6e988b5e239b28f39fb91b6b20e2`. One change, marked "P4OS:" in the file: `requests`'s
`BodyStream.read()` with no size (what `Response.content` does with a
Content-Length) collects the body in a list, 4 KB at a time; upstream adds
bytes 256 at a time, quadratic, and a megabyte took minutes on the board.
`mip` is unchanged. They run on the app's `socket` (`main/mod_net.c`), the
firmware's connection with its TLS and certificate store.
