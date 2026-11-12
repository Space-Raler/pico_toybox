SRC=../src/log.c ../src/store.c ../src/persist.c ../src/ctl.c ../src/ld_pad.c ../src/mode_inf.c ../src/mode_sky.c ../src/mode_ld.c \
 ../vendor/inf/infinity_core.c ../vendor/inf/inf_crypto.c ../vendor/inf/infinity_figures.c ../vendor/inf/ble_proto.c \
 ../vendor/sky/core/sky_core.c ../vendor/sky/sky_ble.c ../vendor/ld/ld_ble.c ../vendor/ld/tea.c ../vendor/ld/burtle.c \
 ../vendor/ld/bytes.c ../vendor/ld/minifigures.c ../vendor/ld/debug.c ../vendor/ld/usb/frame.c
host_test: host_test.c $(SRC)
	gcc -std=gnu11 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -I../shim -I../vendor/ld/usb -o $@ $^
test: host_test
	./host_test
