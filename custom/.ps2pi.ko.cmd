cmd_custom/ps2pi.ko := ld -EL -r  -T ./scripts/module-common.lds --build-id  -o custom/ps2pi.ko custom/ps2pi.o custom/ps2pi.mod.o
