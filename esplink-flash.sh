PARTS="$@"
if [ -z "$PARTS" ]; then
    PARTS="app"
fi

ADDR_FILES=
for part in $PARTS
do
    if [ "$part" == "app" ]; then
        ADDR_FILES+="0x120000 ./build/netplayer.bin "
    elif [ "$part" == "bootldr" ]; then
        ADDR_FILES+="0x1000 ./build/bootloader/bootloader.bin "
    elif [ "$part" == "otadata" ]; then
        ADDR_FILES+="0xd000 ./build/ota_data_initial.bin "
    elif [ "$part" == "ptable" ]; then
        ADDR_FILES+="0x8000 ./build/partitions.bin "
    else
       echo "Unrecognized item '$part'"
       exit 1;
    fi
done
set -x
esptool.py --chip esp32 --port socket://192.168.1.78:23 --baud 115200 --before default_reset \
  --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect \
  $ADDR_FILES
