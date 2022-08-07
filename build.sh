for dotc_file in ./src/*.c; do
  clang -g -c "$dotc_file" -I./include/ -I./src/ -o "$dotc_file.o"
done

doto_files=""

for doto_file in ./src/*.c.o; do
  doto_files="${doto_files} ${doto_file}"
done

clang -o bin/program $doto_files
rm ./src/*.c.o
