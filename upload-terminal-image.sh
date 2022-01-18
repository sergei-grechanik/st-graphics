#!/bin/bash

# vim: shiftwidth=4

# TODO: This script uses bash-specific features. Would be nice to make it more
#       portable.

HELP="This script send the given image to the terminal and outputs characters
that can be used to display this image using the unicode image placeholder
symbol approach. Note that it will use a single line of the output to display
uploading progress (which may be disabled with '-q').

  Usage:
    $(basename $0) [OPTIONS] IMAGE_FILE

  Options:
    -c N, --columns N
        The number of columns for the image. By default the script will try to
        compute the optimal value by itself.
    -r N, --rows N
        The number of rows for the image. By default the script will try to
        compute the optimal value by itself.
    --256
        Restrict the image id to be within the range [1; 255] and use the 256
        colors mode to specify the image id (~24 bits will be used for image ids
        by default).
    -a, --append
        Do not clear the output file (the one specified with -o).
    -o FILE, --output FILE
        Use FILE to output the characters representing the image, instead of
        stdout.
    -e FILE, --err FILE
        Use FILE to output error messages in addition to displaying them as the
        status.
    -l FILE, --log FILE
        Enable logging and write logs to FILE.
    -f FILE, --file FILE
        The image file (but you can specify it as a positional argument).
    -q, --quiet
        Do not show status messages or uploading progress. Error messages are
        still shown (but you can redirect them with -e).
    --noesc
        Do not issue the escape codes representing row numbers (encoded as
        foreground color).
    --tmux-hijack
        If inside tmux, run image upload inside a temporary pane. This is useful
        as a workaround for the case when the pane from where the script is
        called may be out of focus (in which case tmux won't pass the graphics
        command to the terminal). This is very experimental.
    --max-cols N
        Do not exceed this value when automatically computing the number of
        columns. By default the width of the terminal is used as the maximum.
    --max-rows N
        Do not exceed this value when automatically computing the number of
        rows. This value cannot be larger than 255. By default the height of the
        terminal is used as the maximum.
    --cols-per-inch N
    --rows-per-inch N
        Floating point values specifying the number of terminal columns and rows
        per inch (may be approximate). Used to compute the optimal number of
        columns and rows when -c and -r are not specified. If these parameters
        are not specified, the environment variables
        TERMINAL_IMAGES_COLS_PER_INCH and TERMINAL_IMAGES_ROWS_PER_INCH will
        be used. If they are not specified either, 12.0 and 6.0 will be used as
        columns per inch and rows per inch respectively.
    --override-dpi N
        Override dpi value for the image (both vertical and horizontal). By
        default dpi values will be requested using the 'identify' utility.
    -h, --help
        Show this message

  Environment variables:
    TERMINAL_IMAGES_COLS_PER_INCH
    TERMINAL_IMAGES_ROWS_PER_INCH
        See  --cols-per-inch and --rows-per-inch.
    TERMINAL_IMAGES_CACHE_DIR
        The directory to store images being uploaded (in case if they need to be
        reuploaded) and information about used image ids.
"

# Exit the script on keyboard interrupt
trap "exit 1" INT

COLS=""
ROWS=""
MAX_COLS=""
MAX_ROWS=""
USE_256=""
COLS_PER_INCH="$TERMINAL_IMAGES_COLS_PER_INCH"
ROWS_PER_INCH="$TERMINAL_IMAGES_ROWS_PER_INCH"
CACHE_DIR="$TERMINAL_IMAGES_CACHE_DIR"
OVERRIDE_DPI=""
FILE=""
OUT="/dev/stdout"
ERR="/dev/null"
LOG=""
QUIET=""
NOESC=""
APPEND=""
TMUX_HIJACK=""
STORE_PID=""
DEFAULT_TIMEOUT=3

# A utility function to print logs
echolog() {
    if [[ -n "$LOG" ]]; then
        (flock 1; echo "$$ $(date +%s.%3N) $1") >> "$LOG"
    fi
}

# A utility function to display what the script is doing.
echostatus() {
    echolog "$1"
    if [[ -z "$QUIET" ]]; then
        # clear the current line
        echo -en "\033[2K\r"
        # And display the status
        echo -n "$1"
    fi
}

