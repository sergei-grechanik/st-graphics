#!/bin/bash

if [[ ! -f wikipedia.png ]]; then
    curl -o wikipedia.png https://upload.wikimedia.org/wikipedia/en/thumb/8/80/Wikipedia-logo-v2.svg/440px-Wikipedia-logo-v2.svg.png
fi

echo -en "\e_Ga=t,t=f,i=1,r=10,c=20;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
echo -en "\e\\"

cat cell-image-placeholder.txt | head -10 | cut -c -350
