clang-9 test.c -c -emit-llvm -o  test.bc -O0 -g

opt-9 \
	-analyze -load=/home/adil/pex/build/gatlin/libgatlin.so \
	-gatlin -stats \
    -gating=audit-lsm \
    -ccfv=1 -ccvv=0 -cctv=0\
    -ccf=1 -ccv=0 -cct=0\
	-cvf=1 \
    -prt-good=1 -prt-bad=1 -prt-ign=1 \
    -kinit=0 -nkinit=0 \
	test.bc -o /dev/null 2>&1

