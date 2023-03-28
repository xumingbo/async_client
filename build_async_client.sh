
topDir='/home/bxu/workspace/libevent'
srcDir="${topDir}/sample"
#srcfile='bo-client'
srcfile='async_client'
echo compiling ${srcfile} ...

g++ -m64 -march=opteron -mno-3dnow -ggdb -O2 -std=c++17 -fno-omit-frame-pointer -Wall -Werror  -I${topDir}/include -I/usr/local/include   -I ${srcDir}  -c -MMD -MF ${srcDir}/${srcfile}.o.d -MT ${srcDir}/${srcfile}.o ${srcDir}/${srcfile}.cc -o ${srcDir}/${srcfile}.o

if test $? -ne 0
then
  echo "compiling failed"
  exit -1
fi

echo linking ${srcfile} ...

g++ -L${topDir} -m64 -march=opteron -mno-3dnow  ${srcDir}/${srcfile}.o -levent -levent_pthreads -ldl -lpthread -L/usr/local/lib   -o ${srcDir}/${srcfile}
 
