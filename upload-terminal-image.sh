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
    --id N
        Use the specified image id instead of finding a free one.
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
    --no-tmux-hijack
        Do not try to hijack focus by creating a new pane when inside tmux and
        the current pane is not active (just fail instead).
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
IMAGE_D=""
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
TMUX_HIJACK_ALLOWED="1"
TMUX_HIJACK_HELPER=""
STORE_CODE=""
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
        --id)
            IMAGE_ID="$2"
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
        --no-tmux-hijack)
            TMUX_HIJACK_ALLOWED=""
            shift
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

        # Options used internally.
        --store-pid)
            STORE_PID="$2"
            shift 2
            ;;
        --store-code)
            STORE_CODE="$2"
            shift 2
            ;;
        --tmux-hijack-helper)
            TMUX_HIJACK_HELPER=1
            shift
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

# Store the pid of the current process if requested. This is needed for tmux
# hijacking so that the original script can wait for the uploading process.
if [[ -n "$STORE_PID" ]]; then
    echolog "Storing $$ to $STORE_PID"
    echo "$$" > "$STORE_PID"
fi

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
# Creating a temp dir and adjusting the terminal state
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

#####################################################################
# Helper functions for image uploading
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
    echoerr "Invalid terminal response: $(sed 's/\x1b/^[/g' <<< "$TERM_RESPONSE")"
}

# Get a response from the terminal and store it in TERM_RESPONSE,
# returns 1 if there is no response.
get_terminal_response() {
    TERM_RESPONSE=""
    TERM_RESPONSE_PRINTABLE=""
    # -r means backslash is part of the line
    # -d '\' means \ is the line delimiter
    # -t 2 is timeout
    if ! read -r -d '\' -t 2 TERM_RESPONSE; then
        if [[ -z "$TERM_RESPONSE" ]]; then
            echoerr "No response from terminal"
        else
            invalid_terminal_response
        fi
        return 1
    fi
    TERM_RESPONSE_PRINTABLE="$(sed 's/\x1b/^[/g' <<< "$TERM_RESPONSE")"
    echolog "TERM_RESPONSE: $TERM_RESPONSE_PRINTABLE"
}

# Uploads an image to the terminal.
# Usage: upload_image $IMAGE_ID $FILE $COLS $ROWS
upload_image() {
    local IMAGE_ID="$1"
    local FILE="$2"
    local COLS="$3"
    local ROWS="$4"

    if [[ "$ACTUAL_TERM" == *kitty* ]]; then
        # Check if the image is a png, and if it's not, try to convert it.
        if ! (file "$FILE" | grep -q "PNG image"); then
            echostatus "Converting $FILE to png"
            if ! convert "$FILE" "$TMPDIR/image.png" || \
                    ! [[ -f "$TMPDIR/image.png" ]]; then
                echoerr "Cannot convert image to png"
                return 1
            fi
            FILE="$TMPDIR/image.png"
        fi
    fi

    # base64-encode the file and split it into chunks. The size of each graphics
    # command shouldn't be more than 4096, so we set the size of an encoded
    # chunk to be 3968, slightly less than that.
    echolog "base64-encoding and chunking the image"
    cat "$FILE" | base64 -w0 | split -b 3968 - "$TMPDIR/chunk_"

    upload_chunked_image "$IMAGE_ID" "$TMPDIR" "$COLS" "$ROWS"
    return $?
}

