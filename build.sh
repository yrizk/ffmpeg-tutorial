gcc -o $1 $1.c -lavutil -lavformat -lavcodec -lswscale -lswresample -lz -lm
`sdl2-config --cflags --libs` && ./$1 ~/Downloads/test.mp4
