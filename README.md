# zopfli-pnginator

Embbed javascript code in PNG and add custom chunk with a "html unpacking" script. Image data (= js code) compression done via standard deflate or zopfli.

Compile: `clang -lz -lzopfli -std=c17 -Wall -Wextra -pedantic zopfli-pnginator.c`

Based on:
[daeken](https://daeken.dev/blog/2011-08-31_Superpacking_JS_Demos.html)
[gasman](https://gist.github.com/gasman/2560551)