# Uploads an already chunked image to the terminal.
# Usage: upload_chunked_image $IMAGE_ID $CHUNKS_DIR $COLS $ROWS
upload_chunked_image() {
    local IMAGE_ID="$1"
    local CHUNKS_DIR="$2"
    local COLS="$3"
    local ROWS="$4"

    # Check if we are in the active tmux pane. Uploading images from inactive
    # panes is impossible, so we either need to fail or to hijack focus by
    # creating a new pane.
    if [[ -n "$INSIDE_TMUX" ]]; then
        local TMUX_ACTIVE="$(tmux display-message -t $TMUX_PANE \
                                -p "#{window_active}#{pane_active}")"
        if [[ "$TMUX_ACTIVE" != "11" ]]; then
            hijack_tmux "$IMAGE_ID" "$CHUNKS_DIR" "$COLS" "$ROWS"
            return $?
        fi
    fi

    # Issue a command indicating that we want to start data transmission for a
    # new image.
    # a=t    the action is to transmit data
    # i=$IMAGE_ID
    # f=100  PNG. st will ignore this field, for kitty we support only PNG.
    # t=d    transmit data directly
    # c=,r=  width and height in cells
    # s=,v=  width and height in pixels (not used here)
    # o=z    use compression (not used here)
    # m=1    multi-chunked data
    gr_command "a=t,i=$IMAGE_ID,f=100,t=d,c=${COLS},r=${ROWS},m=1"

    CHUNKS_COUNT="$(ls -1 $CHUNKS_DIR/chunk_* | wc -l)"
    CHUNK_I=0
    STARTTIME="$(date +%s%3N)"
    SPEED=""

    # Transmit chunks and display progress.
    for CHUNK in "$CHUNKS_DIR/chunk_"*; do
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
        echo -en "i=$IMAGE_ID,m=1;"
        cat $CHUNK
        end_gr_command
    done

    # Tell the terminal that we are done.
    gr_command "i=$IMAGE_ID,m=0"

    echostatus "Awaiting terminal response"
    get_terminal_response
    if [[ "$?" != 0 ]]; then
        return 1
    fi
    REGEX='.*_G.*;OK.*'
    if ! [[ "$TERM_RESPONSE" =~ $REGEX ]]; then
        echoerr "Uploading error: $TERM_RESPONSE_PRINTABLE"
        return 1
    fi
    return 0
}

# Creates a tmux pane and uploads an already chunked image to the terminal.
# Usage: hijack_tmux $IMAGE_ID $CHUNKS_DIR $COLS $ROWS
hijack_tmux() {
    local IMAGE_ID="$1"
    local CHUNKS_DIR="$2"
    local COLS="$3"
    local ROWS="$4"

    if [[ -z "$TMUX_HIJACK_ALLOWED" ]]; then
        echoerr "Not in active pane and tmux hijacking is not allowed"
        return 1
    fi

    echostatus "Not in active pane, hijacking tmux"
    TMP_PID="$(mktemp)"
    TMP_RET="$(mktemp)"
    echolog "TMP_PID=$TMP_PID"
    # Run a helper in a new pane
    tmux split-window -l 1 "$0" \
        -c "$COLS" \
        -r "$ROWS" \
        -e "$ERR" \
        -l "$LOG" \
        -f "$CHUNKS_DIR" \
        --id "$IMAGE_ID" \
        --store-pid "$TMP_PID" \
        --store-code "$TMP_RET" \
        --tmux-hijack-helper
    if [[ $? -ne 0 ]]; then
        return 1
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
        return 1
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
    RET_CODE="$(cat $TMP_RET)"
    echolog "Process $PID_TO_WAIT finished with code $RET_CODE"
    rm "$TMP_RET" 2> /dev/null || echolog "Could not rm $TMP_RET"
    rm "$TMP_PID" 2> /dev/null || echolog "Could not rm $TMP_PID"
    [[ -n "$RET_CODE" ]] || RET_CODE="1"
    return "$RET_CODE"
}

#####################################################################
# Running the tmux hijack helper if requested.
#####################################################################

if [[ -n "$TMUX_HIJACK_HELPER" ]]; then
    # Do not allow any more hijack attempts.
    TMUX_HIJACK_ALLOWED=""
    upload_chunked_image "$IMAGE_ID" "$FILE" "$COLS" "$ROWS"
    RET_CODE="$?"
    echo "$RET_CODE" > "$STORE_CODE"
    exit "$RET_CODE"
fi

#####################################################################
# Compute the number of rows and columns
#####################################################################

# Check if the file exists.
if ! [[ -f "$FILE" ]]; then
    echoerr "File not found: $FILE (pwd: $(pwd))"
    exit 1
fi

echolog "Image file: $FILE (pwd: $(pwd))"

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

# If the id is explicitly specified, set the $USE_256 variable accordingly and
# then check that the id is correct.
if [[ -n "$IMAGE_ID" ]]; then
    if (( "$IMAGE_ID" < 256 )); then
        USE_256="1"
    else
        USE_256=""
    fi
    if ! is_image_id_correct "$IMAGE_ID"; then
        echoerr "The specified image id $IMAGE_ID is not correct"
        exit 1
    fi
