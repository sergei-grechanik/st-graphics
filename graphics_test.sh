#!/bin/bash

INSIDE_TMUX=""
if [[ -n "$TMUX" ]] && [[ "$TERM" =~ "screen" ]]; then
    INSIDE_TMUX="1"
fi

if [[ -n "$INSIDE_TMUX" ]]; then
    # If we are in tmux we have to wrap the command in Ptmux.
    start_gr_command() {
        echo -en '\ePtmux;\e\e_G'
    }
    end_gr_command() {
        echo -en '\e\e\\\e\\'
    }
else
    start_gr_command() {
        echo -en '\e_G'
    }
    end_gr_command() {
        echo -en '\e\\'
    }
fi

. rowcolumn_diacritics.sh

[[ -f wikipedia.png ]] || \
    curl -o wikipedia.png https://upload.wikimedia.org/wikipedia/en/thumb/8/80/Wikipedia-logo-v2.svg/440px-Wikipedia-logo-v2.svg.png
[[ -f neuschwanstein.jpg ]] || \
    curl -o neuschwanstein.jpg https://upload.wikimedia.org/wikipedia/commons/1/10/Neuschwanstein_Castle_from_Marienbr%C3%BCcke_Bridge.jpg
[[ -f transparency.png ]] || \
    curl -o transparency.png https://upload.wikimedia.org/wikipedia/commons/4/47/PNG_transparency_demonstration_1.png

mkdir _test_screenshots

stty_orig=`stty -g`
reset
stty -echo
stty susp undef

cleanup() {
    stty $stty_orig
}

trap cleanup EXIT TERM

save_screenshot() {
    local name="$1"
    sleep 0.2
    import -depth 8 -resize 320x192 -window "$WINDOWID" "_test_screenshots/$name.jpg"
}

read_response() {
    TERM_RESPONSE=""
    while read -r -d '\' -t 0.1 TERM_RESPONSE; do
        echo "        $(sed 's/\x1b/^[/g' <<< "$TERM_RESPONSE")"
    done
}

line() {
    echo -en "\e[48;5;${BG}m"
    echo -en "\e[38;5;${ID}m"
    for col in `seq $1 $2`; do
        printf "\U10EEEE${ROWCOLUMN_DIACRITICS[$3]}${ROWCOLUMN_DIACRITICS[$col]}"
    done
    echo -en "\e[49;m"
    echo -en "\e[39;m"
}

box() {
    for row in `seq $3 $4`; do
        line $1 $2 $row
        echo
    done
}

setimg() {
    echo -en "\e[38;5;${1}m"
}

start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=1,r=10,c=20,q=1;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=T,U=1,f=100,i=2,r=10,t=f,c=72,q=1;"
echo -n "$(realpath neuschwanstein.jpg | tr -d '\n' | base64 -w0)"
end_gr_command

echo "Wikipedia logo and neuschwanstein side by side:"

ID=1
BG=2
box 0 19 0 9 > _1.txt

# We crop the neuschwanstein image to 60 columns to get 80 columns total.
ID=2
BG=3
box 0 59 0 9 > _2.txt

paste -d "" _1.txt _2.txt

save_screenshot "010-wiki-neuschwanstein"


echo "Wikipedia logo in different sizes:"

start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=3,r=1,c=1,q=1;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=4,r=1,c=2,q=1;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=5,r=2,c=1,q=1;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=6,r=2,c=2,q=1;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=7,r=1,c=10,q=1;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

BG=1

ID=3
line 0 0 0
ID=4
line 0 1 0
ID=7
line 0 9 0
echo

ID=5
box 0 0 0 1 > _1.txt
ID=6
box 0 1 0 1 > _2.txt
paste -d "" _1.txt _2.txt

save_screenshot "020-wikipedia-sizes"


echo "Transparency test:"

start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=8,r=10,c=20,q=1;"
echo -n "$(realpath transparency.png | tr -d '\n' | base64 -w0)"
end_gr_command

ID=8
for row in `seq 0 9`; do
    BG=$row
    line 0 19 $row
    echo
done

echo "Changing the dimensions of the placement:"
start_gr_command
echo -n "a=t,U=1,f=100,t=f,i=9,r=10,c=20,q=1;"
echo -n "$(realpath transparency.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=p,p=1,U=1,i=9,r=5,c=10,q=1;"
end_gr_command

ID=9
BG=2
box 0 9 0 4

start_gr_command
echo -n "a=p,p=1,U=1,i=9,r=2,c=2,q=1;"
end_gr_command

ID=9
BG=2
box 0 1 0 1

save_screenshot "030-transparency"


echo "Neuschwanstein with blocks of wikipedia inside:"

BG=0

