set backtrace limit 25
set confirm 0
set pagination 0
set print pretty
set unwindonsignal
file ../gcc-install/libexec/gcc/x86_64-unknown-linux-gnu/4.5.0/cc1
run data/test.c \
    -dumpbase test.c -quiet -o /dev/null \
    -O0 -Wall -Wextra \
    -fplugin=./slplug.so \
    -fplugin-arg-slplug-verbose=1 \
    -fplugin-arg-slplug-dump-pp
echo \nbt:\n
bt
echo \nframe:\n
f 1
echo \ncode:\n
list
echo \n\n
