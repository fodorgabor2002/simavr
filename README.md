Clone után:

- export LDFLAGS="-Wl,--copy-dt-needed-entries"

- make clean (biztonság kedvéért, friss futtatás esetén nem szükséges)

- make

- ./examples/board_olimex_avr_mt128/obj-x86_64-linux-gnu/mt128.elf ./examples/board_olimex_avr_mt128/atmega128_flappybird.axf