fi

# 8-bit and 24-bit image ids live in different namespaces. We use different
# session and terminal directories to store information about them.
ID_SUBSPACE="24bit_ids"
if [[ -n "$USE_256" ]]; then
    ID_SUBSPACE="8bit_ids"
fi

SESSION_DIR="$CACHE_DIR/sessions/${SESSION_ID}-${ID_SUBSPACE}"
TERMINAL_DIR="$CACHE_DIR/terminals/${TERMINAL_ID}-${ID_SUBSPACE}"

mkdir -p "$CACHE_DIR/cache/" 2> /dev/null
mkdir -p "$SESSION_DIR" 2> /dev/null
mkdir -p "$TERMINAL_DIR" 2> /dev/null

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

if [[ -n "$IMAGE_ID" ]]; then
    echostatus "Using the specified image id $IMAGE_ID"
else
    # Find an id for the image. We want to avoid reuploading, and at the same
    # time we want to minimize image id collisions.
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
fi

#####################################################################
# Image uploading
#####################################################################

# Check if this instance has already been uploaded to this terminal with the
# found image id.
# TODO: Also check date and reupload if too old.
if [[ -e "$TERMINAL_DIR/$IMAGE_ID" ]] &&
   [[ "$(head -1 "$TERMINAL_DIR/$IMAGE_ID")" == "$IMG_INSTANCE" ]]; then
    echostatus "Image already uploaded"
else
    (
        flock --timeout "$DEFAULT_TIMEOUT" 9 || \
            { echoerr "Could not acquire a lock on $TERMINAL_DIR.lock"; exit 1; }
        rm "$TERMINAL_DIR/$IMAGE_ID" 2> /dev/null
        upload_image "$IMAGE_ID" "$FILE" "$COLS" "$ROWS"
        if [[ "$?" == "0" ]]; then
            echo "$IMG_INSTANCE" > "$TERMINAL_DIR/$IMAGE_ID"
        fi
    ) 9>"$TERMINAL_DIR.lock" || exit 1
fi

#####################################################################
# Printing the image placeholder
#####################################################################

