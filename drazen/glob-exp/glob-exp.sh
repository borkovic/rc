#! /usr/bin/env bash

Script=$0
Error () { echo 1>&2 "$@"; }
Fatal () { Error "$@"; exit 1; }
Usage () { Fatal Usage: $Script '[-switch|-file|-echo]' '<POSITIVE_NUMBER>'; }

DO_CASE=false
DO_FILE=false
ECHO_CMD=false
###########################################################################
while let "$# > 0"
do
    case x$1 in
    (x-case|x--case|x-switch|x--switch) DO_CASE=true ;;
    (x-file|x--file|x-ls|x--ls)         DO_FILE=true ;;
    (x-echo|x--echo)                    ECHO_CMD=true ;;
    (*)                                 break ;;
    esac
    shift
done

###########################################################################
let "$# == 1" || Usage
let "N=$1"    || Usage
let "$N <= 0" && Usage

###########################################################################
str='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'

i=0
pat=''
###########################################################################
while let "$i < $N"; do
    pat=$pat'a*'
    let "i = $i+1"
done
pat=$pat'b'

echo 'shell=sh, N='$N


EvalCmd () { 
    typeset cmds="$@"
    if $ECHO_CMD; then
        echo "Cmd: $cmds"
        echo -n 'Result: '
    fi
    eval "$cmds"
}

###########################################################################
###########################################################################
if $DO_CASE; then
    cmd='case '$str' in ('$pat') echo case-yes;; (*) echo case-no;; esac'
    EvalCmd $cmd
fi

#--------------------------------------------------------------------------
if $DO_FILE; then
    test -f $str || touch $str
    cmd="ls $pat"

    EvalCmd "$cmd"

    rm -f $str
fi

#--------------------------------------------------------------------------
