curl -i -X POST 192.168.1.5:80/file/spiffs/$1 -H "Content-Type: text/html"   --data-binary "@www/$1"
