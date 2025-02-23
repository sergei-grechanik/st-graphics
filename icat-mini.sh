#!/bin/sh

# vim: shiftwidth=4

script_name="$(basename "$0")"

short_help="Usage: $script_name [OPTIONS] <image_file>

This is a script to display images in the terminal using the kitty graphics
protocol with Unicode placeholders. It is very basic, please use something else
if you have alternatives.

Options:
  -h                  Show this help.
  -s SCALE            The scale of the image, may be floating point.
  -c N, --cols N      The number of columns.
  -r N, --rows N      The number of rows.
  --max-cols N        The maximum number of columns.
  --max-rows N        The maximum number of rows.
  --cell-size WxH     The cell size in pixels.
  -m METHOD           The uploading method, may be 'file', 'direct' or 'auto'.
  --speed SPEED       The multiplier for the animation speed (float).
"

# Exit the script on keyboard interrupt
trap "echo 'icat-mini was interrupted' >&2; exit 1" INT

cols=""
rows=""
file=""
command_tty=""
response_tty=""
uploading_method="auto"
cell_size=""
scale=1
max_cols=""
max_rows=""
speed=""

# Parse the command line.
while [ $# -gt 0 ]; do
    case "$1" in
        -c|--columns|--cols)
            cols="$2"
            shift 2
            ;;
        -r|--rows|-l|--lines)
            rows="$2"
            shift 2
            ;;
        -s|--scale)
            scale="$2"
            shift 2
            ;;
        -h|--help)
            echo "$short_help"
            exit 0
            ;;
        -m|--upload-method|--uploading-method)
            uploading_method="$2"
            shift 2
            ;;
        --cell-size)
            cell_size="$2"
            shift 2
            ;;
        --max-cols)
            max_cols="$2"
            shift 2
            ;;
        --max-rows)
            max_rows="$2"
            shift 2
            ;;
        --speed)
            speed="$2"
            shift 2
            ;;
        --)
            file="$2"
            shift 2
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
        *)
            if [ -n "$file" ]; then
                echo "Multiple image files are not supported: $file and $1" >&2
                exit 1
            fi
            file="$1"
            shift
            ;;
    esac
done

file="$(realpath "$file")"

#####################################################################
# Detect imagemagick
#####################################################################

# If there is the 'magick' command, use it instead of separate 'convert' and
# 'identify' commands.
if command -v magick > /dev/null; then
    identify="magick identify"
    convert="magick"
else
    identify="identify"
    convert="convert"
fi

#####################################################################
# Detect tmux
#####################################################################

# Check if we are inside tmux.
inside_tmux=""
if [ -n "$TMUX" ]; then
    inside_tmux=1
fi

if [ -z "$command_tty" ] && [ -n "$inside_tmux" ]; then
    # Get the pty of the current tmux pane.
    command_tty="$(tmux display-message -t "$TMUX_PANE" -p "#{pane_tty}")"
    if [ ! -e "$command_tty" ]; then
        command_tty=""
    fi
fi

#####################################################################
# Adjust the terminal state
#####################################################################

if [ -z "$command_tty" ]; then
    command_tty="/dev/tty"
fi
if [ -z "$response_tty" ]; then
    response_tty="/dev/tty"
fi

stty_orig="$(stty -g < "$response_tty")"
stty -echo < "$response_tty"
# Disable ctrl-z. Pressing ctrl-z during image uploading may cause some
# horrible issues otherwise.
stty susp undef < "$response_tty"
stty -icanon < "$response_tty"

restore_echo() {
    [ -n "$stty_orig" ] || return
    stty $stty_orig < "$response_tty"
}

trap restore_echo EXIT TERM

#####################################################################
# Compute the number of rows and columns
#####################################################################

is_pos_int() {
    if [ -z "$1" ]; then
        return 1 # false
    fi
    if [ -z "$(printf '%s' "$1" | tr -d '[:digit:]')" ]; then
        if [ "$1" -gt 0 ]; then
            return 0 # true
        fi
    fi
    return 1 # false
}

if [ -n "$cols" ] || [ -n "$rows" ]; then
    if [ -n "$max_cols" ] || [ -n "$max_rows" ]; then
        echo "You can't specify both max-cols/rows and cols/rows" >&2
        exit 1
    fi
fi

