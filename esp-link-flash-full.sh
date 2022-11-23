esptool.py --chip esp32 --port socket://192.168.1.78:23 --baud 115200 --before default_reset --after hard_reset write_flash\
  -z --flash_mode dio --flash_freq 80m --flash_size detect\
  0xd000 ./build/ota_data_initial.bin\
  0x1000 ./build/bootloader/bootloader.bin\
  0x60000 ./build/netplayer.bin\
  0x8000 ./build/partitions.bin
