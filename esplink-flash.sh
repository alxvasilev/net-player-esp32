set -e
PARTS="$@"
if [ -z "$PARTS" ]; then
    PARTS="app"
elif [ -z "$PARTS" ] || [ "$PARTS" == "-h" ] || [ "$PARTS" == "--help" ]; then
   echo -e "Script for flashing via an esp-link device and esptool.py\nUsage:"
   echo -e "$0 [app|bootldr|otadata|ptable|recovery]"
   exit 1
fi

base="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
partoffs="$base/partoffset.py $base/partitions.csv \$part"
ADDR_FILES=
for part in $PARTS
do
    if [ "$part" == "app" ]; then
        ADDR_FILES+="$(eval $partoffs) $base/build/netplayer.bin "
    elif [ "$part" == "bootldr" ]; then
        ADDR_FILES+="0x1000 $base/build/bootloader/bootloader.bin "
    elif [ "$part" == "otadata" ]; then
        ADDR_FILES+="$(eval $partoffs) $base/build/ota_data_initial.bin "
    elif [ "$part" == "ptable" ]; then
        ptfile="$base/build/partition_table/partition-table.bin"
        if [ ! -f "$ptfile" ]; then
            echo "Partition table bin file not found for IDF version >= 5, trying legacy file"
            ptfile="$base/build/partitions.bin"
	fi
        ADDR_FILES+="0x8000 $ptfile "
    elif [ "$part" == "recovery" ]; then
	ADDR_FILES+="$(eval $partoffs) $base/recovery/build/recovery.bin "
    else
       echo "Unrecognized item '$part'"
       exit 1;
    fi
done

set -x
esptool.py --chip esp32 --port socket://192.168.1.78:23 --baud 115200 --before default_reset \
  --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect \
  $ADDR_FILES
