#!/bin/zsh
# builds tools/harness.c and runs it on a table: harness.sh <id> [balls] [heat.pgm]
set -e
APP=${0:a:h:h}
ROOT=${APP:h:h}
mkdir -p $APP/build
cc -O2 -DAOS_SIM -I$APP/main -I$ROOT/components/aos_hal/include -I$ROOT/components/aos_ui/include \
   $APP/tools/harness.c $APP/main/pb_table.c $APP/main/pb_phys.c $APP/main/pb_game.c $APP/main/pb_dmd.c \
   $APP/main/pb_pak.c -o $APP/build/harness -lm
python3 -c "import sys; sys.path.insert(0, '$APP/tools'); import pbtable; print(pbtable.load('$1').def_text(), end='')" > $APP/build/$1.def
$APP/build/harness $APP/build/$1.def ${2:-300} $3