# Get the max number of cols and rows.
[ -n "$max_cols" ] || max_cols="$(tput cols)"
[ -n "$max_rows" ] || max_rows="$(tput lines)"
if [ "$max_rows" -gt 255 ]; then
    max_rows=255
fi

python_ioctl_command="import array, fcntl, termios
buf = array.array('H', [0, 0, 0, 0])
fcntl.ioctl(0, termios.TIOCGWINSZ, buf)
print(int(buf[2]/buf[1]), int(buf[3]/buf[0]))"

# Get the cell size in pixels if either cols or rows are not specified.
if [ -z "$cols" ] || [ -z "$rows" ]; then
    cell_width=""
    cell_height=""
    # If the cell size is specified, use it.
    if [ -n "$cell_size" ]; then
        cell_width="${cell_size%x*}"
        cell_height="${cell_size#*x}"
        if ! is_pos_int "$cell_height" || ! is_pos_int "$cell_width"; then
            echo "Invalid cell size: $cell_size" >&2
            exit 1
        fi
    fi
    # Otherwise try to use TIOCGWINSZ ioctl via python.
    if [ -z "$cell_width" ] || [ -z "$cell_height" ]; then
        cell_size_ioctl="$(python3 -c "$python_ioctl_command" < "$command_tty" 2> /dev/null)"
        cell_width="${cell_size_ioctl% *}"
        cell_height="${cell_size_ioctl#* }"
        if ! is_pos_int "$cell_height" || ! is_pos_int "$cell_width"; then
            cell_width=""
            cell_height=""
        fi
    fi
    # If it didn't work, try to use csi XTWINOPS.
    if [ -z "$cell_width" ] || [ -z "$cell_height" ]; then
        if [ -n "$inside_tmux" ]; then
            printf '\ePtmux;\e\e[16t\e\\' >> "$command_tty"
        else
            printf '\e[16t' >> "$command_tty"
        fi
        # The expected response will look like ^[[6;<height>;<width>t
        term_response=""
        while true; do
            char=$(dd bs=1 count=1 <"$response_tty" 2>/dev/null)
            if [ "$char" = "t" ]; then
                break
            fi
            term_response="$term_response$char"
        done
        cell_height="$(printf '%s' "$term_response" | cut -d ';' -f 2)"
        cell_width="$(printf '%s' "$term_response" | cut -d ';' -f 3)"
        if ! is_pos_int "$cell_height" || ! is_pos_int "$cell_width"; then
            cell_width=8
            cell_height=16
        fi
    fi
fi

# Compute a formula with bc and round to the nearest integer.
bc_round() {
    LC_NUMERIC=C printf '%.0f' "$(printf '%s\n' "scale=2;($1) + 0.5" | bc)"
}

# Compute the number of rows and columns of the image.
if [ -z "$cols" ] || [ -z "$rows" ]; then
    # Get the size of the image and its resolution. If it's an animation, use
    # the first frame.
    format_output="$($identify -format '%w %h\n' "$file" | head -1)"
    img_width="$(printf '%s' "$format_output" | cut -d ' ' -f 1)"
    img_height="$(printf '%s' "$format_output" | cut -d ' ' -f 2)"
    if ! is_pos_int "$img_width" || ! is_pos_int "$img_height"; then
        echo "Couldn't get image size from identify: $format_output" >&2
        echo >&2
        exit 1
    fi
    opt_cols_expr="(${scale}*${img_width}/${cell_width})"
    opt_rows_expr="(${scale}*${img_height}/${cell_height})"
    if [ -z "$cols" ] && [ -z "$rows" ]; then
        # If columns and rows are not specified, compute the optimal values
        # using the information about rows and columns per inch.
        cols="$(bc_round "$opt_cols_expr")"
        rows="$(bc_round "$opt_rows_expr")"
        # Make sure that automatically computed rows and columns are within some
        # sane limits
        if [ "$cols" -gt "$max_cols" ]; then
            rows="$(bc_round "$rows * $max_cols / $cols")"
            cols="$max_cols"
        fi
        if [ "$rows" -gt "$max_rows" ]; then
            cols="$(bc_round "$cols * $max_rows / $rows")"
            rows="$max_rows"
        fi
    elif [ -z "$cols" ]; then
        # If only one dimension is specified, compute the other one to match the
        # aspect ratio as close as possible.
        cols="$(bc_round "${opt_cols_expr}*${rows}/${opt_rows_expr}")"
    elif [ -z "$rows" ]; then
        rows="$(bc_round "${opt_rows_expr}*${cols}/${opt_cols_expr}")"
    fi

    if [ "$cols" -lt 1 ]; then
        cols=1
    fi
    if [ "$rows" -lt 1 ]; then
        rows=1
    fi
