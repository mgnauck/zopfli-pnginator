# zopfli-pnginator

Quick utility to embbed javascript code in a (compressed) PNG image. Adds a custom chunk to the PNG that contains a tiny "html/js unpacking" script. Image data (input javascript source) will be compressed via zopfli (default) or standard deflate. Opening the PNG with file extension .html in a browser will unpack the image contents and execute the javascript.

Compile with gcc or clang: `gcc -lz -lzopfli -std=c17 -Wall -Wextra -pedantic zopfli-pnginator.c`

Based on:
- [daeken](https://daeken.dev/blog/2011-08-31_Superpacking_JS_Demos.html)
- [gasman](https://gist.github.com/gasman/2560551)

Uses:
- https://github.com/google/zopfli
- https://github.com/madler/zlib

Notes:
- Superseded by [https://github.com/mgnauck/js-payload-compress](https://github.com/mgnauck/js-payload-compress)
