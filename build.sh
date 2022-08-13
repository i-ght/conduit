CFLAGS="-I$HOME/.local/include -I./include -I./src"
LFLAGS="-L$HOME/.local/lib -l:librefqueue.a"

for dotc_file in ./src/*.c; do
  clang -g -fPIC -c "$dotc_file" -I./include/ -I./src/ $CFLAGS -o "$dotc_file.o"
done

doto_files=""

for doto_file in ./src/*.c.o; do
  doto_files="${doto_files} ${doto_file}"
done

ar rcs bin/libconduit.a $doto_files
clang -g -shared -o bin/libconduit.so $doto_files $LFLAGS $CFLAGS
 
clang -g -o bin/program -L./bin -l:libconduit.a $LFLAGS $CFLAGS
rm ./src/*.c.o