fi

#####################################################################
# Generate an image id
#####################################################################

image_id=""
while [ -z "$image_id" ]; do
    image_id="$(shuf -i 16777217-4294967295 -n 1)"
    # Check that the id requires 24-bit fg colors.
    if [ "$(expr \( "$image_id" / 256 \) % 65536)" -eq 0 ]; then
        image_id=""
    fi
done

#####################################################################
# Uploading the image
#####################################################################

# Choose the uploading method
if [ "$uploading_method" = "auto" ]; then
    if [ -n "$SSH_CLIENT" ] || [ -n "$SSH_TTY" ] || [ -n "$SSH_CONNECTION" ]; then
        uploading_method="direct"
    else
        uploading_method="file"
    fi
fi

# Functions to emit the start and the end of a graphics command.
if [ -n "$inside_tmux" ]; then
    # If we are in tmux we have to wrap the command in Ptmux.
    graphics_command_start='\ePtmux;\e\e_G'
    graphics_command_end='\e\e\\\e\\'
else
    graphics_command_start='\e_G'
    graphics_command_end='\e\\'
fi

# Send a graphics command with the correct start and end
gr_command() {
    printf "${graphics_command_start}%s${graphics_command_end}" "$1" >> "$command_tty"
}

# Compute the size of a data chunk for direct transmission.
if [ "$uploading_method" = "direct" ]; then
    # Get the value of PIPE_BUF.
    pipe_buf="$(getconf PIPE_BUF "$command_tty" 2> /dev/null)"
    if is_pos_int "$pipe_buf"; then
        # Make sure it's between 512 and 4096.
        if [ "$(expr "$pipe_buf" \< 512)" -eq 1 ]; then
            pipe_buf=512
        elif [ "$(expr "$pipe_buf" \> 4096)" -eq 1 ]; then
            pipe_buf=4096
        fi
    else
        pipe_buf=512
    fi

    # The size of each graphics command shouldn't be more than PIPE_BUF, so we
    # set the size of an encoded chunk to be PIPE_BUF - 128 to leave some space
    # for the command.
    chunk_size="$(expr "$pipe_buf" - 128)"
fi

# Check if the image format is supported.
is_format_supported() {
    arg_format="$1"
    if [ "$arg_format" = "PNG" ]; then
        return 0
    elif [ "$arg_format" = "JPEG" ]; then
        if [ -z "$inside_tmux" ]; then
            actual_term="$TERM"
        else
            # Get the actual current terminal name from tmux.
            actual_term="$(tmux display-message -p "#{client_termname}")"
        fi
        # st is known to support JPEG.
        case "$actual_term" in
            st | *-st | st-* | *-st-*)
                return 0
                ;;
        esac
        return 1
    else
        return 1
    fi
}