# Display an error message, both as the status and to $ERR.
echoerr() {
    echostatus "ERROR: $1"
    echo "$1" >> "$ERR"
}

# Parse the command line.
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--columns)
            COLS="$2"
            shift 2
            ;;
        -r|--rows)
            ROWS="$2"
            shift 2
            ;;
        -a|--append)
            APPEND="1"
            shift
            ;;
        -o|--output)
            OUT="$2"
            shift 2
            ;;
        -e|--err)
            ERR="$2"
            shift 2
            ;;
        -l|--log)
            LOG="$2"
            shift 2
            ;;
        -q|--quiet)
            QUIET="1"
            shift
            ;;
        -h|--help)
            echo "$HELP"
            exit 0
            ;;
        -f|--file)
            if [[ -n "$FILE" ]]; then
                echoerr "Multiple image files are not supported"
                exit 1
            fi
            FILE="$2"
            shift 2
            ;;
        --256)
            USE_256=1
            shift
            ;;
        --noesc)
            NOESC=1
            shift
            ;;
        --tmux-hijack)
            TMUX_HIJACK=1
            shift
            ;;
        --store-pid)
            STORE_PID="$2"
            shift 2
            ;;
        --max-cols)
            MAX_COLS="$2"
            shift 2
            ;;
        --max-rows)
            MAX_ROWS="$2"
            if (( MAX_ROWS > 255 )); then
                echoerr "--max-rows cannot be larger than 255 ($2 is specified)"
                exit 1
            fi
            shift 2
            ;;
        --cols-per-inch)
            COLS_PER_INCH="$2"
            shift 2
            ;;
        --rows-per-inch)
            ROWS_PER_INCH="$2"
            shift 2
            ;;
        --override-dpi)
            OVERRIDE_DPI="$2"
            shift 2
            ;;
        -*)
            echoerr "Unknown option: $1"
            exit 1
            ;;
        *)
            if [[ -n "$FILE" ]]; then
                echoerr "Multiple image files are not supported: $FILE and $1"
                exit 1
            fi
            FILE="$1"
            shift
            ;;
    esac
done

echolog ""

#####################################################################
# Detecting tmux and figuring out the actual terminal name
#####################################################################

# The name of the terminal, used to adjust to the specific flavor of the
# protocol (like whether we need to convert to PNG).
ACTUAL_TERM="$TERM"

# Some id of the terminal. Used to figure out whether some image has already
# been uploaded to the terminal without querying the terminal itself. Without
# tmux we just use the window id of the terminal which should somewhat uniquely
# identify the instance of the terminal.
TERMINAL_ID="$TERM-$WINDOWID"

# Some id of the session. Each session contains its own "namespace" of image
# ids, i.e. if two processes belong to the same session and load different
# images, these images have to be assigned to different image ids. Without tmux
# we just use the terminal id since we can't (easily) reattach a session to a
# different terminal. For tmux see below.
SESSION_ID="$TERMINAL_ID"

# This variable indicates whether we are inside tmux.
INSIDE_TMUX=""
if [[ -n "$TMUX" ]] && [[ "$TERM" =~ "screen" ]]; then
    INSIDE_TMUX="1"
    # Get the actual current terminal name from tmux.
    ACTUAL_TERM="$(tmux display-message -p "#{client_termname}")"
    # There doesn't seem to be a nice way to reliably get the current WINDOWID
    # of the terminal we are attached to (please correct me if I'm wrong). So we
    # use the last client pid of tmux as terminal id.
    TERMINAL_ID="tmux-client-$(tmux display-message -p "#{client_pid}")"
    # For the session id we use the tmux server pid with tmux session id.
    SESSION_ID="tmux-$(tmux display-message -p "#{pid}-#{session_id}")"
fi

# Replace non-alphabetical chars with '-' to make them suitable for dir names.
TERMINAL_ID="$(sed 's/[^0-9a-zA-Z]/-/g' <<< "$TERMINAL_ID")"
SESSION_ID="$(sed 's/[^0-9a-zA-Z]/-/g' <<< "$SESSION_ID")"

echolog "TERMINAL_ID=$TERMINAL_ID"
echolog "SESSION_ID=$SESSION_ID"

#####################################################################
# Compute the number of rows and columns
#####################################################################

echolog "Image file: $FILE"

if [[ ! -e "$FILE" ]]; then
    echoerr "The file '$FILE' does not exist"
    exit 1
