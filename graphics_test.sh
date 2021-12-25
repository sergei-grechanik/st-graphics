#!/bin/bash

stty_orig=`stty -g`
stty -echo
stty susp undef

cleanup() {
    stty $stty_orig
}

trap cleanup EXIT TERM

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

line() {
    echo -en "\e[48;5;${BG}m"
    echo -en "\e[38;5;${ID}m"
    for col in `seq $1 $2`; do
        printf "\UEEEE${ROWCOLUMN_DIACRITICS[$3]}${ROWCOLUMN_DIACRITICS[$col]}"
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
echo -n "a=t,t=f,i=1,r=10,c=20;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=t,i=2,r=10,t=f,c=72;"
echo -n "$(realpath neuschwanstein.jpg | tr -d '\n' | base64 -w0)"
end_gr_command

ID=1
BG=2
box 0 19 0 9 > _1.txt

ID=2
BG=3
box 0 71 0 9 > _2.txt

paste -d "" _1.txt _2.txt

start_gr_command
echo -n "a=t,t=f,i=3,r=1,c=1;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=t,t=f,i=4,r=1,c=2;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=t,t=f,i=5,r=2,c=1;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=t,t=f,i=6,r=2,c=2;"
echo -n "$(realpath wikipedia.png | tr -d '\n' | base64 -w0)"
end_gr_command

start_gr_command
echo -n "a=t,t=f,i=7,r=1,c=10;"
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

start_gr_command
echo -n "a=t,t=f,i=8,r=10,c=20;"
echo -n "$(realpath transparency.png | tr -d '\n' | base64 -w0)"
end_gr_command

ID=8
for row in `seq 0 9`; do
    BG=$row
    line 0 19 $row
    echo
done


TMPDIR="$(mktemp -d)"
test_direct() {
    rm $TMPDIR/chunk_* 2> /dev/null
    cat neuschwanstein.jpg | base64 -w0 | split -b $1 - "$TMPDIR/chunk_"
    echo ======
    start_gr_command
    echo -n "a=t,t=d,i=$2,r=4,c=29,m=1;"
    end_gr_command
    # We need to wait a bit before we start transmission, in the script it will
    # be done using the read or something like this
    sleep 0.3
    for CHUNK in $TMPDIR/chunk_*; do
        start_gr_command
        echo -n "m=1;"
        cat $CHUNK
        end_gr_command
    done
    start_gr_command
    echo -n "m=0;"
    end_gr_command
    ID=$2
    box 0 28 0 3
}

test_directs() {
    test_direct 3800 9
}

test_directs