# Send an uploading command. Usage: gr_upload <action> <command> <file>
# Where <action> is a part of command that specifies the action, it will be
# repeated for every chunk (if the method is direct), and <command> is the rest
# of the command that specifies the image parameters. <action> and <command>
# must not include the transmission method or ';'.
# Example:
# gr_upload "a=T,q=2" "U=1,i=${image_id},f=100,c=${cols},r=${rows}" "$file"
gr_upload() {
    arg_action="$1"
    arg_command="$2"
    arg_file="$3"
    if [ "$uploading_method" = "file" ]; then
        # base64-encode the filename
        encoded_filename=$(printf '%s' "$arg_file" | base64 -w0)
        # If the file name contains 'tty-graphics-protocol', assume it's
        # temporary and use t=t.
        medium="t=f"
        case "$arg_file" in
            *tty-graphics-protocol*)
                medium="t=t"
                ;;
            *)
                medium="t=f"
                ;;
        esac
        gr_command "${arg_action},${arg_command},${medium};${encoded_filename}"
    fi
    if [ "$uploading_method" = "direct" ]; then
        # Create a temporary directory to store the chunked image.
        chunkdir="$(mktemp -d)"
        if [ ! "$chunkdir" ] || [ ! -d "$chunkdir" ]; then
            echo "Can't create a temp dir" >&2
            exit 1
        fi
        # base64-encode the file and split it into chunks. The size of each
        # graphics command shouldn't be more than 4096, so we set the size of an
        # encoded chunk to be 3968, slightly less than that.
        chunk_size=3968
        cat "$arg_file" | base64 -w0 | split -b "$chunk_size" - "$chunkdir/chunk_"

        # Issue a command indicating that we want to start data transmission for
        # a new image.
        gr_command "${arg_action},${arg_command},t=d,m=1"

        # Transmit chunks.
        for chunk in "$chunkdir/chunk_"*; do
            gr_command "${arg_action},i=${image_id},m=1;$(cat "$chunk")"
            rm "$chunk"
        done

        # Tell the terminal that we are done.
        gr_command "${arg_action},i=$image_id,m=0"

        # Remove the temporary directory.
        rmdir "$chunkdir"
    fi
}

delayed_frame_dir_cleanup() {
    arg_frame_dir="$1"
    sleep 2
    if [ -n "$arg_frame_dir" ]; then
        for frame in "$arg_frame_dir"/frame_*.png; do
            rm "$frame"
        done
        rmdir "$arg_frame_dir"
    fi
}

upload_image_and_print_placeholder() {
    # Check if the file is an animation.
    format_output=$($identify -format '%n %m\n' "$file" | head -n 1)
    frame_count="$(printf '%s' "$format_output" | cut -d ' ' -f 1)"
    image_format="$(printf '%s' "$format_output" | cut -d ' ' -f 2)"

    if [ "$frame_count" -gt 1 ]; then
        # The file is an animation, decompose into frames and upload each frame.
        frame_dir="$(mktemp -d)"
        frame_dir="$HOME/temp/frames${frame_dir}"
        mkdir -p "$frame_dir"
        if [ ! "$frame_dir" ] || [ ! -d "$frame_dir" ]; then
            echo "Can't create a temp dir for frames" >&2
            exit 1
        fi

        # Decompose the animation into separate frames.
        $convert "$file" -coalesce "$frame_dir/frame_%06d.png"

        # Get all frame delays at once, in centiseconds, as a space-separated
        # string.
        delays=$($identify -format "%T " "$file")

        frame_number=1
        for frame in "$frame_dir"/frame_*.png; do
            # Read the delay for the current frame and convert it from
            # centiseconds to milliseconds.
            delay=$(printf '%s' "$delays" | cut -d ' ' -f "$frame_number")
            delay=$((delay * 10))
            # If the delay is 0, set it to 100ms.
            if [ "$delay" -eq 0 ]; then
                delay=100
            fi

            if [ -n "$speed" ]; then
                delay=$(bc_round "$delay / $speed")
            fi

            if [ "$frame_number" -eq 1 ]; then
                # Abort the previous transmission, just in case.
                gr_command "q=2,a=t,i=${image_id},m=0"
                # Upload the first frame with a=T
                gr_upload "q=2,a=T" "f=100,U=1,i=${image_id},c=${cols},r=${rows}" "$frame"
                # Set the delay for the first frame and also play the animation
                # in loading mode (s=2).
                gr_command "a=a,v=1,s=2,r=${frame_number},z=${delay},i=${image_id}"
                # Print the placeholder after the first frame to reduce the wait
                # time.
                print_placeholder
            else
                # Upload subsequent frames with a=f
                gr_upload "q=2,a=f" "f=100,i=${image_id},z=${delay}" "$frame"
            fi

            frame_number=$((frame_number + 1))
        done

        # Play the animation in loop mode (s=3).
        gr_command "a=a,v=1,s=3,i=${image_id}"

        # Remove the temporary directory, but do it in the background with a
        # delay to avoid removing files before they are loaded by the terminal.
        delayed_frame_dir_cleanup "$frame_dir" 2> /dev/null &
    elif is_format_supported "$image_format"; then
        # The file is not an animation and has a supported format, upload it
        # directly.
        gr_upload "q=2,a=T" "U=1,i=${image_id},f=100,c=${cols},r=${rows}" "$file"
        # Print the placeholder
        print_placeholder
    else
        # The format is not supported, try to convert it to png.
        temp_file="$(mktemp --tmpdir "icat-mini-tty-graphics-protocol-XXXXX.png")"
        if ! $convert "$file" "$temp_file"; then
            echo "Failed to convert the image to PNG" >&2
            exit 1
        fi
        # Upload the converted image.
        gr_upload "q=2,a=T" "U=1,i=${image_id},f=100,c=${cols},r=${rows}" "$temp_file"
        # Print the placeholder
        print_placeholder
    fi
}

