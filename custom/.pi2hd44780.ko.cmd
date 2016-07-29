cmd_custom/pi2hd44780.ko := ld -EL -r  -T ./scripts/module-common.lds --build-id  -o custom/pi2hd44780.ko custom/pi2hd44780.o custom/pi2hd44780.mod.o