fi

# Compute the formula with bc and round to the nearest integer.
bc_round() {
    echo "$(LC_NUMERIC=C printf %.0f "$(echo "scale=2;($1) + 0.5" | bc)")"
}

if [[ -z "$COLS" || -z "$ROWS" ]]; then
    # Compute the maximum number of rows and columns if these values were not
    # specified.
    if [[ -z "$MAX_COLS" ]]; then
        MAX_COLS="$(tput cols)"
    fi
    if [[ -z "$MAX_ROWS" ]]; then
        MAX_ROWS="$(tput lines)"
        if (( MAX_ROWS > 255 )); then
            MAX_ROWS=255
        fi
    fi
    # Default values of rows per inch and columns per inch.
    [[ -n "$COLS_PER_INCH" ]] || COLS_PER_INCH=12.0
    [[ -n "$ROWS_PER_INCH" ]] || ROWS_PER_INCH=6.0
    # Get the size of the image and its resolution
    PROPS=($(identify -format '%w %h %x %y' -units PixelsPerInch "$FILE"))
    if [[ "${#PROPS[@]}" -ne 4 ]]; then
        echoerr "Couldn't get result from identify"
        exit 1
    fi
    if [[ -n "$OVERRIDE_DPI" ]]; then
        PROPS[2]="$OVERRIDE_DPI"
        PROPS[3]="$OVERRIDE_DPI"
    fi
    echolog "Image pixel width: ${PROPS[0]} pixel height: ${PROPS[1]}"
    echolog "Image x dpi: ${PROPS[2]} y dpi: ${PROPS[3]}"
    echolog "Columns per inch: ${COLS_PER_INCH} Rows per inch: ${ROWS_PER_INCH}"
    OPT_COLS_EXPR="(${PROPS[0]}*${COLS_PER_INCH}/${PROPS[2]})"
    OPT_ROWS_EXPR="(${PROPS[1]}*${ROWS_PER_INCH}/${PROPS[3]})"
    if [[ -z "$COLS" && -z "$ROWS" ]]; then
        # If columns and rows are not specified, compute the optimal values
        # using the information about rows and columns per inch.
        COLS="$(bc_round "$OPT_COLS_EXPR")"
        ROWS="$(bc_round "$OPT_ROWS_EXPR")"
    elif [[ -z "$COLS" ]]; then
        # If only one dimension is specified, compute the other one to match the
        # aspect ratio as close as possible.
        COLS="$(bc_round "${OPT_COLS_EXPR}*${ROWS}/${OPT_ROWS_EXPR}")"
    elif [[ -z "$ROWS" ]]; then
        ROWS="$(bc_round "${OPT_ROWS_EXPR}*${COLS}/${OPT_COLS_EXPR}")"
    fi

    echolog "Image size before applying min/max columns: $COLS, rows: $ROWS"
    # Make sure that automatically computed rows and columns are within some
    # sane limits
    if (( COLS > MAX_COLS )); then
        ROWS="$(bc_round "$ROWS * $MAX_COLS / $COLS")"
        COLS="$MAX_COLS"
    fi
    if (( ROWS > MAX_ROWS )); then
        COLS="$(bc_round "$COLS * $MAX_ROWS / $ROWS")"
        ROWS="$MAX_ROWS"
    fi
    if (( COLS < 1 )); then
        COLS=1
    fi
    if (( ROWS < 1 )); then
        ROWS=1
    fi
fi

echolog "Image size columns: $COLS, rows: $ROWS"

#####################################################################
# Helper functions for finding image ids
#####################################################################

# Checks if the given string is an integer within the correct id range.
is_image_id_correct() {
    if [[ "$1" =~ ^[0-9]+$ ]]; then
        if [[ -n "$USE_256" ]]; then
            if (( "$1" < 256 )) && (( "$1" > 0 )); then
                return 0
            fi
        else
            if (( "$1" >= 256 )) && (( "$1" < 16777216 )); then
                return 0
            fi
        fi
    fi
    echoerr "$1 is incorrect"
    return 1
}

