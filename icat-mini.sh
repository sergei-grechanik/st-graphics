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
"

# Exit the script on keyboard interrupt
trap "exit 1" INT

cols=""
rows=""
file=""
tty="/dev/tty"
uploading_method="auto"
cell_size=""
scale=1
max_cols=""
max_rows=""

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

#####################################################################
# Adjust the terminal state
#####################################################################

stty_orig="$(stty -g < "$tty")"
stty -echo < "$tty"
# Disable ctrl-z. Pressing ctrl-z during image uploading may cause some
# horrible issues otherwise.
stty susp undef < "$tty"
stty -icanon < "$tty"

restore_echo() {
    [ -n "$stty_orig" ] || return
    stty $stty_orig < "$tty"
}

trap restore_echo EXIT TERM

#####################################################################
# Detect tmux
#####################################################################

# Check if we are inside tmux.
inside_tmux=""
if [ -n "$TMUX" ]; then
    case "$TERM" in
        *tmux*|*screen*)
            inside_tmux=1
            ;;
    esac
fi


#####################################################################
# Compute the number of rows and columns
#####################################################################

is_decimal() {
    if [ -z "$1" ]; then
        return 1 # false
    fi
    if [ -z "$(printf '%s' "$1" | tr -d '[:digit:]')" ]; then
        return 0 # true
    else
        return 1 # false
    fi
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

# Get the cell size in pixels if either cols or rows are not specified.
if [ -z "$cols" ] || [ -z "$rows" ]; then
    cell_width=""
    cell_height=""
    if [ -n "$cell_size" ]; then
        if [ "$cell_size" =~ ^[0-9]+x[0-9]+$ ]; then
            cell_width="${cell_size%x*}"
            cell_height="${cell_size#*x}"
        else
            echo "Invalid cell size: $cell_size" >&2
            exit 1
        fi
    else
        if [ -n "$inside_tmux" ]; then
            printf '\ePtmux;\e\e[16t\e\\' >> "$tty"
        else
            printf '\e[16t' >> "$tty"
        fi
        # The expected response will look like ^[[6;<height>;<width>t
        term_response=""
        while true; do
            char=$(dd bs=1 count=1 <"$tty" 2>/dev/null)
            if [ "$char" = "t" ]; then
                break
            fi
            term_response="$term_response$char"
        done
        cell_height="$(printf '%s' "$term_response" | cut -d ';' -f 2)"
        cell_width="$(printf '%s' "$term_response" | cut -d ';' -f 3)"
        if ! is_decimal "$cell_height" || ! is_decimal "$cell_width"; then
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
    # Get the size of the image and its resolution
    format_output="$(identify -format '%w %h' "$file")"
    img_width="$(printf '%s' "$format_output" | cut -d ' ' -f 1)"
    img_height="$(printf '%s' "$format_output" | cut -d ' ' -f 2)"
    if ! is_decimal "$img_width" || ! is_decimal "$img_height"; then
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
# Upload the image
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

start_gr_command() {
    printf "$graphics_command_start" >> "$tty"
}
end_gr_command() {
    printf "$graphics_command_end" >> "$tty"
}

# Send a graphics command with the correct start and end
gr_command() {
    start_gr_command
    printf '%s' "$1" >> "$tty"
    end_gr_command
}

file="$(realpath "$file")"

if [ "$uploading_method" = "file" ]; then
    # base64-encode the filename
    encoded_filename=$(printf '%s' "$file" | base64 -w0)
    gr_command "q=2,a=T,U=1,i=${image_id},f=100,t=f,c=${cols},r=${rows};${encoded_filename}"
fi

if [ "$uploading_method" = "direct" ]; then
    # Create a temporary directory to store the chunked image.
    tmpdir="$(mktemp -d)"
    if [ ! "$tmpdir" ] || [ ! -d "$tmpdir" ]; then
        echo "Can't create a temp dir" >&2
        exit 1
    fi
    # base64-encode the file and split it into chunks. The size of each
    # graphics command shouldn't be more than 4096, so we set the size of an
    # encoded chunk to be 3968, slightly less than that.
    chunk_size=3968
    cat "$file" | base64 -w0 | split -b "$chunk_size" - "$tmpdir/chunk_"

    # Issue a command indicating that we want to start data transmission for a
    # new image.
    gr_command "q=2,a=T,U=1,i=${image_id},f=100,t=d,c=${cols},r=${rows},m=1"

    # Transmit chunks.
    for chunk in "$tmpdir/chunk_"*; do
        start_gr_command
        printf '%s' "i=${image_id},m=1;" >> "$tty"
        cat "$chunk" >> "$tty"
        end_gr_command
        rm "$chunk"
    done

    # Tell the terminal that we are done.
    gr_command "i=$image_id,m=0"

    # Remove the temporary directory.
    rmdir "$tmpdir"
fi

#####################################################################
# Printing the image placeholder
#####################################################################

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
    printf '%s' "$line_start"
    for x in $(seq 0 "$(expr "$cols" - 1)"); do
        eval "col_diacritic=\$d${x}"
        # Note that when $x is out of bounds, the column diacritic will
        # be empty, meaning that the column should be guessed by the
        # terminal.
        if [ "$x" -ge "$num_diacritics" ]; then
            printf '%s' "${placeholder}${row_diacritic}"
        else
            printf '%s' "${placeholder}${row_diacritic}${col_diacritic}${id_diacritic}"
        fi
    done
    printf '%s\n' "$line_end"
done

printf "\e[0m"
