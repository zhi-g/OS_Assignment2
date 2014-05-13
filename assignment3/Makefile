all:
	gcc -Wall -g -O0 -D_FILE_OFFSET_BITS=64 vfat.c -o vfat -losxfuse -liconv
	echo "Successfull compiled!"

clean:
	rm -f vfat.o vfat
