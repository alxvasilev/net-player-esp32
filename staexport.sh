#!/bin/bash
set -e

if [ -z "$1" ]; then
    echo "Export radio station list to JSON file"
    echo "Usage:"
    echo "$0 <filename>"
    exit 1
fi

curl http://netplayer.local/slist?a=dump -o "$1"