# Finds an image id for the instance $IMG_INSTANCE.
find_image_id() {
    local INST_FILE
    # Make sure that some_dir/* returns an empty list if some_dir is empty.
    shopt -s nullglob
    # Try to find an existing session image id corresponding to the instance.
    INST_FILE="$SESSION_DIR/$IMG_INSTANCE"
    if [[ -e "$INST_FILE" ]]; then
        IMAGE_ID="$(head -1 "$INST_FILE")"
        if ! is_image_id_correct "$IMAGE_ID"; then
            echoerr "Found invalid image_id $IMAGE_ID, deleting $INST_FILE"
            rm "$INST_FILE"
        else
            touch "$INST_FILE"
            echolog "Found an existing image id $IMAGE_ID"
            return 0
        fi
    fi
    # If there is no image id corresponding to the instance, try to find a free
    # image id.
    if [[ -n "$USE_256" ]]; then
        local ID
        local IDS_ARRAY=($(seq 0 255))
        # Load ids and mark occupied ids with "0" (0 is an invalid id anyway).
        for INST_FILE in "$SESSION_DIR"/*; do
            ID="$(head -1 "$INST_FILE")"
            if ! is_image_id_correct "$ID"; then
                echoerr "Found invalid image_id $ID, deleting $INST_FILE"
                rm "$INST_FILE"
            else
                IDS_ARRAY[$ID]="0"
            fi
        done
        # Try to find an array element that is not "0".
        for ID in "${IDS_ARRAY[@]}"; do
            if [[ "$ID" != "0" ]]; then
                IMAGE_ID="$ID"
                echolog "Found a free image id $IMAGE_ID"
                return 0
            fi
        done
        # On failure we need to reassign the id of the oldest image.
        local OLDEST_FILE=""
        for INST_FILE in "$SESSION_DIR"/*; do
            if [[ -z $OLDEST_FILE || $INST_FILE -ot $OLDEST_FILE ]]; then
                OLDEST_FILE="$INST_FILE"
            fi
        done
        IMAGE_ID="$(head -1 "$INST_FILE")"
        echolog "Recuperating the id $IMAGE_ID from $OLDEST_FILE"
        rm "$OLDEST_FILE"
        return 0
    else
        local IDS_ARRAY=()
        # Load ids into the array.
        for INST_FILE in "$SESSION_DIR"/*; do
            ID="$(head -1 "$INST_FILE")"
            if ! is_image_id_correct "$ID"; then
                echoerr "Found invalid image_id $ID, deleting $INST_FILE"
                rm "$INST_FILE"
            else
                IDS_ARRAY+="$ID"
            fi
        done
        IMAGE_ID=""
        # Generate a random id until we find one that is not in use.
        while [[ -z "$IMAGE_ID" ]]; do
            IMAGE_ID="$(shuf -i 256-16777215 -n 1)"
            # Check that the id is not in use
            for ID in "${IDS_ARRAY[@]}"; do
                if [[ "$IMAGE_ID" == "$ID" ]]; then
                    IMAGE_ID=""
                    break
                fi
            done
        done
        return 0
    fi
}

#####################################################################
# Managing the cache dir and assigning the image id
#####################################################################

[[ -n "$CACHE_DIR" ]] || CACHE_DIR="$HOME/.cache/terminal-images"

# 8-bit and 24-bit image ids live in different namespaces. We use different
# session and terminal directories to store information about them.
ID_SUBSPACE="24bit_ids"
if [[ -n "$USE_256" ]]; then
    ID_SUBSPACE="8bit_ids"
fi

SESSION_DIR="$CACHE_DIR/sessions/${SESSION_ID}-${ID_SUBSPACE}"

mkdir -p "$CACHE_DIR/cache/" 2> /dev/null
mkdir -p "$SESSION_DIR" 2> /dev/null

# Compute md5sum and copy the file to the cache dir.
IMG_MD5="$(md5sum "$FILE" | cut -f 1 -d " ")"
echolog "Image md5sum: $IMG_MD5"

CACHED_FILE="$CACHE_DIR/cache/$IMG_MD5"
if [[ ! -e "$CACHED_FILE" ]]; then
    cp "$FILE" "$CACHED_FILE"
else
    touch "$CACHED_FILE"
fi

# Image instance is an image with its positioning attributes: columns, rows and
# any other attributes we may add in the future (alignment, scale mode, etc).
# Each image id corresponds to a single image instance.
IMG_INSTANCE="${IMG_MD5}_${COLS}_${ROWS}"

# Find an id for the image. We want to avoid reuploading, and at the same time
# we want to minimize image id collisions.
echostatus "Searching for a free image id"
(
    flock --timeout "$DEFAULT_TIMEOUT" 9 || \
        { echoerr "Could not acquire a lock on $SESSION_DIR.lock"; exit 1; }
    # Find the image id (see the function definition below).
    IMAGE_ID=""
    find_image_id
    if [[ -z "$IMAGE_ID" ]]; then
        echoerr "Failed to find an image id"
        exit 1
    fi
    # If it hasn't been loaded, create the instance file.
    if [[ ! -e "$SESSION_DIR/$IMG_INSTANCE" ]]; then
        echo "$IMAGE_ID" > "$SESSION_DIR/$IMG_INSTANCE"
    fi
) 9>"$SESSION_DIR.lock" || exit 1

IMAGE_ID="$(head -1 "$SESSION_DIR/$IMG_INSTANCE")"

echostatus "Found an image id $IMAGE_ID"
exit 1

#####################################################################
# Tmux hijacking
#####################################################################

# Store the pid of the current process. It is used for --tmux-hijack
if [[ -n "$STORE_PID" ]]; then
    echolog "Storing $$ to $STORE_PID"
    echo "$$" > "$STORE_PID"
fi

# Run the script inside a new tmux pane if requested
# TODO: This is slow, it would be better to check if current pane is active and
# create a new pane only if it's not.
if [[ -n "$INSIDE_TMUX" && -n "$TMUX_HIJACK" ]]; then
    echoopt() {
        if [[ -n $1 ]]; then
          echo "$2"
        fi
    }
    TMP_OUT="$(mktemp)"
    TMP_PID="$(mktemp)"
    echolog "Hijacking tmux. TMP_OUT=$TMP_OUT TMP_PID=$TMP_PID"
    # Note that we've already computed the number of rows and columns, so we don't
    # have to do it in the "child" process.
    tmux split-window -l 1 "$0" \
        -c "$COLS" \
        -r "$ROWS" \
        -o "$TMP_OUT" \
        -e "$ERR" \
        -l "$LOG" \
        -f "$FILE" \
        --store-pid "$TMP_PID" \
        $(echoopt "$NOESC" "--noesc")
    if [[ $? -ne 0 ]]; then
        exit 1
    fi
    # The process we've created should write its pid to the specified file. It's
    # not always quick.
    for ITER in $(seq 10); do
        sleep 0.1
        PID_TO_WAIT="$(cat $TMP_PID)"
        if [[ -n "$PID_TO_WAIT" ]]; then
            break
        fi
    done
    if [[ -z "$PID_TO_WAIT" ]]; then
        echoerr "Can't create a tmux hijacking process"
        exit 1
    fi
    echolog "Waiting for the process $PID_TO_WAIT"
    # Wait for the process to finish.
    # tail --pid=$PID_TO_WAIT -f /dev/null
    while true; do
        sleep 0.1
        if ! kill -0 "$PID_TO_WAIT" 2> /dev/null; then
            break
        fi
    done
    echolog "Process $PID_TO_WAIT finished"
    # Clear the output file
    if [[ -z "$APPEND" ]]; then
        > "$OUT"
    fi
    # If the output file size is 0, we construe it as a failure of the script
    if [[ ! -s $TMP_OUT ]]; then
        echolog "Seems like the file size is 0"
        exit 1
    fi
    cat "$TMP_OUT" >> "$OUT"
    rm "$TMP_OUT" 2> /dev/null || echolog "Could not rm $TMP_OUT"
    rm "$TMP_PID" 2> /dev/null || echolog "Could not rm $TMP_PID"
    exit 0
fi

#####################################################################
# Some general preparations
#####################################################################

# Create a temporary directory to store the chunked image.
TMPDIR="$(mktemp -d)"

if [[ ! "$TMPDIR" || ! -d "$TMPDIR" ]]; then
    echoerr "Can't create a temp dir"
    exit 1
fi

# We need to disable echo, otherwise the response from the terminal containing
# the image id will get echoed. We will restore the terminal settings on exit
# unless we get brutally killed.
stty_orig=`stty -g`
stty -echo
# Disable ctrl-z. Pressing ctrl-z during image uploading may cause some horrible
# issues otherwise.
stty susp undef

# Utility to read response from the terminal that we don't need anymore. (If we
# don't read it it may end up being displayed which is not pretty).
consume_errors() {
    while read -r -d '\' -t 0.1 TERM_RESPONSE; do
        echolog "Consuming unneeded response: $(sed 's/\x1b/^[/g' <<< "$TERM_RESPONSE")"
    done
}

# On exit restore terminal settings, consume possible errors from the terminal
# and remove the temporary directory.
cleanup() {
    consume_errors
    stty $stty_orig
    rm $TMPDIR/chunk_* 2> /dev/null
    rm $TMPDIR/image* 2> /dev/null
    rmdir $TMPDIR || echolog "Could not remove $TMPDIR"
}

# Register the cleanup function to be called on the EXIT signal.
trap cleanup EXIT TERM

# Check if the file exists.
if ! [[ -f "$FILE" ]]; then
    echoerr "File not found: $FILE (pwd: $(pwd))"
    exit 1
fi

echolog "Uploading file: $FILE (pwd: $(pwd))"

#####################################################################
# Helper functions
#####################################################################

# Functions to emit the start and the end of a graphics command.
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

# Send a graphics command with the correct start and end
gr_command() {
    start_gr_command
    echo -en "$1"
    end_gr_command
    if [[ -n "$LOG" ]]; then
        local GR_COMMAND="$(start_gr_command)$(echo -en "$1")$(end_gr_command)"
        echolog "SENDING COMMAND: $(sed 's/\x1b/^[/g' <<< "$GR_COMMAND")"
    fi
}

# Show the invalid terminal response message.
invalid_terminal_response() {
    # Replace control characters with '?'.
    echoerr "Invalid terminal response: $(sed 's/\x1b/^[/g' <<< "$TERM_RESPONSE")"
}

# Get a response from the terminal and store it in TERM_RESPONSE,
# aborts the script if there is no response.
get_terminal_response() {
    TERM_RESPONSE=""
    # -r means backslash is part of the line
    # -d '\' means \ is the line delimiter
    # -t 0.5 is timeout
    if ! read -r -d '\' -t 2 TERM_RESPONSE; then
        if [[ -z "$TERM_RESPONSE" ]]; then
            echoerr "No response from terminal"
        else
            invalid_terminal_response
        fi
        exit 1
    fi
    echolog "TERM_RESPONSE: $(sed 's/\x1b/^[/g' <<< "$TERM_RESPONSE")"
}


# Output characters representing the image
output_image() {
    # Convert the image id to the corresponding unicode symbol.
    local IMAGE_ID="$(printf "%x" "$1")"
    local IMAGE_SYMBOL="$(printf "\U$IMAGE_ID")"

    echostatus "Successfully received imaged id: $IMAGE_ID ($1)"
    echostatus

    # Clear the output file
    if [[ -z "$APPEND" ]]; then
        > "$OUT"
    fi

    local ROW=""
    for X in `seq 0 $(expr $COLS - 1)`; do
        ROW="$ROW$IMAGE_SYMBOL"
    done

    # Fill the output with characters representing the image
    for Y in `seq 0 $(expr $ROWS - 1)`; do
        # Each line starts with the escape sequence to set the foreground color
        # to the row number.
        if [[ -z "$NOESC" ]]; then
            echo -en "\e[38;5;${Y}m" >> "$OUT"
        fi
        # And then we just repeat the unicode symbol.
        echo -en "$ROW" >> "$OUT"
        printf "\n" >> "$OUT"
    done

    if [[ -z "$NOESC" && "$OUT" == "/dev/stdout" ]]; then
        # Reset the style. This is useful when stdout is the same as $OUT to
        # prevent colors used for image display leaking to the subsequent text.
        echo -en "\e[0m"
    fi

    echolog "Finished displaying the image"

    return 0
}

#####################################################################
# Try to query the image client id by md5sum
#####################################################################

echostatus "Trying to find image by md5sum"

# Compute image IMGUID based on its md5sum and the number of rows and columns.
IMGUID="$(md5sum "$FILE" | cut -f 1 -d " ")x${ROWS}x${COLS}"
# Pad it with '='' so it looks like a base64 encoding of something (we could
# actually encode the md5sum but I'm too lazy, and the terminal doesn't care
# whatever UIDs we assign to images).
UID_LEN="${#IMGUID}"
PAD_LEN="$((4 - ($UID_LEN % 4)))"
for i in $(seq $PAD_LEN); do
    IMGUID="${IMGUID}="
done

# a=U    the action is to query the image by IMGUID
# q=1    be quiet
gr_command "a=U,q=1;${IMGUID}"

get_terminal_response

# Parse the image client ID in the response.
IMAGE_ID="$(sed -n "s/^.*_G.*i=\([0-9]\+\).*;OK.*$/\1/p" <<< "$TERM_RESPONSE")"

if ! [[ "$IMAGE_ID" =~ ^[0-9]+$ ]]; then
    # If there is no image id in the response then the response should contain
    # something like NOTFOUND
    NOT_FOUND="$(sed -n "s/^.*_G.*;.*NOT.*FOUND.*$/NOTFOUND/p" <<< "$TERM_RESPONSE")"
    if [[ -z "$NOT_FOUND" ]]; then
        # Otherwise the terminal behaves in an unexpected way, better quit.
        invalid_terminal_response
        exit 1
    fi
else
    output_image "$IMAGE_ID"
    exit 0
fi

#####################################################################
# Chunk and upload the image
#####################################################################

# Check if the image is a png, and if it's not, try to convert it.
if ! (file "$FILE" | grep -q "PNG image"); then
    echostatus "Converting $FILE to png"
    if ! convert "$FILE" "$TMPDIR/image.png" || ! [[ -f "$TMPDIR/image.png" ]]; then
        echoerr "Cannot convert image to png"
        exit 1
    fi
    FILE="$TMPDIR/image.png"
fi

# Use some random number for the I id, not to be confused with the client id
# of the image which is not known yet.
ID=$RANDOM

# base64-encode the file and split it into chunks.
echolog "base64-encoding and chunking the image"
cat "$FILE" | base64 -w0 | split -b 4096 - "$TMPDIR/chunk_"

# Issue a command indicating that we want to start data transmission for a new
# image.
# a=t    the action is to transmit data
# I=$ID
# f=100  PNG
# t=d    transmit data directly
# c=,r=  width and height in cells
# s=,v=  width and height in pixels (not used)
# o=z    use compression (not used)
# m=1    multi-chunked data
gr_command "a=t,I=$ID,f=100,t=d,c=${COLS},r=${ROWS},m=1"

CHUNKS_COUNT="$(ls -1 $TMPDIR/chunk_* | wc -l)"
CHUNK_I=0
STARTTIME="$(date +%s%3N)"
SPEED=""

# Transmit chunks and display progress.
for CHUNK in $TMPDIR/chunk_*; do
    echolog "Uploading chunk $CHUNK"
    CHUNK_I=$((CHUNK_I+1))
    if [[ $((CHUNK_I % 10)) -eq 1 ]]; then
        # Do not compute the speed too often
        if [[ $((CHUNK_I % 100)) -eq 1 ]]; then
            # We use +%s%3N tow show time in nanoseconds
            CURTIME="$(date +%s%3N)"
            TIMEDIFF="$((CURTIME - STARTTIME))"
            if [[ "$TIMEDIFF" -ne 0 ]]; then
                SPEED="$(((CHUNK_I*4 - 4)*1000/TIMEDIFF)) K/s"
            fi
        fi
        echostatus "$((CHUNK_I*4))/$((CHUNKS_COUNT*4))K [$SPEED]"
    fi
    # The uploading of the chunk goes here.
    start_gr_command
    echo -en "I=$ID,m=1;"
    cat $CHUNK
    end_gr_command
done

# Tell the terminal that we are done.
gr_command "I=$ID,m=0"

echostatus "Awaiting terminal response"
get_terminal_response

# The terminal should respond with the client id for the image.
IMAGE_ID="$(sed -n "s/^.*_G.*i=\([0-9]\+\),I=${ID}.*;OK.*$/\1/p" <<< "$TERM_RESPONSE")"

if ! [[ "$IMAGE_ID" =~ ^[0-9]+$ ]]; then
    invalid_terminal_response
    exit 1
else
    # Set UID for the uploaded image.
    gr_command "a=U,i=$IMAGE_ID,q=1;${IMGUID}"

    output_image "$IMAGE_ID"
    exit 0
fi