N=0
ID=2 line 0 71 $N; echo
N=1
ID=2 line 0 71 $N; echo
N=2
ID=2 line 0 9 $N; ID=1 line 5 14 $N; ID=2 line 20 29 $N; ID=1 line 5 14 $N; ID=2 line 40 71 $N; echo
N=3
ID=2 line 0 9 $N; ID=1 line 5 14 $N; ID=2 line 20 29 $N; ID=1 line 5 14 $N; ID=2 line 40 40 $N;
ID=1 line 1 9 $N; ID=2 line 50 50 $N; ID=1 line 11 19 $N; ID=2 line 60 71 $N; echo
N=4
ID=2 line 0 9 $N; ID=1 line 5 14 $N; ID=2 line 20 29 $N; ID=1 line 5 14 $N; ID=2 line 40 40 $N;
ID=1 line 1 9 $N; ID=2 line 50 50 $N; ID=1 line 11 19 $N; ID=2 line 60 71 $N; echo
N=5
ID=2 line 0 9 $N; ID=1 line 5 14 $N; ID=2 line 20 29 $N; ID=1 line 5 14 $N; ID=2 line 40 40 $N;
ID=1 line 1 9 $N; ID=2 line 50 50 $N; ID=1 line 11 19 $N; ID=2 line 60 71 $N; echo
N=6
ID=2 line 0 40 $N; ID=1 line 1 19 $N; ID=2 line 60 71 $N; echo
N=7
ID=2 line 0 40 $N; ID=1 line 1 19 $N; ID=2 line 60 71 $N; echo
N=8
ID=2 line 0 40 $N; ID=1 line 1 19 7; ID=2 line 60 71 $N; echo
N=9
ID=2 line 0 40 $N; ID=1 line 1 9 8; ID=2 line 50 50 $N; ID=1 line 11 19 8; ID=2 line 60 71 $N; echo

save_screenshot "040-neuschwanstein-with-wikipedia"


echo "Part of wiki logo, lower half indented:"

ID=1 line 0 9 3; echo "aaaaaaaaaa"
ID=1 line 0 9 4; echo "aaaaaaaaaa"
echo -n "bbbbbbbbbb"; ID=1 line 0 9 5; echo
echo -n "bbbbbbbbbb"; ID=1 line 0 9 6; echo

save_screenshot "050-wikipedia-lower-half-indented"


echo "Direct uploading test:"

TMPDIR="$(mktemp -d)"
test_direct() {
    rm $TMPDIR/chunk_* 2> /dev/null
    file=neuschwanstein.jpg
    cat "$file" | base64 -w0 | split -b $1 - "$TMPDIR/chunk_"
    start_gr_command
    echo -n "a=T,U=1,f=100,t=d,i=$2,r=4,c=29,m=1,S=$(wc -c < "$file"),q=$QUIET;"
    end_gr_command
    # We need to wait a bit before we start transmission, in the script it will
    # be done using the read or something like this
    sleep 0.3
    if [[ -z "$3" ]]; then
        for CHUNK in $TMPDIR/chunk_*; do
            start_gr_command
            echo -n "m=1;"
            cat $CHUNK
            end_gr_command
        done
    fi
    start_gr_command
    echo -n "m=0;"
    end_gr_command
    ID=$2
    box 0 28 0 3
}

QUIET=1
test_direct 3800 10

save_screenshot "060-direct-upload"


echo "Invalid image (broken uploading):"
QUIET=2
test_direct 3800 200 t

echo "Invalid image (not uploaded):"

BG=2
ID=201
box 0 20 0 1

echo "Invalid image (non-image file):"

start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=202,r=1,c=1,q=2;"
echo -n "$(realpath README | tr -d '\n' | base64 -w0)"
end_gr_command

BG=3
ID=202
box 0 20 0 1

echo "There shouldn't be any responses here:"

read_response
echo

save_screenshot "070-invalid-image"


echo "Now testing responses"

echo "OK after uploading by filename:"
start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=203,r=10,c=20;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

echo "Invalid image (file doesn't exist):"
start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=250,r=1,c=1;"
echo -n "$(echo __dummy__nonexisting | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

echo "Invalid image (non-image file):"
start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=250,r=1,c=1;"
echo -n "$(echo $0 | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

echo "Invalid transmission medium:"
start_gr_command
echo -n "a=T,U=1,f=100,t=X,i=250,r=1,c=1;"
echo -n "$(echo __dummy__nonexisting | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

echo "Invalid action:"
start_gr_command
echo -n "a=X,t=f,i=250,r=1,c=1;"
echo -n "$(echo __dummy__nonexisting | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

echo "No action:"
start_gr_command
echo -n "t=f,i=250,r=1,c=1;"
echo -n "$(echo __dummy__nonexisting | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

echo "Unknown key:"
start_gr_command
echo -n "key=1,a=T,U=1,f=100,t=f,i=250,r=1,c=1;"
echo -n "$(echo __dummy__nonexisting | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

echo "Key without value:"
start_gr_command
echo -n "a=T,U=1,f=100,t=f,i,r=1,c=1;"
echo -n "$(echo __dummy__nonexisting | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

echo "Zero image id:"
start_gr_command
echo -n "a=T,U=1,f=100,t=f,i=0,r=1,c=1;"
echo -n "$(echo __dummy__nonexisting | tr -d '\n' | base64 -w0)"
end_gr_command
read_response

save_screenshot "080-responses"