ROWCOLUMN_DIACRITICS=("\U305" "\U30d" "\U30e" "\U310" "\U312" "\U33d" "\U33e"
    "\U33f" "\U346" "\U34a" "\U34b" "\U34c" "\U350" "\U351" "\U352" "\U357"
    "\U35b" "\U363" "\U364" "\U365" "\U366" "\U367" "\U368" "\U369" "\U36a"
    "\U36b" "\U36c" "\U36d" "\U36e" "\U36f" "\U483" "\U484" "\U485" "\U486"
    "\U487" "\U592" "\U593" "\U594" "\U595" "\U597" "\U598" "\U599" "\U59c"
    "\U59d" "\U59e" "\U59f" "\U5a0" "\U5a1" "\U5a8" "\U5a9" "\U5ab" "\U5ac"
    "\U5af" "\U5c4" "\U610" "\U611" "\U612" "\U613" "\U614" "\U615" "\U616"
    "\U617" "\U657" "\U658" "\U659" "\U65a" "\U65b" "\U65d" "\U65e" "\U6d6"
    "\U6d7" "\U6d8" "\U6d9" "\U6da" "\U6db" "\U6dc" "\U6df" "\U6e0" "\U6e1"
    "\U6e2" "\U6e4" "\U6e7" "\U6e8" "\U6eb" "\U6ec" "\U730" "\U732" "\U733"
    "\U735" "\U736" "\U73a" "\U73d" "\U73f" "\U740" "\U741" "\U743" "\U745"
    "\U747" "\U749" "\U74a" "\U7eb" "\U7ec" "\U7ed" "\U7ee" "\U7ef" "\U7f0"
    "\U7f1" "\U7f3" "\U816" "\U817" "\U818" "\U819" "\U81b" "\U81c" "\U81d"
    "\U81e" "\U81f" "\U820" "\U821" "\U822" "\U823" "\U825" "\U826" "\U827"
    "\U829" "\U82a" "\U82b" "\U82c" "\U82d" "\U951" "\U953" "\U954" "\Uf82"
    "\Uf83" "\Uf86" "\Uf87" "\U135d" "\U135e" "\U135f" "\U17dd" "\U193a"
    "\U1a17" "\U1a75" "\U1a76" "\U1a77" "\U1a78" "\U1a79" "\U1a7a" "\U1a7b"
    "\U1a7c" "\U1b6b" "\U1b6d" "\U1b6e" "\U1b6f" "\U1b70" "\U1b71" "\U1b72"
    "\U1b73" "\U1cd0" "\U1cd1" "\U1cd2" "\U1cda" "\U1cdb" "\U1ce0" "\U1dc0"
    "\U1dc1" "\U1dc3" "\U1dc4" "\U1dc5" "\U1dc6" "\U1dc7" "\U1dc8" "\U1dc9"
    "\U1dcb" "\U1dcc" "\U1dd1" "\U1dd2" "\U1dd3" "\U1dd4" "\U1dd5" "\U1dd6"
    "\U1dd7" "\U1dd8" "\U1dd9" "\U1dda" "\U1ddb" "\U1ddc" "\U1ddd" "\U1dde"
    "\U1ddf" "\U1de0" "\U1de1" "\U1de2" "\U1de3" "\U1de4" "\U1de5" "\U1de6"
    "\U1dfe" "\U20d0" "\U20d1" "\U20d4" "\U20d5" "\U20d6" "\U20d7" "\U20db"
    "\U20dc" "\U20e1" "\U20e7" "\U20e9" "\U20f0" "\U2cef" "\U2cf0" "\U2cf1"
    "\U2de0" "\U2de1" "\U2de2" "\U2de3" "\U2de4" "\U2de5" "\U2de6" "\U2de7"
    "\U2de8" "\U2de9" "\U2dea" "\U2deb" "\U2dec" "\U2ded" "\U2dee" "\U2def"
    "\U2df0" "\U2df1" "\U2df2" "\U2df3" "\U2df4" "\U2df5" "\U2df6" "\U2df7"
    "\U2df8" "\U2df9" "\U2dfa" "\U2dfb" "\U2dfc" "\U2dfd" "\U2dfe" "\U2dff"
    "\Ua66f" "\Ua67c" "\Ua67d" "\Ua6f0" "\Ua6f1" "\Ua8e0" "\Ua8e1" "\Ua8e2"
    "\Ua8e3" "\Ua8e4" "\Ua8e5" "\Ua8e6" "\Ua8e7" "\Ua8e8" "\Ua8e9" "\Ua8ea"
    "\Ua8eb" "\Ua8ec" "\Ua8ed" "\Ua8ee" "\Ua8ef" "\Ua8f0" "\Ua8f1" "\Uaab0"
    "\Uaab2" "\Uaab3" "\Uaab7" "\Uaab8" "\Uaabe" "\Uaabf" "\Uaac1" "\Ufe20"
    "\Ufe21" "\Ufe22" "\Ufe23" "\Ufe24" "\Ufe25" "\Ufe26" "\U10a0f" "\U10a38"
    "\U1d185" "\U1d186" "\U1d187" "\U1d188" "\U1d189" "\U1d1aa" "\U1d1ab"
    "\U1d1ac" "\U1d1ad" "\U1d242" "\U1d243" "\U1d244")

# Each line starts with the escape sequence to set the foreground color to the
# image id, unless --noesc is specified.
LINE_START=""
LINE_END=""
if [[ -z "$NOESC" ]]; then
    # TODO: 24 bit ids
    LINE_START="$(echo -en "\e[38;5;${IMAGE_ID}m")"
    LINE_END="$(echo -en "\e[39;m")"
fi

# Clear the status line.
echostatus

# Clear the output file
if [[ -z "$APPEND" ]]; then
    > "$OUT"
fi

# Fill the output with characters representing the image
for Y in `seq 0 $(expr $ROWS - 1)`; do
    echo -n "$LINE_START" >> "$OUT"
    for X in `seq 0 $(expr $COLS - 1)`; do
        printf "\UEEEE${ROWCOLUMN_DIACRITICS[$Y]}${ROWCOLUMN_DIACRITICS[$X]}"
    done
    echo -n "$LINE_END" >> "$OUT"
    printf "\n" >> "$OUT"
done

echolog "Finished displaying the image"
exit 0