#####################################################################
# Printing the image placeholder
#####################################################################

print_placeholder() {
    # Each line starts with the escape sequence to set the foreground color to
    # the image id.
    blue="$(expr "$image_id" % 256 )"
    green="$(expr \( "$image_id" / 256 \) % 256 )"
    red="$(expr \( "$image_id" / 65536 \) % 256 )"
    line_start="$(printf "\e[38;2;%d;%d;%dm" "$red" "$green" "$blue")"
    line_end="$(printf "\e[39;m")"

    id4th="$(expr \( "$image_id" / 16777216 \) % 256 )"
    eval "id_diacritic=\$d${id4th}"

    # Reset the brush state, mostly to reset the underline color.
    printf "\e[0m"

    # Fill the output with characters representing the image
    for y in $(seq 0 "$(expr "$rows" - 1)"); do
        eval "row_diacritic=\$d${y}"
        line="$line_start"
        for x in $(seq 0 "$(expr "$cols" - 1)"); do
            eval "col_diacritic=\$d${x}"
            # Note that when $x is out of bounds, the column diacritic will
            # be empty, meaning that the column should be guessed by the
            # terminal.
            if [ "$x" -ge "$num_diacritics" ]; then
                line="${line}${placeholder}${row_diacritic}"
            else
                line="${line}${placeholder}${row_diacritic}${col_diacritic}${id_diacritic}"
            fi
        done
        line="${line}${line_end}"
        printf "%s\n" "$line"
    done

    printf "\e[0m"
}

d0="̅"
d1="̍"
d2="̎"
d3="̐"
d4="̒"
d5="̽"
d6="̾"
d7="̿"
d8="͆"
d9="͊"
d10="͋"
d11="͌"
d12="͐"
d13="͑"
d14="͒"
d15="͗"
d16="͛"
d17="ͣ"
d18="ͤ"
d19="ͥ"
d20="ͦ"
d21="ͧ"
d22="ͨ"
d23="ͩ"
d24="ͪ"
d25="ͫ"
d26="ͬ"
d27="ͭ"
d28="ͮ"
d29="ͯ"
d30="҃"
d31="҄"
d32="҅"
d33="҆"
d34="҇"
d35="֒"
d36="֓"
d37="֔"
d38="֕"
d39="֗"
d40="֘"
d41="֙"
d42="֜"
d43="֝"
d44="֞"
d45="֟"
d46="֠"
d47="֡"
d48="֨"
d49="֩"
d50="֫"
d51="֬"
d52="֯"
d53="ׄ"
d54="ؐ"
d55="ؑ"
d56="ؒ"
d57="ؓ"
d58="ؔ"
d59="ؕ"
d60="ؖ"
d61="ؗ"
d62="ٗ"
d63="٘"
d64="ٙ"
d65="ٚ"
d66="ٛ"
d67="ٝ"
d68="ٞ"
d69="ۖ"
d70="ۗ"
d71="ۘ"
d72="ۙ"
d73="ۚ"
d74="ۛ"
d75="ۜ"
d76="۟"
d77="۠"
d78="ۡ"
d79="ۢ"
d80="ۤ"
d81="ۧ"
d82="ۨ"
d83="۫"
d84="۬"
d85="ܰ"
d86="ܲ"
d87="ܳ"
d88="ܵ"
d89="ܶ"
d90="ܺ"
d91="ܽ"
d92="ܿ"
d93="݀"
d94="݁"
d95="݃"
d96="݅"
d97="݇"
d98="݉"
d99="݊"
d100="߫"
d101="߬"
d102="߭"
d103="߮"
d104="߯"
d105="߰"
d106="߱"
d107="߳"
d108="ࠖ"
d109="ࠗ"
d110="࠘"
d111="࠙"
d112="ࠛ"
d113="ࠜ"
d114="ࠝ"
d115="ࠞ"
d116="ࠟ"
d117="ࠠ"
d118="ࠡ"
d119="ࠢ"
d120="ࠣ"
d121="ࠥ"
d122="ࠦ"
d123="ࠧ"
d124="ࠩ"
d125="ࠪ"
d126="ࠫ"
d127="ࠬ"
d128="࠭"
d129="॑"
d130="॓"
d131="॔"
d132="ྂ"
d133="ྃ"
d134="྆"
d135="྇"
d136="፝"
d137="፞"
d138="፟"
d139="៝"
d140="᤺"
d141="ᨗ"
d142="᩵"
d143="᩶"
d144="᩷"
d145="᩸"
d146="᩹"
d147="᩺"
d148="᩻"
d149="᩼"
d150="᭫"
d151="᭭"
d152="᭮"
d153="᭯"
d154="᭰"
d155="᭱"
d156="᭲"
d157="᭳"
d158="᳐"
d159="᳑"
d160="᳒"
d161="᳚"
d162="᳛"
d163="᳠"
d164="᷀"
d165="᷁"
d166="᷃"
d167="᷄"
d168="᷅"
d169="᷆"
d170="᷇"
d171="᷈"
d172="᷉"
d173="᷋"
d174="᷌"
d175="᷑"
d176="᷒"
d177="ᷓ"
d178="ᷔ"
d179="ᷕ"
d180="ᷖ"
d181="ᷗ"
d182="ᷘ"
d183="ᷙ"
d184="ᷚ"
d185="ᷛ"
d186="ᷜ"
d187="ᷝ"
d188="ᷞ"
d189="ᷟ"
d190="ᷠ"
d191="ᷡ"
d192="ᷢ"
d193="ᷣ"
d194="ᷤ"
d195="ᷥ"
d196="ᷦ"
d197="᷾"
d198="⃐"
d199="⃑"
d200="⃔"
d201="⃕"
d202="⃖"
d203="⃗"
d204="⃛"
d205="⃜"
d206="⃡"
d207="⃧"
d208="⃩"
d209="⃰"
d210="⳯"
d211="⳰"
d212="⳱"
d213="ⷠ"
d214="ⷡ"
d215="ⷢ"
d216="ⷣ"
d217="ⷤ"
d218="ⷥ"
d219="ⷦ"
d220="ⷧ"
d221="ⷨ"
d222="ⷩ"
d223="ⷪ"
d224="ⷫ"
d225="ⷬ"
d226="ⷭ"
d227="ⷮ"
d228="ⷯ"
d229="ⷰ"
d230="ⷱ"
d231="ⷲ"
d232="ⷳ"
d233="ⷴ"
d234="ⷵ"
d235="ⷶ"
d236="ⷷ"
d237="ⷸ"
d238="ⷹ"
d239="ⷺ"
d240="ⷻ"
d241="ⷼ"
d242="ⷽ"
d243="ⷾ"
d244="ⷿ"
d245="꙯"
d246="꙼"
d247="꙽"
d248="꛰"
d249="꛱"
d250="꣠"
d251="꣡"
d252="꣢"
d253="꣣"
d254="꣤"
d255="꣥"
d256="꣦"
d257="꣧"
d258="꣨"
d259="꣩"
d260="꣪"
d261="꣫"
d262="꣬"
d263="꣭"
d264="꣮"
d265="꣯"
d266="꣰"
d267="꣱"
d268="ꪰ"
d269="ꪲ"
d270="ꪳ"
d271="ꪷ"
d272="ꪸ"
d273="ꪾ"
d274="꪿"
d275="꫁"
d276="︠"
d277="︡"
d278="︢"
d279="︣"
d280="︤"
d281="︥"
d282="︦"
d283="𐨏"
d284="𐨸"
d285="𝆅"
d286="𝆆"
d287="𝆇"
d288="𝆈"
d289="𝆉"
d290="𝆪"
d291="𝆫"
d292="𝆬"
d293="𝆭"
d294="𝉂"
d295="𝉃"
d296="𝉄"

num_diacritics="297"

placeholder="􎻮"

#####################################################################
# Upload the image and print the placeholder
#####################################################################

upload_image_and_print_placeholder
